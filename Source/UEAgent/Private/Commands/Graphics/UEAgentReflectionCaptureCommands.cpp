#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

// Reflection / scene capture actors + components (all in the Engine module).
#include "Engine/SphereReflectionCapture.h"                 // ASphereReflectionCapture
#include "Engine/BoxReflectionCapture.h"                     // ABoxReflectionCapture
#include "Engine/SceneCapture2D.h"                           // ASceneCapture2D
#include "Components/ReflectionCaptureComponent.h"           // UReflectionCaptureComponent (Brightness, Cubemap, ReflectionSourceType, EReflectionSourceType, MarkDirtyForRecapture)
#include "Components/SphereReflectionCaptureComponent.h"     // USphereReflectionCaptureComponent (InfluenceRadius)
#include "Components/BoxReflectionCaptureComponent.h"         // UBoxReflectionCaptureComponent (BoxTransitionDistance; extent derives from actor scale)
#include "Components/SceneCaptureComponent.h"                 // USceneCaptureComponent base (CaptureSource, bCaptureEveryFrame)
#include "Components/SceneCaptureComponent2D.h"               // USceneCaptureComponent2D (FOVAngle, ProjectionType, TextureTarget)
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureCube.h"                               // UTextureCube (Cubemap source)

#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "UEAgentReflectionCapture"

/**
 * Reflection-capture / scene-capture actor commands plus Lumen / global-illumination scene tuning.
 *
 * They spawn ASphereReflectionCapture, ABoxReflectionCapture and ASceneCapture2D actors into the editor world,
 * edit and read the properties of existing capture actors, rebuild reflection-capture contents, and
 * apply/read Lumen GI quality and the dynamic global-illumination method via console variables.
 *
 * NOTE: named ReflectionCapture* to avoid colliding with the unrelated C++-introspection family
 * UEAgentReflectionCommands.
 */
namespace UEAgentReflectionCaptureCommands
{

// MCP-DOMAIN: Lighting

namespace
{
    /** The name the scene-capture replies give an ESceneCaptureSource. */
    FString ReflectionCapture_CaptureSourceName(ESceneCaptureSource Src)
    {
        switch (Src)
        {
            case SCS_SceneColorHDR:          return TEXT("SceneColorHDR");
            case SCS_SceneColorHDRNoAlpha:   return TEXT("SceneColorHDRNoAlpha");
            case SCS_FinalColorLDR:          return TEXT("FinalColorLDR");
            case SCS_SceneColorSceneDepth:   return TEXT("SceneColorSceneDepth");
            case SCS_SceneDepth:             return TEXT("SceneDepth");
            case SCS_DeviceDepth:            return TEXT("DeviceDepth");
            case SCS_Normal:                 return TEXT("Normal");
            case SCS_BaseColor:              return TEXT("BaseColor");
            case SCS_FinalColorHDR:          return TEXT("FinalColorHDR");
            case SCS_FinalToneCurveHDR:      return TEXT("FinalToneCurveHDR");
            default:                         return TEXT("Unknown");
        }
    }

    /** A reply that opens with the actor's label (actor_name) and object name (actor_internal_name). */
    TSharedPtr<FJsonObject> ReflectionCapture_StartReply(const AActor* Actor)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("actor_internal_name"), Actor->GetName());
        return Result;
    }

    /**
     * Applies the optional "brightness" param to the capture component and returns its Brightness read back (0
     * without a component); bOutSet tells whether the param was applied.
     */
    double ReflectionCapture_ApplyBrightness(const TSharedPtr<FJsonObject>& Params,
        UReflectionCaptureComponent* CaptureComp, bool& bOutSet)
    {
        bOutSet = false;
        double Brightness = 0.0;
        if (CaptureComp)
        {
            Brightness = static_cast<double>(CaptureComp->Brightness);
        }
        double RequestedBrightness = 0.0;
        if (CaptureComp && Params->TryGetNumberField(TEXT("brightness"), RequestedBrightness))
        {
            CaptureComp->Brightness = static_cast<float>(RequestedBrightness);
            Brightness = static_cast<double>(CaptureComp->Brightness); // read back
            bOutSet = true;
        }
        return Brightness;
    }

    /**
     * Reads "actor_name" and resolves that reflection-capture actor of the editor world and its capture component.
     * Null on success, else the error reply.
     */
    TSharedPtr<FJsonObject> ReflectionCapture_ResolveCapture(const TSharedPtr<FJsonObject>& Params,
        AReflectionCapture*& OutActor, UReflectionCaptureComponent*& OutComponent)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world")))
        {
            return NoWorld;
        }

        FString ActorName;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'actor_name'"));
        }

        AActor* Actor = mcp_common::FindActorByName(World, ActorName);
        if (!Actor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Reflection-capture actor not found: %s"), *ActorName));
        }
        OutActor = Cast<AReflectionCapture>(Actor);
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' is not a reflection capture (class %s)"), *ActorName, *Actor->GetClass()->GetName()));
        }
        OutComponent = OutActor->GetCaptureComponent();
        if (!OutComponent)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Reflection-capture actor '%s' has no capture component"), *ActorName));
        }
        return nullptr;
    }

    /**
     * Reads "actor_name" and resolves that SceneCapture2D actor of the editor world and its capture component. Null on
     * success, else the error reply.
     */
    TSharedPtr<FJsonObject> ReflectionCapture_ResolveSceneCapture(const TSharedPtr<FJsonObject>& Params,
        ASceneCapture2D*& OutActor, USceneCaptureComponent2D*& OutComponent)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world")))
        {
            return NoWorld;
        }

        FString ActorName;
        if (!Params.IsValid() || !Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'actor_name'"));
        }

        AActor* Actor = mcp_common::FindActorByName(World, ActorName);
        if (!Actor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("SceneCapture2D actor not found: %s"), *ActorName));
        }
        OutActor = Cast<ASceneCapture2D>(Actor);
        if (!OutActor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' is not a SceneCapture2D (class %s)"), *ActorName, *Actor->GetClass()->GetName()));
        }
        OutComponent = OutActor->GetCaptureComponent2D();
        if (!OutComponent)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("SceneCapture2D actor '%s' has no capture component"), *ActorName));
        }
        return nullptr;
    }
}

// MCP:spawn_sphere_reflection_capture - Spawn a SphereReflectionCapture actor into the editor world at location in one undo step, optionally set its InfluenceRadius (cm) and Brightness (read back), and mark it dirty for recapture; spawn_actor has no reflection-capture type, and spawn_box_reflection_capture places a box probe. Each call adds another actor; not saved (save_level). Tune it with set_reflection_capture_properties.
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param influence_radius - Optional InfluenceRadius (cm) for the sphere capture component
//   @param brightness - Optional Brightness multiplier on the capture component
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_component: bool, influence_radius_set: bool, influence_radius: number, brightness_set: bool, brightness: number, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnSphereReflectionCapture(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    const FScopedTransaction Transaction(LOCTEXT("SpawnSphereReflectionCapture", "MCP: Spawn Sphere Reflection Capture"));
    ASphereReflectionCapture* Actor = World->SpawnActor<ASphereReflectionCapture>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn SphereReflectionCapture actor"));

    Actor->Modify();

    // GetCaptureComponent() is public on AReflectionCapture and returns the base
    // UReflectionCaptureComponent. For a sphere capture the concrete component is a
    // USphereReflectionCaptureComponent which owns InfluenceRadius.
    UReflectionCaptureComponent* CaptureComp = Actor->GetCaptureComponent();
    USphereReflectionCaptureComponent* SphereComp = Cast<USphereReflectionCaptureComponent>(CaptureComp);

    bool bInfluenceSet = false;
    double InfluenceRadius = 0.0;
    if (SphereComp)
    {
        InfluenceRadius = static_cast<double>(SphereComp->InfluenceRadius);
    }

    double RequestedInfluence = 0.0;
    if (SphereComp && Params->TryGetNumberField(TEXT("influence_radius"), RequestedInfluence))
    {
        SphereComp->InfluenceRadius = static_cast<float>(RequestedInfluence);
        InfluenceRadius = static_cast<double>(SphereComp->InfluenceRadius); // read back
        bInfluenceSet = true;
    }

    bool bBrightnessSet = false;
    const double Brightness = ReflectionCapture_ApplyBrightness(Params, CaptureComp, bBrightnessSet);

    // Queue a recapture so the change is reflected (exported ENGINE_API path).
    if (CaptureComp)
    {
        CaptureComp->MarkDirtyForRecapture();
    }

    TSharedPtr<FJsonObject> Result = ReflectionCapture_StartReply(Actor);
    Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    Result->SetBoolField(TEXT("has_component"), CaptureComp != nullptr);
    Result->SetBoolField(TEXT("influence_radius_set"), bInfluenceSet);
    Result->SetNumberField(TEXT("influence_radius"), InfluenceRadius);
    Result->SetBoolField(TEXT("brightness_set"), bBrightnessSet);
    Result->SetNumberField(TEXT("brightness"), Brightness);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:spawn_box_reflection_capture - Spawn a BoxReflectionCapture actor into the editor world at location in one undo step, optionally set its Brightness (read back), and mark it dirty for recapture. spawn_actor has no reflection-capture type; spawn_sphere_reflection_capture places a sphere probe. Each call adds an actor; not saved (save_level). Tune: set_reflection_capture_properties.
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param brightness - Optional Brightness multiplier on the capture component
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_component: bool, brightness_set: bool, brightness: number, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnBoxReflectionCapture(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    const FScopedTransaction Transaction(LOCTEXT("SpawnBoxReflectionCapture", "MCP: Spawn Box Reflection Capture"));
    ABoxReflectionCapture* Actor = World->SpawnActor<ABoxReflectionCapture>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn BoxReflectionCapture actor"));

    Actor->Modify();

    UReflectionCaptureComponent* CaptureComp = Actor->GetCaptureComponent();

    bool bBrightnessSet = false;
    const double Brightness = ReflectionCapture_ApplyBrightness(Params, CaptureComp, bBrightnessSet);

    if (CaptureComp)
    {
        CaptureComp->MarkDirtyForRecapture();
    }

    TSharedPtr<FJsonObject> Result = ReflectionCapture_StartReply(Actor);
    Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    Result->SetBoolField(TEXT("has_component"), CaptureComp != nullptr);
    Result->SetBoolField(TEXT("brightness_set"), bBrightnessSet);
    Result->SetNumberField(TEXT("brightness"), Brightness);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:spawn_scene_capture_2d - Spawn a SceneCapture2D actor in the editor world (also while PIE runs, never into PIE) at location and rotation, in one undo step; the level is marked dirty but not saved. spawn_actor has no scene-capture type. Each call spawns another actor (not idempotent) under the engine default label: actor_name is that label, actor_internal_name the object name to address it by later. A fresh capture has no render target, so has_render_target is false until one is assigned. Set its TextureTarget, capture source, projection and FOV with set_scene_capture_2d_properties (a target comes from create_render_target_2d), read them back with get_scene_capture_2d_properties. A reflection probe is spawn_sphere_reflection_capture or spawn_box_reflection_capture.
//   @param location - Optional [X, Y, Z] world location; missing means the origin, and any other value is an error and nothing is spawned
//   @param rotation - Optional world rotation in degrees, as [Pitch, Yaw, Roll] or {"pitch", "yaw", "roll"} by name (defaults to zero)
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_component: bool, has_render_target: bool, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnSceneCapture2D(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params.IsValid() && Params->HasField(TEXT("rotation")))
    {
        Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    const FScopedTransaction Transaction(LOCTEXT("SpawnSceneCapture2D", "MCP: Spawn Scene Capture 2D"));
    ASceneCapture2D* Actor = World->SpawnActor<ASceneCapture2D>(Location, Rotation);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn SceneCapture2D actor"));

    Actor->Modify();

    // GetCaptureComponent2D() is public on ASceneCapture2D; TextureTarget is the
    // public render-target field on USceneCaptureComponent2D.
    USceneCaptureComponent2D* CaptureComp = Actor->GetCaptureComponent2D();
    const bool bHasComponent = (CaptureComp != nullptr);
    const bool bHasRenderTarget = (CaptureComp && CaptureComp->TextureTarget != nullptr);

    TSharedPtr<FJsonObject> Result = ReflectionCapture_StartReply(Actor);
    Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
    Result->SetBoolField(TEXT("has_component"), bHasComponent);
    Result->SetBoolField(TEXT("has_render_target"), bHasRenderTarget);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:update_reflection_captures - Rebuilds reflection-capture contents for all captures in the editor world.  Marks every reflection capture dirty and runs the canonical editor rebuild path (UReflectionCaptureComponent::UpdateReflectionCaptureContents).
//   @returns { updated: bool, capture_count: number }

TSharedPtr<FJsonObject> HandleUpdateReflectionCaptures(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    // Count existing reflection captures and mark them dirty so the rebuild has work.
    int32 CaptureCount = 0;
    for (TActorIterator<AReflectionCapture> It(World); It; ++It)
    {
        AReflectionCapture* RC = *It;
        if (RC)
        {
            ++CaptureCount;
            if (UReflectionCaptureComponent* Comp = RC->GetCaptureComponent())
            {
                Comp->MarkDirtyForRecapture();
            }
        }
    }

    // Canonical editor rebuild path (exported ENGINE_API static; this is exactly what
    // UEditorEngine uses to update reflection captures, e.g. EditorEngine.cpp).
    UReflectionCaptureComponent::UpdateReflectionCaptureContents(World, TEXT("MCP UpdateReflectionCaptures"));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetBoolField(TEXT("updated"), true);
    Result->SetNumberField(TEXT("capture_count"), CaptureCount);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_lumen_gi_quality - Applies Lumen / global-illumination scene tuning via console variables.  Provide exactly one of:     quality: "Low" | "Medium" | "High" | "Epic" | "Cinematic"         -> sg.GlobalIlluminationQuality (scalability group 0..4), read back.     method:  "Lumen" | "ScreenSpace" | "None"         -> r.DynamicGlobalIlluminationMethod (0=None,1=Lumen,2=ScreenSpace), read back.
//   @param quality - One of "Low" | "Medium" | "High" | "Epic" | "Cinematic" (mutually exclusive with method)
//   @param method - One of "Lumen" | "ScreenSpace" | "None" (mutually exclusive with quality)
//   @returns { mode: string, cvar: string, requested_method: string?, requested_quality: string?, requested_value: number, value: number, applied: bool }

TSharedPtr<FJsonObject> HandleSetLumenGIQuality(const TSharedPtr<FJsonObject>& Params)
{
    // Two modes:
    //  * "quality": "Low"|"Medium"|"High"|"Epic"|"Cinematic" -> sg.GlobalIlluminationQuality
    //    (scalability group 0..4) read back.
    //  * "method":  "Lumen"|"ScreenSpace"|"None" -> r.DynamicGlobalIlluminationMethod
    //    (0=None,1=Lumen,2=ScreenSpace) read back.
    FString Quality;
    FString Method;
    const bool bHasQuality = Params.IsValid() && Params->TryGetStringField(TEXT("quality"), Quality);
    const bool bHasMethod  = Params.IsValid() && Params->TryGetStringField(TEXT("method"),  Method);

    if (!bHasQuality && !bHasMethod)
    {
        return mcp_common::MakeErrorReply(TEXT("Provide either 'quality' (Low|Medium|High|Epic|Cinematic) or 'method' (Lumen|ScreenSpace|None)"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

    if (bHasMethod)
    {
        const FString M = Method.ToLower();
        int32 MethodValue = 1; // default Lumen
        if (M == TEXT("none"))             { MethodValue = 0; }
        else if (M == TEXT("lumen"))       { MethodValue = 1; }
        else if (M == TEXT("screenspace")) { MethodValue = 2; }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown GI method '%s' (expected Lumen|ScreenSpace|None)"), *Method));
        }

        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
        if (!CVar)
        {
            return mcp_common::MakeErrorReply(TEXT("Console variable r.DynamicGlobalIlluminationMethod not found"));
        }
        CVar->Set(MethodValue, ECVF_SetByConsole);
        const int32 ReadBack = CVar->GetInt(); // verify by reading the cvar back

        Result->SetStringField(TEXT("mode"), TEXT("method"));
        Result->SetStringField(TEXT("cvar"), TEXT("r.DynamicGlobalIlluminationMethod"));
        Result->SetStringField(TEXT("requested_method"), Method);
        Result->SetNumberField(TEXT("requested_value"), static_cast<double>(MethodValue));
        Result->SetNumberField(TEXT("value"), static_cast<double>(ReadBack));
        Result->SetBoolField(TEXT("applied"), ReadBack == MethodValue);
    }
    else // bHasQuality
    {
        const FString Q = Quality.ToLower();
        int32 QualityLevel = 3; // default Epic
        if (Q == TEXT("low"))           { QualityLevel = 0; }
        else if (Q == TEXT("medium"))   { QualityLevel = 1; }
        else if (Q == TEXT("high"))     { QualityLevel = 2; }
        else if (Q == TEXT("epic"))     { QualityLevel = 3; }
        else if (Q == TEXT("cinematic")){ QualityLevel = 4; }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown GI quality '%s' (expected Low|Medium|High|Epic|Cinematic)"), *Quality));
        }

        IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("sg.GlobalIlluminationQuality"));
        if (!CVar)
        {
            return mcp_common::MakeErrorReply(TEXT("Console variable sg.GlobalIlluminationQuality not found"));
        }
        CVar->Set(QualityLevel, ECVF_SetByScalability);
        const int32 ReadBack = CVar->GetInt(); // verify by reading the cvar back

        Result->SetStringField(TEXT("mode"), TEXT("quality"));
        Result->SetStringField(TEXT("cvar"), TEXT("sg.GlobalIlluminationQuality"));
        Result->SetStringField(TEXT("requested_quality"), Quality);
        Result->SetNumberField(TEXT("requested_value"), static_cast<double>(QualityLevel));
        Result->SetNumberField(TEXT("value"), static_cast<double>(ReadBack));
        Result->SetBoolField(TEXT("applied"), ReadBack == QualityLevel);
    }

    return mcp_common::MakeOkReply(Result);
}

// MCP:set_reflection_capture_properties - Set influence radius (Sphere), transition distance and extent (Box), brightness or cubemap of a Sphere or Box reflection-capture actor found by label or name in the editor world, then mark it dirty for recapture and read each field back. Over set_properties: box_extent maps to the actor scale (the box capture has no extent property), brightness is clamped to [0.5, 4] and a cubemap also switches ReflectionSourceType to SpecifiedCubemap. A param for the other capture shape is a no-op reported as <prop>_set=false. A box_extent that is not exactly 3 numbers, or a cubemap path that does not load as a TextureCube, fails the call before anything is written. Nothing is saved (save_level).
//   @param actor_name [!] - Label or internal name of the reflection-capture actor
//   @param influence_radius [number] - Sphere capture InfluenceRadius in cm (Sphere only)
//   @param box_transition_distance [number] - Box capture BoxTransitionDistance (Box only)
//   @param brightness [number] - Capture Brightness multiplier (clamped to [0.5, 4])
//   @param box_extent [array] - Box capture extent as actor scale [X, Y, Z] (Box only; SetActorScale3D). Anything but an array of exactly 3 numbers fails the call and nothing is changed, on a Sphere capture too.
//   @param cubemap [string] - Path to a UTextureCube; assigning it sets ReflectionSourceType=SpecifiedCubemap. A path that does not load as a TextureCube fails the call and nothing is changed.
//   @returns { actor_name: string, actor_internal_name: string, capture_type: string, influence_radius_set: bool, influence_radius: number, box_transition_distance_set: bool, box_transition_distance: number, brightness_set: bool, brightness: number, box_extent_set: bool, box_extent: [number], cubemap_set: bool, cubemap: string, reflection_source_type: string, marked_dirty: bool }
TSharedPtr<FJsonObject> HandleSetReflectionCaptureProperties(const TSharedPtr<FJsonObject>& Params)
{
    AReflectionCapture* CaptureActor = nullptr;
    UReflectionCaptureComponent* CaptureComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = ReflectionCapture_ResolveCapture(Params, CaptureActor, CaptureComp))
    {
        return Error;
    }

    // The base getter hands back UReflectionCaptureComponent*; the concrete component
    // (sphere vs box) owns the type-specific fields, reached via Cast.
    USphereReflectionCaptureComponent* SphereComp = Cast<USphereReflectionCaptureComponent>(CaptureComp);
    UBoxReflectionCaptureComponent* BoxComp = Cast<UBoxReflectionCaptureComponent>(CaptureComp);
    const FString CaptureType = SphereComp ? TEXT("Sphere") : (BoxComp ? TEXT("Box") : TEXT("Unknown"));

    // box_extent and cubemap are parsed before the first write, so a refused call leaves the capture as it was.
    bool bHasBoxExtent = false;
    FVector RequestedExtent = FVector::OneVector;
    // Read as an array, so the published schema says array; a value of another JSON type (null aside) is still refused.
    const TArray<TSharedPtr<FJsonValue>>* ExtentArr = nullptr;
    const bool bExtentIsArray = Params->TryGetArrayField(TEXT("box_extent"), ExtentArr) && ExtentArr;
    const TSharedPtr<FJsonValue>* ExtentRaw = Params->Values.Find(TEXT("box_extent"));
    if (bExtentIsArray || (ExtentRaw && ExtentRaw->IsValid() && !(*ExtentRaw)->IsNull()))
    {
        bool bValidExtent = bExtentIsArray && ExtentArr->Num() == 3;
        for (int32 Axis = 0; bValidExtent && Axis < 3; ++Axis)
        {
            double AxisValue = 0.0;
            bValidExtent = (*ExtentArr)[Axis].IsValid() && (*ExtentArr)[Axis]->TryGetNumber(AxisValue);
            RequestedExtent[Axis] = AxisValue;
        }
        if (!bValidExtent)
        {
            return mcp_common::MakeErrorReply(TEXT("box_extent must be an array of exactly 3 numbers [X, Y, Z]; nothing was changed"));
        }
        bHasBoxExtent = true;
    }

    UTextureCube* RequestedCube = nullptr;
    FString CubemapPath;
    if (Params->TryGetStringField(TEXT("cubemap"), CubemapPath) && !CubemapPath.IsEmpty())
    {
        RequestedCube = LoadObject<UTextureCube>(nullptr, *CubemapPath);
        if (!RequestedCube)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Failed to load cubemap texture: %s (expected a TextureCube asset path); nothing was changed"), *CubemapPath));
        }
    }

    const FScopedTransaction Transaction(LOCTEXT("SetReflectionCaptureProperties", "MCP: Set Reflection Capture Properties"));
    CaptureActor->Modify();
    CaptureComp->Modify();

    TSharedPtr<FJsonObject> Result = ReflectionCapture_StartReply(CaptureActor);
    Result->SetStringField(TEXT("capture_type"), CaptureType);

    // influence_radius (Sphere only) -------------------------------------------------
    bool bInfluenceSet = false;
    double InfluenceRadius = SphereComp ? static_cast<double>(SphereComp->InfluenceRadius) : 0.0;
    double RequestedInfluence = 0.0;
    if (Params->TryGetNumberField(TEXT("influence_radius"), RequestedInfluence) && SphereComp)
    {
        SphereComp->InfluenceRadius = static_cast<float>(RequestedInfluence);
        InfluenceRadius = static_cast<double>(SphereComp->InfluenceRadius); // read back
        bInfluenceSet = true;
    }
    Result->SetBoolField(TEXT("influence_radius_set"), bInfluenceSet);
    Result->SetNumberField(TEXT("influence_radius"), InfluenceRadius);

    // box_transition_distance (Box only) ---------------------------------------------
    bool bTransitionSet = false;
    double BoxTransitionDistance = BoxComp ? static_cast<double>(BoxComp->BoxTransitionDistance) : 0.0;
    double RequestedTransition = 0.0;
    if (Params->TryGetNumberField(TEXT("box_transition_distance"), RequestedTransition) && BoxComp)
    {
        BoxComp->BoxTransitionDistance = static_cast<float>(RequestedTransition);
        BoxTransitionDistance = static_cast<double>(BoxComp->BoxTransitionDistance); // read back
        bTransitionSet = true;
    }
    Result->SetBoolField(TEXT("box_transition_distance_set"), bTransitionSet);
    Result->SetNumberField(TEXT("box_transition_distance"), BoxTransitionDistance);

    // brightness (base; logical clamp [0.5, 4]) --------------------------------------
    bool bBrightnessSet = false;
    double Brightness = static_cast<double>(CaptureComp->Brightness);
    double RequestedBrightness = 0.0;
    if (Params->TryGetNumberField(TEXT("brightness"), RequestedBrightness))
    {
        const double Clamped = FMath::Clamp(RequestedBrightness, 0.5, 4.0);
        CaptureComp->Brightness = static_cast<float>(Clamped);
        Brightness = static_cast<double>(CaptureComp->Brightness); // read back
        bBrightnessSet = true;
    }
    Result->SetBoolField(TEXT("brightness_set"), bBrightnessSet);
    Result->SetNumberField(TEXT("brightness"), Brightness);

    // box_extent -> actor scale (Box only; no extent UPROPERTY on the box capture) ----
    bool bBoxExtentSet = false;
    if (bHasBoxExtent && BoxComp)
    {
        CaptureActor->SetActorScale3D(RequestedExtent);
        bBoxExtentSet = true;
    }
    const FVector ReadScale = CaptureActor->GetActorScale3D(); // read back actual scale
    TArray<TSharedPtr<FJsonValue>> ScaleJson = mcp_common::VectorToJsonArray(ReadScale);
    Result->SetBoolField(TEXT("box_extent_set"), bBoxExtentSet);
    Result->SetArrayField(TEXT("box_extent"), ScaleJson);

    // cubemap -> load UTextureCube + ReflectionSourceType = SpecifiedCubemap ----------
    bool bCubemapSet = false;
    if (RequestedCube)
    {
        CaptureComp->Cubemap = RequestedCube;
        CaptureComp->ReflectionSourceType = EReflectionSourceType::SpecifiedCubemap;
        bCubemapSet = true;
    }
    Result->SetBoolField(TEXT("cubemap_set"), bCubemapSet);
    Result->SetStringField(TEXT("cubemap"), CaptureComp->Cubemap ? CaptureComp->Cubemap->GetPathName() : FString());
    Result->SetStringField(TEXT("reflection_source_type"),
        CaptureComp->ReflectionSourceType == EReflectionSourceType::SpecifiedCubemap ? TEXT("SpecifiedCubemap") : TEXT("CapturedScene"));

    // Queue a recapture so changes take (exported, headless-safe ENGINE_API path).
    CaptureComp->MarkDirtyForRecapture();
    Result->SetBoolField(TEXT("marked_dirty"), true);

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_reflection_capture_properties - Read a Sphere or Box reflection-capture actor of the editor level, found by label or internal name: capture_type, InfluenceRadius (Sphere), BoxTransitionDistance (Box), Brightness, the actor scale reported as box_extent, the Cubemap path and ReflectionSourceType. A curated round-trip view for set_reflection_capture_properties, not every property; get_properties (internal names only) reads any other UPROPERTY. Pure reader, no recapture.
//   @param actor_name [!] - Label or internal name of the reflection-capture actor
//   @returns { actor_name: string, actor_internal_name: string, capture_type: string, influence_radius: number, box_transition_distance: number, brightness: number, box_extent: [number], cubemap: string, reflection_source_type: string }
TSharedPtr<FJsonObject> HandleGetReflectionCaptureProperties(const TSharedPtr<FJsonObject>& Params)
{
    AReflectionCapture* CaptureActor = nullptr;
    UReflectionCaptureComponent* CaptureComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = ReflectionCapture_ResolveCapture(Params, CaptureActor, CaptureComp))
    {
        return Error;
    }

    USphereReflectionCaptureComponent* SphereComp = Cast<USphereReflectionCaptureComponent>(CaptureComp);
    UBoxReflectionCaptureComponent* BoxComp = Cast<UBoxReflectionCaptureComponent>(CaptureComp);
    const FString CaptureType = SphereComp ? TEXT("Sphere") : (BoxComp ? TEXT("Box") : TEXT("Unknown"));

    const FVector Scale = CaptureActor->GetActorScale3D();
    TArray<TSharedPtr<FJsonValue>> ScaleJson = mcp_common::VectorToJsonArray(Scale);

    TSharedPtr<FJsonObject> Result = ReflectionCapture_StartReply(CaptureActor);
    Result->SetStringField(TEXT("capture_type"), CaptureType);
    Result->SetNumberField(TEXT("influence_radius"), SphereComp ? static_cast<double>(SphereComp->InfluenceRadius) : 0.0);
    Result->SetNumberField(TEXT("box_transition_distance"), BoxComp ? static_cast<double>(BoxComp->BoxTransitionDistance) : 0.0);
    Result->SetNumberField(TEXT("brightness"), static_cast<double>(CaptureComp->Brightness));
    Result->SetArrayField(TEXT("box_extent"), ScaleJson);
    Result->SetStringField(TEXT("cubemap"), CaptureComp->Cubemap ? CaptureComp->Cubemap->GetPathName() : FString());
    Result->SetStringField(TEXT("reflection_source_type"),
        CaptureComp->ReflectionSourceType == EReflectionSourceType::SpecifiedCubemap ? TEXT("SpecifiedCubemap") : TEXT("CapturedScene"));
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_scene_capture_2d_properties - Set CaptureSource, ProjectionType, FOVAngle, TextureTarget and bCaptureEveryFrame of a SceneCapture2D actor's capture component, found by label or name in the editor world, without capturing or saving. Beyond set_properties it adds only the label lookup and lenient enum names (SCS_ prefix optional, case-insensitive); write any other field with set_properties. An unknown capture_source or projection_type, or a texture_target that does not load as a TextureRenderTarget2D, fails the call before anything is written. Returns each field read back with a *_set flag; get_scene_capture_2d_properties reads them.
//   @param actor_name [!] - Label or internal name of the SceneCapture2D actor
//   @param capture_source [string] - ESceneCaptureSource (e.g. SceneColorHDR | FinalColorLDR | FinalColorHDR | SceneDepth); the SCS_ prefix is optional; any other value fails the call and nothing is changed
//   @param projection_type [string] - Projection mode, "Perspective" or "Orthographic" (case-insensitive); any other value fails the call and nothing is changed; omit to keep the current one
//   @param fov_angle [number] - Field-of-view angle in degrees (FOVAngle field)
//   @param texture_target [string] - Path to a UTextureRenderTarget2D to assign; a path that does not load as one fails the call and nothing is changed
//   @param capture_every_frame [boolean] - Whether the component captures every frame (bCaptureEveryFrame)
//   @returns { actor_name: string, actor_internal_name: string, capture_source_set: bool, capture_source: string, projection_type_set: bool, projection_type: string, fov_angle_set: bool, fov_angle: number, texture_target_set: bool, texture_target: string, has_render_target: bool, capture_every_frame_set: bool, capture_every_frame: bool }
TSharedPtr<FJsonObject> HandleSetSceneCapture2DProperties(const TSharedPtr<FJsonObject>& Params)
{
    ASceneCapture2D* CaptureActor = nullptr;
    USceneCaptureComponent2D* CaptureComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = ReflectionCapture_ResolveSceneCapture(Params, CaptureActor, CaptureComp))
    {
        return Error;
    }

    // The three values that can be refused are parsed before the first write, so a refused call leaves the capture
    // exactly as it was.
    bool bCaptureSourceSet = false;
    ESceneCaptureSource NewSource = SCS_MAX;
    FString CaptureSourceStr;
    if (Params->TryGetStringField(TEXT("capture_source"), CaptureSourceStr) && !CaptureSourceStr.IsEmpty())
    {
        FString Key = CaptureSourceStr;
        if (Key.StartsWith(TEXT("SCS_"))) { Key = Key.RightChop(4); }
        Key = Key.ToLower();
        if      (Key == TEXT("scenecolorhdr"))        { NewSource = SCS_SceneColorHDR; }
        else if (Key == TEXT("scenecolorhdrnoalpha")) { NewSource = SCS_SceneColorHDRNoAlpha; }
        else if (Key == TEXT("finalcolorldr"))        { NewSource = SCS_FinalColorLDR; }
        else if (Key == TEXT("scenecolorscenedepth")) { NewSource = SCS_SceneColorSceneDepth; }
        else if (Key == TEXT("scenedepth"))           { NewSource = SCS_SceneDepth; }
        else if (Key == TEXT("devicedepth"))          { NewSource = SCS_DeviceDepth; }
        else if (Key == TEXT("normal"))               { NewSource = SCS_Normal; }
        else if (Key == TEXT("basecolor"))            { NewSource = SCS_BaseColor; }
        else if (Key == TEXT("finalcolorhdr"))        { NewSource = SCS_FinalColorHDR; }
        else if (Key == TEXT("finaltonecurvehdr"))    { NewSource = SCS_FinalToneCurveHDR; }
        else
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Unknown capture_source '%s' (expected SceneColorHDR|SceneColorHDRNoAlpha|FinalColorLDR|SceneColorSceneDepth|SceneDepth|DeviceDepth|Normal|BaseColor|FinalColorHDR|FinalToneCurveHDR); nothing was changed"),
                *CaptureSourceStr));
        bCaptureSourceSet = true;
    }

    bool bProjectionSet = false;
    ECameraProjectionMode::Type NewProjection = ECameraProjectionMode::Perspective;
    FString ProjectionStr;
    if (Params->TryGetStringField(TEXT("projection_type"), ProjectionStr) && !ProjectionStr.IsEmpty())
    {
        const FString P = ProjectionStr.ToLower();
        if      (P == TEXT("perspective"))  { NewProjection = ECameraProjectionMode::Perspective; }
        else if (P == TEXT("orthographic")) { NewProjection = ECameraProjectionMode::Orthographic; }
        else
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Unknown projection_type '%s' (expected Perspective|Orthographic); nothing was changed"), *ProjectionStr));
        bProjectionSet = true;
    }

    UTextureRenderTarget2D* NewTarget = nullptr;
    FString TexturePath;
    if (Params->TryGetStringField(TEXT("texture_target"), TexturePath) && !TexturePath.IsEmpty())
    {
        NewTarget = LoadObject<UTextureRenderTarget2D>(nullptr, *TexturePath);
        if (!NewTarget)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Failed to load render target: %s (expected a TextureRenderTarget2D asset path); nothing was changed"), *TexturePath));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSceneCapture2DProperties", "MCP: Set Scene Capture 2D Properties"));
    CaptureActor->Modify();
    CaptureComp->Modify();

    TSharedPtr<FJsonObject> Result = ReflectionCapture_StartReply(CaptureActor);

    // capture_source (base USceneCaptureComponent::CaptureSource) ---------------------
    if (bCaptureSourceSet)
    {
        CaptureComp->CaptureSource = NewSource;
    }
    Result->SetBoolField(TEXT("capture_source_set"), bCaptureSourceSet);
    Result->SetStringField(TEXT("capture_source"), ReflectionCapture_CaptureSourceName(CaptureComp->CaptureSource.GetValue()));

    // projection_type (USceneCaptureComponent2D::ProjectionType) ----------------------
    if (bProjectionSet)
    {
        CaptureComp->ProjectionType = NewProjection;
    }
    Result->SetBoolField(TEXT("projection_type_set"), bProjectionSet);
    Result->SetStringField(TEXT("projection_type"),
        CaptureComp->ProjectionType == ECameraProjectionMode::Orthographic ? TEXT("Orthographic") : TEXT("Perspective"));

    // fov_angle (USceneCaptureComponent2D::FOVAngle — real field name) ----------------
    bool bFovSet = false;
    double RequestedFov = 0.0;
    if (Params->TryGetNumberField(TEXT("fov_angle"), RequestedFov))
    {
        CaptureComp->FOVAngle = static_cast<float>(RequestedFov);
        bFovSet = true;
    }
    Result->SetBoolField(TEXT("fov_angle_set"), bFovSet);
    Result->SetNumberField(TEXT("fov_angle"), static_cast<double>(CaptureComp->FOVAngle));

    // texture_target (USceneCaptureComponent2D::TextureTarget) ------------------------
    bool bTextureSet = false;
    if (NewTarget)
    {
        CaptureComp->TextureTarget = NewTarget;
        bTextureSet = true;
    }
    Result->SetBoolField(TEXT("texture_target_set"), bTextureSet);
    Result->SetStringField(TEXT("texture_target"), CaptureComp->TextureTarget ? CaptureComp->TextureTarget->GetPathName() : FString());
    Result->SetBoolField(TEXT("has_render_target"), CaptureComp->TextureTarget != nullptr);

    // capture_every_frame (base USceneCaptureComponent::bCaptureEveryFrame bitfield) --
    bool bCaptureEveryFrameSet = false;
    bool RequestedEveryFrame = false;
    if (Params->TryGetBoolField(TEXT("capture_every_frame"), RequestedEveryFrame))
    {
        CaptureComp->bCaptureEveryFrame = RequestedEveryFrame ? 1 : 0;
        bCaptureEveryFrameSet = true;
    }
    Result->SetBoolField(TEXT("capture_every_frame_set"), bCaptureEveryFrameSet);
    Result->SetBoolField(TEXT("capture_every_frame"), CaptureComp->bCaptureEveryFrame != 0);

    return mcp_common::MakeOkReply(Result);
}

// MCP:get_scene_capture_2d_properties - Read a SceneCapture2D actor's USceneCaptureComponent2D, found by label or internal name in the editor world: CaptureSource, TextureTarget path, FOVAngle, ProjectionType, OrthoWidth and the bCaptureEveryFrame / bCaptureOnMovement bitfields. A curated round-trip view for set_scene_capture_2d_properties, not every property; get_properties (internal names only) reads any other UPROPERTY on the component. Pure reader -- no Modify, no capture, no save.
//   @param actor_name [!] - Label or internal name of the SceneCapture2D actor
//   @returns { actor_name: string, component_name: string, texture_target: string, fov_angle: number, capture_source: string, capture_every_frame: bool, capture_on_movement: bool, projection_type: string, ortho_width: number }
TSharedPtr<FJsonObject> HandleGetSceneCapture2DProperties(const TSharedPtr<FJsonObject>& Params)
{
    ASceneCapture2D* CaptureActor = nullptr;
    USceneCaptureComponent2D* CaptureComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = ReflectionCapture_ResolveSceneCapture(Params, CaptureActor, CaptureComp))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), CaptureActor->GetActorLabel());
    Result->SetStringField(TEXT("component_name"), CaptureComp->GetName());
    Result->SetStringField(TEXT("texture_target"), CaptureComp->TextureTarget ? CaptureComp->TextureTarget->GetPathName() : FString());
    Result->SetNumberField(TEXT("fov_angle"), static_cast<double>(CaptureComp->FOVAngle));
    Result->SetStringField(TEXT("capture_source"), ReflectionCapture_CaptureSourceName(CaptureComp->CaptureSource.GetValue()));
    Result->SetBoolField(TEXT("capture_every_frame"), CaptureComp->bCaptureEveryFrame != 0);
    Result->SetBoolField(TEXT("capture_on_movement"), CaptureComp->bCaptureOnMovement != 0);
    Result->SetStringField(TEXT("projection_type"),
        CaptureComp->ProjectionType == ECameraProjectionMode::Orthographic ? TEXT("Orthographic") : TEXT("Perspective"));
    Result->SetNumberField(TEXT("ortho_width"), static_cast<double>(CaptureComp->OrthoWidth));
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_reflection_capture_properties"), &HandleGetReflectionCaptureProperties },
    { TEXT("get_scene_capture_2d_properties"), &HandleGetSceneCapture2DProperties },
    { TEXT("set_lumen_gi_quality"), &HandleSetLumenGIQuality },
    { TEXT("set_reflection_capture_properties"), &HandleSetReflectionCaptureProperties },
    { TEXT("set_scene_capture_2d_properties"), &HandleSetSceneCapture2DProperties },
    { TEXT("spawn_box_reflection_capture"), &HandleSpawnBoxReflectionCapture },
    { TEXT("spawn_scene_capture_2d"), &HandleSpawnSceneCapture2D },
    { TEXT("spawn_sphere_reflection_capture"), &HandleSpawnSphereReflectionCapture },
    { TEXT("update_reflection_captures"), &HandleUpdateReflectionCaptures },
};
} // namespace UEAgentReflectionCaptureCommands

#undef LOCTEXT_NAMESPACE
