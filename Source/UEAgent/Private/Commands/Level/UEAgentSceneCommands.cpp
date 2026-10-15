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
#include "Engine/Light.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "EditorBuildUtils.h"
#include "LightingBuildOptions.h"
#include "Misc/ConfigCacheIni.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "UEAgentScene"

/** Scene, actor and lighting commands: actor label, folder, attachment and material, viewport focus,
 *  spawning lights and building lighting. */
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

// MCP:attach_actor - Attach one actor to another optionally at a socket keeping world transform
//   @param child_actor_name - Name or label of the child actor to attach
//   @param parent_actor_name - Name or label of the parent actor to attach to
//   @param socket_name - Optional socket on the parent to attach to
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

    const FScopedTransaction Transaction(LOCTEXT("AttachActor", "MCP: Attach Actor"));
    Child->Modify();
    const bool bAttached = Child->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform,
        SocketName.IsEmpty() ? NAME_None : FName(*SocketName));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("child"), Child->GetName());
    Result->SetStringField(TEXT("parent"), Parent->GetName());
    Result->SetBoolField(TEXT("attached"), bAttached);
    return Result;
}

// MCP:detach_actor - Detach an actor from its current parent keeping world transform
//   @param actor_name - Name or label of the actor to detach from its parent
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

// MCP:set_actor_material - Set a component-level override material on one slot of an editor-world actor (found by name or label): on its first StaticMeshComponent, else its first other mesh component. The mesh asset and other slots are untouched. Errors when the actor, a mesh component or the material is missing. One undo step; the level is marked dirty, not saved. slot_index is not range-checked: past the mesh's slots it has no visible effect, and a negative one does nothing. For the mesh asset use set_static_mesh_material_slot; for a tweakable runtime copy, create_dynamic_material_on_actor.
//   @param actor_name - Name or label of the target actor
//   @param material_path - Full asset path of the UMaterialInterface
//   @param slot_index - Material slot index, default 0
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

    const FScopedTransaction Transaction(LOCTEXT("SetActorMaterial", "MCP: Set Actor Material"));
    MeshComp->Modify();
    MeshComp->SetMaterial(SlotIndex, Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetName());
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetNumberField(TEXT("slot_index"), SlotIndex);
    return Result;
}

// MCP:focus_viewport - Frame the editor viewport on an actor or the current selection
//   @param actor_name - Optional actor to focus on, defaults to current selection
//   @returns { focused_actor: string, focused_count: int }
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

// MCP:add_light - Spawn a Directional, Point, Spot, Rect or Sky light actor in the editor world (not PIE) at location and rotation, optionally setting intensity and color, outside any undo transaction; nothing is saved. Returns light_type and the internal actor_name of the new actor.
//   @param light_type - Directional, Point, Spot, Rect, or Sky (case-insensitive); anything else is an error.
//   @param location - [x, y, z] world location in cm; default [0, 0, 0], also when the array has fewer than 3 numbers.
//   @param rotation - World rotation in degrees as [pitch, yaw, roll], default [0, 0, 0]; directional, spot and rect lights shine along its forward axis.
//   @param intensity - Value for the Intensity property in the units of the light (Directional: lux; Sky: a multiplier); engine default when omitted.
//   @param color - [r, g, b] linear color, usually 0..1; default white. Any other form (an object, a hex string, fewer than 3 numbers) sets black.
//   @returns { light_type: string, actor_name: string }
TSharedPtr<FJsonObject> HandleAddLight(const TSharedPtr<FJsonObject>& Params)
{
    FString LightType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_type"), LightType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    const FRotator Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));

    double Intensity = 0.0;
    const bool bHasIntensity = Params->TryGetNumberField(TEXT("intensity"), Intensity);
    FLinearColor Color = FLinearColor::White;
    bool bHasColor = false;
    if (Params->HasField(TEXT("color")))
    {
        const FVector C = mcp_common::ReadJsonVector(Params, TEXT("color"));
        Color = FLinearColor(C.X, C.Y, C.Z);
        bHasColor = true;
    }

    const FString TypeLower = LightType.ToLower();
    AActor* NewActor = nullptr;
    if (TypeLower == TEXT("directional"))   { NewActor = World->SpawnActor<ADirectionalLight>(Location, Rotation); }
    else if (TypeLower == TEXT("point"))    { NewActor = World->SpawnActor<APointLight>(Location, Rotation); }
    else if (TypeLower == TEXT("spot"))     { NewActor = World->SpawnActor<ASpotLight>(Location, Rotation); }
    else if (TypeLower == TEXT("rect"))     { NewActor = World->SpawnActor<ARectLight>(Location, Rotation); }
    else if (TypeLower == TEXT("sky"))      { NewActor = World->SpawnActor<ASkyLight>(Location, Rotation); }
    else
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown light_type '%s' (Directional/Point/Spot/Rect/Sky)"), *LightType));

    if (!NewActor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn light actor"));

    if (ASkyLight* Sky = Cast<ASkyLight>(NewActor))
    {
        if (USkyLightComponent* C = Sky->GetLightComponent())
        {
            if (bHasIntensity) { C->SetIntensity((float)Intensity); }
            if (bHasColor)     { C->SetLightColor(Color); }
            C->MarkRenderStateDirty();
        }
    }
    else if (ALight* Light = Cast<ALight>(NewActor))
    {
        if (ULightComponent* C = Light->GetLightComponent())
        {
            if (bHasIntensity) { C->SetIntensity((float)Intensity); }
            if (bHasColor)     { C->SetLightColor(Color); }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("light_type"), LightType);
    Result->SetStringField(TEXT("actor_name"), NewActor->GetName());
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
    { TEXT("add_light"), &HandleAddLight },
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
