#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Components/LightComponentBase.h"     // SetCastShadows / SetCastVolumetricShadow
#include "Components/LightComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LocalLightComponent.h"   // ULocalLightComponent::SetAttenuationRadius
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Components/RectLightComponent.h"     // URectLightComponent source/barn-door
#include "Components/SkyLightComponent.h"      // USkyLightComponent: a ULightComponentBase, counted by get_lighting_quality
#include "Engine/TextureLightProfile.h"        // IES profile asset for SetIESTexture
#include "Components/StaticMeshComponent.h"
#include "UnrealEngine.h"
#include "Components/SkeletalMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Particles/ParticleSystemComponent.h"  // UFXSystemComponent: base of Cascade and Niagara components
#include "ScopedTransaction.h"      // FScopedTransaction for set_post_process_volume_settings
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/PostProcessVolume.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/IConsoleManager.h"
#include "Engine/RendererSettings.h"
#include "GameFramework/GameUserSettings.h"
#include "RenderingThread.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameEngine.h"
#include "ImageUtils.h"
#include "Kismet/GameplayStatics.h"
#include "SceneTypes.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "DynamicRHI.h"                 // GDynamicRHI: the RHI really in use

namespace UEAgentLightingCommands
{

// MCP-DOMAIN: Lighting

namespace UEAgentLightingImpl
{
    // =============================================================================================
    // Shared building blocks
    // =============================================================================================

    // The first world context the engine holds, which most commands of this file work on;
    // get_lighting_quality reads the editor world instead.
    UWorld* FirstContextWorld()
    {
        return mcp_common::GetFirstContextWorld();
    }

    // Error reply for a required field the request did not carry.
    TSharedPtr<FJsonObject> MissingParameterReply(const TCHAR* FieldName)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Missing '%s' parameter"), FieldName));
    }

    // Key/value pair used to write several numeric fields as one braced list.
    struct FNumberEntry
    {
        const TCHAR* Key;
        double Value;
    };

    void SetNumbers(FJsonObject& Target, std::initializer_list<FNumberEntry> Entries)
    {
        for (const FNumberEntry& Entry : Entries)
        {
            Target.SetNumberField(Entry.Key, Entry.Value);
        }
    }

    FString AssetPathOrEmpty(const UObject* Asset)
    {
        return Asset != nullptr ? Asset->GetPathName() : FString();
    }

    // A rating band: the label applies from Floor upwards.
    struct FScoreBand
    {
        float Floor;
        const TCHAR* Label;
    };

    // Label of the first band whose floor the score reaches (bands listed from the highest floor down),
    // or Below when it reaches none of them.
    const TCHAR* BandFor(const float Score, std::initializer_list<FScoreBand> Bands, const TCHAR* Below)
    {
        for (const FScoreBand& Band : Bands)
        {
            if (Score >= Band.Floor)
            {
                return Band.Label;
            }
        }
        return Below;
    }

    // Adds Weight * clamp(Count / Saturation, 0, 1) to Score when Count is positive.
    void AddSaturatingTerm(float& Score, const int32 Count, const float Saturation, const float Weight)
    {
        if (Count > 0)
        {
            Score += FMath::Clamp(Count / Saturation, 0.0f, 1.0f) * Weight;
        }
    }

    void AddAdviceIf(TArray<TSharedPtr<FJsonValue>>& Advice, const bool bApplies, const TCHAR* Text)
    {
        if (bApplies)
        {
            Advice.Add(MakeShared<FJsonValueString>(Text));
        }
    }

    int32 ConsoleIntOr(const TCHAR* Name, const int32 Fallback)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable != nullptr ? Variable->GetInt() : Fallback;
    }

    float ConsoleFloatOr(const TCHAR* Name, const float Fallback)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable != nullptr ? Variable->GetFloat() : Fallback;
    }

    // =============================================================================================
    // Light lookup
    // =============================================================================================

    // First actor of World named LightName (FString ==, so case is ignored) that also owns a light
    // component. Only actors already present in the world are visited; no asset is loaded.
    AActor* FindNamedLightActor(UWorld* World, const FString& LightName)
    {
        if (World == nullptr || LightName.IsEmpty())
        {
            return nullptr;
        }
        for (AActor* Candidate : TActorRange<AActor>(World))
        {
            if (Candidate->GetName() == LightName && Candidate->FindComponentByClass<ULightComponent>() != nullptr)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    // Outcome of resolving a light_name: either Actor and Light are both set, or Error holds the reply.
    struct FLightLookup
    {
        AActor* Actor = nullptr;
        ULightComponent* Light = nullptr;
        TSharedPtr<FJsonObject> Error;
    };

    FLightLookup LookUpLight(const FString& LightName)
    {
        FLightLookup Lookup;
        UWorld* World = FirstContextWorld();
        if (World == nullptr)
        {
            Lookup.Error = mcp_common::MakeErrorReply(TEXT("No world found"));
            return Lookup;
        }
        Lookup.Actor = FindNamedLightActor(World, LightName);
        if (Lookup.Actor == nullptr)
        {
            Lookup.Error = mcp_common::MakeErrorReply(FString::Printf(TEXT("Light actor not found: %s"), *LightName));
            return Lookup;
        }
        Lookup.Light = Lookup.Actor->FindComponentByClass<ULightComponent>();
        if (Lookup.Light == nullptr)
        {
            Lookup.Error = mcp_common::MakeErrorReply(FString::Printf(TEXT("Light component not found on actor: %s"), *LightName));
        }
        return Lookup;
    }

    // =============================================================================================
    // Light state shared by the per-light replies
    // =============================================================================================

    // light_enabled and light_mobility, plus the optional cast-shadows/affects-world pair and intensity;
    // each caller picks the extras it reports. Every field read here lives on ULightComponentBase, so a sky light
    // reports them too.
    void WriteLightStatus(const ULightComponentBase& Light, FJsonObject& Out, const bool bShadowSwitches, const bool bIntensity)
    {
        Out.SetBoolField(TEXT("light_enabled"), Light.IsVisible());
        if (bShadowSwitches)
        {
            Out.SetBoolField(TEXT("light_cast_shadows"), Light.CastShadows != 0);
            Out.SetBoolField(TEXT("light_affect_world"), Light.bAffectsWorld != 0);
        }
        if (bIntensity)
        {
            Out.SetNumberField(TEXT("light_intensity"), Light.Intensity);
        }
        Out.SetStringField(TEXT("light_mobility"), mcp_common::MobilityLabel(Light.Mobility));
    }

    // light_type block of the intensity, color and shadow commands. A spot light IS a point light
    // (USpotLightComponent derives from UPointLightComponent), so Spot is tested before Point; a rect
    // light is a local light but no point light.
    void WriteSetterTypeBlock(const ULightComponent& Light, FJsonObject& Out, const bool bShadowDetail)
    {
        if (const UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(&Light))
        {
            Out.SetStringField(TEXT("light_type"), TEXT("DirectionalLight"));
            Out.SetNumberField(TEXT("light_atmosphere_sun_light"), Directional->bAtmosphereSunLight ? 1.0f : 0.0f);
            if (bShadowDetail)
            {
                Out.SetNumberField(TEXT("light_cascade_distribution_exponent"), Directional->CascadeDistributionExponent);
                Out.SetNumberField(TEXT("light_cascade_transition_fraction"), Directional->CascadeTransitionFraction);
            }
        }
        else if (const USpotLightComponent* Spot = Cast<USpotLightComponent>(&Light))
        {
            Out.SetStringField(TEXT("light_type"), TEXT("SpotLight"));
            Out.SetNumberField(TEXT("light_inner_cone_angle"), Spot->InnerConeAngle);
            Out.SetNumberField(TEXT("light_outer_cone_angle"), Spot->OuterConeAngle);
            if (bShadowDetail)
            {
                Out.SetNumberField(TEXT("light_attenuation_radius"), Spot->AttenuationRadius);
                Out.SetNumberField(TEXT("light_source_radius"), Spot->SourceRadius);
                Out.SetNumberField(TEXT("light_source_length"), Spot->SourceLength);
            }
        }
        else if (const UPointLightComponent* Point = Cast<UPointLightComponent>(&Light))
        {
            Out.SetStringField(TEXT("light_type"), TEXT("PointLight"));
            Out.SetNumberField(TEXT("light_attenuation_radius"), Point->AttenuationRadius);
            if (bShadowDetail)
            {
                Out.SetNumberField(TEXT("light_source_radius"), Point->SourceRadius);
                Out.SetNumberField(TEXT("light_source_length"), Point->SourceLength);
            }
        }
        else if (const URectLightComponent* Rect = Cast<URectLightComponent>(&Light))
        {
            Out.SetStringField(TEXT("light_type"), TEXT("RectLight"));
            Out.SetNumberField(TEXT("light_attenuation_radius"), Rect->AttenuationRadius);
            if (bShadowDetail)
            {
                Out.SetNumberField(TEXT("light_source_width"), Rect->SourceWidth);
                Out.SetNumberField(TEXT("light_source_height"), Rect->SourceHeight);
            }
        }
        else
        {
            Out.SetStringField(TEXT("light_type"), TEXT("Unknown"));
        }
    }

    // =============================================================================================
    // get_light_properties
    // =============================================================================================

    // light_subtype block, mirroring the per-type fields set_light_properties writes.
    void WriteReaderSubtype(const ULightComponent& Light, FJsonObject& Out)
    {
        if (const UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(&Light))
        {
            Out.SetStringField(TEXT("light_subtype"), TEXT("DirectionalLight"));
            Out.SetNumberField(TEXT("light_atmosphere_sun_light"), Directional->bAtmosphereSunLight ? 1.0f : 0.0f);
            Out.SetNumberField(TEXT("light_atmosphere_sun_light_index"), Directional->AtmosphereSunLightIndex);
            Out.SetObjectField(TEXT("light_atmosphere_sun_disk_color_scale"), mcp_common::LinearColorToJsonObject(Directional->AtmosphereSunDiskColorScale));
            return;
        }

        // A rect light is a local light but not a point light, so it is matched before the point branch.
        if (const URectLightComponent* Rect = Cast<URectLightComponent>(&Light))
        {
            Out.SetStringField(TEXT("light_subtype"), TEXT("RectLight"));
            SetNumbers(Out, {
                { TEXT("light_attenuation_radius"), Rect->AttenuationRadius },
                { TEXT("light_source_width"), Rect->SourceWidth },
                { TEXT("light_source_height"), Rect->SourceHeight },
                { TEXT("light_barn_door_angle"), Rect->BarnDoorAngle },
                { TEXT("light_barn_door_length"), Rect->BarnDoorLength } });
            Out.SetStringField(TEXT("light_source_texture"), AssetPathOrEmpty(Rect->SourceTexture.Get()));
            return;
        }

        // Spot lights share every point field and add their cone.
        if (const UPointLightComponent* Point = Cast<UPointLightComponent>(&Light))
        {
            const USpotLightComponent* Spot = Cast<USpotLightComponent>(&Light);
            Out.SetStringField(TEXT("light_subtype"), Spot != nullptr ? TEXT("SpotLight") : TEXT("PointLight"));
            SetNumbers(Out, {
                { TEXT("light_attenuation_radius"), Point->AttenuationRadius },
                { TEXT("light_source_radius"), Point->SourceRadius },
                { TEXT("light_soft_source_radius"), Point->SoftSourceRadius },
                { TEXT("light_source_length"), Point->SourceLength } });
            if (Spot != nullptr)
            {
                SetNumbers(Out, { { TEXT("light_inner_cone_angle"), Spot->InnerConeAngle }, { TEXT("light_outer_cone_angle"), Spot->OuterConeAngle } });
            }
            return;
        }

        Out.SetStringField(TEXT("light_subtype"), TEXT("Unknown"));
    }

    // Complete get_light_properties reply for a resolved light.
    TSharedPtr<FJsonObject> DescribeLight(const FString& LightName, const AActor& Owner, const ULightComponent& Light)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("light_name"), LightName);
        Out->SetBoolField(TEXT("light_found"), true);
        Out->SetStringField(TEXT("light_message"), TEXT("Light properties retrieved successfully"));
        Out->SetStringField(TEXT("light_class"), Owner.GetClass()->GetName());
        Out->SetStringField(TEXT("light_type"), TEXT("Light Component"));

        // Placement of the owning actor.
        Out->SetObjectField(TEXT("light_position"), mcp_common::VectorToJsonObject(Owner.GetActorLocation()));
        Out->SetObjectField(TEXT("light_rotation"), mcp_common::RotatorToJsonObject(Owner.GetActorRotation()));
        Out->SetObjectField(TEXT("light_scale"), mcp_common::VectorToJsonObject(Owner.GetActorScale3D()));

        // Emission and state. No intensity-units property is read: the field is always 0.
        Out->SetNumberField(TEXT("light_intensity_units"), 0.0);
        Out->SetObjectField(TEXT("light_color"), mcp_common::LinearColorToJsonObject(FLinearColor(Light.LightColor)));
        WriteLightStatus(Light, *Out, true, true);

        // Falloff only exists on point and spot lights; every other kind reports the inverse-squared
        // defaults. Minimum roughness and the channel mask are fixed values for every light.
        const UPointLightComponent* PointLike = Cast<UPointLightComponent>(&Light);
        Out->SetBoolField(TEXT("light_use_inverse_squared_falloff"), PointLike != nullptr ? PointLike->bUseInverseSquaredFalloff != 0 : true);
        Out->SetNumberField(TEXT("light_falloff_exponent"), PointLike != nullptr ? PointLike->LightFalloffExponent : 2.0f);
        SetNumbers(*Out, { { TEXT("light_min_roughness"), 0.0 }, { TEXT("light_lighting_channel_mask"), 1.0 } });

        // The remaining ULightComponent fields set_light_properties can write, so both commands round-trip.
        SetNumbers(*Out, {
            { TEXT("light_temperature"), Light.Temperature },
            { TEXT("light_indirect_lighting_intensity"), Light.IndirectLightingIntensity },
            { TEXT("light_volumetric_scattering_intensity"), Light.VolumetricScatteringIntensity },
            { TEXT("light_ies_brightness_scale"), Light.IESBrightnessScale },
            { TEXT("light_function_fade_distance"), Light.LightFunctionFadeDistance },
            { TEXT("light_function_disabled_brightness"), Light.DisabledBrightness },
            { TEXT("light_shadow_bias"), Light.ShadowBias },
            { TEXT("light_contact_shadow_length"), Light.ContactShadowLength } });
        Out->SetBoolField(TEXT("light_use_temperature"), Light.bUseTemperature != 0);
        Out->SetBoolField(TEXT("light_affect_translucent_lighting"), Light.bAffectTranslucentLighting != 0);
        Out->SetBoolField(TEXT("light_use_ies_brightness"), Light.bUseIESBrightness != 0);
        Out->SetBoolField(TEXT("light_cast_volumetric_shadow"), Light.bCastVolumetricShadow != 0);
        Out->SetBoolField(TEXT("light_contact_shadow_length_in_ws"), Light.GetContactShadowLengthInWS());
        Out->SetStringField(TEXT("light_ies_texture"), AssetPathOrEmpty(Light.IESTexture.Get()));
        Out->SetStringField(TEXT("light_function_material"), AssetPathOrEmpty(Light.LightFunctionMaterial.Get()));
        Out->SetObjectField(TEXT("light_function_scale"), mcp_common::VectorToJsonObject(Light.LightFunctionScale));

        WriteReaderSubtype(Light, *Out);

        // Where the actor lives.
        TArray<TSharedPtr<FJsonValue>> TagValues;
        for (const FName& Tag : Owner.Tags)
        {
            TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
        }
        Out->SetArrayField(TEXT("light_tags"), TagValues);
        Out->SetStringField(TEXT("light_level"), Owner.GetLevel()->GetName());
        Out->SetStringField(TEXT("light_world"), Owner.GetWorld()->GetName());
        return Out;
    }

    // =============================================================================================
    // set_light_color
    // =============================================================================================

    // Accepts "R,G,B" or "R,G,B,A" (floats, alpha defaulting to 1) and "#RRGGBB" (hex bytes scaled to
    // 0..1). Text with three or more comma-separated parts always takes the float route.
    bool ParseColorText(const FString& Text, FLinearColor& OutColor)
    {
        TArray<FString> Parts;
        Text.ParseIntoArray(Parts, TEXT(","), true);
        if (Parts.Num() >= 3)
        {
            const float Alpha = Parts.Num() > 3 ? FCString::Atof(*Parts[3]) : 1.0f;
            OutColor = FLinearColor(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]), Alpha);
            return true;
        }

        const FString Hex = Text.StartsWith(TEXT("#")) ? Text.Mid(1) : FString();
        if (Hex.Len() != 6)
        {
            return false;
        }
        const auto Channel = [&Hex](const int32 Offset)
        {
            return FCString::Strtoi(*Hex.Mid(Offset, 2), nullptr, 16) / 255.0f;
        };
        OutColor = FLinearColor(Channel(0), Channel(2), Channel(4), 1.0f);
        return true;
    }

    // =============================================================================================
    // configure_light_shadows
    // =============================================================================================

    // A named shadow preset. Only bCastShadows is applied to the light; the three numbers are reported
    // in the reply but never written to the component.
    struct FShadowPreset
    {
        const TCHAR* Name;
        bool bCastShadows;
        bool bSetsBias;  // Enabled/Disabled only switch shadows; the quality presets also set the bias
        float Bias;
        float SlopeBias;
        float FilterSharpen;
    };

    // Preset names compare with FString ==, so case is ignored. An unknown name is null.
    const FShadowPreset* FindShadowPreset(const FString& Requested)
    {
        static const FShadowPreset Presets[] =
        {
            { TEXT("Enabled"),  true,  false, 0.0f, 0.0f, 0.0f },
            { TEXT("Disabled"), false, false, 0.0f, 0.0f, 0.0f },
            { TEXT("High"),     true,  true,  0.1f, 0.1f, 1.0f },
            { TEXT("Medium"),   true,  true,  0.2f, 0.2f, 0.5f },
            { TEXT("Low"),      true,  true,  0.5f, 0.5f, 0.0f },
        };
        for (const FShadowPreset& Preset : Presets)
        {
            if (Requested == Preset.Name)
            {
                return &Preset;
            }
        }
        return nullptr;
    }

    // =============================================================================================
    // get_lighting_quality
    // =============================================================================================

    // Name of the r.LightingQuality level: "Unknown" without the variable, "Custom" outside 0..3.
    FString LightingQualityName()
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.LightingQuality"));
        if (Variable == nullptr)
        {
            return TEXT("Unknown");
        }
        static const TCHAR* const LevelNames[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic") };
        const int32 Level = Variable->GetInt();
        return (Level >= 0 && Level <= 3) ? FString(LevelNames[Level]) : FString(TEXT("Custom"));
    }

    // "Dynamic" when r.DynamicLighting is positive, "Static" otherwise, "Unknown" without the variable.
    FString LightingTypeName()
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicLighting"));
        if (Variable == nullptr)
        {
            return TEXT("Unknown");
        }
        return Variable->GetInt() > 0 ? TEXT("Dynamic") : TEXT("Static");
    }

    // Per-light tallies of get_lighting_quality, accumulated per light component in actor iteration order. A sky
    // light is a ULightComponentBase but no ULightComponent (USkyLightComponent derives from the base directly),
    // so the tally takes the base and counts sky lights apart from the four ULightComponent types.
    struct FLightTally
    {
        int32 Lights = 0;
        int32 Directional = 0;
        int32 Point = 0;
        int32 Spot = 0;
        int32 Rect = 0;
        int32 Sky = 0;
        int32 StaticMobility = 0;
        int32 StationaryMobility = 0;
        int32 MovableMobility = 0;
        int32 ShadowCasters = 0;
        int32 WorldAffecting = 0;
        float IntensitySum = 0.0f;
        float IntensityMax = 0.0f;
        float IntensityMin = FLT_MAX;
        TArray<TSharedPtr<FJsonValue>> Details;

        void Count(const AActor& Owner, const ULightComponentBase& Light)
        {
            ++Lights;

            // A spot light is also a point light, so Spot is tested first.
            if (Light.IsA<UDirectionalLightComponent>())
            {
                ++Directional;
            }
            else if (Light.IsA<USpotLightComponent>())
            {
                ++Spot;
            }
            else if (Light.IsA<UPointLightComponent>())
            {
                ++Point;
            }
            else if (Light.IsA<URectLightComponent>())
            {
                ++Rect;
            }
            else if (Light.IsA<USkyLightComponent>())
            {
                ++Sky;
            }

            switch (Light.Mobility.GetValue())
            {
                case EComponentMobility::Static:     ++StaticMobility;     break;
                case EComponentMobility::Stationary: ++StationaryMobility; break;
                case EComponentMobility::Movable:    ++MovableMobility;    break;
                default:                                                    break;
            }

            ShadowCasters += Light.CastShadows ? 1 : 0;
            WorldAffecting += Light.bAffectsWorld ? 1 : 0;

            const float Intensity = Light.Intensity;
            IntensitySum += Intensity;
            IntensityMax = FMath::Max(IntensityMax, Intensity);
            IntensityMin = FMath::Min(IntensityMin, Intensity);

            // One entry per light component: an actor with several has several entries with the same light_name,
            // told apart by light_component and light_component_class.
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("light_name"), Owner.GetName());
            Entry->SetStringField(TEXT("light_class"), Owner.GetClass()->GetName());
            Entry->SetStringField(TEXT("light_component"), Light.GetName());
            Entry->SetStringField(TEXT("light_component_class"), Light.GetClass()->GetName());
            WriteLightStatus(Light, *Entry, true, true);
            Entry->SetObjectField(TEXT("light_position"), mcp_common::VectorToJsonObject(Owner.GetActorLocation()));
            Entry->SetObjectField(TEXT("light_color"), mcp_common::LinearColorToJsonObject(FLinearColor(Light.LightColor)));
            Details.Add(MakeShared<FJsonValueObject>(Entry));
        }
    };

    // 0..1 rating: 30% light count (saturating at ten lights), 30% shadow casters, 20% lights that affect
    // the world and 20% variety of light types. A level without lights scores 0.
    float LightingScore(const FLightTally& Tally)
    {
        float Score = 0.0f;
        if (Tally.Lights > 0)
        {
            const float LightTotal = static_cast<float>(Tally.Lights);
            AddSaturatingTerm(Score, Tally.Lights, 10.0f, 0.3f);
            AddSaturatingTerm(Score, Tally.ShadowCasters, LightTotal, 0.3f);
            AddSaturatingTerm(Score, Tally.WorldAffecting, LightTotal, 0.2f);
            float Variety = 0.0f;
            Variety += Tally.Directional > 0 ? 0.3f : 0.0f;
            Variety += Tally.Point > 0 ? 0.3f : 0.0f;
            Variety += Tally.Spot > 0 ? 0.4f : 0.0f;
            Score += Variety * 0.2f;
        }
        return Score;
    }

    TSharedPtr<FJsonObject> LightingQualityReport(UWorld& World)
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("level_name"), World.GetName());
        Report->SetBoolField(TEXT("lighting_quality_found"), true);
        Report->SetStringField(TEXT("lighting_quality_message"), TEXT("Lighting quality retrieved successfully using official APIs"));
        Report->SetStringField(TEXT("lighting_quality"), LightingQualityName());
        Report->SetStringField(TEXT("lighting_type"), LightingTypeName());
        // There is no per-world switch to read: lighting is always reported as on.
        Report->SetBoolField(TEXT("lighting_enabled"), true);

        // Every light component of every actor, sky lights included (see FLightTally).
        FLightTally Tally;
        for (AActor* Owner : TActorRange<AActor>(&World))
        {
            TInlineComponentArray<ULightComponentBase*> LightComponents(Owner);
            for (const ULightComponentBase* Light : LightComponents)
            {
                if (Light != nullptr)
                {
                    Tally.Count(*Owner, *Light);
                }
            }
        }

        Report->SetNumberField(TEXT("light_count"), Tally.Lights);
        Report->SetNumberField(TEXT("directional_light_count"), Tally.Directional);
        Report->SetNumberField(TEXT("point_light_count"), Tally.Point);
        Report->SetNumberField(TEXT("spot_light_count"), Tally.Spot);
        Report->SetNumberField(TEXT("rect_light_count"), Tally.Rect);
        Report->SetNumberField(TEXT("sky_light_count"), Tally.Sky);
        Report->SetNumberField(TEXT("static_light_count"), Tally.StaticMobility);
        Report->SetNumberField(TEXT("stationary_light_count"), Tally.StationaryMobility);
        Report->SetNumberField(TEXT("movable_light_count"), Tally.MovableMobility);
        Report->SetNumberField(TEXT("shadow_casting_light_count"), Tally.ShadowCasters);
        Report->SetNumberField(TEXT("world_affecting_light_count"), Tally.WorldAffecting);

        const float Average = Tally.Lights > 0 ? Tally.IntensitySum / Tally.Lights : 0.0f;
        Report->SetNumberField(TEXT("total_intensity"), Tally.IntensitySum);
        Report->SetNumberField(TEXT("average_intensity"), Average);
        Report->SetNumberField(TEXT("max_intensity"), Tally.IntensityMax);
        Report->SetNumberField(TEXT("min_intensity"), Tally.IntensityMin == FLT_MAX ? 0.0f : Tally.IntensityMin);
        Report->SetArrayField(TEXT("light_details"), Tally.Details);

        const float Score = LightingScore(Tally);
        Report->SetNumberField(TEXT("lighting_quality_score"), Score);
        Report->SetStringField(TEXT("lighting_quality_rating"),
            BandFor(Score, { { 0.8f, TEXT("Excellent") }, { 0.6f, TEXT("Good") }, { 0.4f, TEXT("Fair") } }, TEXT("Poor")));
        return Report;
    }

    // =============================================================================================
    // get_rendering_settings / get_shader_info
    // =============================================================================================

    // Actor counts shared by the rendering and shader reports; a null world yields all zeros.
    struct FActorCensus
    {
        int32 Actors = 0;
        int32 WithLight = 0;
        int32 WithStaticMesh = 0;
        int32 WithSkeletalMesh = 0;
        int32 WithCamera = 0;
        int32 PostProcessVolumes = 0;
        int32 WithParticles = 0;  // actors with an FX component (Niagara or Cascade)
    };

    FActorCensus CountActors(UWorld* World)
    {
        FActorCensus Census;
        if (World == nullptr)
        {
            return Census;
        }
        for (AActor* Actor : TActorRange<AActor>(World))
        {
            ++Census.Actors;
            Census.WithLight += Actor->FindComponentByClass<ULightComponent>() != nullptr ? 1 : 0;
            Census.WithStaticMesh += Actor->FindComponentByClass<UStaticMeshComponent>() != nullptr ? 1 : 0;
            Census.WithSkeletalMesh += Actor->FindComponentByClass<USkeletalMeshComponent>() != nullptr ? 1 : 0;
            Census.WithCamera += Actor->FindComponentByClass<UCameraComponent>() != nullptr ? 1 : 0;
            Census.PostProcessVolumes += Actor->IsA<APostProcessVolume>() ? 1 : 0;
            Census.WithParticles += Actor->FindComponentByClass<UFXSystemComponent>() != nullptr ? 1 : 0;
        }
        return Census;
    }

    const TCHAR* WorldTypeName(const EWorldType::Type Type)
    {
        switch (Type)
        {
            case EWorldType::Game:   return TEXT("Game");
            case EWorldType::Editor: return TEXT("Editor");
            case EWorldType::PIE:    return TEXT("PIE");
            default:                 return TEXT("Unknown");
        }
    }

    // Engine version plus the platform fields; architecture and OS are fixed strings.
    void WriteEngineAndPlatform(FJsonObject& Out)
    {
        Out.SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
        Out.SetStringField(TEXT("platform_name"), FPlatformProperties::PlatformName());
        Out.SetStringField(TEXT("platform_architecture"), TEXT("x64"));
        Out.SetStringField(TEXT("platform_os"), TEXT("Windows"));
    }

    // Quality bucket of r.ScreenPercentage, or "Unknown" without the variable.
    FString ScreenPercentageQuality()
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
        if (Variable == nullptr)
        {
            return TEXT("Unknown");
        }
        return BandFor(Variable->GetFloat(), { { 100.0f, TEXT("Epic") }, { 80.0f, TEXT("High") }, { 60.0f, TEXT("Medium") } }, TEXT("Low"));
    }

    // Renderer feature switches as their console variables have them right now, written as
    // "<Prefix><feature>_enabled". On whenever the variable is non-zero (AO's -1 is "automatic",
    // which renders it). A variable this build does not register is left out of the reply rather
    // than reported with a guessed default.
    void WriteFeatureSwitches(FJsonObject& Out, const FString& Prefix)
    {
        struct FFeature
        {
            const TCHAR* Name;
            const TCHAR* Variable;
        };
        static const FFeature Features[] =
        {
            { TEXT("shadows_enabled"), TEXT("r.ShadowQuality") },
            { TEXT("fog_enabled"), TEXT("r.Fog") },
            { TEXT("atmosphere_enabled"), TEXT("r.SupportSkyAtmosphere") },
            { TEXT("anti_aliasing_enabled"), TEXT("r.AntiAliasingMethod") },
            { TEXT("motion_blur_enabled"), TEXT("r.MotionBlurQuality") },
            { TEXT("depth_of_field_enabled"), TEXT("r.DepthOfFieldQuality") },
            { TEXT("ambient_occlusion_enabled"), TEXT("r.AmbientOcclusionLevels") },
            { TEXT("global_illumination_enabled"), TEXT("r.DynamicGlobalIlluminationMethod") },
        };
        IConsoleManager& Console = IConsoleManager::Get();
        for (const FFeature& Feature : Features)
        {
            if (const IConsoleVariable* Variable = Console.FindConsoleVariable(Feature.Variable))
            {
                Out.SetBoolField(Prefix + Feature.Name, Variable->GetInt() != 0);
            }
        }
    }

    // "Forward Rendering"/"Deferred Rendering" style label from r.ForwardShading; "Unknown" without it.
    const TCHAR* ShadingPathName(const bool bShaderWording)
    {
        const IConsoleVariable* Forward = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ForwardShading"));
        if (Forward == nullptr)
        {
            return TEXT("Unknown");
        }
        if (Forward->GetInt() != 0)
        {
            return bShaderWording ? TEXT("Forward Shading") : TEXT("Forward Rendering");
        }
        return bShaderWording ? TEXT("Deferred Shading") : TEXT("Deferred Rendering");
    }

    void WriteWorldSummary(UWorld& World, const FActorCensus& Census, FJsonObject& Out)
    {
        Out.SetStringField(TEXT("world_name"), World.GetName());
        Out.SetStringField(TEXT("world_type"), WorldTypeName(World.WorldType));

        // What the world contains: a light, a shadow-casting light, a height fog, a sky atmosphere.
        bool bShadowCaster = false;
        bool bFog = false;
        bool bAtmosphere = false;
        for (AActor* Actor : TActorRange<AActor>(&World))
        {
            const ULightComponent* Light = Actor->FindComponentByClass<ULightComponent>();
            bShadowCaster |= Light != nullptr && Light->CastShadows;
            bFog |= Actor->FindComponentByClass<UExponentialHeightFogComponent>() != nullptr;
            bAtmosphere |= Actor->FindComponentByClass<USkyAtmosphereComponent>() != nullptr;
        }
        Out.SetBoolField(TEXT("world_lighting_enabled"), Census.WithLight > 0);
        Out.SetBoolField(TEXT("world_shadows_enabled"), bShadowCaster);
        Out.SetBoolField(TEXT("world_fog_enabled"), bFog);
        Out.SetBoolField(TEXT("world_atmosphere_enabled"), bAtmosphere);
        // The engine's frame cap (0 = uncapped), not a fixed 60.
        Out.SetNumberField(TEXT("world_max_tick_rate"), GEngine != nullptr ? GEngine->GetMaxFPS() : 0.0f);

        Out.SetNumberField(TEXT("world_delta_seconds"), World.GetDeltaSeconds());
        Out.SetNumberField(TEXT("world_time_seconds"), World.GetTimeSeconds());
        Out.SetNumberField(TEXT("world_real_time_seconds"), World.GetRealTimeSeconds());
        Out.SetNumberField(TEXT("world_level_count"), World.GetNumLevels());
        Out.SetStringField(TEXT("world_persistent_level"), World.PersistentLevel != nullptr ? World.PersistentLevel->GetName() : FString());

        Out.SetNumberField(TEXT("world_actor_count"), Census.Actors);
        Out.SetNumberField(TEXT("world_light_actor_count"), Census.WithLight);
        Out.SetNumberField(TEXT("world_static_mesh_actor_count"), Census.WithStaticMesh);
        Out.SetNumberField(TEXT("world_skeletal_mesh_actor_count"), Census.WithSkeletalMesh);
        Out.SetNumberField(TEXT("world_camera_actor_count"), Census.WithCamera);
        Out.SetNumberField(TEXT("world_post_process_actor_count"), Census.PostProcessVolumes);
    }

    struct FRenderQualityVerdict
    {
        const TCHAR* Level;
        float Score;
    };

    // High needs the high material level and full view distance, Medium the medium level and at least
    // three quarters of it; everything else is Low.
    FRenderQualityVerdict JudgeRenderQuality(const EMaterialQualityLevel::Type Level, const float ViewDistance)
    {
        if (Level == EMaterialQualityLevel::High && ViewDistance >= 1.0f)
        {
            return { TEXT("High"), 0.9f };
        }
        if (Level == EMaterialQualityLevel::Medium && ViewDistance >= 0.75f)
        {
            return { TEXT("Medium"), 0.7f };
        }
        return { TEXT("Low"), 0.5f };
    }

    // Scalability readings of get_rendering_settings and the quality verdict derived from them.
    void WriteRenderingScalability(FJsonObject& Out)
    {
        // The level goes through the byte-wide enum.
        const EMaterialQualityLevel::Type MaterialLevel = static_cast<EMaterialQualityLevel::Type>(ConsoleIntOr(TEXT("r.MaterialQualityLevel"), 1));
        const float ViewDistance = ConsoleFloatOr(TEXT("r.ViewDistanceScale"), 1.0f);
        const int32 ShadowResolution = ConsoleIntOr(TEXT("r.Shadow.MaxResolution"), 2048);
        const int32 CascadeResolution = ConsoleIntOr(TEXT("r.Shadow.MaxCSMResolution"), 2048);
        const FRenderQualityVerdict Verdict = JudgeRenderQuality(MaterialLevel, ViewDistance);

        Out.SetNumberField(TEXT("view_distance_scale"), ViewDistance);
        Out.SetNumberField(TEXT("max_shadow_resolution"), ShadowResolution);
        Out.SetNumberField(TEXT("max_csm_shadow_resolution"), CascadeResolution);
        WriteFeatureSwitches(Out, TEXT("rendering_"));

        Out.SetStringField(TEXT("rendering_quality_level"), Verdict.Level);
        Out.SetStringField(TEXT("rendering_quality_preset"), Verdict.Level);
        Out.SetBoolField(TEXT("rendering_quality_auto"), false);
        Out.SetNumberField(TEXT("rendering_quality_score"), Verdict.Score);
    }

    // Display block. Target FPS and the frame-rate limit are not readable from the user settings and
    // are fixed values; without user settings the remaining fields take their defaults.
    void WriteDisplaySettings(UGameUserSettings* UserSettings, FJsonObject& Out)
    {
        const float ScreenPercentage = ConsoleFloatOr(TEXT("r.ScreenPercentage"), 100.0f);
        Out.SetNumberField(TEXT("rendering_screen_percentage"), ScreenPercentage);
        Out.SetNumberField(TEXT("rendering_resolution_scale"), ScreenPercentage / 100.0f);
        if (UserSettings == nullptr)
        {
            return;
        }
        // The game user settings: the frame-rate limit the player set (0 = none) and vsync.
        Out.SetNumberField(TEXT("rendering_frame_rate_limit"), UserSettings->GetFrameRateLimit());
        Out.SetBoolField(TEXT("rendering_vsync_enabled"), UserSettings->IsVSyncEnabled());
        const FIntPoint Resolution = UserSettings->GetScreenResolution();
        Out.SetNumberField(TEXT("rendering_resolution_x"), Resolution.X);
        Out.SetNumberField(TEXT("rendering_resolution_y"), Resolution.Y);
    }

    // Label and score of r.MaterialQualityLevel as get_shader_info reports them.
    struct FMaterialQualityInfo
    {
        const TCHAR* Label;
        float Score;
    };

    FMaterialQualityInfo ReadMaterialQuality()
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MaterialQualityLevel"));
        if (Variable == nullptr)
        {
            return { TEXT("Medium"), 0.5f };
        }
        switch (static_cast<EMaterialQualityLevel::Type>(Variable->GetInt()))
        {
            case EMaterialQualityLevel::Low:    return { TEXT("Low"), 0.5f };
            case EMaterialQualityLevel::Medium: return { TEXT("Medium"), 0.7f };
            case EMaterialQualityLevel::High:   return { TEXT("High"), 0.9f };
            case EMaterialQualityLevel::Epic:   return { TEXT("Epic"), 1.0f };
            default:                            return { TEXT("Unknown"), 0.0f };
        }
    }

    // The RHI the editor is really running on ("D3D12", "D3D11", "Vulkan", "Metal", "NullDrv"...),
    // not a guess from the platform it was compiled for.
    const TCHAR* ShaderApiName()
    {
        return GDynamicRHI != nullptr ? GDynamicRHI->GetName() : TEXT("Unknown");
    }

    // Adds the distinct non-null materials of a mesh component's slots to InOut, in slot order.
    void AppendSlotMaterials(const UMeshComponent* Mesh, TArray<UMaterialInterface*>& InOut)
    {
        if (Mesh == nullptr)
        {
            return;
        }
        const int32 SlotCount = Mesh->GetNumMaterials();
        for (int32 Slot = 0; Slot < SlotCount; ++Slot)
        {
            if (UMaterialInterface* Material = Mesh->GetMaterial(Slot))
            {
                InOut.AddUnique(Material);
            }
        }
    }

    TSharedPtr<FJsonObject> DescribeMaterial(const UMaterialInterface& Material)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("material_name"), Material.GetName());
        Entry->SetStringField(TEXT("material_class"), Material.GetClass()->GetName());
        Entry->SetStringField(TEXT("material_path"), Material.GetPathName());
        Entry->SetBoolField(TEXT("material_is_material"), Material.IsA<UMaterial>());
        Entry->SetBoolField(TEXT("material_is_material_instance"), Material.IsA<UMaterialInstanceConstant>());
        Entry->SetBoolField(TEXT("material_is_dynamic_material_instance"), Material.IsA<UMaterialInstanceDynamic>());

        // Parameter counts per kind; the ids returned next to them are not used.
        TArray<FMaterialParameterInfo> Scalars, Vectors, Textures;
        TArray<FGuid> UnusedIds;
        Material.GetAllScalarParameterInfo(Scalars, UnusedIds);
        Material.GetAllVectorParameterInfo(Vectors, UnusedIds);
        Material.GetAllTextureParameterInfo(Textures, UnusedIds);
        Entry->SetNumberField(TEXT("material_scalar_parameter_count"), Scalars.Num());
        Entry->SetNumberField(TEXT("material_vector_parameter_count"), Vectors.Num());
        Entry->SetNumberField(TEXT("material_texture_parameter_count"), Textures.Num());
        Entry->SetNumberField(TEXT("material_total_parameter_count"), Scalars.Num() + Vectors.Num() + Textures.Num());
        return Entry;
    }

    // One entry per actor material: each actor contributes the distinct materials of its first static
    // mesh component followed by those of its first skeletal mesh component.
    TArray<TSharedPtr<FJsonValue>> CollectMaterialEntries(UWorld& World)
    {
        // One entry per DISTINCT material: a material shared by many actors is counted once, not
        // once per actor.
        TArray<UMaterialInterface*> WorldMaterials;
        for (AActor* Actor : TActorRange<AActor>(&World))
        {
            AppendSlotMaterials(Actor->FindComponentByClass<UStaticMeshComponent>(), WorldMaterials);
            AppendSlotMaterials(Actor->FindComponentByClass<USkeletalMeshComponent>(), WorldMaterials);
        }
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (const UMaterialInterface* Material : WorldMaterials)
        {
            Entries.Add(MakeShared<FJsonValueObject>(DescribeMaterial(*Material)));
        }
        return Entries;
    }

    void WriteShaderWorldBlock(UWorld& World, const FActorCensus& Census, FJsonObject& Out)
    {
        const TArray<TSharedPtr<FJsonValue>> Materials = CollectMaterialEntries(World);
        Out.SetStringField(TEXT("world_name"), World.GetName());
        Out.SetNumberField(TEXT("world_material_count"), Materials.Num());
        Out.SetNumberField(TEXT("world_static_mesh_count"), Census.WithStaticMesh);
        Out.SetNumberField(TEXT("world_skeletal_mesh_count"), Census.WithSkeletalMesh);
        Out.SetNumberField(TEXT("world_light_count"), Census.WithLight);
        Out.SetNumberField(TEXT("world_post_process_count"), Census.PostProcessVolumes);
        Out.SetNumberField(TEXT("world_particle_system_count"), Census.WithParticles);
        Out.SetArrayField(TEXT("material_details"), Materials);
    }

    // Shader features and performance figures that are not queried; all fixed values.
    void WriteShaderFeatureState(FJsonObject& Out)
    {
        IConsoleManager& Console = IConsoleManager::Get();
        if (const IConsoleVariable* Forward = Console.FindConsoleVariable(TEXT("r.ForwardShading")))
        {
            Out.SetBoolField(TEXT("shader_forward_shading_enabled"), Forward->GetInt() != 0);
            Out.SetBoolField(TEXT("shader_deferred_shading_enabled"), Forward->GetInt() == 0);
        }
        WriteFeatureSwitches(Out, TEXT("shader_"));
        if (const IConsoleVariable* VSync = Console.FindConsoleVariable(TEXT("r.VSync")))
        {
            Out.SetBoolField(TEXT("shader_vsync_enabled"), VSync->GetInt() != 0);
        }
        if (const IConsoleVariable* MaxFps = Console.FindConsoleVariable(TEXT("t.MaxFPS")))
        {
            Out.SetNumberField(TEXT("shader_frame_rate_limit"), MaxFps->GetFloat());
        }
        if (const IConsoleVariable* Percentage = Console.FindConsoleVariable(TEXT("r.ScreenPercentage")))
        {
            Out.SetNumberField(TEXT("shader_screen_percentage"), Percentage->GetFloat());
            Out.SetNumberField(TEXT("shader_resolution_scale"), Percentage->GetFloat() / 100.0f);
        }
    }

    // Inputs of the shader advice and complexity score. Only the census-backed fields are ever filled:
    // materials and particle systems stay at zero, so their advice line and score term never trigger.
    struct FShaderLoad
    {
        int32 Materials = 0;
        int32 StaticMeshes = 0;
        int32 SkeletalMeshes = 0;
        int32 Lights = 0;
        int32 PostProcessVolumes = 0;
        int32 ParticleSystems = 0;
    };

    TArray<TSharedPtr<FJsonValue>> ShaderAdvice(const FShaderLoad& Load)
    {
        TArray<TSharedPtr<FJsonValue>> Advice;
        AddAdviceIf(Advice, Load.Materials > 100, TEXT("High material count - consider material optimization"));
        AddAdviceIf(Advice, Load.StaticMeshes > 1000, TEXT("High static mesh count - consider LOD optimization"));
        AddAdviceIf(Advice, Load.SkeletalMeshes > 100, TEXT("High skeletal mesh count - consider animation optimization"));
        AddAdviceIf(Advice, Load.Lights > 50, TEXT("High light count - consider shadow optimization"));
        AddAdviceIf(Advice, Load.PostProcessVolumes > 10, TEXT("High post-process count - consider performance impact"));
        AddAdviceIf(Advice, Load.ParticleSystems > 50, TEXT("High particle system count - consider particle optimization"));
        return Advice;
    }

    float ShaderComplexity(const FShaderLoad& Load)
    {
        float Score = 0.0f;
        AddSaturatingTerm(Score, Load.Materials, 50.0f, 0.3f);
        AddSaturatingTerm(Score, Load.StaticMeshes, 200.0f, 0.2f);
        AddSaturatingTerm(Score, Load.SkeletalMeshes, 20.0f, 0.2f);
        AddSaturatingTerm(Score, Load.Lights, 25.0f, 0.2f);
        AddSaturatingTerm(Score, Load.PostProcessVolumes, 5.0f, 0.1f);
        return Score;
    }

    // =============================================================================================
    // configure_post_processing
    // =============================================================================================

    // Features a preset drives, in the order their console variables are written.
    namespace PostFeature
    {
        enum Type : int32
        {
            AntiAliasing,
            Bloom,
            MotionBlur,
            DepthOfField,
            AmbientOcclusion,
            GlobalIllumination,
            ColorGrading,
            LensFlare,
            ScreenSpaceReflections,
            Count
        };
    }

    struct FPostFeatureInfo
    {
        const TCHAR* ConsoleVariable;
        const TCHAR* ReplyKey;
    };

    const FPostFeatureInfo PostFeatures[PostFeature::Count] =
    {
        { TEXT("r.PostProcessAAQuality"),      TEXT("anti_aliasing_enabled") },
        { TEXT("r.BloomQuality"),              TEXT("bloom_enabled") },
        { TEXT("r.MotionBlurQuality"),         TEXT("motion_blur_enabled") },
        { TEXT("r.DepthOfFieldQuality"),       TEXT("depth_of_field_enabled") },
        { TEXT("r.AmbientOcclusionLevels"),    TEXT("ambient_occlusion_enabled") },
        { TEXT("r.GlobalIlluminationQuality"), TEXT("global_illumination_enabled") },
        { TEXT("r.ColorGradingQuality"),       TEXT("color_grading_enabled") },
        { TEXT("r.LensFlareQuality"),          TEXT("lens_flare_enabled") },
        { TEXT("r.SSRQuality"),                TEXT("screen_space_reflections_enabled") },
    };

    // A preset writes Levels[i] to the console variable of feature i (PostFeature order); a feature is
    // reported as enabled when its level is above zero. bPostProcessing is reported as is: "Low" writes
    // the same zeros as "Disabled" yet still counts as post-processing on.
    struct FPostProcessPreset
    {
        const TCHAR* Name;
        bool bPostProcessing;
        int32 Levels[PostFeature::Count];
        const TCHAR* Message;
    };

    // Preset names compare with FString ==, so case is ignored.
    const FPostProcessPreset* FindPostProcessPreset(const FString& Requested)
    {
        static const FPostProcessPreset Presets[] =
        {
            { TEXT("Enabled"),  true,  { 2, 2, 2, 2, 2, 2, 2, 2, 2 }, TEXT("Post processing configured successfully using official APIs") },
            { TEXT("Disabled"), false, { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, TEXT("Post processing disabled successfully using official APIs") },
            { TEXT("High"),     true,  { 2, 2, 2, 2, 2, 2, 2, 2, 2 }, TEXT("High quality post processing configured successfully using official APIs") },
            { TEXT("Medium"),   true,  { 1, 1, 0, 1, 1, 0, 1, 0, 0 }, TEXT("Medium quality post processing configured successfully using official APIs") },
            { TEXT("Low"),      true,  { 0, 0, 0, 0, 0, 0, 0, 0, 0 }, TEXT("Low quality post processing configured successfully using official APIs") },
        };
        for (const FPostProcessPreset& Preset : Presets)
        {
            if (Requested == Preset.Name)
            {
                return &Preset;
            }
        }
        return nullptr;
    }

    // What configure_post_processing reports; the defaults describe an unrecognised preset name.
    struct FPostProcessState
    {
        bool bConfigured = false;
        bool bPostProcessing = false;
        bool Features[PostFeature::Count] = {};
        const TCHAR* Message = TEXT("Invalid post-processing settings. Use 'Enabled', 'Disabled', 'High', 'Medium', or 'Low'");
    };

    // Applies the named preset through console variables. An unknown name changes nothing.
    FPostProcessState ApplyPostProcessSettings(UWorld& World, const FString& Requested)
    {
        FPostProcessState State;
        const FPostProcessPreset* Preset = FindPostProcessPreset(Requested);
        if (Preset == nullptr)
        {
            return State;
        }
        for (int32 Index = 0; Index < PostFeature::Count; ++Index)
        {
            const int32 Level = Preset->Levels[Index];
            GEngine->Exec(&World, *FString::Printf(TEXT("%s %d"), PostFeatures[Index].ConsoleVariable, Level));
            State.Features[Index] = Level > 0;
        }
        State.bConfigured = true;
        State.bPostProcessing = Preset->bPostProcessing;
        State.Message = Preset->Message;
        return State;
    }

    struct FFeatureCost
    {
        PostFeature::Type Feature;
        float Cost;
    };

    // Rough cost estimate: a base cost, one term per enabled feature and up to 0.5 for the volume count.
    // The terms are added in exactly this order.
    float PostProcessImpact(const FPostProcessState& State, const int32 VolumeCount)
    {
        float Impact = 0.0f;
        if (!State.bPostProcessing)
        {
            return Impact;
        }
        static const FFeatureCost Costs[] =
        {
            { PostFeature::Bloom, 0.1f },
            { PostFeature::MotionBlur, 0.2f },
            { PostFeature::DepthOfField, 0.2f },
            { PostFeature::AmbientOcclusion, 0.3f },
            { PostFeature::GlobalIllumination, 0.4f },
            { PostFeature::AntiAliasing, 0.1f },
            { PostFeature::ColorGrading, 0.1f },
            { PostFeature::LensFlare, 0.1f },
            { PostFeature::ScreenSpaceReflections, 0.3f },
        };
        Impact += 0.2f;
        for (const FFeatureCost& Term : Costs)
        {
            if (State.Features[Term.Feature])
            {
                Impact += Term.Cost;
            }
        }
        Impact += FMath::Clamp(VolumeCount / 10.0f, 0.0f, 0.5f);
        return Impact;
    }

    TSharedPtr<FJsonObject> DescribeVolume(const APostProcessVolume& Volume)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("volume_name"), Volume.GetName());
        Entry->SetStringField(TEXT("volume_class"), Volume.GetClass()->GetName());
        Entry->SetBoolField(TEXT("volume_enabled"), Volume.bEnabled != 0);
        Entry->SetBoolField(TEXT("volume_unbound"), Volume.bUnbound != 0);
        SetNumbers(*Entry, {
            { TEXT("volume_priority"), Volume.Priority },
            { TEXT("volume_blend_weight"), Volume.BlendWeight },
            { TEXT("volume_blend_radius"), Volume.BlendRadius } });
        Entry->SetObjectField(TEXT("volume_position"), mcp_common::VectorToJsonObject(Volume.GetActorLocation()));
        return Entry;
    }

    TSharedPtr<FJsonObject> PostProcessReply(const FString& Requested, const FPostProcessState& State, const UWorld& World, const TArray<APostProcessVolume*>& Volumes)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("post_process_settings"), Requested);
        Reply->SetBoolField(TEXT("post_processing_configured"), State.bConfigured);
        Reply->SetStringField(TEXT("post_processing_message"), State.Message);
        Reply->SetBoolField(TEXT("post_processing_enabled"), State.bPostProcessing);
        for (int32 Index = 0; Index < PostFeature::Count; ++Index)
        {
            Reply->SetBoolField(PostFeatures[Index].ReplyKey, State.Features[Index]);
        }

        Reply->SetStringField(TEXT("world_name"), World.GetName());
        const int32 VolumeCount = Volumes.Num();
        Reply->SetNumberField(TEXT("post_process_volume_count"), VolumeCount);
        TArray<TSharedPtr<FJsonValue>> VolumeEntries;
        for (const APostProcessVolume* Volume : Volumes)
        {
            VolumeEntries.Add(MakeShared<FJsonValueObject>(DescribeVolume(*Volume)));
        }
        Reply->SetArrayField(TEXT("post_process_volume_details"), VolumeEntries);

        const bool* On = State.Features;
        TArray<TSharedPtr<FJsonValue>> Advice;
        AddAdviceIf(Advice, VolumeCount > 10, TEXT("High post-process volume count - consider reducing for performance"));
        AddAdviceIf(Advice, State.bPostProcessing && VolumeCount == 0, TEXT("Post-processing enabled but no volumes found - consider adding volumes"));
        AddAdviceIf(Advice, On[PostFeature::GlobalIllumination] && VolumeCount > 5, TEXT("Global illumination enabled with many volumes - high performance impact"));
        AddAdviceIf(Advice, On[PostFeature::MotionBlur] && On[PostFeature::DepthOfField], TEXT("Motion blur and depth of field both enabled - consider reducing for performance"));
        AddAdviceIf(Advice, !State.bPostProcessing && VolumeCount > 0, TEXT("Post-processing disabled but volumes exist - consider removing unused volumes"));
        Reply->SetArrayField(TEXT("post_processing_recommendations"), Advice);

        const float Impact = PostProcessImpact(State, VolumeCount);
        Reply->SetNumberField(TEXT("performance_impact"), Impact);
        Reply->SetStringField(TEXT("performance_rating"),
            BandFor(Impact, { { 0.8f, TEXT("High") }, { 0.5f, TEXT("Medium") }, { 0.3f, TEXT("Low") } }, TEXT("Minimal")));
        return Reply;
    }

    // =============================================================================================
    // Directional light cascades
    // =============================================================================================

    // The seven Cascaded Shadow Map fields both cascade commands report (the setter as its read-back).
    void WriteCascadeFields(const UDirectionalLightComponent& Directional, FJsonObject& Out)
    {
        Out.SetNumberField(TEXT("dynamic_shadow_distance_movable"), Directional.DynamicShadowDistanceMovableLight);
        Out.SetNumberField(TEXT("dynamic_shadow_distance_stationary"), Directional.DynamicShadowDistanceStationaryLight);
        Out.SetNumberField(TEXT("num_dynamic_shadow_cascades"), Directional.DynamicShadowCascades);
        Out.SetNumberField(TEXT("cascade_distribution_exponent"), Directional.CascadeDistributionExponent);
        Out.SetNumberField(TEXT("cascade_transition_fraction"), Directional.CascadeTransitionFraction);
        Out.SetNumberField(TEXT("shadow_distance_fadeout_fraction"), Directional.ShadowDistanceFadeoutFraction);
        Out.SetBoolField(TEXT("use_inset_shadows_for_movable_objects"), Directional.bUseInsetShadowsForMovableObjects != 0);
    }

    // light_name + light_subtype header of the cascade replies, followed by the cascade fields.
    TSharedPtr<FJsonObject> MakeCascadeReply(const FString& LightName, const UDirectionalLightComponent& Directional)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("light_name"), LightName);
        Reply->SetStringField(TEXT("light_subtype"), TEXT("DirectionalLight"));
        WriteCascadeFields(Directional, *Reply);
        return Reply;
    }
}

// Resolve an optional asset path param to a UObject of ExpectedClass.
// Absent or empty => bOutHadValue=false, nullptr (caller leaves the field unchanged).
// Present but unresolvable/wrong-class => bOutHadValue=true, nullptr (caller emits an honest note).
UObject* LoadOptionalAssetParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, UClass* ExpectedClass, bool& bOutHadValue)
{
    bOutHadValue = false;
    FString AssetPath;
    if (!Params->TryGetStringField(FieldName, AssetPath) || AssetPath.IsEmpty())
    {
        return nullptr;
    }
    bOutHadValue = true;
    UObject* Loaded = StaticLoadObject(ExpectedClass, nullptr, *AssetPath);
    UE_LOG(LogTemp, Verbose, TEXT("set_light_properties: %s='%s' -> %s"), *FieldName, *AssetPath, Loaded ? TEXT("loaded") : TEXT("NOT FOUND"));
    return Loaded;
}

// Lights and shadows

// MCP:get_light_properties - Retrieve comprehensive properties of a light actor including intensity, color, position, rotation, scale, and type-specific settings. Reads back every field set_light_properties writes (including IES, light function, shadow bias, contact shadow, cast/volumetric shadow, inverse-squared falloff, and RectLight source size / barn doors / source texture).
//   @param light_name - Name of the light actor to query
//   @returns { light_name, light_found, light_message, light_class, light_type, light_intensity, light_intensity_units, r, g, b, a, x, y, z, pitch, yaw, roll, light_enabled, light_cast_shadows, light_affect_world, light_use_inverse_squared_falloff, light_falloff_exponent, light_min_roughness, light_lighting_channel_mask, light_temperature, light_use_temperature, light_indirect_lighting_intensity, light_volumetric_scattering_intensity, light_affect_translucent_lighting, light_use_ies_brightness, light_ies_brightness_scale, light_ies_texture, light_function_scale, light_function_fade_distance, light_function_disabled_brightness, light_function_material, light_shadow_bias, light_cast_volumetric_shadow, light_contact_shadow_length, light_contact_shadow_length_in_ws, light_subtype, light_atmosphere_sun_light, light_atmosphere_sun_light_index, light_atmosphere_sun_disk_color_scale: { r, g, b, a }, light_attenuation_radius, light_source_radius, light_soft_source_radius, light_source_length, light_source_width, light_source_height, light_barn_door_angle, light_barn_door_length, light_source_texture, light_inner_cone_angle, light_outer_cone_angle, light_mobility, light_tags, light_level, light_world }
TSharedPtr<FJsonObject> HandleGetLightProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString LightName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_name"), LightName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const UEAgentLightingImpl::FLightLookup Found = UEAgentLightingImpl::LookUpLight(LightName);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }
    return UEAgentLightingImpl::DescribeLight(LightName, *Found.Actor, *Found.Light);
}

// MCP:set_light_properties - Set component-level light properties: attenuation radius, source radius/length, cone angles, color temperature, indirect/volumetric intensity, translucent lighting, plus RectLight source size + barn doors + source texture, IES profile (texture/brightness scale/use-brightness), light-function (material/scale/fade distance/disabled brightness), shadow bias, cast/volumetric shadow, contact-shadow length (+ world-space flag), and inverse-squared falloff. Round-trips with get_light_properties. Partial update (idempotent): only fields present are written; a field that doesn't apply to the light's type returns an honest note instead of an error. Verify by read-back via get_light_properties (light_* keys).
//   @param light_name - Name of the light actor to modify.
//   @param attenuation_radius - float, light reach — local lights only (Point/Spot/Rect).
//   @param source_radius - float, soft source radius (Point/Spot).
//   @param soft_source_radius - float (Point/Spot).
//   @param source_length - float, line-source length (Point/Spot).
//   @param inner_cone_angle - float degrees (Spot only).
//   @param outer_cone_angle - float degrees (Spot only).
//   @param temperature - float Kelvin color temperature (all lights; pair with use_temperature).
//   @param use_temperature - bool, enable color temperature (all lights).
//   @param indirect_lighting_intensity - float (all lights).
//   @param volumetric_scattering_intensity - float (all lights).
//   @param affect_translucent_lighting - bool (all lights).
//   @param ies_texture [string] - asset path to a UTextureLightProfile (IES profile); omit/empty leaves it unchanged (all lights).
//   @param use_ies_brightness - bool, use the IES profile's brightness (all lights).
//   @param ies_brightness_scale - float, IES brightness multiplier (all lights).
//   @param light_function_material [string] - asset path to a UMaterialInterface light-function material; omit/empty leaves it unchanged (all lights).
//   @param light_function_scale - [x,y,z] float array, light-function projection scale (all lights).
//   @param light_function_fade_distance - float, distance at which the light function fades out (all lights).
//   @param light_function_disabled_brightness - float, brightness where the light function is faded out (all lights).
//   @param shadow_bias - float, dynamic-shadow depth bias (all lights).
//   @param cast_shadows - bool, whether the light casts shadows (all lights).
//   @param cast_volumetric_shadow - bool, whether the light casts volumetric (beam) shadows (all lights).
//   @param contact_shadow_length - float, screen-space contact-shadow ray length (all lights).
//   @param contact_shadow_length_in_ws - bool, interpret contact_shadow_length in world space instead of screen space (all lights).
//   @param use_inverse_squared_falloff - bool, physically-based inverse-square falloff (Point/Spot).
//   @param falloff_exponent - float, manual falloff exponent when inverse-squared is off (Point/Spot).
//   @param source_width - float, RectLight emissive width (Rect only).
//   @param source_height - float, RectLight emissive height (Rect only).
//   @param barn_door_angle - float degrees, RectLight barn-door opening angle (Rect only).
//   @param barn_door_length - float, RectLight barn-door length (Rect only).
//   @param source_texture [string] - asset path to a UTexture for the RectLight surface; omit/empty leaves it unchanged (Rect only).
//   @returns { light_name, light_subtype, settings_applied: {...}, notes: [string], applied_count }
TSharedPtr<FJsonObject> HandleSetLightProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString LightName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_name"), LightName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const UEAgentLightingImpl::FLightLookup Target = UEAgentLightingImpl::LookUpLight(LightName);
    if (Target.Error.IsValid())
    {
        return Target.Error;
    }
    ULightComponent* LightComponent = Target.Light;

    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Notes;
    double Num; bool B;

    // Authoring writes: set the fields directly (as the editor details panel does) so they
    // apply regardless of the light's Mobility. The runtime SetX() helpers gate by mobility —
    // SetAttenuationRadius() is Movable-only — which is a runtime-safety concern, not an
    // authoring one. One MarkRenderStateDirty() at the end re-registers the component so the
    // change is visible and read-back-verifiable.

    // Base ULightComponent / ULightComponentBase fields (every light type).
    if (Params->TryGetNumberField(TEXT("temperature"), Num)) { LightComponent->Temperature = static_cast<float>(Num); Applied->SetNumberField(TEXT("temperature"), LightComponent->Temperature); }
    if (Params->TryGetBoolField(TEXT("use_temperature"), B)) { LightComponent->bUseTemperature = B ? 1 : 0; Applied->SetBoolField(TEXT("use_temperature"), LightComponent->bUseTemperature != 0); }
    if (Params->TryGetNumberField(TEXT("indirect_lighting_intensity"), Num)) { LightComponent->IndirectLightingIntensity = static_cast<float>(Num); Applied->SetNumberField(TEXT("indirect_lighting_intensity"), LightComponent->IndirectLightingIntensity); }
    if (Params->TryGetNumberField(TEXT("volumetric_scattering_intensity"), Num)) { LightComponent->VolumetricScatteringIntensity = static_cast<float>(Num); Applied->SetNumberField(TEXT("volumetric_scattering_intensity"), LightComponent->VolumetricScatteringIntensity); }
    if (Params->TryGetBoolField(TEXT("affect_translucent_lighting"), B)) { LightComponent->bAffectTranslucentLighting = B ? 1 : 0; Applied->SetBoolField(TEXT("affect_translucent_lighting"), LightComponent->bAffectTranslucentLighting != 0); }

    // Base ULightComponent / ULightComponentBase fields (every light type).
    // These setters guard internally (if (... && X != NewValue)), so re-applying is a true no-op.

    // IES profile. SetIESTexture only changes the texture when an asset path is supplied; absent => unchanged.
    {
        bool bHad = false;
        UObject* IES = LoadOptionalAssetParam(Params, TEXT("ies_texture"), UTextureLightProfile::StaticClass(), bHad);
        if (bHad)
        {
            if (IES) { LightComponent->SetIESTexture(Cast<UTextureLightProfile>(IES)); Applied->SetStringField(TEXT("ies_texture"), UEAgentLightingImpl::AssetPathOrEmpty(LightComponent->IESTexture.Get())); }
            else { Notes.Add(MakeShared<FJsonValueString>(TEXT("ies_texture: asset could not be loaded as UTextureLightProfile (left unchanged)"))); }
        }
    }
    if (Params->TryGetBoolField(TEXT("use_ies_brightness"), B)) { LightComponent->SetUseIESBrightness(B); Applied->SetBoolField(TEXT("use_ies_brightness"), LightComponent->bUseIESBrightness != 0); }
    if (Params->TryGetNumberField(TEXT("ies_brightness_scale"), Num)) { LightComponent->SetIESBrightnessScale(static_cast<float>(Num)); Applied->SetNumberField(TEXT("ies_brightness_scale"), LightComponent->IESBrightnessScale); }

    // Light function.
    {
        bool bHad = false;
        UObject* Mat = LoadOptionalAssetParam(Params, TEXT("light_function_material"), UMaterialInterface::StaticClass(), bHad);
        if (bHad)
        {
            if (Mat) { LightComponent->SetLightFunctionMaterial(Cast<UMaterialInterface>(Mat)); Applied->SetStringField(TEXT("light_function_material"), UEAgentLightingImpl::AssetPathOrEmpty(LightComponent->LightFunctionMaterial.Get())); }
            else { Notes.Add(MakeShared<FJsonValueString>(TEXT("light_function_material: asset could not be loaded as UMaterialInterface (left unchanged)"))); }
        }
    }
    {
        const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
        if (Params->TryGetArrayField(TEXT("light_function_scale"), ScaleArr) && ScaleArr && ScaleArr->Num() == 3)
        {
            FVector NewScale((*ScaleArr)[0]->AsNumber(), (*ScaleArr)[1]->AsNumber(), (*ScaleArr)[2]->AsNumber());
            LightComponent->SetLightFunctionScale(NewScale);
            Applied->SetObjectField(TEXT("light_function_scale"), mcp_common::VectorToJsonObject(LightComponent->LightFunctionScale));
        }
    }
    if (Params->TryGetNumberField(TEXT("light_function_fade_distance"), Num)) { LightComponent->SetLightFunctionFadeDistance(static_cast<float>(Num)); Applied->SetNumberField(TEXT("light_function_fade_distance"), LightComponent->LightFunctionFadeDistance); }
    if (Params->TryGetNumberField(TEXT("light_function_disabled_brightness"), Num)) { LightComponent->SetLightFunctionDisabledBrightness(static_cast<float>(Num)); Applied->SetNumberField(TEXT("light_function_disabled_brightness"), LightComponent->DisabledBrightness); }

    // Shadow.
    if (Params->TryGetNumberField(TEXT("shadow_bias"), Num)) { LightComponent->SetShadowBias(static_cast<float>(Num)); Applied->SetNumberField(TEXT("shadow_bias"), LightComponent->ShadowBias); }
    if (Params->TryGetBoolField(TEXT("cast_shadows"), B)) { LightComponent->SetCastShadows(B); Applied->SetBoolField(TEXT("cast_shadows"), LightComponent->CastShadows != 0); }
    if (Params->TryGetBoolField(TEXT("cast_volumetric_shadow"), B)) { LightComponent->SetCastVolumetricShadow(B); Applied->SetBoolField(TEXT("cast_volumetric_shadow"), LightComponent->bCastVolumetricShadow != 0); }

    // Contact shadow — NO setter on these two members; guard idempotency by hand (read current,
    // skip write + MarkRenderStateDirty if unchanged).
    if (Params->TryGetNumberField(TEXT("contact_shadow_length"), Num))
    {
        const float NewLen = static_cast<float>(Num);
        if (LightComponent->ContactShadowLength != NewLen)
        {
            LightComponent->ContactShadowLength = NewLen;
            LightComponent->MarkRenderStateDirty();
        }
        Applied->SetNumberField(TEXT("contact_shadow_length"), LightComponent->ContactShadowLength);
    }
    if (Params->TryGetBoolField(TEXT("contact_shadow_length_in_ws"), B))
    {
        const uint32 NewVal = B ? 1u : 0u;
        if (LightComponent->ContactShadowLengthInWS != NewVal)
        {
            LightComponent->ContactShadowLengthInWS = NewVal;
            LightComponent->MarkRenderStateDirty();
        }
        Applied->SetBoolField(TEXT("contact_shadow_length_in_ws"), LightComponent->GetContactShadowLengthInWS());
    }

    ULocalLightComponent* Local = Cast<ULocalLightComponent>(LightComponent);
    UPointLightComponent* Point = Cast<UPointLightComponent>(LightComponent);
    USpotLightComponent* Spot = Cast<USpotLightComponent>(LightComponent);
    URectLightComponent* Rect = Cast<URectLightComponent>(LightComponent);

    // ULocalLightComponent (Point/Spot/Rect).
    if (Params->HasField(TEXT("attenuation_radius")))
    {
        if (Local) { Params->TryGetNumberField(TEXT("attenuation_radius"), Num); Local->AttenuationRadius = static_cast<float>(Num); Applied->SetNumberField(TEXT("attenuation_radius"), Local->AttenuationRadius); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("attenuation_radius: only applies to local lights (Point/Spot/Rect)"))); }
    }
    // UPointLightComponent (Point/Spot).
    if (Params->HasField(TEXT("source_radius")))
    {
        if (Point) { Params->TryGetNumberField(TEXT("source_radius"), Num); Point->SourceRadius = static_cast<float>(Num); Applied->SetNumberField(TEXT("source_radius"), Point->SourceRadius); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("source_radius: only applies to Point/Spot lights"))); }
    }
    if (Params->HasField(TEXT("soft_source_radius")))
    {
        if (Point) { Params->TryGetNumberField(TEXT("soft_source_radius"), Num); Point->SoftSourceRadius = static_cast<float>(Num); Applied->SetNumberField(TEXT("soft_source_radius"), Point->SoftSourceRadius); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("soft_source_radius: only applies to Point/Spot lights"))); }
    }
    if (Params->HasField(TEXT("source_length")))
    {
        if (Point) { Params->TryGetNumberField(TEXT("source_length"), Num); Point->SourceLength = static_cast<float>(Num); Applied->SetNumberField(TEXT("source_length"), Point->SourceLength); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("source_length: only applies to Point/Spot lights"))); }
    }
    // USpotLightComponent (Spot only).
    if (Params->HasField(TEXT("inner_cone_angle")))
    {
        if (Spot) { Params->TryGetNumberField(TEXT("inner_cone_angle"), Num); Spot->InnerConeAngle = static_cast<float>(Num); Applied->SetNumberField(TEXT("inner_cone_angle"), Spot->InnerConeAngle); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("inner_cone_angle: only applies to Spot lights"))); }
    }
    if (Params->HasField(TEXT("outer_cone_angle")))
    {
        if (Spot) { Params->TryGetNumberField(TEXT("outer_cone_angle"), Num); Spot->OuterConeAngle = static_cast<float>(Num); Applied->SetNumberField(TEXT("outer_cone_angle"), Spot->OuterConeAngle); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("outer_cone_angle: only applies to Spot lights"))); }
    }

    // UPointLightComponent (Point/Spot) inverse-squared falloff. These setters guard internally.
    if (Params->HasField(TEXT("use_inverse_squared_falloff")))
    {
        if (Point) { Params->TryGetBoolField(TEXT("use_inverse_squared_falloff"), B); Point->SetUseInverseSquaredFalloff(B); Applied->SetBoolField(TEXT("use_inverse_squared_falloff"), Point->bUseInverseSquaredFalloff != 0); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("use_inverse_squared_falloff: only applies to Point/Spot lights"))); }
    }
    if (Params->HasField(TEXT("falloff_exponent")))
    {
        if (Point) { Params->TryGetNumberField(TEXT("falloff_exponent"), Num); Point->SetLightFalloffExponent(static_cast<float>(Num)); Applied->SetNumberField(TEXT("falloff_exponent"), Point->LightFalloffExponent); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("falloff_exponent: only applies to Point/Spot lights"))); }
    }

    // URectLightComponent (Rect only). All five guard internally.
    if (Params->HasField(TEXT("source_width")))
    {
        if (Rect) { Params->TryGetNumberField(TEXT("source_width"), Num); Rect->SetSourceWidth(static_cast<float>(Num)); Applied->SetNumberField(TEXT("source_width"), Rect->SourceWidth); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("source_width: only applies to RectLight"))); }
    }
    if (Params->HasField(TEXT("source_height")))
    {
        if (Rect) { Params->TryGetNumberField(TEXT("source_height"), Num); Rect->SetSourceHeight(static_cast<float>(Num)); Applied->SetNumberField(TEXT("source_height"), Rect->SourceHeight); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("source_height: only applies to RectLight"))); }
    }
    if (Params->HasField(TEXT("barn_door_angle")))
    {
        if (Rect) { Params->TryGetNumberField(TEXT("barn_door_angle"), Num); Rect->SetBarnDoorAngle(static_cast<float>(Num)); Applied->SetNumberField(TEXT("barn_door_angle"), Rect->BarnDoorAngle); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("barn_door_angle: only applies to RectLight"))); }
    }
    if (Params->HasField(TEXT("barn_door_length")))
    {
        if (Rect) { Params->TryGetNumberField(TEXT("barn_door_length"), Num); Rect->SetBarnDoorLength(static_cast<float>(Num)); Applied->SetNumberField(TEXT("barn_door_length"), Rect->BarnDoorLength); }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("barn_door_length: only applies to RectLight"))); }
    }
    if (Params->HasField(TEXT("source_texture")))
    {
        if (Rect)
        {
            bool bHad = false;
            UObject* Tex = LoadOptionalAssetParam(Params, TEXT("source_texture"), UTexture::StaticClass(), bHad);
            if (Tex) { Rect->SetSourceTexture(Cast<UTexture>(Tex)); Applied->SetStringField(TEXT("source_texture"), UEAgentLightingImpl::AssetPathOrEmpty(Rect->SourceTexture.Get())); }
            else { Notes.Add(MakeShared<FJsonValueString>(TEXT("source_texture: asset could not be loaded as UTexture (left unchanged)"))); }
        }
        else { Notes.Add(MakeShared<FJsonValueString>(TEXT("source_texture: only applies to RectLight"))); }
    }

    LightComponent->MarkRenderStateDirty();

    FString Subtype = TEXT("Light");
    if (Cast<UDirectionalLightComponent>(LightComponent)) { Subtype = TEXT("DirectionalLight"); }
    else if (Spot)  { Subtype = TEXT("SpotLight"); }
    else if (Point) { Subtype = TEXT("PointLight"); }
    else if (Rect)  { Subtype = TEXT("RectLight"); }
    else if (Local) { Subtype = TEXT("LocalLight"); }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("light_name"), LightName);
    ResultObj->SetStringField(TEXT("light_subtype"), Subtype);
    ResultObj->SetObjectField(TEXT("settings_applied"), Applied);
    ResultObj->SetArrayField(TEXT("notes"), Notes);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied->Values.Num());
    return ResultObj;
}

// MCP:set_light_intensity - Set the intensity value for a specified light actor and verify the change.
//   @param light_name - Name of the light actor to modify
//   @param intensity - New intensity value (float)
//   @returns { light_name, old_intensity, new_intensity, requested_intensity, intensity_set, intensity_message, light_enabled, light_cast_shadows, light_affect_world, light_mobility, light_type, light_atmosphere_sun_light, light_attenuation_radius, light_inner_cone_angle, light_outer_cone_angle, x, y, z, r, g, b, a }
TSharedPtr<FJsonObject> HandleSetLightIntensity(const TSharedPtr<FJsonObject>& Params)
{
    FString LightName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_name"), LightName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Read as float: the request is applied, echoed and compared at float precision.
    float Requested = 0.0f;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("intensity"), Requested)) { return Missing; }

    const UEAgentLightingImpl::FLightLookup Found = UEAgentLightingImpl::LookUpLight(LightName);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }
    ULightComponent& Light = *Found.Light;

    const float Before = Light.Intensity;
    Light.SetIntensity(Requested);
    const float After = Light.Intensity;
    const bool bApplied = FMath::IsNearlyEqual(After, Requested, 0.001f);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("light_name"), LightName);
    UEAgentLightingImpl::SetNumbers(*Reply, {
        { TEXT("old_intensity"), Before },
        { TEXT("new_intensity"), After },
        { TEXT("requested_intensity"), Requested } });
    Reply->SetBoolField(TEXT("intensity_set"), bApplied);
    Reply->SetStringField(TEXT("intensity_message"), bApplied ? TEXT("Light intensity set successfully") : TEXT("Failed to set light intensity"));
    UEAgentLightingImpl::WriteLightStatus(Light, *Reply, true, false);
    UEAgentLightingImpl::WriteSetterTypeBlock(Light, *Reply, false);
    Reply->SetObjectField(TEXT("light_position"), mcp_common::VectorToJsonObject(Found.Actor->GetActorLocation()));
    Reply->SetObjectField(TEXT("light_color"), mcp_common::LinearColorToJsonObject(FLinearColor(Light.LightColor)));
    return Reply;
}

// MCP:set_light_color - Set the color of a light actor using either RGB/RGBA format (comma-separated floats) or hexadecimal format (#RRGGBB).
//   @param light_name - Name of the light actor to modify
//   @param color - Color in 'R,G,B,A' or 'R,G,B' format (values 0-1) or hex '#RRGGBB' format
//   @returns { light_name, requested_color, color_set, color_message, r, g, b, a, light_enabled, light_cast_shadows, light_affect_world, light_intensity, light_mobility, light_type, light_atmosphere_sun_light, light_attenuation_radius, light_inner_cone_angle, light_outer_cone_angle, x, y, z }
TSharedPtr<FJsonObject> HandleSetLightColor(const TSharedPtr<FJsonObject>& Params)
{
    FString LightName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_name"), LightName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ColorText;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("color"), ColorText, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const UEAgentLightingImpl::FLightLookup Found = UEAgentLightingImpl::LookUpLight(LightName);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }

    // The color text is only validated once the light is known, so an unknown light is reported first.
    FLinearColor Wanted(ForceInit);
    if (!UEAgentLightingImpl::ParseColorText(ColorText, Wanted))
    {
        return mcp_common::MakeErrorReply(TEXT("Invalid color format. Use 'R,G,B,A' or '#RRGGBB'"));
    }

    // LightColor is stored as an 8-bit sRGB FColor, so the request is quantized the way SetLightColor
    // quantizes it and the two FColors are compared exactly. Comparing the linear readings at a 0.001
    // tolerance (finer than the quantization step) would report most colors as not set.
    ULightComponent& Light = *Found.Light;
    const FLinearColor Before(Light.LightColor);
    Light.SetLightColor(Wanted);
    const FLinearColor After(Light.LightColor);
    const bool bApplied = Light.LightColor == Wanted.ToFColor(/*bSRGB=*/true);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("light_name"), LightName);
    Reply->SetStringField(TEXT("requested_color"), ColorText);
    Reply->SetBoolField(TEXT("color_set"), bApplied);
    Reply->SetStringField(TEXT("color_message"), bApplied ? TEXT("Light color set successfully") : TEXT("Failed to set light color"));
    Reply->SetObjectField(TEXT("old_color"), mcp_common::LinearColorToJsonObject(Before));
    Reply->SetObjectField(TEXT("new_color"), mcp_common::LinearColorToJsonObject(After));
    UEAgentLightingImpl::WriteLightStatus(Light, *Reply, true, true);
    UEAgentLightingImpl::WriteSetterTypeBlock(Light, *Reply, false);
    Reply->SetObjectField(TEXT("light_position"), mcp_common::VectorToJsonObject(Found.Actor->GetActorLocation()));
    return Reply;
}

// MCP:configure_light_shadows - Configure shadow settings for a light actor using preset shadow qualities. Enabled/Disabled switch cast shadows; High, Medium and Low also set ShadowBias, ShadowSlopeBias and ShadowSharpen (0.1/0.1/1.0, 0.2/0.2/0.5, 0.5/0.5/0.0). Everything is read back from the light; it does not touch whether the light affects the world.
//   @param light_name - Name of the light actor to configure
//   @param shadow_settings - Shadow configuration: 'Enabled', 'Disabled', 'High', 'Medium', or 'Low' (case-insensitive); any other name is an error
//   @returns { light_name, shadow_settings, shadows_configured, shadow_message, old_cast_shadows, new_cast_shadows, old_affect_world, new_affect_world, shadows_enabled, affect_world, bias_applied, shadow_bias, shadow_slope_bias, shadow_filter_sharpen, light_enabled, light_intensity, light_mobility, light_type, light_atmosphere_sun_light, light_cascade_distribution_exponent, light_cascade_transition_fraction, light_attenuation_radius, light_source_radius, light_source_length, light_inner_cone_angle, light_outer_cone_angle, x, y, z, r, g, b, a }
TSharedPtr<FJsonObject> HandleConfigureLightShadows(const TSharedPtr<FJsonObject>& Params)
{
    FString LightName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_name"), LightName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ShadowSettings;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("shadow_settings"), ShadowSettings, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const UEAgentLightingImpl::FLightLookup Found = UEAgentLightingImpl::LookUpLight(LightName);
    if (Found.Error.IsValid())
    {
        return Found.Error;
    }
    ULightComponent& Light = *Found.Light;

    const UEAgentLightingImpl::FShadowPreset* FoundPreset = UEAgentLightingImpl::FindShadowPreset(ShadowSettings);
    if (FoundPreset == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown shadow_settings '%s': use Enabled, Disabled, High, Medium or Low"), *ShadowSettings));
    }
    const UEAgentLightingImpl::FShadowPreset& Preset = *FoundPreset;

    const bool bCastBefore = Light.CastShadows != 0;
    const bool bAffectBefore = Light.bAffectsWorld != 0;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ConfigureLightShadows", "MCP: Configure Light Shadows"));
        Light.Modify();
        Light.SetCastShadows(Preset.bCastShadows);
        if (Preset.bSetsBias)
        {
            Light.SetShadowBias(Preset.Bias);
            Light.SetShadowSlopeBias(Preset.SlopeBias);
            Light.ShadowSharpen = Preset.FilterSharpen;  // no setter: the render state is refreshed below
            Light.MarkRenderStateDirty();
        }
    }
    const bool bBiasOk = !Preset.bSetsBias
        || (FMath::IsNearlyEqual(Light.ShadowBias, Preset.Bias)
            && FMath::IsNearlyEqual(Light.ShadowSlopeBias, Preset.SlopeBias)
            && FMath::IsNearlyEqual(Light.ShadowSharpen, Preset.FilterSharpen));
    const bool bConfigured = (Light.CastShadows != 0) == Preset.bCastShadows && bBiasOk;

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("light_name"), LightName);
    Reply->SetStringField(TEXT("shadow_settings"), ShadowSettings);
    Reply->SetBoolField(TEXT("shadows_configured"), bConfigured);
    Reply->SetStringField(TEXT("shadow_message"), bConfigured ? TEXT("Light shadows configured successfully") : TEXT("Failed to configure light shadows"));
    Reply->SetBoolField(TEXT("old_cast_shadows"), bCastBefore);
    Reply->SetBoolField(TEXT("new_cast_shadows"), Light.CastShadows != 0);
    Reply->SetBoolField(TEXT("old_affect_world"), bAffectBefore);
    Reply->SetBoolField(TEXT("new_affect_world"), Light.bAffectsWorld != 0);
    Reply->SetBoolField(TEXT("shadows_enabled"), Light.CastShadows != 0);
    Reply->SetBoolField(TEXT("affect_world"), Light.bAffectsWorld != 0);
    Reply->SetBoolField(TEXT("bias_applied"), Preset.bSetsBias);
    UEAgentLightingImpl::SetNumbers(*Reply, {
        { TEXT("shadow_bias"), Light.ShadowBias },
        { TEXT("shadow_slope_bias"), Light.ShadowSlopeBias },
        { TEXT("shadow_filter_sharpen"), Light.ShadowSharpen } });
    UEAgentLightingImpl::WriteLightStatus(Light, *Reply, false, true);
    UEAgentLightingImpl::WriteSetterTypeBlock(Light, *Reply, true);
    Reply->SetObjectField(TEXT("light_position"), mcp_common::VectorToJsonObject(Found.Actor->GetActorLocation()));
    Reply->SetObjectField(TEXT("light_color"), mcp_common::LinearColorToJsonObject(FLinearColor(Light.LightColor)));
    return Reply;
}

// MCP:get_lighting_quality - Read-only census of every light component in the editor world (the editor level, even while Play-in-Editor runs), sky lights included, an actor with several counting each: counts by type (sky_light_count among them) and mobility, shadow-casting and world-affecting counts, intensity total/average/min/max over all of them (a sky light's intensity is its scale), one light_details entry per component, plus r.LightingQuality and r.DynamicLighting. lighting_quality_score (0 to 1) and its rating are a heuristic of light count, shadow casters and type variety, not a measure of built lighting; nothing is built. For one light use get_light_properties, for lights near a point get_lighting_in_area.
//   @returns { level_name, lighting_quality_found, lighting_quality_message, lighting_quality, lighting_type, lighting_enabled, light_name, light_class, light_component, light_component_class, light_intensity, light_enabled, light_cast_shadows, light_affect_world, light_mobility, x, y, z, r, g, b, a, light_count, directional_light_count, point_light_count, spot_light_count, rect_light_count, sky_light_count, static_light_count, stationary_light_count, movable_light_count, shadow_casting_light_count, world_affecting_light_count, total_intensity, average_intensity, max_intensity, min_intensity, light_details, lighting_quality_score, lighting_quality_rating } -- light_name and light_class are the owning actor's, light_component and light_component_class the component's
TSharedPtr<FJsonObject> HandleGetLightingQuality(const TSharedPtr<FJsonObject>& Params)
{
    // The editor world itself, not the engine's first world context, which nothing guarantees to be that world.
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world: get_lighting_quality reads the editor level")))
    {
        return NoWorld;
    }
    return UEAgentLightingImpl::LightingQualityReport(*World);
}

// Rendering and scalability

// MCP:get_rendering_settings - Retrieve current rendering settings including quality levels, rendering mode, world settings, engine version, and rendering feature states.
//   @returns { rendering_settings_found, rendering_settings_message, rendering_quality, rendering_api, rendering_enabled, rendering_mode, world_name, world_type, world_lighting_enabled, world_shadows_enabled, world_fog_enabled, world_atmosphere_enabled, world_max_tick_rate, world_delta_seconds, world_time_seconds, world_real_time_seconds, world_level_count, world_persistent_level, world_actor_count, world_light_actor_count, world_static_mesh_actor_count, world_skeletal_mesh_actor_count, world_camera_actor_count, world_post_process_actor_count, engine_version, platform_name, platform_architecture, platform_os, view_distance_scale, max_shadow_resolution, max_csm_shadow_resolution, rendering_shadows_enabled, rendering_fog_enabled, rendering_atmosphere_enabled, rendering_anti_aliasing_enabled, rendering_motion_blur_enabled, rendering_depth_of_field_enabled, rendering_ambient_occlusion_enabled, rendering_global_illumination_enabled (each from its console variable), rendering_quality_level, rendering_quality_preset, rendering_quality_auto, rendering_quality_score, rendering_vsync_enabled?, rendering_screen_percentage, rendering_resolution_x?, rendering_resolution_y?, rendering_frame_rate_limit?, rendering_resolution_scale, rendering_recommendations } -- rendering_api is the RHI in use; world_*_enabled say what the world contains (a light, a shadow caster, a height fog, a sky atmosphere)
TSharedPtr<FJsonObject> HandleGetRenderingSettings(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("rendering_settings_found"), true);
    Report->SetStringField(TEXT("rendering_settings_message"), TEXT("Rendering settings retrieved successfully using official APIs"));
    Report->SetStringField(TEXT("rendering_quality"), UEAgentLightingImpl::ScreenPercentageQuality());
    Report->SetStringField(TEXT("rendering_api"), UEAgentLightingImpl::ShaderApiName());
    Report->SetStringField(TEXT("rendering_mode"), UEAgentLightingImpl::ShadingPathName(/*bShaderWording=*/false));
    Report->SetBoolField(TEXT("rendering_enabled"), true);

    // Fetched once, before the world is looked at, and reused for the display block further down.
    UGameUserSettings* UserSettings = GEngine != nullptr ? GEngine->GetGameUserSettings() : nullptr;

    UWorld* World = UEAgentLightingImpl::FirstContextWorld();
    const UEAgentLightingImpl::FActorCensus Census = UEAgentLightingImpl::CountActors(World);
    if (World != nullptr)
    {
        UEAgentLightingImpl::WriteWorldSummary(*World, Census, *Report);
    }

    UEAgentLightingImpl::WriteEngineAndPlatform(*Report);
    UEAgentLightingImpl::WriteRenderingScalability(*Report);
    UEAgentLightingImpl::WriteDisplaySettings(UserSettings, *Report);

    TArray<TSharedPtr<FJsonValue>> Advice;
    UEAgentLightingImpl::AddAdviceIf(Advice, Census.WithLight > 50, TEXT("High light count detected - consider LOD settings"));
    UEAgentLightingImpl::AddAdviceIf(Advice, Census.WithStaticMesh > 1000, TEXT("High static mesh count - consider culling optimization"));
    UEAgentLightingImpl::AddAdviceIf(Advice, Census.PostProcessVolumes > 10, TEXT("High post-process count - consider performance impact"));
    UEAgentLightingImpl::AddAdviceIf(Advice, Census.Actors > 5000, TEXT("High actor count - consider level streaming"));
    Report->SetArrayField(TEXT("rendering_recommendations"), Advice);
    return Report;
}

// MCP:configure_post_processing - Configure post-processing settings using preset levels (Enabled, Disabled, High, Medium, Low) that control bloom, motion blur, DOF, AO, GI, and other effects.
//   @param post_process_settings - Post-processing setting: 'Enabled', 'Disabled', 'High', 'Medium', or 'Low'
//   @returns { post_process_settings, post_processing_configured, post_processing_message, post_processing_enabled, bloom_enabled, motion_blur_enabled, depth_of_field_enabled, ambient_occlusion_enabled, global_illumination_enabled, anti_aliasing_enabled, color_grading_enabled, lens_flare_enabled, screen_space_reflections_enabled, world_name, post_process_volume_count, volume_name, volume_class, volume_enabled, volume_unbound, volume_priority, volume_blend_weight, volume_blend_radius, x, y, z, post_process_volume_details, post_processing_recommendations, performance_impact, performance_rating }
TSharedPtr<FJsonObject> HandleConfigurePostProcessing(const TSharedPtr<FJsonObject>& Params)
{
    FString PostProcessSettings;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("post_process_settings"), PostProcessSettings, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* World = UEAgentLightingImpl::FirstContextWorld();
    if (!World)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }

    // The volume list is taken before the preset touches any console variable.
    TArray<APostProcessVolume*> Volumes;
    for (APostProcessVolume* Volume : TActorRange<APostProcessVolume>(World))
    {
        Volumes.Add(Volume);
    }

    const UEAgentLightingImpl::FPostProcessState State = UEAgentLightingImpl::ApplyPostProcessSettings(*World, PostProcessSettings);
    return UEAgentLightingImpl::PostProcessReply(PostProcessSettings, State, *World, Volumes);
}

// MCP:get_shader_info - Retrieve comprehensive shader information including shader type, quality level, API, world material/mesh/light counts, and shader complexity analysis.
//   @returns { shader_info_found, shader_info_message, shader_type, shader_quality, shaders_enabled, shader_api, world_name, material_name, material_class, material_path, material_is_material, material_is_material_instance, material_is_dynamic_material_instance, material_scalar_parameter_count, material_vector_parameter_count, material_texture_parameter_count, material_total_parameter_count, world_material_count, world_static_mesh_count, world_skeletal_mesh_count, world_light_count, world_post_process_count, world_particle_system_count, material_details, engine_version, platform_name, platform_architecture, platform_os, shader_quality_level, shader_quality_preset, shader_quality_auto, shader_quality_score, shader_forward_shading_enabled, shader_deferred_shading_enabled, shader_shadows_enabled, shader_fog_enabled, shader_atmosphere_enabled, shader_anti_aliasing_enabled, shader_motion_blur_enabled, shader_depth_of_field_enabled, shader_ambient_occlusion_enabled, shader_global_illumination_enabled, shader_vsync_enabled, shader_frame_rate_limit, shader_screen_percentage, shader_resolution_scale (each read from its console variable; left out when the build lacks it), shader_recommendations, shader_complexity_score, shader_complexity_rating }
TSharedPtr<FJsonObject> HandleGetShaderInfo(const TSharedPtr<FJsonObject>& Params)
{
    const UEAgentLightingImpl::FMaterialQualityInfo Quality = UEAgentLightingImpl::ReadMaterialQuality();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("shader_info_found"), true);
    Report->SetStringField(TEXT("shader_info_message"), TEXT("Shader info retrieved successfully"));
    Report->SetStringField(TEXT("shader_type"), UEAgentLightingImpl::ShadingPathName(/*bShaderWording=*/true));
    Report->SetStringField(TEXT("shader_quality"), Quality.Label);
    Report->SetBoolField(TEXT("shaders_enabled"), true);
    Report->SetStringField(TEXT("shader_api"), UEAgentLightingImpl::ShaderApiName());

    UWorld* World = UEAgentLightingImpl::FirstContextWorld();
    const UEAgentLightingImpl::FActorCensus Census = UEAgentLightingImpl::CountActors(World);
    if (World != nullptr)
    {
        UEAgentLightingImpl::WriteShaderWorldBlock(*World, Census, *Report);
    }

    UEAgentLightingImpl::WriteEngineAndPlatform(*Report);
    Report->SetStringField(TEXT("shader_quality_level"), Quality.Label);
    Report->SetStringField(TEXT("shader_quality_preset"), Quality.Label);
    Report->SetBoolField(TEXT("shader_quality_auto"), false);
    Report->SetNumberField(TEXT("shader_quality_score"), Quality.Score);
    UEAgentLightingImpl::WriteShaderFeatureState(*Report);

    // Advice and complexity are driven by the actor census only (see FShaderLoad).
    UEAgentLightingImpl::FShaderLoad Load;
    Load.StaticMeshes = Census.WithStaticMesh;
    Load.SkeletalMeshes = Census.WithSkeletalMesh;
    Load.Lights = Census.WithLight;
    Load.PostProcessVolumes = Census.PostProcessVolumes;
    Report->SetArrayField(TEXT("shader_recommendations"), UEAgentLightingImpl::ShaderAdvice(Load));

    const float Complexity = UEAgentLightingImpl::ShaderComplexity(Load);
    Report->SetNumberField(TEXT("shader_complexity_score"), Complexity);
    Report->SetStringField(TEXT("shader_complexity_rating"),
        UEAgentLightingImpl::BandFor(Complexity, { { 0.8f, TEXT("High") }, { 0.6f, TEXT("Medium") }, { 0.4f, TEXT("Low") } }, TEXT("Minimal")));
    return Report;
}

// Resolve the light actor by name and return its UDirectionalLightComponent.
// Honest guards: actor-not-found and not-a-directional-light produce distinct error responses;
// on any error returns nullptr and writes the response into OutError for the caller to surface.
UDirectionalLightComponent* ResolveDirectionalLightComponent(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
{
    OutError.Reset();

    FString LightName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("light_name"), LightName, mcp_common::EAllowEmpty::Yes))
    {
        OutError = Missing;
        return nullptr;
    }

    const UEAgentLightingImpl::FLightLookup Found = UEAgentLightingImpl::LookUpLight(LightName);
    if (Found.Error.IsValid())
    {
        OutError = Found.Error;
        return nullptr;
    }

    UDirectionalLightComponent* Directional = Cast<UDirectionalLightComponent>(Found.Light);
    if (Directional == nullptr)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Light '%s' is not a directional light (cascade settings apply only to UDirectionalLightComponent)"), *LightName));
    }
    return Directional;
}

// Cascaded Shadow Map (CSM) settings on a UDirectionalLightComponent. They get dedicated commands because
// set_light_properties works across light types and writes no CSM field.

// MCP:set_directional_light_cascade_settings - Configure Cascaded Shadow Map (CSM) settings on a directional light: dynamic shadow distance (movable/stationary), cascade count, distribution exponent, transition fraction, distance-fadeout fraction, and per-object inset shadows for movable objects. Partial update (idempotent): only fields present are written; the engine setters auto-dedup, and the inset-shadows bit (no setter) is guarded by hand. Errors honestly if the actor is not a directional light. Verify by read-back (every field is echoed) or via get_directional_light_cascade_settings.
//   @param light_name [string!] - Name of the directional light actor to modify
//   @param dynamic_shadow_distance_movable [number] - How far CSM dynamic shadows cover for a movable light, measured from the camera (0 disables)
//   @param dynamic_shadow_distance_stationary [number] - How far CSM dynamic shadows cover for a stationary light, measured from the camera (0 disables)
//   @param num_dynamic_shadow_cascades [integer] - Number of cascades the view frustum is split into for the whole-scene dynamic shadow
//   @param cascade_distribution_exponent [number] - Distributes cascades closer (larger) or farther (smaller) from the camera
//   @param cascade_transition_fraction [number] - Proportion of the blended fade region between cascades (0 = hard transitions)
//   @param shadow_distance_fadeout_fraction [number] - Size of the fade-out region at the far extent of the dynamic shadow, as a fraction of the shadow distance
//   @param use_inset_shadows_for_movable_objects [bool] - Stationary lights: use per-object inset shadows for movable components even though CSM is enabled
//   @returns { light_name, light_subtype, settings_applied: {...}, applied_count, dynamic_shadow_distance_movable, dynamic_shadow_distance_stationary, num_dynamic_shadow_cascades, cascade_distribution_exponent, cascade_transition_fraction, shadow_distance_fadeout_fraction, use_inset_shadows_for_movable_objects }
TSharedPtr<FJsonObject> HandleSetDirectionalLightCascadeSettings(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UDirectionalLightComponent* Directional = ResolveDirectionalLightComponent(Params, Error);
    if (!Directional)
    {
        return Error;
    }

    FString LightName;
    Params->TryGetStringField(TEXT("light_name"), LightName);

    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    double Num; bool B;

    // The six ENGINE_API setters guard internally (if (X != NewValue) { ...; MarkRenderStateDirty(); }),
    // so re-applying the same value is a true no-op (idempotent). Partial update: only present keys.
    if (Params->TryGetNumberField(TEXT("dynamic_shadow_distance_movable"), Num))
    {
        Directional->SetDynamicShadowDistanceMovableLight(static_cast<float>(Num));
        Applied->SetNumberField(TEXT("dynamic_shadow_distance_movable"), Directional->DynamicShadowDistanceMovableLight);
    }
    if (Params->TryGetNumberField(TEXT("dynamic_shadow_distance_stationary"), Num))
    {
        Directional->SetDynamicShadowDistanceStationaryLight(static_cast<float>(Num));
        Applied->SetNumberField(TEXT("dynamic_shadow_distance_stationary"), Directional->DynamicShadowDistanceStationaryLight);
    }
    if (Params->TryGetNumberField(TEXT("num_dynamic_shadow_cascades"), Num))
    {
        Directional->SetDynamicShadowCascades(static_cast<int32>(Num));
        Applied->SetNumberField(TEXT("num_dynamic_shadow_cascades"), Directional->DynamicShadowCascades);
    }
    if (Params->TryGetNumberField(TEXT("cascade_distribution_exponent"), Num))
    {
        Directional->SetCascadeDistributionExponent(static_cast<float>(Num));
        Applied->SetNumberField(TEXT("cascade_distribution_exponent"), Directional->CascadeDistributionExponent);
    }
    if (Params->TryGetNumberField(TEXT("cascade_transition_fraction"), Num))
    {
        Directional->SetCascadeTransitionFraction(static_cast<float>(Num));
        Applied->SetNumberField(TEXT("cascade_transition_fraction"), Directional->CascadeTransitionFraction);
    }
    if (Params->TryGetNumberField(TEXT("shadow_distance_fadeout_fraction"), Num))
    {
        Directional->SetShadowDistanceFadeoutFraction(static_cast<float>(Num));
        Applied->SetNumberField(TEXT("shadow_distance_fadeout_fraction"), Directional->ShadowDistanceFadeoutFraction);
    }

    // bUseInsetShadowsForMovableObjects has NO setter — set the UPROPERTY bit directly (as the editor
    // details panel does), guarding idempotency by hand (skip the write + MarkRenderStateDirty when
    // unchanged). Same pattern as the contact-shadow branch in set_light_properties.
    if (Params->TryGetBoolField(TEXT("use_inset_shadows_for_movable_objects"), B))
    {
        const uint32 NewVal = B ? 1u : 0u;
        if (Directional->bUseInsetShadowsForMovableObjects != NewVal)
        {
            Directional->bUseInsetShadowsForMovableObjects = NewVal;
            Directional->MarkRenderStateDirty();
        }
        Applied->SetBoolField(TEXT("use_inset_shadows_for_movable_objects"), Directional->bUseInsetShadowsForMovableObjects != 0);
    }

    // Full read-back of every CSM field so the change is verifiable from this call alone.
    TSharedPtr<FJsonObject> ResultObj = UEAgentLightingImpl::MakeCascadeReply(LightName, *Directional);
    ResultObj->SetObjectField(TEXT("settings_applied"), Applied);
    ResultObj->SetNumberField(TEXT("applied_count"), Applied->Values.Num());
    return ResultObj;
}

// MCP:get_directional_light_cascade_settings - Read the seven Cascaded Shadow Map (CSM) settings of a directional light: dynamic shadow distance (movable/stationary), cascade count, distribution exponent, transition fraction, distance-fadeout fraction, and per-object inset shadows for movable objects. Round-trips set_directional_light_cascade_settings. Errors honestly if the actor is not a directional light.
//   @param light_name [string!] - Name of the directional light actor to query
//   @returns { light_name, light_subtype, dynamic_shadow_distance_movable, dynamic_shadow_distance_stationary, num_dynamic_shadow_cascades, cascade_distribution_exponent, cascade_transition_fraction, shadow_distance_fadeout_fraction, use_inset_shadows_for_movable_objects }
TSharedPtr<FJsonObject> HandleGetDirectionalLightCascadeSettings(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    UDirectionalLightComponent* Directional = ResolveDirectionalLightComponent(Params, Error);
    if (!Directional)
    {
        return Error;
    }

    // Already validated by the resolver; read again only so it can be echoed back.
    FString EchoedName;
    Params->TryGetStringField(TEXT("light_name"), EchoedName);
    return UEAgentLightingImpl::MakeCascadeReply(EchoedName, *Directional);
}

// ===========================================================================
// APostProcessVolume settings
// ===========================================================================

namespace
{
    // Resolve a volume_name param to an APostProcessVolume*; sets OutError on failure.
    APostProcessVolume* ResolvePPVolume(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
    {
        FString VolumeName;
        if (!Params->TryGetStringField(TEXT("volume_name"), VolumeName) || VolumeName.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(TEXT("volume_name is required"));
            return nullptr;
        }
        UWorld* World = nullptr;
        OutError = mcp_common::RequireEditorWorld(World);
        if (OutError.IsValid())
        {
            return nullptr;
        }
        for (TActorIterator<APostProcessVolume> It(World); It; ++It)
        {
            if (It->GetActorLabel() == VolumeName) { return *It; }
        }
        OutError = mcp_common::MakeErrorReply(
            FString::Printf(TEXT("APostProcessVolume '%s' not found in the editor world"), *VolumeName));
        return nullptr;
    }
}

// configure_post_processing applies preset buckets globally; these two read and write the FPostProcessSettings
// of one specific volume actor and return the values actually in effect.

// MCP:get_post_process_volume_settings - Read the FPostProcessSettings struct of an APostProcessVolume. Returns the fields that only this pair can reach (actor-level enabled/unbounded/priority + the FPostProcessSettings members not covered by the key-value pair get_post_process_settings/set_post_process_setting). bloom_intensity/ambient_occlusion_intensity/vignette_intensity are read there, not here (one code path per FPostProcessSettings field).
//   @param volume_name - Label of the APostProcessVolume in the editor world.
//   @returns { volume_name, enabled, unbounded, priority, white_temp, auto_exposure_method, film_grain_intensity, motion_blur_amount, color_grading_lut_path }
TSharedPtr<FJsonObject> HandleGetPostProcessVolumeSettings(
    const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    APostProcessVolume* PPV = ResolvePPVolume(Params, Error);
    if (!PPV)
        return Error;

    const FPostProcessSettings& S = PPV->Settings;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("volume_name"), PPV->GetActorLabel());
    Data->SetBoolField(TEXT("enabled"), PPV->bEnabled);
    Data->SetBoolField(TEXT("unbounded"), PPV->bUnbound);
    Data->SetNumberField(TEXT("priority"), PPV->Priority);
    // FPostProcessSettings exposes WhiteTemp (Kelvin color temperature) and a
    // Color Grading WhiteTint, but no SceneWhitePoint member — return the temperature
    // as a single scalar (the canonical "white point" of the volume's grading).
    Data->SetNumberField(TEXT("white_temp"), S.WhiteTemp);
    Data->SetNumberField(TEXT("film_grain_intensity"), S.FilmGrainIntensity);
    Data->SetNumberField(TEXT("motion_blur_amount"), S.MotionBlurAmount);

    UEnum* ExposureEnum = StaticEnum<EAutoExposureMethod>();
    if (ExposureEnum)
    {
        Data->SetStringField(TEXT("auto_exposure_method"),
            ExposureEnum->GetNameStringByValue(static_cast<int64>(S.AutoExposureMethod)));
    }

    if (S.ColorGradingLUT)
    {
        Data->SetStringField(TEXT("color_grading_lut_path"), S.ColorGradingLUT->GetPathName());
    }
    else
    {
        Data->SetStringField(TEXT("color_grading_lut_path"), FString());
    }

    return mcp_common::MakeOkReply(Data);
}

// MCP:set_post_process_volume_settings - Set enabled, priority, film_grain_intensity and motion_blur_amount of a PostProcessVolume in the editor world as one undo step; only the fields you pass change, and film grain and motion blur also get their bOverride flag so they take effect. The level is not saved. Returns applied_count and applied_fields; other settings use set_post_process_setting.
//   @param volume_name - Editor label of the PostProcessVolume in the editor world; object names are not matched.
//   @param enabled - Whether the volume is enabled (APostProcessVolume bEnabled).
//   @param priority [number] - Blend priority among overlapping volumes, a number such as 1; a numeric string is also accepted.
//   @param film_grain_intensity [number] - Film grain intensity from 0 to 1, a number; a numeric string is also accepted.
//   @param motion_blur_amount [number] - Motion blur strength from 0 to 1 (0 is off), a number; a numeric string is also accepted.
//   @returns { volume_name, applied_count, applied_fields: [string] }
TSharedPtr<FJsonObject> HandleSetPostProcessVolumeSettings(
    const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    APostProcessVolume* PPV = ResolvePPVolume(Params, Error);
    if (!PPV)
        return Error;

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPPVolumeSettings", "MCP: Set Post Process Volume Settings"));
    PPV->Modify();

    FPostProcessSettings& S = PPV->Settings;
    TArray<FString> AppliedFields;
    int32 Applied = 0;

    // Returns true iff the field was present and applied — the caller uses that to
    // also flip the corresponding FPostProcessSettings bOverride_* flag. This is
    // load-bearing: the engine's volume blend (SceneView.cpp LERP_PP) SKIPS any field
    // whose bOverride_X is false, so writing the value without the override is a silent
    // no-op at render time. bOverride_* are uint8:1 bitfields (no addressable pointer),
    // so we set them explicitly per field rather than passing a flag into the lambda.
    auto TryApplyFloat = [&](const TCHAR* FieldName, float& Target) -> bool {
        if (!Params->HasField(FieldName)) { return false; }
        double V = 0.0;
        if (Params->TryGetNumberField(FieldName, V))
        {
            Target = static_cast<float>(V);
            AppliedFields.Add(FieldName);
            ++Applied;
            return true;
        }
        return false;
    };

    if (Params->HasField(TEXT("enabled")))
    {
        bool bE = PPV->bEnabled;
        Params->TryGetBoolField(TEXT("enabled"), bE);
        PPV->bEnabled = bE;
        AppliedFields.Add(TEXT("enabled"));
        ++Applied;
    }
    // priority / enabled are APostProcessVolume members (not FPostProcessSettings) — no bOverride.
    TryApplyFloat(TEXT("priority"), PPV->Priority);
    // FPostProcessSettings fields: set the matching bOverride_* so the value actually blends.
    if (TryApplyFloat(TEXT("film_grain_intensity"), S.FilmGrainIntensity))               { S.bOverride_FilmGrainIntensity = 1; }
    if (TryApplyFloat(TEXT("motion_blur_amount"), S.MotionBlurAmount))                   { S.bOverride_MotionBlurAmount = 1; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("volume_name"), PPV->GetActorLabel());
    Data->SetNumberField(TEXT("applied_count"), Applied);
    Data->SetArrayField(TEXT("applied_fields"), mcp_common::StringsToJson(AppliedFields));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("configure_light_shadows"), &HandleConfigureLightShadows },
    { TEXT("configure_post_processing"), &HandleConfigurePostProcessing },
    { TEXT("get_directional_light_cascade_settings"), &HandleGetDirectionalLightCascadeSettings },
    { TEXT("get_light_properties"), &HandleGetLightProperties },
    { TEXT("get_lighting_quality"), &HandleGetLightingQuality },
    { TEXT("get_post_process_volume_settings"), &HandleGetPostProcessVolumeSettings },
    { TEXT("get_rendering_settings"), &HandleGetRenderingSettings },
    { TEXT("get_shader_info"), &HandleGetShaderInfo },
    { TEXT("set_directional_light_cascade_settings"), &HandleSetDirectionalLightCascadeSettings },
    { TEXT("set_light_color"), &HandleSetLightColor },
    { TEXT("set_light_intensity"), &HandleSetLightIntensity },
    { TEXT("set_light_properties"), &HandleSetLightProperties },
    { TEXT("set_post_process_volume_settings"), &HandleSetPostProcessVolumeSettings },
};
} // namespace UEAgentLightingCommands
