#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"                                // TActorIterator
#include "LevelInstance/LevelInstanceActor.h"           // ALevelInstance
#include "LevelInstance/LevelInstanceInterface.h"       // ILevelInstanceInterface (GetWorldAsset/IsWorldAssetValid/IsLoaded)
#include "LevelInstance/LevelInstanceSubsystem.h"        // ULevelInstanceSubsystem + ELevelInstanceBreakFlags (CanBreak/BlockLoad/BreakLevelInstance)
#include "UObject/SoftObjectPtr.h"                       // TSoftObjectPtr, FSoftObjectPath
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
// pack_level_actors: build a APackedLevelActor Blueprint from a saved map (headless).
#include "PackedLevelActor/PackedLevelActor.h"               // APackedLevelActor + GetPackedHash (inline)
#include "PackedLevelActor/PackedLevelActorBuilder.h"        // FPackedLevelActorBuilder (ENGINE_API)
#include "Engine/Blueprint.h"                                // UBlueprint / BPTYPE_Normal
#include "Kismet2/KismetEditorUtilities.h"                   // FKismetEditorUtilities::CreateBlueprint (UNREALED_API)
#include "FileHelpers.h"                                     // UEditorLoadingAndSavingUtils::SavePackages (UNREALED_API)
#include "AssetRegistry/AssetRegistryModule.h"               // FAssetRegistryModule::AssetCreated
#include "UObject/Package.h"                                 // CreatePackage

#define LOCTEXT_NAMESPACE "UEAgentLevelInstance"

/**
 * Level Instance commands: the editor world's ALevelInstance actors, and APackedLevelActor Blueprints baked
 * from a saved map.
 *
 * An ALevelInstance is a placed actor that references another saved map (.umap) through a
 * TSoftObjectPtr<UWorld>. Its sub-level content is streamed in deferred/async: binding the map does NOT load
 * it synchronously.
 */
namespace UEAgentLevelInstanceCommands
{

// MCP-DOMAIN: Level

namespace
{
    /**
     * The ALevelInstance of the editor world whose label or name is ActorName (case ignored), and the world's level
     * instance subsystem. Null on success, else the error reply; Command names the command in the no-editor error.
     */
    TSharedPtr<FJsonObject> LevelInstance_ResolveTarget(const FString& ActorName, const TCHAR* Command, UWorld*& OutWorld,
        ALevelInstance*& OutTarget, ULevelInstanceSubsystem*& OutSubsystem)
    {
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(OutWorld))
        {
            return NoWorld;
        }

        // Each caller then dereferences GEditor without a guard; without a valid editor it would crash.
        if (!GEditor)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("%s requires the editor (GEditor); not available in this process"), Command));
        }

        OutTarget = nullptr;
        for (TActorIterator<ALevelInstance> It(OutWorld); It; ++It)
        {
            ALevelInstance* LevelInstance = *It;
            if (LevelInstance && LevelInstance->GetActorNameOrLabel().Equals(ActorName, ESearchCase::IgnoreCase))
            {
                OutTarget = LevelInstance;
                break;
            }
        }
        if (!OutTarget)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("level instance not found: %s"), *ActorName));
        }

        OutSubsystem = OutWorld->GetSubsystem<ULevelInstanceSubsystem>();
        if (!OutSubsystem)
        {
            return mcp_common::MakeErrorReply(
                TEXT("ULevelInstanceSubsystem not available on the editor world"));
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Normalise a user-supplied world asset path to a bare package name ("/Game/Foo"). */
FString NormalizePackageName(const FString& InPath)
{
    FString Path = InPath;
    Path.TrimStartAndEndInline();
    if (Path.IsEmpty())
    {
        return Path;
    }

    // Reduce an object path ("/Game/Foo.Foo") to its package name ("/Game/Foo");
    // a no-op for a bare package name.
    Path = FPackageName::ObjectPathToPackageName(Path);

    while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
    {
        Path.LeftChopInline(1);
    }
    return Path;
}

/** Serialise one ALevelInstance to a JSON object (actor name + world asset + loaded state). */
TSharedPtr<FJsonObject> LevelInstanceToJson(ALevelInstance* LevelInstance)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!LevelInstance)
    {
        return Obj;
    }

    Obj->SetStringField(TEXT("actor_name"), LevelInstance->GetActorNameOrLabel());

    // GetWorldAsset() is ENGINE_API on ILevelInstanceInterface; ToString() yields the
    // referenced .umap object path (empty when nothing is bound yet).
    const FString WorldAssetPath = LevelInstance->GetWorldAsset().ToString();
    Obj->SetStringField(TEXT("world_asset_path"), WorldAssetPath);
    Obj->SetBoolField(TEXT("world_asset_valid"), LevelInstance->IsWorldAssetValid());

    // IsLoaded() reflects whether the sub-level streaming has actually completed; it
    // is deferred/async so this is false immediately after binding a world asset.
    Obj->SetBoolField(TEXT("is_loaded"), LevelInstance->IsLoaded());

    const FVector Loc = LevelInstance->GetActorLocation();
    Obj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Loc));
    return Obj;
}

// ---------------------------------------------------------------------------
// create_level_instance
// ---------------------------------------------------------------------------

// MCP:create_level_instance - Spawns an ALevelInstance actor in the editor world and binds it to a saved map (.umap).  Uses the canonical headless-safe path: SpawnActor<ALevelInstance> + ALevelInstance::SetWorldAsset (NOT the editor-interactive ULevelInstanceSubsystem::CreateLevelInstanceFrom). The referenced .umap MUST already exist on disk; validated before binding so no actor is left with a broken WorldAsset. The sub-level is streamed in deferred/async, so loaded is always false here (binding != synchronous load). Idempotent: an existing ALevelInstance referencing the same world asset at the same location is returned with already_exists=true.
//   @param world_asset_path - Package/object path of the saved map to reference (e.g. "/Game/Maps/Foo").
//   @param location - Optional spawn location [x, y, z] (default [0,0,0]).
//   @param name - Optional actor label for the spawned level instance.
//   @returns { actor_name: string, world_asset_path: string, world_asset_set: bool, world_asset_valid: bool, loaded: bool, location: [number,number,number], already_exists: bool, note: string }

TSharedPtr<FJsonObject> HandleCreateLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("world_asset_path"), WorldAssetPath)) { return Missing; }

    // Optional spawn location.
    FVector Location = FVector::ZeroVector;
    const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
    if (Params->TryGetArrayField(TEXT("location"), LocationArray) && LocationArray && LocationArray->Num() >= 3)
    {
        Location.X = (*LocationArray)[0]->AsNumber();
        Location.Y = (*LocationArray)[1]->AsNumber();
        Location.Z = (*LocationArray)[2]->AsNumber();
    }

    FString ActorLabel;
    Params->TryGetStringField(TEXT("name"), ActorLabel);

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const FString PackageName = NormalizePackageName(WorldAssetPath);

    // The referenced .umap must exist on disk: CanUseWorldAsset (inside SetWorldAsset)
    // rejects a non-existent package, and we must never leave an actor bound to a
    // broken WorldAsset. Validate up-front and fail honestly.
    if (!FPackageName::DoesPackageExist(PackageName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("World asset package does not exist on disk: %s (create/save the map first)"), *PackageName));
    }

    // Idempotent: if an ALevelInstance already references this world asset at this
    // location, return it (the engine intentionally allows N instances of the same
    // level, so we dedup by package + location).
    for (TActorIterator<ALevelInstance> It(World); It; ++It)
    {
        ALevelInstance* Existing = *It;
        if (!Existing)
        {
            continue;
        }
        const FString ExistingPkg = NormalizePackageName(Existing->GetWorldAsset().ToString());
        if (ExistingPkg.Equals(PackageName, ESearchCase::IgnoreCase) &&
            Existing->GetActorLocation().Equals(Location))
        {
            TSharedPtr<FJsonObject> Data = LevelInstanceToJson(Existing);
            Data->SetBoolField(TEXT("world_asset_set"), Existing->IsWorldAssetValid());
            Data->SetBoolField(TEXT("loaded"), Existing->IsLoaded());
            Data->SetBoolField(TEXT("already_exists"), true);
            Data->SetStringField(TEXT("note"), TEXT("level instance already exists referencing this world asset at this location"));
            return mcp_common::MakeOkReply(Data);
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("CreateLevelInstance", "Create Level Instance"));
    World->Modify();

    FActorSpawnParameters SpawnParams;
    SpawnParams.ObjectFlags = RF_Transactional;
    ALevelInstance* LevelInstance = World->SpawnActor<ALevelInstance>(Location, FRotator::ZeroRotator, SpawnParams);
    if (!LevelInstance)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ALevelInstance actor"));
    }

    if (!ActorLabel.IsEmpty())
    {
        LevelInstance->SetActorLabel(ActorLabel);
    }

    // Bind the world asset. SetWorldAsset validates CanUseWorldAsset (package exists +
    // no reference loop) and only assigns on success; it does NOT load the sub-level.
    TSoftObjectPtr<UWorld> WorldAssetSoftPtr;
    WorldAssetSoftPtr = FSoftObjectPath(WorldAssetPath);
    const bool bWorldAssetSet = LevelInstance->SetWorldAsset(WorldAssetSoftPtr);

    if (!bWorldAssetSet)
    {
        // Honest failure: do not leave an actor with no/broken WorldAsset behind.
        LevelInstance->Destroy();
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetWorldAsset rejected world asset '%s' (CanUseWorldAsset failed: package missing or reference loop)"), *WorldAssetPath));
    }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = LevelInstanceToJson(LevelInstance);
    Data->SetBoolField(TEXT("world_asset_set"), true);
    Data->SetBoolField(TEXT("loaded"), false);
    Data->SetBoolField(TEXT("already_exists"), false);
    Data->SetStringField(TEXT("note"),
        TEXT("actor created and WorldAsset bound; sublevel streaming is deferred (not loaded synchronously)"));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_level_instance_info
// ---------------------------------------------------------------------------

// MCP:get_level_instance_info - Lists every ALevelInstance actor in the editor world.  REAL read via TActorIterator<ALevelInstance>; serialises each actor's referenced world asset and loaded state. An empty list on a map with no level instances is the correct result (honest read).
//   @returns { world: string, level_instances: [{ actor_name: string, world_asset_path: string, world_asset_valid: bool, is_loaded: bool, location: [number,number,number] }], count: number }

TSharedPtr<FJsonObject> HandleGetLevelInstanceInfo(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TArray<TSharedPtr<FJsonValue>> InstanceArray;
    for (TActorIterator<ALevelInstance> It(World); It; ++It)
    {
        ALevelInstance* LevelInstance = *It;
        if (!LevelInstance)
        {
            continue;
        }
        InstanceArray.Add(MakeShared<FJsonValueObject>(LevelInstanceToJson(LevelInstance)));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("world"), World->GetName());
    Data->SetArrayField(TEXT("level_instances"), InstanceArray);
    Data->SetNumberField(TEXT("count"), InstanceArray.Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// pack_level_actors
// ---------------------------------------------------------------------------

// MCP:pack_level_actors - Bakes a saved map (.umap) into a reusable APackedLevelActor Blueprint headless: the source level's static-mesh actors are merged into ISM components on the generated Blueprint so it streams as one cheap actor.  Uses FPackedLevelActorBuilder::CreateDefaultBuilder + CreateOrUpdateBlueprint with bCheckoutAndSave=false on purpose — that builder save path is silently cancelled under -unattended (FileHelpers PR_Cancelled) and would falsely report success; instead the Blueprint package is persisted explicitly via UEditorLoadingAndSavingUtils::SavePackages and its REAL result is returned as `saved`. The build runs in the builder's own FPreviewScene (no editor-world swap, no PIE, no render). Requires the editor (GEditor). Idempotent: an existing Blueprint at blueprint_path is updated in place (the engine early-outs via PackedHash when the source is unchanged), never duplicated.
//   @param world_asset_path - Package/object path of the saved source map to pack (e.g. "/Game/Maps/Foo"). Must exist on disk.
//   @param blueprint_path - Destination package path for the APackedLevelActor Blueprint (e.g. "/Game/Maps/BPP_Foo").
//   @param save - Optional, default true. Persist the Blueprint package to disk and report the real save result in `saved`.
//   @returns { blueprint_path: string, source_world_asset: string, blueprint_created: bool, packed: bool, packed_hash: number, saved: bool, note: string }

TSharedPtr<FJsonObject> HandlePackLevelActors(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("world_asset_path"), WorldAssetPath)) { return Missing; }

    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_path"), BlueprintPath)) { return Missing; }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // The builder spawns into its own FPreviewScene but still touches GEditor->Trans;
    // without a valid editor (a commandlet launched without -editor) it cannot run.
    if (!GEditor)
    {
        return mcp_common::MakeErrorReply(
            TEXT("pack_level_actors requires the editor (GEditor); not available in this process"));
    }

    // The source map must exist on disk (the builder loads it into a preview scene).
    const FString WorldPackage = NormalizePackageName(WorldAssetPath);
    if (!FPackageName::DoesPackageExist(WorldPackage))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Source world asset package does not exist on disk: %s"), *WorldPackage));
    }

    // Resolve the destination Blueprint package + asset name.
    const FString BPPackageName = NormalizePackageName(BlueprintPath);
    const FString BPAssetName = FPackageName::GetLongPackageAssetName(BPPackageName);
    if (BPAssetName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid blueprint_path (no asset name): %s"), *BlueprintPath));
    }
    const FString BPObjectPath = BPPackageName + TEXT(".") + BPAssetName;

    // Load an existing BP (update in place) or create a fresh APackedLevelActor BP.
    // CreateOrUpdateBlueprint shows a modal picker ONLY when InBlueprintAsset is null,
    // so we always pass a concrete Blueprint -> fully headless, no dialog.
    UBlueprint* Blueprint = nullptr;
    bool bBlueprintCreated = false;
    if (FPackageName::DoesPackageExist(BPPackageName))
    {
        Blueprint = LoadObject<UBlueprint>(nullptr, *BPObjectPath);
    }
    if (!Blueprint)
    {
        FString PackageError;
        UPackage* Package = mcp_common::CreateAssetPackage(BPPackageName, BPAssetName, UBlueprint::StaticClass(), PackageError);
        if (!Package)
        {
            return mcp_common::MakeErrorReply(PackageError);
        }
        Blueprint = FKismetEditorUtilities::CreateBlueprint(
            APackedLevelActor::StaticClass(), Package, FName(*BPAssetName), BPTYPE_Normal);
        if (Blueprint)
        {
            bBlueprintCreated = true;
            FAssetRegistryModule::AssetCreated(Blueprint);
        }
    }
    if (!Blueprint)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create/load APackedLevelActor Blueprint at %s"), *BPObjectPath));
    }

    // Build the packed Blueprint from the source map (own preview scene, synchronous).
    TSharedPtr<FPackedLevelActorBuilder> Builder = FPackedLevelActorBuilder::CreateDefaultBuilder();
    if (!Builder.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create FPackedLevelActorBuilder"));
    }

    TSoftObjectPtr<UWorld> SourceWorld;
    SourceWorld = FSoftObjectPath(WorldAssetPath);
    const TSoftObjectPtr<UBlueprint> BlueprintSoft(Blueprint);

    // bCheckoutAndSave=false on purpose: the builder's own save is silently cancelled
    // under -unattended and would falsely report success. We persist explicitly below.
    const bool bPacked = Builder->CreateOrUpdateBlueprint(
        SourceWorld, BlueprintSoft, /*bCheckoutAndSave=*/false, /*bPromptForSave=*/false);
    if (!bPacked)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("CreateOrUpdateBlueprint failed to pack '%s' into '%s'"), *WorldAssetPath, *BPObjectPath));
    }

    // Read the packed hash off the generated CDO (GetPackedHash is inline -> link-safe).
    uint32 PackedHash = 0;
    if (Blueprint->GeneratedClass)
    {
        if (APackedLevelActor* CDO = Cast<APackedLevelActor>(Blueprint->GeneratedClass->GetDefaultObject()))
        {
            PackedHash = CDO->GetPackedHash();
        }
    }

    // Persist honestly: SavePackages returns the real on-disk result, never the
    // builder's cancelled-but-true save. `saved:false` is an honest write failure.
    bool bSaved = false;
    if (bSave)
    {
        TArray<UPackage*> PackagesToSave;
        PackagesToSave.Add(Blueprint->GetPackage());
        bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, /*bOnlyDirty=*/false);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_path"), BPObjectPath);
    Data->SetStringField(TEXT("source_world_asset"), WorldAssetPath);
    Data->SetBoolField(TEXT("blueprint_created"), bBlueprintCreated);
    Data->SetBoolField(TEXT("packed"), bPacked);
    Data->SetNumberField(TEXT("packed_hash"), static_cast<double>(PackedHash));
    Data->SetBoolField(TEXT("saved"), bSaved);
    Data->SetStringField(TEXT("note"),
        TEXT("BP packed in-memory via FPackedLevelActorBuilder (PreviewScene, headless); persisted explicitly with SavePackages -- 'saved' reflects the real disk write, not the builder's -unattended-cancelled save"));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// break_level_instance
// ---------------------------------------------------------------------------

// MCP:break_level_instance - Breaks an ALevelInstance actor, un-nesting its sub-level content into the current editor level as plain actors and destroying the original level-instance actor.  Canonical headless path: resolve the ALevelInstance by label/name, then ULevelInstanceSubsystem::CanBreakLevelInstance (gate) -> BlockLoadLevelInstance (synchronous load) -> BreakLevelInstance. CanBreakLevelInstance is checked BEFORE BlockLoadLevelInstance on purpose: BlockLoadLevelInstance asserts check(!IsEditing()) and CanBreakLevelInstance rejects the editing/level-script states, so this ordering prevents the assert. BreakLevelInstance returns false when nothing was moved (an empty sub-level is the honest broke=false case, NOT an error). The original actor is destroyed by the engine, so this is a one-shot destructive op: re-running with the same name reports "level instance not found". Requires the editor (GEditor); BreakLevelInstance touches GEditor->GetSelectedActors()/Trans unguarded.
//   @param actor_name - Required. Label or name of the ALevelInstance actor to break (matched via GetActorNameOrLabel).
//   @param levels - Optional, default 1. How many nested levels deep to break (passed to BreakLevelInstance Levels).
//   @param keep_folders - Optional, default false. When true, preserve the source actor folders (ELevelInstanceBreakFlags::KeepFolders) instead of flattening them.
//   @returns { actor_name: string, broke: bool, moved_actor_count: number, moved_actors: [string], levels: number, keep_folders: bool, was_loaded_forced: bool, note: string }

TSharedPtr<FJsonObject> HandleBreakLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    // Levels defaults to 1; the JSON number is read as double then narrowed to uint32.
    uint32 Levels = 1;
    {
        double LevelsNum = 1.0;
        if (Params->TryGetNumberField(TEXT("levels"), LevelsNum) && LevelsNum >= 1.0)
        {
            Levels = static_cast<uint32>(LevelsNum);
        }
    }

    bool bKeepFolders = false;
    Params->TryGetBoolField(TEXT("keep_folders"), bKeepFolders);

    // Resolve the target ALevelInstance by label/name (REAL read). BreakLevelInstance dereferences
    // GEditor (GetSelectedActors / Trans) without a guard, so a missing editor is refused first.
    UWorld* World = nullptr;
    ALevelInstance* TargetLI = nullptr;
    ULevelInstanceSubsystem* Sub = nullptr;
    if (TSharedPtr<FJsonObject> Error = LevelInstance_ResolveTarget(ActorName, TEXT("break_level_instance"), World, TargetLI, Sub))
    {
        return Error;
    }

    // Capture the resolved label NOW: BreakLevelInstance destroys the original actor,
    // so reading it back after the break would be a use-after-destroy.
    const FString ResolvedName = TargetLI->GetActorNameOrLabel();

    // ILevelInstanceInterface is a base of ALevelInstance (implicit upcast).
    ILevelInstanceInterface* LI = TargetLI;

    // Gate FIRST: CanBreakLevelInstance rejects the editing / level-script states.
    // This MUST run before BlockLoadLevelInstance, which asserts check(!IsEditing())
    // and would crash on a level instance currently being edited.
    if (!Sub->CanBreakLevelInstance(LI))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("cannot break level instance '%s' (it is being edited, has property overrides in flight, or contains a level script blueprint)"), *ResolvedName));
    }

    const FScopedTransaction Transaction(LOCTEXT("BreakLevelInstance", "Break Level Instance"));

    // Force the sub-level fully streamed in synchronously so its actors are present
    // before the break copies them out (void; safe: editing is ruled out above).
    Sub->BlockLoadLevelInstance(LI);
    const bool bWasLoadedForced = true;

    // Break: returns false when nothing was moved (empty sub-level -> honest broke=false).
    TArray<AActor*> OutMoved;
    const ELevelInstanceBreakFlags Flags = bKeepFolders ? ELevelInstanceBreakFlags::KeepFolders : ELevelInstanceBreakFlags::None;
    const bool bBroke = Sub->BreakLevelInstance(LI, Levels, &OutMoved, Flags);

    TArray<TSharedPtr<FJsonValue>> MovedArr;
    for (AActor* Moved : OutMoved)
    {
        if (Moved)
        {
            MovedArr.Add(MakeShared<FJsonValueString>(Moved->GetActorNameOrLabel()));
        }
    }

    World->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), ResolvedName);
    Data->SetBoolField(TEXT("broke"), bBroke);
    Data->SetNumberField(TEXT("moved_actor_count"), OutMoved.Num());
    Data->SetArrayField(TEXT("moved_actors"), MovedArr);
    Data->SetNumberField(TEXT("levels"), static_cast<double>(Levels));
    Data->SetBoolField(TEXT("keep_folders"), bKeepFolders);
    Data->SetBoolField(TEXT("was_loaded_forced"), bWasLoadedForced);
    Data->SetStringField(TEXT("note"),
        bBroke
            ? TEXT("level instance broken; sub-level actors moved into the current level and the original level-instance actor was destroyed")
            : TEXT("level instance broke with 0 moved actors (empty sub-level) -- honest broke:false; the original actor was still consumed by the engine"));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// edit_level_instance
// ---------------------------------------------------------------------------

// MCP:edit_level_instance - Enters in-place edit mode on an ALevelInstance: the engine unloads the instanced sub-level and re-loads it as an editable streaming level so its actors can be modified in the current editor world, then closed by commit_level_instance.  Canonical headless path: resolve the ALevelInstance by label/name, then ULevelInstanceSubsystem::CanEditLevelInstance (gate) -> EditLevelInstance. CanEditLevelInstance is checked BEFORE EditLevelInstance on purpose: EditLevelInstanceInternal opens with check(CanEditLevelInstance(LI)) — a HARD ASSERT — so this ordering turns an un-editable instance into an honest error (with the engine's Reason) instead of a crash. Idempotent: an instance already in edit mode is a no-op (already_editing:true, editing:true). DEFENSIVE CRASH GUARD: on a World-Partitioned map the edit path calls FModuleManager::GetModuleChecked("LevelInstanceEditor") which asserts if that module is not loaded; when the world is partitioned AND the LevelInstanceEditor module is not loaded we fail honestly instead of proceeding. (create_level_instance produces non-partitioned maps, so the normal flow passes this guard.) Requires the editor (GEditor); EditLevelInstance touches GEditor->Trans / selection unguarded. The edit resets the undo stack and streams the world asynchronously (deferred world-swap), so the edit MUST be closed by a matching commit_level_instance.
//   @param actor_name - Required. Label or name of the ALevelInstance actor to open for edit (matched via GetActorNameOrLabel).
//   @returns { actor_name: string, editing: bool, already_editing: bool, note: string }

TSharedPtr<FJsonObject> HandleEditLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    // Resolve the target ALevelInstance by label/name (REAL read). EditLevelInstance dereferences
    // GEditor (SelectNone / ResetTransaction / Trans) without a guard, so a missing editor is refused first.
    UWorld* World = nullptr;
    ALevelInstance* TargetLI = nullptr;
    ULevelInstanceSubsystem* Sub = nullptr;
    if (TSharedPtr<FJsonObject> Error = LevelInstance_ResolveTarget(ActorName, TEXT("edit_level_instance"), World, TargetLI, Sub))
    {
        return Error;
    }

    const FString ResolvedName = TargetLI->GetActorNameOrLabel();

    // ILevelInstanceInterface is a base of ALevelInstance (implicit upcast).
    ILevelInstanceInterface* LI = TargetLI;

    UE_LOG(LogTemp, Verbose, TEXT("[edit_level_instance] target='%s' partitioned=%d editing=%d"),
        *ResolvedName, World->GetWorldPartition() != nullptr ? 1 : 0, Sub->IsEditingLevelInstance(LI) ? 1 : 0);

    // Idempotent: an instance already in edit mode is a no-op (no re-entry).
    if (Sub->IsEditingLevelInstance(LI))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actor_name"), ResolvedName);
        Data->SetBoolField(TEXT("editing"), true);
        Data->SetBoolField(TEXT("already_editing"), true);
        Data->SetStringField(TEXT("note"),
            TEXT("level instance was already in edit mode (no-op); close it with commit_level_instance"));
        return mcp_common::MakeOkReply(Data);
    }

    // DEFENSIVE CRASH GUARD: EditLevelInstanceInternal -> CanEditLevelInstance takes a
    // FModuleManager::GetModuleChecked("LevelInstanceEditor") path on a partitioned +
    // streaming-enabled world, which asserts if that module is not loaded. If the world
    // is partitioned and the module is not loaded, fail honestly rather than risk the
    // assert. (create_level_instance produces non-partitioned maps -> guard passes.)
    if (World->GetWorldPartition() != nullptr &&
        !FModuleManager::Get().IsModuleLoaded(TEXT("LevelInstanceEditor")))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("cannot edit level instance '%s': level instance edit requires the LevelInstanceEditor module (this world is World-Partitioned and the module is not loaded) -- use a non-partitioned map or run with the editor UI"), *ResolvedName));
    }

    // Gate FIRST: CanEditLevelInstance rejects un-editable states. This MUST run before
    // EditLevelInstance, which opens with check(CanEditLevelInstance(LI)) — a HARD ASSERT.
    // Return the engine's own Reason so the failure is honest and diagnosable.
    FText EditReason;
    if (!Sub->CanEditLevelInstance(LI, &EditReason))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("cannot edit level instance '%s': %s"), *ResolvedName, *EditReason.ToString()));
    }

    // Edit resets the undo stack (GEditor->ResetTransaction) and streams the sub-level
    // asynchronously (deferred world-swap), so do NOT wrap it in a ScopedTransaction.
    Sub->EditLevelInstance(LI, nullptr);

    // Read back the real edit state: GetEditingLevelInstance() is the live pointer.
    const bool bEditing = (Sub->GetEditingLevelInstance() == LI);

    UE_LOG(LogTemp, Verbose, TEXT("[edit_level_instance] '%s' EditLevelInstance done; editing=%d"),
        *ResolvedName, bEditing ? 1 : 0);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), ResolvedName);
    Data->SetBoolField(TEXT("editing"), bEditing);
    Data->SetBoolField(TEXT("already_editing"), false);
    Data->SetStringField(TEXT("note"),
        TEXT("entered in-place edit mode; sub-level re-loads as an editable streaming level (deferred/async world-swap) and the undo stack was reset -- this edit MUST be closed by commit_level_instance"));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// commit_level_instance
// ---------------------------------------------------------------------------

// MCP:commit_level_instance - Closes the in-place edit opened by edit_level_instance: the engine commits (or discards) the edits, saves the sub-level .umap back to disk, and re-collapses the streaming level into the instanced ALevelInstance.  Canonical headless path: resolve the ALevelInstance by label/name, then ULevelInstanceSubsystem::CanCommitLevelInstance (gate) -> CommitLevelInstance. When the instance is NOT being edited, CanCommitLevelInstance returns false with reason "not currently being edited" — that is reported honestly as { committed:false, was_editing:false } and is a SUCCESS (a no-op close), NOT an error, so this is safe to call unconditionally to guarantee no dangling edit-mode. CommitLevelInstance returns whether the sub-level package save succeeded (committed:false on an honest save failure). Optional discard (default false) throws the edits away instead of saving. Requires the editor (GEditor); CommitLevelInstance touches GEditor->Trans / selection unguarded. Under -unattended the commit's save dialogs do not block.
//   @param actor_name - Required. Label or name of the ALevelInstance actor whose edit to commit (matched via GetActorNameOrLabel).
//   @param discard - Optional, default false. When true, discard the in-place edits instead of saving them back to the sub-level .umap.
//   @returns { actor_name: string, committed: bool, was_editing: bool, discarded: bool, note: string }

TSharedPtr<FJsonObject> HandleCommitLevelInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    bool bDiscard = false;
    Params->TryGetBoolField(TEXT("discard"), bDiscard);

    // Resolve the target ALevelInstance by label/name (REAL read). CommitLevelInstance dereferences
    // GEditor (SelectNone / SelectActor / Trans) without a guard, so a missing editor is refused first.
    UWorld* World = nullptr;
    ALevelInstance* TargetLI = nullptr;
    ULevelInstanceSubsystem* Sub = nullptr;
    if (TSharedPtr<FJsonObject> Error = LevelInstance_ResolveTarget(ActorName, TEXT("commit_level_instance"), World, TargetLI, Sub))
    {
        return Error;
    }

    const FString ResolvedName = TargetLI->GetActorNameOrLabel();

    ILevelInstanceInterface* LI = TargetLI;

    // Gate: CanCommitLevelInstance returns false with "not currently being edited" when the
    // instance is not in edit mode. That is an HONEST no-op close (committed:false,
    // was_editing:false), reported as SUCCESS — NOT an error — so calling this on a
    // not-being-edited instance is safe and idempotent.
    FText CommitReason;
    if (!Sub->CanCommitLevelInstance(LI, bDiscard, &CommitReason))
    {
        UE_LOG(LogTemp, Verbose, TEXT("[commit_level_instance] '%s' not committable: %s"),
            *ResolvedName, *CommitReason.ToString());

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actor_name"), ResolvedName);
        Data->SetBoolField(TEXT("committed"), false);
        Data->SetBoolField(TEXT("was_editing"), false);
        Data->SetBoolField(TEXT("discarded"), bDiscard);
        Data->SetStringField(TEXT("note"),
            FString::Printf(TEXT("no in-place edit to commit (honest no-op): %s"), *CommitReason.ToString()));
        return mcp_common::MakeOkReply(Data);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[commit_level_instance] committing '%s' discard=%d"),
        *ResolvedName, bDiscard ? 1 : 0);

    // CommitLevelInstance returns whether the sub-level package save succeeded.
    const bool bCommitted = Sub->CommitLevelInstance(LI, bDiscard);

    UE_LOG(LogTemp, Verbose, TEXT("[commit_level_instance] '%s' done; committed=%d"),
        *ResolvedName, bCommitted ? 1 : 0);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), ResolvedName);
    Data->SetBoolField(TEXT("committed"), bCommitted);
    Data->SetBoolField(TEXT("was_editing"), true);
    Data->SetBoolField(TEXT("discarded"), bDiscard);
    Data->SetStringField(TEXT("note"),
        bDiscard
            ? TEXT("in-place edit discarded and the editable streaming level re-collapsed into the level instance (no save)")
            : (bCommitted
                ? TEXT("in-place edit committed; the sub-level .umap was saved to disk and the streaming level re-collapsed into the level instance")
                : TEXT("in-place edit closed but the sub-level package save reported failure (committed:false is an honest save-failure, e.g. read-only/cancelled) -- the edit mode was still exited")));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("break_level_instance"), &HandleBreakLevelInstance },
    { TEXT("commit_level_instance"), &HandleCommitLevelInstance },
    { TEXT("create_level_instance"), &HandleCreateLevelInstance },
    { TEXT("edit_level_instance"), &HandleEditLevelInstance },
    { TEXT("get_level_instance_info"), &HandleGetLevelInstanceInfo },
    { TEXT("pack_level_actors"), &HandlePackLevelActors },
};
} // namespace UEAgentLevelInstanceCommands

#undef LOCTEXT_NAMESPACE
