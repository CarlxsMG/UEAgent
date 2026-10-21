// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Level/PrimitiveComponentUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"

// Text-render / decal wrapper actors + their components (Engine module).
#include "Engine/TextRenderActor.h"          // ATextRenderActor::GetTextRender()
#include "Components/TextRenderComponent.h"   // UTextRenderComponent::SetText/SetTextRenderColor/SetWorldSize
#include "Engine/DecalActor.h"                // ADecalActor::GetDecal()
#include "Components/DecalComponent.h"        // UDecalComponent::SetDecalMaterial / DecalSize
#include "Materials/MaterialInterface.h"

// Spline + spline-mesh components (Engine module).
#include "Components/SplineComponent.h"       // USplineComponent + ESplineCoordinateSpace
#include "Components/SplineMeshComponent.h"   // USplineMeshComponent::SetStartAndEnd / SetStaticMesh
#include "Engine/StaticMesh.h"

// set_actor_component_collision: primitive-component collision on a placed actor.
#include "Components/PrimitiveComponent.h"    // UPrimitiveComponent::SetCollision{ProfileName,Enabled,ResponseToChannel}
#include "Engine/CollisionProfile.h"          // UCollisionProfile channel-name <-> index resolution
#include "Engine/EngineTypes.h"               // ECollisionEnabled / ECollisionChannel / ECollisionResponse
#include "Engine/Engine.h"                    // GEngine world contexts (world='pie')

#define LOCTEXT_NAMESPACE "UEAgentSceneComponent"

/**
 * Scene component commands: text-render actors, spline and spline-mesh components, and the collision of a
 * placed actor's primitive component. Spline and spline-mesh components are added to a named editor-world
 * actor as instance components (CreationMethod = Instance).
 */
namespace UEAgentSceneComponentCommands
{

// MCP-DOMAIN: Level

namespace
{
    // Every entry of a points array as [x, y, z]; the first entry that is not exactly 3 numbers makes it an error, so
    // the spline tools refuse a malformed list before writing instead of building a spline from the entries that parsed.
    bool SC_ReadSplinePoints(const TArray<TSharedPtr<FJsonValue>>& PointsArr, TArray<FVector>& Out, FString& OutError)
    {
        for (int32 Index = 0; Index < PointsArr.Num(); ++Index)
        {
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            FVector P;
            bool bValid = PointsArr[Index].IsValid() && PointsArr[Index]->TryGetArray(Arr) && Arr && Arr->Num() == 3;
            for (int32 Axis = 0; bValid && Axis < 3; ++Axis)
            {
                double AxisValue = 0.0;
                bValid = (*Arr)[Axis].IsValid() && (*Arr)[Axis]->TryGetNumber(AxisValue);
                P[Axis] = AxisValue;
            }
            if (!bValid)
            {
                OutError = FString::Printf(TEXT("points[%d] is not [x, y, z] (3 numbers); nothing was changed"), Index);
                return false;
            }
            Out.Add(P);
        }
        return true;
    }

    // Pull the actor's first USplineComponent (the instance we add).
    USplineComponent* SC_GetSplineComponent(AActor* Actor)
    {
        return Actor ? Actor->FindComponentByClass<USplineComponent>() : nullptr;
    }

    // The editor-world actor the required actor_name names; null on success, else the error reply. The editor world is
    // checked before the param.
    TSharedPtr<FJsonObject> SC_ReadEditorActor(const TSharedPtr<FJsonObject>& Params, AActor*& OutActor, FString& OutActorName)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), OutActorName)) { return Missing; }

        OutActor = mcp_common::FindActorByName(World, OutActorName);
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found in the editor world"), *OutActorName));
        }
        return nullptr;
    }

    // SC_ReadEditorActor, then that actor's USplineComponent; null on success, else the error reply.
    TSharedPtr<FJsonObject> SC_ReadSplineActor(const TSharedPtr<FJsonObject>& Params, AActor*& OutActor, USplineComponent*& OutSpline)
    {
        FString ActorName;
        if (TSharedPtr<FJsonObject> Error = SC_ReadEditorActor(Params, OutActor, ActorName))
        {
            return Error;
        }

        OutSpline = SC_GetSplineComponent(OutActor);
        if (!OutSpline)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no USplineComponent (call add_spline_component first)"), *ActorName));
        }
        return nullptr;
    }

    // CANONICAL WORLD RESOLUTION, shared by every tool so that two tools never read two
    // different PIE instances and disagree in a way that looks like a game bug.
    // Resolve the world an actor lookup should run against from an optional "world" param.
    //  - absent / "editor" (default): the editor world, like the other handlers in this
    //    file, and the only world whose changes save_level persists.
    //  - "server" / "client" / "pie:N": the addressed PIE world.
    //  - "pie" / "game": the live PIE/Game world (ambiguous with 2+ instances).
    // OutWorldMode receives the selector actually used. Returns nullptr + fills OutError
    // (which already lists the worlds that DO exist) on failure.
    UWorld* SC_ResolveWorld(const TSharedPtr<FJsonObject>& Params, FString& OutError, FString& OutWorldMode)
    {
        UWorld* Resolved = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutWorldMode, OutError);
        if (Resolved)
        {
            return Resolved;
        }

        // Also accept an EWorldType::Game world (a cooked or -game process): the 'pie'
        // selector only scans PIE contexts.
        if (!OutWorldMode.Equals(TEXT("editor")))
        {
            if (UWorld* Active = mcp_common::ActiveGameWorld())
            {
                OutError.Reset();
                return Active;
            }
        }
        return nullptr;
    }
}

// MCP:spawn_text_render_actor - Spawn a TextRenderActor with its text, color (0..255, opaque) and world size set in the same call, into the ACTIVE world: the first PIE/game world when one runs (the actor then vanishes when play stops), otherwise the editor world. spawn_actor has no text-render type, and unlike it this has no world selector -- with several PIE instances it takes the first PIE context, not necessarily the server. world_size is read back. One undo step in the editor world; not saved (save_level).
//   @retired spawn_actor - type="TextRenderActor", location, rotation, properties={"TextRenderComponent.Text":"...","TextRenderComponent.TextRenderColor":{"R":255,"G":255,"B":255,"A":255},"TextRenderComponent.WorldSize":..}
//   @param text - Optional text to assign to the text render component
//   @param location - Optional [x, y, z] world location
//   @param rotation - Optional [pitch, yaw, roll] world rotation
//   @param color - Optional [r, g, b] 0..255 color (alpha forced opaque)
//   @param size - Optional world size for the text render component
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_component: bool, text_set: bool, color_set: bool, size_set: bool, world_size: number, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnTextRenderActor(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = mcp_common::GetActiveWorld();
    if (!World)
        return mcp_common::MakeErrorReply(TEXT("Failed to get active world (no PIE/game world running and no editor world available)"));

    FString Text;
    Params->TryGetStringField(TEXT("text"), Text);

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params.IsValid() && Params->HasField(TEXT("rotation")))
    {
        Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    const FScopedTransaction Transaction(LOCTEXT("SpawnTextRenderActor", "MCP: Spawn Text Render Actor"));
    ATextRenderActor* Actor = World->SpawnActor<ATextRenderActor>(Location, Rotation);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn TextRenderActor"));

    Actor->Modify();

    UTextRenderComponent* TextComp = Actor->GetTextRender();
    bool bTextSet = false;
    bool bColorSet = false;
    bool bSizeSet = false;
    double WorldSize = 0.0;

    if (TextComp)
    {
        if (!Text.IsEmpty())
        {
            TextComp->SetText(FText::FromString(Text));
            bTextSet = true;
        }

        // Optional [r,g,b] color in 0..255 (alpha forced opaque).
        const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
        if (Params->TryGetArrayField(TEXT("color"), ColorArr) && ColorArr && ColorArr->Num() >= 3)
        {
            const uint8 R = static_cast<uint8>(FMath::Clamp((*ColorArr)[0]->AsNumber(), 0.0, 255.0));
            const uint8 G = static_cast<uint8>(FMath::Clamp((*ColorArr)[1]->AsNumber(), 0.0, 255.0));
            const uint8 B = static_cast<uint8>(FMath::Clamp((*ColorArr)[2]->AsNumber(), 0.0, 255.0));
            TextComp->SetTextRenderColor(FColor(R, G, B, 255));
            bColorSet = true;
        }

        double Size = 0.0;
        if (Params->TryGetNumberField(TEXT("size"), Size))
        {
            TextComp->SetWorldSize(static_cast<float>(Size));
            bSizeSet = true;
        }
        WorldSize = static_cast<double>(TextComp->WorldSize); // read back
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("actor_internal_name"), Actor->GetName());
    Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    Result->SetBoolField(TEXT("has_component"), TextComp != nullptr);
    Result->SetBoolField(TEXT("text_set"), bTextSet);
    Result->SetBoolField(TEXT("color_set"), bColorSet);
    Result->SetBoolField(TEXT("size_set"), bSizeSet);
    Result->SetNumberField(TEXT("world_size"), WorldSize);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_spline_component - Add a USplineComponent to a named editor-world actor (name or label) as an undoable instance component on its root, optionally replacing its default points with your local-space points -- what add_component_to_actor cannot set in the same call. Not idempotent: each call adds a spline, while set_spline_points and get_spline_points act on the actor's FIRST one. An entry that is not [x, y, z] (3 numbers) is an error and nothing is added. Not saved (save_level).
//   @param actor_name - Display label or internal name of the target editor-world actor
//   @param points - Optional [[x, y, z], ...] spline points in local space; an entry that is not 3 numbers is an error and nothing is added
//   @returns { actor_name: string, component_name: string, component_class: string, num_points: int, added: bool }

TSharedPtr<FJsonObject> HandleAddSplineComponent(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    FString ActorName;
    if (TSharedPtr<FJsonObject> Error = SC_ReadEditorActor(Params, Actor, ActorName))
    {
        return Error;
    }

    TArray<FVector> Points;
    const TArray<TSharedPtr<FJsonValue>>* PointsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("points"), PointsArr) && PointsArr)
    {
        FString PointsError;
        if (!SC_ReadSplinePoints(*PointsArr, Points, PointsError))
        {
            return mcp_common::MakeErrorReply(PointsError);
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("AddSplineComponent", "MCP: Add Spline Component"));
    Actor->Modify();

    // Create + register an instance USplineComponent attached to the actor's root.
    USplineComponent* Spline = NewObject<USplineComponent>(Actor, USplineComponent::StaticClass(), NAME_None, RF_Transactional);
    if (!Spline)
        return mcp_common::MakeErrorReply(TEXT("Failed to create USplineComponent"));

    Spline->CreationMethod = EComponentCreationMethod::Instance;
    PrimitiveComponentUtils::RegisterInstanceComponent(Actor, Spline);

    // Optional point list -> clear default + add each in local space.
    if (Points.Num() > 0)
    {
        Spline->ClearSplinePoints(false);
        for (const FVector& P : Points)
        {
            Spline->AddSplinePoint(P, ESplineCoordinateSpace::Local, false);
        }
        Spline->UpdateSpline();
    }
    const int32 NumPoints = Spline->GetNumberOfSplinePoints();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("component_name"), Spline->GetName());
    Result->SetStringField(TEXT("component_class"), Spline->GetClass()->GetName());
    Result->SetNumberField(TEXT("num_points"), NumPoints);
    Result->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_spline_points - Replaces the spline points on a named actor's USplineComponent.  ClearSplinePoints + AddSplinePoint each (local space) + optional SetClosedLoop + UpdateSpline.
//   @param actor_name - Display label or internal name of the target editor-world actor
//   @param points - [[x, y, z], ...] spline points in local space; an entry that is not 3 numbers is an error and nothing is changed
//   @param closed_loop - Optional bool; toggles closed-loop mode
//   @returns { actor_name: string, component_name: string, num_points: int, closed_loop: bool, points_set: bool }

TSharedPtr<FJsonObject> HandleSetSplinePoints(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    USplineComponent* Spline = nullptr;
    if (TSharedPtr<FJsonObject> Error = SC_ReadSplineActor(Params, Actor, Spline))
    {
        return Error;
    }

    const TArray<TSharedPtr<FJsonValue>>* PointsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("points"), PointsArr) || !PointsArr)
        return mcp_common::MakeErrorReply(TEXT("Missing 'points' array"));
    TArray<FVector> Points;
    FString PointsError;
    if (!SC_ReadSplinePoints(*PointsArr, Points, PointsError))
    {
        return mcp_common::MakeErrorReply(PointsError);
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSplinePoints", "MCP: Set Spline Points"));
    Actor->Modify();
    Spline->Modify();

    Spline->ClearSplinePoints(false);
    for (const FVector& P : Points)
    {
        Spline->AddSplinePoint(P, ESplineCoordinateSpace::Local, false);
    }

    bool bClosedLoop = false;
    if (Params->TryGetBoolField(TEXT("closed_loop"), bClosedLoop))
    {
        Spline->SetClosedLoop(bClosedLoop, false);
    }

    Spline->UpdateSpline();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("component_name"), Spline->GetName());
    Result->SetNumberField(TEXT("num_points"), Spline->GetNumberOfSplinePoints());
    Result->SetBoolField(TEXT("closed_loop"), Spline->IsClosedLoop());
    Result->SetBoolField(TEXT("points_set"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_spline_points - Reads the control points of a named actor's USplineComponent (reader twin of set_spline_points). Returns each point's index and local-space location (the same space set_spline_points writes in), plus the total count and closed-loop flag.
//   @param actor_name - Display label or internal name of the target editor-world actor
//   @returns { actor_name: string, component_name: string, count: int, closed_loop: bool, points: [{ index: int, location: [number, number, number] }] }

TSharedPtr<FJsonObject> HandleGetSplinePoints(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    USplineComponent* Spline = nullptr;
    if (TSharedPtr<FJsonObject> Error = SC_ReadSplineActor(Params, Actor, Spline))
    {
        return Error;
    }

    const int32 Count = Spline->GetNumberOfSplinePoints();
    TArray<TSharedPtr<FJsonValue>> Points;
    for (int32 Index = 0; Index < Count; ++Index)
    {
        // Local space to mirror set_spline_points (which adds via ESplineCoordinateSpace::Local).
        const FVector Loc = Spline->GetLocationAtSplinePoint(Index, ESplineCoordinateSpace::Local);

        TSharedPtr<FJsonObject> Point = MakeShared<FJsonObject>();
        Point->SetNumberField(TEXT("index"), Index);
        Point->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Loc));
        Points.Add(MakeShared<FJsonValueObject>(Point));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("component_name"), Spline->GetName());
    Result->SetNumberField(TEXT("count"), Count);
    Result->SetBoolField(TEXT("closed_loop"), Spline->IsClosedLoop());
    Result->SetArrayField(TEXT("points"), Points);
    return mcp_common::MakeOkReply(Result);
}

// MCP:add_spline_mesh_component - Add a USplineMeshComponent to a named editor-world actor (name or label) as an undoable instance component attached to its root, with a static mesh deformed straight from start to end in local space (SetStartAndEnd, tangent = end - start). Over add_component_to_actor it loads the mesh and sets the deformation in the same call; a mesh that does not load is an error and nothing is added. Not idempotent: each call adds another component. Nothing is saved (save_level).
//   @param actor_name - Display label or internal name of the target editor-world actor
//   @param static_mesh_path - /Game or /Engine path to the static mesh to deform
//   @param start - Optional [x, y, z] start point; defaults to (0,0,0)
//   @param end - Optional [x, y, z] end point; defaults to (100,0,0)
//   @returns { actor_name: string, component_name: string, component_class: string, static_mesh_path: string, mesh_set: bool, start: [number, number, number], end: [number, number, number], added: bool }

TSharedPtr<FJsonObject> HandleAddSplineMeshComponent(const TSharedPtr<FJsonObject>& Params)
{
    AActor* Actor = nullptr;
    FString ActorName;
    if (TSharedPtr<FJsonObject> Error = SC_ReadEditorActor(Params, Actor, ActorName))
    {
        return Error;
    }

    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), MeshPath)) { return Missing; }

    UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *MeshPath));
    if (!Mesh)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load static mesh '%s'"), *MeshPath));

    FVector Start = FVector::ZeroVector;
    FVector End = FVector(100.0, 0.0, 0.0);
    const TArray<TSharedPtr<FJsonValue>>* StartArr = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* EndArr = nullptr;
    if (Params->TryGetArrayField(TEXT("start"), StartArr) && StartArr && StartArr->Num() >= 3)
    {
        Start = FVector((*StartArr)[0]->AsNumber(), (*StartArr)[1]->AsNumber(), (*StartArr)[2]->AsNumber());
    }
    if (Params->TryGetArrayField(TEXT("end"), EndArr) && EndArr && EndArr->Num() >= 3)
    {
        End = FVector((*EndArr)[0]->AsNumber(), (*EndArr)[1]->AsNumber(), (*EndArr)[2]->AsNumber());
    }

    const FScopedTransaction Transaction(LOCTEXT("AddSplineMeshComponent", "MCP: Add Spline Mesh Component"));
    Actor->Modify();

    USplineMeshComponent* SplineMesh = NewObject<USplineMeshComponent>(Actor, USplineMeshComponent::StaticClass(), NAME_None, RF_Transactional);
    if (!SplineMesh)
        return mcp_common::MakeErrorReply(TEXT("Failed to create USplineMeshComponent"));

    // Set the mesh before registration so the component builds with it.
    const bool bMeshSet = SplineMesh->SetStaticMesh(Mesh);

    SplineMesh->CreationMethod = EComponentCreationMethod::Instance;
    PrimitiveComponentUtils::RegisterInstanceComponent(Actor, SplineMesh);

    // Straight deformation along Start->End (tangent = direction * length).
    const FVector Tangent = End - Start;
    SplineMesh->SetStartAndEnd(Start, Tangent, End, Tangent, true);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Result->SetStringField(TEXT("component_name"), SplineMesh->GetName());
    Result->SetStringField(TEXT("component_class"), SplineMesh->GetClass()->GetName());
    Result->SetStringField(TEXT("static_mesh_path"), MeshPath);
    Result->SetBoolField(TEXT("mesh_set"), bMeshSet);
    Result->SetArrayField(TEXT("start"), mcp_common::VectorToJsonArray(Start));
    Result->SetArrayField(TEXT("end"), mcp_common::VectorToJsonArray(End));
    Result->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_actor_component_collision - Sets collision on a PLACED actor's UPrimitiveComponent (the placed-actor twin of set_blueprint_component_collision). Resolves the actor by name/label then its primitive component (by name, or the root primitive when component_name is omitted) and applies, in order: SetCollisionProfileName (expands the profile into the FBodyInstance response array), SetCollisionEnabled, then per-channel SetCollisionResponseToChannel. Channel keys accept a friendly display name ("Pawn", "Visibility", or a project CUSTOM trace/object channel) OR an ECC_* token; this is how you block a custom game channel that the BlockAll profile leaves at Ignore. After applying, collision-enabled is toggled off/on to force the FBodyInstance to rebuild its filter data, and Actor->Modify() marks the (OFPA) package dirty so save_level persists the change.
//   @param actor_name [string!] - Display label or internal name of the target actor
//   @param component_name [string] - Primitive component name; defaults to the actor's root primitive component when omitted
//   @param profile [string] - Optional collision profile name to apply first (e.g. "BlockAll", "OverlapAllDynamic", a custom preset)
//   @param collision_enabled [string] - Optional NoCollision/QueryOnly/PhysicsOnly/QueryAndPhysics
//   @param responses [object] - Optional { channel: response } map; channel = friendly name or ECC_* token, response = Ignore/Overlap/Block
//   @param world [string] - absent/"editor" (default; the world save_level persists) | "server" (the PIE world with authority) | "client" | "pie:N" | "pie" (ambiguous) | "game" (alias of "pie")
//   @returns { actor_name: string, component_name: string, component_class: string, profile: string, collision_enabled: string, responses_applied: [{ channel: string, response: string }], collision_enabled_effective: string, rebuilt: bool, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
TSharedPtr<FJsonObject> HandleSetActorComponentCollision(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    FString WorldErr, WorldMode;
    UWorld* World = SC_ResolveWorld(Params, WorldErr, WorldMode);
    if (!World)
        return mcp_common::MakeErrorReply(WorldErr);

    AActor* Actor = mcp_common::FindActorByName(World, ActorName);
    if (!Actor)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));

    // The target primitive component: by name if given, else the root primitive (the resolution
    // add_impulse_to_actor uses).
    FString ComponentName;
    Params->TryGetStringField(TEXT("component_name"), ComponentName);

    UPrimitiveComponent* Prim = nullptr;
    FString ComponentError;
    if (!PrimitiveComponentUtils::FindPrimitive(Actor, ActorName, ComponentName, Prim, ComponentError))
    {
        return mcp_common::MakeErrorReply(ComponentError);
    }
    if (!Prim)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor '%s' has no UPrimitiveComponent root; pass 'component_name'"), *ActorName));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetActorComponentCollision", "MCP: Set Actor Component Collision"));
    // Actor->Modify() marks the actor's (external/OFPA) package dirty so save_level persists this;
    // Prim->Modify() records the component's pre-change state for undo.
    Actor->Modify();
    Prim->Modify();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Applied;

    // The profile first, then collision_enabled, then the per-channel responses
    // ({"ECC_Pawn":"Overlap", "MyCustomChannel":"Block", ...}), so the explicit settings override the
    // profile's baseline.
    FString Profile;
    Params->TryGetStringField(TEXT("profile"), Profile);
    FString EnabledStr;
    Params->TryGetStringField(TEXT("collision_enabled"), EnabledStr);
    const TSharedPtr<FJsonObject>* ResponsesObj = nullptr;
    const bool bHasResponses = Params->TryGetObjectField(TEXT("responses"), ResponsesObj);
    FString CollisionError;
    if (!PrimitiveComponentUtils::ApplyCollisionSettings(Prim, Profile, EnabledStr, bHasResponses ? ResponsesObj : nullptr,
            Data, Applied, CollisionError))
    {
        return mcp_common::MakeErrorReply(CollisionError);
    }

    // Toggle collision-enabled off/on to force the FBodyInstance to rebuild its collision filter
    // data. On a placed actor with an already-cooked physics state, response-only edits don't always
    // re-cook the filter; the toggle guarantees the new channel responses take effect for traces.
    const ECollisionEnabled::Type FinalEnabled = Prim->GetCollisionEnabled();
    if (FinalEnabled != ECollisionEnabled::NoCollision)
    {
        Prim->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Prim->SetCollisionEnabled(FinalEnabled);
    }

    Data->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
    Data->SetStringField(TEXT("component_name"), Prim->GetName());
    Data->SetStringField(TEXT("component_class"), Prim->GetClass()->GetName());
    Data->SetArrayField(TEXT("responses_applied"), Applied);
    Data->SetStringField(TEXT("collision_enabled_effective"),
        StaticEnum<ECollisionEnabled::Type>()->GetNameStringByValue((int64)FinalEnabled));
    Data->SetBoolField(TEXT("rebuilt"), FinalEnabled != ECollisionEnabled::NoCollision);
    // The selector that picked the world, not a second parse of the params.
    Data->SetStringField(TEXT("world"), WorldMode);
    // Collision edited in the PIE world is transient; knowing WHICH world took the edit
    // is the difference between "it didn't apply" and "it applied where nothing saves".
    mcp_common::AddWorldEvidence(Data, World);
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_spline_component"), &HandleAddSplineComponent },
    { TEXT("add_spline_mesh_component"), &HandleAddSplineMeshComponent },
    { TEXT("get_spline_points"), &HandleGetSplinePoints },
    { TEXT("set_actor_component_collision"), &HandleSetActorComponentCollision },
    { TEXT("set_spline_points"), &HandleSetSplinePoints },
    { TEXT("spawn_text_render_actor"), &HandleSpawnTextRenderActor },
};
} // namespace UEAgentSceneComponentCommands

#undef LOCTEXT_NAMESPACE
