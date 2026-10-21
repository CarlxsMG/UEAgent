// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Selection.h"
#include "GameFramework/Actor.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "EditorBuildUtils.h"
#include "LightingBuildOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "UEAgentScene"

/** Scene, actor and lighting commands: actor label, folder, attachment and material, viewport focus,
 *  and building lighting. */
namespace UEAgentSceneCommands
{

// MCP-DOMAIN: Level

// Helpers for the outliner commands below.
namespace UEAgentSceneCommandsImpl
{
    // The actor of the editor world with that internal name or outliner label (first match): null once
    // OutActor holds it, otherwise the error reply (also when there is no editor world).
    TSharedPtr<FJsonObject> FindEditorActor(const FString& NameOrLabel, AActor*& OutActor)
    {
        OutActor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), NameOrLabel);
        if (OutActor == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *NameOrLabel));
        }
        return nullptr;
    }

    // Why AActor::AttachToActor refused to attach Child to Parent: the refusals of
    // USceneComponent::AttachToComponent that can be told apart from outside, else a pointer to the editor
    // log, where the engine writes its reason.
    FString AttachRefusalReason(const AActor* Child, const AActor* Parent)
    {
        const USceneComponent* ChildRoot = Child->GetRootComponent();
        const USceneComponent* ParentComponent = Parent->GetDefaultAttachComponent();
        if (ChildRoot == nullptr)
        {
            return TEXT("the child has no root component");
        }
        if (ParentComponent == nullptr)
        {
            return TEXT("the parent has no component to attach to");
        }
        if (Child == Parent || ChildRoot == ParentComponent)
        {
            return TEXT("an actor cannot be attached to itself");
        }
        if (ParentComponent->IsAttachedTo(ChildRoot))
        {
            return TEXT("the parent is attached under the child, so the attachment would form a cycle");
        }
        if (ChildRoot->GetMobility() == EComponentMobility::Static && ParentComponent->GetMobility() != EComponentMobility::Static)
        {
            return TEXT("the child's root component is Static and the parent's is not; a Static component attaches only to a Static parent");
        }
        return TEXT("the engine refused the attachment (its reason is in the editor log)");
    }
}

// MCP:set_actor_label - Set the outliner label of an editor-world actor found by internal name or current label (first match). The engine trims it and keeps the old label when the new one is invalid, for example empty, so check label in the reply. Marks the level dirty without an undo entry.
//   @param actor_name - Current actor name or label in the world outliner
//   @param new_label - Display label to set, e.g. "Lamp_Kitchen"; leading and trailing spaces are trimmed.
//   @returns { actor_name: string, label: string }
TSharedPtr<FJsonObject> HandleSetActorLabel(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, NewLabel;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_label"), NewLabel, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(ActorName, Actor)) { return NotFound; }

    Actor->SetActorLabel(NewLabel, /*bMarkDirty=*/true);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetName());
    Result->SetStringField(TEXT("label"), Actor->GetActorLabel());
    return Result;
}

// MCP:get_actor_label - Read an actor's display label from the level (reader twin of set_actor_label)
//   @param actor_name - Actor name or label in the world outliner
//   @returns { actor_name: string, actor_label: string }
TSharedPtr<FJsonObject> HandleGetActorLabel(const TSharedPtr<FJsonObject>& Params)
{
    FString Lookup;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Lookup, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* Found = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(Lookup, Found)) { return NotFound; }

    // Internal name and display label side by side.
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), Found->GetName());
    Reply->SetStringField(TEXT("actor_label"), Found->GetActorLabel());
    return Reply;
}

// MCP:set_actor_folder - Move an editor-world actor, found by internal name or label (first match), into an outliner folder; an empty folder_path moves it back to the root. Undoable, marks the level dirty, not saved; returns the internal actor_name and folder_path as given. get_actor_folder reads it back.
//   @param actor_name - Current actor name or label in the world outliner
//   @param folder_path - Outliner folder path with "/" between levels, e.g. "Lighting/Sky"; an empty string moves the actor to the root.
//   @returns { actor_name: string, folder_path: string }
TSharedPtr<FJsonObject> HandleSetActorFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString Lookup;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Lookup, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString Folder;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("folder_path"), Folder, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* Found = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(Lookup, Found)) { return NotFound; }

    // Undoable: Modify() records the current folder before it changes.
    const FScopedTransaction Transaction(LOCTEXT("SetActorFolder", "MCP: Set Actor Folder"));
    Found->Modify();
    Found->SetFolderPath(FName(*Folder));

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), Found->GetName());
    Reply->SetStringField(TEXT("folder_path"), Folder);
    return Reply;
}

// MCP:get_actor_folder - Read an actor's outliner folder path (reader twin of set_actor_folder)
//   @param actor_name - Actor name or label in the world outliner
//   @returns { actor_name: string, folder_path: string }
TSharedPtr<FJsonObject> HandleGetActorFolder(const TSharedPtr<FJsonObject>& Params)
{
    FString Lookup;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Lookup, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* Found = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(Lookup, Found)) { return NotFound; }

    // The folder is an FName; an actor at the outliner root reports NAME_None ("None").
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), Found->GetName());
    Reply->SetStringField(TEXT("folder_path"), Found->GetFolderPath().ToString());
    return Reply;
}

// MCP:attach_actor - Attach one editor-world actor to another, both found by internal name or outliner label (first match), optionally at a socket of the parent, keeping the child's world transform, in one undo step; the level is marked dirty, not saved. A child that already has a parent moves to the new one, and repeating the same attachment changes nothing (idempotent). attached is the engine's verdict: an attachment it refuses (to the actor itself, into a cycle, a Static child under a non-Static parent) is an error naming the reason, with attached false and no undo step left. The socket name is not checked against the parent. A missing child or parent is an error. Inverse: detach_actor; for a component inside a Blueprint use attach_blueprint_component; to attach over time in a Level Sequence use add_attach_track.
//   @param child_actor_name - Internal name or outliner label of the child actor to attach (first match in the editor world)
//   @param parent_actor_name - Internal name or outliner label of the parent actor to attach to (first match in the editor world)
//   @param socket_name - Optional socket name on the parent, not checked; omitted or empty attaches without a socket
//   @returns { child: string, parent: string, attached: bool }
TSharedPtr<FJsonObject> HandleAttachActor(const TSharedPtr<FJsonObject>& Params)
{
    FString ChildName, ParentName, SocketName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("child_actor_name"), ChildName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_actor_name"), ParentName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("socket_name"), SocketName);

    UWorld* World = mcp_common::GetEditorWorld();
    AActor* Child = mcp_common::FindActorByName(World, ChildName);
    AActor* Parent = mcp_common::FindActorByName(World, ParentName);
    if (!Child)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Child actor not found: %s"), *ChildName));
    if (!Parent)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent actor not found: %s"), *ParentName));

    UPackage* ChildPackage = Child->GetPackage();
    const bool bChildPackageWasDirty = ChildPackage != nullptr && ChildPackage->IsDirty();

    const FScopedTransaction Transaction(LOCTEXT("AttachActor", "MCP: Attach Actor"));
    Child->Modify();
    const bool bAttached = Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform,
        SocketName.IsEmpty() ? NAME_None : FName(*SocketName));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("child"), Child->GetName());
    Result->SetStringField(TEXT("parent"), Parent->GetName());
    Result->SetBoolField(TEXT("attached"), bAttached);
    if (!bAttached)
    {
        // The engine refuses before it changes anything, so only Modify() touched the child: put its package's
        // dirty flag back and drop the call's undo step, which would undo nothing.
        if (ChildPackage != nullptr)
        {
            ChildPackage->SetDirtyFlag(bChildPackageWasDirty);
        }
        mcp_common::DiscardUndoStepOfThisCall();
        return mcp_common::MakeOutcomeReply(Result, false, FString::Printf(TEXT("attach_actor: %s was not attached to %s: %s"),
            *Child->GetName(), *Parent->GetName(), *UEAgentSceneCommandsImpl::AttachRefusalReason(Child, Parent)));
    }
    return Result;
}

// MCP:detach_actor - Detach an editor-world actor from the actor it is attached to, keeping its world transform (it does not move), in one undo step; the level is marked dirty, not saved (save_level keeps it). Its own attached children stay with it. The inverse of attach_actor, which also moves an actor to a new parent directly, so detaching first is not needed for that. Idempotent: an actor with no parent is a success with was_attached=false and detached=false, and nothing changes. detached is read back after the call (former_parent names the old parent), so was_attached=true with detached=false means the engine kept the attachment, still answered as a success. An actor that is not found is an error.
//   @param actor_name - Internal name or outliner label of the editor-world actor, e.g. "SM_Lamp_3" or "Lamp"; get_actors_in_level or find_actors_by_name list them.
//   @returns { actor: string, was_attached: bool, detached: bool, former_parent: string }
TSharedPtr<FJsonObject> HandleDetachActor(const TSharedPtr<FJsonObject>& Params)
{
    FString Lookup;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Lookup, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(Lookup, Actor)) { return NotFound; }

    // Read the current parent BEFORE mutating so we can (a) short-circuit the no-op
    // idempotent case and (b) report the former parent's name.
    AActor* Parent = Actor->GetAttachParentActor();
    const bool bWasAttached = (Parent != nullptr);
    const FString FormerParent = bWasAttached ? Parent->GetName() : FString();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor"), Actor->GetName());
    Result->SetStringField(TEXT("former_parent"), FormerParent);

    if (!bWasAttached)
    {
        // Idempotent no-op: nothing to detach, do NOT open a transaction or call Detach.
        UE_LOG(LogTemp, Verbose, TEXT("[MCP:detach_actor] '%s' has no attach parent; no-op (idempotent)"),
            *Actor->GetName());
        Result->SetBoolField(TEXT("was_attached"), false);
        Result->SetBoolField(TEXT("detached"), false);
        return Result;
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP:detach_actor] Detaching '%s' from parent '%s' (KeepWorldTransform)"),
        *Actor->GetName(), *FormerParent);

    const FScopedTransaction Transaction(LOCTEXT("DetachActor", "MCP: Detach Actor"));
    Actor->Modify();
    Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);

    // Read back to confirm the detach actually took effect.
    const bool bDetached = (Actor->GetAttachParentActor() == nullptr);
    UE_LOG(LogTemp, Verbose, TEXT("[MCP:detach_actor] '%s' detached=%s"),
        *Actor->GetName(), bDetached ? TEXT("true") : TEXT("false"));

    Result->SetBoolField(TEXT("was_attached"), true);
    Result->SetBoolField(TEXT("detached"), bDetached);
    return Result;
}

// MCP:set_actor_material - Set a component-level override material on one slot of an editor-world actor (found by name or label): on its first StaticMeshComponent, else its first other mesh component. The mesh asset and other slots are untouched. Errors when the actor, a mesh component or the material is missing. One undo step; the level is marked dirty, not saved. A slot_index outside the component's material slots is refused. For the mesh asset use set_static_mesh_material_slot; for a tweakable runtime copy, create_dynamic_material_on_actor.
//   @param actor_name - Name or label of the target actor
//   @param material_path - Full asset path of the UMaterialInterface
//   @param slot_index - Material slot index, from 0 to the component's slot count minus 1, default 0
//   @returns { actor_name: string, material: string, slot_index: int }
TSharedPtr<FJsonObject> HandleSetActorMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString Lookup;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), Lookup, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Material slot on the mesh component; the first slot unless told otherwise.
    int32 SlotIndex = 0;
    Params->TryGetNumberField(TEXT("slot_index"), SlotIndex);

    AActor* Actor = nullptr;
    if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(Lookup, Actor)) { return NotFound; }

    UMeshComponent* MeshComp = Actor->FindComponentByClass<UStaticMeshComponent>();
    if (!MeshComp)
    {
        MeshComp = Actor->FindComponentByClass<UMeshComponent>();
    }
    if (!MeshComp)
        return mcp_common::MakeErrorReply(TEXT("Actor has no mesh component"));

    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Material)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));

    // SetMaterial grows the override array for an index past the slots (an override no slot
    // renders) and ignores a negative one, so both are refused before the transaction opens.
    const int32 NumSlots = MeshComp->GetNumMaterials();
    if (SlotIndex < 0 || SlotIndex >= NumSlots)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("slot_index %d out of range (component %s has %d material slots)"),
            SlotIndex, *MeshComp->GetName(), NumSlots));

    const FScopedTransaction Transaction(LOCTEXT("SetActorMaterial", "MCP: Set Actor Material"));
    MeshComp->Modify();
    MeshComp->SetMaterial(SlotIndex, Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetName());
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetNumberField(TEXT("slot_index"), SlotIndex);
    return Result;
}

// MCP:focus_viewport - Move the camera of every level editor viewport to frame one actor of the editor world, found by internal name or outliner label (first match), or, when actor_name is omitted, the actors currently selected. Only the viewport cameras change: nothing in the level, no undo entry, nothing saved, and the selection stays as it is. An actor_name that matches nothing, or no actor_name with an empty selection, is an error. The reply has focused_actor (the internal name) in actor mode or focused_count in selection mode, never both. To place the camera at an exact location and rotation use set_viewport_settings, read it back with get_viewport_state, and select actors first with select_actors.
//   @param actor_name - Optional internal name or outliner label of the actor to frame; omitted, the current selection is framed
//   @returns { focused_actor?: string, focused_count?: int }
//   @hints additive idempotent
TSharedPtr<FJsonObject> HandleFocusViewport(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
        return mcp_common::MakeErrorReply(TEXT("GEditor unavailable"));

    FString ActorName;
    if (Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        AActor* Actor = nullptr;
        if (TSharedPtr<FJsonObject> NotFound = UEAgentSceneCommandsImpl::FindEditorActor(ActorName, Actor)) { return NotFound; }
        GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly=*/false);

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("focused_actor"), Actor->GetName());
        return Result;
    }

    // No actor: focus on the current selection.
    TArray<AActor*> Selected;
    if (USelection* Selection = GEditor->GetSelectedActors())
    {
        for (FSelectionIterator It(*Selection); It; ++It)
        {
            if (AActor* Actor = Cast<AActor>(*It))
            {
                Selected.Add(Actor);
            }
        }
    }
    if (Selected.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("No actor_name given and nothing is selected"));
    GEditor->MoveViewportCamerasToActor(Selected, /*bActiveViewportOnly=*/false);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("focused_count"), Selected.Num());
    return Result;
}

// MCP:build_lighting - Start an asynchronous static lighting build of all levels in the editor world (not only the current one) and return started without waiting for the result. Side effect: writes the quality and OnlyBuildCurrentLevel=false into LightingBuildOptions of the editor per-project settings.
//   @param quality - Preview, Medium, High or Production (case-insensitive); any other value silently builds at Preview. Default Preview.
//   @returns { quality: string, started: bool, note: string }
TSharedPtr<FJsonObject> HandleBuildLighting(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    FString Quality = TEXT("Preview");
    Params->TryGetStringField(TEXT("quality"), Quality);
    const FString QLower = Quality.ToLower();
    int32 QualityLevel = Quality_Preview;
    if (QLower == TEXT("medium"))          { QualityLevel = Quality_Medium; }
    else if (QLower == TEXT("high"))       { QualityLevel = Quality_High; }
    else if (QLower == TEXT("production")) { QualityLevel = Quality_Production; }

    // FEditorBuildUtils::EditorBuild reads the quality from this INI key.
    GConfig->SetInt(TEXT("LightingBuildOptions"), TEXT("QualityLevel"), QualityLevel, GEditorPerProjectIni);
    GConfig->SetBool(TEXT("LightingBuildOptions"), TEXT("OnlyBuildCurrentLevel"), false, GEditorPerProjectIni);

    const bool bSuccess = FEditorBuildUtils::EditorBuild(World, FBuildOptions::BuildLighting, /*bAllowLightingDialog=*/false);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("quality"), Quality);
    Result->SetBoolField(TEXT("started"), bSuccess);
    Result->SetStringField(TEXT("note"), TEXT("Lighting builds run asynchronously; this returns once the build is kicked off."));
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("attach_actor"), &HandleAttachActor },
    { TEXT("build_lighting"), &HandleBuildLighting },
    { TEXT("detach_actor"), &HandleDetachActor },
    { TEXT("focus_viewport"), &HandleFocusViewport },
    { TEXT("get_actor_folder"), &HandleGetActorFolder },
    { TEXT("get_actor_label"), &HandleGetActorLabel },
    { TEXT("set_actor_folder"), &HandleSetActorFolder },
    { TEXT("set_actor_label"), &HandleSetActorLabel },
    { TEXT("set_actor_material"), &HandleSetActorMaterial },
};
} // namespace UEAgentSceneCommands

#undef LOCTEXT_NAMESPACE
