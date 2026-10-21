// Copyright (c) 2026 CarlxsMG.

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
#include "AudioDeviceManager.h"
#include "AudioThread.h"
#include "ActiveSound.h"
#include "Audio.h"
#include "Sound/AudioSettings.h"
#include "GameFramework/GameUserSettings.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/UObjectGlobals.h"
#include "Runtime/Launch/Resources/Version.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformMisc.h"
#include "Modules/ModuleManager.h"
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

    // The first world context the engine holds. set_audio_3d_settings and get_audio_settings have
    // always read this one instead of asking GEditor for its editor world.
    UWorld* FirstContextWorld()
    {
        return mcp_common::GetFirstContextWorld();
    }

    // The lookup every per-component command shares: EVERY audio component of every actor of World takes part, the
    // name compared without case, and a non-empty OwnerName keeps only the actor of that object name. Component
    // names are unique per actor only -- every AmbientSound's is AudioComponent0 -- so a name that matches on several
    // actors is an error naming the owners, never the first one the iterator happens to reach. Returns the error
    // response to send back, or null once OutComponent has been found.
    TSharedPtr<FJsonObject> LocateAudioComponent(UWorld* World, const FString& ComponentName, const FString& OwnerName, UAudioComponent*& OutComponent)
    {
        OutComponent = nullptr;
        if (World == nullptr || !World->IsValidLowLevel())
        {
            return mcp_common::MakeErrorReply(TEXT("No valid world found"));
        }
        const bool bOwnerFilter = !OwnerName.IsEmpty();
        bool bOwnerSeen = false;
        TArray<UAudioComponent*> Matches;
        for (TActorIterator<AActor> Owner(World); Owner; ++Owner)
        {
            if (bOwnerFilter)
            {
                if (Owner->GetName() != OwnerName)
                {
                    continue;
                }
                bOwnerSeen = true;
            }
            TInlineComponentArray<UAudioComponent*> AudioComponents(*Owner);
            for (UAudioComponent* Candidate : AudioComponents)
            {
                if (Candidate->GetName() == ComponentName)
                {
                    Matches.Add(Candidate);
                }
            }
        }
        if (bOwnerFilter && !bOwnerSeen)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Actor not found in the editor world: %s (actor_name is the object name, not the label)"), *OwnerName));
        }
        if (Matches.IsEmpty())
        {
            return mcp_common::MakeErrorReply(bOwnerFilter
                ? FString::Printf(TEXT("Audio component not found: %s on actor %s"), *ComponentName, *OwnerName)
                : FString::Printf(TEXT("Audio component not found: %s"), *ComponentName));
        }
        if (Matches.Num() > 1)
        {
            TArray<FString> Owners;
            for (const UAudioComponent* Match : Matches)
            {
                Owners.Add(Match->GetOwner() != nullptr ? Match->GetOwner()->GetName() : FString(TEXT("None")));
            }
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Audio component name '%s' matches %d audio components, owned by %s; %s"),
                *ComponentName, Matches.Num(), *FString::Join(Owners, TEXT(", ")),
                bOwnerFilter ? TEXT("several actors share that object name.") : TEXT("pass actor_name to pick one.")));
        }
        OutComponent = Matches[0];
        return nullptr;
    }

    // The optional actor_name param, the owner filter of LocateAudioComponent. An empty one is refused rather than
    // read as "any actor". Null when it reads (OutOwner empty when the param is absent).
    TSharedPtr<FJsonObject> ReadOwnerFilter(const TSharedPtr<FJsonObject>& Params, FString& OutOwner)
    {
        OutOwner.Reset();
        if (Params->TryGetStringField(TEXT("actor_name"), OutOwner) && OutOwner.IsEmpty())
        {
            return mcp_common::MakeErrorReply(EmptyParameter(TEXT("actor_name")));
        }
        return nullptr;
    }

    // Which component a call reached: its object name, its full object path and its owning actor.
    void AddComponentIdentity(const TSharedPtr<FJsonObject>& Out, const UAudioComponent* AudioComp)
    {
        const AActor* Owner = AudioComp->GetOwner();
        Out->SetStringField(TEXT("audio_component_name"), AudioComp->GetName());
        Out->SetStringField(TEXT("audio_component_path"), AudioComp->GetPathName());
        Out->SetStringField(TEXT("audio_owner_name"), Owner ? Owner->GetName() : FString(TEXT("None")));
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

    // The audio device a one-shot at a location of World would be handed to, refusing -- with why, Sound named --
    // each case where UGameplayStatics::PlaySoundAtLocation or FAudioDevice::PlaySoundAtLocation would drop the
    // request without a word: no device manager (audio off: -NoSound, a commandlet or a server), a world that does not
    // allow audio playback or is a dedicated server, no device for the world, an active level collection that is not
    // visible. Null once OutDevice and OutDeviceId are set.
    TSharedPtr<FJsonObject> RequireWorldAudioDevice(UWorld* World, const USoundBase* Sound, FAudioDevice*& OutDevice, uint32& OutDeviceId)
    {
        OutDevice = nullptr;
        OutDeviceId = 0;
        const FString SoundName = Sound->GetName();
        if (GEngine == nullptr || !GEngine->UseSound())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No audio device: audio is off in this editor (started with -NoSound, or no audio device could be created), so %s was not played"), *SoundName));
        }
        if (!World->bAllowAudioPlayback || World->IsNetMode(NM_DedicatedServer))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("The editor world does not allow audio playback (bAllowAudioPlayback is false, or it is a dedicated server), so %s was not played"), *SoundName));
        }
        const FAudioDeviceHandle WorldDevice = World->GetAudioDevice();
        FAudioDevice* Device = WorldDevice.IsValid() ? WorldDevice.GetAudioDevice() : nullptr;
        if (Device == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No audio device for the editor world, so %s was not played"), *SoundName));
        }
        const FLevelCollection* ActiveCollection = World->GetActiveLevelCollection();
        if (ActiveCollection != nullptr && !ActiveCollection->IsVisible())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("The editor world's active level collection is not visible, so the engine would not play %s"), *SoundName));
        }
        OutDevice = Device;
        OutDeviceId = WorldDevice.GetDeviceID();
        return nullptr;
    }

    // How many active sounds of Sound the device holds. Active sounds may only be read on the audio thread, so the
    // count runs there, after every audio command queued before it (a play request included), while the game thread
    // waits for it; without an audio thread the command runs right here.
    int32 CountActiveSoundsOf(FAudioDevice* Device, const USoundBase* Sound)
    {
        int32 Count = 0;
        int32* CountOut = &Count;
        FAudioThread::RunCommandOnAudioThread([Device, Sound, CountOut]()
        {
            for (const FActiveSound* Active : Device->GetActiveSounds())
            {
                if (Active != nullptr && Active->GetSound() == Sound)
                {
                    ++(*CountOut);
                }
            }
        });
        FAudioCommandFence Fence;
        Fence.BeginFence();
        Fence.Wait();
        return Count;
    }

    // Instance IDs of the active sounds the device holds for one audio component and is not about to stop, read on
    // the audio thread after every audio command queued before it (a Play or a Stop included), while the game thread
    // waits for it; without an audio thread the command runs right here.
    TArray<uint32> LiveActiveSoundsOfComponent(FAudioDevice* Device, const uint64 ComponentId)
    {
        TArray<uint32> InstanceIds;
        TArray<uint32>* IdsOut = &InstanceIds;
        FAudioThread::RunCommandOnAudioThread([Device, ComponentId, IdsOut]()
        {
            for (FActiveSound* Active : Device->GetActiveSounds())
            {
                if (Active != nullptr && Active->GetAudioComponentID() == ComponentId && !Device->IsPendingStop(Active))
                {
                    IdsOut->Add(Active->GetInstanceID());
                }
            }
        });
        FAudioCommandFence Fence;
        Fence.BeginFence();
        Fence.Wait();
        return InstanceIds;
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

    // Read-back context shared by set_audio_volume and set_audio_3d_settings.
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

    // Values set_audio_3d_settings describes for a named preset.
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

    // Advice attached to set_audio_3d_settings, judged on the preset's numbers.
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

    // Tallies taken by get_audio_settings over every audio component of every actor.
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

    // Adds one audio component to the tallies and returns its detail entry, owner and object path included.
    TSharedPtr<FJsonObject> TallyAudioComponent(UAudioComponent* AudioComp, FAudioCensus& Census)
    {
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
        AddComponentIdentity(Entry, AudioComp);
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
        return Entry;
    }

    // Walks the actors in iteration order and EVERY audio component of each (an actor may own several),
    // updating the tallies and returning one detail entry per audio component found.
    TArray<TSharedPtr<FJsonValue>> TakeAudioCensus(UWorld* World, FAudioCensus& Census)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (TActorIterator<AActor> Owner(World); Owner; ++Owner)
        {
            TInlineComponentArray<UAudioComponent*> AudioComponents(*Owner);
            for (UAudioComponent* AudioComp : AudioComponents)
            {
                Entries.Add(MakeShared<FJsonValueObject>(TallyAudioComponent(AudioComp, Census)));
            }
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

    // The main audio device, or an invalid handle when audio is off (-NoSound, or no device could be created). Not
    // FAudioDevice::GetMainAudioDevice(): with audio disabled it falls back to a device manager that does not exist
    // and dereferences it.
    FAudioDeviceHandle MainAudioDevice()
    {
        FAudioDeviceHandle MainDevice = GEngine ? GEngine->GetMainAudioDevice() : FAudioDeviceHandle();
        if (!MainDevice.IsValid() && GEngine && GEngine->GetAudioDeviceManager())
        {
            MainDevice = GEngine->GetAudioDeviceManager()->GetActiveAudioDevice();
        }
        return MainDevice;
    }

    // Name of the module that created the audio devices (e.g. AudioMixerWasapi), empty when audio is off. The device
    // manager keeps only the module's instance, so it is matched against the instance of every loaded module; an
    // audio device module derives from IModuleInterface alone, so both pointers hold the same address.
    FString LoadedAudioDeviceModuleName()
    {
        FAudioDeviceManager* Manager = GEngine ? GEngine->GetAudioDeviceManager() : nullptr;
        const void* DeviceModule = Manager ? static_cast<const void*>(Manager->GetAudioDeviceModule()) : nullptr;
        if (DeviceModule == nullptr)
        {
            return FString();
        }
        FModuleManager& Modules = FModuleManager::Get();
        TArray<FModuleStatus> Statuses;
        Modules.QueryModules(Statuses);
        for (const FModuleStatus& Status : Statuses)
        {
            if (Status.bIsLoaded && static_cast<const void*>(Modules.GetModule(FName(*Status.Name))) == DeviceModule)
            {
                return Status.Name;
            }
        }
        return FString();
    }

    // Live facts read from the main audio device. Without a device every one of them is null, audio_enabled is
    // false and audio_device_note says why.
    void AddDeviceFacts(const TSharedPtr<FJsonObject>& Out)
    {
        const FAudioDeviceHandle MainDevice = MainAudioDevice();
        FAudioDevice* Device = MainDevice.IsValid() ? MainDevice.GetAudioDevice() : nullptr;
        Out->SetBoolField(TEXT("audio_enabled"), Device != nullptr);

        const FString ModuleName = LoadedAudioDeviceModuleName();
        if (ModuleName.IsEmpty())
        {
            Out->SetField(TEXT("audio_api"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Out->SetStringField(TEXT("audio_api"), ModuleName);
        }

        if (Device == nullptr)
        {
            static const TCHAR* const DeviceKeys[] = {
                TEXT("audio_mixer_loaded"), TEXT("audio_max_channels"), TEXT("audio_sample_rate"), TEXT("audio_buffer_size"),
                TEXT("audio_num_buffers"), TEXT("audio_spatialization_plugin_enabled"), TEXT("audio_reverb_plugin_enabled"),
                TEXT("audio_occlusion_plugin_enabled"), TEXT("audio_modulation_plugin_enabled") };
            for (const TCHAR* Key : DeviceKeys)
            {
                Out->SetField(Key, MakeShared<FJsonValueNull>());
            }
            Out->SetStringField(TEXT("audio_device_note"),
                TEXT("No audio device: audio is off in this editor (started with -NoSound, or no audio device could be created), so the device fields are null."));
            return;
        }
        Out->SetBoolField(TEXT("audio_mixer_loaded"), Device->bAudioMixerModuleLoaded != 0);
        Out->SetNumberField(TEXT("audio_max_channels"), Device->GetMaxChannels());
        Out->SetNumberField(TEXT("audio_sample_rate"), Device->GetSampleRate());
        Out->SetNumberField(TEXT("audio_buffer_size"), Device->GetBufferLength());
        Out->SetNumberField(TEXT("audio_num_buffers"), Device->GetNumBuffers());
        Out->SetBoolField(TEXT("audio_spatialization_plugin_enabled"), Device->IsSpatializationPluginEnabled());
        Out->SetBoolField(TEXT("audio_reverb_plugin_enabled"), Device->IsReverbPluginEnabled());
        Out->SetBoolField(TEXT("audio_occlusion_plugin_enabled"), Device->IsOcclusionPluginEnabled());
        Out->SetBoolField(TEXT("audio_modulation_plugin_enabled"), Device->IsModulationPluginEnabled());
    }

    // The audio quality level the engine applies, as FAudioDevice picks it: the user's quality level (game user
    // settings) clamped to the levels of the project's Audio settings, with that level's name and channel budget.
    void AddQualityFacts(const TSharedPtr<FJsonObject>& Out)
    {
        const UAudioSettings* Settings = GetDefault<UAudioSettings>();
        const int32 LevelCount = Settings ? Settings->GetQualityLevelSettingsNum() : 0;
        const UGameUserSettings* UserSettings = GEngine ? GEngine->GetGameUserSettings() : nullptr;
        Out->SetNumberField(TEXT("audio_quality_level_count"), LevelCount);
        if (LevelCount <= 0 || UserSettings == nullptr)
        {
            Out->SetField(TEXT("audio_quality"), MakeShared<FJsonValueNull>());
            Out->SetField(TEXT("audio_quality_level"), MakeShared<FJsonValueNull>());
            Out->SetField(TEXT("audio_quality_max_channels"), MakeShared<FJsonValueNull>());
            return;
        }
        const int32 Level = FMath::Clamp(UserSettings->GetAudioQualityLevel(), 0, LevelCount - 1);
        const FAudioQualitySettings& Quality = Settings->GetQualityLevelSettings(Level);
        Out->SetStringField(TEXT("audio_quality"), Quality.DisplayName.ToString());
        Out->SetNumberField(TEXT("audio_quality_level"), Level);
        Out->SetNumberField(TEXT("audio_quality_max_channels"), Quality.MaxChannels);
    }

    // The platform's audio settings as the project's engine config sets them, in the section of the platform's
    // runtime settings class (e.g. /Script/WindowsTargetPlatform.WindowsTargetSettings) where the audio device reads
    // them. A key the config does not set is left out: the engine then uses its built-in default.
    TSharedPtr<FJsonObject> PlatformAudioConfig()
    {
        TSharedPtr<FJsonObject> Config = MakeShared<FJsonObject>();
        const TCHAR* Section = FPlatformProperties::GetRuntimeSettingsClassName();
        Config->SetStringField(TEXT("section"), Section);
        struct FConfigKey
        {
            const TCHAR* IniKey;
            const TCHAR* Field;
        };
        static const FConfigKey Keys[] = {
            { TEXT("AudioSampleRate"), TEXT("sample_rate") },
            { TEXT("AudioCallbackBufferFrameSize"), TEXT("callback_buffer_frame_size") },
            { TEXT("AudioNumBuffersToEnqueue"), TEXT("num_buffers") },
            { TEXT("AudioMaxChannels"), TEXT("max_channels") },
            { TEXT("AudioNumSourceWorkers"), TEXT("num_source_workers") },
        };
        for (const FConfigKey& Key : Keys)
        {
            FString Value;
            if (GConfig != nullptr && GConfig->GetString(Section, Key.IniKey, Value, GEngineIni))
            {
                Config->SetNumberField(Key.Field, FCString::Atoi(*Value));
            }
        }
        return Config;
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

// MCP:get_audio_component_properties - Read an editor-level audio component by object name: volume, pitch, active state, transform, sound and its duration, and the attenuation really applied (override, else asset, else sound). Use it for these derived values; get_component_properties reads raw UPROPERTYs. Every audio component of every actor is searched, the name compared without case. Component names are unique only per actor (every AmbientSound's is AudioComponent0), so a name that several actors carry is an error listing the owners unless actor_name (object name, not label) picks one; an actor_name that names no actor, or whose actor has no component of that name, is an error too. Read-only.
//   @param audio_component_name - Object name of the audio component to query (e.g. "AudioComponent0"), compared without case against EVERY audio component of every actor in the editor world.
//   @param actor_name - Optional object name (not label) of the actor that owns the component, compared without case. Needed when several actors own an audio component of that name (every AmbientSound's is AudioComponent0): without it that is an error listing the owners. An actor_name that names no actor, or whose actor has no such component, is an error.
//   @returns { audio_component_name: string, audio_component_found: bool, audio_component_message: string, component_class: string, component_type: string, audio_volume: number, audio_pitch: number, audio_enabled: bool (IsActive), audio_auto_activate: bool, audio_mobility: string, audio_position: { x, y, z }, audio_rotation: { pitch, yaw, roll }, audio_scale: { x, y, z } (component world transform), audio_sound_name: string, audio_sound_class: string, audio_sound_duration: number, audio_sound_path: string ("None" with no sound), audio_sound_is_sound_wave: bool, audio_sound_is_sound_cue: bool, audio_3d_enabled: bool (bAllowSpatialization), audio_3d_attenuation_radius: number (shape extent X), audio_3d_attenuation_falloff: number (both 0 with no attenuation applied), audio_3d_attenuation_cone_angle: number, audio_3d_attenuation_cone_feather: number (cone shape only, else 0), audio_owner_name: string, audio_owner_class: string, audio_owner_path: string, audio_owner_position: { x, y, z }, audio_owner_rotation: { pitch, yaw, roll }, audio_level: string, audio_world: string }

TSharedPtr<FJsonObject> HandleGetAudioComponentProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Optional owner filter; an empty one is refused rather than read as "any actor".
    FString OwnerFilter;
    const bool bOwnerGiven = Params->TryGetStringField(TEXT("actor_name"), OwnerFilter);
    if (Wanted.IsEmpty())
    {
        return mcp_common::MakeErrorReply(UEAgentAudioImpl::EmptyParameter(TEXT("audio_component_name")));
    }
    if (bOwnerGiven && OwnerFilter.IsEmpty())
    {
        return mcp_common::MakeErrorReply(UEAgentAudioImpl::EmptyParameter(TEXT("actor_name")));
    }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateAudioComponent(mcp_common::GetEditorWorld(), Wanted, OwnerFilter, AudioComp))
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

// MCP:set_audio_volume - Sets the VolumeMultiplier of one audio component placed in the editor world (not PIE); the engine setter also resets its VolumeModulationMin/Max to 1. The component is found by object name (case-insensitive) among EVERY audio component of every actor; a name that components of several actors carry is an error listing the owners, nothing changed, unless actor_name (object name, not label) picks one. No undo step and no dirty mark: the value is lost on reload unless the level gets saved for another reason. A volume outside 0-10 is an error and nothing changes; the same value again gives the same result. The multiplier is read back: new_volume is the value the component holds, and one that does not match the request makes success false with volume_set false. Find components with get_audio_settings and read one with get_audio_component_properties; set_audio_sound changes its sound, set_audio_3d_settings its attenuation; to scale a whole sound class use set_sound_mix_class_override.
//   @param audio_component_name - Object name of the audio component to modify (e.g. "AudioComponent0"), compared without case against every audio component of every actor in the editor world; get_audio_settings lists them with their owners.
//   @param volume - Volume multiplier from 0.0 to 10.0 (1.0 = the sound's own level, 0.5 = half, 0 = silent); outside that range is an error.
//   @param actor_name - Optional object name (not label) of the actor that owns the component, compared without case. Needed when several actors own an audio component of that name (every AmbientSound's is AudioComponent0); an empty one, one that names no actor, or one whose actor has no such component is an error.
//   @returns { audio_component_name: string, audio_component_path: string, audio_owner_name: string, old_volume: number, new_volume: number, requested_volume: number, volume_set: bool, volume_message: string, audio_enabled: bool, audio_pitch: number, audio_mobility: string, audio_sound_name: string, audio_3d_attenuation_radius: number, audio_position: { x, y, z } }

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
    FString OwnerFilter;
    if (TSharedPtr<FJsonObject> BadOwner = UEAgentAudioImpl::ReadOwnerFilter(Params, OwnerFilter)) { return BadOwner; }
    if (TargetVolume < 0.0 || TargetVolume > 10.0)
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'volume' must be between 0.0 and 10.0"));
    }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateAudioComponent(mcp_common::GetEditorWorld(), Wanted, OwnerFilter, AudioComp))
    {
        return Failure;
    }

    const float VolumeBefore = AudioComp->VolumeMultiplier;
    AudioComp->SetVolumeMultiplier(static_cast<float>(TargetVolume));
    // Verified by reading the multiplier back and comparing it with the request.
    const float VolumeAfter = AudioComp->VolumeMultiplier;
    const bool bApplied = FMath::IsNearlyEqual(VolumeAfter, TargetVolume, 0.001f);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    UEAgentAudioImpl::AddComponentIdentity(Report, AudioComp);
    Report->SetNumberField(TEXT("old_volume"), VolumeBefore);
    Report->SetNumberField(TEXT("new_volume"), VolumeAfter);
    Report->SetNumberField(TEXT("requested_volume"), TargetVolume);
    Report->SetBoolField(TEXT("volume_set"), bApplied);
    Report->SetStringField(TEXT("volume_message"),
        bApplied ? TEXT("Audio volume set successfully") : TEXT("Failed to set audio volume"));
    UEAgentAudioImpl::AddPlaybackContext(Report, AudioComp);
    UEAgentAudioImpl::AddAttenuationFields(Report, AudioComp);
    // The verdict is the read-back: a multiplier that does not read back as requested is a failed call.
    return mcp_common::MakeOutcomeReply(Report, bApplied,
        bApplied ? FString() : mcp_common::MeasuredOutcomeError(TEXT("set_audio_volume"), TEXT("volume_set")));
}

// MCP:set_audio_3d_settings - Apply a 3D audio preset to one audio component of a placed actor in the editor world, in one undo step that dirties the level without saving it: spatialization (bAllowSpatialization) plus, for every preset but Disabled, an attenuation override sphere with the preset's inner radius and falloff distance (High 1000/2000 cm, Enabled and Medium 500/1000, Low 250/500); Disabled turns both off. audio_3d_configured is read back from the component; attenuation_falloff and the cone numbers only describe the preset. The same preset again gives the same result. The component is found by object name (case-insensitive) among EVERY audio component of every actor, so an actor's second or third audio component is reached too; a name that components of several actors carry is an error listing the owners unless actor_name (object name, not label) picks one, and the reply names the component changed (audio_component_path, audio_owner_name). A missing component or unknown preset is an error and nothing changes. Read the result with get_audio_component_properties; set_audio_volume and set_audio_sound change the rest.
//   @renamed-from configure_audio_3d
//   @param audio_component_name - Object name of the audio component, compared without case against every audio component of every actor, e.g. 'AudioComponent0' (get_audio_settings lists them with their owners).
//   @param audio_3d_settings - Preset name: 'Enabled', 'Disabled', 'High', 'Medium' or 'Low' (case-insensitive); any other name is an error.
//   @param actor_name - Optional object name (not label) of the actor that owns the component, compared without case. Needed when several actors own an audio component of that name (every AmbientSound's is AudioComponent0); an empty one, one that names no actor, or one whose actor has no such component is an error.
//   @returns { audio_component_name: string, audio_component_path: string, audio_owner_name: string, audio_3d_settings: string, audio_3d_configured: bool, audio_3d_message: string, old_3d_enabled: bool, new_3d_enabled: bool, attenuation_override: bool, applied_attenuation_radius?: number, applied_falloff_distance?: number, 3d_enabled: bool, spatialization_enabled: bool, attenuation_distance: number, attenuation_radius: number, attenuation_falloff: number, attenuation_cone_angle: number, attenuation_cone_feather: number, audio_3d_recommendations: [string] }

TSharedPtr<FJsonObject> HandleConfigureAudio3D(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PresetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_3d_settings"), PresetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString OwnerFilter;
    if (TSharedPtr<FJsonObject> BadOwner = UEAgentAudioImpl::ReadOwnerFilter(Params, OwnerFilter)) { return BadOwner; }

    UWorld* ContextWorld = UEAgentAudioImpl::FirstContextWorld();
    if (ContextWorld == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateAudioComponent(ContextWorld, Wanted, OwnerFilter, AudioComp))
    {
        return Failure;
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
    UEAgentAudioImpl::AddComponentIdentity(Report, AudioComp);
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

// MCP:get_audio_settings - Survey audio in the editor, every value read from the engine: a census of EVERY audio component of every actor in the editor world (even while PIE runs; an actor with several is counted once per component), each entry naming its owner and object path, with its volume, pitch and playing, looping and spatialized state, plus the counts and volume and pitch statistics; the main audio device's live state (audio_api is the module that created the device, then sample rate, buffer size, max channels, mixer and which audio plugins are on), all null with audio_device_note when there is no device (audio off: an editor started with -NoSound, or no device could be created); the audio quality level the engine applies (the user's quality level within the project's Audio settings levels); and the platform audio config the project sets (audio_platform_config, only the keys the config sets). Read-only; no parameters. audio_quality_score (the share of active components, null with none), the recommendations and the complexity score are heuristics. For other project audio settings use get_project_settings_value; for one component in depth, get_audio_component_properties.
//   @returns { audio_settings_found: bool, audio_settings_message: string, world_name?: string, audio_component_count: int, active_audio_component_count: int, playing_audio_component_count: int, looping_audio_component_count: int, spatialized_audio_component_count: int, sound_wave_count: int, sound_cue_count: int, total_volume: number, average_volume: number, max_volume: number, min_volume: number, total_pitch: number, average_pitch: number, max_pitch: number, min_pitch: number, audio_component_details: [{ audio_component_name: string, audio_component_path: string, audio_owner_name: string, audio_component_class: string, audio_volume: number, audio_pitch: number, audio_enabled: bool, audio_playing: bool, audio_looping: bool, audio_spatialized: bool, audio_mobility: string, audio_sound_name: string, audio_sound_class: string, audio_sound_duration: number, audio_sound_is_sound_wave: bool, audio_sound_is_sound_cue: bool, audio_position: { x, y, z } }], engine_version: string, platform_name: string, platform_architecture: string, platform_os: string, audio_enabled: bool, audio_api: string|null, audio_mixer_loaded: bool|null, audio_max_channels: int|null, audio_sample_rate: number|null, audio_buffer_size: int|null, audio_num_buffers: int|null, audio_spatialization_plugin_enabled: bool|null, audio_reverb_plugin_enabled: bool|null, audio_occlusion_plugin_enabled: bool|null, audio_modulation_plugin_enabled: bool|null, audio_device_note?: string, audio_quality: string|null, audio_quality_level: int|null, audio_quality_max_channels: int|null, audio_quality_level_count: int, audio_platform_config: { section: string, sample_rate?: int, callback_buffer_frame_size?: int, num_buffers?: int, max_channels?: int, num_source_workers?: int }, audio_quality_score: number|null, audio_recommendations: [string], audio_complexity_score: number, audio_complexity_rating: string }

TSharedPtr<FJsonObject> HandleGetAudioSettings(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("audio_settings_found"), true);
    Report->SetStringField(TEXT("audio_settings_message"), TEXT("Audio settings retrieved successfully"));

    // Per-component statistics; the whole block is omitted when there is no world to walk.
    UEAgentAudioImpl::FAudioCensus Census;
    if (UWorld* ContextWorld = UEAgentAudioImpl::FirstContextWorld())
    {
        Report->SetStringField(TEXT("world_name"), ContextWorld->GetName());
        const TArray<TSharedPtr<FJsonValue>> PerComponent = UEAgentAudioImpl::TakeAudioCensus(ContextWorld, Census);
        UEAgentAudioImpl::AddCensusTotals(Report, Census);
        Report->SetArrayField(TEXT("audio_component_details"), PerComponent);
    }

    // Build and platform identity: the platform this editor was built for, the host CPU architecture and the
    // operating system's version string.
    const FString PlatformLabel = FPlatformProperties::PlatformName();
    Report->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    Report->SetStringField(TEXT("platform_name"), PlatformLabel);
    Report->SetStringField(TEXT("platform_architecture"), FPlatformMisc::GetHostArchitecture());
    Report->SetStringField(TEXT("platform_os"), FPlatformMisc::GetOSVersion());

    // The live device (null fields without one), the quality level the engine applies and the platform config.
    UEAgentAudioImpl::AddDeviceFacts(Report);
    UEAgentAudioImpl::AddQualityFacts(Report);
    Report->SetObjectField(TEXT("audio_platform_config"), UEAgentAudioImpl::PlatformAudioConfig());

    // Derived scores and advice. The share of active components has no value without components.
    if (Census.Components > 0)
    {
        Report->SetNumberField(TEXT("audio_quality_score"),
            FMath::Clamp(static_cast<float>(Census.Active) / static_cast<float>(Census.Components), 0.0f, 1.0f));
    }
    else
    {
        Report->SetField(TEXT("audio_quality_score"), MakeShared<FJsonValueNull>());
    }
    Report->SetArrayField(TEXT("audio_recommendations"), UEAgentAudioImpl::CensusAdvice(Census));
    const float Complexity = UEAgentAudioImpl::ComplexityScore(Census);
    Report->SetNumberField(TEXT("audio_complexity_score"), Complexity);
    Report->SetStringField(TEXT("audio_complexity_rating"), UEAgentAudioImpl::ComplexityRating(Complexity));
    return Report;
}

// Playback

// MCP:play_sound - Starts the sound already assigned to an audio component of the editor world, found by object name (case-insensitive) among EVERY audio component of every actor, so an actor's second or third audio component is reached too; a name that components of several actors carry is an error listing the owners unless actor_name (object name, not label) picks one. Components in a running PIE world are not reached. start_time is an offset in seconds into the sound. A component without a sound is an error: assign one with set_audio_sound. Transient: nothing is saved, marked dirty or undoable. success is true only when the audio device holds a new active sound for this component afterwards, read on the audio thread before and after the call (active_sounds_before/after, sounds of the component not being stopped). No audio device (an editor started with -NoSound, or no device could be created), a world that does not allow audio playback, or a sound the device did not start (not playable, out of range, a concurrency limit, a looping sound parked as a virtual loop until a listener comes in range) is an error saying which and naming the component. was_playing and playing (UAudioComponent::IsPlaying) are read just before and just after the call; audio_component_path and audio_owner_name name the component used. Stop with stop_sound; to play an asset at a location without a component use play_sound_at_location.
//   @param audio_component_name - Object name of the audio component to play (e.g. "AudioComponent0"), compared without case against every audio component of every actor in the editor world.
//   @param start_time - Optional offset in seconds to start playback at
//   @param actor_name - Optional object name (not label) of the actor that owns the component, compared without case. Needed when several actors own an audio component of that name (every AmbientSound's is AudioComponent0); an empty one, one that names no actor, or one whose actor has no such component is an error.
//   @returns { audio_component_name: string, audio_component_path: string, audio_owner_name: string, playing: bool, was_playing: bool, sound_name: string, start_time: number, active_sounds_before: int, active_sounds_after: int (active sounds of this component on the device, those being stopped left out), active_sound_started: bool, success: bool }
//   @hints additive

TSharedPtr<FJsonObject> HandlePlaySound(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString OwnerFilter;
    if (TSharedPtr<FJsonObject> BadOwner = UEAgentAudioImpl::ReadOwnerFilter(Params, OwnerFilter)) { return BadOwner; }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateAudioComponent(mcp_common::GetEditorWorld(), Wanted, OwnerFilter, AudioComp))
    {
        return Failure;
    }
    USoundBase* Assigned = AudioComp->GetSound();
    if (Assigned == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Audio component has no sound assigned. Use set_audio_sound first."));
    }

    // UAudioComponent::Play returns nothing and does nothing on each of these, so they are tested first, each refusal
    // naming the component; nothing is changed.
    const FString ComponentPath = AudioComp->GetPathName();
    if (GEngine == nullptr || !GEngine->UseSound())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No audio device: audio is off in this editor (started with -NoSound, or no audio device could be created), so %s did not play %s"),
            *ComponentPath, *Assigned->GetName()));
    }
    const UWorld* ComponentWorld = AudioComp->GetWorld();
    if (ComponentWorld != nullptr && !ComponentWorld->bAllowAudioPlayback)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The world of %s does not allow audio playback (bAllowAudioPlayback is false), so it did not play %s"),
            *ComponentPath, *Assigned->GetName()));
    }
    FAudioDevice* Device = AudioComp->GetAudioDevice();
    if (Device == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No audio device for the world of %s, so it did not play %s"), *ComponentPath, *Assigned->GetName()));
    }

    float Offset = 0.0f;
    Params->TryGetNumberField(TEXT("start_time"), Offset);
    const uint64 ComponentId = AudioComp->GetAudioComponentID();
    const bool bWasPlaying = AudioComp->IsPlaying();
    const TArray<uint32> LiveBefore = UEAgentAudioImpl::LiveActiveSoundsOfComponent(Device, ComponentId);
    AudioComp->Play(Offset);
    const TArray<uint32> LiveAfter = UEAgentAudioImpl::LiveActiveSoundsOfComponent(Device, ComponentId);
    // A restart stops the previous active sound (left out as being stopped), so the test is a NEW instance, not a
    // larger count.
    bool bStarted = false;
    for (const uint32 InstanceId : LiveAfter)
    {
        bStarted = bStarted || !LiveBefore.Contains(InstanceId);
    }
    if (!bStarted)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s was asked to play %s but the audio device holds no new active sound for it (%d before, %d after): the device did not start it. The sound is not playable (e.g. a cue with nothing wired to its output), out of range of every listener, refused by a concurrency limit, or, if it loops, parked as a virtual loop that starts when a listener comes in range (stop_sound stops it)"),
            *ComponentPath, *Assigned->GetName(), LiveBefore.Num(), LiveAfter.Num()));
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    UEAgentAudioImpl::AddComponentIdentity(Report, AudioComp);
    Report->SetBoolField(TEXT("was_playing"), bWasPlaying);
    Report->SetBoolField(TEXT("playing"), AudioComp->IsPlaying());
    Report->SetStringField(TEXT("sound_name"), Assigned->GetName());
    Report->SetNumberField(TEXT("start_time"), Offset);
    Report->SetNumberField(TEXT("active_sounds_before"), LiveBefore.Num());
    Report->SetNumberField(TEXT("active_sounds_after"), LiveAfter.Num());
    Report->SetBoolField(TEXT("active_sound_started"), bStarted);
    Report->SetBoolField(TEXT("success"), bStarted);
    return Report;
}

// MCP:stop_sound - Stop playback on an audio component of the editor world (UAudioComponent::Stop), found by object name (case-insensitive) among EVERY audio component of every actor, so an actor's second or third audio component is reached too; a name that components of several actors carry is an error listing the owners unless actor_name (object name, not label) picks one. Components in a running PIE world are not reached. Returns was_playing and playing, and audio_component_path and audio_owner_name naming the component stopped; success is always true once the component is found. Start playback with play_sound.
//   @param audio_component_name - Object name of the audio component to stop (e.g. "AudioComponent0"), compared without case against every audio component of every actor in the editor world.
//   @param actor_name - Optional object name (not label) of the actor that owns the component, compared without case. Needed when several actors own an audio component of that name (every AmbientSound's is AudioComponent0); an empty one, one that names no actor, or one whose actor has no such component is an error.
//   @returns { audio_component_name: string, audio_component_path: string, audio_owner_name: string, playing: bool, was_playing: bool, success: bool }

TSharedPtr<FJsonObject> HandleStopSound(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString OwnerFilter;
    if (TSharedPtr<FJsonObject> BadOwner = UEAgentAudioImpl::ReadOwnerFilter(Params, OwnerFilter)) { return BadOwner; }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateAudioComponent(mcp_common::GetEditorWorld(), Wanted, OwnerFilter, AudioComp))
    {
        return Failure;
    }

    const bool bWasPlaying = AudioComp->IsPlaying();
    AudioComp->Stop();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    UEAgentAudioImpl::AddComponentIdentity(Report, AudioComp);
    Report->SetBoolField(TEXT("was_playing"), bWasPlaying);
    Report->SetBoolField(TEXT("playing"), AudioComp->IsPlaying());
    Report->SetBoolField(TEXT("success"), true);
    return Report;
}

// MCP:play_sound_at_location - Play a sound asset (sound wave, sound cue or MetaSound source) once at a world location in the editor world, as a fire-and-forget one-shot with no audio component (UGameplayStatics::PlaySoundAtLocation) and only the sound's own attenuation. Nothing is saved, dirtied or undoable, and no handle comes back: the sound plays to its end, and a looping one keeps looping with no way to stop it from here. Each call starts another instance. success is true only when the audio device holds a new active sound of that asset afterwards, counted on the audio thread before and after the call (active_sounds_before/after). No audio device (an editor started with -NoSound, or no device could be created), a world that does not allow audio playback, a one-shot out of earshot of every listener (the engine's own audibility test), or a sound the device did not start (not playable, a concurrency limit, a looping sound parked as a virtual loop) is an error saying which. A sound that does not load or a location that is not an array of 3 numbers is an error too. For playback you can stop or inspect, use play_sound and stop_sound on an audio component (set_audio_sound assigns its sound).
//   @param sound_path - Object path of the sound asset, e.g. '/Game/Audio/MySound.MySound' (the package path '/Game/Audio/MySound' also loads).
//   @param location - World position [x, y, z] in centimetres, e.g. [0, 0, 100]; required, and the origin [0, 0, 0] is valid.
//   @param volume - Optional volume multiplier, e.g. 0.5 for half; default 1.0.
//   @param pitch - Optional pitch multiplier, e.g. 2.0 for an octave up; default 1.0.
//   @param rotation - Optional [pitch, yaw, roll] in degrees, the facing of the sound (it matters to a directional attenuation); default [0, 0, 0].
//   @returns { sound_path: string, sound_name: string, location: { x, y, z }, rotation: { pitch, yaw, roll }, volume: number, pitch: number, audio_device_id: int, audible: bool (the engine's audibility test at that location), max_distance: number, looping: bool, active_sounds_before: int, active_sounds_after: int (active sounds of this asset on the device), active_sound_created: bool, success: bool }
//   @hints additive

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

    // UGameplayStatics::PlaySoundAtLocation and FAudioDevice::PlaySoundAtLocation return nothing and drop the request
    // silently on each of these, so they are tested first, in the engine's order, and the refusal says which.
    FAudioDevice* Device = nullptr;
    uint32 DeviceId = 0;
    if (const TSharedPtr<FJsonObject> NoDevice = UEAgentAudioImpl::RequireWorldAudioDevice(EditorWorld, Sound, Device, DeviceId))
    {
        return NoDevice;
    }
    const FSoundAttenuationSettings* Attenuation = Sound->GetAttenuationSettingsToApply();
    float MaxDistance = 0.0f;
    float FocusFactor = 1.0f;
    Device->GetMaxDistanceAndFocusFactor(Sound, EditorWorld, Where, Attenuation, MaxDistance, FocusFactor);
    const bool bAudible = Device->SoundIsAudible(Sound, EditorWorld, Where, Attenuation, MaxDistance, FocusFactor);
    const bool bLooping = Sound->IsLooping();
    if (!bAudible && !bLooping && !Sound->IsPlayWhenSilent())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s was not played: at that location it is out of earshot of every listener (max distance %.0f cm), and the engine does not start a one-shot that begins out of range"),
            *Sound->GetName(), static_cast<double>(MaxDistance)));
    }

    const int32 ActiveBefore = UEAgentAudioImpl::CountActiveSoundsOf(Device, Sound);
    UGameplayStatics::PlaySoundAtLocation(EditorWorld, Sound, Where, Facing, Volume, Pitch,
        /*StartTime=*/0.0f, /*AttenuationSettings=*/nullptr, /*ConcurrencySettings=*/nullptr);
    const int32 ActiveAfter = UEAgentAudioImpl::CountActiveSoundsOf(Device, Sound);
    const bool bCreated = ActiveAfter > ActiveBefore;
    if (!bCreated)
    {
        const TCHAR* LastCause = (bLooping && !bAudible)
            ? TEXT("or, being a looping sound out of earshot, the engine parked it as a virtual loop that starts when a listener comes in range and that nothing here can stop")
            : TEXT("or an audio debug filter excluded it");
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s was handed to the audio device but it holds no new active sound of it (%d before, %d after): the device did not start it. The sound is not playable (e.g. a cue with nothing wired to its output), a concurrency limit refused it, %s"),
            *Sound->GetName(), ActiveBefore, ActiveAfter, LastCause));
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("sound_path"), AssetPath);
    Report->SetStringField(TEXT("sound_name"), Sound->GetName());
    Report->SetObjectField(TEXT("location"), mcp_common::VectorToJsonObject(Where));
    Report->SetObjectField(TEXT("rotation"), mcp_common::RotatorToJsonObject(Facing));
    Report->SetNumberField(TEXT("volume"), Volume);
    Report->SetNumberField(TEXT("pitch"), Pitch);
    Report->SetNumberField(TEXT("audio_device_id"), DeviceId);
    Report->SetBoolField(TEXT("audible"), bAudible);
    Report->SetNumberField(TEXT("max_distance"), MaxDistance);
    Report->SetBoolField(TEXT("looping"), bLooping);
    Report->SetNumberField(TEXT("active_sounds_before"), ActiveBefore);
    Report->SetNumberField(TEXT("active_sounds_after"), ActiveAfter);
    Report->SetBoolField(TEXT("active_sound_created"), bCreated);
    Report->SetBoolField(TEXT("success"), bCreated);
    return Report;
}

// MCP:set_audio_sound - Sets the sound asset of an audio component of the editor world, found by object name (case-insensitive) among EVERY audio component of every actor, so an actor's second or third audio component is reached too; a name that components of several actors carry is an error listing the owners unless actor_name (object name, not label) picks one. Components in a running PIE world are not reached. A component that is playing restarts with the new sound unless it allows multiple instances. The change is not saved, marked dirty or undoable. A missing component or a sound that does not load is an error. previous_sound_name names the sound it replaced (None when there was none); audio_component_path and audio_owner_name name the component changed. Start playback with play_sound.
//   @param audio_component_name - Object name of the audio component to modify (e.g. "AudioComponent0"), compared without case against every audio component of every actor in the editor world.
//   @param sound_path - Full asset path to the sound (e.g. '/Game/Audio/MySound.MySound')
//   @param actor_name - Optional object name (not label) of the actor that owns the component, compared without case. Needed when several actors own an audio component of that name (every AmbientSound's is AudioComponent0); an empty one, one that names no actor, or one whose actor has no such component is an error.
//   @returns { audio_component_name: string, audio_component_path: string, audio_owner_name: string, sound_path: string, sound_name: string, previous_sound_name: string, sound_duration: number, success: bool }

TSharedPtr<FJsonObject> HandleSetAudioSound(const TSharedPtr<FJsonObject>& Params)
{
    FString Wanted;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("audio_component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString OwnerFilter;
    if (TSharedPtr<FJsonObject> BadOwner = UEAgentAudioImpl::ReadOwnerFilter(Params, OwnerFilter)) { return BadOwner; }
    UAudioComponent* AudioComp = nullptr;
    if (const TSharedPtr<FJsonObject> Failure = UEAgentAudioImpl::LocateAudioComponent(mcp_common::GetEditorWorld(), Wanted, OwnerFilter, AudioComp))
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
    UEAgentAudioImpl::AddComponentIdentity(Report, AudioComp);
    Report->SetStringField(TEXT("sound_path"), AssetPath);
    Report->SetStringField(TEXT("sound_name"), Sound->GetName());
    Report->SetStringField(TEXT("previous_sound_name"), Replaced ? Replaced->GetName() : FString(TEXT("None")));
    Report->SetNumberField(TEXT("sound_duration"), Sound->GetDuration());
    Report->SetBoolField(TEXT("success"), true);
    return Report;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("set_audio_3d_settings"), &HandleConfigureAudio3D },
    { TEXT("get_audio_component_properties"), &HandleGetAudioComponentProperties },
    { TEXT("get_audio_settings"), &HandleGetAudioSettings },
    { TEXT("play_sound"), &HandlePlaySound },
    { TEXT("play_sound_at_location"), &HandlePlaySoundAtLocation },
    { TEXT("set_audio_sound"), &HandleSetAudioSound },
    { TEXT("set_audio_volume"), &HandleSetAudioVolume },
    { TEXT("stop_sound"), &HandleStopSound },
};
} // namespace UEAgentAudioCommands
