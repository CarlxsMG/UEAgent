// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "ScopedTransaction.h"

// Asset creation / persistence (mirrors UEAgentDataAssetCommands).
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

// EQS asset (AIModule).
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
// EQS synchronous execution (manager + request/result) + discovery (contexts, item types) + reflection.
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/EnvQueryContext.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_VectorBase.h"
#include "UObject/UnrealType.h"          // FClassProperty / FStructProperty / FNumericProperty (EQS generator props)

// Navigation actors + system (NavigationSystem + AIModule).
#include "NavModifierVolume.h"                 // ANavModifierVolume (NavigationSystem)
#include "Navigation/NavLinkProxy.h"           // ANavLinkProxy (AIModule)
#include "NavAreas/NavArea.h"                  // UNavArea
#include "NavigationSystem.h"                  // UNavigationSystemV1
#include "NavigationData.h"                    // ANavigationData
#include "NavMesh/RecastNavMesh.h"             // ARecastNavMesh + ENavigationDataResolution accessors
#include "AI/Navigation/NavigationTypes.h"     // FNavDataConfig (Engine module)

// diagnose_navmesh: nav bounds volumes, brush validity, invokers, tile counts.
#include "EngineUtils.h"                        // TActorIterator
#include "NavMesh/NavMeshBoundsVolume.h"        // ANavMeshBoundsVolume
#include "NavigationInvokerComponent.h"         // UNavigationInvokerComponent
#include "Engine/Brush.h"                       // ABrush::Brush (UModel*)
#include "Model.h"                              // UModel
#include "Engine/Polys.h"                       // UPolys (editor-only brush poly count)
#include "Components/BrushComponent.h"          // UBrushComponent

// Collision channels: config-driven channel <-> friendly-name + trace/object query index.
#include "Engine/CollisionProfile.h"           // UCollisionProfile, FCustomChannelSetup
#include "Engine/EngineTypes.h"                 // ETraceTypeQuery / EObjectTypeQuery / ECollisionChannel
#include "Misc/ConfigCacheIni.h"               // GConfig, FConfigFile
#include "Misc/Paths.h"                         // FPaths

/**
 * Navigation + EQS commands: EQS query assets (create, inspect, edit their options, generators and tests,
 * run them synchronously, list the generator/test/context types), nav modifier volumes and nav link
 * proxies, navmesh build, settings and diagnosis, and the project's collision-channel mapping (dump, and
 * diff against another project).
 */
namespace UEAgentNavigationCommands
{

// MCP-DOMAIN: Navigation

namespace
{
    // The reply of create_eqs_query before its outcome: the request, then the query the path holds.
    TSharedPtr<FJsonObject> NavEqs_CreatedQueryReply(const FString& Name, const FString& PackagePath, const UEnvQuery* Query)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("asset_path"), Query->GetPathName());
        Reply->SetStringField(TEXT("asset_class"), Query->GetClass()->GetName());
        Reply->SetStringField(TEXT("query_name"), Query->GetQueryName().ToString());
        Reply->SetBoolField(TEXT("has_options"), Query->GetOptions().Num() > 0);
        Reply->SetNumberField(TEXT("option_count"), Query->GetOptions().Num());
        return Reply;
    }
}

// ----------------------------------------------------------------------------
// create_eqs_query
// UEnvQuery is a plain UDataAsset (Runtime/AIModule/Classes/EnvironmentQuery/EnvQuery.h,
// UCLASS(BlueprintType, MinimalAPI)). This creates and persists the empty query asset;
// its options, generators and tests are added afterwards by add_eqs_generator and
// add_eqs_test. QueryName follows the asset's own name (it is patched from the asset name
// on PostLoad anyway). Honest about emptiness: returns has_options=false.
// ----------------------------------------------------------------------------
// MCP:create_eqs_query - Create an empty Environment Query System asset (UEnvQuery) named name in package_path (default /Game/EQS), with no options yet, and save it. Idempotent: a query already at that path is returned as it is, with already_exists=true and saved=false, and nothing is saved; an asset of another class there is an error. A failed save (PIE running, read-only file) makes success false with saved=false, the new query kept in memory. Then give it an option and a generator with add_eqs_generator and tests with add_eqs_test, tune them with set_eqs_test_scoring, inspect it with get_eqs_query_info and run it with run_eqs_query.
//   @param name - Asset name for the new UEnvQuery
//   @param package_path - Target content package path where the asset is created (default /Game/EQS)
//   @returns { name, package_path, asset_path, asset_class, query_name, has_options, option_count, already_exists, saved, success, message }
TSharedPtr<FJsonObject> HandleCreateEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    FString PackagePath;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/EQS");
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: an existing query is returned as it is, in a stable payload shape, with nothing saved.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        if (UEnvQuery* Existing = Cast<UEnvQuery>(StaticLoadObject(UEnvQuery::StaticClass(), nullptr, *FullPath)))
        {
            TSharedPtr<FJsonObject> ResultObj = NavEqs_CreatedQueryReply(Name, PackagePath, Existing);
            ResultObj->SetBoolField(TEXT("already_exists"), true);
            ResultObj->SetBoolField(TEXT("saved"), false);
            ResultObj->SetBoolField(TEXT("success"), true);
            ResultObj->SetStringField(TEXT("message"),
                TEXT("EQS query already exists (loaded, left unchanged and not saved)."));
            return mcp_common::MakeOkReply(ResultObj);
        }
    }

    FString CreateError;
    UEnvQuery* NewQuery = mcp_common::NewAssetInPackage<UEnvQuery>(FullPath, Name, TEXT("Failed to create UEnvQuery asset"), CreateError);
    if (!NewQuery)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewQuery);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewQuery->GetOutermost(), NewQuery, SaveError);

    TSharedPtr<FJsonObject> ResultObj = NavEqs_CreatedQueryReply(Name, PackagePath, NewQuery);
    ResultObj->SetBoolField(TEXT("already_exists"), false);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetBoolField(TEXT("success"), bSaved);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? TEXT("Empty EQS query asset created and saved; give it a generator with add_eqs_generator and tests with add_eqs_test.")
        : FString::Printf(TEXT("EQS query created but save failed: %s"), *SaveError));

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_eqs_query"), TEXT("bSaved")));
}

// Helpers for the editor-spawn and navmesh-settings handlers below.
namespace UEAgentNavigationCommandsImpl
{
    // The editor's actor subsystem, which places actors the way the editor UI does. Needs an
    // editor world; when something is missing, OutFailure holds the reply to send instead.
    UEditorActorSubsystem* EditorActorSpawner(TSharedPtr<FJsonObject>& OutFailure)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("World not available")))
        {
            OutFailure = NoWorld;
            return nullptr;
        }
        UEditorActorSubsystem* Spawner = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
        if (Spawner == nullptr)
        {
            OutFailure = mcp_common::MakeErrorReply(TEXT("EditorActorSubsystem not available"));
        }
        return Spawner;
    }

    // Identity of a freshly spawned actor, as both spawn replies report it.
    TSharedPtr<FJsonObject> SpawnedActorReply(const AActor* Spawned)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("actor_name"), Spawned->GetName());
        Reply->SetStringField(TEXT("actor_label"), Spawned->GetActorLabel());
        Reply->SetStringField(TEXT("actor_path"), Spawned->GetPathName());
        Reply->SetStringField(TEXT("actor_class"), Spawned->GetClass()->GetName());
        return Reply;
    }

    // Records the clamped setting Value under Field in Applied and returns it, so the reply reports the value written.
    float RecordApplied(const TSharedPtr<FJsonObject>& Applied, const TCHAR* Field, float Value)
    {
        Applied->SetNumberField(Field, Value);
        return Value;
    }
}

// ----------------------------------------------------------------------------
// spawn_nav_modifier_volume
// ANavModifierVolume is an AVolume (Runtime/NavigationSystem/Public/NavModifierVolume.h),
// so like spawn_box_volume we must build a box brush for it (mcp_common::BuildCubeBrushForVolume).
// Optional area_class resolves a UNavArea subclass; we apply it via SetAreaClass (public,
// NAVIGATIONSYSTEM_API).
// ----------------------------------------------------------------------------
// MCP:spawn_nav_modifier_volume - Spawn an ANavModifierVolume in the editor world at location, give it a real box brush of half-extent extent (a spawned volume has none) and optionally set its nav area class, in one undo step, then label it with its internal name so label lookups find it. Over spawn_box_volume(volume_class='NavModifierVolume') it sets the area class in the same call; spawn_box_volume instead re-reads the bounds against the request. area_class names a UNavArea subclass already loaded (e.g. NavArea_Null or NavArea_Obstacle; a Blueprint area by its _C name); a name that does not resolve is not an error: area_class_applied comes back false and the default area stays. An extent axis of 0 or less becomes 100. Each call adds a new actor. Not saved (save_level); this call does not rebuild navigation (rebuild_navigation). Refused during PIE. Navmesh bounds: spawn_navmesh_bounds_volume; links: spawn_nav_link_proxy.
//   @param location - [x, y, z] world location for the volume in cm; missing spawns at the origin, and any other value is an error and nothing is spawned
//   @param area_class - Optional UNavArea subclass name to apply via SetAreaClass, e.g. NavArea_Null; unresolved names are ignored with area_class_applied false
//   @param extent - Optional [x, y, z] box half-extent in cm (default 100,100,100, a 200 cm box); a component of 0 or less becomes 100
//   @returns { actor_name, actor_label, actor_path, actor_class, brush_built, area_class_applied, area_class, success }
TSharedPtr<FJsonObject> HandleSpawnNavModifierVolume(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> SpawnerFailure;
    UEditorActorSubsystem* EditorActorSubsystem = UEAgentNavigationCommandsImpl::EditorActorSpawner(SpawnerFailure);
    if (EditorActorSubsystem == nullptr)
    {
        return SpawnerFailure;
    }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    // Optional half-extent; defaults to 100uu half-extent -> 200uu full box (the cube
    // builder's own default size).
    FVector Extent(100.0, 100.0, 100.0);
    if (Params->HasField(TEXT("extent")))
    {
        Extent = mcp_common::ReadJsonVector(Params, TEXT("extent"));
        if (Extent.X <= 0.0) Extent.X = 100.0;
        if (Extent.Y <= 0.0) Extent.Y = 100.0;
        if (Extent.Z <= 0.0) Extent.Z = 100.0;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SpawnNavModifierVolume", "Spawn Nav Modifier Volume"));

    AActor* NewActor = EditorActorSubsystem->SpawnActorFromClass(ANavModifierVolume::StaticClass(), Location, FRotator::ZeroRotator);
    if (!NewActor)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ANavModifierVolume actor"));
    }

    ANavModifierVolume* NavVolume = Cast<ANavModifierVolume>(NewActor);

    // Build a default box brush so the volume has real geometry/bounds (a brush-based
    // volume spawns with no brush).
    const bool bBrushBuilt = mcp_common::BuildCubeBrushForVolume(NewActor, Extent);

    // Optional nav area class to apply.
    FString AreaClassName;
    FString AppliedAreaClass;
    bool bAreaClassApplied = false;
    if (Params->TryGetStringField(TEXT("area_class"), AreaClassName) && !AreaClassName.IsEmpty() && NavVolume)
    {
        UClass* AreaClass = UClass::TryFindTypeSlow<UClass>(AreaClassName);
        if (!AreaClass)
        {
            AreaClass = FindFirstObject<UClass>(*AreaClassName);
        }
        if (AreaClass && AreaClass->IsChildOf(UNavArea::StaticClass()))
        {
            NavVolume->SetAreaClass(TSubclassOf<UNavArea>(AreaClass));
            AppliedAreaClass = AreaClass->GetName();
            bAreaClassApplied = true;
        }
    }

    NewActor->SetActorLabel(NewActor->GetName());

    TSharedPtr<FJsonObject> Reply = UEAgentNavigationCommandsImpl::SpawnedActorReply(NewActor);
    Reply->SetBoolField(TEXT("brush_built"), bBrushBuilt);
    Reply->SetBoolField(TEXT("area_class_applied"), bAreaClassApplied);
    if (bAreaClassApplied)
    {
        Reply->SetStringField(TEXT("area_class"), AppliedAreaClass);
    }
    Reply->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Reply);
}

// ----------------------------------------------------------------------------
// spawn_nav_link_proxy
// ANavLinkProxy is a plain AActor (Runtime/AIModule/Classes/Navigation/NavLinkProxy.h),
// so a straight editor-actor spawn is all that's needed.
// ----------------------------------------------------------------------------
// MCP:spawn_nav_link_proxy - Spawn an ANavLinkProxy actor (with its class-default point link) in the editor world at location through the editor actor subsystem, in one undo step, and label it with its internal name so label lookups find it. spawn_actor has no nav-link type. Each call adds a new actor (not idempotent); the link endpoints are not set here -- write its PointLinks with set_properties. The level is not saved (save_level) and this call does not rebuild navigation (rebuild_navigation). During PIE the editor refuses the spawn and the call is an error, as it is without an editor world. Move it with set_actor_transform; inverse: delete_actor. An area that changes path cost or blocks: spawn_nav_modifier_volume; the navmesh bounds: spawn_navmesh_bounds_volume.
//   @param location - [x, y, z] world location for the nav link proxy in cm; missing spawns at the origin, and any other value is an error and nothing is spawned
//   @returns { actor_name, actor_label, actor_path, actor_class, success }
TSharedPtr<FJsonObject> HandleSpawnNavLinkProxy(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> SpawnerFailure;
    UEditorActorSubsystem* Spawner = UEAgentNavigationCommandsImpl::EditorActorSpawner(SpawnerFailure);
    if (Spawner == nullptr)
    {
        return SpawnerFailure;
    }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SpawnNavLinkProxy", "Spawn Nav Link Proxy"));

    // A plain actor: the editor spawn is all it needs.
    AActor* NewActor = Spawner->SpawnActorFromClass(ANavLinkProxy::StaticClass(), Location, FRotator::ZeroRotator);
    if (!NewActor)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ANavLinkProxy actor"));
    }

    // Label = internal name, so label-based lookups find it.
    NewActor->SetActorLabel(NewActor->GetName());

    TSharedPtr<FJsonObject> Reply = UEAgentNavigationCommandsImpl::SpawnedActorReply(NewActor);
    Reply->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Reply);
}

// ----------------------------------------------------------------------------
// rebuild_navigation
// UNavigationSystemV1::Build() (public, NAVIGATIONSYSTEM_API) triggers navigation
// building on all eligible navigation data for the editor world.
// ----------------------------------------------------------------------------
// MCP:rebuild_navigation - Triggers a navigation build on the editor world's navigation system
//   @returns { rebuilt, nav_system_present, message, success, nav_data_count }
TSharedPtr<FJsonObject> HandleRebuildNavigation(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("World not available"))) { return NoWorld; }

    UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
    if (!NavSys)
    {
        // Honest: no navigation system in this world (e.g. an empty level with no nav data).
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetBoolField(TEXT("rebuilt"), false);
        ResultObj->SetBoolField(TEXT("nav_system_present"), false);
        ResultObj->SetStringField(TEXT("message"),
            TEXT("No navigation system present in the editor world; nothing to rebuild."));
        ResultObj->SetBoolField(TEXT("success"), true);
        return mcp_common::MakeOkReply(ResultObj);
    }

    NavSys->Build();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("rebuilt"), true);
    ResultObj->SetBoolField(TEXT("nav_system_present"), true);
    ResultObj->SetNumberField(TEXT("nav_data_count"), NavSys->NavDataSet.Num());
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetStringField(TEXT("message"), TEXT("Navigation build triggered on the editor world."));

    return mcp_common::MakeOkReply(ResultObj);
}

// ----------------------------------------------------------------------------
// Shared resolver: the editor world's default RecastNavMesh, if any.
// UNavigationSystemV1::GetDefaultNavDataInstance(FNavigationSystem::DontCreate)
// fetches the registered default nav data WITHOUT spawning one (the
// signature takes FNavigationSystem::ECreateIfMissing, DontCreate == 0). Returns
// null when there is no nav system or no RecastNavMesh (e.g. a level with no
// NavMeshBoundsVolume yet) -> the callers emit an honest gate, never a fake actor.
// ----------------------------------------------------------------------------
/** Resolve the editor world's default ARecastNavMesh without creating one; null when absent. */
ARecastNavMesh* ResolveDefaultRecastNavMesh(UWorld* World)
{
    if (!World)
    {
        return nullptr;
    }

    UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
    if (!NavSys)
    {
        return nullptr;
    }

    ANavigationData* DefaultNavData = NavSys->GetDefaultNavDataInstance(FNavigationSystem::DontCreate);
    return Cast<ARecastNavMesh>(DefaultNavData);
}

// ----------------------------------------------------------------------------
// set_recast_navmesh_settings
// Data-only edit of the editor world's default ARecastNavMesh
// (Runtime/NavigationSystem/Public/NavMesh/RecastNavMesh.h). Only the parameters
// actually present in the request are applied (idempotent partial update). Every
// numeric value is clamped server-side BEFORE being written, so the engine never
// sees an out-of-range value.
//
// Direct public UPROPERTY floats: AgentRadius, AgentHeight, AgentMaxSlope,
// TileSizeUU, MergeRegionSize, MinRegionArea, MaxSimplificationError,
// SimplificationElevationRatio. The deprecated flat CellSize/CellHeight/
// AgentMaxStepHeight (UE_DEPRECATED(all)) and MaxVerticalMergeError (UE_DEPRECATED(5.5))
// are intentionally NOT written; the resolution-aware Set*(...Default, v) accessors
// are used instead.
//
// HONEST gate: if there is no RecastNavMesh in the editor world we return
// recast_navmesh_present=false with a note (success stays true — nothing failed,
// there is simply nothing to configure yet).
// ----------------------------------------------------------------------------
// MCP:set_recast_navmesh_settings - Configures generation settings on the editor world's default ARecastNavMesh (partial/idempotent; only provided params are applied and all numeric values are clamped server-side first).
//   @param agent_radius - Optional float; ARecastNavMesh::AgentRadius (>= 0)
//   @param agent_height - Optional float; ARecastNavMesh::AgentHeight (>= 0)
//   @param agent_max_slope - Optional float degrees; ARecastNavMesh::AgentMaxSlope (clamped to <= 89)
//   @param agent_max_step_height - Optional float; SetAgentMaxStepHeight(Default, v) (>= 0)
//   @param tile_size_uu - Optional float; ARecastNavMesh::TileSizeUU (clamped to >= 300)
//   @param cell_size - Optional float; SetCellSize(Default, v) (clamped 1..1024)
//   @param cell_height - Optional float; SetCellHeight(Default, v) (clamped 1..1024)
//   @param merge_region_size - Optional float; ARecastNavMesh::MergeRegionSize (>= 0)
//   @param min_region_area - Optional float; ARecastNavMesh::MinRegionArea (>= 0)
//   @param max_simplification_error - Optional float; ARecastNavMesh::MaxSimplificationError (>= 0)
//   @param simplification_elevation_ratio - Optional float; ARecastNavMesh::SimplificationElevationRatio (>= 0)
//   @param rebuild - Optional bool (default false); when true, calls UNavigationSystemV1::Build() synchronously to apply
//   @returns { recast_navmesh_present, navmesh_name, applied:{field:value}, requires_rebuild, rebuilt, note, success }
TSharedPtr<FJsonObject> HandleSetRecastNavMeshSettings(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("World not available"))) { return NoWorld; }

    ARecastNavMesh* Recast = ResolveDefaultRecastNavMesh(World);
    if (!Recast)
    {
        UE_LOG(LogTemp, Verbose,
            TEXT("[MCP] set_recast_navmesh_settings: no RecastNavMesh in editor world; emitting honest gate."));
        TSharedPtr<FJsonObject> GateObj = MakeShared<FJsonObject>();
        GateObj->SetBoolField(TEXT("recast_navmesh_present"), false);
        GateObj->SetBoolField(TEXT("success"), true);
        GateObj->SetStringField(TEXT("note"),
            TEXT("No RecastNavMesh in editor world; add a NavMeshBoundsVolume and rebuild_navigation first."));
        return mcp_common::MakeOkReply(GateObj);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_recast_navmesh_settings: applying to '%s'."), *Recast->GetName());

    Recast->Modify();

    // Collect the applied field->value pairs for an honest echo of exactly what changed.
    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    using UEAgentNavigationCommandsImpl::RecordApplied;

    // --- Direct public UPROPERTY floats (clamp BEFORE writing) ---------------
    double Value = 0.0;

    if (Params->TryGetNumberField(TEXT("agent_radius"), Value))
    {
        Recast->AgentRadius = RecordApplied(Applied, TEXT("agent_radius"), FMath::Max(0.0f, static_cast<float>(Value)));
    }
    if (Params->TryGetNumberField(TEXT("agent_height"), Value))
    {
        Recast->AgentHeight = RecordApplied(Applied, TEXT("agent_height"), FMath::Max(0.0f, static_cast<float>(Value)));
    }
    if (Params->TryGetNumberField(TEXT("agent_max_slope"), Value))
    {
        Recast->AgentMaxSlope = RecordApplied(Applied, TEXT("agent_max_slope"), FMath::Clamp(static_cast<float>(Value), 0.0f, 89.0f));
    }
    if (Params->TryGetNumberField(TEXT("tile_size_uu"), Value))
    {
        Recast->TileSizeUU = RecordApplied(Applied, TEXT("tile_size_uu"), FMath::Max(300.0f, static_cast<float>(Value)));
    }
    if (Params->TryGetNumberField(TEXT("merge_region_size"), Value))
    {
        Recast->MergeRegionSize = RecordApplied(Applied, TEXT("merge_region_size"), FMath::Max(0.0f, static_cast<float>(Value)));
    }
    if (Params->TryGetNumberField(TEXT("min_region_area"), Value))
    {
        Recast->MinRegionArea = RecordApplied(Applied, TEXT("min_region_area"), FMath::Max(0.0f, static_cast<float>(Value)));
    }
    if (Params->TryGetNumberField(TEXT("max_simplification_error"), Value))
    {
        Recast->MaxSimplificationError = RecordApplied(Applied, TEXT("max_simplification_error"), FMath::Max(0.0f, static_cast<float>(Value)));
    }
    if (Params->TryGetNumberField(TEXT("simplification_elevation_ratio"), Value))
    {
        Recast->SimplificationElevationRatio = RecordApplied(Applied, TEXT("simplification_elevation_ratio"), FMath::Max(0.0f, static_cast<float>(Value)));
    }

    // --- Resolution-aware accessors (Default resolution) ---------------------
    if (Params->TryGetNumberField(TEXT("cell_size"), Value))
    {
        Recast->SetCellSize(ENavigationDataResolution::Default,
            RecordApplied(Applied, TEXT("cell_size"), FMath::Clamp(static_cast<float>(Value), 1.0f, 1024.0f)));
    }
    if (Params->TryGetNumberField(TEXT("cell_height"), Value))
    {
        Recast->SetCellHeight(ENavigationDataResolution::Default,
            RecordApplied(Applied, TEXT("cell_height"), FMath::Clamp(static_cast<float>(Value), 1.0f, 1024.0f)));
    }
    if (Params->TryGetNumberField(TEXT("agent_max_step_height"), Value))
    {
        Recast->SetAgentMaxStepHeight(ENavigationDataResolution::Default,
            RecordApplied(Applied, TEXT("agent_max_step_height"), FMath::Max(0.0f, static_cast<float>(Value))));
    }

    Recast->MarkPackageDirty();

    // Optional synchronous rebuild so changes take effect immediately.
    bool bRebuild = false;
    Params->TryGetBoolField(TEXT("rebuild"), bRebuild);

    bool bRebuilt = false;
    bool bRequiresRebuild = false;
    FString Note;

    if (bRebuild)
    {
        if (UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World))
        {
            UE_LOG(LogTemp, Verbose,
                TEXT("[MCP] set_recast_navmesh_settings: rebuild=true, triggering NavSys->Build()."));
            NavSys->Build();
            bRebuilt = true;
            Note = TEXT("Settings applied and navigation rebuild triggered.");
        }
        else
        {
            bRequiresRebuild = true;
            Note = TEXT("Settings applied but navigation system disappeared; call rebuild_navigation to apply.");
        }
    }
    else
    {
        bRequiresRebuild = true;
        Note = TEXT("Settings applied; call rebuild_navigation to apply.");
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("recast_navmesh_present"), true);
    ResultObj->SetStringField(TEXT("navmesh_name"), Recast->GetName());
    ResultObj->SetObjectField(TEXT("applied"), Applied);
    ResultObj->SetBoolField(TEXT("requires_rebuild"), bRequiresRebuild);
    ResultObj->SetBoolField(TEXT("rebuilt"), bRebuilt);
    ResultObj->SetStringField(TEXT("note"), Note);
    ResultObj->SetBoolField(TEXT("success"), true);

    return mcp_common::MakeOkReply(ResultObj);
}

// ----------------------------------------------------------------------------
// get_recast_navmesh_settings
// Read-back of the editor world's default ARecastNavMesh generation settings.
// Resolution-dependent values (cell_size/cell_height/agent_max_step_height) are
// read through the Default-resolution getters; tile_size is reported both as the
// resolved GetTileSizeUU() and the raw TileSizeUU UPROPERTY for transparency.
//
// HONEST gate: recast_navmesh_present=false (success still true) when no
// RecastNavMesh exists in the world.
// ----------------------------------------------------------------------------
// MCP:get_recast_navmesh_settings - Reads the generation settings of the editor world's default ARecastNavMesh.
//   @returns { recast_navmesh_present, navmesh_name, agent_radius, agent_height, agent_max_slope, agent_max_step_height, tile_size_uu, tile_size_uu_raw, cell_size, cell_height, merge_region_size, min_region_area, max_simplification_error, simplification_elevation_ratio, note, success }
TSharedPtr<FJsonObject> HandleGetRecastNavMeshSettings(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("World not available"))) { return NoWorld; }

    ARecastNavMesh* Recast = ResolveDefaultRecastNavMesh(World);
    if (!Recast)
    {
        UE_LOG(LogTemp, Verbose,
            TEXT("[MCP] get_recast_navmesh_settings: no RecastNavMesh in editor world; emitting honest gate."));
        TSharedPtr<FJsonObject> GateObj = MakeShared<FJsonObject>();
        GateObj->SetBoolField(TEXT("recast_navmesh_present"), false);
        GateObj->SetBoolField(TEXT("success"), true);
        GateObj->SetStringField(TEXT("note"),
            TEXT("No RecastNavMesh in editor world; add a NavMeshBoundsVolume and rebuild_navigation first."));
        return mcp_common::MakeOkReply(GateObj);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] get_recast_navmesh_settings: reading '%s'."), *Recast->GetName());

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("recast_navmesh_present"), true);
    ResultObj->SetStringField(TEXT("navmesh_name"), Recast->GetName());

    ResultObj->SetNumberField(TEXT("agent_radius"), Recast->AgentRadius);
    ResultObj->SetNumberField(TEXT("agent_height"), Recast->AgentHeight);
    ResultObj->SetNumberField(TEXT("agent_max_slope"), Recast->AgentMaxSlope);
    ResultObj->SetNumberField(TEXT("merge_region_size"), Recast->MergeRegionSize);
    ResultObj->SetNumberField(TEXT("min_region_area"), Recast->MinRegionArea);
    ResultObj->SetNumberField(TEXT("max_simplification_error"), Recast->MaxSimplificationError);
    ResultObj->SetNumberField(TEXT("simplification_elevation_ratio"), Recast->SimplificationElevationRatio);

    // Resolution-dependent values via the Default-resolution getters.
    ResultObj->SetNumberField(TEXT("cell_size"), Recast->GetCellSize(ENavigationDataResolution::Default));
    ResultObj->SetNumberField(TEXT("cell_height"), Recast->GetCellHeight(ENavigationDataResolution::Default));
    ResultObj->SetNumberField(TEXT("agent_max_step_height"), Recast->GetAgentMaxStepHeight(ENavigationDataResolution::Default));

    // Tile size: resolved value + the raw UPROPERTY.
    ResultObj->SetNumberField(TEXT("tile_size_uu"), Recast->GetTileSizeUU());
    ResultObj->SetNumberField(TEXT("tile_size_uu_raw"), Recast->TileSizeUU);

    ResultObj->SetBoolField(TEXT("success"), true);

    return mcp_common::MakeOkReply(ResultObj);
}

// ============================================================================
// EQS authoring: generators + tests + scoring
// ============================================================================

namespace
{
    /**
     * Resolve a UClass for a UEnvQueryGenerator (or Test) from a JSON name.
     * Accepts:
     *   - "/Script/AIModule.EnvQueryGenerator_ActorsOfClass" (full path)
     *   - "EnvQueryGenerator_ActorsOfClass" (bare class name)
     *   - "ActorsOfClass" / "SimpleGrid" / "PathingGrid" / "Distance" / "Trace" / "Pathfinding" / "Dot"
     *     (short alias -> prefix with "EnvQueryGenerator_" / "EnvQueryTest_")
     */
    UClass* ResolveEQSNodeClass(const FString& Input, const TCHAR* BasePrefix, FString& OutError)
    {
        if (Input.IsEmpty())
        {
            OutError = FString::Printf(TEXT("Missing class name (expected e.g. '%sActorsOfClass' or '/Script/AIModule.%sActorsOfClass')"), BasePrefix, BasePrefix);
            return nullptr;
        }

        TArray<FString> Tried;
        auto TryLoad = [&Tried](const FString& Path) -> UClass*
        {
            Tried.Add(Path);
            return LoadObject<UClass>(nullptr, *Path);
        };

        if (Input.StartsWith(TEXT("/")))
        {
            if (UClass* C = TryLoad(Input)) { return C; }
        }

        if (!Input.StartsWith(TEXT("/")))
        {
            const FString BareNoPrefix = Input;
            const FString WithPrefix = FString::Printf(TEXT("%s%s"), BasePrefix, *Input);

            if (UClass* C = TryLoad(FString::Printf(TEXT("/Script/AIModule.%s"), *WithPrefix))) { return C; }
            if (UClass* C = TryLoad(FString::Printf(TEXT("/Script/AIModule.%s"), *BareNoPrefix))) { return C; }
        }

        if (UClass* Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s%s"), BasePrefix, *Input)))
        {
            return Found;
        }
        if (UClass* Found = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/AIModule.%s"), *Input)))
        {
            return Found;
        }

        OutError = FString::Printf(TEXT("Failed to resolve class. Tried: %s"), *FString::Join(Tried, TEXT(", ")));
        return nullptr;
    }

    /** The UEnvQuery at QueryPath, in OutQuery; the error reply when none loads there. */
    TSharedPtr<FJsonObject> NavEqs_LoadQuery(const FString& QueryPath, UEnvQuery*& OutQuery)
    {
        OutQuery = Cast<UEnvQuery>(StaticLoadObject(UEnvQuery::StaticClass(), nullptr, *QueryPath));
        if (!OutQuery)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("UEnvQuery not found at '%s'"), *QueryPath));
        }
        return nullptr;
    }

    /** NavEqs_LoadQuery, then the error reply when that query has no option OptionIndex. */
    TSharedPtr<FJsonObject> NavEqs_LoadQueryOption(const FString& QueryPath, int32 OptionIndex, UEnvQuery*& OutQuery)
    {
        if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQuery(QueryPath, OutQuery))
        {
            return Failure;
        }
        if (!OutQuery->GetOptions().IsValidIndex(OptionIndex))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Option index %d out of range (query has %d options)"), OptionIndex, OutQuery->GetOptions().Num()));
        }
        return nullptr;
    }

    TSharedPtr<FJsonObject> NavEqs_OptionReply(const FString& QueryPath, int32 OptionIndex)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("query_path"), QueryPath);
        Reply->SetNumberField(TEXT("option_index"), OptionIndex);
        return Reply;
    }

    /**
     * Into Out, the value of the EQS test enum TEnum that the param ParamName names in Given: one of AcceptedNames,
     * the enum's own names, matched without case; Default when Given is empty. False, with OutError listing
     * AcceptedNames, for any other name.
     */
    template <typename TEnum>
    bool NavEqs_ParseEnumParam(const TCHAR* ParamName, const FString& Given, TArrayView<const TCHAR* const> AcceptedNames,
                               TEnum Default, TEnum& Out, FString& OutError)
    {
        Out = Default;
        if (Given.IsEmpty())
        {
            return true;
        }
        for (const TCHAR* Name : AcceptedNames)
        {
            if (Given.Equals(Name, ESearchCase::IgnoreCase))
            {
                const int64 Value = StaticEnum<TEnum>()->GetValueByNameString(Name);
                if (Value != INDEX_NONE)
                {
                    Out = static_cast<TEnum>(Value);
                    return true;
                }
            }
        }
        OutError = FString::Printf(TEXT("%s '%s' is not one of: %s. Nothing was changed."),
            ParamName, *Given, *FString::Join(AcceptedNames, TEXT(", ")));
        return false;
    }

    /**
     * The optional "option_index" param of a command that may create the option, into OutOptionIndex (0 when
     * absent); null on success, else the error reply. It is read as a double so a number past int32 is refused
     * instead of failing the int32 read and silently naming option 0.
     */
    TSharedPtr<FJsonObject> NavEqs_ReadNewOptionIndex(const TSharedPtr<FJsonObject>& Params, double& OutOptionIndex)
    {
        OutOptionIndex = 0.0;
        Params->TryGetNumberField(TEXT("option_index"), OutOptionIndex);
        if (OutOptionIndex < 0.0)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("option_index must be 0 or greater (got %g); nothing was changed"), OutOptionIndex));
        }
        return nullptr;
    }

    /**
     * The error reply when OptionIndex is past the end of Query's options; null when it names an existing option or
     * the next new one. NavEqs_EnsureOption appends an empty option for every missing index up to it, so a far index
     * would append that many (a huge one hangs the editor or runs it out of memory).
     */
    TSharedPtr<FJsonObject> NavEqs_RefuseOptionPastEnd(const UEnvQuery* Query, double OptionIndex)
    {
        const int32 NumOptions = Query->GetOptions().Num();
        if (OptionIndex > NumOptions)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("option_index %g is past the end: the query has %d options, so it can name one of them or be %d to append a new one; nothing was changed"),
                OptionIndex, NumOptions, NumOptions));
        }
        return nullptr;
    }

    /**
     * The option at OptionIndex, appending empty options to Query until it exists; null for a negative OptionIndex.
     * Callers refuse an index past the option count first (NavEqs_RefuseOptionPastEnd), so at most one is appended.
     */
    UEnvQueryOption* NavEqs_EnsureOption(UEnvQuery* Query, int32 OptionIndex)
    {
        if (OptionIndex < 0)
        {
            return nullptr;
        }
        while (Query->GetOptions().Num() <= OptionIndex)
        {
            // RF_Transactional, as the EQS editor creates options, so a later Modify() on it records undo.
            UEnvQueryOption* NewOpt = NewObject<UEnvQueryOption>(Query, NAME_None, RF_Transactional);
            Query->GetOptionsMutable().Add(NewOpt);
        }
        return Query->GetOptions()[OptionIndex];
    }
}

// MCP:add_eqs_generator - Adds a UEnvQueryGenerator subclass to a UEnvQuery option. If the option has no generator yet, this becomes it; otherwise replaces. option_index names an existing option, or equals the option count to append one new empty option; a negative option_index, or one past the option count, is an error checked before anything changes.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param generator_class - Generator class name (e.g. "ActorsOfClass", "SimpleGrid", "PathingGrid") or full path (e.g. "/Script/AIModule.EnvQueryGenerator_ActorsOfClass")
//   @param option_index - Optional option index to target (default 0); the current option count appends a new option, a larger index is refused
//   @returns { query_path, option_index, generator_class, generator_name, replaced, saved }
TSharedPtr<FJsonObject> HandleAddEQSGenerator(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    FString GenClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("generator_class"), GenClassName)) { return Missing; }

    double OptionIndexD = 0.0;
    if (TSharedPtr<FJsonObject> Refused = NavEqs_ReadNewOptionIndex(Params, OptionIndexD))
    {
        return Refused;
    }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQuery(QueryPath, Query))
    {
        return Failure;
    }
    if (TSharedPtr<FJsonObject> Refused = NavEqs_RefuseOptionPastEnd(Query, OptionIndexD))
    {
        return Refused;
    }
    const int32 OptionIndex = static_cast<int32>(OptionIndexD);

    FString ClassErr;
    UClass* GenClass = ResolveEQSNodeClass(GenClassName, TEXT("EnvQueryGenerator_"), ClassErr);
    if (!GenClass)
        return mcp_common::MakeErrorReply(ClassErr);
    if (GenClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class '%s' is abstract/deprecated"), *GenClass->GetName()));

    // Same FScopedTransaction pattern the sibling HandleSpawnNavModifierVolume/
    // HandleSpawnNavLinkProxy already use in this file: open before mutating,
    // Modify() each object before it's actually mutated.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddEQSGenerator", "Add EQS Generator (MCP)"));
    Query->Modify();

    UEnvQueryOption* Option = NavEqs_EnsureOption(Query, OptionIndex);
    if (!Option) { return mcp_common::MakeErrorReply(TEXT("Failed to resolve option")); }

    bool bReplaced = Option->Generator != nullptr;

    UEnvQueryGenerator* Gen = NewObject<UEnvQueryGenerator>(Option, GenClass, NAME_None, RF_Transactional);
    if (!Gen)
        return mcp_common::MakeErrorReply(TEXT("Failed to instantiate UEnvQueryGenerator subclass"));

    Option->Modify();
    Option->Generator = Gen;
    Query->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetStringField(TEXT("generator_class"), GenClass->GetName());
    Result->SetStringField(TEXT("generator_path"), GenClass->GetPathName());
    Result->SetStringField(TEXT("generator_name"), Gen->GetName());
    Result->SetBoolField(TEXT("replaced"), bReplaced);
    Result->SetBoolField(TEXT("saved"), false);
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_eqs_test - Add a test of test_class to an option of a UEnvQuery asset, after the option's existing tests. Idempotent per class: if the option already has a test of that class, that one is returned (already_existed=true) and nothing is added, so an option holds at most one test per class. option_index names an existing option, or equals the option count to append one new empty option; a negative option_index, or one past the option count, is an error checked before anything changes. Undoable as one editor transaction; marks the asset dirty without saving (save_asset). Valid classes: list_eqs_test_types; configure the test with set_eqs_test_scoring; remove_eqs_test is the inverse.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param test_class - Test class name (e.g. "Distance", "Trace", "Pathfinding", "Dot") or full path
//   @param option_index - Optional option index to target (default 0); the current option count appends a new option, a larger index is refused
//   @returns { query_path, option_index, test_class, test_name, test_index, test_count, already_existed } — idempotent by test class within the option
TSharedPtr<FJsonObject> HandleAddEQSTest(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    FString TestClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("test_class"), TestClassName)) { return Missing; }

    double OptionIndexD = 0.0;
    if (TSharedPtr<FJsonObject> Refused = NavEqs_ReadNewOptionIndex(Params, OptionIndexD))
    {
        return Refused;
    }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQuery(QueryPath, Query))
    {
        return Failure;
    }
    if (TSharedPtr<FJsonObject> Refused = NavEqs_RefuseOptionPastEnd(Query, OptionIndexD))
    {
        return Refused;
    }
    const int32 OptionIndex = static_cast<int32>(OptionIndexD);

    FString ClassErr;
    UClass* TestClass = ResolveEQSNodeClass(TestClassName, TEXT("EnvQueryTest_"), ClassErr);
    if (!TestClass)
        return mcp_common::MakeErrorReply(ClassErr);
    if (TestClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class '%s' is abstract/deprecated"), *TestClass->GetName()));

    // Idempotent: a test of this class already in an existing option -> return it, no dup and no edit.
    if (Query->GetOptions().IsValidIndex(OptionIndex))
    {
        if (const UEnvQueryOption* ExistingOption = Query->GetOptions()[OptionIndex])
        {
            for (int32 i = 0; i < ExistingOption->Tests.Num(); ++i)
            {
                const UEnvQueryTest* Existing = ExistingOption->Tests[i];
                if (Existing && Existing->GetClass() == TestClass)
                {
                    TSharedPtr<FJsonObject> Dup = NavEqs_OptionReply(QueryPath, OptionIndex);
                    Dup->SetStringField(TEXT("test_class"), TestClass->GetName());
                    Dup->SetStringField(TEXT("test_path"), TestClass->GetPathName());
                    Dup->SetStringField(TEXT("test_name"), Existing->GetName());
                    Dup->SetNumberField(TEXT("test_index"), i);
                    Dup->SetNumberField(TEXT("test_count"), ExistingOption->Tests.Num());
                    Dup->SetBoolField(TEXT("already_existed"), true);
                    return mcp_common::MakeOkReply(Dup);
                }
            }
        }
    }

    // Same FScopedTransaction pattern as add_eqs_generator: open before mutating and Modify() each object
    // before it is mutated -- the query before NavEqs_EnsureOption may append an option, the option before
    // its Tests array grows -- so one undo removes the test and any option this call created.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddEQSTest", "Add EQS Test (MCP)"));
    Query->Modify();

    UEnvQueryOption* Option = NavEqs_EnsureOption(Query, OptionIndex);
    if (!Option) { return mcp_common::MakeErrorReply(TEXT("Failed to resolve option")); }

    UEnvQueryTest* Test = NewObject<UEnvQueryTest>(Option, TestClass, NAME_None, RF_Transactional);
    if (!Test)
        return mcp_common::MakeErrorReply(TEXT("Failed to instantiate UEnvQueryTest subclass"));

    // An option saved without RF_Transactional would be skipped by Modify(); the EQS editor sets it too.
    Option->SetFlags(RF_Transactional);
    Option->Modify();
    Option->Tests.Add(Test);
    Query->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetStringField(TEXT("test_class"), TestClass->GetName());
    Result->SetStringField(TEXT("test_path"), TestClass->GetPathName());
    Result->SetStringField(TEXT("test_name"), Test->GetName());
    Result->SetNumberField(TEXT("test_index"), Option->Tests.Num() - 1);
    Result->SetNumberField(TEXT("test_count"), Option->Tests.Num());
    Result->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_eqs_test_scoring - Set the scoring and filter settings of test test_index in option option_index of a UEnvQuery asset, in one undo step: TestPurpose, FilterType, ScoringEquation, the FloatValueMin and FloatValueMax bounds and, only when given, ScoringFactor. The first five are written on every call, so one left out goes back to its default (Score, Range, Linear, 0, 1); the reply reads the stored values back. Values equal to the stored ones are a no-op: changed=false, a note, nothing written. A changed asset is marked dirty, not saved: persist it with save_asset. An enum name the param does not list (the error names the accepted ones), a query that does not load or an index out of range is an error, and nothing changes. Add the test with add_eqs_test; read the indices with get_eqs_query_info; generator settings: set_eqs_generator_properties.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param option_index - Option index (default 0)
//   @param test_index - Test index within the option (default 0)
//   @param test_purpose - "Filter" | "Score" | "FilterAndScore" (default "Score")
//   @param filter_type - "Minimum" | "Maximum" | "Range" | "Match" (default "Range")
//   @param scoring_equation - "Linear" | "Square" | "InverseLinear" | "SquareRoot" | "Constant" (default "Linear")
//   @param scoring_factor - Optional weight multiplier applied after the equation (ScoringFactor.DefaultValue); only applied when provided
//   @param float_value_min - Optional min for FloatValueMin (default 0)
//   @param float_value_max - Optional max for FloatValueMax (default 1)
//   @returns { query_path, option_index, test_index, test_name, changed, test_purpose, filter_type, scoring_equation, scoring_factor, float_value_min, float_value_max, note? }
TSharedPtr<FJsonObject> HandleSetEQSTestScoring(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    int32 OptionIndex = 0;
    int32 TestIndex = 0;
    Params->TryGetNumberField(TEXT("option_index"), OptionIndex);
    Params->TryGetNumberField(TEXT("test_index"), TestIndex);

    FString PurposeStr, FilterStr, EqStr;
    Params->TryGetStringField(TEXT("test_purpose"), PurposeStr);
    Params->TryGetStringField(TEXT("filter_type"), FilterStr);
    Params->TryGetStringField(TEXT("scoring_equation"), EqStr);

    // The three enum names are checked before anything loads or changes; one left out takes its default.
    static const TCHAR* const PurposeNames[] = { TEXT("Filter"), TEXT("Score"), TEXT("FilterAndScore") };
    static const TCHAR* const FilterNames[] = { TEXT("Minimum"), TEXT("Maximum"), TEXT("Range"), TEXT("Match") };
    static const TCHAR* const EquationNames[] =
        { TEXT("Linear"), TEXT("Square"), TEXT("InverseLinear"), TEXT("SquareRoot"), TEXT("Constant") };
    EEnvTestPurpose::Type Purpose = EEnvTestPurpose::Score;
    EEnvTestFilterType::Type FilterType = EEnvTestFilterType::Range;
    EEnvTestScoreEquation::Type Equation = EEnvTestScoreEquation::Linear;
    FString EnumError;
    if (!NavEqs_ParseEnumParam(TEXT("test_purpose"), PurposeStr, MakeArrayView(PurposeNames), EEnvTestPurpose::Score, Purpose, EnumError)
        || !NavEqs_ParseEnumParam(TEXT("filter_type"), FilterStr, MakeArrayView(FilterNames), EEnvTestFilterType::Range, FilterType, EnumError)
        || !NavEqs_ParseEnumParam(TEXT("scoring_equation"), EqStr, MakeArrayView(EquationNames), EEnvTestScoreEquation::Linear, Equation, EnumError))
    {
        return mcp_common::MakeErrorReply(EnumError);
    }

    double FloatMin = 0.0, FloatMax = 1.0;
    Params->TryGetNumberField(TEXT("float_value_min"), FloatMin);
    Params->TryGetNumberField(TEXT("float_value_max"), FloatMax);

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQueryOption(QueryPath, OptionIndex, Query))
    {
        return Failure;
    }

    UEnvQueryOption* Option = Query->GetOptions()[OptionIndex];
    if (!Option->Tests.IsValidIndex(TestIndex))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Test index %d out of range (option has %d tests)"), TestIndex, Option->Tests.Num()));

    UEnvQueryTest* Test = Option->Tests[TestIndex];

    double ScoringFactor = 0.0;
    const bool bHasScoringFactor = Params->TryGetNumberField(TEXT("scoring_factor"), ScoringFactor);

    // Values that all equal the stored ones change nothing: nothing is Modify()'d, written or dirtied, so the call
    // leaves no undo step.
    const bool bChanged = Test->TestPurpose.GetValue() != Purpose
        || Test->FilterType.GetValue() != FilterType
        || Test->ScoringEquation.GetValue() != Equation
        || Test->FloatValueMin.DefaultValue != static_cast<float>(FloatMin)
        || Test->FloatValueMax.DefaultValue != static_cast<float>(FloatMax)
        || (bHasScoringFactor && Test->ScoringFactor.DefaultValue != static_cast<float>(ScoringFactor));
    if (bChanged)
    {
        // Modify() before the writes, so undo restores the previous scoring. A test saved without RF_Transactional
        // would be skipped by Modify(); add_eqs_test and the EQS editor create tests with it.
        Test->SetFlags(RF_Transactional);
        Test->Modify();
        Option->Modify();
        Query->Modify();

        Test->TestPurpose = Purpose;
        // FilterType + ScoringEquation are real UEnvQueryTest UPROPERTYs (TEnumAsByte) that
        // serialize on the asset, so they are persisted.
        Test->FilterType = FilterType;
        Test->ScoringEquation = Equation;
        // FloatValue*/ScoringFactor are FAIDataProviderFloatValue structs (DefaultValue field).
        Test->FloatValueMin.DefaultValue = static_cast<float>(FloatMin);
        Test->FloatValueMax.DefaultValue = static_cast<float>(FloatMax);
        if (bHasScoringFactor)
        {
            Test->ScoringFactor.DefaultValue = static_cast<float>(ScoringFactor);
        }

        Query->MarkPackageDirty();
    }

    // Read BACK the real persisted values (not the input echo) for honest verification.
    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetNumberField(TEXT("test_index"), TestIndex);
    Result->SetStringField(TEXT("test_name"), Test->GetName());
    Result->SetBoolField(TEXT("changed"), bChanged);
    if (!bChanged)
    {
        Result->SetStringField(TEXT("note"),
            TEXT("The test already holds these settings; nothing was written, dirtied or recorded for undo."));
    }
    Result->SetStringField(TEXT("test_purpose"),
        StaticEnum<EEnvTestPurpose::Type>()->GetNameStringByValue(static_cast<int64>(Test->TestPurpose)));
    Result->SetStringField(TEXT("filter_type"),
        StaticEnum<EEnvTestFilterType::Type>()->GetNameStringByValue(static_cast<int64>(Test->FilterType)));
    Result->SetStringField(TEXT("scoring_equation"),
        StaticEnum<EEnvTestScoreEquation::Type>()->GetNameStringByValue(static_cast<int64>(Test->ScoringEquation)));
    Result->SetNumberField(TEXT("scoring_factor"), Test->ScoringFactor.DefaultValue);
    Result->SetNumberField(TEXT("float_value_min"), Test->FloatValueMin.DefaultValue);
    Result->SetNumberField(TEXT("float_value_max"), Test->FloatValueMax.DefaultValue);
    return mcp_common::MakeOkReply(Result);
}

// ============================================================================
// EQS execution + discovery (run the query, introspect it, list node types)
// ============================================================================

namespace
{
    /** Build a { base_class, count, types:[{name, path}] } payload of the concrete
     *  (non-abstract, non-deprecated) subclasses of BaseClass. Deterministic order. */
    TSharedPtr<FJsonObject> BuildEQSDerivedClassList(UClass* BaseClass, const TCHAR* Label)
    {
        const TArray<TSharedPtr<FJsonValue>> Types = mcp_common::ConcreteSubclassesToJson(BaseClass);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("base_class"), Label);
        Result->SetNumberField(TEXT("count"), Types.Num());
        Result->SetArrayField(TEXT("types"), Types);
        return Result;
    }
}

// MCP:run_eqs_query - Runs a UEnvQuery synchronously against the editor world (UEnvQueryManager::RunInstantQuery) and returns the scored items. Generators/tests needing navigation data (SimpleGrid/PathingGrid/Pathfinding/Project/Trace) require a built navmesh; without it the query returns a non-Success status honestly (still success:true, ran:true). An unknown run_mode is an error and the query does not run.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param querier_actor - Optional actor display label or name used as the query Querier (drives contexts like EnvQueryContext_Querier); default: the query asset (no location)
//   @param run_mode - "SingleResult" | "RandomBest5Pct" | "RandomBest25Pct" | "AllMatching" (default "AllMatching")
//   @param max_items - Optional cap on how many items are serialized into the payload (default 50; the query still evaluates all)
//   @returns { query_path, ran, eqs_manager_present, status, is_successful, num_items, item_type, run_mode, items:[{index, score, location:{x,y,z}, actor?}], note? }
TSharedPtr<FJsonObject> HandleRunEQSQuery(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("World not available"))) { return NoWorld; }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQuery(QueryPath, Query))
    {
        return Failure;
    }

    UEnvQueryManager* Manager = UEnvQueryManager::GetCurrent(World);
    if (!Manager)
    {
        // Honest gate: no EQS manager (no UAISystem) in this world — cannot run.
        TSharedPtr<FJsonObject> Gate = MakeShared<FJsonObject>();
        Gate->SetStringField(TEXT("query_path"), QueryPath);
        Gate->SetBoolField(TEXT("ran"), false);
        Gate->SetBoolField(TEXT("eqs_manager_present"), false);
        Gate->SetStringField(TEXT("note"), TEXT("No EQS manager (UAISystem) available in the editor world; cannot run the query."));
        return mcp_common::MakeOkReply(Gate);
    }

    // Optional querier actor -> the request Owner (drives EnvQueryContext_Querier and Querier-relative generators).
    UObject* Owner = Query;
    FString QuerierName;
    if (Params->TryGetStringField(TEXT("querier_actor"), QuerierName) && !QuerierName.IsEmpty())
    {
        AActor* Querier = mcp_common::FindActorByName(World, QuerierName);
        if (!Querier)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("querier_actor '%s' not found in the editor world"), *QuerierName));
        Owner = Querier;
    }

    FString RunModeStr;
    Params->TryGetStringField(TEXT("run_mode"), RunModeStr);
    // An unknown run_mode is refused rather than run as AllMatching.
    static const TCHAR* const RunModeNames[] = { TEXT("SingleResult"), TEXT("RandomBest5Pct"), TEXT("RandomBest25Pct"), TEXT("AllMatching") };
    EEnvQueryRunMode::Type RunMode = EEnvQueryRunMode::AllMatching;
    FString RunModeError;
    if (!NavEqs_ParseEnumParam(TEXT("run_mode"), RunModeStr, MakeArrayView(RunModeNames), EEnvQueryRunMode::AllMatching, RunMode, RunModeError))
    {
        return mcp_common::MakeErrorReply(RunModeError);
    }

    int32 MaxItems = 50;
    Params->TryGetNumberField(TEXT("max_items"), MaxItems);
    MaxItems = FMath::Max(0, MaxItems);

    FEnvQueryRequest Request(Query, Owner);
    Request.SetWorldOverride(World);
    TSharedPtr<FEnvQueryResult> Res = Manager->RunInstantQuery(Request, RunMode);
    if (!Res.IsValid())
    {
        TSharedPtr<FJsonObject> Gate = MakeShared<FJsonObject>();
        Gate->SetStringField(TEXT("query_path"), QueryPath);
        Gate->SetBoolField(TEXT("ran"), false);
        Gate->SetBoolField(TEXT("eqs_manager_present"), true);
        Gate->SetStringField(TEXT("note"), TEXT("RunInstantQuery returned no result (the query likely has no valid option/generator)."));
        return mcp_common::MakeOkReply(Gate);
    }

    const EEnvQueryStatus::Type Status = Res->GetRawStatus();
    const bool bActorItems = Res->ItemType && Res->ItemType->IsChildOf(UEnvQueryItemType_ActorBase::StaticClass());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("query_path"), QueryPath);
    Result->SetBoolField(TEXT("ran"), true);
    Result->SetBoolField(TEXT("eqs_manager_present"), true);
    Result->SetStringField(TEXT("status"), StaticEnum<EEnvQueryStatus::Type>()->GetNameStringByValue(static_cast<int64>(Status)));
    Result->SetBoolField(TEXT("is_successful"), Res->IsSuccessful());
    Result->SetNumberField(TEXT("num_items"), Res->Items.Num());
    Result->SetStringField(TEXT("item_type"), Res->ItemType ? Res->ItemType->GetName() : TEXT("None"));
    Result->SetStringField(TEXT("run_mode"), StaticEnum<EEnvQueryRunMode::Type>()->GetNameStringByValue(static_cast<int64>(RunMode)));

    const int32 Count = FMath::Min(Res->Items.Num(), MaxItems);
    TArray<TSharedPtr<FJsonValue>> ItemsArr;
    for (int32 i = 0; i < Count; ++i)
    {
        TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetNumberField(TEXT("index"), i);
        Item->SetNumberField(TEXT("score"), Res->GetItemScore(i));
        Item->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Res->GetItemAsLocation(i)));

        if (bActorItems)
        {
            if (AActor* A = Res->GetItemAsActor(i))
                Item->SetStringField(TEXT("actor"), A->GetActorNameOrLabel());
        }
        ItemsArr.Add(MakeShared<FJsonValueObject>(Item));
    }
    Result->SetArrayField(TEXT("items"), ItemsArr);

    if (Status != EEnvQueryStatus::Success)
        Result->SetStringField(TEXT("note"), TEXT("Query did not return Success. If its generator/test needs navigation data (SimpleGrid/PathingGrid/Pathfinding/Project/Trace), build a navmesh first (rebuild_navigation), or provide a valid querier_actor for Querier-relative contexts."));

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_eqs_query_info - Reports the composition of a UEnvQuery asset: each option's generator class and its list of test classes. Read-only.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @returns { query_path, option_count, options:[{index, generator_class?, generator_path?, test_count, tests:[{index, class, path}]}] }
TSharedPtr<FJsonObject> HandleGetEQSQueryInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQuery(QueryPath, Query))
    {
        return Failure;
    }

    const TArray<UEnvQueryOption*>& Opts = Query->GetOptions();
    TArray<TSharedPtr<FJsonValue>> Options;
    for (int32 oi = 0; oi < Opts.Num(); ++oi)
    {
        UEnvQueryOption* Opt = Opts[oi];
        TSharedPtr<FJsonObject> OptObj = MakeShared<FJsonObject>();
        OptObj->SetNumberField(TEXT("index"), oi);

        if (Opt && Opt->Generator)
        {
            OptObj->SetStringField(TEXT("generator_class"), Opt->Generator->GetClass()->GetName());
            OptObj->SetStringField(TEXT("generator_path"), Opt->Generator->GetClass()->GetPathName());
        }

        TArray<TSharedPtr<FJsonValue>> Tests;
        if (Opt)
        {
            for (int32 ti = 0; ti < Opt->Tests.Num(); ++ti)
            {
                UEnvQueryTest* T = Opt->Tests[ti];
                if (!T) { continue; }
                TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
                TObj->SetNumberField(TEXT("index"), ti);
                TObj->SetStringField(TEXT("class"), T->GetClass()->GetName());
                TObj->SetStringField(TEXT("path"), T->GetClass()->GetPathName());
                Tests.Add(MakeShared<FJsonValueObject>(TObj));
            }
        }
        OptObj->SetNumberField(TEXT("test_count"), Tests.Num());
        OptObj->SetArrayField(TEXT("tests"), Tests);
        Options.Add(MakeShared<FJsonValueObject>(OptObj));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("query_path"), QueryPath);
    Result->SetNumberField(TEXT("option_count"), Opts.Num());
    Result->SetArrayField(TEXT("options"), Options);
    return mcp_common::MakeOkReply(Result);
}

// MCP:list_eqs_generator_types - Lists all concrete UEnvQueryGenerator subclasses usable as EQS generators. Read-only reflection (GetDerivedClasses).
//   @returns { base_class, count, types:[{name, path}] }
TSharedPtr<FJsonObject> HandleListEQSGeneratorTypes(const TSharedPtr<FJsonObject>& Params)
{
    return mcp_common::MakeOkReply(BuildEQSDerivedClassList(UEnvQueryGenerator::StaticClass(), TEXT("EnvQueryGenerator")));
}

// MCP:list_eqs_test_types - Lists all concrete UEnvQueryTest subclasses usable as EQS tests. Read-only reflection (GetDerivedClasses).
//   @returns { base_class, count, types:[{name, path}] }
TSharedPtr<FJsonObject> HandleListEQSTestTypes(const TSharedPtr<FJsonObject>& Params)
{
    return mcp_common::MakeOkReply(BuildEQSDerivedClassList(UEnvQueryTest::StaticClass(), TEXT("EnvQueryTest")));
}

// MCP:list_eqs_context_types - Lists all concrete UEnvQueryContext subclasses usable as EQS contexts. Read-only reflection (GetDerivedClasses).
//   @returns { base_class, count, types:[{name, path}] }
TSharedPtr<FJsonObject> HandleListEQSContextTypes(const TSharedPtr<FJsonObject>& Params)
{
    return mcp_common::MakeOkReply(BuildEQSDerivedClassList(UEnvQueryContext::StaticClass(), TEXT("EnvQueryContext")));
}

// ============================================================================
// EQS authoring: generator properties, remove/add option/test/generator
// ============================================================================

// MCP:set_eqs_generator_properties - Set properties of the generator of one option (option_index) of a UEnvQuery asset by reflection, in one undo step; only the named properties change. Use it over set_properties, which needs the generator's object path: here a class property (SearchedActorClass, SearchCenter) takes a class path such as /Script/Engine.Pawn, checked against its base class, and an FAIDataProvider value (SearchRadius, GenerateOnlyActorsInRadius) takes {"DefaultValue": number or bool}; others take their JSON value. A property that is unknown or rejects the value lands in failed without failing the call; applied echoes each stored value. A query that does not load, an option_index out of range or an option with no generator (add_eqs_generator) is an error. Marked dirty, not saved (save_asset). Scoring: set_eqs_test_scoring; classes: list_eqs_generator_types; composition: get_eqs_query_info.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param option_index - Zero-based option index whose generator is edited (default 0); out of range is an error
//   @param properties - JSON object { PropertyName: value, ... } applied to the generator, keys as named on its class (value formats in the description)
//   @returns { query_path, option_index, generator_class, set:[name], failed:[{name, error}], applied:{name: stored_value_text} }
TSharedPtr<FJsonObject> HandleSetEQSGeneratorProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    int32 OptionIndex = 0;
    Params->TryGetNumberField(TEXT("option_index"), OptionIndex);

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
        return mcp_common::MakeErrorReply(TEXT("Missing 'properties' object"));

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQueryOption(QueryPath, OptionIndex, Query))
    {
        return Failure;
    }

    UEnvQueryOption* Option = Query->GetOptions()[OptionIndex];
    if (!Option || !Option->Generator)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Option %d has no generator (call add_eqs_generator first)"), OptionIndex));

    UEnvQueryGenerator* Gen = Option->Generator;

    // Same FScopedTransaction pattern as add_eqs_generator above: open before
    // mutating, Modify() before the mutation loop.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetEQSGeneratorProperties", "Set EQS Generator Properties (MCP)"));
    Gen->Modify();
    Option->Modify();
    Query->Modify();

    TArray<TSharedPtr<FJsonValue>> SetOk, Failed;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsObj)->Values)
    {
        FProperty* Prop = Gen->GetClass()->FindPropertyByName(*Pair.Key);
        FClassProperty* ClassProp = Prop ? CastField<FClassProperty>(Prop) : nullptr;
        FStructProperty* StructProp = Prop ? CastField<FStructProperty>(Prop) : nullptr;
        const bool bAIProvider = StructProp && StructProp->Struct
            && StructProp->Struct->GetName().StartsWith(TEXT("AIDataProvider"));
        FString Err;
        bool bOk = false;

        if (!Prop)
        {
            Err = FString::Printf(TEXT("Property not found: %s"), *Pair.Key);
        }
        else if (ClassProp)
        {
            // TSubclassOf<...> class-ref (SearchedActorClass, SearchCenter): resolve the
            // class from a path/name string directly — JsonValueToUProperty nulls it silently.
            const FString ClassStr = Pair.Value.IsValid() ? Pair.Value->AsString() : FString();
            UClass* Resolved = ClassStr.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ClassStr);
            if (!Resolved && !ClassStr.IsEmpty()) { Resolved = FindObject<UClass>(nullptr, *ClassStr); }
            if (Resolved && (!ClassProp->MetaClass || Resolved->IsChildOf(ClassProp->MetaClass)))
            {
                ClassProp->SetObjectPropertyValue_InContainer(Gen, Resolved);
                bOk = true;
            }
            else
            {
                Err = FString::Printf(TEXT("could not resolve class '%s' as a %s"), *ClassStr,
                    ClassProp->MetaClass ? *ClassProp->MetaClass->GetName() : TEXT("UClass"));
            }
        }
        else if (bAIProvider && Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
        {
            // FAIDataProvider*Value ({DataBinding, DataField, DefaultValue}): set the inner
            // DefaultValue scalar from {"DefaultValue": <n|bool>} — the bare struct does not
            // round-trip through JsonValueToUProperty.
            const TSharedPtr<FJsonObject> Obj = Pair.Value->AsObject();
            const TSharedPtr<FJsonValue> DV = Obj.IsValid() ? Obj->TryGetField(TEXT("DefaultValue")) : nullptr;
            FProperty* Inner = StructProp->Struct->FindPropertyByName(TEXT("DefaultValue"));
            void* StructAddr = StructProp->ContainerPtrToValuePtr<void>(Gen);
            if (DV.IsValid() && Inner)
            {
                void* InnerAddr = Inner->ContainerPtrToValuePtr<void>(StructAddr);
                if (FBoolProperty* BP = CastField<FBoolProperty>(Inner))
                {
                    BP->SetPropertyValue(InnerAddr, DV->AsBool());
                    bOk = true;
                }
                else if (FNumericProperty* NP = CastField<FNumericProperty>(Inner))
                {
                    if (NP->IsFloatingPoint()) { NP->SetFloatingPointPropertyValue(InnerAddr, DV->AsNumber()); }
                    else { NP->SetIntPropertyValue(InnerAddr, static_cast<int64>(DV->AsNumber())); }
                    bOk = true;
                }
                else { Err = TEXT("unsupported AIDataProvider DefaultValue type"); }
            }
            else { Err = TEXT("AIDataProvider value needs a numeric/bool DefaultValue field"); }
        }
        else
        {
            bOk = mcp_common::SetObjectProperty(Gen, Pair.Key, Pair.Value, Err);
        }

        if (bOk)
        {
            SetOk.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
        else
        {
            TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
            F->SetStringField(TEXT("name"), Pair.Key);
            F->SetStringField(TEXT("error"), Err);
            Failed.Add(MakeShared<FJsonValueObject>(F));
        }
    }

    Query->MarkPackageDirty();

    // Read BACK each attempted property's stored value (honest verification: proves the
    // mutation actually landed, not just that the setter returned true).
    TSharedPtr<FJsonObject> AppliedObj = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsObj)->Values)
    {
        if (FProperty* P = Gen->GetClass()->FindPropertyByName(*Pair.Key))
        {
            FString ValStr;
            P->ExportTextItem_Direct(ValStr, P->ContainerPtrToValuePtr<void>(Gen), nullptr, Gen, PPF_None);
            AppliedObj->SetStringField(Pair.Key, ValStr);
        }
    }

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetStringField(TEXT("generator_class"), Gen->GetClass()->GetName());
    Result->SetArrayField(TEXT("set"), SetOk);
    Result->SetArrayField(TEXT("failed"), Failed);
    Result->SetObjectField(TEXT("applied"), AppliedObj);
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_eqs_generator - Clears the generator on a query option. Idempotent (removed=false if the option had none).
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param option_index - Option index (default 0)
//   @returns { query_path, option_index, removed }
TSharedPtr<FJsonObject> HandleRemoveEQSGenerator(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    int32 OptionIndex = 0;
    Params->TryGetNumberField(TEXT("option_index"), OptionIndex);

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQueryOption(QueryPath, OptionIndex, Query))
    {
        return Failure;
    }

    UEnvQueryOption* Option = Query->GetOptions()[OptionIndex];
    const bool bRemoved = Option && Option->Generator != nullptr;
    if (bRemoved)
    {
        // Modify() before the write, so undo puts the generator back (RF_Transactional as in remove_eqs_test).
        Option->SetFlags(RF_Transactional);
        Option->Modify();
        Query->Modify();
        Option->Generator = nullptr;
        Query->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetBoolField(TEXT("removed"), bRemoved);
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_eqs_test - Remove the test at test_index from the option at option_index (default 0) of a UEnvQuery asset loaded by asset path; the tests after it shift down one index, and test_count is what the option keeps. One undo step puts the test back. The asset is marked dirty, not saved: persist it with save_asset. A query that does not load, or an option or test index out of range, is an error and nothing changes. Not idempotent: repeating the call removes the test that moved into that index, so read the indices with get_eqs_query_info first. Inverse: add_eqs_test; retune a test instead: set_eqs_test_scoring; a whole option: remove_eqs_option; the generator: remove_eqs_generator.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param option_index - 0-based index of the query option that holds the test (default 0).
//   @param test_index - Index of the test to remove within the option (required)
//   @returns { query_path, option_index, test_index, removed, test_count }
TSharedPtr<FJsonObject> HandleRemoveEQSTest(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    int32 OptionIndex = 0;
    Params->TryGetNumberField(TEXT("option_index"), OptionIndex);
    int32 TestIndex = -1;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("test_index"), TestIndex)) { return Missing; }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQueryOption(QueryPath, OptionIndex, Query))
    {
        return Failure;
    }

    UEnvQueryOption* Option = Query->GetOptions()[OptionIndex];
    if (!Option || !Option->Tests.IsValidIndex(TestIndex))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Test index %d out of range (option has %d tests)"), TestIndex, Option ? Option->Tests.Num() : 0));

    // Modify() before the removal, so undo puts the test back. An option saved without RF_Transactional would be
    // skipped by Modify(); the EQS editor sets it too.
    Option->SetFlags(RF_Transactional);
    Option->Modify();
    Query->Modify();
    Option->Tests.RemoveAt(TestIndex);
    Query->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetNumberField(TEXT("test_index"), TestIndex);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("test_count"), Option->Tests.Num());
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_eqs_option - Append a new empty option (no generator, no tests) after the existing options of the UEnvQuery asset at query_path and return its option_index, always the last one. Each call adds another option (not idempotent). The asset is only marked dirty, not saved (save_asset), and the append is one undo step; a query that does not load is an error. An option without a generator is skipped, with an error log, when the query runs: give it one with add_eqs_generator (which can also append the option itself when option_index equals the option count), then add tests with add_eqs_test. Inspect with get_eqs_query_info; inverse: remove_eqs_option; a new query: create_eqs_query.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @returns { query_path, option_index, option_count }
TSharedPtr<FJsonObject> HandleAddEQSOption(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQuery(QueryPath, Query))
    {
        return Failure;
    }

    // Modify() before the append records the options as they were, so undo removes the new one; NavEqs_EnsureOption
    // creates it RF_Transactional, as the EQS editor does, so later edits of it are undoable too.
    Query->Modify();
    const int32 NewIndex = Query->GetOptions().Num();
    NavEqs_EnsureOption(Query, NewIndex);
    Query->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, NewIndex);
    Result->SetNumberField(TEXT("option_count"), Query->GetOptions().Num());
    return mcp_common::MakeOkReply(Result);
}

// MCP:remove_eqs_option - Remove the option at option_index from a UEnvQuery asset loaded by asset path, together with its generator and all its tests; the options after it shift down one index, and option_count is what remains. One undo step puts it back. The asset is marked dirty, not saved: persist it with save_asset. A query that does not load, or an option_index out of range, is an error and nothing changes. Not idempotent: repeating the call removes the option that moved into that index, so read the indices with get_eqs_query_info first. Inverse: add_eqs_option; keep the option but drop its generator: remove_eqs_generator; a single test: remove_eqs_test.
//   @param query_path - /Game/... path to the UEnvQuery asset (required)
//   @param option_index - Index of the option to remove (required)
//   @returns { query_path, option_index, removed, option_count }
TSharedPtr<FJsonObject> HandleRemoveEQSOption(const TSharedPtr<FJsonObject>& Params)
{
    FString QueryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("query_path"), QueryPath)) { return Missing; }

    int32 OptionIndex = -1;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("option_index"), OptionIndex)) { return Missing; }

    UEnvQuery* Query = nullptr;
    if (TSharedPtr<FJsonObject> Failure = NavEqs_LoadQueryOption(QueryPath, OptionIndex, Query))
    {
        return Failure;
    }

    // Modify() before the removal, so undo puts the option back.
    Query->Modify();
    Query->GetOptionsMutable().RemoveAt(OptionIndex);
    Query->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = NavEqs_OptionReply(QueryPath, OptionIndex);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("option_count"), Query->GetOptions().Num());
    return mcp_common::MakeOkReply(Result);
}

// ============================================================================
// diagnose_navmesh  |  dump/diff collision channels
// ============================================================================

namespace
{
    // ------------------------------------------------------------------------
    // One parsed custom collision channel (from DefaultChannelResponses config).
    // ------------------------------------------------------------------------
    struct FNavParsedChannel
    {
        FString ChannelEnumName;      // "ECC_GameTraceChannel3"
        int32   ChannelValue = INDEX_NONE;
        FString Name;                 // friendly "Traversable"
        bool    bTraceType = false;
        bool    bStaticObject = false;
        FString DefaultResponse;      // "ECR_Block"
        bool    bIsGameSlot = false;
        // Assigned by NavColl_AssignQueryOrdinals:
        int32   QueryIndex = INDEX_NONE;   // 0-based index within its (trace|object) query enum
        FString QueryType;                 // "TraceTypeQuery3" / "ObjectTypeQuery1"
    };

    // Parse raw DefaultChannelResponses array entries (each like
    // "(Channel=ECC_GameTraceChannel3,DefaultResponse=ECR_Block,bTraceType=True,Name=\"Traversable\")")
    // by importing each into the engine's own FCustomChannelSetup struct (handles field
    // ordering/quoting/enum-by-name robustly). Same struct the engine loads config into.
    void NavColl_ParseArray(const TArray<FString>& RawEntries, TArray<FNavParsedChannel>& Out)
    {
        UEnum* ChEnum = StaticEnum<ECollisionChannel>();
        UEnum* RespEnum = StaticEnum<ECollisionResponse>();
        UScriptStruct* SetupStruct = FCustomChannelSetup::StaticStruct();
        if (!SetupStruct)
        {
            return;
        }

        for (const FString& Raw : RawEntries)
        {
            FString Line = Raw.TrimStartAndEnd();
            if (!Line.StartsWith(TEXT("(")))
            {
                continue; // not a struct literal
            }

            FCustomChannelSetup Setup;
            const TCHAR* ImportResult = SetupStruct->ImportText(
                *Line, &Setup, /*OwnerObject=*/nullptr, PPF_None, GWarn, TEXT("FCustomChannelSetup"));
            if (ImportResult == nullptr)
            {
                continue; // unparseable entry -> skip honestly
            }

            FNavParsedChannel Info;
            Info.ChannelValue = static_cast<int32>(Setup.Channel.GetValue());
            Info.ChannelEnumName = ChEnum ? ChEnum->GetNameStringByValue(Info.ChannelValue) : FString::FromInt(Info.ChannelValue);
            if (Info.ChannelEnumName.IsEmpty())
            {
                Info.ChannelEnumName = FString::Printf(TEXT("Channel%d"), Info.ChannelValue);
            }
            Info.Name = Setup.Name.ToString();
            Info.bTraceType = Setup.bTraceType;
            Info.bStaticObject = Setup.bStaticObject;
            Info.DefaultResponse = RespEnum ? RespEnum->GetNameStringByValue(static_cast<int64>(Setup.DefaultResponse.GetValue())) : FString();
            Info.bIsGameSlot = Info.ChannelEnumName.StartsWith(TEXT("ECC_GameTraceChannel"));
            Out.Add(Info);
        }
    }

    // Derive the engine's built-in trace/object channel counts from the LIVE UCollisionProfile
    // (the base index at which CUSTOM channels start). Built-ins are engine-fixed and identical
    // across projects that share this engine build, so the same bases apply to an unloaded project.
    // Falls back to the documented built-ins (Visibility+Camera = 2 trace) if the profile is absent.
    void NavColl_ComputeBuiltinBases(int32& OutTraceBase, int32& OutObjectBase)
    {
        OutTraceBase = 2;   // ECC_Visibility, ECC_Camera
        OutObjectBase = 0;

        UCollisionProfile* Profile = UCollisionProfile::Get();
        UEnum* ChEnum = StaticEnum<ECollisionChannel>();
        if (!Profile || !ChEnum)
        {
            return;
        }

        int32 TraceMax = 0;
        int32 ObjMax = 0;
        for (int32 Value = 0; Value <= 63; ++Value)
        {
            const FString Nm = ChEnum->GetNameStringByValue(Value);
            if (Nm.IsEmpty() || Nm.StartsWith(TEXT("ECC_GameTraceChannel")))
            {
                continue; // only built-in (non-custom) channels contribute to the base
            }
            const ECollisionChannel Channel = static_cast<ECollisionChannel>(Value);
            const ETraceTypeQuery Tq = Profile->ConvertToTraceType(Channel);
            if (Tq != TraceTypeQuery_MAX)
            {
                TraceMax = FMath::Max(TraceMax, static_cast<int32>(Tq) + 1);
            }
            const EObjectTypeQuery Oq = Profile->ConvertToObjectType(Channel);
            if (Oq != ObjectTypeQuery_MAX)
            {
                ObjMax = FMath::Max(ObjMax, static_cast<int32>(Oq) + 1);
            }
        }
        if (TraceMax > 0) { OutTraceBase = TraceMax; }
        OutObjectBase = ObjMax;
    }

    // Assign each named custom channel its ETraceTypeQuery / EObjectTypeQuery ordinal, exactly
    // as UCollisionProfile::LoadProfileConfig does: sort by channel value ascending, then append
    // trace channels after the built-in trace base and object channels after the object base.
    // Unnamed (NAME_None) channels are skipped (the engine ignores them for the mapping).
    void NavColl_AssignQueryOrdinals(TArray<FNavParsedChannel>& Channels, int32 TraceBase, int32 ObjectBase)
    {
        Channels.Sort([](const FNavParsedChannel& A, const FNavParsedChannel& B) { return A.ChannelValue < B.ChannelValue; });

        UEnum* TraceEnum = StaticEnum<ETraceTypeQuery>();
        UEnum* ObjEnum = StaticEnum<EObjectTypeQuery>();
        int32 TraceIdx = TraceBase;
        int32 ObjIdx = ObjectBase;

        for (FNavParsedChannel& Ch : Channels)
        {
            if (Ch.Name.IsEmpty() || Ch.Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
            {
                continue;
            }
            if (Ch.bTraceType)
            {
                Ch.QueryIndex = TraceIdx++;
                Ch.QueryType = TraceEnum ? TraceEnum->GetNameStringByValue(Ch.QueryIndex) : FString();
                if (Ch.QueryType.IsEmpty()) { Ch.QueryType = FString::Printf(TEXT("TraceTypeQuery%d"), Ch.QueryIndex + 1); }
            }
            else
            {
                Ch.QueryIndex = ObjIdx++;
                Ch.QueryType = ObjEnum ? ObjEnum->GetNameStringByValue(Ch.QueryIndex) : FString();
                if (Ch.QueryType.IsEmpty()) { Ch.QueryType = FString::Printf(TEXT("ObjectTypeQuery%d"), Ch.QueryIndex + 1); }
            }
        }
    }

    // Serialize one parsed channel to JSON (shared by dump + diff).
    TSharedPtr<FJsonObject> NavColl_ChannelToJson(const FNavParsedChannel& Ch)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("channel"), Ch.ChannelEnumName);
        Obj->SetNumberField(TEXT("channel_value"), Ch.ChannelValue);
        Obj->SetStringField(TEXT("name"), Ch.Name);
        Obj->SetBoolField(TEXT("is_trace_type"), Ch.bTraceType);
        Obj->SetBoolField(TEXT("is_static_object"), Ch.bStaticObject);
        Obj->SetBoolField(TEXT("is_custom_game_slot"), Ch.bIsGameSlot);
        Obj->SetStringField(TEXT("default_response"), Ch.DefaultResponse);
        if (Ch.QueryIndex != INDEX_NONE)
        {
            Obj->SetStringField(TEXT("query_type"), Ch.QueryType);
            Obj->SetNumberField(TEXT("query_index"), Ch.QueryIndex);          // 0-based enum value
            Obj->SetNumberField(TEXT("query_number"), Ch.QueryIndex + 1);     // 1-based (matches the "N" in TraceTypeQueryN)
        }
        return Obj;
    }

    // Read the current project's custom collision channels from GConfig (merged GEngineIni).
    void NavColl_ReadCurrentProject(TArray<FNavParsedChannel>& Out, int32& OutTraceBase, int32& OutObjectBase)
    {
        NavColl_ComputeBuiltinBases(OutTraceBase, OutObjectBase);
        TArray<FString> Raw;
        if (GConfig)
        {
            GConfig->GetArray(TEXT("/Script/Engine.CollisionProfile"), TEXT("DefaultChannelResponses"), Raw, GEngineIni);
        }
        NavColl_ParseArray(Raw, Out);
        NavColl_AssignQueryOrdinals(Out, OutTraceBase, OutObjectBase);
    }

    // The named channels keyed by their lowercased friendly name (unnamed ones are skipped).
    TMap<FString, const FNavParsedChannel*> NavColl_IndexByName(const TArray<FNavParsedChannel>& Channels)
    {
        TMap<FString, const FNavParsedChannel*> ByName;
        for (const FNavParsedChannel& Ch : Channels)
        {
            if (!Ch.Name.IsEmpty() && !Ch.Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
            {
                ByName.Add(Ch.Name.ToLower(), &Ch);
            }
        }
        return ByName;
    }
}

// ----------------------------------------------------------------------------
// diagnose_navmesh
// One-call "why is there no navmesh" report. The expensive failure
// (a NavMeshBoundsVolume with an EMPTY brush -> 0 bounds ->
// 0 navigable region -> 0 tiles, silently) is surfaced explicitly: per volume we
// report brush validity + world-space extent (empty brush == degenerate bounds),
// plus is_spatially_loaded, the union inclusion/total bounds, bWholeWorldNavigable,
// invoker-only generation + registered invokers, and the RecastNavMesh MaxTiles /
// built (active) tile count / RuntimeGeneration. A `diagnosis` array collects the
// actionable reasons. Purely read-only; never spawns nav data.
// ----------------------------------------------------------------------------
// MCP:diagnose_navmesh - One-call navmesh diagnosis: reports every ANavMeshBoundsVolume (brush validity + world extent — detects the EMPTY brush = 0 bounds that yields 0 tiles), inclusion/total nav bounds, bWholeWorldNavigable, invoker-only generation + registered invokers, the RecastNavMesh MaxTiles / built (active) tile count / RuntimeGeneration, and is_spatially_loaded per volume, the nav system state (supported agents by name, nav data count, dirty flag; a world without a navigation system reports zeros, not an error), plus an actionable `diagnosis` list. Read-only.
//   @param world - Optional world selector: absent/'editor' (default) | 'server' (**the one you want in PIE**: the navmesh is SERVER-AUTHORITATIVE, so only the authoritative world has a nav system and a RecastNavMesh) | 'pie' (ambiguous: PlayWorld, else the first PIE context -- may well land on the CLIENT) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { world, has_authority, nav_system_present, whole_world_navigable, generate_only_around_invokers, supported_agent_count, supported_agents:[string], nav_data_count, is_navigation_dirty, nav_bounds_volume_count, nav_bounds_volumes:[{actor_name, actor_label, is_spatially_loaded, brush_present, brush_empty, bounds_valid, bounds_extent:{x,y,z}, brush_poly_count, note}], registered_nav_bounds_count, total_nav_bounds:{valid, min:{x,y,z}, max:{x,y,z}, extent:{x,y,z}}, invoker_component_count, invoker_location_count, invoker_seed_bounds_count, recast_navmesh_present, recast:{name, runtime_generation, supports_runtime_generation, max_tiles, active_tile_count, tile_size_uu, is_spatially_loaded}, diagnosis:[string], success, world_name, world_package, world_type, net_mode, pie_instance }. On a CLIENT world (has_authority=false) the heuristics are suppressed and `diagnosis` says so first: a client with no nav system / no RecastNavMesh is EXPECTED, and rebuild_navigation is NEVER recommended from there.
TSharedPtr<FJsonObject> HandleDiagnoseNavmesh(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldKind;
    FString WorldErr;
    UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), WorldKind, WorldErr);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldErr.IsEmpty() ? TEXT("World not available") : WorldErr);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("world"), WorldKind);
    // WHICH world was diagnosed. Without this, a client reading and a server reading of
    // the same map are indistinguishable -- and they say opposite things.
    mcp_common::AddWorldEvidence(Result, World);

    TArray<FString> Diagnosis;

    // AUTHORITY GATE.
    // The navmesh is SERVER-AUTHORITATIVE. On a client world UNavigationSystemV1::GetCurrent
    // returns null and there is no RecastNavMesh -- that is CORRECT, not a fault -- so EVERY
    // heuristic below that keys off "no nav system / no recast / no registered bounds" is a
    // false positive by construction: ungated, this tool tells a caller to
    // "call rebuild_navigation" about a navmesh that is alive in UEDPIE_0.
    // So: say it FIRST, and never recommend a rebuild against a client.
    const bool bIsClientWorld = (World->GetNetMode() == NM_Client);
    const UPackage* WorldPackage = World->GetOutermost();
    Result->SetBoolField(TEXT("has_authority"), !bIsClientWorld);
    if (bIsClientWorld)
    {
        Diagnosis.Add(FString::Printf(
            TEXT("THIS IS A CLIENT WORLD (%s, net_mode=Client). The navmesh is SERVER-AUTHORITATIVE: ")
            TEXT("a client having NO navigation system, NO RecastNavMesh and 0 registered nav bounds is ")
            TEXT("EXPECTED and CORRECT, not a fault. Nothing in this report can diagnose the navmesh from here."),
            WorldPackage ? *WorldPackage->GetName() : *World->GetName()));
        Diagnosis.Add(TEXT("Re-run as diagnose_navmesh(world=\"server\") to read the authoritative world. NO rebuild is recommended from here: rebuild_navigation against a client would 'fix' something that is not broken."));
    }

    UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(World);
    Result->SetBoolField(TEXT("nav_system_present"), NavSys != nullptr);

    bool bWholeWorld = false;
    bool bInvokerOnly = false;
    if (NavSys)
    {
        bWholeWorld = NavSys->ShouldGenerateNavigationEverywhere();
        bInvokerOnly = NavSys->IsActiveTilesGenerationEnabled();
        Result->SetBoolField(TEXT("whole_world_navigable"), bWholeWorld);
        Result->SetBoolField(TEXT("generate_only_around_invokers"), bInvokerOnly);
        Result->SetNumberField(TEXT("supported_agent_count"), NavSys->GetSupportedAgents().Num());
        TArray<TSharedPtr<FJsonValue>> Agents;
        for (const FNavDataConfig& Agent : NavSys->GetSupportedAgents())
        {
            Agents.Add(MakeShared<FJsonValueString>(Agent.Name.ToString()));
        }
        Result->SetArrayField(TEXT("supported_agents"), Agents);
        Result->SetNumberField(TEXT("nav_data_count"), NavSys->NavDataSet.Num());
        Result->SetBoolField(TEXT("is_navigation_dirty"), NavSys->IsNavigationDirty());
    }
    else
    {
        Result->SetBoolField(TEXT("whole_world_navigable"), false);
        Result->SetBoolField(TEXT("generate_only_around_invokers"), false);
        Result->SetNumberField(TEXT("supported_agent_count"), 0);
        Result->SetArrayField(TEXT("supported_agents"), TArray<TSharedPtr<FJsonValue>>());
        Result->SetNumberField(TEXT("nav_data_count"), 0);
        Result->SetBoolField(TEXT("is_navigation_dirty"), false);
        if (!bIsClientWorld)
        {
            Diagnosis.Add(TEXT("No navigation system in this world. Ensure the project has navigation enabled and a NavMeshBoundsVolume is present, then rebuild_navigation."));
        }
        // On a client this is the EXPECTED reading and the authority note above already
        // explains it -- adding the "then rebuild_navigation" advice here would be a lie.
    }

    // --- NavMeshBoundsVolumes + per-volume brush validity/extent (empty-brush detection) ---
    int32 EmptyBrushCount = 0;
    int32 ValidVolumeCount = 0;
    TArray<TSharedPtr<FJsonValue>> VolumesArr;
    for (TActorIterator<ANavMeshBoundsVolume> It(World); It; ++It)
    {
        ANavMeshBoundsVolume* Vol = *It;
        if (!Vol)
        {
            continue;
        }

        // World-space bounds are the primary (runtime-safe) empty-brush signal: an
        // un-built brush yields degenerate/invalid bounds -> no navigable region.
        const FBox Box = Vol->GetComponentsBoundingBox(/*bNonColliding=*/true);
        const FVector Extent = Box.IsValid ? Box.GetExtent() : FVector::ZeroVector;
        const bool bBoundsValid = (Box.IsValid != 0) && (Extent.SizeSquared() > 1.0);

        // Editor-only secondary signal: the brush model's polygon count (0 == empty box).
        int32 PolyCount = -1;
        bool bBrushPresent = false;
        if (const ABrush* AsBrush = Cast<ABrush>(Vol))
        {
            if (const UModel* Model = AsBrush->Brush)
            {
                bBrushPresent = true;
                if (Model->Polys)
                {
                    PolyCount = Model->Polys->Element.Num();
                }
            }
        }

        const bool bBrushEmpty = !bBoundsValid || (PolyCount == 0);
        if (bBrushEmpty) { ++EmptyBrushCount; } else { ++ValidVolumeCount; }

        TSharedPtr<FJsonObject> VObj = MakeShared<FJsonObject>();
        VObj->SetStringField(TEXT("actor_name"), Vol->GetName());
        VObj->SetStringField(TEXT("actor_label"), Vol->GetActorLabel());
        VObj->SetBoolField(TEXT("is_spatially_loaded"), Vol->GetIsSpatiallyLoaded());
        VObj->SetBoolField(TEXT("brush_present"), bBrushPresent);
        VObj->SetBoolField(TEXT("brush_empty"), bBrushEmpty);
        VObj->SetBoolField(TEXT("bounds_valid"), bBoundsValid);
        VObj->SetObjectField(TEXT("bounds_extent"), mcp_common::VectorToJsonObject(Extent));
        VObj->SetNumberField(TEXT("brush_poly_count"), PolyCount);
        if (bBrushEmpty)
        {
            VObj->SetStringField(TEXT("note"),
                TEXT("EMPTY/degenerate brush -> contributes no navigable region (0 bounds). The spawn_navmesh_bounds_volume MCP tool creates an empty brush; place/scale a real box brush via the editor UI (Landscape/Volume brush) or spawn_box_volume, then rebuild_navigation."));
        }
        VolumesArr.Add(MakeShared<FJsonValueObject>(VObj));
    }
    Result->SetNumberField(TEXT("nav_bounds_volume_count"), VolumesArr.Num());
    Result->SetArrayField(TEXT("nav_bounds_volumes"), VolumesArr);

    // --- Registered nav bounds (inclusion / total) — union of AreaBoxes ---
    int32 RegisteredBoundsCount = 0;
    FBox TotalBounds(ForceInit);
    if (NavSys)
    {
        const TSet<FNavigationBounds>& Bounds = NavSys->GetNavigationBounds();
        RegisteredBoundsCount = Bounds.Num();
        for (const FNavigationBounds& NB : Bounds)
        {
            TotalBounds += NB.AreaBox;
        }
    }
    Result->SetNumberField(TEXT("registered_nav_bounds_count"), RegisteredBoundsCount);
    {
        TSharedPtr<FJsonObject> TB = MakeShared<FJsonObject>();
        TB->SetBoolField(TEXT("valid"), TotalBounds.IsValid != 0);
        if (TotalBounds.IsValid)
        {
            TB->SetObjectField(TEXT("min"), mcp_common::VectorToJsonObject(TotalBounds.Min));
            TB->SetObjectField(TEXT("max"), mcp_common::VectorToJsonObject(TotalBounds.Max));
            TB->SetObjectField(TEXT("extent"), mcp_common::VectorToJsonObject(TotalBounds.GetExtent()));
        }
        Result->SetObjectField(TEXT("total_nav_bounds"), TB);
    }

    // --- Navigation invokers (registered components + runtime seed/location caches) ---
    int32 InvokerComponentCount = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* A = *It;
        if (!A) { continue; }
        TArray<UNavigationInvokerComponent*> InvokerComps;
        A->GetComponents<UNavigationInvokerComponent>(InvokerComps);
        InvokerComponentCount += InvokerComps.Num();
    }
    Result->SetNumberField(TEXT("invoker_component_count"), InvokerComponentCount);
    if (NavSys)
    {
        Result->SetNumberField(TEXT("invoker_location_count"), NavSys->GetInvokerLocations().Num());
        Result->SetNumberField(TEXT("invoker_seed_bounds_count"), NavSys->GetInvokersSeedBounds().Num());
    }
    else
    {
        Result->SetNumberField(TEXT("invoker_location_count"), 0);
        Result->SetNumberField(TEXT("invoker_seed_bounds_count"), 0);
    }

    // --- Default RecastNavMesh: MaxTiles vs built tiles, RuntimeGeneration ---
    ARecastNavMesh* Recast = ResolveDefaultRecastNavMesh(World);
    Result->SetBoolField(TEXT("recast_navmesh_present"), Recast != nullptr);
    int32 MaxTiles = 0;
    int32 ActiveTiles = 0;
    if (Recast)
    {
        MaxTiles = Recast->GetNavMeshTilesCount();
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        // 5.7 has no GetNumActiveTiles(). 5.8 returns its generator's count of the tiles that hold data (0 without a
        // generator); count the same tiles, those with a header, through the public tile refs.
        if (Recast->GetGenerator())
        {
            TArray<FNavTileRef> TileRefs;
            Recast->GetAllNavMeshTiles(TileRefs);
            for (const FNavTileRef& TileRef : TileRefs)
            {
                int32 TileX = 0, TileY = 0, TileLayer = 0;
                if (Recast->GetNavMeshTileXY(TileRef, TileX, TileY, TileLayer))
                {
                    ++ActiveTiles;
                }
            }
        }
#else
        ActiveTiles = Recast->GetNumActiveTiles();
#endif

        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("name"), Recast->GetName());
        const ERuntimeGenerationType RtMode = Recast->GetRuntimeGenerationMode();
        const UEnum* RtEnum = StaticEnum<ERuntimeGenerationType>();
        R->SetStringField(TEXT("runtime_generation"),
            RtEnum ? RtEnum->GetNameStringByValue(static_cast<int64>(RtMode)) : TEXT("Unknown"));
        // Derived from the (public) generation mode to avoid depending on the access level
        // of the SupportsRuntimeGeneration() override: anything past Static supports it.
        R->SetBoolField(TEXT("supports_runtime_generation"),
            RtMode == ERuntimeGenerationType::Dynamic || RtMode == ERuntimeGenerationType::DynamicModifiersOnly);
        R->SetNumberField(TEXT("max_tiles"), MaxTiles);
        R->SetNumberField(TEXT("active_tile_count"), ActiveTiles);
        R->SetNumberField(TEXT("tile_size_uu"), Recast->GetTileSizeUU());
        R->SetBoolField(TEXT("is_spatially_loaded"), Recast->GetIsSpatiallyLoaded());
        Result->SetObjectField(TEXT("recast"), R);
    }

    // --- Actionable diagnosis --------------------------------------------------
    // Every heuristic here is gated on having AUTHORITY. On a client they all fire, all
    // of them wrongly, and the one at the bottom ("no RecastNavMesh -> call rebuild_navigation")
    // would tell a caller to fix a working navmesh.
    if (!bIsClientWorld)
    {
        if (VolumesArr.Num() == 0 && !bWholeWorld && InvokerComponentCount == 0)
        {
            Diagnosis.Add(TEXT("No NavMeshBoundsVolume, whole-world-navigable is OFF, and no navigation invokers -> there is no region to build navigation in. Add a NavMeshBoundsVolume covering the play area (with a real box brush) and rebuild_navigation."));
        }
        if (EmptyBrushCount > 0)
        {
            Diagnosis.Add(FString::Printf(
                TEXT("%d NavMeshBoundsVolume(s) have an EMPTY/degenerate brush (0 bounds) -> they contribute no navigable region. This is the classic '0 tiles' cause. Give them a real box brush (editor UI or spawn_box_volume) then rebuild_navigation."),
                EmptyBrushCount));
        }
        if (bInvokerOnly && InvokerComponentCount == 0)
        {
            Diagnosis.Add(TEXT("Invoker-only generation (bGenerateNavigationOnlyAroundNavigationInvokers) is ON but no NavigationInvokerComponents exist -> no tiles will be generated. Add an invoker (e.g. on the player pawn) or disable invoker-only generation."));
        }
        if (Recast && MaxTiles > 0 && ActiveTiles == 0)
        {
            Diagnosis.Add(TEXT("RecastNavMesh exists with 0 built (active) tiles. Call rebuild_navigation; if it stays 0, the nav bounds cover no walkable geometry (check the volume brush extent above)."));
        }
        if (!Recast && VolumesArr.Num() > 0 && ValidVolumeCount > 0)
        {
            Diagnosis.Add(TEXT("Valid NavMeshBoundsVolume(s) exist but no RecastNavMesh has been created yet. Call rebuild_navigation to spawn/build the nav data."));
        }
    }
    if (Diagnosis.Num() == 0)
    {
        if (Recast && ActiveTiles > 0)
        {
            Diagnosis.Add(TEXT("Navmesh looks healthy: a RecastNavMesh exists with built tiles. If pathing still fails, verify agent radius/height vs. the geometry and the querier's nav agent."));
        }
        else
        {
            // The advice points at world='server': 'pie' is the AMBIGUOUS selector that can
            // land on the client world.
            Diagnosis.Add(TEXT("No blocking issue detected by the heuristics; if navigation is still missing, call rebuild_navigation and re-run this diagnosis with world=\"server\" (invoker-driven tiles only build in the authoritative live world -- world=\"pie\" may resolve the client, which never has nav data)."));
        }
    }

    Result->SetArrayField(TEXT("diagnosis"), mcp_common::StringsToJson(Diagnosis));

    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Result);
}

// ----------------------------------------------------------------------------
// dump_collision_channels
// Maps every custom collision channel (ECC_GameTraceChannelN / object channels)
// to its friendly name from config, plus its TraceTypeQueryN / ObjectTypeQueryN
// index (the value a raycast/overlap actually passes). Read from GConfig
// ([/Script/Engine.CollisionProfile] DefaultChannelResponses in the merged
// GEngineIni). The query indices are computed exactly as
// UCollisionProfile::LoadProfileConfig assigns them.
// ----------------------------------------------------------------------------
// MCP:dump_collision_channels - Dumps this project's custom collision channels (from config): each ECC_GameTraceChannelN / object channel -> friendly name, whether it's a trace or object channel, and its TraceTypeQueryN / ObjectTypeQueryN index (the value a raycast/overlap passes). Read-only. Read from GConfig [/Script/Engine.CollisionProfile] DefaultChannelResponses.
//   @returns { source, builtin_trace_count, builtin_object_count, count, channels:[{channel, channel_value, name, is_trace_type, is_static_object, is_custom_game_slot, default_response, query_type, query_index, query_number}], success }
TSharedPtr<FJsonObject> HandleDumpCollisionChannels(const TSharedPtr<FJsonObject>& /*Params*/)
{
    TArray<FNavParsedChannel> Channels;
    int32 TraceBase = 0;
    int32 ObjectBase = 0;
    NavColl_ReadCurrentProject(Channels, TraceBase, ObjectBase);

    TArray<TSharedPtr<FJsonValue>> Arr;
    for (const FNavParsedChannel& Ch : Channels)
    {
        Arr.Add(MakeShared<FJsonValueObject>(NavColl_ChannelToJson(Ch)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), TEXT("GConfig [/Script/Engine.CollisionProfile] DefaultChannelResponses (GEngineIni)"));
    Result->SetNumberField(TEXT("builtin_trace_count"), TraceBase);
    Result->SetNumberField(TEXT("builtin_object_count"), ObjectBase);
    Result->SetNumberField(TEXT("count"), Arr.Num());
    Result->SetArrayField(TEXT("channels"), Arr);
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Result);
}

// ----------------------------------------------------------------------------
// diff_collision_channels
// Aligns THIS project's custom collision channels with ANOTHER project's config
// BY NAME and reports index shifts: which ECC_GameTraceChannelN slot each named
// channel occupies here vs. there, and which TraceTypeQueryN / ObjectTypeQueryN
// it maps to. This is exactly the migration hazard when bringing in content from
// another project (TraceTypeQuery3 can be one channel here and a different one there): custom channels
// don't travel on a Migrate, so same-named channels land at a different index and
// traces silently stop hitting. The other project is NOT loaded, so its config is
// read straight from disk (<dir>/DefaultEngine.ini). Query ordinals for both sides
// use the same engine-derived built-in bases, so the comparison is apples-to-apples.
// ----------------------------------------------------------------------------
// MCP:diff_collision_channels - Aligns this project's custom collision channels with another project's config BY NAME and reports index shifts (which ECC_GameTraceChannelN slot / TraceTypeQueryN a same-named channel moved to). Diagnoses the migration hazard where custom channels don't travel and traces silently stop hitting. Reads the other project's DefaultEngine.ini straight from disk (not loaded). Read-only.
//   @param other_project_config_dir - Path to the OTHER project's Config directory (containing DefaultEngine.ini); the project root also works (Config/DefaultEngine.ini is tried as a fallback)
//   @returns { other_ini_path, other_ini_found, aligned_by, this_channel_count, other_channel_count, shifted_count, matched:[{name, is_trace_type, channel_this, channel_other, channel_shifted, query_this, query_other, query_shifted}], only_in_this:[{name, channel, query_type}], only_in_other:[{name, channel, query_type}], success }
TSharedPtr<FJsonObject> HandleDiffCollisionChannels(const TSharedPtr<FJsonObject>& Params)
{
    FString OtherDir;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("other_project_config_dir"), OtherDir)) { return Missing; }

    // Resolve the other project's DefaultEngine.ini: try <dir>/DefaultEngine.ini first,
    // then <dir>/Config/DefaultEngine.ini (in case a project root was passed).
    FString IniPath = FPaths::Combine(OtherDir, TEXT("DefaultEngine.ini"));
    if (!FPaths::FileExists(IniPath))
    {
        const FString Alt = FPaths::Combine(OtherDir, TEXT("Config"), TEXT("DefaultEngine.ini"));
        if (FPaths::FileExists(Alt))
        {
            IniPath = Alt;
        }
    }
    const bool bOtherFound = FPaths::FileExists(IniPath);

    // This project (from GConfig) + shared built-in bases.
    TArray<FNavParsedChannel> ThisChannels;
    int32 TraceBase = 0;
    int32 ObjectBase = 0;
    NavColl_ReadCurrentProject(ThisChannels, TraceBase, ObjectBase);

    // Other project (from disk). Combine handles the '+' array-op prefix so GetArray
    // returns clean entries (a raw Read would keep the '+DefaultChannelResponses' key).
    TArray<FNavParsedChannel> OtherChannels;
    if (bOtherFound)
    {
        FConfigFile OtherConfig;
        OtherConfig.Combine(IniPath);
        TArray<FString> RawOther;
        OtherConfig.GetArray(TEXT("/Script/Engine.CollisionProfile"), TEXT("DefaultChannelResponses"), RawOther);
        NavColl_ParseArray(RawOther, OtherChannels);
        NavColl_AssignQueryOrdinals(OtherChannels, TraceBase, ObjectBase);
    }

    // Index each side by (lowercased) friendly name; skip unnamed channels.
    TMap<FString, const FNavParsedChannel*> ThisByName = NavColl_IndexByName(ThisChannels);
    TMap<FString, const FNavParsedChannel*> OtherByName = NavColl_IndexByName(OtherChannels);

    TArray<TSharedPtr<FJsonValue>> Matched;
    TArray<TSharedPtr<FJsonValue>> OnlyThis;
    TArray<TSharedPtr<FJsonValue>> OnlyOther;
    int32 ShiftedCount = 0;

    for (const TPair<FString, const FNavParsedChannel*>& Pair : ThisByName)
    {
        const FNavParsedChannel* A = Pair.Value;
        const FNavParsedChannel** BPtr = OtherByName.Find(Pair.Key);
        if (BPtr)
        {
            const FNavParsedChannel* B = *BPtr;
            const bool bChannelShifted = !A->ChannelEnumName.Equals(B->ChannelEnumName, ESearchCase::CaseSensitive);
            const bool bQueryShifted = !A->QueryType.Equals(B->QueryType, ESearchCase::CaseSensitive);
            if (bChannelShifted || bQueryShifted) { ++ShiftedCount; }

            TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
            M->SetStringField(TEXT("name"), A->Name);
            M->SetBoolField(TEXT("is_trace_type"), A->bTraceType);
            M->SetStringField(TEXT("channel_this"), A->ChannelEnumName);
            M->SetStringField(TEXT("channel_other"), B->ChannelEnumName);
            M->SetBoolField(TEXT("channel_shifted"), bChannelShifted);
            M->SetStringField(TEXT("query_this"), A->QueryType);
            M->SetStringField(TEXT("query_other"), B->QueryType);
            M->SetBoolField(TEXT("query_shifted"), bQueryShifted);
            Matched.Add(MakeShared<FJsonValueObject>(M));
        }
        else
        {
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), A->Name);
            O->SetStringField(TEXT("channel"), A->ChannelEnumName);
            O->SetStringField(TEXT("query_type"), A->QueryType);
            OnlyThis.Add(MakeShared<FJsonValueObject>(O));
        }
    }
    for (const TPair<FString, const FNavParsedChannel*>& Pair : OtherByName)
    {
        if (!ThisByName.Contains(Pair.Key))
        {
            const FNavParsedChannel* B = Pair.Value;
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), B->Name);
            O->SetStringField(TEXT("channel"), B->ChannelEnumName);
            O->SetStringField(TEXT("query_type"), B->QueryType);
            OnlyOther.Add(MakeShared<FJsonValueObject>(O));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("other_ini_path"), IniPath);
    Result->SetBoolField(TEXT("other_ini_found"), bOtherFound);
    Result->SetStringField(TEXT("aligned_by"), TEXT("name"));
    Result->SetNumberField(TEXT("this_channel_count"), ThisByName.Num());
    Result->SetNumberField(TEXT("other_channel_count"), OtherByName.Num());
    Result->SetNumberField(TEXT("shifted_count"), ShiftedCount);
    Result->SetArrayField(TEXT("matched"), Matched);
    Result->SetArrayField(TEXT("only_in_this"), OnlyThis);
    Result->SetArrayField(TEXT("only_in_other"), OnlyOther);
    if (!bOtherFound)
    {
        Result->SetStringField(TEXT("note"),
            TEXT("Other project's DefaultEngine.ini not found; pass the OTHER project's Config directory (or its root). Reported only this project's channels."));
    }
    Result->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_eqs_generator"), &HandleAddEQSGenerator },
    { TEXT("add_eqs_option"), &HandleAddEQSOption },
    { TEXT("add_eqs_test"), &HandleAddEQSTest },
    { TEXT("create_eqs_query"), &HandleCreateEQSQuery },
    { TEXT("diagnose_navmesh"), &HandleDiagnoseNavmesh },
    { TEXT("diff_collision_channels"), &HandleDiffCollisionChannels },
    { TEXT("dump_collision_channels"), &HandleDumpCollisionChannels },
    { TEXT("get_eqs_query_info"), &HandleGetEQSQueryInfo },
    { TEXT("get_recast_navmesh_settings"), &HandleGetRecastNavMeshSettings },
    { TEXT("list_eqs_context_types"), &HandleListEQSContextTypes },
    { TEXT("list_eqs_generator_types"), &HandleListEQSGeneratorTypes },
    { TEXT("list_eqs_test_types"), &HandleListEQSTestTypes },
    { TEXT("rebuild_navigation"), &HandleRebuildNavigation },
    { TEXT("remove_eqs_generator"), &HandleRemoveEQSGenerator },
    { TEXT("remove_eqs_option"), &HandleRemoveEQSOption },
    { TEXT("remove_eqs_test"), &HandleRemoveEQSTest },
    { TEXT("run_eqs_query"), &HandleRunEQSQuery },
    { TEXT("set_eqs_generator_properties"), &HandleSetEQSGeneratorProperties },
    { TEXT("set_eqs_test_scoring"), &HandleSetEQSTestScoring },
    { TEXT("set_recast_navmesh_settings"), &HandleSetRecastNavMeshSettings },
    { TEXT("spawn_nav_link_proxy"), &HandleSpawnNavLinkProxy },
    { TEXT("spawn_nav_modifier_volume"), &HandleSpawnNavModifierVolume },
};
} // namespace UEAgentNavigationCommands
