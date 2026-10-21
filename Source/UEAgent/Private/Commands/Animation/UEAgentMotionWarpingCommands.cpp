// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "ScopedTransaction.h"
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
     * Reads "actor_path", or its alias "actor_name" (the internal object name the typed setters take; MW_ResolveActor
     * finds an actor by object name or label either way). Null when one is there, else the error reply.
     */
    TSharedPtr<FJsonObject> MW_ReadActorPathOrName(const TSharedPtr<FJsonObject>& Params, FString& OutActorPath)
    {
        if (!Params->TryGetStringField(TEXT("actor_path"), OutActorPath) && !Params->TryGetStringField(TEXT("actor_name"), OutActorPath))
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'actor_path' parameter (actor_name is accepted as an alias)"));
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
     * to add_motion_warping_target. Returns the first match.
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

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    /**
     * UE 5.7 has no UMotionWarpingComponent::GetWarpTargets(): the protected `WarpTargets` array it returns in 5.8,
     * read through its reflected property (FindFProperty bypasses C++ access control). Empty when the property
     * cannot be resolved.
     */
    TConstArrayView<FMotionWarpingTarget> MW_GetWarpTargets(const UMotionWarpingComponent* Comp)
    {
        const FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(
            UMotionWarpingComponent::StaticClass(), TEXT("WarpTargets"));
        if (!Comp || !ArrayProp)
        {
            return TConstArrayView<FMotionWarpingTarget>();
        }
        return *ArrayProp->ContainerPtrToValuePtr<TArray<FMotionWarpingTarget>>(Comp);
    }
#endif

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
// MCP:add_motion_warping_component - Attach a UMotionWarpingComponent named MotionWarping to an editor-world actor given by full object path, internal name or label. add_component_to_actor (component_type MotionWarpingComponent, idempotent=true) does the same for an actor's internal name only; this tool adds the path/label lookup. Idempotent: if the actor already has one, it is returned with already_existed:true and nothing is written. A new one is an instance component (save_level keeps it, and construction-script reruns do not drop it) added in one undo step; the actor's package is marked dirty, not saved. When another object of the actor already holds the name MotionWarping, the new component gets a unique variant, returned in component_name. Add targets with add_motion_warping_target, read them with get_motion_warping_component_info.
//   @param actor_path [string!] - Full editor path (e.g. "/Game/Maps/Test.Test:PersistentLevel.ActorName"), internal name or label of the actor.
//   @returns { actor_path: string, component_name: string, component_path: string, already_existed: bool }

// ---------------------------------------------------------------------------
// add_motion_warping_target
// ---------------------------------------------------------------------------
// MCP:add_motion_warping_target - Add (or update in-place) a FMotionWarpingTarget on an existing UMotionWarpingComponent, including moving or re-aiming an existing target. Exactly one of four payload shapes must be supplied: (1) component_path -> AddOrUpdateWarpTargetFromComponent(name, comp, NAME_None, false, TargetsForwardVector, ZeroVector, ZeroRotator); (2) location + rotation -> AddOrUpdateWarpTargetFromLocationAndRotation; (3) location only -> AddOrUpdateWarpTargetFromLocation (rotation [0,0,0]); (4) rotation only, on a target that already exists -> its Location is kept and only the rotation changes (a missing target is an error). Shapes 2-4 rebuild the target as a plain transform target, so a component-based one loses its Component, BoneName, follow flag and offsets. The engine API is idempotent by WarpTargetName (replace-in-place). Marks the owning level dirty; WarpTargets is Transient, so the targets are not saved with the level.
//   @param actor_path [string!] - Full editor path or short name of the actor. actor_name (the internal object name) is accepted as an alias.
//   @param target_name [string!] - FName identifier for the warp target. warp_target_name is accepted as an alias.
//   @param location - Optional world [x, y, z] location for the target.
//   @param rotation - Optional world [pitch, yaw, roll] rotation in degrees for the target; alone, it re-aims an existing target and keeps its location.
//   @param component_path [string] - Optional name of a USceneComponent on the actor to drive the target transform.
//   @returns { actor_path: string, target_name: string, warp_target_count: int, previous_count: int, source: string (component | location_and_rotation | location | rotation), already_existed: bool, previous_location?: [x,y,z], previous_rotation?: [pitch,yaw,roll], location: [x,y,z], rotation: [pitch,yaw,roll], changed: bool }

// ---------------------------------------------------------------------------
// remove_motion_warping_target
// ---------------------------------------------------------------------------
// MCP:remove_motion_warping_target - Remove a FMotionWarpingTarget from a UMotionWarpingComponent by FName, or with all=true every warp target at once. Wraps UMotionWarpingComponent::RemoveWarpTarget(name) (0 or 1 removed; the array is keyed by FName), looped over a snapshot of the names for all=true. Idempotent: removing a non-existent target, or clearing an empty component, returns removed_count 0 without raising an error. Pass target_name or all=true, not both.
//   @param actor_path [string!] - Full editor path or short name of the actor. actor_name (the internal object name) is accepted as an alias.
//   @param target_name - FName identifier of the warp target to remove. Required unless all is true.
//   @param all - Optional bool (default false). When true, remove every warp target of the component; target_name must then be omitted.
//   @returns { actor_path: string, target_name?: string, all: bool, removed_count: int, removed_names: [string], previous_count: int, warp_target_count: int, cleared: bool }

// ---------------------------------------------------------------------------
// get_motion_warping_component_info
// ---------------------------------------------------------------------------
// MCP:get_motion_warping_component_info - Read the UMotionWarpingComponent of an actor without changing anything: whether warp windows are also searched in animations inside montages, every warp target (name, location, rotation, the component it follows or null, bone name, follow flag, location and rotation offsets, and location_offset_direction as the enum's integer), the modifier count, the owner adapter class (None when unset) and the number of switch-off conditions (a protected array read by reflection; -1 if the engine renames it). Warp targets are transient, so on an editor actor they are the ones added this session with add_motion_warping_target. Change a target with set_motion_warping_target, take one out with remove_motion_warping_target, and flip the montage search with set_properties on "<actor>.<component name>" (property bSearchForWindowsInAnimsWithinMontages). An actor that is not found, or one without a UMotionWarpingComponent (add_motion_warping_component adds it), is an error. For a generic component property dump use get_component_properties.
//   @renamed-from inspect_motion_warping_component
//   @param actor_path [string!] - Actor as a full object path ('/Game/Maps/Arena.Arena:PersistentLevel.BP_Hero_C_1'), or the internal name or label of an editor-world actor ('BP_Hero_C_1', 'Hero').
//   @returns { actor_path: string, b_search_for_windows_in_anims_within_montages: bool, warp_target_count: int, modifier_count: int, owner_adapter_class: string, switch_off_condition_count: int, warp_targets: array }

TSharedPtr<FJsonObject> HandleAddMotionWarpingComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPath(Params, ActorPath)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActor(ActorPath, Actor)) { return Error; }

    // Idempotency check: if the actor already has a UMotionWarpingComponent,
    // return it with already_existed:true (idempotent authoring); nothing is written.
    if (UMotionWarpingComponent* Existing = MW_FindMotionWarpingComponent(Actor))
    {
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_motion_warping_component: actor '%s' already has MotionWarping at '%s'"),
            *Actor->GetName(), *Existing->GetPathName());

        TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
        Data->SetStringField(TEXT("component_name"), Existing->GetName());
        Data->SetStringField(TEXT("component_path"), Existing->GetPathName());
        Data->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(Data);
    }

    // One undo step: Modify() records the actor (and marks its package dirty) before the component joins it.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddMotionWarpingComponent", "Add Motion Warping Component (MCP)"));
    Actor->Modify();

    // NewObject with a name another object of the actor already holds is fatal when that object has another class
    // ("Cannot replace existing object of a different class"), and replaces it in place when it is a motion warping
    // component (a removed one still waiting for GC). Any holder counts, garbage included, so a taken name gets a
    // unique variant, returned in component_name.
    FName NewName(TEXT("MotionWarping"));
    if (StaticFindObjectFast(nullptr, Actor, NewName) != nullptr)
    {
        NewName = MakeUniqueObjectName(Actor, UMotionWarpingComponent::StaticClass(), NewName);
    }
    UMotionWarpingComponent* NewComp = NewObject<UMotionWarpingComponent>(
        Actor, UMotionWarpingComponent::StaticClass(), NewName, RF_Transactional);
    if (!NewComp)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create UMotionWarpingComponent for actor '%s'"), *Actor->GetName()));
    }

    // An instance component, as the editor's Add Component makes one: the level saves it with the actor, and a
    // construction-script rerun keeps it instead of throwing it away.
    NewComp->CreationMethod = EComponentCreationMethod::Instance;
    Actor->AddInstanceComponent(NewComp);
    NewComp->RegisterComponent();

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_motion_warping_component: created %s at '%s' on actor '%s'"),
        *NewComp->GetName(), *NewComp->GetPathName(), *Actor->GetName());

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("component_name"), NewComp->GetName());
    Data->SetStringField(TEXT("component_path"), NewComp->GetPathName());
    Data->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeOkReply(Data);
}

TSharedPtr<FJsonObject> HandleAddWarpTargetToComponent(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPathOrName(Params, ActorPath)) { return Missing; }

    // target_name, else its alias warp_target_name.
    FString TargetNameStr;
    if (!Params->TryGetStringField(TEXT("target_name"), TargetNameStr) && !Params->TryGetStringField(TEXT("warp_target_name"), TargetNameStr))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: target_name (warp_target_name is accepted as an alias)"));
    }
    const FName TargetName(*TargetNameStr);

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActorComponent(ActorPath, Actor, Comp)) { return Error; }

    FString ComponentPath;
    const bool bHasComponentPath = Params->TryGetStringField(TEXT("component_path"), ComponentPath) && !ComponentPath.IsEmpty();
    const bool bHasLocation = Params->HasField(TEXT("location"));
    const bool bHasRotation = Params->HasField(TEXT("rotation"));

    // The target as it was, for previous_* and changed (a copy: the write below may reallocate the array).
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    const int32 PreviousCount = MW_GetWarpTargets(Comp).Num();
#else
    const int32 PreviousCount = Comp->GetWarpTargets().Num();
#endif
    TOptional<FMotionWarpingTarget> Before;
    if (const FMotionWarpingTarget* Found = Comp->FindWarpTarget(TargetName))
    {
        Before = *Found;
    }

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
    else if (bHasRotation)
    {
        // (4) Rotation only: re-aim an existing target, keeping its Location.
        if (!Before.IsSet())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Warp target '%s' not found on actor '%s': rotation alone re-aims an existing target; pass 'location' (with or without 'rotation') to create one."),
                *TargetNameStr, *Actor->GetName()));
        }
        const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
        bAlreadyExisted = true;
        Comp->AddOrUpdateWarpTargetFromLocationAndRotation(TargetName, Before->Location, Rotation);
        Source = TEXT("rotation");
    }
    else
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide 'component_path' or 'location' (optionally with 'rotation'), or 'rotation' alone for an existing target."));
    }

    if (ULevel* Level = Actor->GetLevel())
    {
        Level->MarkPackageDirty();
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] add_motion_warping_target: actor='%s' target='%s' source=%s already_existed=%s"),
        *Actor->GetName(), *TargetNameStr, *Source, bAlreadyExisted ? TEXT("true") : TEXT("false"));

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("target_name"), TargetNameStr);
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    Data->SetNumberField(TEXT("warp_target_count"), MW_GetWarpTargets(Comp).Num());
#else
    Data->SetNumberField(TEXT("warp_target_count"), Comp->GetWarpTargets().Num());
#endif
    Data->SetNumberField(TEXT("previous_count"), PreviousCount);
    Data->SetStringField(TEXT("source"), Source);
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);

    // Read the target back: its stored location/rotation, and whether the call changed anything.
    bool bChanged = !Before.IsSet();
    if (Before.IsSet())
    {
        Data->SetArrayField(TEXT("previous_location"), mcp_common::VectorToJsonArray(Before->Location));
        Data->SetArrayField(TEXT("previous_rotation"), mcp_common::RotatorToJsonArray(Before->Rotation));
    }
    if (const FMotionWarpingTarget* After = Comp->FindWarpTarget(TargetName))
    {
        Data->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(After->Location));
        Data->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(After->Rotation));
        if (Before.IsSet())
        {
            bChanged = !Before->Location.Equals(After->Location) || !Before->Rotation.Equals(After->Rotation)
                || Before->Component != After->Component || Before->BoneName != After->BoneName
                || Before->bFollowComponent != After->bFollowComponent;
        }
    }
    Data->SetBoolField(TEXT("changed"), bChanged);
    return mcp_common::MakeOkReply(Data);
}

TSharedPtr<FJsonObject> HandleRemoveWarpTarget(
    const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = MW_ReadActorPathOrName(Params, ActorPath)) { return Missing; }

    // target_name names one target; all=true removes every one. Exactly one of the two.
    bool bAll = false;
    Params->TryGetBoolField(TEXT("all"), bAll);
    FString TargetNameStr;
    Params->TryGetStringField(TEXT("target_name"), TargetNameStr);
    if (bAll && !TargetNameStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Pass target_name or all=true, not both."));
    }
    if (!bAll && TargetNameStr.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: target_name (or all=true to remove every warp target)"));
    }

    AActor* Actor = nullptr;
    UMotionWarpingComponent* Comp = nullptr;
    if (TSharedPtr<FJsonObject> Error = MW_ResolveActorComponent(ActorPath, Actor, Comp)) { return Error; }

    // The engine removes one FName at a time: snapshot the names to remove first.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    const int32 PreviousCount = MW_GetWarpTargets(Comp).Num();
#else
    const int32 PreviousCount = Comp->GetWarpTargets().Num();
#endif
    TArray<FName> ToRemove;
    if (bAll)
    {
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        for (const FMotionWarpingTarget& Existing : MW_GetWarpTargets(Comp))
#else
        for (const FMotionWarpingTarget& Existing : Comp->GetWarpTargets())
#endif
        {
            ToRemove.Add(Existing.Name);
        }
    }
    else if (Comp->FindWarpTarget(FName(*TargetNameStr)) != nullptr)
    {
        ToRemove.Add(FName(*TargetNameStr));
    }

    TArray<FString> RemovedNames;
    if (ToRemove.Num() > 0)
    {
        Comp->Modify();
        for (const FName& Name : ToRemove)
        {
            if (Comp->RemoveWarpTarget(Name) > 0)
            {
                RemovedNames.Add(Name.ToString());
            }
        }
        if (ULevel* Level = Actor->GetLevel())
        {
            Level->MarkPackageDirty();
        }
    }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_motion_warping_target: actor='%s' target='%s' all=%s removed=%d remaining=%d"),
        *Actor->GetName(), *TargetNameStr, bAll ? TEXT("true") : TEXT("false"), RemovedNames.Num(), MW_GetWarpTargets(Comp).Num());
#else
    UE_LOG(LogTemp, Verbose,
        TEXT("[MCP] remove_motion_warping_target: actor='%s' target='%s' all=%s removed=%d remaining=%d"),
        *Actor->GetName(), *TargetNameStr, bAll ? TEXT("true") : TEXT("false"), RemovedNames.Num(), Comp->GetWarpTargets().Num());
#endif

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    if (!bAll)
    {
        Data->SetStringField(TEXT("target_name"), TargetNameStr);
    }
    Data->SetBoolField(TEXT("all"), bAll);
    Data->SetNumberField(TEXT("removed_count"), RemovedNames.Num());
    Data->SetArrayField(TEXT("removed_names"), mcp_common::StringsToJson(RemovedNames));
    Data->SetNumberField(TEXT("previous_count"), PreviousCount);
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    Data->SetNumberField(TEXT("warp_target_count"), MW_GetWarpTargets(Comp).Num());
#else
    Data->SetNumberField(TEXT("warp_target_count"), Comp->GetWarpTargets().Num());
#endif
    Data->SetBoolField(TEXT("cleared"), bAll && RemovedNames.Num() > 0);
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

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    const TConstArrayView<FMotionWarpingTarget> Targets = MW_GetWarpTargets(Comp);
#else
    const TConstArrayView<FMotionWarpingTarget> Targets = Comp->GetWarpTargets();
#endif
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
        TEXT("[MCP] get_motion_warping_component_info: actor='%s' warp_targets=%d modifiers=%d switch_off=%d adapter=%s"),
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
// remove_motion_warping_component
// ---------------------------------------------------------------------------
// MCP:remove_motion_warping_component - Destroy the UMotionWarpingComponent of an actor (the first one found), with all its warp targets; the inverse of add_motion_warping_component. The actor is recorded in the call's undo step (Actor->Modify) and its level is marked dirty, not saved (save_level persists it). Only an instance component stays gone: one that the actor's Blueprint or C++ class defines comes back (when its construction script reruns, or on reload), so remove that one from the class instead. To clear targets but keep the component use remove_motion_warping_target (all=true); check the result with get_motion_warping_component_info. removed is read back from the actor: true once the component is off its component list and being destroyed, and a component still there makes success false. Idempotent: an actor without one returns already_existed=false, removed=false and a note, not an error. An actor that is not found is an error and nothing changes.
//   @param actor_path [string!] - Actor as a full object path ('/Game/Maps/Arena.Arena:PersistentLevel.BP_Hero_C_1'), or the internal name or label of an editor-world actor ('BP_Hero_C_1', 'Hero').
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
    // UActorComponent::DestroyComponent returns void, so the outcome is read back from the actor: the component
    // must be off its component list and marked for destruction.
    Existing->DestroyComponent();
    TInlineComponentArray<UActorComponent*> Remaining(Actor);
    const bool bDestroyed = !Remaining.Contains(Existing) && Existing->IsBeingDestroyed();

    if (ULevel* Level = Actor->GetLevel())
    {
        Level->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Data = MW_StartReply(Actor);
    Data->SetStringField(TEXT("component_path"), ComponentPath);
    Data->SetBoolField(TEXT("already_existed"), true);
    Data->SetBoolField(TEXT("removed"), bDestroyed);
    if (!bDestroyed)
    {
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("remove_motion_warping_component: %s is still a component of the actor after DestroyComponent; it was not removed."),
            *ComponentPath));
    }
    return mcp_common::MakeOkReply(Data);
}// ---------------------------------------------------------------------------
// Typed setters on FMotionWarpingTarget fields and the
// bSearchForWindowsInAnimsWithinMontages component-level toggle.
//
// Pattern: the actor is found by internal object name (MW_ResolveNamedComponent below).
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
                TEXT("FMotionWarpingTarget has no Component ref -- create the target via add_motion_warping_target first"));
        }
        return nullptr;
    }
}

// Each typed setter changes ONE property in place (no removal + re-add), so the gameplay layer keeps the same
// FMotionWarpingTarget instance with the same RPC/replication identity. The component-ref based setters read
// the existing component from the BlueprintReadOnly TWeakObjectPtr stored on the target itself.

// MCP:set_motion_warping_target_bone_name - Set the BoneName on an existing FMotionWarpingTarget (resolves the existing Component ref).
//   @retired set_motion_warping_target - bone_name
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
//   @retired set_motion_warping_target - follow_component
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
//   @retired set_motion_warping_target - location_offset
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
//   @retired set_properties - object_path="<actor>.MotionWarpingComponent", properties={"bSearchForWindowsInAnimsWithinMontages":..}
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

// MCP:set_motion_warping_target - Change any fields of an existing warp target of an editor-world actor's UMotionWarpingComponent in one call -- its bone name, whether it follows its component, its location offset -- keeping the others. Pass only the fields to change; each is applied by its own step and read back, and the call stops at the first one refused (a missing target, follow without a component), with applied listing those already changed. WarpTargets is transient: the change lives in this editor session. Add one: add_motion_warping_target; read them: get_motion_warping_component_info.
//   @param actor_name - Engine name of the actor carrying the UMotionWarpingComponent
//   @param warp_target_name - Name of the existing warp target on the component
//   @param bone_name - Optional bone (or socket) of the target's component the target follows
//   @param follow_component [bool] - Optional: whether the target follows its component
//   @param location_offset [array] - Optional [x, y, z] offset applied to the target
//   @returns { actor_name: string, warp_target_name: string, applied: { field: that field's reply }, failed?: { label, command, error } (when a field is refused) }
TSharedPtr<FJsonObject> HandleSetMotionWarpingTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }
    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("warp_target_name"), TargetName)) { return Missing; }
    auto Target = [&ActorName, &TargetName]()
    {
        TSharedPtr<FJsonObject> StepParams = MakeShared<FJsonObject>();
        StepParams->SetStringField(TEXT("actor_name"), ActorName);
        StepParams->SetStringField(TEXT("warp_target_name"), TargetName);
        return StepParams;
    };
    TArray<mcp_common::FSubCommand> Steps;
    FString BoneName;
    if (Params->TryGetStringField(TEXT("bone_name"), BoneName))
    {
        TSharedPtr<FJsonObject> Bone = Target();
        Bone->SetStringField(TEXT("bone_name"), BoneName);
        Steps.Add({ TEXT("bone_name"), TEXT("set_motion_warping_target_bone_name"), Bone });
    }
    bool bFollow = false;
    if (Params->TryGetBoolField(TEXT("follow_component"), bFollow))
    {
        TSharedPtr<FJsonObject> Follow = Target();
        Follow->SetBoolField(TEXT("follow"), bFollow);
        Steps.Add({ TEXT("follow_component"), TEXT("set_motion_warping_target_follow_component"), Follow });
    }
    const TArray<TSharedPtr<FJsonValue>>* Offset = nullptr;
    if (Params->TryGetArrayField(TEXT("location_offset"), Offset) && Offset)
    {
        TSharedPtr<FJsonObject> OffsetParams = Target();
        OffsetParams->SetArrayField(TEXT("location_offset"), *Offset);
        Steps.Add({ TEXT("location_offset"), TEXT("set_motion_warping_target_location_offset"), OffsetParams });
    }
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), ActorName);
    Reply->SetStringField(TEXT("warp_target_name"), TargetName);
    return mcp_common::RunSubCommands(TEXT("set_motion_warping_target"), Steps, Reply);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("set_motion_warping_target"), &HandleSetMotionWarpingTarget },
    { TEXT("add_motion_warping_component"), &HandleAddMotionWarpingComponent },
    { TEXT("add_motion_warping_target"), &HandleAddWarpTargetToComponent },
    { TEXT("get_motion_warping_component_info"), &HandleInspectMotionWarpingComponent },
    { TEXT("remove_motion_warping_component"), &HandleRemoveMotionWarpingComponent },
    { TEXT("remove_motion_warping_target"), &HandleRemoveWarpTarget },
    { TEXT("set_motion_warping_component_search_in_anims"), &HandleSetMotionWarpingComponentSearchInAnims },
    { TEXT("set_motion_warping_target_bone_name"), &HandleSetMotionWarpingTargetBoneName },
    { TEXT("set_motion_warping_target_follow_component"), &HandleSetMotionWarpingTargetFollowComponent },
    { TEXT("set_motion_warping_target_location_offset"), &HandleSetMotionWarpingTargetLocationOffset },
};
} // namespace UEAgentMotionWarpingCommands
