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

// MCP:spawn_sky_atmosphere - Spawn a new SkyAtmosphere actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one; nothing is saved. Returns actor_name (the label), actor_internal_name, actor_class and has_component. Tune it with set_sky_atmosphere_properties.
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

// MCP:spawn_volumetric_cloud - Spawn a new VolumetricCloud actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one; nothing is saved. Returns actor_name (the label), actor_internal_name, actor_class and has_component. Tune it with set_volumetric_cloud_properties.
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

// MCP:spawn_exponential_height_fog - Spawn a new ExponentialHeightFog actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one, so each call adds another fog actor (not idempotent); nothing is saved (save_level). fog_density, when given, is written to its UExponentialHeightFogComponent with SetFogDensity and density_set turns true; either way the reply reads fog_density back from the component, so without the param it shows the class default. No editor world, or a failed spawn, is an error. actor_name is the label and actor_internal_name the object name. Tune or inspect an existing fog with set_exponential_height_fog_properties and get_exponential_height_fog_properties; other atmosphere actors: spawn_sky_atmosphere, spawn_volumetric_cloud or spawn_sky_light.
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

// MCP:spawn_sky_light - Spawn a new SkyLight actor in the editor world at location (no rotation) as one undoable transaction, without checking for an existing one; nothing is saved. intensity, when given, is set on its USkyLightComponent, and recapture=true calls RecaptureSky so the light refreshes its captured sky lighting now. The reply reads intensity back from the component and names the actor (actor_name is the label). Tune an existing sky light with set_sky_light_properties; spawn_sky_atmosphere adds the atmosphere.
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

// MCP:spawn_post_process_volume - Spawn a new PostProcessVolume actor in the editor world at location as one undoable transaction, unbound (affecting the whole world) by default; nothing is saved. Returns actor_name (the label), actor_internal_name, unbound and enabled. Configure it with set_post_process_setting or set_post_process_color_grading.
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

// MCP:get_post_process_settings - Read a PostProcessVolume's color grading and the scalar settings set_post_process_setting manages, each with its override flag.  The reader that was missing: every value comes with `overridden` (its bOverride_* flag) so you can see what is actually active.
//   @param actor_name - Label/name of the target PostProcessVolume actor.
//   @returns { actor_name: string, color_grading: { saturation, contrast, gamma, gain, offset, saturation_shadows, saturation_midtones, saturation_highlights, contrast_shadows, contrast_midtones, contrast_highlights (each {value:[float], overridden}) }, white_balance: { white_temp, white_tint, shadows_max, highlights_min (each {value, overridden}) }, scalars: { bloom_intensity, vignette_intensity, ambient_occlusion_intensity, auto_exposure_bias, auto_exposure_min_brightness, auto_exposure_max_brightness (each {value, overridden}) }, exposure_method: int, dynamic_global_illumination_method: int }
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
    Result->SetObjectField(TEXT("color_grading"), CG);
    Result->SetObjectField(TEXT("white_balance"), WhiteBalance);
    Result->SetObjectField(TEXT("scalars"), Scalars);
    Result->SetNumberField(TEXT("exposure_method"), static_cast<int32>(S.AutoExposureMethod.GetValue()));
    Result->SetNumberField(TEXT("dynamic_global_illumination_method"), static_cast<int32>(S.DynamicGlobalIlluminationMethod.GetValue()));
    return mcp_common::MakeOkReply(Result);
}

// <generated:property-handlers> -- DO NOT EDIT BY HAND. Source of truth:
// Tools/property_descriptors/{sky_atmosphere,exponential_height_fog,volumetric_cloud,sky_light}.json
// Regenerate: python Tools/integrate_property_handlers.py

// Each set handler applies the supplied params through the component's ENGINE_API setters, which update
// its render state. All but the fog's change nothing on a Static component (AreDynamicDataChangesAllowed());
// the components default to Movable (SkyLight: Stationary), so the setters apply headless too. The readers
// read back the public fields the setters write.
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

    // A color param, [R, G, B] or [R, G, B, A] with A defaulting to 1; false when it has fewer than three entries.
    bool PropertyHandlers_ParseLinearColor(const TArray<TSharedPtr<FJsonValue>>* Values, FLinearColor& OutColor)
    {
        FVector4 Rgba;
        if (!ParseColorVec4(Values, 1.0, Rgba))
        {
            return false;
        }
        OutColor = FLinearColor(Rgba.X, Rgba.Y, Rgba.Z, Rgba.W);
        return true;
    }

    // An [R, G, B] param of 0-255 bytes as an opaque FColor, each channel clamped; false when it has fewer than
    // three entries.
    bool PropertyHandlers_ParseColorBytes(const TArray<TSharedPtr<FJsonValue>>* Values, FColor& OutColor)
    {
        if (!Values || Values->Num() < 3)
        {
            return false;
        }
        auto Channel = [Values](int32 Index)
        {
            return static_cast<uint8>(FMath::Clamp(static_cast<int32>((*Values)[Index]->AsNumber()), 0, 255));
        };
        OutColor = FColor(Channel(0), Channel(1), Channel(2), 255);
        return true;
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

// MCP:set_sky_atmosphere_properties - Set the atmosphere properties of a SkyAtmosphere actor's USkyAtmosphereComponent. Only the supplied fields change (applied_count counts them) and the reply reads every property back. Colors are [R,G,B] or [R,G,B,A].
//   @param actor_name - Label/name of the target SkyAtmosphere actor.
//   @param rayleigh_scattering [array] - Optional [R,G,B(,A)] Rayleigh scattering color.
//   @param rayleigh_scattering_scale [number] - Optional Rayleigh scattering scale.
//   @param mie_scattering [array] - Optional [R,G,B(,A)] Mie scattering color.
//   @param mie_scattering_scale [number] - Optional Mie scattering scale.
//   @param mie_anisotropy [number] - Optional Mie anisotropy (0..1).
//   @param ground_albedo [array] - Optional [R,G,B] ground albedo as 0-255 bytes (FColor).
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

    const FScopedTransaction Transaction(LOCTEXT("SetSkyAtmosphereProperties", "MCP: Set Sky Atmosphere Properties"));
    Component->Modify();

    int32 AppliedCount = 0;
    double Number = 0.0;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    FLinearColor LinearColor(ForceInit);
    FColor ByteColor(ForceInit);

    if (Params->TryGetArrayField(TEXT("rayleigh_scattering"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetRayleighScattering(LinearColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("rayleigh_scattering_scale"), Number))
    {
        Component->SetRayleighScatteringScale(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("mie_scattering"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetMieScattering(LinearColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("mie_scattering_scale"), Number))
    {
        Component->SetMieScatteringScale(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("mie_anisotropy"), Number))
    {
        Component->SetMieAnisotropy(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("ground_albedo"), Values) && PropertyHandlers_ParseColorBytes(Values, ByteColor))
    {
        Component->SetGroundAlbedo(ByteColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("bottom_radius"), Number))
    {
        Component->SetBottomRadius(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("atmosphere_height"), Number))
    {
        Component->SetAtmosphereHeight(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("multi_scattering_factor"), Number))
    {
        Component->SetMultiScatteringFactor(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("aerial_perspective_view_distance_scale"), Number))
    {
        Component->SetAerialPespectiveViewDistanceScale(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("height_fog_contribution"), Number))
    {
        Component->SetHeightFogContribution(static_cast<float>(Number));
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: rayleigh_scattering, rayleigh_scattering_scale, mie_scattering, mie_scattering_scale, mie_anisotropy, ground_albedo, bottom_radius, atmosphere_height, multi_scattering_factor, aerial_perspective_view_distance_scale, height_fog_contribution"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_SkyAtmosphereJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_sky_atmosphere_properties - Read the atmosphere properties of a SkyAtmosphere actor's USkyAtmosphereComponent, under the names set_sky_atmosphere_properties takes.
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

// MCP:set_exponential_height_fog_properties - Set the properties of an ExponentialHeightFog actor's UExponentialHeightFogComponent: density and falloff, inscattering, distances, max opacity, volumetric fog, the second fog layer and directional inscattering. Only the supplied fields change (applied_count counts them) and the reply reads every property back. Colors are [R,G,B] or [R,G,B,A].
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
//   @param volumetric_fog_albedo [array] - Optional [R,G,B] volumetric albedo as 0-255 bytes (FColor).
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

    const FScopedTransaction Transaction(LOCTEXT("SetExpHeightFogProperties", "MCP: Set Exponential Height Fog Properties"));
    Component->Modify();

    int32 AppliedCount = 0;
    double Number = 0.0;
    bool bValue = false;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    FLinearColor LinearColor(ForceInit);
    FColor ByteColor(ForceInit);

    if (Params->TryGetNumberField(TEXT("fog_density"), Number))
    {
        Component->SetFogDensity(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("fog_height_falloff"), Number))
    {
        Component->SetFogHeightFalloff(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("fog_inscattering_color"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetFogInscatteringColor(LinearColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("fog_max_opacity"), Number))
    {
        Component->SetFogMaxOpacity(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("start_distance"), Number))
    {
        Component->SetStartDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("end_distance"), Number))
    {
        Component->SetEndDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("fog_cutoff_distance"), Number))
    {
        Component->SetFogCutoffDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetBoolField(TEXT("volumetric_fog"), bValue))
    {
        Component->SetVolumetricFog(bValue);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("volumetric_fog_scattering_distribution"), Number))
    {
        Component->SetVolumetricFogScatteringDistribution(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("volumetric_fog_albedo"), Values) && PropertyHandlers_ParseColorBytes(Values, ByteColor))
    {
        Component->SetVolumetricFogAlbedo(ByteColor);
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("volumetric_fog_emissive"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetVolumetricFogEmissive(LinearColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("volumetric_fog_extinction_scale"), Number))
    {
        Component->SetVolumetricFogExtinctionScale(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("second_fog_density"), Number))
    {
        Component->SetSecondFogDensity(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("second_fog_height_falloff"), Number))
    {
        Component->SetSecondFogHeightFalloff(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("second_fog_height_offset"), Number))
    {
        Component->SetSecondFogHeightOffset(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("directional_inscattering_exponent"), Number))
    {
        Component->SetDirectionalInscatteringExponent(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("directional_inscattering_start_distance"), Number))
    {
        Component->SetDirectionalInscatteringStartDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("directional_inscattering_color"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetDirectionalInscatteringColor(LinearColor);
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: fog_density, fog_height_falloff, fog_inscattering_color, fog_max_opacity, start_distance, end_distance, fog_cutoff_distance, volumetric_fog, volumetric_fog_scattering_distribution, volumetric_fog_albedo, volumetric_fog_emissive, volumetric_fog_extinction_scale, second_fog_density, second_fog_height_falloff, second_fog_height_offset, directional_inscattering_exponent, directional_inscattering_start_distance, directional_inscattering_color"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_ExponentialHeightFogJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_exponential_height_fog_properties - Read the properties of an ExponentialHeightFog actor's UExponentialHeightFogComponent, under the names set_exponential_height_fog_properties takes.
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

// MCP:set_volumetric_cloud_properties - Set the properties of a VolumetricCloud actor's UVolumetricCloudComponent: layer and planet geometry, ground albedo, tracing distances and sample counts. Only the supplied fields change (applied_count counts them) and the reply reads every property back. ground_albedo is [R,G,B] in 0-255 bytes.
//   @param actor_name - Label/name of the target VolumetricCloud actor.
//   @param layer_bottom_altitude [number] - Optional layer bottom altitude (km).
//   @param layer_height [number] - Optional layer height (km).
//   @param planet_radius [number] - Optional planet radius (km).
//   @param ground_albedo [array] - Optional [R,G,B] ground albedo as 0-255 bytes (FColor).
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

    const FScopedTransaction Transaction(LOCTEXT("SetVolumetricCloudProperties", "MCP: Set Volumetric Cloud Properties"));
    Component->Modify();

    int32 AppliedCount = 0;
    double Number = 0.0;
    bool bValue = false;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    FColor ByteColor(ForceInit);

    if (Params->TryGetNumberField(TEXT("layer_bottom_altitude"), Number))
    {
        Component->SetLayerBottomAltitude(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("layer_height"), Number))
    {
        Component->SetLayerHeight(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("planet_radius"), Number))
    {
        Component->SetPlanetRadius(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("ground_albedo"), Values) && PropertyHandlers_ParseColorBytes(Values, ByteColor))
    {
        Component->SetGroundAlbedo(ByteColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("tracing_start_max_distance"), Number))
    {
        Component->SetTracingStartMaxDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("tracing_max_distance"), Number))
    {
        Component->SetTracingMaxDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("sky_light_cloud_bottom_occlusion"), Number))
    {
        Component->SetSkyLightCloudBottomOcclusion(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("view_sample_count_scale"), Number))
    {
        Component->SetViewSampleCountScale(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("shadow_view_sample_count_scale"), Number))
    {
        Component->SetShadowViewSampleCountScale(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("shadow_tracing_distance"), Number))
    {
        Component->SetShadowTracingDistance(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("stop_tracing_transmittance_threshold"), Number))
    {
        Component->SetStopTracingTransmittanceThreshold(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetBoolField(TEXT("use_per_sample_atmospheric_light_transmittance"), bValue))
    {
        Component->SetbUsePerSampleAtmosphericLightTransmittance(bValue);
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: layer_bottom_altitude, layer_height, planet_radius, ground_albedo, tracing_start_max_distance, tracing_max_distance, sky_light_cloud_bottom_occlusion, view_sample_count_scale, shadow_view_sample_count_scale, shadow_tracing_distance, stop_tracing_transmittance_threshold, use_per_sample_atmospheric_light_transmittance"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_VolumetricCloudJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_volumetric_cloud_properties - Read the properties of a VolumetricCloud actor's UVolumetricCloudComponent, under the names set_volumetric_cloud_properties takes.
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

// MCP:set_sky_light_properties - Set the properties of a SkyLight actor's USkyLightComponent: intensity, light color, indirect and volumetric intensity, lower-hemisphere color and occlusion. Only the supplied fields change (applied_count counts them) and the reply reads every property back. Colors are [R,G,B] or [R,G,B,A].
//   @param actor_name - Label/name of the target SkyLight actor.
//   @param intensity [number] - Optional sky light intensity.
//   @param light_color [array] - Optional [R,G,B(,A)] light color (stored as FColor).
//   @param indirect_lighting_intensity [number] - Optional indirect lighting intensity scale.
//   @param volumetric_scattering_intensity [number] - Optional volumetric scattering intensity.
//   @param lower_hemisphere_color [array] - Optional [R,G,B(,A)] lower-hemisphere color (FLinearColor).
//   @param occlusion_tint [array] - Optional [R,G,B] occlusion tint as 0-255 bytes (FColor).
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

    const FScopedTransaction Transaction(LOCTEXT("SetSkyLightProperties", "MCP: Set Sky Light Properties"));
    Component->Modify();

    int32 AppliedCount = 0;
    double Number = 0.0;
    const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
    FLinearColor LinearColor(ForceInit);
    FColor ByteColor(ForceInit);

    if (Params->TryGetNumberField(TEXT("intensity"), Number))
    {
        Component->SetIntensity(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("light_color"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetLightColor(LinearColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("indirect_lighting_intensity"), Number))
    {
        Component->SetIndirectLightingIntensity(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("volumetric_scattering_intensity"), Number))
    {
        Component->SetVolumetricScatteringIntensity(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("lower_hemisphere_color"), Values) && PropertyHandlers_ParseLinearColor(Values, LinearColor))
    {
        Component->SetLowerHemisphereColor(LinearColor);
        ++AppliedCount;
    }
    if (Params->TryGetArrayField(TEXT("occlusion_tint"), Values) && PropertyHandlers_ParseColorBytes(Values, ByteColor))
    {
        Component->SetOcclusionTint(ByteColor);
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("occlusion_contrast"), Number))
    {
        Component->SetOcclusionContrast(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("occlusion_exponent"), Number))
    {
        Component->SetOcclusionExponent(static_cast<float>(Number));
        ++AppliedCount;
    }
    if (Params->TryGetNumberField(TEXT("min_occlusion"), Number))
    {
        Component->SetMinOcclusion(static_cast<float>(Number));
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No properties provided. Supply at least one of: intensity, light_color, indirect_lighting_intensity, volumetric_scattering_intensity, lower_hemisphere_color, occlusion_tint, occlusion_contrast, occlusion_exponent, min_occlusion"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("actor_name"), ActorName);
    Result->SetObjectField(TEXT("properties"), PropertyHandlers_SkyLightJson(Component));
    Result->SetNumberField(TEXT("applied_count"), AppliedCount);
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_sky_light_properties - Read the properties of a SkyLight actor's USkyLightComponent, under the names set_sky_light_properties takes.
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
