#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentPropertyJson.h"

#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"

// Environment actors / components (all in the Engine module).
#include "Components/SkyAtmosphereComponent.h"   // ASkyAtmosphere, USkyAtmosphereComponent
#include "Components/VolumetricCloudComponent.h"  // AVolumetricCloud, UVolumetricCloudComponent
#include "Engine/ExponentialHeightFog.h"          // AExponentialHeightFog
#include "Components/ExponentialHeightFogComponent.h"
#include "Engine/SkyLight.h"                       // ASkyLight
#include "Components/SkyLightComponent.h"
#include "Engine/PostProcessVolume.h"             // APostProcessVolume
#include "Engine/Scene.h"                          // FPostProcessSettings, EAutoExposureMethod
#include "Engine/Texture.h"                        // UTexture (ColorGradingLUT path in get_post_process_settings)
#include "Engine/EngineTypes.h"                    // EDynamicGlobalIlluminationMethod

#define LOCTEXT_NAMESPACE "UEAgentEnvironment"

/**
 * Environment-actor commands: spawn sky atmosphere, volumetric cloud, exponential height fog, sky light and
 * post-process volume actors into the editor world, read and tune the properties of the first four, and
 * set and read the post-process settings of a PostProcessVolume, color grading included.
 */
namespace UEAgentEnvironmentCommands
{

// MCP-DOMAIN: Lighting

namespace UEAgentEnvironmentImpl
{
    // The fields every spawn command reports first: the new actor's label, internal name and class.
    TSharedPtr<FJsonObject> SpawnedActorData(const AActor* Actor)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
        Result->SetStringField(TEXT("actor_internal_name"), Actor->GetName());
        Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
        return Result;
    }

    // Resolves the PostProcessVolume the post-process commands work on: the actor_name is looked up in the
    // editor world (no editor means no world, which reports as "not found") and must be a volume.
    // Returns nullptr with OutError set when either step fails.
    APostProcessVolume* FindTargetVolume(const FString& ActorName, TSharedPtr<FJsonObject>& OutError)
    {
        UWorld* EditorWorld = mcp_common::GetEditorWorld();
        AActor* Named = mcp_common::FindActorByName(EditorWorld, ActorName);
        if (Named == nullptr)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
            return nullptr;
        }

        APostProcessVolume* Volume = Cast<APostProcessVolume>(Named);
        if (Volume == nullptr)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' is not a PostProcessVolume"), *ActorName));
        }
        return Volume;
    }
}

// MCP:spawn_sky_atmosphere - Spawn a new SkyAtmosphere actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one, so each call adds another; nothing is saved (save_level). spawn_actor has no atmosphere type. Returns actor_name (the label), actor_internal_name, actor_class and has_component. Tune it with set_sky_atmosphere_properties; the rest of a sky: spawn_sky_light, spawn_volumetric_cloud, spawn_exponential_height_fog.
//   @param location - [X, Y, Z] world location in cm; defaults to the origin. Any other value (an object, another length) is an error and nothing is spawned.
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_component: bool, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnSkyAtmosphere(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    const FScopedTransaction Transaction(LOCTEXT("SpawnSkyAtmosphere", "MCP: Spawn Sky Atmosphere"));
    ASkyAtmosphere* Actor = World->SpawnActor<ASkyAtmosphere>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn SkyAtmosphere actor"));

    const bool bHasComponent = (Actor->GetComponent() != nullptr);

    TSharedPtr<FJsonObject> Result = UEAgentEnvironmentImpl::SpawnedActorData(Actor);
    Result->SetBoolField(TEXT("has_component"), bHasComponent);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:spawn_volumetric_cloud - Spawn a new VolumetricCloud actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one, so each call adds another; nothing is saved (save_level). spawn_actor has no cloud type. Returns actor_name (the label), actor_internal_name, actor_class and has_component. Tune it with set_volumetric_cloud_properties; the rest of a sky: spawn_sky_atmosphere, spawn_sky_light, spawn_exponential_height_fog.
//   @param location - [X, Y, Z] world location in cm; defaults to the origin. Any other value (an object, another length) is an error and nothing is spawned.
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, has_component: bool, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnVolumetricCloud(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    const FScopedTransaction Transaction(LOCTEXT("SpawnVolumetricCloud", "MCP: Spawn Volumetric Cloud"));
    AVolumetricCloud* Actor = World->SpawnActor<AVolumetricCloud>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn VolumetricCloud actor"));

    // AVolumetricCloud has no public component getter; resolve it via the component class.
    const bool bHasComponent = (Actor->FindComponentByClass<UVolumetricCloudComponent>() != nullptr);

    TSharedPtr<FJsonObject> Result = UEAgentEnvironmentImpl::SpawnedActorData(Actor);
    Result->SetBoolField(TEXT("has_component"), bHasComponent);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:spawn_exponential_height_fog - Spawn a new ExponentialHeightFog actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one, so each call adds another fog actor (not idempotent); nothing is saved (save_level). spawn_actor has no fog type, and this one also seeds fog_density: when given, it is written to its UExponentialHeightFogComponent with SetFogDensity and density_set turns true; either way the reply reads fog_density back from the component, so without the param it shows the class default. No editor world, or a failed spawn, is an error. actor_name is the label and actor_internal_name the object name. Tune or inspect an existing fog with set_exponential_height_fog_properties and get_exponential_height_fog_properties; other atmosphere actors: spawn_sky_atmosphere, spawn_volumetric_cloud or spawn_sky_light.
//   @param location - Optional [X, Y, Z] world location in cm (defaults to origin); any other value is an error and nothing is spawned
//   @param fog_density - Optional fog density multiplier; if supplied, applied to the component and read back
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, density_set: bool, fog_density: number, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnExponentialHeightFog(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    double FogDensity = 0.0;
    const bool bHasDensity = Params->TryGetNumberField(TEXT("fog_density"), FogDensity);

    const FScopedTransaction Transaction(LOCTEXT("SpawnExpHeightFog", "MCP: Spawn Exponential Height Fog"));
    AExponentialHeightFog* Actor = World->SpawnActor<AExponentialHeightFog>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ExponentialHeightFog actor"));

    double AppliedDensity = 0.0;
    bool bDensitySet = false;
    if (UExponentialHeightFogComponent* Comp = Actor->GetComponent())
    {
        if (bHasDensity)
        {
            Comp->Modify();
            Comp->SetFogDensity(static_cast<float>(FogDensity));
            bDensitySet = true;
        }
        // Read back the value from the component to prove the effect.
        AppliedDensity = Comp->FogDensity;
    }

    TSharedPtr<FJsonObject> Result = UEAgentEnvironmentImpl::SpawnedActorData(Actor);
    Result->SetBoolField(TEXT("density_set"), bDensitySet);
    Result->SetNumberField(TEXT("fog_density"), AppliedDensity);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:spawn_sky_light - Spawn a new SkyLight actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one; nothing is saved. intensity, when given, is set on its USkyLightComponent, and recapture=true calls RecaptureSky so the light refreshes its captured sky lighting now. spawn_actor(type='SkyLight') also spawns one and adds color, a name and a PIE world; pick this one for the recapture. The reply reads intensity back from the component and names the actor (actor_name is the label). Tune an existing sky light with set_sky_light_properties; spawn_sky_atmosphere adds the atmosphere.
//   @param location - Optional [X, Y, Z] world location (defaults to origin)
//   @param intensity - Optional SkyLight intensity multiplier; if supplied, applied to the component
//   @param recapture - If true, call RecaptureSky on the component (optional, default false)
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, intensity_set: bool, intensity: number, recaptured: bool, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnSkyLight(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    double Intensity = 0.0;
    const bool bHasIntensity = Params->TryGetNumberField(TEXT("intensity"), Intensity);
    bool bRecapture = false;
    Params->TryGetBoolField(TEXT("recapture"), bRecapture);

    const FScopedTransaction Transaction(LOCTEXT("SpawnSkyLight", "MCP: Spawn Sky Light"));
    ASkyLight* Actor = World->SpawnActor<ASkyLight>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn SkyLight actor"));

    double AppliedIntensity = 0.0;
    bool bIntensitySet = false;
    bool bRecaptured = false;
    if (USkyLightComponent* Comp = Actor->GetLightComponent())
    {
        if (bHasIntensity)
        {
            Comp->Modify();
            Comp->SetIntensity(static_cast<float>(Intensity));
            bIntensitySet = true;
        }
        if (bRecapture)
        {
            Comp->RecaptureSky();
            bRecaptured = true;
        }
        // Read back the live intensity from the component.
        AppliedIntensity = Comp->Intensity;
    }

    TSharedPtr<FJsonObject> Result = UEAgentEnvironmentImpl::SpawnedActorData(Actor);
    Result->SetBoolField(TEXT("intensity_set"), bIntensitySet);
    Result->SetNumberField(TEXT("intensity"), AppliedIntensity);
    Result->SetBoolField(TEXT("recaptured"), bRecaptured);
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:spawn_post_process_volume - Spawn a new PostProcessVolume actor in the editor world at location in one undo step, unbound (affecting the whole world) by default; not saved (save_level). It builds NO brush, so unbound=false covers no space: for a bounded volume use spawn_box_volume(volume_class='PostProcessVolume'). spawn_actor has no post-process type. Returns actor_name (the label), actor_internal_name, unbound and enabled. Configure it with set_post_process_setting or set_post_process_color_grading.
//   @param location - [X, Y, Z] world location in cm; defaults to the origin. Any other value (an object, another length) is an error and nothing is spawned.
//   @param unbound - true (default) makes the volume affect the whole world; false limits it to its brush, which this spawn leaves without geometry, so it then covers no space.
//   @returns { actor_name: string, actor_internal_name: string, actor_class: string, unbound: bool, enabled: bool, spawned: bool }

TSharedPtr<FJsonObject> HandleSpawnPostProcessVolume(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("Failed to get editor world"))) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));

    bool bUnbound = true;
    Params->TryGetBoolField(TEXT("unbound"), bUnbound);

    const FScopedTransaction Transaction(LOCTEXT("SpawnPostProcessVolume", "MCP: Spawn Post Process Volume"));
    APostProcessVolume* Actor = World->SpawnActor<APostProcessVolume>(Location, FRotator::ZeroRotator);
    if (!Actor)
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn PostProcessVolume actor"));

    Actor->Modify();
    Actor->bUnbound = bUnbound ? 1 : 0;
    // Read back the bitfield to prove the effect.
    const bool bUnboundApplied = (Actor->bUnbound != 0);

    TSharedPtr<FJsonObject> Result = UEAgentEnvironmentImpl::SpawnedActorData(Actor);
    Result->SetBoolField(TEXT("unbound"), bUnboundApplied);
    Result->SetBoolField(TEXT("enabled"), (Actor->bEnabled != 0));
    Result->SetBoolField(TEXT("spawned"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_post_process_setting - Sets a post-process setting on a PostProcessVolume (sets bOverride flag + field, reads back).          exposure (auto_exposure_bias), exposure_min, exposure_max, exposure_method         (0=Histogram,1=Basic,2=Manual), lumen_gi / dynamic_global_illumination_method         (0=None,1=Lumen,2=ScreenSpace,3=Plugin)
//   @param actor_name - Label/name of the target PostProcessVolume actor
//   @param setting - One of: bloom_intensity, vignette_intensity, ambient_occlusion_intensity,
//   @param value - Numeric value to apply (enum index for method/gi settings)
//   @returns { actor_name: string, setting: string, canonical_setting: string, requested_value: number, applied_value: number, applied: bool }

TSharedPtr<FJsonObject> HandleSetPostProcessSetting(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName, Setting;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("setting"), Setting, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double Value = 0.0;
    if (!Params->TryGetNumberField(TEXT("value"), Value))
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (number)"));

    TSharedPtr<FJsonObject> LookupError;
    APostProcessVolume* PPV = UEAgentEnvironmentImpl::FindTargetVolume(ActorName, LookupError);
    if (!PPV)
    {
        return LookupError;
    }

    const FString Key = Setting.ToLower();

    const FScopedTransaction Transaction(LOCTEXT("SetPostProcessSetting", "MCP: Set Post Process Setting"));
    PPV->Modify();

    FPostProcessSettings& S = PPV->Settings;

    // Map of friendly setting names -> (set bOverride + value) then read-back.
    // Each branch sets bOverride_<X>=1 and the corresponding field, then reports
    // the value read back out of the struct so success is proven by readback.
    double Applied = 0.0;
    FString Canonical;
    bool bHandled = true;

    if (Key == TEXT("bloom_intensity") || Key == TEXT("bloomintensity"))
    {
        S.bOverride_BloomIntensity = 1;
        S.BloomIntensity = static_cast<float>(Value);
        Applied = S.BloomIntensity;
        Canonical = TEXT("BloomIntensity");
    }
    else if (Key == TEXT("vignette_intensity") || Key == TEXT("vignette") || Key == TEXT("vignetteintensity"))
    {
        S.bOverride_VignetteIntensity = 1;
        S.VignetteIntensity = static_cast<float>(Value);
        Applied = S.VignetteIntensity;
        Canonical = TEXT("VignetteIntensity");
    }
    else if (Key == TEXT("ambient_occlusion_intensity") || Key == TEXT("ao") ||
             Key == TEXT("ambientocclusionintensity") || Key == TEXT("ambient_occlusion"))
    {
        S.bOverride_AmbientOcclusionIntensity = 1;
        S.AmbientOcclusionIntensity = static_cast<float>(Value);
        Applied = S.AmbientOcclusionIntensity;
        Canonical = TEXT("AmbientOcclusionIntensity");
    }
    else if (Key == TEXT("exposure") || Key == TEXT("exposure_bias") ||
             Key == TEXT("auto_exposure_bias") || Key == TEXT("autoexposurebias"))
    {
        // Exposure compensation (EV bias).
        S.bOverride_AutoExposureBias = 1;
        S.AutoExposureBias = static_cast<float>(Value);
        Applied = S.AutoExposureBias;
        Canonical = TEXT("AutoExposureBias");
    }
    else if (Key == TEXT("exposure_min") || Key == TEXT("auto_exposure_min_brightness") ||
             Key == TEXT("min_brightness"))
    {
        S.bOverride_AutoExposureMinBrightness = 1;
        S.AutoExposureMinBrightness = static_cast<float>(Value);
        Applied = S.AutoExposureMinBrightness;
        Canonical = TEXT("AutoExposureMinBrightness");
    }
    else if (Key == TEXT("exposure_max") || Key == TEXT("auto_exposure_max_brightness") ||
             Key == TEXT("max_brightness"))
    {
        S.bOverride_AutoExposureMaxBrightness = 1;
        S.AutoExposureMaxBrightness = static_cast<float>(Value);
        Applied = S.AutoExposureMaxBrightness;
        Canonical = TEXT("AutoExposureMaxBrightness");
    }
    else if (Key == TEXT("exposure_method") || Key == TEXT("auto_exposure_method") ||
             Key == TEXT("metering_mode"))
    {
        // value: 0=Histogram, 1=Basic, 2=Manual (matches EAutoExposureMethod order).
        const int32 Mode = FMath::Clamp(static_cast<int32>(Value), 0, (int32)AEM_MAX - 1);
        S.bOverride_AutoExposureMethod = 1;
        S.AutoExposureMethod = static_cast<EAutoExposureMethod>(Mode);
        Applied = static_cast<double>(static_cast<int32>(S.AutoExposureMethod.GetValue()));
        Canonical = TEXT("AutoExposureMethod");
    }
    else if (Key == TEXT("lumen_gi") || Key == TEXT("gi") || Key == TEXT("global_illumination") ||
             Key == TEXT("dynamic_global_illumination_method") || Key == TEXT("gi_method"))
    {
        // value: 0=None, 1=Lumen, 2=ScreenSpace, 3=Plugin (EDynamicGlobalIlluminationMethod).
        const int32 Method = FMath::Clamp(static_cast<int32>(Value), 0, 3);
        S.bOverride_DynamicGlobalIlluminationMethod = 1;
        S.DynamicGlobalIlluminationMethod = static_cast<EDynamicGlobalIlluminationMethod::Type>(Method);
        Applied = static_cast<double>(static_cast<int32>(S.DynamicGlobalIlluminationMethod.GetValue()));
        Canonical = TEXT("DynamicGlobalIlluminationMethod");
    }
    else
    {
        bHandled = false;
    }

    if (!bHandled)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unsupported post-process setting '%s'. Supported: bloom_intensity, vignette_intensity, ")
            TEXT("ambient_occlusion_intensity, exposure (auto_exposure_bias), exposure_min, exposure_max, ")
            TEXT("exposure_method, lumen_gi (dynamic_global_illumination_method)"), *Setting));
    }

    // Ensure render state / editor reflects the change.
    PPV->MarkComponentsRenderStateDirty();
    PPV->PostEditChange();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), PPV->GetActorLabel());
    Result->SetStringField(TEXT("setting"), Setting);
    Result->SetStringField(TEXT("canonical_setting"), Canonical);
    Result->SetNumberField(TEXT("requested_value"), Value);
    Result->SetNumberField(TEXT("applied_value"), Applied);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// ===========================================================================
// Post-process color grading + a full reader.
// set_post_process_setting only covered 9 scalars and there was no reader. The
// color-grading block (FVector4 Saturation/Contrast/Gamma/Gain/Offset, each gated
// by its own bOverride_* flag) was entirely unreachable. Same APostProcessVolume
// target + Modify()/render-state-dirty pattern as set_post_process_setting.
// ===========================================================================
namespace
{
    TArray<TSharedPtr<FJsonValue>> Vec4ToJson(const FVector4& V)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        A.Add(MakeShared<FJsonValueNumber>(V.X));
        A.Add(MakeShared<FJsonValueNumber>(V.Y));
        A.Add(MakeShared<FJsonValueNumber>(V.Z));
        A.Add(MakeShared<FJsonValueNumber>(V.W));
        return A;
    }

    // {value: [R,G,B,W], overridden: bool} for one color-grading channel.
    TSharedPtr<FJsonObject> ColorGradeJson(const FVector4& V, bool bOverridden)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetArrayField(TEXT("value"), Vec4ToJson(V));
        J->SetBoolField(TEXT("overridden"), bOverridden);
        return J;
    }

    // {value: float, overridden: bool} for one scalar setting.
    TSharedPtr<FJsonObject> ScalarJson(double Value, bool bOverridden)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("value"), Value);
        J->SetBoolField(TEXT("overridden"), bOverridden);
        return J;
    }
}

// MCP:set_post_process_color_grading - Set the global AND per-range (shadows/midtones/highlights) color-grading wheels plus white balance and the range pivots on a PostProcessVolume.  Each wheel is an FVector4 [R, G, B, master]; white balance and pivots are scalars — set_post_process_setting never covered these. Setting a field also turns on its bOverride flag (so it takes effect) and reads the value back. Only the supplied fields change.
//   @param actor_name - Label/name of the target PostProcessVolume actor.
//   @param saturation [array] - Optional [R, G, B, master] saturation (default master 1.0; neutral is [1,1,1,1]).
//   @param contrast [array] - Optional [R, G, B, master] contrast (neutral [1,1,1,1]).
//   @param gamma [array] - Optional [R, G, B, master] gamma (neutral [1,1,1,1]).
//   @param gain [array] - Optional [R, G, B, master] gain (neutral [1,1,1,1]).
//   @param offset [array] - Optional [R, G, B, master] offset (default master 0.0; neutral is [0,0,0,0]).
//   @param saturation_shadows [array] - Optional [R, G, B, master] saturation for the shadows range (neutral [1,1,1,1]).
//   @param saturation_midtones [array] - Optional [R, G, B, master] saturation for the midtones range (neutral [1,1,1,1]).
//   @param saturation_highlights [array] - Optional [R, G, B, master] saturation for the highlights range (neutral [1,1,1,1]).
//   @param contrast_shadows [array] - Optional [R, G, B, master] contrast for the shadows range (neutral [1,1,1,1]).
//   @param contrast_midtones [array] - Optional [R, G, B, master] contrast for the midtones range (neutral [1,1,1,1]).
//   @param contrast_highlights [array] - Optional [R, G, B, master] contrast for the highlights range (neutral [1,1,1,1]).
//   @param white_temp [number] - Optional white balance temperature (Kelvin, e.g. 6500 neutral).
//   @param white_tint [number] - Optional white balance tint.
//   @param shadows_max [number] - Optional pivot: upper bound of the shadows range.
//   @param highlights_min [number] - Optional pivot: lower bound of the highlights range.
//   @returns { actor_name: string, settings_applied: object (wheels as arrays, white_temp/white_tint/shadows_max/highlights_min as numbers), applied_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetPostProcessColorGrading(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    TSharedPtr<FJsonObject> LookupError;
    APostProcessVolume* PPV = UEAgentEnvironmentImpl::FindTargetVolume(ActorName, LookupError);
    if (!PPV)
        return LookupError;

    const FScopedTransaction Transaction(LOCTEXT("SetPostProcessColorGrading", "MCP: Set Post Process Color Grading"));
    PPV->Modify();
    FPostProcessSettings& S = PPV->Settings;

    int32 AppliedCount = 0;
    auto TryGetCG = [&](const TCHAR* Field, double DefaultW, FVector4& Out) -> bool
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        return Params->TryGetArrayField(Field, Arr) && ParseColorVec4(Arr, DefaultW, Out);
    };

    FVector4 V;
    if (TryGetCG(TEXT("saturation"), 1.0, V)) { S.ColorSaturation = V; S.bOverride_ColorSaturation = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("contrast"),   1.0, V)) { S.ColorContrast   = V; S.bOverride_ColorContrast   = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("gamma"),      1.0, V)) { S.ColorGamma      = V; S.bOverride_ColorGamma      = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("gain"),       1.0, V)) { S.ColorGain       = V; S.bOverride_ColorGain       = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("offset"),     0.0, V)) { S.ColorOffset     = V; S.bOverride_ColorOffset     = 1; ++AppliedCount; }

    if (TryGetCG(TEXT("saturation_shadows"),    1.0, V)) { S.ColorSaturationShadows    = V; S.bOverride_ColorSaturationShadows    = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("saturation_midtones"),   1.0, V)) { S.ColorSaturationMidtones   = V; S.bOverride_ColorSaturationMidtones   = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("saturation_highlights"), 1.0, V)) { S.ColorSaturationHighlights = V; S.bOverride_ColorSaturationHighlights = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("contrast_shadows"),      1.0, V)) { S.ColorContrastShadows      = V; S.bOverride_ColorContrastShadows      = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("contrast_midtones"),     1.0, V)) { S.ColorContrastMidtones     = V; S.bOverride_ColorContrastMidtones     = 1; ++AppliedCount; }
    if (TryGetCG(TEXT("contrast_highlights"),   1.0, V)) { S.ColorContrastHighlights   = V; S.bOverride_ColorContrastHighlights   = 1; ++AppliedCount; }

    double Scalar = 0.0;
    if (Params->TryGetNumberField(TEXT("white_temp"),     Scalar)) { S.WhiteTemp                   = Scalar; S.bOverride_WhiteTemp                   = 1; ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("white_tint"),     Scalar)) { S.WhiteTint                   = Scalar; S.bOverride_WhiteTint                   = 1; ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("shadows_max"),    Scalar)) { S.ColorCorrectionShadowsMax   = Scalar; S.bOverride_ColorCorrectionShadowsMax   = 1; ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("highlights_min"), Scalar)) { S.ColorCorrectionHighlightsMin = Scalar; S.bOverride_ColorCorrectionHighlightsMin = 1; ++AppliedCount; }

    if (AppliedCount == 0)
        return mcp_common::MakeErrorReply(
            TEXT("No color grading provided. Supply at least one of: saturation, contrast, gamma, gain, offset, saturation_shadows/midtones/highlights, contrast_shadows/midtones/highlights (each [R,G,B,master]), or white_temp, white_tint, shadows_max, highlights_min (numbers)"));

    PPV->MarkComponentsRenderStateDirty();
    PPV->PostEditChange();

    // Verified read-back of the current values.
    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    Applied->SetArrayField(TEXT("saturation"), Vec4ToJson(S.ColorSaturation));
    Applied->SetArrayField(TEXT("contrast"),   Vec4ToJson(S.ColorContrast));
    Applied->SetArrayField(TEXT("gamma"),      Vec4ToJson(S.ColorGamma));
    Applied->SetArrayField(TEXT("gain"),       Vec4ToJson(S.ColorGain));
    Applied->SetArrayField(TEXT("offset"),     Vec4ToJson(S.ColorOffset));
    Applied->SetArrayField(TEXT("saturation_shadows"),    Vec4ToJson(S.ColorSaturationShadows));
    Applied->SetArrayField(TEXT("saturation_midtones"),   Vec4ToJson(S.ColorSaturationMidtones));
    Applied->SetArrayField(TEXT("saturation_highlights"), Vec4ToJson(S.ColorSaturationHighlights));
    Applied->SetArrayField(TEXT("contrast_shadows"),      Vec4ToJson(S.ColorContrastShadows));
    Applied->SetArrayField(TEXT("contrast_midtones"),     Vec4ToJson(S.ColorContrastMidtones));
    Applied->SetArrayField(TEXT("contrast_highlights"),   Vec4ToJson(S.ColorContrastHighlights));
    Applied->SetNumberField(TEXT("white_temp"),     S.WhiteTemp);
    Applied->SetNumberField(TEXT("white_tint"),     S.WhiteTint);
    Applied->SetNumberField(TEXT("shadows_max"),    S.ColorCorrectionShadowsMax);
    Applied->SetNumberField(TEXT("highlights_min"), S.ColorCorrectionHighlightsMin);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), PPV->GetActorLabel());
    Result->SetObjectField(TEXT("settings_applied"), Applied);
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_post_process_settings - Read a PostProcessVolume without changing it: the actor-level enabled / unbounded / priority, its color grading and the scalar settings set_post_process_setting manages, each with its override flag, plus film grain, motion blur, the auto-exposure method and the color grading LUT.  Every graded or scalar value comes with `overridden` (its bOverride_* flag) so you can see what is actually active.
//   @param actor_name - Label/name of the target PostProcessVolume actor.
//   @returns { actor_name: string, enabled: bool, unbounded: bool, priority: number, color_grading: { saturation, contrast, gamma, gain, offset, saturation_shadows, saturation_midtones, saturation_highlights, contrast_shadows, contrast_midtones, contrast_highlights (each {value:[float], overridden}) }, white_balance: { white_temp, white_tint, shadows_max, highlights_min (each {value, overridden}) }, scalars: { bloom_intensity, vignette_intensity, ambient_occlusion_intensity, auto_exposure_bias, auto_exposure_min_brightness, auto_exposure_max_brightness (each {value, overridden}) }, film_grain_intensity: number, motion_blur_amount: number, color_grading_lut_path: string, exposure_method: int, auto_exposure_method: string, dynamic_global_illumination_method: int }
TSharedPtr<FJsonObject> HandleGetPostProcessSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    TSharedPtr<FJsonObject> LookupError;
    APostProcessVolume* PPV = UEAgentEnvironmentImpl::FindTargetVolume(ActorName, LookupError);
    if (!PPV)
        return LookupError;

    const FPostProcessSettings& S = PPV->Settings;

    TSharedPtr<FJsonObject> CG = MakeShared<FJsonObject>();
    CG->SetObjectField(TEXT("saturation"), ColorGradeJson(S.ColorSaturation, S.bOverride_ColorSaturation != 0));
    CG->SetObjectField(TEXT("contrast"),   ColorGradeJson(S.ColorContrast,   S.bOverride_ColorContrast != 0));
    CG->SetObjectField(TEXT("gamma"),      ColorGradeJson(S.ColorGamma,      S.bOverride_ColorGamma != 0));
    CG->SetObjectField(TEXT("gain"),       ColorGradeJson(S.ColorGain,       S.bOverride_ColorGain != 0));
    CG->SetObjectField(TEXT("offset"),     ColorGradeJson(S.ColorOffset,     S.bOverride_ColorOffset != 0));
    CG->SetObjectField(TEXT("saturation_shadows"),    ColorGradeJson(S.ColorSaturationShadows,    S.bOverride_ColorSaturationShadows != 0));
    CG->SetObjectField(TEXT("saturation_midtones"),   ColorGradeJson(S.ColorSaturationMidtones,   S.bOverride_ColorSaturationMidtones != 0));
    CG->SetObjectField(TEXT("saturation_highlights"), ColorGradeJson(S.ColorSaturationHighlights, S.bOverride_ColorSaturationHighlights != 0));
    CG->SetObjectField(TEXT("contrast_shadows"),      ColorGradeJson(S.ColorContrastShadows,      S.bOverride_ColorContrastShadows != 0));
    CG->SetObjectField(TEXT("contrast_midtones"),     ColorGradeJson(S.ColorContrastMidtones,     S.bOverride_ColorContrastMidtones != 0));
    CG->SetObjectField(TEXT("contrast_highlights"),   ColorGradeJson(S.ColorContrastHighlights,   S.bOverride_ColorContrastHighlights != 0));

    TSharedPtr<FJsonObject> WhiteBalance = MakeShared<FJsonObject>();
    WhiteBalance->SetObjectField(TEXT("white_temp"),     ScalarJson(S.WhiteTemp,                    S.bOverride_WhiteTemp != 0));
    WhiteBalance->SetObjectField(TEXT("white_tint"),     ScalarJson(S.WhiteTint,                    S.bOverride_WhiteTint != 0));
    WhiteBalance->SetObjectField(TEXT("shadows_max"),    ScalarJson(S.ColorCorrectionShadowsMax,    S.bOverride_ColorCorrectionShadowsMax != 0));
    WhiteBalance->SetObjectField(TEXT("highlights_min"), ScalarJson(S.ColorCorrectionHighlightsMin, S.bOverride_ColorCorrectionHighlightsMin != 0));

    TSharedPtr<FJsonObject> Scalars = MakeShared<FJsonObject>();
    Scalars->SetObjectField(TEXT("bloom_intensity"), ScalarJson(S.BloomIntensity, S.bOverride_BloomIntensity != 0));
    Scalars->SetObjectField(TEXT("vignette_intensity"), ScalarJson(S.VignetteIntensity, S.bOverride_VignetteIntensity != 0));
    Scalars->SetObjectField(TEXT("ambient_occlusion_intensity"), ScalarJson(S.AmbientOcclusionIntensity, S.bOverride_AmbientOcclusionIntensity != 0));
    Scalars->SetObjectField(TEXT("auto_exposure_bias"), ScalarJson(S.AutoExposureBias, S.bOverride_AutoExposureBias != 0));
    Scalars->SetObjectField(TEXT("auto_exposure_min_brightness"), ScalarJson(S.AutoExposureMinBrightness, S.bOverride_AutoExposureMinBrightness != 0));
    Scalars->SetObjectField(TEXT("auto_exposure_max_brightness"), ScalarJson(S.AutoExposureMaxBrightness, S.bOverride_AutoExposureMaxBrightness != 0));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), PPV->GetActorLabel());
    Result->SetBoolField(TEXT("enabled"), PPV->bEnabled);
    Result->SetBoolField(TEXT("unbounded"), PPV->bUnbound);
    Result->SetNumberField(TEXT("priority"), PPV->Priority);
    Result->SetObjectField(TEXT("color_grading"), CG);
    Result->SetObjectField(TEXT("white_balance"), WhiteBalance);
    Result->SetObjectField(TEXT("scalars"), Scalars);
    Result->SetNumberField(TEXT("film_grain_intensity"), S.FilmGrainIntensity);
    Result->SetNumberField(TEXT("motion_blur_amount"), S.MotionBlurAmount);
    Result->SetStringField(TEXT("color_grading_lut_path"), S.ColorGradingLUT ? S.ColorGradingLUT->GetPathName() : FString());
    Result->SetNumberField(TEXT("exposure_method"), static_cast<int32>(S.AutoExposureMethod.GetValue()));
    if (const UEnum* ExposureEnum = StaticEnum<EAutoExposureMethod>())
    {
        Result->SetStringField(TEXT("auto_exposure_method"),
            ExposureEnum->GetNameStringByValue(static_cast<int64>(S.AutoExposureMethod.GetValue())));
    }
    Result->SetNumberField(TEXT("dynamic_global_illumination_method"), static_cast<int32>(S.DynamicGlobalIlluminationMethod.GetValue()));
    return mcp_common::MakeOkReply(Result);
}

// <generated:property-handlers> -- DO NOT EDIT BY HAND. Source of truth:
// Tools/property_descriptors/{sky_atmosphere,exponential_height_fog,volumetric_cloud,sky_light}.json
// Regenerate: python Tools/integrate_property_handlers.py

// Each set handler applies the supplied params through the component's ENGINE_API setters, which update
// its render state. All but the fog's change nothing on a Static component (AreDynamicDataChangesAllowed()),
// so those set handlers refuse one before writing; the components default to Movable (SkyLight: Stationary).
// Every param is checked before the first write. The readers read back the public fields the setters write.
namespace
{
    // The component GetComponent returns for the TActor the required actor_name param names in the editor
    // world. Returns nullptr with OutError set when the param is missing, no actor has that name, the actor
    // is not a TActor (Article and Kind name it in the error) or it has no component.
    template <typename TActor, typename TComponent>
    TComponent* PropertyHandlers_ResolveComponent(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* Article,
        const TCHAR* Kind,
        TComponent* (*GetComponent)(TActor&),
        FString& OutActorName,
        TSharedPtr<FJsonObject>& OutError)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_name"));
            return nullptr;
        }
        AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), OutActorName);
        if (!Actor)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *OutActorName));
            return nullptr;
        }
        TActor* Target = Cast<TActor>(Actor);
        if (!Target)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Actor '%s' is not %s %s"), *OutActorName, Article, Kind));
            return nullptr;
        }
        TComponent* Component = GetComponent(*Target);
        if (!Component)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("%s '%s' has no component"), Kind, *OutActorName));
        }
        return Component;
    }

    // How many of Supplied are true: the params a set call supplies.
    int32 PropertyHandlers_CountSupplied(std::initializer_list<bool> Supplied)
    {
        int32 Count = 0;
        for (const bool bSupplied : Supplied)
        {
            Count += bSupplied ? 1 : 0;
        }
        return Count;
    }

    // What Value holds, for the error of a param reader that expected something else.
    FString PropertyHandlers_DescribeJson(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        switch (Value->Type)
        {
        case EJson::String:  return FString::Printf(TEXT("the string '%s'"), *Value->AsString());
        case EJson::Number:  return FString::Printf(TEXT("the number %s"), *FString::SanitizeFloat(Value->AsNumber()));
        case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Array:   return FString::Printf(TEXT("an array of %d element(s)"), Value->AsArray().Num());
        case EJson::Object:  return TEXT("an object");
        default:             return TEXT("null");
        }
    }

    // The optional number param Key in Out, which stays unset when Key is absent or null. An error reply naming Key,
    // with nothing changed, when it holds anything but a finite JSON number within the float range.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadFloat(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<float>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        const double Number = Value->Type == EJson::Number ? Value->AsNumber() : 0.0;
        if (Value->Type != EJson::Number || !FMath::IsFinite(Number)
            || FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be a finite number within the float range (got %s). Nothing was changed."),
                Key, *PropertyHandlers_DescribeJson(Value)));
        }
        Out = static_cast<float>(Number);
        return nullptr;
    }

    // The optional bool param Key in Out, which stays unset when Key is absent or null. An error reply naming Key,
    // with nothing changed, when it holds anything but a JSON true or false.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadBool(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<bool>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        if (Value->Type != EJson::Boolean)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be true or false (got %s). Nothing was changed."), Key, *PropertyHandlers_DescribeJson(Value)));
        }
        Out = Value->AsBool();
        return nullptr;
    }

    // The optional color param Key, [R, G, B] or [R, G, B, A] with A defaulting to 1, in Out, which stays unset when
    // Key is absent or null. The error reply of mcp_common::ReadJsonLinearColor, naming Key, with nothing changed,
    // when it is not an array of 3 or 4 finite numbers.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadLinearColor(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<FLinearColor>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        FLinearColor Color;
        if (TSharedPtr<FJsonObject> Malformed = mcp_common::ReadJsonLinearColor(Params, Key, FLinearColor(0.0f, 0.0f, 0.0f, 1.0f), Color))
        {
            return Malformed;
        }
        Out = Color;
        return nullptr;
    }

    // The optional color param Key of 0-255 bytes, [R, G, B] or [R, G, B, A] with A defaulting to 255, in Out, which
    // stays unset when Key is absent or null. An error reply naming Key, with nothing changed, when it is not an array
    // of 3 or 4 whole numbers from 0 to 255: a fraction is most likely a linear 0-1 color, which would turn black.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadColorBytes(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<FColor>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        FString Problem;
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Value->TryGetArray(Items) || !Items || Items->Num() < 3 || Items->Num() > 4)
        {
            Problem = TEXT("got ") + PropertyHandlers_DescribeJson(Value);
        }
        uint8 Channels[4] = { 0, 0, 0, 255 };
        for (int32 Index = 0; Problem.IsEmpty() && Index < Items->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Item = (*Items)[Index];
            const double Number = Item.IsValid() && Item->Type == EJson::Number ? Item->AsNumber() : -1.0;
            if (Number < 0.0 || Number > 255.0 || Number != FMath::FloorToDouble(Number))
            {
                Problem = FString::Printf(TEXT("element %d is %s"), Index, *PropertyHandlers_DescribeJson(Item));
            }
            else
            {
                Channels[Index] = static_cast<uint8>(Number);
            }
        }
        if (!Problem.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be an array of 3 or 4 whole numbers from 0 to 255, [R, G, B] or [R, G, B, A] (%s). Nothing was changed."),
                Key, *Problem));
        }
        Out = FColor(Channels[0], Channels[1], Channels[2], Channels[3]);
        return nullptr;
    }

    // The refusal of a set call on Component when it is registered with Static mobility, and null for any other: the
    // engine setters change nothing on such a component (USceneComponent::AreDynamicDataChangesAllowed, protected,
    // outside a construction script), so a write would count fields that never reach it. Kind and ActorName name the
    // actor in the error.
    TSharedPtr<FJsonObject> PropertyHandlers_RefuseStaticComponent(
        const USceneComponent& Component, const TCHAR* Kind, const FString& ActorName)
    {
        if (!Component.IsRegistered() || Component.Mobility != EComponentMobility::Static)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s '%s' has Static mobility, and the engine's setters do not change a registered static component; ")
            TEXT("nothing was changed. Make its component Stationary or Movable first (USceneComponent::SetMobility, ")
            TEXT("e.g. call_function with component_class)."),
            Kind, *ActorName));
    }
}

namespace
{
    USkyAtmosphereComponent* PropertyHandlers_GetSkyAtmosphereComponent(ASkyAtmosphere& Actor)
    {
        return Actor.GetComponent();
    }

    TSharedPtr<FJsonObject> PropertyHandlers_SkyAtmosphereJson(const USkyAtmosphereComponent* Component)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetArrayField(TEXT("rayleigh_scattering"), mcp_common::ColorToJsonArray(Component->RayleighScattering));
        Properties->SetNumberField(TEXT("rayleigh_scattering_scale"), Component->RayleighScatteringScale);
        Properties->SetArrayField(TEXT("mie_scattering"), mcp_common::ColorToJsonArray(Component->MieScattering));
        Properties->SetNumberField(TEXT("mie_scattering_scale"), Component->MieScatteringScale);
        Properties->SetNumberField(TEXT("mie_anisotropy"), Component->MieAnisotropy);
        Properties->SetArrayField(TEXT("ground_albedo"), mcp_common::ColorToJsonArray(Component->GroundAlbedo));
        Properties->SetNumberField(TEXT("bottom_radius"), Component->BottomRadius);
        Properties->SetNumberField(TEXT("atmosphere_height"), Component->AtmosphereHeight);
        Properties->SetNumberField(TEXT("multi_scattering_factor"), Component->MultiScatteringFactor);
        Properties->SetNumberField(TEXT("aerial_perspective_view_distance_scale"), Component->AerialPespectiveViewDistanceScale);
        Properties->SetNumberField(TEXT("height_fog_contribution"), Component->HeightFogContribution);
        return Properties;
    }
}

// MCP:set_sky_atmosphere_properties - Set atmosphere fields (Rayleigh/Mie scattering, anisotropy, ground albedo, radius, height, multi-scattering, aerial perspective, height fog) of a SkyAtmosphere actor found by label or name, through the component's setters, which refresh its render state; set_properties writes any other field. Only supplied fields change, in one undo step; all are read back. Colors are linear [R,G,B(,A)], ground_albedo [R,G,B(,A)] whole 0-255 bytes. A malformed field, or a component of Static mobility (its setters change nothing; make it Movable first), is an error and nothing changes. Not saved.
//   @param actor_name - Label/name of the target SkyAtmosphere actor.
//   @param rayleigh_scattering [array] - Optional [R,G,B(,A)] Rayleigh scattering color.
//   @param rayleigh_scattering_scale [number] - Optional Rayleigh scattering scale.
//   @param mie_scattering [array] - Optional [R,G,B(,A)] Mie scattering color.
//   @param mie_scattering_scale [number] - Optional Mie scattering scale.
//   @param mie_anisotropy [number] - Optional Mie anisotropy (0..1).
//   @param ground_albedo [array] - Optional [R,G,B] or [R,G,B,A] ground albedo, each a whole number from 0 to 255 (FColor; A defaults to 255).
//   @param bottom_radius [number] - Optional planet bottom radius (km).
//   @param atmosphere_height [number] - Optional atmosphere height (km).
//   @param multi_scattering_factor [number] - Optional multi-scattering factor (0..2).
//   @param aerial_perspective_view_distance_scale [number] - Optional aerial perspective view distance scale.
//   @param height_fog_contribution [number] - Optional height fog contribution (0..1).
//   @returns { actor_name: string, properties: object, applied_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetSkyAtmosphereProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    USkyAtmosphereComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("a"), TEXT("SkyAtmosphere"), &PropertyHandlers_GetSkyAtmosphereComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    // Every supplied param is read and checked before the first write, so a malformed one changes nothing.
    TOptional<FLinearColor> RayleighScattering;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("rayleigh_scattering"), RayleighScattering)) { return Malformed; }
    TOptional<float> RayleighScatteringScale;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("rayleigh_scattering_scale"), RayleighScatteringScale)) { return Malformed; }
    TOptional<FLinearColor> MieScattering;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("mie_scattering"), MieScattering)) { return Malformed; }
    TOptional<float> MieScatteringScale;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("mie_scattering_scale"), MieScatteringScale)) { return Malformed; }
    TOptional<float> MieAnisotropy;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("mie_anisotropy"), MieAnisotropy)) { return Malformed; }
    TOptional<FColor> GroundAlbedo;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadColorBytes(Params, TEXT("ground_albedo"), GroundAlbedo)) { return Malformed; }
    TOptional<float> BottomRadius;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("bottom_radius"), BottomRadius)) { return Malformed; }
    TOptional<float> AtmosphereHeight;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("atmosphere_height"), AtmosphereHeight)) { return Malformed; }
    TOptional<float> MultiScatteringFactor;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("multi_scattering_factor"), MultiScatteringFactor)) { return Malformed; }
    TOptional<float> AerialPerspectiveViewDistanceScale;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("aerial_perspective_view_distance_scale"), AerialPerspectiveViewDistanceScale)) { return Malformed; }
    TOptional<float> HeightFogContribution;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("height_fog_contribution"), HeightFogContribution)) { return Malformed; }

    const int32 AppliedCount = PropertyHandlers_CountSupplied({ RayleighScattering.IsSet(),
        RayleighScatteringScale.IsSet(), MieScattering.IsSet(), MieScatteringScale.IsSet(), MieAnisotropy.IsSet(),
        GroundAlbedo.IsSet(), BottomRadius.IsSet(), AtmosphereHeight.IsSet(), MultiScatteringFactor.IsSet(),
        AerialPerspectiveViewDistanceScale.IsSet(), HeightFogContribution.IsSet() });
    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: rayleigh_scattering, rayleigh_scattering_scale, mie_scattering, mie_scattering_scale, mie_anisotropy, ground_albedo, bottom_radius, atmosphere_height, multi_scattering_factor, aerial_perspective_view_distance_scale, height_fog_contribution"));
    }
    if (TSharedPtr<FJsonObject> Refused = PropertyHandlers_RefuseStaticComponent(*Component, TEXT("SkyAtmosphere"), ActorName))
    {
        return Refused;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSkyAtmosphereProperties", "MCP: Set Sky Atmosphere Properties"));
    Component->Modify();
    if (RayleighScattering)
    {
        Component->SetRayleighScattering(*RayleighScattering);
    }
    if (RayleighScatteringScale)
    {
        Component->SetRayleighScatteringScale(*RayleighScatteringScale);
    }
    if (MieScattering)
    {
        Component->SetMieScattering(*MieScattering);
    }
    if (MieScatteringScale)
    {
        Component->SetMieScatteringScale(*MieScatteringScale);
    }
    if (MieAnisotropy)
    {
        Component->SetMieAnisotropy(*MieAnisotropy);
    }
    if (GroundAlbedo)
    {
        Component->SetGroundAlbedo(*GroundAlbedo);
    }
    if (BottomRadius)
    {
        Component->SetBottomRadius(*BottomRadius);
    }
    if (AtmosphereHeight)
    {
        Component->SetAtmosphereHeight(*AtmosphereHeight);
    }
    if (MultiScatteringFactor)
    {
        Component->SetMultiScatteringFactor(*MultiScatteringFactor);
    }
    if (AerialPerspectiveViewDistanceScale)
    {
        Component->SetAerialPespectiveViewDistanceScale(*AerialPerspectiveViewDistanceScale);
    }
    if (HeightFogContribution)
    {
        Component->SetHeightFogContribution(*HeightFogContribution);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_SkyAtmosphereJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_sky_atmosphere_properties - Read the atmosphere settings of a SkyAtmosphere actor's component, found by label or name in the editor world, under the snake_case names set_sky_atmosphere_properties takes (Rayleigh/Mie scattering, ground albedo, planet radius, height). A curated view; get_properties (internal names only) reads any other UPROPERTY on the component.
//   @param actor_name - Label/name of the target SkyAtmosphere actor.
//   @returns { actor_name: string, properties: { rayleigh_scattering:[R,G,B,A], rayleigh_scattering_scale, mie_scattering:[R,G,B,A], mie_scattering_scale, mie_anisotropy, ground_albedo:[R,G,B,A], bottom_radius, atmosphere_height, multi_scattering_factor, aerial_perspective_view_distance_scale, height_fog_contribution } }
TSharedPtr<FJsonObject> HandleGetSkyAtmosphereProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    const USkyAtmosphereComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("a"), TEXT("SkyAtmosphere"), &PropertyHandlers_GetSkyAtmosphereComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_SkyAtmosphereJson(Component));
    return mcp_common::MakeOkReply(Result);
}

namespace
{
    UExponentialHeightFogComponent* PropertyHandlers_GetExponentialHeightFogComponent(AExponentialHeightFog& Actor)
    {
        return Actor.GetComponent();
    }

    TSharedPtr<FJsonObject> PropertyHandlers_ExponentialHeightFogJson(const UExponentialHeightFogComponent* Component)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetNumberField(TEXT("fog_density"), Component->FogDensity);
        Properties->SetNumberField(TEXT("fog_height_falloff"), Component->FogHeightFalloff);
        // SetFogInscatteringColor writes FogInscatteringLuminance.
        Properties->SetArrayField(TEXT("fog_inscattering_color"), mcp_common::ColorToJsonArray(Component->FogInscatteringLuminance));
        Properties->SetNumberField(TEXT("fog_max_opacity"), Component->FogMaxOpacity);
        Properties->SetNumberField(TEXT("start_distance"), Component->StartDistance);
        Properties->SetNumberField(TEXT("end_distance"), Component->EndDistance);
        Properties->SetNumberField(TEXT("fog_cutoff_distance"), Component->FogCutoffDistance);
        Properties->SetBoolField(TEXT("volumetric_fog"), Component->bEnableVolumetricFog);
        Properties->SetNumberField(TEXT("volumetric_fog_scattering_distribution"), Component->VolumetricFogScatteringDistribution);
        Properties->SetArrayField(TEXT("volumetric_fog_albedo"), mcp_common::ColorToJsonArray(Component->VolumetricFogAlbedo));
        Properties->SetArrayField(TEXT("volumetric_fog_emissive"), mcp_common::ColorToJsonArray(Component->VolumetricFogEmissive));
        Properties->SetNumberField(TEXT("volumetric_fog_extinction_scale"), Component->VolumetricFogExtinctionScale);
        Properties->SetNumberField(TEXT("second_fog_density"), Component->SecondFogData.FogDensity);
        Properties->SetNumberField(TEXT("second_fog_height_falloff"), Component->SecondFogData.FogHeightFalloff);
        Properties->SetNumberField(TEXT("second_fog_height_offset"), Component->SecondFogData.FogHeightOffset);
        Properties->SetNumberField(TEXT("directional_inscattering_exponent"), Component->DirectionalInscatteringExponent);
        Properties->SetNumberField(TEXT("directional_inscattering_start_distance"), Component->DirectionalInscatteringStartDistance);
        // SetDirectionalInscatteringColor writes DirectionalInscatteringLuminance; DirectionalInscatteringColor_DEPRECATED
        // keeps its default, so it does not round-trip.
        Properties->SetArrayField(TEXT("directional_inscattering_color"), mcp_common::ColorToJsonArray(Component->DirectionalInscatteringLuminance));
        return Properties;
    }
}

// MCP:set_exponential_height_fog_properties - Set an ExponentialHeightFog actor's fog component, actor found by label or name in the editor world: density, falloff, inscattering, distances, max opacity, volumetric fog, second fog layer, directional inscattering. Over set_properties it needs no component path and goes through the component's own setters; other fog properties: set_properties. Supplied fields only, one undo step, not saved (save_level); applied_count counts them and every property is read back. Colors are [R,G,B(,A)] floats, volumetric_fog_albedo [R,G,B(,A)] whole 0-255 bytes. Every field is checked before any write: a color that is not 3 or 4 numbers, a byte color with a fraction or a value past 255, a number given as a string or a non-boolean volumetric_fog is an error and nothing changes.
//   @param actor_name - Label/name of the target ExponentialHeightFog actor.
//   @param fog_density [number] - Optional global fog density.
//   @param fog_height_falloff [number] - Optional height falloff.
//   @param fog_inscattering_color [array] - Optional [R,G,B(,A)] inscattering color (luminance).
//   @param fog_max_opacity [number] - Optional max opacity (0..1).
//   @param start_distance [number] - Optional fog start distance.
//   @param end_distance [number] - Optional fog end distance.
//   @param fog_cutoff_distance [number] - Optional fog cutoff distance.
//   @param volumetric_fog [bool] - Optional enable volumetric fog.
//   @param volumetric_fog_scattering_distribution [number] - Optional volumetric scattering distribution (-1..1).
//   @param volumetric_fog_albedo [array] - Optional [R,G,B] or [R,G,B,A] volumetric albedo, each a whole number from 0 to 255 (FColor; A defaults to 255).
//   @param volumetric_fog_emissive [array] - Optional [R,G,B(,A)] volumetric emissive color.
//   @param volumetric_fog_extinction_scale [number] - Optional volumetric extinction scale.
//   @param second_fog_density [number] - Optional second fog layer density.
//   @param second_fog_height_falloff [number] - Optional second fog layer height falloff.
//   @param second_fog_height_offset [number] - Optional second fog layer height offset.
//   @param directional_inscattering_exponent [number] - Optional directional inscattering exponent (sun glow tightness).
//   @param directional_inscattering_start_distance [number] - Optional directional inscattering start distance.
//   @param directional_inscattering_color [array] - Optional [R,G,B(,A)] directional inscattering color (luminance).
//   @returns { actor_name: string, properties: object, applied_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetExponentialHeightFogProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    UExponentialHeightFogComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("an"), TEXT("ExponentialHeightFog"), &PropertyHandlers_GetExponentialHeightFogComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    // Every supplied param is read and checked before the first write, so a malformed one changes nothing.
    TOptional<float> FogDensity;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("fog_density"), FogDensity)) { return Malformed; }
    TOptional<float> FogHeightFalloff;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("fog_height_falloff"), FogHeightFalloff)) { return Malformed; }
    TOptional<FLinearColor> FogInscatteringColor;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("fog_inscattering_color"), FogInscatteringColor)) { return Malformed; }
    TOptional<float> FogMaxOpacity;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("fog_max_opacity"), FogMaxOpacity)) { return Malformed; }
    TOptional<float> StartDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("start_distance"), StartDistance)) { return Malformed; }
    TOptional<float> EndDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("end_distance"), EndDistance)) { return Malformed; }
    TOptional<float> FogCutoffDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("fog_cutoff_distance"), FogCutoffDistance)) { return Malformed; }
    TOptional<bool> VolumetricFog;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadBool(Params, TEXT("volumetric_fog"), VolumetricFog)) { return Malformed; }
    TOptional<float> VolumetricFogScatteringDistribution;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("volumetric_fog_scattering_distribution"), VolumetricFogScatteringDistribution)) { return Malformed; }
    TOptional<FColor> VolumetricFogAlbedo;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadColorBytes(Params, TEXT("volumetric_fog_albedo"), VolumetricFogAlbedo)) { return Malformed; }
    TOptional<FLinearColor> VolumetricFogEmissive;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("volumetric_fog_emissive"), VolumetricFogEmissive)) { return Malformed; }
    TOptional<float> VolumetricFogExtinctionScale;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("volumetric_fog_extinction_scale"), VolumetricFogExtinctionScale)) { return Malformed; }
    TOptional<float> SecondFogDensity;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("second_fog_density"), SecondFogDensity)) { return Malformed; }
    TOptional<float> SecondFogHeightFalloff;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("second_fog_height_falloff"), SecondFogHeightFalloff)) { return Malformed; }
    TOptional<float> SecondFogHeightOffset;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("second_fog_height_offset"), SecondFogHeightOffset)) { return Malformed; }
    TOptional<float> DirectionalInscatteringExponent;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("directional_inscattering_exponent"), DirectionalInscatteringExponent)) { return Malformed; }
    TOptional<float> DirectionalInscatteringStartDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("directional_inscattering_start_distance"), DirectionalInscatteringStartDistance)) { return Malformed; }
    TOptional<FLinearColor> DirectionalInscatteringColor;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("directional_inscattering_color"), DirectionalInscatteringColor)) { return Malformed; }

    const int32 AppliedCount = PropertyHandlers_CountSupplied({ FogDensity.IsSet(), FogHeightFalloff.IsSet(),
        FogInscatteringColor.IsSet(), FogMaxOpacity.IsSet(), StartDistance.IsSet(), EndDistance.IsSet(),
        FogCutoffDistance.IsSet(), VolumetricFog.IsSet(), VolumetricFogScatteringDistribution.IsSet(),
        VolumetricFogAlbedo.IsSet(), VolumetricFogEmissive.IsSet(), VolumetricFogExtinctionScale.IsSet(),
        SecondFogDensity.IsSet(), SecondFogHeightFalloff.IsSet(), SecondFogHeightOffset.IsSet(),
        DirectionalInscatteringExponent.IsSet(), DirectionalInscatteringStartDistance.IsSet(),
        DirectionalInscatteringColor.IsSet() });
    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: fog_density, fog_height_falloff, fog_inscattering_color, fog_max_opacity, start_distance, end_distance, fog_cutoff_distance, volumetric_fog, volumetric_fog_scattering_distribution, volumetric_fog_albedo, volumetric_fog_emissive, volumetric_fog_extinction_scale, second_fog_density, second_fog_height_falloff, second_fog_height_offset, directional_inscattering_exponent, directional_inscattering_start_distance, directional_inscattering_color"));
    }

    const FScopedTransaction Transaction(LOCTEXT("SetExpHeightFogProperties", "MCP: Set Exponential Height Fog Properties"));
    Component->Modify();
    if (FogDensity)
    {
        Component->SetFogDensity(*FogDensity);
    }
    if (FogHeightFalloff)
    {
        Component->SetFogHeightFalloff(*FogHeightFalloff);
    }
    if (FogInscatteringColor)
    {
        Component->SetFogInscatteringColor(*FogInscatteringColor);
    }
    if (FogMaxOpacity)
    {
        Component->SetFogMaxOpacity(*FogMaxOpacity);
    }
    if (StartDistance)
    {
        Component->SetStartDistance(*StartDistance);
    }
    if (EndDistance)
    {
        Component->SetEndDistance(*EndDistance);
    }
    if (FogCutoffDistance)
    {
        Component->SetFogCutoffDistance(*FogCutoffDistance);
    }
    if (VolumetricFog)
    {
        Component->SetVolumetricFog(*VolumetricFog);
    }
    if (VolumetricFogScatteringDistribution)
    {
        Component->SetVolumetricFogScatteringDistribution(*VolumetricFogScatteringDistribution);
    }
    if (VolumetricFogAlbedo)
    {
        Component->SetVolumetricFogAlbedo(*VolumetricFogAlbedo);
    }
    if (VolumetricFogEmissive)
    {
        Component->SetVolumetricFogEmissive(*VolumetricFogEmissive);
    }
    if (VolumetricFogExtinctionScale)
    {
        Component->SetVolumetricFogExtinctionScale(*VolumetricFogExtinctionScale);
    }
    if (SecondFogDensity)
    {
        Component->SetSecondFogDensity(*SecondFogDensity);
    }
    if (SecondFogHeightFalloff)
    {
        Component->SetSecondFogHeightFalloff(*SecondFogHeightFalloff);
    }
    if (SecondFogHeightOffset)
    {
        Component->SetSecondFogHeightOffset(*SecondFogHeightOffset);
    }
    if (DirectionalInscatteringExponent)
    {
        Component->SetDirectionalInscatteringExponent(*DirectionalInscatteringExponent);
    }
    if (DirectionalInscatteringStartDistance)
    {
        Component->SetDirectionalInscatteringStartDistance(*DirectionalInscatteringStartDistance);
    }
    if (DirectionalInscatteringColor)
    {
        Component->SetDirectionalInscatteringColor(*DirectionalInscatteringColor);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_ExponentialHeightFogJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_exponential_height_fog_properties - Read the fog of an ExponentialHeightFog actor's component, found by label or name in the editor world, under the snake_case names set_exponential_height_fog_properties takes (density, falloff, inscattering, distances, volumetric fog, second layer). A curated view; get_properties (internal names only) reads any other UPROPERTY on the component.
//   @param actor_name - Label/name of the target ExponentialHeightFog actor.
//   @returns { actor_name: string, properties: { fog_density, fog_height_falloff, fog_inscattering_color:[R,G,B,A], fog_max_opacity, start_distance, end_distance, fog_cutoff_distance, volumetric_fog, volumetric_fog_scattering_distribution, volumetric_fog_albedo:[R,G,B,A], volumetric_fog_emissive:[R,G,B,A], volumetric_fog_extinction_scale, second_fog_density, second_fog_height_falloff, second_fog_height_offset, directional_inscattering_exponent, directional_inscattering_start_distance, directional_inscattering_color:[R,G,B,A] } }
TSharedPtr<FJsonObject> HandleGetExponentialHeightFogProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    const UExponentialHeightFogComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("an"), TEXT("ExponentialHeightFog"), &PropertyHandlers_GetExponentialHeightFogComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_ExponentialHeightFogJson(Component));
    return mcp_common::MakeOkReply(Result);
}

// SetReflectionViewSampleCountScale and SetShadowReflectionViewSampleCountScale are left out: the fields
// they write are Value-suffixed, next to deprecated fields of the plain name.
namespace
{
    UVolumetricCloudComponent* PropertyHandlers_GetVolumetricCloudComponent(AVolumetricCloud& Actor)
    {
        // AVolumetricCloud has no public component getter, so the component is found by class.
        return Actor.FindComponentByClass<UVolumetricCloudComponent>();
    }

    TSharedPtr<FJsonObject> PropertyHandlers_VolumetricCloudJson(const UVolumetricCloudComponent* Component)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetNumberField(TEXT("layer_bottom_altitude"), Component->LayerBottomAltitude);
        Properties->SetNumberField(TEXT("layer_height"), Component->LayerHeight);
        Properties->SetNumberField(TEXT("planet_radius"), Component->PlanetRadius);
        Properties->SetArrayField(TEXT("ground_albedo"), mcp_common::ColorToJsonArray(Component->GroundAlbedo));
        Properties->SetNumberField(TEXT("tracing_start_max_distance"), Component->TracingStartMaxDistance);
        Properties->SetNumberField(TEXT("tracing_max_distance"), Component->TracingMaxDistance);
        Properties->SetNumberField(TEXT("sky_light_cloud_bottom_occlusion"), Component->SkyLightCloudBottomOcclusion);
        Properties->SetNumberField(TEXT("view_sample_count_scale"), Component->ViewSampleCountScale);
        Properties->SetNumberField(TEXT("shadow_view_sample_count_scale"), Component->ShadowViewSampleCountScale);
        Properties->SetNumberField(TEXT("shadow_tracing_distance"), Component->ShadowTracingDistance);
        Properties->SetNumberField(TEXT("stop_tracing_transmittance_threshold"), Component->StopTracingTransmittanceThreshold);
        Properties->SetBoolField(TEXT("use_per_sample_atmospheric_light_transmittance"), Component->bUsePerSampleAtmosphericLightTransmittance != 0);
        return Properties;
    }
}

// MCP:set_volumetric_cloud_properties - Set layer and planet geometry, ground albedo, tracing distances and sample counts of a VolumetricCloud actor found by label or name, through the component's setters, which refresh its render state; set_properties writes any other field. Only supplied fields change, in one undo step (applied_count counts them), and the reply reads every property back. ground_albedo is [R,G,B(,A)] in whole 0-255 bytes. A malformed field, or a component of Static mobility (its setters change nothing; make it Movable first), is an error and nothing changes. Not saved.
//   @param actor_name - Label/name of the target VolumetricCloud actor.
//   @param layer_bottom_altitude [number] - Optional layer bottom altitude (km).
//   @param layer_height [number] - Optional layer height (km).
//   @param planet_radius [number] - Optional planet radius (km).
//   @param ground_albedo [array] - Optional [R,G,B] or [R,G,B,A] ground albedo, each a whole number from 0 to 255 (FColor; A defaults to 255).
//   @param tracing_start_max_distance [number] - Optional tracing start max distance.
//   @param tracing_max_distance [number] - Optional tracing max distance.
//   @param sky_light_cloud_bottom_occlusion [number] - Optional sky light cloud bottom occlusion (0..1).
//   @param view_sample_count_scale [number] - Optional view sample count scale.
//   @param shadow_view_sample_count_scale [number] - Optional shadow view sample count scale.
//   @param shadow_tracing_distance [number] - Optional shadow tracing distance (km).
//   @param stop_tracing_transmittance_threshold [number] - Optional stop-tracing transmittance threshold.
//   @param use_per_sample_atmospheric_light_transmittance [bool] - Optional per-sample atmospheric light transmittance.
//   @returns { actor_name: string, properties: object, applied_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetVolumetricCloudProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    UVolumetricCloudComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("a"), TEXT("VolumetricCloud"), &PropertyHandlers_GetVolumetricCloudComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    // Every supplied param is read and checked before the first write, so a malformed one changes nothing.
    TOptional<float> LayerBottomAltitude;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("layer_bottom_altitude"), LayerBottomAltitude)) { return Malformed; }
    TOptional<float> LayerHeight;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("layer_height"), LayerHeight)) { return Malformed; }
    TOptional<float> PlanetRadius;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("planet_radius"), PlanetRadius)) { return Malformed; }
    TOptional<FColor> GroundAlbedo;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadColorBytes(Params, TEXT("ground_albedo"), GroundAlbedo)) { return Malformed; }
    TOptional<float> TracingStartMaxDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("tracing_start_max_distance"), TracingStartMaxDistance)) { return Malformed; }
    TOptional<float> TracingMaxDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("tracing_max_distance"), TracingMaxDistance)) { return Malformed; }
    TOptional<float> SkyLightCloudBottomOcclusion;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("sky_light_cloud_bottom_occlusion"), SkyLightCloudBottomOcclusion)) { return Malformed; }
    TOptional<float> ViewSampleCountScale;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("view_sample_count_scale"), ViewSampleCountScale)) { return Malformed; }
    TOptional<float> ShadowViewSampleCountScale;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("shadow_view_sample_count_scale"), ShadowViewSampleCountScale)) { return Malformed; }
    TOptional<float> ShadowTracingDistance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("shadow_tracing_distance"), ShadowTracingDistance)) { return Malformed; }
    TOptional<float> StopTracingTransmittanceThreshold;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("stop_tracing_transmittance_threshold"), StopTracingTransmittanceThreshold)) { return Malformed; }
    TOptional<bool> UsePerSampleAtmosphericLightTransmittance;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadBool(Params, TEXT("use_per_sample_atmospheric_light_transmittance"), UsePerSampleAtmosphericLightTransmittance)) { return Malformed; }

    const int32 AppliedCount = PropertyHandlers_CountSupplied({ LayerBottomAltitude.IsSet(), LayerHeight.IsSet(),
        PlanetRadius.IsSet(), GroundAlbedo.IsSet(), TracingStartMaxDistance.IsSet(), TracingMaxDistance.IsSet(),
        SkyLightCloudBottomOcclusion.IsSet(), ViewSampleCountScale.IsSet(), ShadowViewSampleCountScale.IsSet(),
        ShadowTracingDistance.IsSet(), StopTracingTransmittanceThreshold.IsSet(),
        UsePerSampleAtmosphericLightTransmittance.IsSet() });
    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: layer_bottom_altitude, layer_height, planet_radius, ground_albedo, tracing_start_max_distance, tracing_max_distance, sky_light_cloud_bottom_occlusion, view_sample_count_scale, shadow_view_sample_count_scale, shadow_tracing_distance, stop_tracing_transmittance_threshold, use_per_sample_atmospheric_light_transmittance"));
    }
    if (TSharedPtr<FJsonObject> Refused = PropertyHandlers_RefuseStaticComponent(*Component, TEXT("VolumetricCloud"), ActorName))
    {
        return Refused;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetVolumetricCloudProperties", "MCP: Set Volumetric Cloud Properties"));
    Component->Modify();
    if (LayerBottomAltitude)
    {
        Component->SetLayerBottomAltitude(*LayerBottomAltitude);
    }
    if (LayerHeight)
    {
        Component->SetLayerHeight(*LayerHeight);
    }
    if (PlanetRadius)
    {
        Component->SetPlanetRadius(*PlanetRadius);
    }
    if (GroundAlbedo)
    {
        Component->SetGroundAlbedo(*GroundAlbedo);
    }
    if (TracingStartMaxDistance)
    {
        Component->SetTracingStartMaxDistance(*TracingStartMaxDistance);
    }
    if (TracingMaxDistance)
    {
        Component->SetTracingMaxDistance(*TracingMaxDistance);
    }
    if (SkyLightCloudBottomOcclusion)
    {
        Component->SetSkyLightCloudBottomOcclusion(*SkyLightCloudBottomOcclusion);
    }
    if (ViewSampleCountScale)
    {
        Component->SetViewSampleCountScale(*ViewSampleCountScale);
    }
    if (ShadowViewSampleCountScale)
    {
        Component->SetShadowViewSampleCountScale(*ShadowViewSampleCountScale);
    }
    if (ShadowTracingDistance)
    {
        Component->SetShadowTracingDistance(*ShadowTracingDistance);
    }
    if (StopTracingTransmittanceThreshold)
    {
        Component->SetStopTracingTransmittanceThreshold(*StopTracingTransmittanceThreshold);
    }
    if (UsePerSampleAtmosphericLightTransmittance)
    {
        Component->SetbUsePerSampleAtmosphericLightTransmittance(*UsePerSampleAtmosphericLightTransmittance);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_VolumetricCloudJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_volumetric_cloud_properties - Read the layer and tracing settings of a VolumetricCloud actor's component, found by label or name in the editor world, under the snake_case names set_volumetric_cloud_properties takes. A curated view; get_properties (internal names only) reads any other UPROPERTY on the component.
//   @param actor_name - Label/name of the target VolumetricCloud actor.
//   @returns { actor_name: string, properties: { layer_bottom_altitude, layer_height, planet_radius, ground_albedo:[R,G,B,A], tracing_start_max_distance, tracing_max_distance, sky_light_cloud_bottom_occlusion, view_sample_count_scale, shadow_view_sample_count_scale, shadow_tracing_distance, stop_tracing_transmittance_threshold, use_per_sample_atmospheric_light_transmittance } }
TSharedPtr<FJsonObject> HandleGetVolumetricCloudProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    const UVolumetricCloudComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("a"), TEXT("VolumetricCloud"), &PropertyHandlers_GetVolumetricCloudComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_VolumetricCloudJson(Component));
    return mcp_common::MakeOkReply(Result);
}

namespace
{
    USkyLightComponent* PropertyHandlers_GetSkyLightComponent(ASkyLight& Actor)
    {
        return Actor.GetLightComponent();
    }

    TSharedPtr<FJsonObject> PropertyHandlers_SkyLightJson(const USkyLightComponent* Component)
    {
        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetNumberField(TEXT("intensity"), Component->Intensity);
        // SetLightColor stores LightColor as an sRGB FColor, so it reads back as 0-255 bytes.
        Properties->SetArrayField(TEXT("light_color"), mcp_common::ColorToJsonArray(Component->LightColor));
        Properties->SetNumberField(TEXT("indirect_lighting_intensity"), Component->IndirectLightingIntensity);
        Properties->SetNumberField(TEXT("volumetric_scattering_intensity"), Component->VolumetricScatteringIntensity);
        Properties->SetArrayField(TEXT("lower_hemisphere_color"), mcp_common::ColorToJsonArray(Component->LowerHemisphereColor));
        Properties->SetArrayField(TEXT("occlusion_tint"), mcp_common::ColorToJsonArray(Component->OcclusionTint));
        // SetOcclusionContrast writes Contrast.
        Properties->SetNumberField(TEXT("occlusion_contrast"), Component->Contrast);
        Properties->SetNumberField(TEXT("occlusion_exponent"), Component->OcclusionExponent);
        Properties->SetNumberField(TEXT("min_occlusion"), Component->MinOcclusion);
        return Properties;
    }
}

// MCP:set_sky_light_properties - Set intensity, light color, indirect and volumetric intensity, lower-hemisphere color and occlusion of a SkyLight actor found by label or name, through the component's setters, which update its rendering state; set_properties writes any other field. Only supplied fields change, in one undo step, and all are read back. light_color and lower_hemisphere_color are linear [R,G,B(,A)]; occlusion_tint is [R,G,B(,A)] in whole 0-255 bytes. A Static SkyLight is refused, since its setters change nothing (make it Stationary or Movable first), and so is a malformed field; nothing changes then. Not saved.
//   @param actor_name - Label/name of the target SkyLight actor.
//   @param intensity [number] - Optional sky light intensity.
//   @param light_color [array] - Optional [R,G,B(,A)] light color (stored as FColor).
//   @param indirect_lighting_intensity [number] - Optional indirect lighting intensity scale.
//   @param volumetric_scattering_intensity [number] - Optional volumetric scattering intensity.
//   @param lower_hemisphere_color [array] - Optional [R,G,B(,A)] lower-hemisphere color (FLinearColor).
//   @param occlusion_tint [array] - Optional [R,G,B] or [R,G,B,A] occlusion tint, each a whole number from 0 to 255 (FColor; A defaults to 255).
//   @param occlusion_contrast [number] - Optional AO contrast (0..1).
//   @param occlusion_exponent [number] - Optional AO exponent.
//   @param min_occlusion [number] - Optional minimum occlusion (0..1).
//   @returns { actor_name: string, properties: object, applied_count: int, applied: bool }
TSharedPtr<FJsonObject> HandleSetSkyLightProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    USkyLightComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("a"), TEXT("SkyLight"), &PropertyHandlers_GetSkyLightComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    // Every supplied param is read and checked before the first write, so a malformed one changes nothing.
    TOptional<float> Intensity;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("intensity"), Intensity)) { return Malformed; }
    TOptional<FLinearColor> LightColor;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("light_color"), LightColor)) { return Malformed; }
    TOptional<float> IndirectLightingIntensity;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("indirect_lighting_intensity"), IndirectLightingIntensity)) { return Malformed; }
    TOptional<float> VolumetricScatteringIntensity;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("volumetric_scattering_intensity"), VolumetricScatteringIntensity)) { return Malformed; }
    TOptional<FLinearColor> LowerHemisphereColor;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadLinearColor(Params, TEXT("lower_hemisphere_color"), LowerHemisphereColor)) { return Malformed; }
    TOptional<FColor> OcclusionTint;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadColorBytes(Params, TEXT("occlusion_tint"), OcclusionTint)) { return Malformed; }
    TOptional<float> OcclusionContrast;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("occlusion_contrast"), OcclusionContrast)) { return Malformed; }
    TOptional<float> OcclusionExponent;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("occlusion_exponent"), OcclusionExponent)) { return Malformed; }
    TOptional<float> MinOcclusion;
    if (TSharedPtr<FJsonObject> Malformed = PropertyHandlers_ReadFloat(Params, TEXT("min_occlusion"), MinOcclusion)) { return Malformed; }

    const int32 AppliedCount = PropertyHandlers_CountSupplied({ Intensity.IsSet(), LightColor.IsSet(),
        IndirectLightingIntensity.IsSet(), VolumetricScatteringIntensity.IsSet(), LowerHemisphereColor.IsSet(),
        OcclusionTint.IsSet(), OcclusionContrast.IsSet(), OcclusionExponent.IsSet(), MinOcclusion.IsSet() });
    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: intensity, light_color, indirect_lighting_intensity, volumetric_scattering_intensity, lower_hemisphere_color, occlusion_tint, occlusion_contrast, occlusion_exponent, min_occlusion"));
    }
    if (TSharedPtr<FJsonObject> Refused = PropertyHandlers_RefuseStaticComponent(*Component, TEXT("SkyLight"), ActorName))
    {
        return Refused;
    }

    const FScopedTransaction Transaction(LOCTEXT("SetSkyLightProperties", "MCP: Set Sky Light Properties"));
    Component->Modify();
    if (Intensity)
    {
        Component->SetIntensity(*Intensity);
    }
    if (LightColor)
    {
        Component->SetLightColor(*LightColor);
    }
    if (IndirectLightingIntensity)
    {
        Component->SetIndirectLightingIntensity(*IndirectLightingIntensity);
    }
    if (VolumetricScatteringIntensity)
    {
        Component->SetVolumetricScatteringIntensity(*VolumetricScatteringIntensity);
    }
    if (LowerHemisphereColor)
    {
        Component->SetLowerHemisphereColor(*LowerHemisphereColor);
    }
    if (OcclusionTint)
    {
        Component->SetOcclusionTint(*OcclusionTint);
    }
    if (OcclusionContrast)
    {
        Component->SetOcclusionContrast(*OcclusionContrast);
    }
    if (OcclusionExponent)
    {
        Component->SetOcclusionExponent(*OcclusionExponent);
    }
    if (MinOcclusion)
    {
        Component->SetMinOcclusion(*MinOcclusion);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_SkyLightJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_sky_light_properties - Read the intensity, color and occlusion settings of a SkyLight actor's component, found by label or name in the editor world, under the snake_case names set_sky_light_properties takes. A curated view; get_properties (internal names only) reads any other UPROPERTY on it.
//   @param actor_name - Label/name of the target SkyLight actor.
//   @returns { actor_name: string, properties: { intensity, light_color:[R,G,B,A], indirect_lighting_intensity, volumetric_scattering_intensity, lower_hemisphere_color:[R,G,B,A], occlusion_tint:[R,G,B,A], occlusion_contrast, occlusion_exponent, min_occlusion } }
TSharedPtr<FJsonObject> HandleGetSkyLightProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    TSharedPtr<FJsonObject> Error;
    const USkyLightComponent* Component = PropertyHandlers_ResolveComponent(
        Params, TEXT("a"), TEXT("SkyLight"), &PropertyHandlers_GetSkyLightComponent, ActorName, Error);
    if (!Component)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_SkyLightJson(Component));
    return mcp_common::MakeOkReply(Result);
}

// </generated:property-handlers>

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_exponential_height_fog_properties"), &HandleGetExponentialHeightFogProperties },
    { TEXT("get_post_process_settings"), &HandleGetPostProcessSettings },
    { TEXT("get_sky_atmosphere_properties"), &HandleGetSkyAtmosphereProperties },
    { TEXT("get_sky_light_properties"), &HandleGetSkyLightProperties },
    { TEXT("get_volumetric_cloud_properties"), &HandleGetVolumetricCloudProperties },
    { TEXT("set_exponential_height_fog_properties"), &HandleSetExponentialHeightFogProperties },
    { TEXT("set_post_process_color_grading"), &HandleSetPostProcessColorGrading },
    { TEXT("set_post_process_setting"), &HandleSetPostProcessSetting },
    { TEXT("set_sky_atmosphere_properties"), &HandleSetSkyAtmosphereProperties },
    { TEXT("set_sky_light_properties"), &HandleSetSkyLightProperties },
    { TEXT("set_volumetric_cloud_properties"), &HandleSetVolumetricCloudProperties },
    { TEXT("spawn_exponential_height_fog"), &HandleSpawnExponentialHeightFog },
    { TEXT("spawn_post_process_volume"), &HandleSpawnPostProcessVolume },
    { TEXT("spawn_sky_atmosphere"), &HandleSpawnSkyAtmosphere },
    { TEXT("spawn_sky_light"), &HandleSpawnSkyLight },
    { TEXT("spawn_volumetric_cloud"), &HandleSpawnVolumetricCloud },
};
} // namespace UEAgentEnvironmentCommands

#undef LOCTEXT_NAMESPACE
