#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/TriggerVolume.h"
#include "Engine/TriggerBox.h"
#include "Engine/TriggerSphere.h"
// remove_trigger checks for ATriggerBase: TriggerBox, TriggerSphere and TriggerCapsule.
#include "Engine/TriggerBase.h"
#include "GameFramework/Volume.h"
// ABlockingVolume lives under Engine/, not GameFramework/.
#include "Engine/BlockingVolume.h"
#include "GameFramework/KillZVolume.h"
#include "GameFramework/PainCausingVolume.h"
#include "Engine/PostProcessVolume.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/BrushBuilder.h"   // AVolume::BrushBuilder
#include "Components/PrimitiveComponent.h"
#include "Components/BrushComponent.h"
// MCP:spawn_box_volume Modify()s the RootComponent before normalizing the actor scale,
// so the COMPLETE USceneComponent type has to be visible here (Actor.h only forward-
// declares it).
#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "Kismet/GameplayStatics.h"
#include "ScopedTransaction.h"
// spawn_box_volume: needs UModel/UPolys to report real brush geometry, and dynamic class
// resolution (StaticLoadClass / FindFirstObject) to accept short names or /Script/ paths.
#include "Model.h"
#include "Engine/Polys.h"
#include "UObject/UObjectGlobals.h"

/**
 * MCP domain for trigger actors (ATriggerBox, ATriggerSphere, ATriggerVolume) and brush
 * volumes (blocking, kill-Z, pain-causing, post-process, navigation bounds, ...): spawning
 * them with real collision, inspecting them, resizing the box and sphere triggers, overlap
 * configuration and queries, and typed removal.
 */
namespace UEAgentTriggerCommands
{

// MCP-DOMAIN: Trigger

namespace UEAgentTriggerImpl
{
    // Spawning needs both an editor world and the editor actor subsystem. Returns the subsystem,
    // or null with OutError naming whichever of the two is missing (the world is checked first).
    UEditorActorSubsystem* ActorSpawnerOrError(FString& OutError)
    {
        if (mcp_common::GetEditorWorld() == nullptr)
        {
            OutError = TEXT("World not available");
            return nullptr;
        }
        UEditorActorSubsystem* const Spawner = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
        if (Spawner == nullptr)
        {
            OutError = TEXT("EditorActorSubsystem not available");
        }
        return Spawner;
    }

    // The three actor kinds the trigger queries accept.
    bool IsTriggerActor(const AActor& Actor)
    {
        return Actor.IsA<ATriggerBox>() || Actor.IsA<ATriggerSphere>() || Actor.IsA<ATriggerVolume>();
    }

    // The component whose overlaps matter: the root when it is a primitive (the brush of a
    // volume, the shape of a box/sphere trigger), else the first primitive on the actor.
    UPrimitiveComponent* OverlapPrimitive(AActor& Actor)
    {
        if (UPrimitiveComponent* const RootShape = Cast<UPrimitiveComponent>(Actor.GetRootComponent()))
        {
            return RootShape;
        }
        TArray<UPrimitiveComponent*> Primitives;
        Actor.GetComponents<UPrimitiveComponent>(Primitives);
        return Primitives.Num() > 0 ? Primitives[0] : nullptr;
    }

    // bounds_origin / bounds_extent, as FVector::ToString() text, from a component's bounds box.
    void WriteBoundsText(FJsonObject& Out, const FBoxSphereBounds& Bounds)
    {
        FVector Center;
        FVector HalfSize;
        Bounds.GetBox().GetCenterAndExtents(Center, HalfSize);
        Out.SetStringField(TEXT("bounds_origin"), Center.ToString());
        Out.SetStringField(TEXT("bounds_extent"), HalfSize.ToString());
    }

    // Resolve a box-volume class from either a short reflected name
    // ("NavMeshBoundsVolume", "PhysicsVolume", "TriggerVolume", with or without the
    // leading 'A') or a full object path ("/Script/Engine.TriggerVolume", "/Game/...").
    // Only classes deriving from AVolume are accepted: spawn_box_volume builds a UModel
    // brush, which is meaningful only for brush-based volume actors. Abstract classes are
    // rejected so SpawnActorFromClass cannot fail on them.
    UClass* ResolveBoxVolumeClass(const FString& InName)
    {
        if (InName.IsEmpty())
        {
            return nullptr;
        }

        UClass* Found = nullptr;

        // Path / script form.
        if (InName.Contains(TEXT("/")) || InName.Contains(TEXT(".")))
        {
            Found = StaticLoadClass(AVolume::StaticClass(), nullptr, *InName);
        }
        else
        {
            // Short name: reflected UClass names carry no 'A' prefix, so try a bare form
            // (strip a leading 'A') first, then the literal string as given.
            FString Bare = InName;
            if (Bare.StartsWith(TEXT("A")) && Bare.Len() > 1)
            {
                Bare.RightChopInline(1);
            }
            Found = FindFirstObject<UClass>(*Bare, EFindFirstObjectOptions::None);
            if (!Found)
            {
                Found = FindFirstObject<UClass>(*InName, EFindFirstObjectOptions::None);
            }
        }

        if (Found && Found->IsChildOf(AVolume::StaticClass()) && !Found->HasAnyClassFlags(CLASS_Abstract))
        {
            return Found;
        }
        return nullptr;
    }
}

// Editor-world lookup by object name (the editor label is not consulted); null if absent.
AActor* FindActorByName(const FString& Name)
{
    // Lookups are by object name in the editor world only; no world means nothing to find.
    return mcp_common::FindActorByObjectName(mcp_common::GetEditorWorld(), Name);
}

// Shape triggers: creation, inspection, overlap set-up and overlap queries.

// MCP:create_trigger - Creates a trigger actor (TriggerBox or TriggerSphere) at a specified location. These two shapes only -- for a brush-based ATriggerVolume use spawn_box_volume(volume_class="TriggerVolume") instead.
//   @param trigger_name - Name label for the trigger actor
//   @param trigger_type - Trigger class: "TriggerBox" (default) or "TriggerSphere". "TriggerVolume" is deliberately REJECTED here -- ATriggerVolume derives from AVolume/ABrush and needs a real UModel brush built for it, so spawning it through this path would hand you a success-shaped actor with no real collision; use spawn_box_volume(volume_class="TriggerVolume"), which builds that brush.
//   @param location - World [x, y, z] location to spawn the trigger (default 0,0,0)
//   @returns { trigger_name, trigger_type, actor_name, actor_path, success }
TSharedPtr<FJsonObject> HandleCreateTrigger(const TSharedPtr<FJsonObject>& Params)
{
    FString Label;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("trigger_name"), Label, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString Shape = TEXT("TriggerBox");
    Params->TryGetStringField(TEXT("trigger_type"), Shape);

    FString Problem;
    UEditorActorSubsystem* const Spawner = UEAgentTriggerImpl::ActorSpawnerOrError(Problem);
    if (Spawner == nullptr)
    {
        return mcp_common::MakeErrorReply(Problem);
    }

    // Only the two shape triggers are built here (anything unrecognised becomes a box). A
    // TriggerVolume is refused outright: it needs a brush that this path never builds.
    if (Shape == TEXT("TriggerVolume"))
    {
        return mcp_common::MakeErrorReply(
            TEXT("trigger_type=TriggerVolume is not supported by create_trigger (ATriggerVolume needs a real ")
            TEXT("Brush/UModel built via UActorFactory::CreateBrushForVolumeActor, which this command does not ")
            TEXT("build, leaving no working collision); use spawn_box_volume(volume_class=\"TriggerVolume\") instead."));
    }
    UClass* const ShapeClass = (Shape == TEXT("TriggerSphere"))
        ? ATriggerSphere::StaticClass()
        : ATriggerBox::StaticClass();

    const FVector SpawnAt = mcp_common::ReadJsonVector(Params, TEXT("location"));
    AActor* const Spawned = Spawner->SpawnActorFromClass(ShapeClass, SpawnAt, FRotator::ZeroRotator);
    if (Spawned == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn trigger actor"));
    }
    Spawned->SetActorLabel(Label);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("success"), true);
    Report->SetStringField(TEXT("trigger_name"), Label);
    Report->SetStringField(TEXT("trigger_type"), Shape);
    Report->SetStringField(TEXT("actor_name"), Spawned->GetName());
    Report->SetStringField(TEXT("actor_path"), Spawned->GetPathName());
    return mcp_common::MakeOkReply(Report);
}

// MCP:get_trigger_info - Retrieves information about a trigger actor, including its type and bounding box.
//   @param actor_name - Name of the trigger actor to query
//   @returns { actor_name, actor_class, is_trigger_box, is_trigger_sphere, bounds_origin, bounds_extent }
TSharedPtr<FJsonObject> HandleGetTriggerInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* const Found = FindActorByName(Wanted);
    if (Found == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *Wanted));
    }
    if (!UEAgentTriggerImpl::IsTriggerActor(*Found))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor is not a TriggerBox/TriggerSphere/TriggerVolume: %s"), *Wanted));
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Wanted);
    Report->SetStringField(TEXT("actor_class"), Found->GetClass()->GetName());
    Report->SetBoolField(TEXT("is_trigger_box"), Found->IsA<ATriggerBox>());
    Report->SetBoolField(TEXT("is_trigger_sphere"), Found->IsA<ATriggerSphere>());

    // The bounds are read off the root collision primitive, and only when the root is one.
    if (const UPrimitiveComponent* const RootShape = Cast<UPrimitiveComponent>(Found->GetRootComponent()))
    {
        UEAgentTriggerImpl::WriteBoundsText(*Report, RootShape->Bounds);
    }
    return mcp_common::MakeOkReply(Report);
}

// MCP:get_actors_in_trigger - List and count the actors that a TriggerBox, TriggerSphere or TriggerVolume in the editor world (not PIE) currently reports as overlapping (AActor::GetOverlappingActors), without changing anything. Returns actor_count and actors [{actor_name, actor_class}], plus the trigger's own object name and class. The first actor whose object name or editor label matches is used; when it is not a trigger the call fails.
//   @param trigger_name - Object name or editor label of the trigger actor, such as the actor_name returned by create_trigger.
//   @returns { trigger_name, actor_name (the trigger's object name), actor_class (the trigger's class), actors: [{actor_name, actor_class}], actor_count }
TSharedPtr<FJsonObject> HandleGetActorsInTrigger(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("trigger_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* EditorWorld = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(EditorWorld, TEXT("Editor world unavailable"))) { return NoWorld; }

    // The object name or the editor label; the first actor answering to either decides the outcome.
    AActor* const Trigger = mcp_common::FindActorByName(EditorWorld, Wanted);
    if (Trigger == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Trigger not found: %s"), *Wanted));
    }
    if (!UEAgentTriggerImpl::IsTriggerActor(*Trigger))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor is not a TriggerBox/TriggerSphere/TriggerVolume: %s"), *Wanted));
    }

    TArray<AActor*> Inside;
    Trigger->GetOverlappingActors(Inside);

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const AActor* Other : Inside)
    {
        if (Other == nullptr)
        {
            continue;
        }
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actor_name"), Other->GetName());
        Entry->SetStringField(TEXT("actor_class"), Other->GetClass()->GetName());
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("trigger_name"), Wanted);
    Report->SetStringField(TEXT("actor_name"), Trigger->GetName());
    Report->SetStringField(TEXT("actor_class"), Trigger->GetClass()->GetName());
    Report->SetNumberField(TEXT("actor_count"), Entries.Num());
    Report->SetArrayField(TEXT("actors"), Entries);
    return mcp_common::MakeOkReply(Report);
}

// MCP:set_trigger_events - Configure overlap-event generation on a trigger/volume's root primitive component: applies the built-in 'Trigger' collision profile (QueryOnly with overlap responses) and toggles SetGenerateOverlapEvents. Honesty note: this configures the component to FIRE overlap events, but binding an actual OnComponentBeginOverlap / OnActorBeginOverlap callback requires a Blueprint or native C++ class on the actor -- the cmd reports callback_bound=false to make that explicit.
//   @param actor_name [string!] - Trigger/volume actor name (required)
//   @param generate_overlap_events [bool] - Whether the component generates overlap events (default true)
//   @returns { actor_name, component_name, collision_profile, overlap_events_enabled, message, callback_bound, success }
TSharedPtr<FJsonObject> HandleSetTriggerEvents(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* const Target = FindActorByName(Wanted);
    if (Target == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *Wanted));
    }

    // What can honestly be configured from here is whether the collision primitive GENERATES
    // overlap events at all (default: yes). Binding a callback to them is a separate matter.
    bool bEmitOverlaps = true;
    Params->TryGetBoolField(TEXT("generate_overlap_events"), bEmitOverlaps);

    UPrimitiveComponent* const Shape = UEAgentTriggerImpl::OverlapPrimitive(*Target);
    if (Shape == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor '%s' has no primitive/collision component to configure for overlap events"),
            *Wanted));
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetTriggerEvents", "Set Trigger Events"));
    Target->Modify();
    Shape->Modify();

    // The engine's stock "Trigger" profile (BaseEngine.ini: QueryOnly, overlap responses) has no
    // named C++ constant, hence the literal. Together with the overlap flag it is what makes the
    // component actually produce overlap events.
    Shape->SetCollisionProfileName(FName(TEXT("Trigger")));
    Shape->SetGenerateOverlapEvents(bEmitOverlaps);
    Shape->MarkRenderStateDirty();

    // Everything below re-reads the component. callback_bound stays false: a handler for
    // OnComponentBeginOverlap / OnActorBeginOverlap needs a Blueprint graph or a native class,
    // and cannot be attached to an arbitrary placed actor from here.
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Wanted);
    Report->SetStringField(TEXT("component_name"), Shape->GetName());
    Report->SetStringField(TEXT("collision_profile"), Shape->GetCollisionProfileName().ToString());
    Report->SetBoolField(TEXT("overlap_events_enabled"), Shape->GetGenerateOverlapEvents());
    Report->SetStringField(TEXT("message"),
        TEXT("Overlap-event generation enabled and Trigger collision profile applied. ")
        TEXT("Binding an actual overlap callback still requires a Blueprint or native class."));
    Report->SetBoolField(TEXT("callback_bound"), false);
    Report->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Report);
}

// Brush volumes.

// MCP:get_volume_info - Read a volume actor in the editor world without changing it: its class, whether it is a BlockingVolume, KillZVolume, PainCausingVolume or PostProcessVolume and, when it has a brush component, bounds_origin and bounds_extent as text plus has_brush. Fails if the actor is not an AVolume.
//   @param actor_name - Object name of the volume actor in the editor world; its editor label is not matched.
//   @returns { actor_name, actor_class, is_blocking_volume, is_killz_volume, is_pain_causing_volume, is_post_process_volume, bounds_origin, bounds_extent, has_brush }
TSharedPtr<FJsonObject> HandleGetVolumeInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* const Found = FindActorByName(Wanted);
    if (Found == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *Wanted));
    }
    const AVolume* const AsVolume = Cast<AVolume>(Found);
    if (AsVolume == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor is not a Volume: %s"), *Wanted));
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Wanted);
    Report->SetStringField(TEXT("actor_class"), Found->GetClass()->GetName());
    Report->SetBoolField(TEXT("is_blocking_volume"), Found->IsA<ABlockingVolume>());
    Report->SetBoolField(TEXT("is_killz_volume"), Found->IsA<AKillZVolume>());
    Report->SetBoolField(TEXT("is_pain_causing_volume"), Found->IsA<APainCausingVolume>());
    Report->SetBoolField(TEXT("is_post_process_volume"), Found->IsA<APostProcessVolume>());

    // Bounds and has_brush are only published when the volume carries a brush component.
    if (const UBrushComponent* const BrushShape = AsVolume->GetBrushComponent())
    {
        UEAgentTriggerImpl::WriteBoundsText(*Report, BrushShape->Bounds);
        Report->SetBoolField(TEXT("has_brush"), AsVolume->Brush != nullptr);
    }
    return mcp_common::MakeOkReply(Report);
}

// MCP:spawn_box_volume - Spawn ANY AVolume-derived actor (default BlockingVolume) in the editor world AND build a real cube brush for it from a half-extent, so the volume actually encloses space: spawn_actor has no volume types, and a volume spawned without a brush has zero bounds -- 0 nav tiles, no overlap. It then normalizes the residual scale some actor factories stamp on placement (PCGVolume: 25,25,10) and RE-READS the brush bounds, so `success` means the volume matches the extent you asked for, not that the call returned. Typed siblings add one step each: spawn_pcg_volume assigns a graph, spawn_nav_modifier_volume an area class, spawn_level_streaming_volume binds a sub-level. Called with no class and no extent it gives a 200 cm BlockingVolume cube. One undo step; refused during PIE; the level is not saved.
//   @param volume_class [string] - Optional volume class (default "BlockingVolume"): short reflected name ('BlockingVolume', 'KillZVolume', 'PainCausingVolume', 'PostProcessVolume', 'TriggerVolume', 'NavMeshBoundsVolume', 'PhysicsVolume', 'PCGVolume', case-insensitive, with or without a leading 'A') or a full object path ('/Script/Engine.TriggerVolume'). Must be a concrete AVolume subclass; anything else is an error.
//   @param location - World [x, y, z] location to spawn the volume (default 0,0,0)
//   @param extent [array] - Optional [x, y, z] in unreal units, each component > 0; default [100, 100, 100] (a 200 cm cube). !!! THIS IS THE SEMI-AXIS (HALF-size), NOT the full edge length !!! extent=[500,500,200] builds a 1000 x 1000 x 400 cube and, with the scale normalized, gives world bounds of +/-500 / +/-200 around `location`. Check `bounds_extent_actual` against what you asked for. A value that is not an array is an error.
//   @param actor_label [string] - Optional editor label for the spawned actor
//   @param is_spatially_loaded [bool] - Optional World Partition spatial-loading flag. Applied only when passed and the actor allows changing it (spatial_flag_applied); omitted, the actor keeps its class default. Reported RE-READ from the actor.
//   @param normalize_scale [bool] - Default TRUE: resets the actor scale to (1,1,1) after building the brush, so the world bounds equal the requested `extent`. It matters because some actor factories stamp a default scale during placement -- APCGVolume is spawned at (25, 25, 10) -- and since brush polys are authored in LOCAL space that scale MULTIPLIES your extent. Set it false only if you deliberately want the factory's scale; `spawn_scale` always reports what it was. Be aware that with normalize_scale=false and a factory scale != 1 the real bounds are NOT the requested extent, so `extent_matches_request` and therefore `success` come back FALSE -- that is the honest answer, not a bug, and `actor_scale` says why they differ.
//   @returns { actor_name, actor_path, actor_class, volume_class, world_bounds_extent: [x,y,z] (RE-READ, GetActorBounds -- kept for compatibility), bounds_extent_actual: [x,y,z] (RE-READ from the BRUSH component's world bounds; this is the volume itself, with no stray editor sprite in the box), bounds_origin_actual: [x,y,z], actor_scale: [x,y,z] (RE-READ, after normalization), spawn_scale: [x,y,z] (RE-READ, as the actor factory left it), scale_normalized: bool (true only when a correction was actually needed), extent_matches_request: bool, bounds_tolerance_uu: number, has_brush_geometry, brush_built, is_spatially_loaded (RE-READ), spatial_flag_applied, success, requested: { volume_class, location, extent, actor_label, is_spatially_loaded, normalize_scale } }
//   NOTE (impl): the brush is built with UCubeBuilder in C++. The `extent` hint is written `[array]`
//   because the catalog's annotation regex cannot parse `[x, y, z]` as a type hint, and that line is
//   where a caller looks for the half-vs-full answer. The factory scale comes from
//   UPCGEngineSettings::VolumeScale (PCGEngineSettings.h), applied by UPCGVolumeFactory::PreSpawnActor.
//
// WHY THE SCALE IS NORMALIZED.
// The UCubeBuilder brush comes out CORRECT (the semi-axis x2), and a PCGVolume can still end up with
// GetActorScale3D() = (25, 25, 10): world bounds 25x / 10x larger than the requested extent. The
// cause is NOT in this command and NOT in the brush: UEditorActorSubsystem::SpawnActorFromClass routes the spawn
// through the placement path (TryPlacingActorFromObject -> PlaceAssetInCurrentLevel -> the class's
// UActorFactory), and UPCGVolumeFactory::PreSpawnActor stamps
// InOutLocation.SetScale3D(UPCGEngineSettings::VolumeScale), whose default literally is
// FVector(25, 25, 10) -- the scale the PCG plugin wants for a graph dropped in the viewport.
// Brush polys are LOCAL, so world bounds = local half-extent * actor scale, and the whole `extent`
// contract of this tool is only true at scale 1. It has to live here because the factory
// belongs to the engine plugin; for every other volume class the scale is already 1 and the
// normalization is a no-op. `success` is derived from the bounds actually matching `extent`.
TSharedPtr<FJsonObject> HandleSpawnBoxVolume(const TSharedPtr<FJsonObject>& Params)
{
    // Both optional: a bare call gives a BlockingVolume, the most common brush volume.
    FString VolumeClassName = TEXT("BlockingVolume");
    Params->TryGetStringField(TEXT("volume_class"), VolumeClassName);

    // Omitted, the half-extent is 100 uu per axis: the 200 uu cube of the UCubeBuilder defaults, which is
    // what the editor's own volume factories build. A typed read, so the catalog publishes extent as an array.
    FVector Extent(100.0, 100.0, 100.0);
    const TArray<TSharedPtr<FJsonValue>>* ExtentJson = nullptr;
    if (Params->TryGetArrayField(TEXT("extent"), ExtentJson))
    {
        Extent = mcp_common::ReadJsonVector(Params, TEXT("extent"));
    }
    else if (Params->HasField(TEXT("extent")))
    {
        return mcp_common::MakeErrorReply(TEXT("extent must be an array [x, y, z] of cube half-sizes, each > 0"));
    }
    if (Extent.X <= 0.0 || Extent.Y <= 0.0 || Extent.Z <= 0.0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("extent components must all be > 0 (got %f, %f, %f)"), Extent.X, Extent.Y, Extent.Z));
    }

    FString SpawnerProblem;
    UEditorActorSubsystem* const Spawner = UEAgentTriggerImpl::ActorSpawnerOrError(SpawnerProblem);
    if (Spawner == nullptr)
    {
        return mcp_common::MakeErrorReply(SpawnerProblem);
    }

    UClass* VolumeClass = UEAgentTriggerImpl::ResolveBoxVolumeClass(VolumeClassName);
    if (!VolumeClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("spawn_box_volume: could not resolve '%s' to a concrete, non-abstract AVolume-derived class"),
            *VolumeClassName));
    }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    // Written only when the caller asks: omitted, the actor keeps the flag its class gives it.
    bool bIsSpatiallyLoaded = true;
    const bool bSpatialRequested = Params->TryGetBoolField(TEXT("is_spatially_loaded"), bIsSpatiallyLoaded);

    FString ActorLabel;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SpawnBoxVolume", "Spawn Box Volume"));

    AActor* NewActor = Spawner->SpawnActorFromClass(VolumeClass, Location, FRotator::ZeroRotator);
    if (!NewActor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Failed to spawn volume actor of class '%s'"), *VolumeClass->GetName()));
    }

    AVolume* VolumeActor = Cast<AVolume>(NewActor);
    if (!VolumeActor)
    {
        // Should not happen (ResolveBoxVolumeClass enforces AVolume), but guard so we never
        // report a brush on a non-volume actor.
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("spawn_box_volume: spawned actor of class '%s' is not an AVolume"), *NewActor->GetClass()->GetName()));
    }

    // Read the scale the actor FACTORY left on the actor, before anything here touches it.
    // For APCGVolume this is (25, 25, 10); for every stock engine volume it is (1, 1, 1).
    const FVector SpawnScale = VolumeActor->GetActorScale3D();

    // Build the REAL cube brush the way the editor's volume actor factories do, from the half-extent.
    const bool bBrushBuilt = mcp_common::BuildCubeBrushForVolume(VolumeActor, Extent);

    // Re-assert the requested location (brush build runs PreEditChange/PostEditChange).
    VolumeActor->SetActorLocation(Location);

    // Normalize the residual factory scale (see WHY THE SCALE IS NORMALIZED above HandleSpawnBoxVolume).
    // Modify() on the actor AND on its RootComponent: the transform lives on the root
    // component, and on a World Partition (OFPA) map only dirtying both captures the
    // external actor package.
    bool bNormalizeScale = true;
    Params->TryGetBoolField(TEXT("normalize_scale"), bNormalizeScale);
    bool bScaleNormalized = false;
    if (bNormalizeScale && !SpawnScale.Equals(FVector::OneVector, KINDA_SMALL_NUMBER))
    {
        VolumeActor->Modify();
        if (USceneComponent* Root = VolumeActor->GetRootComponent())
        {
            Root->Modify();
        }
        VolumeActor->SetActorScale3D(FVector::OneVector);
        bScaleNormalized = true;
    }

    // Apply the World Partition spatial-loading flag only when it was passed and the actor permits it;
    // the setter asserts (check()) if the flag cannot be changed for this actor, so guard against a crash.
    bool bSpatialApplied = false;
    if (bSpatialRequested && VolumeActor->CanChangeIsSpatiallyLoadedFlag())
    {
        VolumeActor->SetIsSpatiallyLoaded(bIsSpatiallyLoaded);
        bSpatialApplied = true;
    }

    if (!ActorLabel.IsEmpty())
    {
        VolumeActor->SetActorLabel(ActorLabel);
    }

    // --- everything below is a RE-READ of the actor ------------------------
    // Report the real world-space bounds half-extent from the built brush.
    FVector BoundsOrigin(0, 0, 0), BoundsExtent(0, 0, 0);
    VolumeActor->GetActorBounds(/*bOnlyCollidingComponents*/ false, BoundsOrigin, BoundsExtent);

    // The BRUSH component's own bounds are the volume itself. GetActorBounds() unions every
    // component, so an editor-only sprite/billboard on some volume subclass would inflate it;
    // the request/result comparison has to be made against the brush, not against that union.
    FVector BrushOrigin = BoundsOrigin, BrushExtent = BoundsExtent;
    if (const UBrushComponent* BrushComp = VolumeActor->GetBrushComponent())
    {
        BrushComp->Bounds.GetBox().GetCenterAndExtents(BrushOrigin, BrushExtent);
    }

    const FVector ActorScale = VolumeActor->GetActorScale3D();

    // has_brush_geometry: a genuinely built brush has a UModel with a non-empty Polys array.
    const bool bHasBrushGeometry =
        VolumeActor->Brush != nullptr &&
        VolumeActor->Brush->Polys != nullptr &&
        VolumeActor->Brush->Polys->Element.Num() > 0;

    // Tolerance: 0,1 % of the largest requested semi-axis, with a 1 uu floor. The bounds come
    // from a float BSP poly buffer transformed to double world space, so an exact compare would
    // false-alarm on large volumes; 0,1 % is still ~4 orders of magnitude tighter than the x25
    // this check exists to catch.
    const double BoundsTolerance = FMath::Max(1.0, 0.001 * Extent.GetMax());
    const bool bExtentMatches = BrushExtent.Equals(Extent, BoundsTolerance);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_name"), VolumeActor->GetName());
    ResultObj->SetStringField(TEXT("actor_path"), VolumeActor->GetPathName());
    ResultObj->SetStringField(TEXT("actor_class"), VolumeActor->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("volume_class"), VolumeClass->GetName());

    ResultObj->SetArrayField(TEXT("world_bounds_extent"), mcp_common::VectorToJsonArray(BoundsExtent));
    ResultObj->SetArrayField(TEXT("bounds_extent_actual"), mcp_common::VectorToJsonArray(BrushExtent));
    ResultObj->SetArrayField(TEXT("bounds_origin_actual"), mcp_common::VectorToJsonArray(BrushOrigin));
    ResultObj->SetArrayField(TEXT("actor_scale"), mcp_common::VectorToJsonArray(ActorScale));
    ResultObj->SetArrayField(TEXT("spawn_scale"), mcp_common::VectorToJsonArray(SpawnScale));
    ResultObj->SetBoolField(TEXT("scale_normalized"), bScaleNormalized);
    ResultObj->SetBoolField(TEXT("extent_matches_request"), bExtentMatches);
    ResultObj->SetNumberField(TEXT("bounds_tolerance_uu"), BoundsTolerance);

    ResultObj->SetBoolField(TEXT("has_brush_geometry"), bHasBrushGeometry);
    ResultObj->SetBoolField(TEXT("brush_built"), bBrushBuilt);
    // RE-READ, always: reporting back the requested flag when "applied" would be an echo,
    // and CanChangeIsSpatiallyLoadedFlag() is not the only thing that can reject it.
    ResultObj->SetBoolField(TEXT("is_spatially_loaded"), VolumeActor->GetIsSpatiallyLoaded());
    ResultObj->SetBoolField(TEXT("spatial_flag_applied"), bSpatialApplied);

    // `success` is DERIVED from the two things this tool exists to guarantee: that a real
    // brush was built, and that the volume it encloses is the one that was asked for.
    const bool bSuccess = bHasBrushGeometry && bExtentMatches;
    ResultObj->SetBoolField(TEXT("success"), bSuccess);

    mcp_common::AddRequestedEcho(ResultObj, Params,
        { TEXT("volume_class"), TEXT("location"), TEXT("extent"),
          TEXT("actor_label"), TEXT("is_spatially_loaded"), TEXT("normalize_scale") });

    FString Error;
    if (!bHasBrushGeometry)
    {
        Error = FString::Printf(
            TEXT("spawn_box_volume: '%s' was spawned but its UModel brush has no polys -- the volume encloses nothing."),
            *VolumeActor->GetName());
    }
    else if (!bExtentMatches)
    {
        Error = FString::Printf(
            TEXT("spawn_box_volume: real brush half-extent (%.1f, %.1f, %.1f) does not match the requested extent (%.1f, %.1f, %.1f) within %.1f uu. Actor scale is %s (the factory spawned it at %s); `extent` is a SEMI-AXIS and only equals the bounds at scale 1."),
            BrushExtent.X, BrushExtent.Y, BrushExtent.Z,
            Extent.X, Extent.Y, Extent.Z, BoundsTolerance,
            *ActorScale.ToString(), *SpawnScale.ToString());
    }

    return mcp_common::MakeOutcomeReply(ResultObj, bSuccess, Error);
}

// ===========================================================================
// Typed remove commands + get_trigger_events:
//   remove_trigger         (typed: ATriggerBase — TriggerBox, TriggerSphere, TriggerCapsule)
//   remove_volume          (typed: AVolume — covers BlockingVolume, PainCausingVolume, etc.)
//   get_trigger_events     (reads overlap-event config; inverse of set_trigger_events)
// Each remove_X uses a typed-template pattern (load + Cast<T> + World->DestroyActor)
// so they are NOT aliases of any generic delete command (no aliases by design).
// ===========================================================================

/**
 * Shared body of the typed remove commands. The actor is looked up by object name in the
 * editor world and destroyed only when it really is an ExpectedClass. "Not found" is an answer, not a
 * failure: a success envelope with deleted=false and a note. An actor of another class is refused
 * (success false, nothing destroyed), and so is a destroy the world does not carry out. The class is a
 * parameter rather than a template argument so the catalog generator follows each command into this body.
 */
static TSharedPtr<FJsonObject> HandleRemoveTypedTrigger(const TSharedPtr<FJsonObject>& Params, const TCHAR* CmdName,
    const UClass* ExpectedClass)
{
    FString Target;
    if (!Params->TryGetStringField(TEXT("actor_name"), Target) || Target.IsEmpty())
    {
        const FString Missing = FString::Printf(TEXT("[%s] Missing 'actor_name' parameter"), CmdName);
        return mcp_common::MakeErrorReply(Missing);
    }
    UWorld* const EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("World not available"));
    }

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("actor_name"), Target);

    AActor* const Match = mcp_common::FindActorByObjectName(EditorWorld, Target);
    if (Match == nullptr)
    {
        Outcome->SetBoolField(TEXT("already_existed"), false);
        Outcome->SetBoolField(TEXT("deleted"), false);
        Outcome->SetStringField(TEXT("note"), TEXT("actor_name did not resolve to a loadable AActor"));
        return mcp_common::MakeOkReply(Outcome);
    }

    // Captured before any destruction, so it is reported in every remaining branch.
    const FString MatchClass = Match->GetClass()->GetName();
    Outcome->SetStringField(TEXT("actor_class"), MatchClass);

    if (!Match->IsA(ExpectedClass))
    {
        const FString Refusal = FString::Printf(TEXT("actor is not a %s (got %s) - wrong type, refusing to delete"),
            *ExpectedClass->GetName(), *MatchClass);
        Outcome->SetBoolField(TEXT("deleted"), false);
        Outcome->SetStringField(TEXT("note"), Refusal);
        return mcp_common::MakeOutcomeReply(Outcome, false, FString::Printf(TEXT("%s: %s: %s"), CmdName, *Target, *Refusal));
    }

    const bool bDestroyed = EditorWorld->DestroyActor(Match);
    Outcome->SetBoolField(TEXT("already_existed"), true);
    Outcome->SetBoolField(TEXT("deleted"), bDestroyed);
    return mcp_common::MakeOutcomeReply(Outcome, bDestroyed, bDestroyed ? FString() : FString::Printf(
        TEXT("%s: UWorld::DestroyActor refused to destroy '%s' (an actor the world protects, or one already being destroyed)"),
        CmdName, *Target));
}

// MCP:remove_trigger - Delete one shape trigger -- a TriggerBox, TriggerSphere or TriggerCapsule -- from the editor world. The actor is found by its object name and its class is checked first: any other class, a TriggerVolume included, is refused and nothing is destroyed. The deletion records no undo step and does not save the level (save_level does). A name that matches no actor is a success with deleted=false, so a repeat call is harmless (idempotent). For a brush volume use remove_volume; to delete any actor with undo, delete_actor.
//   @param actor_name - The trigger's object name, not its editor label (e.g. TriggerBox_0, as get_actors_in_trigger reports it in actor_name); matched without case.
//   @returns { actor_name, actor_class, deleted (false when nothing was destroyed), already_existed (false when no actor had that name), note? }
TSharedPtr<FJsonObject> HandleRemoveTrigger(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedTrigger(Params, TEXT("remove_trigger"), ATriggerBase::StaticClass());
}

// MCP:remove_volume - Destroy a volume ACTOR in the world -- any AVolume subclass: BlockingVolume, PainCausingVolume, PostProcessVolume, TriggerVolume and the rest. Typed: it verifies the actor really is an AVolume before deleting, so pointing it at a non-volume actor is refused rather than obeyed (success false, deleted false, nothing destroyed). This is the counterpart of spawn_box_volume; for a TriggerBox, TriggerSphere or TriggerCapsule use remove_trigger instead.
//   @param actor_name - Name of the AVolume actor.
//   @returns { actor_name, actor_class, deleted, already_existed, note? }
TSharedPtr<FJsonObject> HandleRemoveVolume(const TSharedPtr<FJsonObject>& Params)
{
    return HandleRemoveTypedTrigger(Params, TEXT("remove_volume"), AVolume::StaticClass());
}

// MCP:get_trigger_events - Read the overlap-event configuration of an actor's primitive component: collision profile, whether overlap events are enabled at all, and which begin/end overlap handlers are actually bound. Read-only inverse of set_trigger_events -- it changes nothing, so it is the way to check a trigger is really armed before blaming the gameplay code.
//   @param actor_name - Name of the actor (must have a primitive/collision component).
//   @returns { actor_name, component_name, collision_profile, overlap_events_enabled, begin_overlap_bound, end_overlap_bound, callback_bound }
TSharedPtr<FJsonObject> HandleGetTriggerEvents(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    AActor* const Actor = FindActorByName(ActorName);
    if (Actor == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // Same component set_trigger_events configures: the root primitive, else the first one.
    UPrimitiveComponent* const PrimComp = UEAgentTriggerImpl::OverlapPrimitive(*Actor);
    if (PrimComp == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor '%s' has no primitive/collision component to inspect"), *ActorName));
    }

    // Delegate presence (informational only — headless cannot bind new delegates
    // on placed editor actors; that requires a Blueprint or native class). We check
    // the multicast delegate's IsBoundToObject state to report whether anything is
    // currently listening (a property of the placed actor itself).
    bool bBeginBound = false;
    if (FMulticastDelegateProperty* BeginProp = CastField<FMulticastDelegateProperty>(
            PrimComp->GetClass()->FindPropertyByName(TEXT("OnComponentBeginOverlap"))))
    {
        FMulticastScriptDelegate& BeginDel = *BeginProp->ContainerPtrToValuePtr<FMulticastScriptDelegate>(PrimComp);
        bBeginBound = BeginDel.IsBound();
    }
    bool bEndBound = false;
    if (FMulticastDelegateProperty* EndProp = CastField<FMulticastDelegateProperty>(
            PrimComp->GetClass()->FindPropertyByName(TEXT("OnComponentEndOverlap"))))
    {
        FMulticastScriptDelegate& EndDel = *EndProp->ContainerPtrToValuePtr<FMulticastScriptDelegate>(PrimComp);
        bEndBound = EndDel.IsBound();
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("actor_name"), ActorName);
    Resp->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    Resp->SetStringField(TEXT("component_name"), PrimComp->GetName());
    Resp->SetStringField(TEXT("component_class"), PrimComp->GetClass()->GetName());
    Resp->SetStringField(TEXT("collision_profile"), PrimComp->GetCollisionProfileName().ToString());
    Resp->SetBoolField(TEXT("overlap_events_enabled"), PrimComp->GetGenerateOverlapEvents());
    Resp->SetBoolField(TEXT("begin_overlap_bound"), bBeginBound);
    Resp->SetBoolField(TEXT("end_overlap_bound"), bEndBound);
    Resp->SetBoolField(TEXT("callback_bound"), bBeginBound || bEndBound);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// Typed setters for the geometry params of the trigger shapes.
//
// Pattern: resolve the typed shape component on the actor, then write the
// public UPROPERTY with a Modify() + MarkRenderStateDirty() so the editor
// re-renders.
// ---------------------------------------------------------------------------

// MCP:set_trigger_box_extent - Resize a TriggerBox in the editor world by setting the unscaled half-extent of its box collision component and updating its overlaps; the level is marked dirty, not saved. Returns previous_extent, new_extent and changed. Each extent must be greater than 0.
//   @param actor_name [string!] - Object name of the TriggerBox actor in the editor world; its editor label is not matched.
//   @param extent_x [number!] - Half-extent along local X in cm, greater than 0; the box is twice as long.
//   @param extent_y [number!] - Half-extent along local Y in cm, greater than 0.
//   @param extent_z [number!] - Half-extent along local Z in cm, greater than 0.
//   @returns { actor_name, component_name, previous_extent: {x, y, z}, new_extent: {x, y, z}, changed }
TSharedPtr<FJsonObject> HandleSetTriggerBoxExtent(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Wanted)) { return Missing; }
    double Ex = 0.0, Ey = 0.0, Ez = 0.0;
    if (!Params->TryGetNumberField(TEXT("extent_x"), Ex) || !Params->TryGetNumberField(TEXT("extent_y"), Ey) || !Params->TryGetNumberField(TEXT("extent_z"), Ez))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameters: extent_x, extent_y, extent_z (all > 0)"));
    }
    if (Ex <= 0.0 || Ey <= 0.0 || Ez <= 0.0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Box extents must be > 0 (got %f, %f, %f)"), Ex, Ey, Ez));
    }

    ATriggerBox* const Box = Cast<ATriggerBox>(FindActorByName(Wanted));
    if (Box == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("TriggerBox actor not found: %s"), *Wanted));
    }
    UBoxComponent* const Collision = Cast<UBoxComponent>(Box->GetCollisionComponent());
    if (Collision == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("TriggerBox '%s' has no UBoxComponent on its collision"), *Wanted));
    }

    const FVector Before = Collision->GetUnscaledBoxExtent();
    const FVector After(Ex, Ey, Ez);

    Box->Modify();
    Collision->Modify();
    Collision->SetBoxExtent(After, /*bUpdateOverlaps=*/true);
    Collision->MarkRenderStateDirty();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Wanted);
    Report->SetStringField(TEXT("component_name"), Collision->GetName());
    Report->SetObjectField(TEXT("previous_extent"), mcp_common::VectorToJsonObject(Before));
    Report->SetObjectField(TEXT("new_extent"), mcp_common::VectorToJsonObject(After));
    Report->SetBoolField(TEXT("changed"), !Before.Equals(After));
    return mcp_common::MakeOkReply(Report);
}

// MCP:set_trigger_sphere_radius - Resize a TriggerSphere in the editor world by setting the unscaled radius of its sphere collision component and updating its overlaps; the level is marked dirty, not saved. Returns previous_radius, new_radius and changed. The radius must be greater than 0.
//   @param actor_name [string!] - Object name of the TriggerSphere actor in the editor world; its editor label is not matched.
//   @param radius [number!] - New unscaled sphere radius in cm, greater than 0.
//   @returns { actor_name, component_name, previous_radius, new_radius, changed }
TSharedPtr<FJsonObject> HandleSetTriggerSphereRadius(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Wanted)) { return Missing; }
    double RequestedRadius = 0.0;
    if (!Params->TryGetNumberField(TEXT("radius"), RequestedRadius))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: radius (float, > 0)"));
    }
    if (RequestedRadius <= 0.0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("radius must be > 0 (got %f)"), RequestedRadius));
    }

    ATriggerSphere* const Sphere = Cast<ATriggerSphere>(FindActorByName(Wanted));
    if (Sphere == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("TriggerSphere actor not found: %s"), *Wanted));
    }
    USphereComponent* const Collision = Cast<USphereComponent>(Sphere->GetCollisionComponent());
    if (Collision == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("TriggerSphere '%s' has no USphereComponent on its collision"), *Wanted));
    }

    // The component stores a float radius; the request is narrowed the same way.
    const float Before = Collision->GetUnscaledSphereRadius();
    const float After = static_cast<float>(RequestedRadius);

    Sphere->Modify();
    Collision->Modify();
    Collision->SetSphereRadius(After, /*bUpdateOverlaps=*/true);
    Collision->MarkRenderStateDirty();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), Wanted);
    Report->SetStringField(TEXT("component_name"), Collision->GetName());
    Report->SetNumberField(TEXT("previous_radius"), Before);
    Report->SetNumberField(TEXT("new_radius"), After);
    Report->SetBoolField(TEXT("changed"), Before != After);
    return mcp_common::MakeOkReply(Report);
}

// UBrushBuilder has no Bounds UPROPERTY (bounds are derived from Vertices/Polys, and a
// resize needs a Build() re-call), so there is no typed setter for an AVolume brush size: only
// the trigger shapes have one (box extent, sphere radius).

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_trigger"), &HandleCreateTrigger },
    { TEXT("get_actors_in_trigger"), &HandleGetActorsInTrigger },
    { TEXT("get_trigger_events"), &HandleGetTriggerEvents },
    { TEXT("get_trigger_info"), &HandleGetTriggerInfo },
    { TEXT("get_volume_info"), &HandleGetVolumeInfo },
    { TEXT("remove_trigger"), &HandleRemoveTrigger },
    { TEXT("remove_volume"), &HandleRemoveVolume },
    { TEXT("set_trigger_box_extent"), &HandleSetTriggerBoxExtent },
    { TEXT("set_trigger_events"), &HandleSetTriggerEvents },
    { TEXT("set_trigger_sphere_radius"), &HandleSetTriggerSphereRadius },
    { TEXT("spawn_box_volume"), &HandleSpawnBoxVolume },
};
} // namespace UEAgentTriggerCommands
