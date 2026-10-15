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
#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
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
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundAttenuation.h"
#include "AudioDevice.h"
#include "Audio.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectGlobals.h"
#include "Runtime/Launch/Resources/Version.h"
#include "HAL/PlatformProperties.h"
#include "ScopedTransaction.h"

namespace UEAgentAudioCommands
{

// MCP-DOMAIN: Audio

namespace UEAgentAudioImpl
{

    FString EmptyParameter(const TCHAR* Name)
    {
        return FString::Printf(TEXT("Parameter '%s' cannot be empty"), Name);
    }

    FString SoundNotFound(const FString& Path)
    {
        return FString::Printf(TEXT("Sound not found: %s. Use full path like '/Game/Audio/MySound.MySound'"), *Path);
    }

    // The first world context the engine holds. configure_audio_3d and get_audio_settings have
    // always read this one instead of asking GEditor for its editor world.
    UWorld* FirstContextWorld()
    {
        return mcp_common::GetFirstContextWorld();
    }

    // Only the FIRST audio component of every actor takes part in the search, and the name test
    // is FString equality, i.e. case-insensitive.
    UAudioComponent* FindAudioComponent(UWorld* World, const FString& ComponentName)
    {
        for (TActorIterator<AActor> Owner(World); Owner; ++Owner)
        {
            UAudioComponent* FirstAudio = Owner->FindComponentByClass<UAudioComponent>();
            if (FirstAudio != nullptr && FirstAudio->GetName() == ComponentName)
            {
                return FirstAudio;
            }
        }
        return nullptr;
    }

    // Editor-world lookup shared by the per-component commands. Returns the error response to
    // send back, or null once OutComponent has been found.
    TSharedPtr<FJsonObject> LocateInEditorWorld(const FString& ComponentName, UAudioComponent*& OutComponent)
    {
        OutComponent = nullptr;
        UWorld* EditorWorld = mcp_common::GetEditorWorld();
        if (EditorWorld == nullptr || !EditorWorld->IsValidLowLevel())
        {
            return mcp_common::MakeErrorReply(TEXT("No valid world found"));
        }
        OutComponent = FindAudioComponent(EditorWorld, ComponentName);
        if (OutComponent == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Audio component not found: %s"), *ComponentName));
        }
        return nullptr;
    }

    // A plain LoadObject first; on a miss, one retry through FSoftObjectPath, which also
    // understands the quoted export-text spelling (Class'/Path.Object').
    USoundBase* LoadSound(const FString& Path)
    {
        if (USoundBase* Loaded = LoadObject<USoundBase>(nullptr, *Path))
        {
            return Loaded;
        }
        const FString Normalized = FSoftObjectPath(Path).ToString();
        return LoadObject<USoundBase>(nullptr, *Normalized);
    }

    // The attenuation the component really applies, as the engine picks it: its own overrides,
    // else the assigned attenuation asset, else the sound's. Null when there is none (callers
    // report 0). Reading only the asset missed every component with overrides.
    const FSoundAttenuationSettings* AttenuationOf(const UAudioComponent* AudioComp)
    {
        return AudioComp->GetAttenuationSettingsToApply();
    }

    // Spatialization flag plus the attenuation extent and falloff.
    void AddAttenuationFields(const TSharedPtr<FJsonObject>& Out, const UAudioComponent* AudioComp)
    {
        const FSoundAttenuationSettings* Attenuation = AttenuationOf(AudioComp);
        Out->SetBoolField(TEXT("audio_3d_enabled"), AudioComp->bAllowSpatialization);
        Out->SetNumberField(TEXT("audio_3d_attenuation_radius"), Attenuation ? Attenuation->AttenuationShapeExtents.X : 0.0f);
        Out->SetNumberField(TEXT("audio_3d_attenuation_falloff"), Attenuation ? Attenuation->FalloffDistance : 0.0f);
    }

    // Name, class and duration of the assigned sound; "None", "None" and 0 when nothing is assigned.
    void AddSoundSummary(const TSharedPtr<FJsonObject>& Out, USoundBase* Sound)
    {
        const FString NoSound(TEXT("None"));
        Out->SetStringField(TEXT("audio_sound_name"), Sound ? Sound->GetName() : NoSound);
        Out->SetStringField(TEXT("audio_sound_class"), Sound ? Sound->GetClass()->GetName() : NoSound);
        Out->SetNumberField(TEXT("audio_sound_duration"), Sound ? Sound->GetDuration() : 0.0f);
    }

    // Whether the assigned sound is a plain wave or a cue (both false when nothing is assigned).
    void AddSoundKindFlags(const TSharedPtr<FJsonObject>& Out, USoundBase* Sound)
    {
        Out->SetBoolField(TEXT("audio_sound_is_sound_wave"), Cast<USoundWave>(Sound) != nullptr);
        Out->SetBoolField(TEXT("audio_sound_is_sound_cue"), Cast<USoundCue>(Sound) != nullptr);
    }

    // Owning actor. Without an owner the identity fields read "None" and no transform is written;
    // the detailed form adds the object path and the rotation.
    void AddOwnerFields(const TSharedPtr<FJsonObject>& Out, const AActor* Owner, const bool bDetailed)
    {
        const FString NoOwner(TEXT("None"));
        Out->SetStringField(TEXT("audio_owner_name"), Owner ? Owner->GetName() : NoOwner);
        Out->SetStringField(TEXT("audio_owner_class"), Owner ? Owner->GetClass()->GetName() : NoOwner);
        if (bDetailed)
        {
            Out->SetStringField(TEXT("audio_owner_path"), Owner ? Owner->GetPathName() : NoOwner);
        }
        if (Owner == nullptr)
        {
            return;
        }
        Out->SetObjectField(TEXT("audio_owner_position"), mcp_common::VectorToJsonObject(Owner->GetActorLocation()));
        if (bDetailed)
        {
            Out->SetObjectField(TEXT("audio_owner_rotation"), mcp_common::RotatorToJsonObject(Owner->GetActorRotation()));
        }
    }

    // Read-back context shared by set_audio_volume and configure_audio_3d.
    void AddPlaybackContext(const TSharedPtr<FJsonObject>& Out, UAudioComponent* AudioComp)
    {
        Out->SetBoolField(TEXT("audio_enabled"), AudioComp->IsActive());
        Out->SetBoolField(TEXT("audio_auto_activate"), AudioComp->bAutoActivate);
        Out->SetBoolField(TEXT("audio_auto_destroy"), AudioComp->bAutoDestroy);
        Out->SetNumberField(TEXT("audio_pitch"), AudioComp->PitchMultiplier);
        Out->SetStringField(TEXT("audio_mobility"), mcp_common::MobilityLabel(AudioComp->GetMobility()));
        AddSoundSummary(Out, AudioComp->GetSound());
        Out->SetObjectField(TEXT("audio_position"), mcp_common::VectorToJsonObject(AudioComp->GetComponentLocation()));
        Out->SetObjectField(TEXT("audio_rotation"), mcp_common::RotatorToJsonObject(AudioComp->GetComponentRotation()));
        AddOwnerFields(Out, AudioComp->GetOwner(), /*bDetailed=*/false);
    }

    // Values configure_audio_3d describes for a named preset.
    struct FSpatialPreset
    {
        bool bEnable3D = false;
        bool bSpatialize = false;
        float Distance = 0.0f;
        float Radius = 0.0f;
        float Falloff = 0.0f;
        float ConeAngle = 0.0f;
        float ConeFeather = 0.0f;
    };

    // Preset names compare with FString equality (case-insensitive). "Enabled" and "Medium" carry
    // the same numbers; an unknown name is null, so a typo is an error and never the all-off
    // preset.
    const FSpatialPreset* FindSpatialPreset(const FString& PresetName)
    {
        struct FNamedPreset
        {
            const TCHAR* Label;
            FSpatialPreset Values;
        };
        static const FNamedPreset Known[] = {
            { TEXT("Enabled"),  { true,  true,  1000.0f,  500.0f, 100.0f,  90.0f, 10.0f } },
            { TEXT("Disabled"), { false, false,    0.0f,    0.0f,   0.0f,   0.0f,  0.0f } },
            { TEXT("High"),     { true,  true,  2000.0f, 1000.0f, 200.0f,  60.0f,  5.0f } },
            { TEXT("Medium"),   { true,  true,  1000.0f,  500.0f, 100.0f,  90.0f, 10.0f } },
            { TEXT("Low"),      { true,  true,   500.0f,  250.0f,  50.0f, 120.0f, 20.0f } },
        };
        for (const FNamedPreset& Candidate : Known)
        {
            if (PresetName == Candidate.Label)
            {
                return &Candidate.Values;
            }
        }
        return nullptr;
    }

    // Advice attached to configure_audio_3d, judged on the preset's numbers.
    TArray<TSharedPtr<FJsonValue>> SpatialAdvice(const FSpatialPreset& Preset, const bool bHasSound)
    {
        TArray<TSharedPtr<FJsonValue>> Advice;
        const auto Note = [&Advice](const TCHAR* Message) { Advice.Add(MakeShared<FJsonValueString>(Message)); };
        if (!Preset.bEnable3D)
        {
            if (bHasSound)
            {
                Note(TEXT("3D disabled but sound exists - consider enabling for spatial audio"));
            }
            return Advice;
        }
        if (Preset.Distance > 1500.0f)
        {
            Note(TEXT("High attenuation distance - consider reducing for performance"));
        }
        if (Preset.Radius > 800.0f)
        {
            Note(TEXT("High attenuation radius - consider reducing for performance"));
        }
        if (Preset.Falloff > 150.0f)
        {
            Note(TEXT("High attenuation falloff - consider reducing for performance"));
        }
        if (Preset.ConeAngle < 60.0f)
        {
            Note(TEXT("Narrow cone angle - consider increasing for better coverage"));
        }
        return Advice;
    }

    // Tallies taken by get_audio_settings over the first audio component of every actor.
    struct FAudioCensus
    {
        int32 Components = 0;
        int32 Active = 0;
        int32 Playing = 0;
        // Components that have a sound assigned, and those whose sound loops (USoundBase::IsLooping).
        int32 WithSound = 0;
        int32 Looping = 0;
        int32 Spatialized = 0;
        int32 Waves = 0;
        int32 Cues = 0;
        float VolumeSum = 0.0f;
        float VolumeMax = 0.0f;
        float VolumeMin = FLT_MAX;
        float PitchSum = 0.0f;
        float PitchMax = 0.0f;
        float PitchMin = FLT_MAX;
    };

    // Walks the actors in iteration order, updating the tallies and returning one detail entry
    // per audio component found.
    TArray<TSharedPtr<FJsonValue>> TakeAudioCensus(UWorld* World, FAudioCensus& Census)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (TActorIterator<AActor> Owner(World); Owner; ++Owner)
        {
            UAudioComponent* AudioComp = Owner->FindComponentByClass<UAudioComponent>();
            if (AudioComp == nullptr)
            {
                continue;
            }
            USoundBase* Sound = AudioComp->GetSound();
            const float Volume = AudioComp->VolumeMultiplier;
            const float Pitch = AudioComp->PitchMultiplier;

            ++Census.Components;
            Census.Active += AudioComp->IsActive() ? 1 : 0;
            Census.Playing += AudioComp->IsPlaying() ? 1 : 0;
            const bool bLoops = Sound != nullptr && Sound->IsLooping();
            Census.WithSound += (Sound != nullptr) ? 1 : 0;
            Census.Looping += bLoops ? 1 : 0;
            Census.Spatialized += AudioComp->bAllowSpatialization ? 1 : 0;
            Census.Waves += (Cast<USoundWave>(Sound) != nullptr) ? 1 : 0;
            Census.Cues += (Cast<USoundCue>(Sound) != nullptr) ? 1 : 0;
            Census.VolumeSum += Volume;
            Census.VolumeMax = FMath::Max(Census.VolumeMax, Volume);
            Census.VolumeMin = FMath::Min(Census.VolumeMin, Volume);
            Census.PitchSum += Pitch;
            Census.PitchMax = FMath::Max(Census.PitchMax, Pitch);
            Census.PitchMin = FMath::Min(Census.PitchMin, Pitch);

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("audio_component_name"), AudioComp->GetName());
            Entry->SetStringField(TEXT("audio_component_class"), AudioComp->GetClass()->GetName());
            Entry->SetNumberField(TEXT("audio_volume"), Volume);
            Entry->SetNumberField(TEXT("audio_pitch"), Pitch);
            Entry->SetBoolField(TEXT("audio_enabled"), AudioComp->IsActive());
            Entry->SetBoolField(TEXT("audio_playing"), AudioComp->IsPlaying());
            Entry->SetBoolField(TEXT("audio_looping"), bLoops);
            Entry->SetBoolField(TEXT("audio_spatialized"), AudioComp->bAllowSpatialization);
            Entry->SetStringField(TEXT("audio_mobility"), mcp_common::MobilityLabel(AudioComp->GetMobility()));
            AddSoundSummary(Entry, Sound);
            AddSoundKindFlags(Entry, Sound);
            Entry->SetObjectField(TEXT("audio_position"), mcp_common::VectorToJsonObject(AudioComp->GetComponentLocation()));
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Entries;
    }

    void AddCensusTotals(const TSharedPtr<FJsonObject>& Out, const FAudioCensus& Census)
    {
        // Means stay 0 for a world without audio; the minimum trackers start at FLT_MAX and are
        // reported as 0 when nothing lowered them.
        const float VolumeMean = Census.Components > 0 ? Census.VolumeSum / Census.Components : 0.0f;
        const float PitchMean = Census.Components > 0 ? Census.PitchSum / Census.Components : 0.0f;

        Out->SetNumberField(TEXT("audio_component_count"), Census.Components);
        Out->SetNumberField(TEXT("active_audio_component_count"), Census.Active);
        Out->SetNumberField(TEXT("playing_audio_component_count"), Census.Playing);
        Out->SetNumberField(TEXT("looping_audio_component_count"), Census.Looping);
        Out->SetNumberField(TEXT("spatialized_audio_component_count"), Census.Spatialized);
        Out->SetNumberField(TEXT("sound_wave_count"), Census.Waves);
        Out->SetNumberField(TEXT("sound_cue_count"), Census.Cues);

        Out->SetNumberField(TEXT("total_volume"), Census.VolumeSum);
        Out->SetNumberField(TEXT("average_volume"), VolumeMean);
        Out->SetNumberField(TEXT("max_volume"), Census.VolumeMax);
        Out->SetNumberField(TEXT("min_volume"), Census.VolumeMin == FLT_MAX ? 0.0f : Census.VolumeMin);

        Out->SetNumberField(TEXT("total_pitch"), Census.PitchSum);
        Out->SetNumberField(TEXT("average_pitch"), PitchMean);
        Out->SetNumberField(TEXT("max_pitch"), Census.PitchMax);
        Out->SetNumberField(TEXT("min_pitch"), Census.PitchMin == FLT_MAX ? 0.0f : Census.PitchMin);
    }

    // Default audio backend of the platform this module was compiled for.
    const TCHAR* PlatformAudioApiName()
    {
        const TCHAR* Backend = TEXT("Unknown");
#if PLATFORM_WINDOWS
        Backend = TEXT("XAudio2");
#elif (PLATFORM_MAC || PLATFORM_IOS)
        Backend = TEXT("CoreAudio");
#elif (PLATFORM_ANDROID || PLATFORM_LINUX)
        Backend = TEXT("OpenAL");
#endif
        return Backend;
    }

    // Live facts read from the main audio device. Without a device the three quality fields read
    // "Unknown" and audio_enabled is false.
    void AddDeviceFacts(const TSharedPtr<FJsonObject>& Out)
    {
        static const TCHAR* const QualityKeys[] = { TEXT("audio_quality"), TEXT("audio_quality_level"), TEXT("audio_quality_preset") };
        const TCHAR* Quality = TEXT("Unknown");
        bool bDeviceUp = false;

        // Not FAudioDevice::GetMainAudioDevice(): with audio disabled (-nosound) it falls back to a
        // device manager that does not exist and dereferences it.
        FAudioDeviceHandle MainDevice = GEngine ? GEngine->GetMainAudioDevice() : FAudioDeviceHandle();
        if (!MainDevice.IsValid() && GEngine && GEngine->GetAudioDeviceManager())
        {
            MainDevice = GEngine->GetAudioDeviceManager()->GetActiveAudioDevice();
        }
        if (FAudioDevice* Device = MainDevice.IsValid() ? MainDevice.GetAudioDevice() : nullptr)
        {
            bDeviceUp = true;
            const int32 Channels = Device->GetMaxChannels();
            const bool bMixerLoaded = Device->bAudioMixerModuleLoaded;
            // Spatialization needs live channels and the mixer module. Reverb and occlusion are not
            // readable through the public device API, so their engine defaults are reported.
            const bool bSpatial = Channels > 0 && bMixerLoaded;
            Out->SetBoolField(TEXT("audio_mixer_loaded"), bMixerLoaded);
            Out->SetNumberField(TEXT("audio_max_channels"), Channels);
            Out->SetBoolField(TEXT("audio_3d_enabled"), bSpatial);
            Out->SetBoolField(TEXT("audio_spatialization_enabled"), bSpatial);
            Out->SetBoolField(TEXT("audio_reverb_enabled"), true);
            Out->SetBoolField(TEXT("audio_occlusion_enabled"), true);

            // Quality tiers by channel budget: 64 and up is High, under 32 is Low.
            Quality = TEXT("Medium");
            if (Channels >= 64)
            {
                Quality = TEXT("High");
            }
            else if (Channels < 32)
            {
                Quality = TEXT("Low");
            }
        }

        Out->SetBoolField(TEXT("audio_enabled"), bDeviceUp);
        for (const TCHAR* QualityKey : QualityKeys)
        {
            Out->SetStringField(QualityKey, Quality);
        }
    }

    // Settings the public API does not expose; the usual engine defaults are reported for them.
    void AddAssumedDefaults(const TSharedPtr<FJsonObject>& Out)
    {
        Out->SetBoolField(TEXT("audio_quality_auto"), false);
        Out->SetBoolField(TEXT("audio_doppler_enabled"), true);
        Out->SetBoolField(TEXT("audio_ambisonics_enabled"), false);
        Out->SetBoolField(TEXT("audio_compression_enabled"), true);
        Out->SetBoolField(TEXT("audio_streaming_enabled"), true);
        Out->SetNumberField(TEXT("audio_target_fps"), 60.0f);
        Out->SetNumberField(TEXT("audio_buffer_size"), 1024.0f);
        Out->SetNumberField(TEXT("audio_sample_rate"), 48000.0f);
        Out->SetNumberField(TEXT("audio_bit_depth"), 16.0f);
        Out->SetNumberField(TEXT("audio_channels"), 2.0f);
    }

    // Share of active components, clamped to [0, 1]; Fallback when there are no components.
    float ActiveShare(const FAudioCensus& Census, const float Fallback)
    {
        if (Census.Components <= 0)
        {
            return Fallback;
        }
        return FMath::Clamp(static_cast<float>(Census.Active) / static_cast<float>(Census.Components), 0.0f, 1.0f);
    }

    TArray<TSharedPtr<FJsonValue>> CensusAdvice(const FAudioCensus& Census)
    {
        struct FRule
        {
            bool bApplies;
            const TCHAR* Message;
        };
        const FRule Rules[] = {
            { Census.Components > 50, TEXT("High audio component count - consider audio optimization") },
            { Census.Spatialized > 30, TEXT("High spatialized audio count - consider performance impact") },
            { Census.Looping > 20, TEXT("High looping audio count - consider memory usage") },
            { Census.VolumeMax > 2.0f, TEXT("High volume detected - consider volume normalization") },
            { Census.PitchMax > 2.0f, TEXT("High pitch detected - consider pitch normalization") },
            { Census.Active < Census.Components * 0.5f, TEXT("Low active audio component ratio - consider activation optimization") },
        };
        TArray<TSharedPtr<FJsonValue>> Advice;
        for (const FRule& Rule : Rules)
        {
            if (Rule.bApplies)
            {
                Advice.Add(MakeShared<FJsonValueString>(Rule.Message));
            }
        }
        return Advice;
    }

    // Weighted mix of population (30%), spatialized share (30%), share with a sound (20%) and
    // active share (20%); 0 for a world without audio components.
    float ComplexityScore(const FAudioCensus& Census)
    {
        float Score = 0.0f;
        if (Census.Components > 0)
        {
            const float Population = static_cast<float>(Census.Components);
            Score += FMath::Clamp(Census.Components / 25.0f, 0.0f, 1.0f) * 0.3f;
            Score += FMath::Clamp(static_cast<float>(Census.Spatialized) / Population, 0.0f, 1.0f) * 0.3f;
            Score += FMath::Clamp(static_cast<float>(Census.WithSound) / Population, 0.0f, 1.0f) * 0.2f;
            Score += FMath::Clamp(static_cast<float>(Census.Active) / Population, 0.0f, 1.0f) * 0.2f;
        }
        return Score;
    }

    const TCHAR* ComplexityRating(const float Score)
    {
        if (Score >= 0.8f)
        {
            return TEXT("High");
        }
        if (Score >= 0.6f)
        {
            return TEXT("Medium");
        }
        return Score >= 0.4f ? TEXT("Low") : TEXT("Minimal");
    }
}

// Audio components

// MCP:get_audio_component_properties - Retrieves comprehensive properties of an audio component including volume, pitch, position, rotation, scale, and sound details.
//   @param audio_component_name - Name of the audio component to query
//   @returns { audio_component_name: string, audio_component_found: bool, audio_component_message: string, component_class: string, component_type: string, audio_volume: number, audio_pitch: number, audio_position: { x, y, z }, audio_rotation: { pitch, yaw, roll }, audio_scale: { x, y, z }, audio_enabled: bool, audio_auto_activate: bool, audio_sound_name: string, audio_sound_class: string, audio_sound_path: string, audio_sound_duration: number, audio_3d_enabled: bool, audio_3d_attenuation_radius: number, audio_mobility: string, audio_owner_name: string, audio_level: string, audio_world: string }

TSharedPtr<FJsonObject> HandleGetAudioComponentProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (Wanted.IsEmpty())
    {
        return mcp_common::MakeErrorReply(UEAgentAudioImpl::EmptyParameter(TEXT("audio_component_name")));
    }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateInEditorWorld(Wanted, AudioComp))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("audio_component_name"), Wanted);
    Report->SetBoolField(TEXT("audio_component_found"), true);
    Report->SetStringField(TEXT("audio_component_message"), TEXT("Audio component properties retrieved successfully"));
    Report->SetStringField(TEXT("component_class"), AudioComp->GetClass()->GetName());
    Report->SetStringField(TEXT("component_type"), TEXT("Audio Component"));

    // Playback state.
    Report->SetNumberField(TEXT("audio_volume"), AudioComp->VolumeMultiplier);
    Report->SetNumberField(TEXT("audio_pitch"), AudioComp->PitchMultiplier);
    Report->SetBoolField(TEXT("audio_enabled"), AudioComp->IsActive());
    Report->SetBoolField(TEXT("audio_auto_activate"), AudioComp->bAutoActivate);
    Report->SetStringField(TEXT("audio_mobility"), mcp_common::MobilityLabel(AudioComp->GetMobility()));

    // World transform of the component.
    Report->SetObjectField(TEXT("audio_position"), mcp_common::VectorToJsonObject(AudioComp->GetComponentLocation()));
    Report->SetObjectField(TEXT("audio_rotation"), mcp_common::RotatorToJsonObject(AudioComp->GetComponentRotation()));
    Report->SetObjectField(TEXT("audio_scale"), mcp_common::VectorToJsonObject(AudioComp->GetComponentScale()));

    // Assigned sound asset.
    USoundBase* Sound = AudioComp->GetSound();
    UEAgentAudioImpl::AddSoundSummary(Report, Sound);
    Report->SetStringField(TEXT("audio_sound_path"), Sound ? Sound->GetPathName() : FString(TEXT("None")));
    UEAgentAudioImpl::AddSoundKindFlags(Report, Sound);

    // Spatialization. A cone keeps its angle and falloff angle in the shape extents (Y and Z); any
    // other shape has no cone and reports 0.
    UEAgentAudioImpl::AddAttenuationFields(Report, AudioComp);
    const FSoundAttenuationSettings* Attenuation = UEAgentAudioImpl::AttenuationOf(AudioComp);
    const bool bCone = Attenuation && Attenuation->AttenuationShape == EAttenuationShape::Cone;
    Report->SetNumberField(TEXT("audio_3d_attenuation_cone_angle"), bCone ? Attenuation->AttenuationShapeExtents.Y : 0.0);
    Report->SetNumberField(TEXT("audio_3d_attenuation_cone_feather"), bCone ? Attenuation->AttenuationShapeExtents.Z : 0.0);

    // Owner and placement.
    UEAgentAudioImpl::AddOwnerFields(Report, AudioComp->GetOwner(), /*bDetailed=*/true);
    UWorld* HostWorld = AudioComp->GetWorld();
    Report->SetStringField(TEXT("audio_level"), HostWorld->GetCurrentLevel()->GetName());
    Report->SetStringField(TEXT("audio_world"), HostWorld->GetName());
    return Report;
}

// MCP:set_audio_volume - Set VolumeMultiplier (0 to 10, else an error; also resets VolumeModulationMin/Max to 1) on an audio component of the editor world, matched by object name against the first audio component of each actor. No undo, no dirty mark. Returns old_volume, new_volume and volume_set.
//   @param audio_component_name - Object name of the audio component to modify (e.g. "AudioComponent0"), compared without case; only the first audio component of each actor is searched.
//   @param volume - Volume multiplier between 0.0 and 10.0
//   @returns { audio_component_name: string, old_volume: number, new_volume: number, requested_volume: number, volume_set: bool, volume_message: string, audio_enabled: bool, audio_pitch: number, audio_mobility: string, audio_sound_name: string, audio_3d_attenuation_radius: number, audio_position: { x, y, z }, audio_owner_name: string }

TSharedPtr<FJsonObject> HandleSetAudioVolume(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double TargetVolume = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("volume"), TargetVolume)) { return Missing; }
    if (Wanted.IsEmpty())
    {
        return mcp_common::MakeErrorReply(UEAgentAudioImpl::EmptyParameter(TEXT("audio_component_name")));
    }
    if (TargetVolume < 0.0 || TargetVolume > 10.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'volume' must be between 0.0 and 10.0"));
    }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateInEditorWorld(Wanted, AudioComp))
    {
        return Failure;
    }

    const float VolumeBefore = AudioComp->VolumeMultiplier;
    AudioComp->SetVolumeMultiplier(static_cast<float>(TargetVolume));
    // Verified by reading the multiplier back and comparing it with the request.
    const float VolumeAfter = AudioComp->VolumeMultiplier;
    const bool bApplied = FMath::IsNearlyEqual(VolumeAfter, TargetVolume, 0.001f);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("audio_component_name"), Wanted);
    Report->SetNumberField(TEXT("old_volume"), VolumeBefore);
    Report->SetNumberField(TEXT("new_volume"), VolumeAfter);
    Report->SetNumberField(TEXT("requested_volume"), TargetVolume);
    Report->SetBoolField(TEXT("volume_set"), bApplied);
    Report->SetStringField(TEXT("volume_message"),
        bApplied ? TEXT("Audio volume set successfully") : TEXT("Failed to set audio volume"));
    UEAgentAudioImpl::AddPlaybackContext(Report, AudioComp);
    UEAgentAudioImpl::AddAttenuationFields(Report, AudioComp);
    return Report;
}

// MCP:configure_audio_3d - Applies a 3D audio preset to an audio component of the editor world: spatialization (bAllowSpatialization) and an attenuation override, a sphere whose inner radius is attenuation_radius and whose falloff distance is attenuation_distance ('Disabled' turns both off). audio_3d_configured is read back from the component. attenuation_falloff and the cone numbers only describe the preset (a sphere has no cone).
//   @param audio_component_name - Name of the audio component to configure
//   @param audio_3d_settings - 3D setting preset: 'Enabled', 'Disabled', 'High', 'Medium', or 'Low' (case-insensitive); any other name is an error
//   @returns { audio_component_name: string, audio_3d_settings: string, audio_3d_configured: bool, audio_3d_message: string, old_3d_enabled: bool, new_3d_enabled: bool, attenuation_override: bool, applied_attenuation_radius?: number, applied_falloff_distance?: number, 3d_enabled: bool, spatialization_enabled: bool, attenuation_distance: number, attenuation_radius: number, attenuation_falloff: number, attenuation_cone_angle: number, attenuation_cone_feather: number, audio_3d_recommendations: [string] }

TSharedPtr<FJsonObject> HandleConfigureAudio3D(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PresetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_3d_settings"), PresetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* ContextWorld = UEAgentAudioImpl::FirstContextWorld();
    if (ContextWorld == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }
    UAudioComponent* AudioComp = UEAgentAudioImpl::FindAudioComponent(ContextWorld, Wanted);
    if (AudioComp == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Audio component not found: %s"), *Wanted));
    }
    const UEAgentAudioImpl::FSpatialPreset* FoundPreset = UEAgentAudioImpl::FindSpatialPreset(PresetName);
    if (FoundPreset == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown audio_3d_settings '%s': use Enabled, Disabled, High, Medium or Low"), *PresetName));
    }
    const UEAgentAudioImpl::FSpatialPreset& Preset = *FoundPreset;

    const bool bSpatializedBefore = AudioComp->bAllowSpatialization;
    {
        const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ConfigureAudio3D", "MCP: Configure Audio 3D"));
        AudioComp->Modify();
        AudioComp->bAllowSpatialization = Preset.bSpatialize;
        AudioComp->bOverrideAttenuation = Preset.bEnable3D;
        if (Preset.bEnable3D)
        {
            FSoundAttenuationSettings Overrides = AudioComp->AttenuationOverrides;
            Overrides.bAttenuate = true;
            Overrides.bSpatialize = Preset.bSpatialize;
            Overrides.AttenuationShape = EAttenuationShape::Sphere;
            Overrides.AttenuationShapeExtents = FVector(Preset.Radius, 0.0, 0.0);
            Overrides.FalloffDistance = Preset.Distance;
            AudioComp->SetAttenuationOverrides(Overrides);
        }
    }

    // Read back from the component, not from the request.
    const FSoundAttenuationSettings& Applied = AudioComp->AttenuationOverrides;
    const bool bOverrideOk = AudioComp->bOverrideAttenuation == Preset.bEnable3D
        && (!Preset.bEnable3D
            || (FMath::IsNearlyEqual(Applied.AttenuationShapeExtents.X, Preset.Radius)
                && FMath::IsNearlyEqual(Applied.FalloffDistance, Preset.Distance)));
    const bool bConfigured = AudioComp->bAllowSpatialization == Preset.bSpatialize && bOverrideOk;

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("audio_component_name"), Wanted);
    Report->SetStringField(TEXT("audio_3d_settings"), PresetName);
    Report->SetBoolField(TEXT("audio_3d_configured"), bConfigured);
    Report->SetStringField(TEXT("audio_3d_message"),
        bConfigured ? TEXT("Audio 3D configured successfully") : TEXT("Failed to configure audio 3D"));
    Report->SetBoolField(TEXT("old_3d_enabled"), bSpatializedBefore);
    Report->SetBoolField(TEXT("new_3d_enabled"), AudioComp->bAllowSpatialization);
    Report->SetBoolField(TEXT("attenuation_override"), AudioComp->bOverrideAttenuation);
    if (AudioComp->bOverrideAttenuation)
    {
        Report->SetNumberField(TEXT("applied_attenuation_radius"), Applied.AttenuationShapeExtents.X);
        Report->SetNumberField(TEXT("applied_falloff_distance"), Applied.FalloffDistance);
    }

    // The preset's own numbers.
    Report->SetBoolField(TEXT("3d_enabled"), Preset.bEnable3D);
    Report->SetBoolField(TEXT("spatialization_enabled"), Preset.bSpatialize);
    Report->SetNumberField(TEXT("attenuation_distance"), Preset.Distance);
    Report->SetNumberField(TEXT("attenuation_radius"), Preset.Radius);
    Report->SetNumberField(TEXT("attenuation_falloff"), Preset.Falloff);
    Report->SetNumberField(TEXT("attenuation_cone_angle"), Preset.ConeAngle);
    Report->SetNumberField(TEXT("attenuation_cone_feather"), Preset.ConeFeather);

    Report->SetNumberField(TEXT("audio_volume"), AudioComp->VolumeMultiplier);
    UEAgentAudioImpl::AddPlaybackContext(Report, AudioComp);
    Report->SetArrayField(TEXT("audio_3d_recommendations"),
        UEAgentAudioImpl::SpatialAdvice(Preset, AudioComp->GetSound() != nullptr));
    return Report;
}

// MCP:get_audio_settings - Retrieves comprehensive audio settings and statistics including all audio components in the world, audio device capabilities, and quality metrics.
//   @returns { audio_settings_found: bool, audio_quality: string, audio_api: string, audio_enabled: bool, audio_mode: string, world_name?: string, audio_component_count: int, active_audio_component_count: int, spatialized_audio_component_count: int, average_volume: number, max_volume: number, max_pitch: number, engine_version: string, platform_name: string, audio_max_channels: int, audio_3d_enabled: bool, audio_reverb_enabled: bool, audio_occlusion_enabled: bool, audio_recommendations: [string], audio_complexity_score: number, audio_complexity_rating: string }

TSharedPtr<FJsonObject> HandleGetAudioSettings(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("audio_settings_found"), true);
    Report->SetStringField(TEXT("audio_settings_message"), TEXT("Audio settings retrieved successfully"));
    Report->SetStringField(TEXT("audio_api"), UEAgentAudioImpl::PlatformAudioApiName());
    Report->SetStringField(TEXT("audio_mode"), TEXT("3D Audio"));

    // Per-component statistics; the whole block is omitted when there is no world to walk.
    UEAgentAudioImpl::FAudioCensus Census;
    if (UWorld* ContextWorld = UEAgentAudioImpl::FirstContextWorld())
    {
        Report->SetStringField(TEXT("world_name"), ContextWorld->GetName());
        const TArray<TSharedPtr<FJsonValue>> PerComponent = UEAgentAudioImpl::TakeAudioCensus(ContextWorld, Census);
        UEAgentAudioImpl::AddCensusTotals(Report, Census);
        Report->SetArrayField(TEXT("audio_component_details"), PerComponent);
    }

    // Build and platform identity. The platform name stands in for architecture and OS, which
    // FPlatformProperties does not expose separately.
    const FString PlatformLabel = FPlatformProperties::PlatformName();
    Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Report->SetStringField(TEXT("platform_name"), PlatformLabel);
    Report->SetStringField(TEXT("platform_architecture"), PlatformLabel);
    Report->SetStringField(TEXT("platform_os"), PlatformLabel);

    // Device state (sets audio_enabled and the three quality fields), then the assumed defaults.
    UEAgentAudioImpl::AddDeviceFacts(Report);
    UEAgentAudioImpl::AddAssumedDefaults(Report);

    // Derived scores and advice.
    Report->SetNumberField(TEXT("audio_quality_score"), UEAgentAudioImpl::ActiveShare(Census, 0.7f));
    Report->SetArrayField(TEXT("audio_recommendations"), UEAgentAudioImpl::CensusAdvice(Census));
    const float Complexity = UEAgentAudioImpl::ComplexityScore(Census);
    Report->SetNumberField(TEXT("audio_complexity_score"), Complexity);
    Report->SetStringField(TEXT("audio_complexity_rating"), UEAgentAudioImpl::ComplexityRating(Complexity));
    return Report;
}

// Playback

// MCP:play_sound - Starts the sound already assigned to an audio component of the editor world, matched by object name (case-insensitive) against the first audio component of each actor; components in a running PIE world are not reached. start_time is an offset in seconds into the sound. A component without a sound is an error: assign one with set_audio_sound. Transient: nothing is saved, marked dirty or undoable. was_playing and playing are read just before and just after the call. Stop with stop_sound; to play an asset at a location without a component use play_sound_at_location.
//   @param audio_component_name - Name of the audio component to play sound on
//   @param start_time - Optional offset in seconds to start playback at
//   @returns { audio_component_name: string, playing: bool, was_playing: bool, sound_name: string, start_time: number, success: bool }
//   @hints additive

TSharedPtr<FJsonObject> HandlePlaySound(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateInEditorWorld(Wanted, AudioComp))
    {
        return Failure;
    }
    USoundBase* Assigned = AudioComp->GetSound();
    if (Assigned == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Audio component has no sound assigned. Use set_audio_sound first."));
    }

    float Offset = 0.0f;
    Params->TryGetNumberField(TEXT("start_time"), Offset);
    const bool bWasPlaying = AudioComp->IsPlaying();
    AudioComp->Play(Offset);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("audio_component_name"), Wanted);
    Report->SetBoolField(TEXT("was_playing"), bWasPlaying);
    Report->SetBoolField(TEXT("playing"), AudioComp->IsPlaying());
    Report->SetStringField(TEXT("sound_name"), Assigned->GetName());
    Report->SetNumberField(TEXT("start_time"), Offset);
    Report->SetBoolField(TEXT("success"), true);
    return Report;
}

// MCP:stop_sound - Stop playback on an audio component of the editor world (UAudioComponent::Stop), matched by object name against the first audio component of each actor; components in a running PIE world are not reached. Returns was_playing and playing; success is always true. Start playback with play_sound.
//   @param audio_component_name - Object name of the audio component to stop (e.g. "AudioComponent0"), compared without case; only the first audio component of each actor is searched.
//   @returns { audio_component_name: string, playing: bool, was_playing: bool, success: bool }

TSharedPtr<FJsonObject> HandleStopSound(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateInEditorWorld(Wanted, AudioComp))
    {
        return Failure;
    }

    const bool bWasPlaying = AudioComp->IsPlaying();
    AudioComp->Stop();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("audio_component_name"), Wanted);
    Report->SetBoolField(TEXT("was_playing"), bWasPlaying);
    Report->SetBoolField(TEXT("playing"), AudioComp->IsPlaying());
    Report->SetBoolField(TEXT("success"), true);
    return Report;
}

// MCP:play_sound_at_location - Plays a sound asset at a specific world location without requiring an audio component.
//   @param sound_path - Full asset path to the sound (e.g. '/Game/Audio/MySound.MySound')
//   @param location - [x, y, z] 3D location to play the sound at
//   @param volume - Optional volume multiplier; defaults to 1.0
//   @param pitch - Optional pitch multiplier; defaults to 1.0
//   @param rotation - Optional [pitch, yaw, roll] rotation of the sound (it matters to a directional attenuation)
//   @returns { sound_path: string, sound_name: string, location: { x, y, z }, rotation: { pitch, yaw, roll }, volume: number, pitch: number, success: bool }

TSharedPtr<FJsonObject> HandlePlaySoundAtLocation(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Presence is checked on the array itself: the world origin (0, 0, 0) is a valid target.
    const TArray<TSharedPtr<FJsonValue>>* LocationArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("location"), LocationArray) || LocationArray->Num() < 3)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid location parameter"));
    }
    const FVector Where = mcp_common::ReadJsonVector(Params, TEXT("location"));
    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr || !EditorWorld->IsValidLowLevel())
    {
        return mcp_common::MakeErrorReply(TEXT("No valid world found"));
    }
    USoundBase* Sound = UEAgentAudioImpl::LoadSound(AssetPath);
    if (Sound == nullptr)
    {
        return mcp_common::MakeErrorReply(UEAgentAudioImpl::SoundNotFound(AssetPath));
    }

    float Volume = 1.0f;
    float Pitch = 1.0f;
    Params->TryGetNumberField(TEXT("volume"), Volume);
    Params->TryGetNumberField(TEXT("pitch"), Pitch);
    // The orientation matters to a directional (cone) attenuation.
    FRotator Facing(0, 0, 0);
    if (Params->HasField(TEXT("rotation")))
    {
        Facing = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    UGameplayStatics::PlaySoundAtLocation(EditorWorld, Sound, Where, Facing, Volume, Pitch,
        /*StartTime=*/0.0f, /*AttenuationSettings=*/nullptr, /*ConcurrencySettings=*/nullptr);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("sound_path"), AssetPath);
    Report->SetStringField(TEXT("sound_name"), Sound->GetName());
    Report->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Where));
    Report->SetObjectField(TEXT("rotation"), mcp_common::RotatorToJsonObject(Facing));
    Report->SetNumberField(TEXT("volume"), Volume);
    Report->SetNumberField(TEXT("pitch"), Pitch);
    Report->SetBoolField(TEXT("success"), true);
    return Report;
}

// MCP:set_audio_sound - Sets the sound asset of an audio component of the editor world, matched by object name (case-insensitive) against the first audio component of each actor; components in a running PIE world are not reached. A component that is playing restarts with the new sound unless it allows multiple instances. The change is not saved, marked dirty or undoable. A missing component or a sound that does not load is an error. previous_sound_name names the sound it replaced (None when there was none). Start playback with play_sound.
//   @param audio_component_name - Name of the audio component to modify
//   @param sound_path - Full asset path to the sound (e.g. '/Game/Audio/MySound.MySound')
//   @returns { audio_component_name: string, sound_path: string, sound_name: string, previous_sound_name: string, sound_duration: number, success: bool }

TSharedPtr<FJsonObject> HandleSetAudioSound(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateInEditorWorld(Wanted, AudioComp))
    {
        return Failure;
    }
    USoundBase* Sound = UEAgentAudioImpl::LoadSound(AssetPath);
    if (Sound == nullptr)
    {
        return mcp_common::MakeErrorReply(UEAgentAudioImpl::SoundNotFound(AssetPath));
    }

    USoundBase* Replaced = AudioComp->GetSound();
    AudioComp->SetSound(Sound);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("audio_component_name"), Wanted);
    Report->SetStringField(TEXT("sound_path"), AssetPath);
    Report->SetStringField(TEXT("sound_name"), Sound->GetName());
    Report->SetStringField(TEXT("previous_sound_name"), Replaced ? Replaced->GetName() : FString(TEXT("None")));
    Report->SetNumberField(TEXT("sound_duration"), Sound->GetDuration());
    Report->SetBoolField(TEXT("success"), true);
    return Report;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("configure_audio_3d"), &HandleConfigureAudio3D },
    { TEXT("get_audio_component_properties"), &HandleGetAudioComponentProperties },
    { TEXT("get_audio_settings"), &HandleGetAudioSettings },
    { TEXT("play_sound"), &HandlePlaySound },
    { TEXT("play_sound_at_location"), &HandlePlaySoundAtLocation },
    { TEXT("set_audio_sound"), &HandleSetAudioSound },
    { TEXT("set_audio_volume"), &HandleSetAudioVolume },
    { TEXT("stop_sound"), &HandleStopSound },
};
} // namespace UEAgentAudioCommands
