#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

// MotionWarping plugin (UE 5.8, Runtime module).
#include "MotionWarpingComponent.h"
#include "MotionWarpingAdapter.h"   // Full type of UMotionWarpingBaseAdapter for Adapter->GetClass()
#include "RootMotionModifier.h"

/**
 * Motion Warping component authoring commands.
 *
 * They operate on the DATA layer of the MotionWarping plugin only:
 *   - Attach/detach a UMotionWarpingComponent to an actor in the editor world.
 *   - Add, edit, remove and list the FMotionWarpingTarget entries of an existing component, and set its
 *     bSearchForWindowsInAnimsWithinMontages flag.
 *   - Inspect the component's runtime state (warp targets + modifier count + adapter class + protected
 *     switch-off-conditions count via reflection).
 *
 * The live warping runtime (Update / ProcessRootMotion) is intentionally out of scope — those code paths run
 * on the game thread during the actor tick and are not safe to drive from the bridge FTSTicker drain.
 */
namespace UEAgentMotionWarpingCommands
{

// MCP-DOMAIN: Animation

namespace
{
    /** Reads the required "actor_path" param. Null when it is there, else the error reply. */
    TSharedPtr<FJsonObject> MW_ReadActorPath(const TSharedPtr<FJsonObject>& Params, FString& OutActorPath)
    {
        if (!mcp_common::ExtractStringPath(Params, TEXT("actor_path"), OutActorPath))
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'actor_path' parameter"));
        }
        return nullptr;
    }

    /**
     * Resolve an actor by its `actor_path` (full editor path or short label).
     *
     * Tries the fast path first: FindObject<AActor>(nullptr, *Path) on the
     * canonical "/Game/Maps/MyMap.MyMap:PersistentLevel.ActorName" form the
     * bridge writes back. Falls back to the editor world's actors by name or
     * label so callers can pass a short "BP_MyCharacter_C_2" style name and
     * still hit the right actor. Null on success, else the error reply.
     */
    TSharedPtr<FJsonObject> MW_ResolveActor(const FString& ActorPath, AActor*& OutActor)
    {
        if (ActorPath.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'actor_path' parameter"));
        }

        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("no editor world")))
        {
            return NoWorld;
        }

        OutActor = FindObject<AActor>(nullptr, *ActorPath);
        if (!OutActor)
        {
            OutActor = mcp_common::FindActorByName(World, ActorPath);
        }
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found at actor_path '%s'"), *ActorPath));
        }
        return nullptr;
    }

    /**
     * Find the first UMotionWarpingComponent attached to an actor, or nullptr.
     * Idempotent authoring relies on this being a singleton-per-actor.
     */
    UMotionWarpingComponent* MW_FindMotionWarpingComponent(AActor* Actor)
    {
        if (!Actor)
        {
            return nullptr;
        }
        return Actor->FindComponentByClass<UMotionWarpingComponent>();
    }

    /**
     * Locate a USceneComponent on the actor by name (FName match against
     * GetFName() + editor label). Used when the caller passes a component_path
     * to add_warp_target_to_component. Returns the first match.
     */
    USceneComponent* MW_FindSceneComponentByName(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor || ComponentName.IsEmpty())
        {
            return nullptr;
        }

        TInlineComponentArray<USceneComponent*> Components(Actor);
        for (USceneComponent* Comp : Components)
        {
            if (!Comp)
            {
                continue;
            }
            if (Comp->GetName() == ComponentName ||
                (Comp->GetOwner() && Comp->GetOwner()->GetActorLabel() == ComponentName))
            {
                return Comp;
            }
        }
        return nullptr;
    }

    /**
     * Read the size of the protected `SwitchOffConditions` array on
     * UMotionWarpingComponent via reflection (FindFProperty bypasses C++
     * access control). Returns -1 when the property cannot be resolved.
     */
    int32 MW_ReadSwitchOffConditionCount(UMotionWarpingComponent* Comp)
    {
        if (!Comp)
        {
            return -1;
        }
        FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(
            UMotionWarpingComponent::StaticClass(), TEXT("SwitchOffConditions"));
        if (!ArrayProp)
        {
            return -1;
        }
        const void* ArrayPtr = ArrayProp->ContainerPtrToValuePtr<void>(Comp);
        FScriptArrayHelper Helper(ArrayProp, ArrayPtr);
        return Helper.Num();
    }

    /** Serialise one FMotionWarpingTarget into a JSON object. */
    TSharedPtr<FJsonObject> MW_TargetToJson(const FMotionWarpingTarget& Target)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Target.Name.ToString());

        Obj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Target.Location));
        Obj->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(Target.Rotation));

        if (const USceneComponent* CompRef = Target.Component.Get())
        {
            Obj->SetStringField(TEXT("component_path"), CompRef->GetPathName());
        }
        else
        {
            Obj->SetField(TEXT("component_path"), MakeShared<FJsonValueNull>());
        }

        Obj->SetStringField(TEXT("bone_name"), Target.BoneName.ToString());
        Obj->SetBoolField(TEXT("b_follow_component"), Target.bFollowComponent);

        Obj->SetArrayField(TEXT("location_offset"), mcp_common::VectorToJsonArray(Target.LocationOffset));
        Obj->SetArrayField(TEXT("rotation_offset"), mcp_common::RotatorToJsonArray(Target.RotationOffset));

        const int32 DirectionInt = static_cast<int32>(Target.LocationOffsetDirection);
        Obj->SetNumberField(TEXT("location_offset_direction"), DirectionInt);
        return Obj;
    }

    /** Convert a TConstArrayView<FMotionWarpingTarget> into a JSON array. */
    TArray<TSharedPtr<FJsonValue>> MW_TargetsToJsonArray(TConstArrayView<FMotionWarpingTarget> Targets)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Targets.Num());
        for (const FMotionWarpingTarget& Target : Targets)
        {
            Out.Add(MakeShared<FJsonValueObject>(MW_TargetToJson(Target)));
        }
        return Out;
    }

    /** MW_ResolveActor, then the actor's UMotionWarpingComponent. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> MW_ResolveActorComponent(const FString& ActorPath, AActor*& OutActor,
        UMotionWarpingComponent*& OutComponent)
    {
        if (TSharedPtr<FJsonObject> Error = MW_ResolveActor(ActorPath, OutActor))
        {
            return Error;
        }
        OutComponent = MW_FindMotionWarpingComponent(OutActor);
        if (!OutComponent)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No UMotionWarpingComponent on actor '%s'. Call add_motion_warping_component first."),
                *OutActor->GetPathName()));
        }
        return nullptr;
    }

    /**
     * The actor of the editor world whose object name is ActorName, and its UMotionWarpingComponent. Null on success,
     * else the error reply.
     */
    TSharedPtr<FJsonObject> MW_FindNamedActorComponent(const FString& ActorName, AActor*& OutActor,
        UMotionWarpingComponent*& OutComponent)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
        {
            return NoWorld;
        }
        OutActor = mcp_common::FindActorByObjectName(World, ActorName);
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
        }
        OutComponent = OutActor->FindComponentByClass<UMotionWarpingComponent>();
        if (!OutComponent)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Actor '%s' has no UMotionWarpingComponent"), *ActorName));
        }
        return nullptr;
    }

    /** A reply that opens with the actor's path (actor_path). */
    TSharedPtr<FJsonObject> MW_StartReply(const AActor* Actor)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("actor_path"), Actor->GetPathName());
        return Reply;
    }

    /** A reply that opens with the actor's object name (actor_name) and the warp target named. */
    TSharedPtr<FJsonObject> MW_StartTargetReply(const AActor* Actor, const FString& WarpTargetName)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("actor_name"), Actor->GetName());
        Reply->SetStringField(TEXT("warp_target_name"), WarpTargetName);
        return Reply;
    }
}

// ---------------------------------------------------------------------------
// add_motion_warping_component
// ---------------------------------------------------------------------------
// MCP:add_motion_warping_component - Attach a UMotionWarpingComponent to an actor in the editor world. Idempotent: if the actor already has one, returns the existing instance with already_existed:true instead of creating a duplicate. The component is NewObject<UMotionWarpingComponent>(Actor, FName("MotionWarping")) followed by RegisterComponent() so it shows up in GetComponents() immediately. The owning world's level is marked dirty so the editor persists the change on save; no explicit SavePackage call (avoids the noise of saving a transient actor out of an editor world).
//   @param actor_path [string!] - Full editor path (e.g. "/Game/Maps/Test.Test:PersistentLevel.ActorName") or short name of the actor.
//   @returns { actor_path: string, component_path: string, already_existed: bool }

// ---------------------------------------------------------------------------
// add_warp_target_to_component
// ---------------------------------------------------------------------------
// MCP:add_warp_target_to_component - Add (or update in-place) a FMotionWarpingTarget on an existing UMotionWarpingComponent. Exactly one of three payload shapes must be supplied: (1) component_path -> AddOrUpdateWarpTargetFromComponent(name, comp, NAME_None, false, TargetsForwardVector, ZeroVector, ZeroRotator); (2) location + rotation -> AddOrUpdateWarpTargetFromLocationAndRotation; (3) location only -> AddOrUpdateWarpTargetFromLocation. The engine API is idempotent by WarpTargetName (replace-in-place). Marking the owning actor dirty so the editor persists the change.
//   @param actor_path [string!] - Full editor path or short name of the actor.
//   @param target_name [string!] - FName identifier for the warp target.
//   @param location - Optional world [x, y, z] location for the target.
//   @param rotation - Optional world [pitch, yaw, roll] rotation in degrees for the target.
//   @param component_path [string] - Optional name of a USceneComponent on the actor to drive the target transform.
//   @returns { actor_path: string, target_name: string, warp_target_count: int, source: string, already_existed: bool }

// ---------------------------------------------------------------------------
// remove_warp_target
// ---------------------------------------------------------------------------
// MCP:remove_warp_target - Remove a FMotionWarpingTarget from a UMotionWarpingComponent by FName. Wraps UMotionWarpingComponent::RemoveWarpTarget(name) which returns the count of entries removed (0 or 1; the array is keyed by FName). Idempotent: removing a non-existent target returns count:0 without raising an error.
//   @param actor_path [string!] - Full editor path or short name of the actor.
//   @param target_name [string!] - FName identifier of the warp target to remove.
//   @returns { actor_path: string, target_name: string, removed_count: int, warp_target_count: int }

// ---------------------------------------------------------------------------
// list_warp_targets_on_component
// ---------------------------------------------------------------------------
// MCP:list_warp_targets_on_component - List every FMotionWarpingTarget on an actor's UMotionWarpingComponent. Read-only: wraps GetWarpTargets() (TConstArrayView<FMotionWarpingTarget>). Each entry reports the FName, the static location/rotation, the optional component reference (via path or null), the optional bone name, the b_follow_component flag, the location/rotation offsets, and the location_offset_direction enum integer.
//   @param actor_path [string!] - Full editor path or short name of the actor.
//   @returns { actor_path: string, warp_target_count: int, warp_targets: array }

// ---------------------------------------------------------------------------
// inspect_motion_warping_component
// ---------------------------------------------------------------------------
// MCP:inspect_motion_warping_component - Read the full data-layer state of a UMotionWarpingComponent: the b_search_for_windows_in_anims_within_montages flag, the warp target array (via GetWarpTargets()), the modifier count (via GetModifiers()), the runtime adapter class (GetOwnerAdapter()->GetClass()), and the protected switch-off-conditions array count (via reflection — FindFProperty<FArrayProperty> + FScriptArrayHelper). Read-only, idempotent.
//   @param actor_path [string!] - Full editor path or short name of the actor.
//   @returns { actor_path: string, b_search_for_windows_in_anims_within_montages: bool, warp_target_count: int, modifier_count: int, owner_adapter_class: string, switch_off_condition_count: int, warp_targets: array }

TSharedPtr<FJsonObject> HandleAddMotionWarpingComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActor(ActorPath, Actor)) { return Error; }

    // Idempotency check: if the actor already has a UMotionWarpingComponent,
    // return it with already_existed:true (idempotent authoring).
    if (UMotionWarpingComponent* Existing = MW_FindMotionWarpingComponent(Actor))
    {
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_motion_warping_component: actor '%s' already has MotionWarping at '%s'"),
            *Actor->GetName(), *Existing->GetPathName());

        TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
        Data->SetStringField(TEXT("component_path"), Existing->GetPathName());
        Data->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(Data);
    }

    UMotionWarpingComponent* NewComp = NewObject<UMotionWarpingComponent>(
        Actor, UMotionWarpingComponent::StaticClass(), FName(TEXT("MotionWarping")));
    if (!NewComp)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create UMotionWarpingComponent for actor '%s'"), *Actor->GetName()));
    }

    NewComp->RegisterComponent();

    // Mark the owning level dirty so the editor persists the new component on save.
    if (ULevel* Level = Actor->GetLevel())
    {
        Level->MarkPackageDirty();
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_motion_warping_component: created MotionWarping at '%s' on actor '%s'"),
        *NewComp->GetPathName(), *Actor->GetName());

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("component_path"), NewComp->GetPathName());
    Data->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Data);
}

TSharedPtr<FJsonObject> HandleAddWarpTargetToComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    FString TargetNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_name"), TargetNameStr)) { return Missing; }
    const FName TargetName(*TargetNameStr);

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActorComponent(ActorPath, Actor, Comp)) { return Error; }

    FString ComponentPath;
    const bool bHasComponentPath = Params->TryGetStringField(TEXT("component_path"), ComponentPath) && !ComponentPath.IsEmpty();
    const bool bHasLocation = Params->HasField(TEXT("location"));
    const bool bHasRotation = Params->HasField(TEXT("rotation"));

    FString Source;
    bool bAlreadyExisted = false;

    if (bHasComponentPath)
    {
        // (1) Component-driven target. The component lives on the same actor
        // for the data-layer scope of these commands.
        USceneComponent* CompRef = MW_FindSceneComponentByName(Actor, ComponentPath);
        if (!CompRef)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Scene component '%s' not found on actor '%s'"),
                    *ComponentPath, *Actor->GetName()));
        }

        bAlreadyExisted = (Comp->FindWarpTarget(TargetName) != nullptr);
        Comp->AddOrUpdateWarpTargetFromComponent(
            TargetName,
            CompRef,
            NAME_None,
            /*bFollowComponent=*/false,
            EWarpTargetLocationOffsetDirection::TargetsForwardVector,
            FVector::ZeroVector,
            FRotator::ZeroRotator);
        Source = TEXT("component");
    }
    else if (bHasLocation && bHasRotation)
    {
        // (2) Location + rotation.
        const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
        const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));

        bAlreadyExisted = (Comp->FindWarpTarget(TargetName) != nullptr);
        Comp->AddOrUpdateWarpTargetFromLocationAndRotation(TargetName, Location, Rotation);
        Source = TEXT("location_and_rotation");
    }
    else if (bHasLocation)
    {
        // (3) Location only.
        const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

        bAlreadyExisted = (Comp->FindWarpTarget(TargetName) != nullptr);
        Comp->AddOrUpdateWarpTargetFromLocation(TargetName, Location);
        Source = TEXT("location");
    }
    else
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide 'component_path' or 'location' (optionally with 'rotation')."));
    }

    if (ULevel* Level = Actor->GetLevel())
    {
        Level->MarkPackageDirty();
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] add_warp_target_to_component: actor='%s' target='%s' source=%s already_existed=%s"),
        *Actor->GetName(), *TargetNameStr, *Source, bAlreadyExisted ? TEXT("true") : TEXT("false"));

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("target_name"), TargetNameStr);
    Data->SetNumberField(TEXT("warp_target_count"), Comp->GetWarpTargets().Num());
    Data->SetStringField(TEXT("source"), Source);
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    return mcp_common::MakeOkReply(Data);
}

TSharedPtr<FJsonObject> HandleRemoveWarpTarget(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    FString TargetNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_name"), TargetNameStr)) { return Missing; }
    const FName TargetName(*TargetNameStr);

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActorComponent(ActorPath, Actor, Comp)) { return Error; }

    const int32 Removed = Comp->RemoveWarpTarget(TargetName);

    if (Removed > 0)
    {
        if (ULevel* Level = Actor->GetLevel())
        {
            Level->MarkPackageDirty();
        }
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_warp_target: actor='%s' target='%s' removed=%d remaining=%d"),
        *Actor->GetName(), *TargetNameStr, Removed, Comp->GetWarpTargets().Num());

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("target_name"), TargetNameStr);
    Data->SetNumberField(TEXT("removed_count"), Removed);
    Data->SetNumberField(TEXT("warp_target_count"), Comp->GetWarpTargets().Num());
    return mcp_common::MakeOkReply(Data);
}

TSharedPtr<FJsonObject> HandleListWarpTargetsOnComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActorComponent(ActorPath, Actor, Comp)) { return Error; }

    const TConstArrayView<FMotionWarpingTarget> Targets = Comp->GetWarpTargets();
    const TArray<TSharedPtr<FJsonValue>> TargetsArr = MW_TargetsToJsonArray(Targets);

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] list_warp_targets_on_component: actor='%s' count=%d"),
        *Actor->GetName(), Targets.Num());

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetNumberField(TEXT("warp_target_count"), Targets.Num());
    Data->SetArrayField(TEXT("warp_targets"), TargetsArr);
    return mcp_common::MakeOkReply(Data);
}

TSharedPtr<FJsonObject> HandleInspectMotionWarpingComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActorComponent(ActorPath, Actor, Comp)) { return Error; }

    const TConstArrayView<FMotionWarpingTarget> Targets = Comp->GetWarpTargets();
    const TArray<TSharedPtr<FJsonValue>> TargetsArr = MW_TargetsToJsonArray(Targets);
    const int32 ModifierCount = Comp->GetModifiers().Num();
    const int32 SwitchOffCount = MW_ReadSwitchOffConditionCount(Comp);

    FString AdapterClassName;
    if (UMotionWarpingBaseAdapter* Adapter = Comp->GetOwnerAdapter())
    {
        AdapterClassName = Adapter->GetClass()->GetName();
    }
    else
    {
        AdapterClassName = TEXT("None");
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] inspect_motion_warping_component: actor='%s' warp_targets=%d modifiers=%d switch_off=%d adapter=%s"),
        *Actor->GetName(), Targets.Num(), ModifierCount, SwitchOffCount, *AdapterClassName);

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetBoolField(TEXT("b_search_for_windows_in_anims_within_montages"),
        Comp->bSearchForWindowsInAnimsWithinMontages);
    Data->SetNumberField(TEXT("warp_target_count"), Targets.Num());
    Data->SetNumberField(TEXT("modifier_count"), ModifierCount);
    Data->SetStringField(TEXT("owner_adapter_class"), AdapterClassName);
    Data->SetNumberField(TEXT("switch_off_condition_count"), SwitchOffCount);
    Data->SetArrayField(TEXT("warp_targets"), TargetsArr);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Explicit location/rotation setter + bulk clear.
// ---------------------------------------------------------------------------

// MCP:set_motion_warping_target_location - Update an existing FMotionWarpingTarget's location (and optionally rotation) by FName -- an idempotent replace-in-place. Choose this over add_warp_target_to_component when the target already exists and you only want to nudge where it points; add_warp_target_to_component is the one that CREATES a target.
//   @param actor_name - Engine name of the actor carrying the UMotionWarpingComponent
//   @param warp_target_name - FName of the existing target
//   @param location - [x, y, z] world location
//   @param rotation - Optional [pitch, yaw, roll] rotation; defaults to ZeroRotator
//   @returns { actor_name, warp_target_name, location_set, previous_count, new_count }
//   NOTE (impl): wraps UMotionWarpingComponent::AddOrUpdateWarpTargetFromLocationAndRotation with the existing target name.

TSharedPtr<FJsonObject> HandleSetMotionWarpingTargetLocation(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString WarpTargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("warp_target_name"), WarpTargetName)) { return Missing; }

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_FindNamedActorComponent(ActorName, Actor, Comp)) { return Error; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    const int32 PrevCount = Comp->GetWarpTargets().Num();
    Comp->Modify();
    Comp->AddOrUpdateWarpTargetFromLocationAndRotation(FName(*WarpTargetName), Location, Rotation);
    Actor->MarkPackageDirty();

    TSharedPtr<FJsonObject> Resp = MW_StartTargetReply(Actor, WarpTargetName);
    Resp->SetBoolField(TEXT("location_set"), true);
    Resp->SetNumberField(TEXT("previous_count"), PrevCount);
    Resp->SetNumberField(TEXT("new_count"), Comp->GetWarpTargets().Num());
    return mcp_common::MakeOkReply(Resp);
}

// MCP:clear_motion_warping_targets - Remove EVERY FMotionWarpingTarget from a UMotionWarpingComponent in one call (there is no bulk engine API; use remove_warp_target for a single named target). Reports previous + new count and the names it removed. Idempotent: clearing an already-empty component returns cleared:false + count:0, not an error.
//   @param actor_name - Engine name of the actor carrying the UMotionWarpingComponent
//   @returns { actor_name, cleared, previous_count, new_count, removed_names: [string] }
//   NOTE (impl): loops UMotionWarpingComponent::RemoveWarpTarget, which takes one FName at a time.

TSharedPtr<FJsonObject> HandleClearMotionWarpingTargets(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetActorName)) { return Missing; }
    AActor* Owner = nullptr;
    UMotionWarpingComponent* Warping = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_FindNamedActorComponent(TargetActorName, Owner, Warping)) { return Error; }

    // The engine has no bulk clear: snapshot the names first, then remove them one by one.
    TArray<FName> RemovedNames;
    for (const FMotionWarpingTarget& Existing : Warping->GetWarpTargets())
    {
        RemovedNames.Add(Existing.Name);
    }
    const int32 CountBefore = RemovedNames.Num();

    Warping->Modify();
    for (const FName& Removed : RemovedNames)
    {
        Warping->RemoveWarpTarget(Removed);
    }
    Owner->MarkPackageDirty();

    TArray<TSharedPtr<FJsonValue>> RemovedJson;
    RemovedJson.Reserve(CountBefore);
    for (const FName& Removed : RemovedNames)
    {
        RemovedJson.Add(MakeShared<FJsonValueString>(Removed.ToString()));
    }

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("actor_name"), Owner->GetName());
    Outcome->SetBoolField(TEXT("cleared"), CountBefore > 0);
    Outcome->SetNumberField(TEXT("previous_count"), CountBefore);
    Outcome->SetNumberField(TEXT("new_count"), Warping->GetWarpTargets().Num());
    Outcome->SetArrayField(TEXT("removed_names"), RemovedJson);
    return mcp_common::MakeOkReply(Outcome);
}

// ---------------------------------------------------------------------------
// remove_motion_warping_component
// ---------------------------------------------------------------------------
// MCP:remove_motion_warping_component - Detach the UMotionWarpingComponent from an actor (mirror of add_motion_warping_component). Idempotent: an actor that has no such component returns already_existed:false + removed:false + note, not an error.
//   @param actor_path [string!] - Full editor path or short name of the actor.
//   @returns { actor_path: string, component_path?: string, removed: bool, already_existed: bool, note?: string }
//   NOTE (impl): calls Actor->DestroyComponent(Comp), which unregisters the component and marks it for GC.
TSharedPtr<FJsonObject> HandleRemoveMotionWarpingComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActor(ActorPath, Actor)) { return Error; }

    UMotionWarpingComponent* Existing = MW_FindMotionWarpingComponent(Actor);
    if (!Existing)
    {
        TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
        Data->SetBoolField(TEXT("already_existed"), false);
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetStringField(TEXT("note"),
            TEXT("No UMotionWarpingComponent attached to this actor; nothing to remove (idempotent)."));
        return mcp_common::MakeOkReply(Data);
    }

    const FString ComponentPath = Existing->GetPathName();
    Actor->Modify();
    // UActorComponent::DestroyComponent returns void; the component is GC-eligible
    // after this call. Mark success unless we somehow didn't have a component (caught above).
    Existing->DestroyComponent();
    const bool bDestroyed = true;

    if (ULevel* Level = Actor->GetLevel())
    {
        Level->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("component_path"), ComponentPath);
    Data->SetBoolField(TEXT("already_existed"), true);
    Data->SetBoolField(TEXT("removed"), bDestroyed);
    return mcp_common::MakeOkReply(Data);
}// ---------------------------------------------------------------------------
// Typed setters on FMotionWarpingTarget fields and the
// bSearchForWindowsInAnimsWithinMontages component-level toggle.
//
// Pattern: same actor/component resolution as HandleSetMotionWarpingTargetLocation.
// For setters that need the existing component ref (BoneName / follow /
// location-offset / rotation-offset), we look it up via FindWarpTarget first
// and re-call AddOrUpdateWarpTargetFromComponent with the new parameter —
// that public API is idempotent on (warp_target_name) per UE 5.8 source
// (MotionWarpingComponent.cpp::AddOrUpdateWarpTargetFromComponent does
// replace-in-place when the name already exists).
// ---------------------------------------------------------------------------

namespace
{
    // Every typed setter below: reads "actor_name" and resolves that actor of the editor world and its
    // UMotionWarpingComponent. Null on success, else the error reply.
    TSharedPtr<FJsonObject> MW_ResolveNamedComponent(const TSharedPtr<FJsonObject>& Params, AActor*& OutActor,
        UMotionWarpingComponent*& OutComponent)
    {
        FString ActorName;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
        return MW_FindNamedActorComponent(ActorName, OutActor, OutComponent);
    }

    /** The warp target WarpTargetName of Comp. Null on success, else the error reply, which names Actor. */
    TSharedPtr<FJsonObject> MW_FindWarpTarget(const UMotionWarpingComponent* Comp, const AActor* Actor,
        const FString& WarpTargetName, const FMotionWarpingTarget*& OutTarget)
    {
        OutTarget = Comp->FindWarpTarget(FName(*WarpTargetName));
        if (!OutTarget)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("FMotionWarpingTarget '%s' not found on actor '%s'"),
                    *WarpTargetName, *Actor->GetName()));
        }
        return nullptr;
    }

    /** MW_FindWarpTarget for a target that follows a scene component. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> MW_FindComponentWarpTarget(const UMotionWarpingComponent* Comp, const AActor* Actor,
        const FString& WarpTargetName, const FMotionWarpingTarget*& OutTarget)
    {
        if (TSharedPtr<FJsonObject> Error = MW_FindWarpTarget(Comp, Actor, WarpTargetName, OutTarget))
        {
            return Error;
        }
        if (!OutTarget->Component.IsValid())
        {
            return mcp_common::MakeErrorReply(
                TEXT("FMotionWarpingTarget has no Component ref -- create the target via add_warp_target_to_component first"));
        }
        return nullptr;
    }
}

// Each typed setter changes ONE property in place (no removal + re-add), so the gameplay layer keeps the same
// FMotionWarpingTarget instance with the same RPC/replication identity. The component-ref based setters read
// the existing component from the BlueprintReadOnly TWeakObjectPtr stored on the target itself.

// MCP:set_motion_warping_target_rotation - Set the rotation of an existing warp target of an editor-world actor (found by internal name), keeping its Location. The target is rebuilt as a plain transform target, so a component-based one loses its Component, BoneName, follow flag and offsets. WarpTargets is Transient: not saved.
//   @param actor_name - Internal object name of the actor with the UMotionWarpingComponent, e.g. "BP_Hero_C_0"; the outliner label does not match.
//   @param warp_target_name - FName of the existing warp target on the component
//   @param rotation - New world rotation in degrees as [pitch, yaw, roll]; omitted means [0, 0, 0].
//   @returns { actor_name, warp_target_name, previous_pitch, previous_yaw, previous_roll, new_pitch, new_yaw, new_roll, changed: bool }
TSharedPtr<FJsonObject> HandleSetMotionWarpingTargetRotation(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveNamedComponent(Params, Actor, Comp)) { return Error; }

    FString WarpTargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("warp_target_name"), WarpTargetName)) { return Missing; }
    const FRotator NewRotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));

    const FMotionWarpingTarget* Existing = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_FindWarpTarget(Comp, Actor, WarpTargetName, Existing)) { return Error; }
    const FRotator PreviousRotation = Existing->Rotation;

    Actor->Modify();
    Comp->Modify();
    Comp->AddOrUpdateWarpTargetFromLocationAndRotation(FName(*WarpTargetName), Existing->Location, NewRotation);
    Actor->MarkPackageDirty();

    TSharedPtr<FJsonObject> Obj = MW_StartTargetReply(Actor, WarpTargetName);
    Obj->SetNumberField(TEXT("previous_pitch"), PreviousRotation.Pitch);
    Obj->SetNumberField(TEXT("previous_yaw"), PreviousRotation.Yaw);
    Obj->SetNumberField(TEXT("previous_roll"), PreviousRotation.Roll);
    Obj->SetNumberField(TEXT("new_pitch"), NewRotation.Pitch);
    Obj->SetNumberField(TEXT("new_yaw"), NewRotation.Yaw);
    Obj->SetNumberField(TEXT("new_roll"), NewRotation.Roll);
    Obj->SetBoolField(TEXT("changed"), PreviousRotation != NewRotation);
    return mcp_common::MakeOkReply(Obj);
}

// MCP:set_motion_warping_target_bone_name - Set the BoneName on an existing FMotionWarpingTarget (resolves the existing Component ref).
//   @param actor_name - Engine name of the actor carrying the UMotionWarpingComponent
//   @param warp_target_name - FName of the existing warp target on the component
//   @param bone_name - Skeletal bone on the target's Component to anchor to; NAME_None follows the component root
//   @returns { actor_name, warp_target_name, previous_bone_name: string, new_bone_name: string, changed: bool }
TSharedPtr<FJsonObject> HandleSetMotionWarpingTargetBoneName(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveNamedComponent(Params, Actor, Comp)) { return Error; }

    FString WarpTargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("warp_target_name"), WarpTargetName)) { return Missing; }
    FString NewBoneNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bone_name"), NewBoneNameStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const FName NewBoneName(*NewBoneNameStr);

    const FMotionWarpingTarget* Existing = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_FindComponentWarpTarget(Comp, Actor, WarpTargetName, Existing)) { return Error; }
    const FName PreviousBoneName = Existing->BoneName;

    Actor->Modify();
    Comp->Modify();
    Comp->AddOrUpdateWarpTargetFromComponent(
        FName(*WarpTargetName),
        Existing->Component.Get(),
        NewBoneName,
        Existing->bFollowComponent,
        Existing->LocationOffsetDirection,
        Existing->LocationOffset,
        Existing->RotationOffset);
    Actor->MarkPackageDirty();

    TSharedPtr<FJsonObject> Obj = MW_StartTargetReply(Actor, WarpTargetName);
    Obj->SetStringField(TEXT("previous_bone_name"), PreviousBoneName.ToString());
    Obj->SetStringField(TEXT("new_bone_name"), NewBoneName.ToString());
    Obj->SetBoolField(TEXT("changed"), PreviousBoneName != NewBoneName);
    return mcp_common::MakeOkReply(Obj);
}

// MCP:set_motion_warping_target_follow_component - Set bFollowComponent on an existing component-based warp target of an editor-world actor found by internal name, keeping its Component, BoneName and offsets. A missing target, or one without a Component, is an error. WarpTargets is Transient, so the change is not saved with the level.
//   @param actor_name - Internal object name of the actor with the UMotionWarpingComponent, e.g. "BP_Hero_C_0"; the outliner label does not match.
//   @param warp_target_name - FName of the existing warp target on the component
//   @param follow - When true the target tracks the Component's live transform each frame; when false it snapshots the current transform (required, no default)
//   @returns { actor_name, warp_target_name, previous_follow: bool, new_follow: bool, changed: bool }
TSharedPtr<FJsonObject> HandleSetMotionWarpingTargetFollowComponent(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveNamedComponent(Params, Actor, Comp)) { return Error; }

    FString WarpTargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("warp_target_name"), WarpTargetName)) { return Missing; }
    bool bFollow = false;
    if (!Params->TryGetBoolField(TEXT("follow"), bFollow))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: follow (bool)"));
    }

    const FMotionWarpingTarget* Existing = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_FindComponentWarpTarget(Comp, Actor, WarpTargetName, Existing)) { return Error; }
    const bool Previous = Existing->bFollowComponent;

    Actor->Modify();
    Comp->Modify();
    Comp->AddOrUpdateWarpTargetFromComponent(
        FName(*WarpTargetName),
        Existing->Component.Get(),
        Existing->BoneName,
        bFollow,
        Existing->LocationOffsetDirection,
        Existing->LocationOffset,
        Existing->RotationOffset);
    Actor->MarkPackageDirty();

    TSharedPtr<FJsonObject> Obj = MW_StartTargetReply(Actor, WarpTargetName);
    Obj->SetBoolField(TEXT("previous_follow"), Previous);
    Obj->SetBoolField(TEXT("new_follow"), bFollow);
    Obj->SetBoolField(TEXT("changed"), Previous != bFollow);
    return mcp_common::MakeOkReply(Obj);
}

// MCP:set_motion_warping_target_location_offset - Set LocationOffset (FVector) on an existing FMotionWarpingTarget (keeps BoneName/follow/rotation-offset).
//   @param actor_name - Engine name of the actor carrying the UMotionWarpingComponent
//   @param warp_target_name - FName of the existing warp target on the component
//   @param location_offset - [x, y, z] offset applied to the target relative to its LocationOffsetDirection
//   @returns { actor_name, warp_target_name, previous_x, previous_y, previous_z, new_x, new_y, new_z, changed: bool }
TSharedPtr<FJsonObject> HandleSetMotionWarpingTargetLocationOffset(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveNamedComponent(Params, Actor, Comp)) { return Error; }

    FString WarpTargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("warp_target_name"), WarpTargetName)) { return Missing; }
    const FVector NewOffset = mcp_common::ReadJsonVector(Params, TEXT("location_offset"));

    const FMotionWarpingTarget* Existing = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_FindComponentWarpTarget(Comp, Actor, WarpTargetName, Existing)) { return Error; }
    const FVector PreviousOffset = Existing->LocationOffset;

    Actor->Modify();
    Comp->Modify();
    Comp->AddOrUpdateWarpTargetFromComponent(
        FName(*WarpTargetName),
        Existing->Component.Get(),
        Existing->BoneName,
        Existing->bFollowComponent,
        Existing->LocationOffsetDirection,
        NewOffset,
        Existing->RotationOffset);
    Actor->MarkPackageDirty();

    TSharedPtr<FJsonObject> Obj = MW_StartTargetReply(Actor, WarpTargetName);
    Obj->SetNumberField(TEXT("previous_x"), PreviousOffset.X);
    Obj->SetNumberField(TEXT("previous_y"), PreviousOffset.Y);
    Obj->SetNumberField(TEXT("previous_z"), PreviousOffset.Z);
    Obj->SetNumberField(TEXT("new_x"), NewOffset.X);
    Obj->SetNumberField(TEXT("new_y"), NewOffset.Y);
    Obj->SetNumberField(TEXT("new_z"), NewOffset.Z);
    Obj->SetBoolField(TEXT("changed"), !PreviousOffset.Equals(NewOffset));
    return mcp_common::MakeOkReply(Obj);
}

// MCP:set_motion_warping_component_search_in_anims - Set bSearchForWindowsInAnimsWithinMontages on the UMotionWarpingComponent of an editor-world actor found by internal name (not label): when true, warping windows are also searched in animations inside montages. Marks the level dirty, not saved; returns previous and new values and changed.
//   @param actor_name - Internal object name of the actor with the UMotionWarpingComponent, e.g. "BP_Hero_C_0"; the outliner label does not match.
//   @param enabled - Sets bSearchForWindowsInAnimsWithinMontages: when true, motion-warping windows are also searched in animations nested inside montages (required, no default)
//   @returns { actor_name, previous_search_in_anims: bool, new_search_in_anims: bool, changed: bool }
TSharedPtr<FJsonObject> HandleSetMotionWarpingComponentSearchInAnims(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveNamedComponent(Params, Actor, Comp)) { return Error; }

    bool bEnabled = false;
    if (!Params->TryGetBoolField(TEXT("enabled"), bEnabled))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: enabled (bool)"));
    }
    const bool Previous = Comp->bSearchForWindowsInAnimsWithinMontages;

    Actor->Modify();
    Comp->Modify();
    Comp->bSearchForWindowsInAnimsWithinMontages = bEnabled;
    Actor->MarkPackageDirty();

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("actor_name"), Actor->GetName());
    Obj->SetBoolField(TEXT("previous_search_in_anims"), Previous);
    Obj->SetBoolField(TEXT("new_search_in_anims"), bEnabled);
    Obj->SetBoolField(TEXT("changed"), Previous != bEnabled);
    return mcp_common::MakeOkReply(Obj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_motion_warping_component"), &HandleAddMotionWarpingComponent },
    { TEXT("add_warp_target_to_component"), &HandleAddWarpTargetToComponent },
    { TEXT("clear_motion_warping_targets"), &HandleClearMotionWarpingTargets },
    { TEXT("inspect_motion_warping_component"), &HandleInspectMotionWarpingComponent },
    { TEXT("list_warp_targets_on_component"), &HandleListWarpTargetsOnComponent },
    { TEXT("remove_motion_warping_component"), &HandleRemoveMotionWarpingComponent },
    { TEXT("remove_warp_target"), &HandleRemoveWarpTarget },
    { TEXT("set_motion_warping_component_search_in_anims"), &HandleSetMotionWarpingComponentSearchInAnims },
    { TEXT("set_motion_warping_target_bone_name"), &HandleSetMotionWarpingTargetBoneName },
    { TEXT("set_motion_warping_target_follow_component"), &HandleSetMotionWarpingTargetFollowComponent },
    { TEXT("set_motion_warping_target_location"), &HandleSetMotionWarpingTargetLocation },
    { TEXT("set_motion_warping_target_location_offset"), &HandleSetMotionWarpingTargetLocationOffset },
    { TEXT("set_motion_warping_target_rotation"), &HandleSetMotionWarpingTargetRotation },
};
} // namespace UEAgentMotionWarpingCommands
