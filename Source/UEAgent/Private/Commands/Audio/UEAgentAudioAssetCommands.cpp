#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Engine sound mix asset classes (all in Runtime/Engine, already a Build.cs dep).
#include "Sound/SoundWave.h"           // USoundWave (concrete) — CompressionQuality/SampleRateQuality/SoundGroup/SoundAssetCompressionType/bLooping/Volume/Pitch
#include "Sound/SoundBase.h"           // USoundBase (abstract base of USoundWave) — Priority/VirtualizationMode/SoundClassObject/AttenuationSettings
#include "Sound/SoundClass.h"          // USoundClass, FSoundClassProperties { Volume, Pitch }
#include "Sound/SoundMix.h"            // USoundMix
#include "Sound/SoundAttenuation.h"    // USoundAttenuation, FSoundAttenuationSettings : FBaseAttenuationSettings
#include "Sound/SoundConcurrency.h"    // USoundConcurrency, FSoundConcurrencySettings { MaxCount }
#include "Sound/SoundSubmix.h"         // USoundSubmix { SubmixEffectChain }
#include "Sound/SoundEffectSubmix.h"   // USoundEffectSubmixPreset (abstract base; concrete subclasses live in AudioMixer)
#include "Engine/Attenuation.h"        // FBaseAttenuationSettings { FalloffDistance, AttenuationShape, AttenuationShapeExtents }
#include "Sound/DialogueWave.h"        // UDialogueWave, FDialogueContextMapping (Engine module, MinimalAPI, non-Abstract)
#include "Sound/DialogueVoice.h"       // UDialogueVoice, EGrammaticalGender/EGrammaticalNumber (Engine module, MinimalAPI, non-Abstract)

#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"        // GetDerivedClasses (reflection lookup of concrete preset subclasses)
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "JsonObjectConverter.h"        // FJsonObjectConverter (UStruct<->JSON reflection)

/**
 * Sound asset authoring commands: sound classes and their hierarchy, mixes, attenuation, concurrency,
 * submixes and their effect chains, sound wave tunables, and dialogue waves and voices.
 *
 * All are plain Engine-module UObjects, created via NewObject + SavePackage (no modal factory) and
 * verified by reading the persisted properties back. Distinct from UEAgentAudioCommands, which works on
 * the audio components of the level.
 */
namespace UEAgentAudioAssetCommands
{

// MCP-DOMAIN: Audio

namespace
{
    // Create-or-load of the TAsset that the name and package_path (default /Game/Audio) params name: the existing
    // asset when there is one (idempotent, bOutAlreadyExists), else a new one registered with the asset registry. The
    // caller sets its properties and saves it. Null on success, else the error reply (TypeName names TAsset).
    template <typename TAsset>
    TSharedPtr<FJsonObject> AudioAsset_CreateOrLoad(const TSharedPtr<FJsonObject>& Params, const TCHAR* TypeName,
        FString& OutAssetName, FString& OutPackagePath, bool& bOutAlreadyExists, TAsset*& OutAsset)
    {
        UClass* AssetClass = TAsset::StaticClass();
        bOutAlreadyExists = false;

        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), OutAssetName)) { return Missing; }
        if (!Params->TryGetStringField(TEXT("package_path"), OutPackagePath) || OutPackagePath.IsEmpty())
        {
            OutPackagePath = TEXT("/Game/Audio");
        }

        const FString FullPath = FString::Printf(TEXT("%s/%s"), *OutPackagePath, *OutAssetName);
        if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

        UObject* Asset = nullptr;
        if (UEditorAssetLibrary::DoesAssetExist(FullPath))
        {
            Asset = StaticLoadObject(AssetClass, nullptr, *FullPath);
            if (!Asset)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Asset already exists at %s but could not be loaded as %s"),
                    *FullPath, *AssetClass->GetName()));
            }
            bOutAlreadyExists = true;
        }
        else
        {
            UPackage* Package = CreatePackage(*FullPath);
            if (!Package)
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create package: %s"), *FullPath));
            }
            Package->FullyLoad();

            Asset = NewObject<UObject>(
                Package, AssetClass, *OutAssetName,
                RF_Public | RF_Standalone | RF_Transactional);
            if (!Asset)
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Failed to create asset of class %s"), *AssetClass->GetName()));
            }
            FAssetRegistryModule::AssetCreated(Asset);
        }

        OutAsset = Cast<TAsset>(Asset);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Created object is not a %s"), TypeName));
        }
        return nullptr;
    }

    // A create reply that opens with the asset's name, content folder, path and class.
    TSharedPtr<FJsonObject> AudioAsset_StartCreateReply(const FString& AssetName, const FString& PackagePath,
        const UObject* Asset)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("name"), AssetName);
        ResultObj->SetStringField(TEXT("package_path"), PackagePath);
        ResultObj->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        ResultObj->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
        return ResultObj;
    }

    // The TAsset at Path. Null on success, else the error reply (TypeName names what was expected there).
    template <typename TAsset>
    TSharedPtr<FJsonObject> AudioAsset_Load(const FString& Path, const TCHAR* TypeName, TAsset*& OutAsset)
    {
        OutAsset = Cast<TAsset>(StaticLoadObject(TAsset::StaticClass(), nullptr, *Path));
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load %s at %s"), TypeName, *Path));
        }
        return nullptr;
    }

    // A reply that opens with the sound wave's path, class and own properties read back through reflection, plus
    // the bLooping bitfield, which may not serialize through it.
    TSharedPtr<FJsonObject> AudioAsset_StartWaveReply(USoundWave* Wave)
    {
        TSharedPtr<FJsonObject> SettingsOut = mcp_common::StructToJson(
            USoundWave::StaticClass(), Wave, 0, 0);
        SettingsOut->SetBoolField(TEXT("bLooping"), Wave->bLooping != 0);

        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("sound_wave_path"), Wave->GetPathName());
        ResultObj->SetStringField(TEXT("asset_class"), Wave->GetClass()->GetName());
        ResultObj->SetObjectField(TEXT("settings"), SettingsOut);
        return ResultObj;
    }

    // Find a concrete (non-abstract, non-deprecated) USoundEffectSubmixPreset subclass
    // via reflection. The concrete presets (e.g. USubmixEffectDynamicsProcessorPreset,
    // USubmixEffectSubmixEQPreset, USubmixEffectReverbPreset) live in the AudioMixer
    // runtime module, which Engine depends on, so they are always registered in an
    // editor session. We resolve by reflection to avoid a hard link dependency.
    //
    // Prefers a stable, settings-only preset (DynamicsProcessor) when present.
    UClass* FindConcreteSubmixEffectPresetClass()
    {
        UClass* BaseClass = USoundEffectSubmixPreset::StaticClass();
        if (!BaseClass)
        {
            return nullptr;
        }

        TArray<UClass*> Derived;
        GetDerivedClasses(BaseClass, Derived, /*bRecursive=*/true);

        UClass* Fallback = nullptr;
        for (UClass* Candidate : Derived)
        {
            if (!Candidate)
            {
                continue;
            }
            if (Candidate->HasAnyClassFlags(
                    CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            {
                continue;
            }
            // Skip transient/duplicate reinstanced classes from hot-reload.
            if (Candidate->GetName().StartsWith(TEXT("REINST_")) ||
                Candidate->GetName().StartsWith(TEXT("SKEL_")))
            {
                continue;
            }

            // Prefer the Dynamics Processor preset (deterministic default settings).
            if (Candidate->GetName() == TEXT("SubmixEffectDynamicsProcessorPreset"))
            {
                return Candidate;
            }
            if (!Fallback)
            {
                Fallback = Candidate;
            }
        }
        return Fallback;
    }
}

// ---------------------------------------------------------------------------- //
// create_sound_class
// ---------------------------------------------------------------------------- //
// MCP:create_sound_class - Create a USoundClass asset and optionally set its volume/pitch
//   @param name - Name of the new Sound Class asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param volume - Optional Volume multiplier on FSoundClassProperties
//   @param pitch - Optional Pitch multiplier on FSoundClassProperties
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, volume: number, pitch: number, applied_volume: bool, applied_pitch: bool, created: bool, already_exists: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateSoundClass(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    USoundClass* SoundClass = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("USoundClass"), AssetName, PackagePath, bAlreadyExists, SoundClass))
    {
        return Error;
    }

    // Apply optional volume / pitch to FSoundClassProperties Properties.
    bool bAppliedVolume = false;
    bool bAppliedPitch = false;
    double VolumeD = 0.0;
    if (Params->TryGetNumberField(TEXT("volume"), VolumeD))
    {
        SoundClass->Modify();
        SoundClass->Properties.Volume = static_cast<float>(VolumeD);
        bAppliedVolume = true;
    }
    double PitchD = 0.0;
    if (Params->TryGetNumberField(TEXT("pitch"), PitchD))
    {
        SoundClass->Modify();
        SoundClass->Properties.Pitch = static_cast<float>(PitchD);
        bAppliedPitch = true;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(SoundClass->GetOutermost(), SoundClass, SaveError);

    // Verify by readback.
    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartCreateReply(AssetName, PackagePath, SoundClass);
    ResultObj->SetNumberField(TEXT("volume"), SoundClass->Properties.Volume);
    ResultObj->SetNumberField(TEXT("pitch"), SoundClass->Properties.Pitch);
    ResultObj->SetBoolField(TEXT("applied_volume"), bAppliedVolume);
    ResultObj->SetBoolField(TEXT("applied_pitch"), bAppliedPitch);
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_sound_class"), SaveError);
}

// ---------------------------------------------------------------------------- //
// create_sound_mix
// ---------------------------------------------------------------------------- //
// MCP:create_sound_mix - Create a USoundMix asset and optionally set its EQ toggle / duration
//   @param name - Name of the new Sound Mix asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param eq_enabled - Optional bool, sets USoundMix::bApplyEQ (whether to apply the EQ effect). Left at the asset default when absent.
//   @param duration - Optional float, sets USoundMix::Duration in seconds (negative means applied until another mix is set). Left at the asset default when absent.
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, eq_enabled: bool, duration: number, applied_eq_enabled: bool, applied_duration: bool, created: bool, already_exists: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateSoundMix(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    USoundMix* SoundMix = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("USoundMix"), AssetName, PackagePath, bAlreadyExists, SoundMix))
    {
        return Error;
    }

    // Apply optional EQ toggle / duration onto the USoundMix. Only write each field
    // when its param is present so a bare create leaves the asset defaults intact.
    bool bAppliedEQ = false;
    bool bAppliedDuration = false;

    bool bApplyEQ = false;
    if (Params->TryGetBoolField(TEXT("eq_enabled"), bApplyEQ))
    {
        SoundMix->Modify();
        SoundMix->bApplyEQ = bApplyEQ ? 1 : 0;
        bAppliedEQ = true;
    }

    double DurationD = 0.0;
    if (Params->TryGetNumberField(TEXT("duration"), DurationD))
    {
        SoundMix->Modify();
        SoundMix->Duration = static_cast<float>(DurationD);
        bAppliedDuration = true;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(SoundMix->GetOutermost(), SoundMix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartCreateReply(AssetName, PackagePath, SoundMix);
    ResultObj->SetBoolField(TEXT("eq_enabled"), SoundMix->bApplyEQ != 0);
    ResultObj->SetNumberField(TEXT("duration"), SoundMix->Duration);
    ResultObj->SetBoolField(TEXT("applied_eq_enabled"), bAppliedEQ);
    ResultObj->SetBoolField(TEXT("applied_duration"), bAppliedDuration);
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_sound_mix"), SaveError);
}

// ---------------------------------------------------------------------------- //
// create_sound_attenuation
// ---------------------------------------------------------------------------- //
// MCP:create_sound_attenuation - Create a USoundAttenuation asset with attenuation enabled
//   @param name - Name of the new Sound Attenuation asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param falloff_distance - Optional FalloffDistance on FBaseAttenuationSettings
//   @param radius - Optional sphere radius applied to AttenuationShapeExtents X
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, attenuate: bool, falloff_distance: number, radius: number, shape_extent_x: number, shape_extent_y: number, shape_extent_z: number, applied_falloff_distance: bool, applied_radius: bool, created: bool, already_exists: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateSoundAttenuation(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    USoundAttenuation* Attenuation = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("USoundAttenuation"), AssetName, PackagePath, bAlreadyExists, Attenuation))
    {
        return Error;
    }

    // Enable distance-based volume attenuation by default (so this asset is functional),
    // then apply optional falloff_distance and radius (sphere radius -> X extent).
    Attenuation->Modify();
    Attenuation->Attenuation.bAttenuate = 1;

    bool bAppliedFalloff = false;
    bool bAppliedRadius = false;

    double FalloffD = 0.0;
    if (Params->TryGetNumberField(TEXT("falloff_distance"), FalloffD))
    {
        Attenuation->Attenuation.FalloffDistance = static_cast<float>(FalloffD);
        bAppliedFalloff = true;
    }

    double RadiusD = 0.0;
    if (Params->TryGetNumberField(TEXT("radius"), RadiusD))
    {
        // Sphere shape: X is the sphere radius, Y/Z unused.
        Attenuation->Attenuation.AttenuationShape = EAttenuationShape::Sphere;
        Attenuation->Attenuation.AttenuationShapeExtents =
            FVector(static_cast<float>(RadiusD), 0.0f, 0.0f);
        bAppliedRadius = true;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Attenuation->GetOutermost(), Attenuation, SaveError);

    // Verify by readback of the persisted settings.
    const FVector Extents = Attenuation->Attenuation.AttenuationShapeExtents;

    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartCreateReply(AssetName, PackagePath, Attenuation);
    ResultObj->SetBoolField(TEXT("attenuate"), Attenuation->Attenuation.bAttenuate != 0);
    ResultObj->SetNumberField(TEXT("falloff_distance"), Attenuation->Attenuation.FalloffDistance);
    ResultObj->SetNumberField(TEXT("radius"), Extents.X);
    ResultObj->SetNumberField(TEXT("shape_extent_x"), Extents.X);
    ResultObj->SetNumberField(TEXT("shape_extent_y"), Extents.Y);
    ResultObj->SetNumberField(TEXT("shape_extent_z"), Extents.Z);
    ResultObj->SetBoolField(TEXT("applied_falloff_distance"), bAppliedFalloff);
    ResultObj->SetBoolField(TEXT("applied_radius"), bAppliedRadius);
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_sound_attenuation"), SaveError);
}

// ---------------------------------------------------------------------------- //
// create_sound_concurrency
// ---------------------------------------------------------------------------- //
// MCP:create_sound_concurrency - Create a USoundConcurrency asset and optionally set MaxCount
//   @param name - Name of the new Sound Concurrency asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param max_count - Optional MaxCount on FSoundConcurrencySettings, clamped to >= 1
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, max_count: int, applied_max_count: bool, created: bool, already_exists: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateSoundConcurrency(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    USoundConcurrency* Concurrency = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("USoundConcurrency"), AssetName, PackagePath, bAlreadyExists, Concurrency))
    {
        return Error;
    }

    bool bAppliedMaxCount = false;
    double MaxCountD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_count"), MaxCountD))
    {
        Concurrency->Modify();
        // MaxCount is the public dynamic int32 on FSoundConcurrencySettings (clamp >= 1).
        Concurrency->Concurrency.MaxCount = FMath::Max(1, static_cast<int32>(MaxCountD));
        bAppliedMaxCount = true;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Concurrency->GetOutermost(), Concurrency, SaveError);

    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartCreateReply(AssetName, PackagePath, Concurrency);
    ResultObj->SetNumberField(TEXT("max_count"), Concurrency->Concurrency.MaxCount);
    ResultObj->SetBoolField(TEXT("applied_max_count"), bAppliedMaxCount);
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_sound_concurrency"), SaveError);
}

// ---------------------------------------------------------------------------- //
// create_sound_submix
// ---------------------------------------------------------------------------- //
// MCP:create_sound_submix - Create a USoundSubmix asset
//   @param name - Name of the new Sound Submix asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, created: bool, already_exists: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateSoundSubmix(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    USoundSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("USoundSubmix"), AssetName, PackagePath, bAlreadyExists, Submix))
    {
        return Error;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartCreateReply(AssetName, PackagePath, Submix);
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_sound_submix"), SaveError);
}

// ---------------------------------------------------------------------------- //
// add_submix_effect_to_chain
//
// Loads a USoundSubmix, resolves a USoundEffectSubmixPreset (loads an existing
// preset asset if effect_preset_path is given; else NewObjects a concrete preset
// subclass into the submix's package), appends it to SubmixEffectChain, and saves.
// ---------------------------------------------------------------------------- //
// MCP:add_submix_effect_to_chain - Append a submix effect preset to a USoundSubmix effect chain
//   @param submix_path - Full content path of the target USoundSubmix
//   @param effect_preset_path - Optional existing USoundEffectSubmixPreset to load
//   @param effect_preset_class - Optional concrete USoundEffectSubmixPreset subclass path to NewObject
//   @returns { submix_path: string, effect_name: string, effect_class: string, resolved_from: string, effect_added: bool, chain_length: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddSubmixEffectToChain(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }

    USoundSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SubmixPath, TEXT("USoundSubmix"), Submix)) { return Error; }

    // Resolve the preset: either an explicit asset path, an explicit class path, or
    // a reflection-discovered concrete subclass NewObject'd into the submix package.
    USoundEffectSubmixPreset* Preset = nullptr;
    FString ResolvedFrom;

    FString PresetPath;
    FString PresetClassPath;
    if (Params->TryGetStringField(TEXT("effect_preset_path"), PresetPath) && !PresetPath.IsEmpty())
    {
        Preset = Cast<USoundEffectSubmixPreset>(
            StaticLoadObject(USoundEffectSubmixPreset::StaticClass(), nullptr, *PresetPath));
        if (!Preset)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load USoundEffectSubmixPreset at %s"), *PresetPath));
        }
        ResolvedFrom = TEXT("preset_path");
    }
    else
    {
        // Determine the concrete preset class: explicit class path or reflection.
        UClass* PresetClass = nullptr;
        if (Params->TryGetStringField(TEXT("effect_preset_class"), PresetClassPath) && !PresetClassPath.IsEmpty())
        {
            PresetClass = UClass::TryFindTypeSlow<UClass>(PresetClassPath);
            if (!PresetClass)
            {
                PresetClass = LoadObject<UClass>(nullptr, *PresetClassPath);
            }
            if (!PresetClass || !PresetClass->IsChildOf(USoundEffectSubmixPreset::StaticClass()) ||
                PresetClass->HasAnyClassFlags(CLASS_Abstract))
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("effect_preset_class '%s' is not a concrete USoundEffectSubmixPreset subclass"), *PresetClassPath));
            }
            ResolvedFrom = TEXT("class_path");
        }
        else
        {
            PresetClass = FindConcreteSubmixEffectPresetClass();
            if (!PresetClass)
            {
                // HONEST: no concrete preset subclass is reachable in this headless
                // session. Report cleanly rather than faking an effect.
                return mcp_common::MakeErrorReply(
                    TEXT("No concrete USoundEffectSubmixPreset subclass is loadable in this session "
                         "(AudioMixer presets not registered); cannot create a submix effect preset"));
            }
            ResolvedFrom = TEXT("reflection");
        }

        // NewObject a fresh preset instance into the submix's package so it persists
        // alongside the submix asset.
        UPackage* SubmixPackage = Submix->GetOutermost();
        const FName PresetName = MakeUniqueObjectName(
            SubmixPackage, PresetClass, FName(*FString::Printf(TEXT("%s_Effect"), *Submix->GetName())));
        Preset = NewObject<USoundEffectSubmixPreset>(
            SubmixPackage, PresetClass, PresetName,
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Preset)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to NewObject preset of class %s"), *PresetClass->GetName()));
        }
        FAssetRegistryModule::AssetCreated(Preset);
    }

    Submix->Modify();
    Submix->SubmixEffectChain.Add(Preset);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetStringField(TEXT("effect_name"), Preset->GetName());
    ResultObj->SetStringField(TEXT("effect_class"), Preset->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("resolved_from"), ResolvedFrom);
    ResultObj->SetBoolField(TEXT("effect_added"), true);
    ResultObj->SetNumberField(TEXT("chain_length"), Submix->SubmixEffectChain.Num());

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_submix_effect_to_chain"), SaveError);
}

// ---------------------------------------------------------------------------- //
// get_submix_effect_chain  (REAL read)
// ---------------------------------------------------------------------------- //
// MCP:get_submix_effect_chain - Read the effect chain of a USoundSubmix
//   @param submix_path - Full content path of the target USoundSubmix
//   @returns { submix_path: string, effects: [object], chain_length: int }
TSharedPtr<FJsonObject> HandleGetSubmixEffectChain(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }

    USoundSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SubmixPath, TEXT("USoundSubmix"), Submix)) { return Error; }

    TArray<TSharedPtr<FJsonValue>> Effects;
    for (const TObjectPtr<USoundEffectSubmixPreset>& EffectPtr : Submix->SubmixEffectChain)
    {
        USoundEffectSubmixPreset* Effect = EffectPtr.Get();
        TSharedPtr<FJsonObject> EffectObj = MakeShared<FJsonObject>();
        if (Effect)
        {
            EffectObj->SetStringField(TEXT("name"), Effect->GetName());
            EffectObj->SetStringField(TEXT("class"), Effect->GetClass()->GetName());
            EffectObj->SetStringField(TEXT("path"), Effect->GetPathName());
        }
        else
        {
            EffectObj->SetStringField(TEXT("name"), TEXT("None"));
            EffectObj->SetStringField(TEXT("class"), TEXT("None"));
        }
        Effects.Add(MakeShared<FJsonValueObject>(EffectObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetArrayField(TEXT("effects"), Effects);
    ResultObj->SetNumberField(TEXT("chain_length"), Submix->SubmixEffectChain.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------- //
// set_sound_class_parent  (REAL hierarchy edit)
//
// Loads both USoundClass; adds the child to the parent's ChildClasses (no dups) and
// sets child->ParentClass = parent (via the exported editor SetParentClass, which
// keeps both sides consistent). Saves both packages.
// ---------------------------------------------------------------------------- //
// MCP:set_sound_class_parent - Wire a child USoundClass under a parent USoundClass
//   @param child_sound_class_path - Full content path of the child USoundClass
//   @param parent_sound_class_path - Full content path of the parent USoundClass
//   @returns { child: string, parent: string, child_path: string, parent_path: string, child_count: int, saved_parent: bool, saved_child: bool }
TSharedPtr<FJsonObject> HandleSetSoundClassParent(const TSharedPtr<FJsonObject>& Params)
{
    FString ChildPath, ParentPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("child_sound_class_path"), ChildPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_sound_class_path"), ParentPath)) { return Missing; }

    USoundClass* Child = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(ChildPath, TEXT("child USoundClass"), Child)) { return Error; }
    USoundClass* Parent = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(ParentPath, TEXT("parent USoundClass"), Parent)) { return Error; }
    if (Child == Parent)
    {
        return mcp_common::MakeErrorReply(TEXT("A sound class cannot be its own parent"));
    }

    Parent->Modify();
    Child->Modify();

    // SetParentClass (ENGINE_API, WITH_EDITOR) removes the child from its previous
    // owner and wires both sides (adds to Parent->ChildClasses, sets Child->ParentClass).
    Child->SetParentClass(Parent);

    // Defensive: guarantee both sides regardless of editor path (no dups).
    Child->ParentClass = Parent;
    Parent->ChildClasses.AddUnique(Child);

    FString SaveErrorParent, SaveErrorChild;
    const bool bSavedParent = mcp_common::SaveAssetPackage(Parent->GetOutermost(), Parent, SaveErrorParent);
    const bool bSavedChild = mcp_common::SaveAssetPackage(Child->GetOutermost(), Child, SaveErrorChild);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("child"), Child->GetName());
    ResultObj->SetStringField(TEXT("parent"), Parent->GetName());
    ResultObj->SetStringField(TEXT("child_path"), Child->GetPathName());
    ResultObj->SetStringField(TEXT("parent_path"), Parent->GetPathName());
    ResultObj->SetNumberField(TEXT("child_count"), Parent->ChildClasses.Num());
    ResultObj->SetBoolField(TEXT("saved_parent"), bSavedParent);
    ResultObj->SetBoolField(TEXT("saved_child"), bSavedChild);

    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------- //
// get_sound_class_hierarchy  (REAL read)
// ---------------------------------------------------------------------------- //
// MCP:get_sound_class_hierarchy - Read the hierarchy of a USoundClass
//   @param sound_class_path - Full content path of the USoundClass
//   @returns { name: string, sound_class_path: string, parent: string, parent_path: string, children: [string], child_count: int }
TSharedPtr<FJsonObject> HandleGetSoundClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_class_path"), ClassPath)) { return Missing; }

    USoundClass* SoundClass = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(ClassPath, TEXT("USoundClass"), SoundClass)) { return Error; }

    TArray<TSharedPtr<FJsonValue>> Children;
    for (const TObjectPtr<USoundClass>& ChildPtr : SoundClass->ChildClasses)
    {
        if (USoundClass* ChildClass = ChildPtr.Get())
        {
            Children.Add(MakeShared<FJsonValueString>(ChildClass->GetName()));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), SoundClass->GetName());
    ResultObj->SetStringField(TEXT("sound_class_path"), SoundClass->GetPathName());
    if (USoundClass* ParentClass = SoundClass->ParentClass.Get())
    {
        ResultObj->SetStringField(TEXT("parent"), ParentClass->GetName());
        ResultObj->SetStringField(TEXT("parent_path"), ParentClass->GetPathName());
    }
    else
    {
        ResultObj->SetStringField(TEXT("parent"), TEXT("None"));
    }
    ResultObj->SetArrayField(TEXT("children"), Children);
    ResultObj->SetNumberField(TEXT("child_count"), SoundClass->ChildClasses.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// <generated:property-handlers> -- DO NOT EDIT BY HAND. Source of truth:
// Tools/property_descriptors/{sound_concurrency,sound_attenuation}.json
// Regenerate: python Tools/integrate_property_handlers.py

namespace
{
    // The TAsset at Path, or nullptr with OutError set when none loads from there (ClassName names it in the error).
    template <typename TAsset>
    TAsset* PropertyHandlers_LoadAsset(const FString& Path, const TCHAR* ClassName, TSharedPtr<FJsonObject>& OutError)
    {
        TAsset* Asset = Cast<TAsset>(StaticLoadObject(TAsset::StaticClass(), nullptr, *Path));
        if (!Asset)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load %s at %s"), ClassName, *Path));
        }
        return Asset;
    }

    // The reply data of both handlers: the asset's path under PathKey, its class, and Settings -- the
    // SettingsStruct value it holds -- read back through reflection.
    TSharedPtr<FJsonObject> PropertyHandlers_SettingsData(
        const UObject* Asset, const TCHAR* PathKey, const UScriptStruct* SettingsStruct, const void* Settings)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(PathKey, Asset->GetPathName());
        Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
        Data->SetObjectField(TEXT("settings"), mcp_common::StructToJson(SettingsStruct, Settings));
        return Data;
    }

    // Merges the required settings param into Settings, the SettingsStruct value Asset holds: only the keys it
    // carries change, and MergeJsonWithClamp also enforces the ClampMin/ClampMax meta the JSON converter ignores.
    // Then saves Asset and replies with PropertyHandlers_SettingsData and the keys applied; the reply succeeds
    // only when the save reached disk (Command names the command in its error).
    TSharedPtr<FJsonObject> PropertyHandlers_MergeSettings(
        const TSharedPtr<FJsonObject>& Params,
        UObject* Asset,
        const TCHAR* PathKey,
        const UScriptStruct* SettingsStruct,
        void* Settings,
        const TCHAR* Command)
    {
        const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
        if (!Params->TryGetObjectField(TEXT("settings"), SettingsJson) || !SettingsJson || !SettingsJson->IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter: settings (object)"));
        }

        Asset->Modify();
        FString MergeError;
        if (!mcp_common::MergeJsonWithClamp(*SettingsJson, SettingsStruct, Settings, MergeError))
        {
            return mcp_common::MakeErrorReply(MergeError);
        }
        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);

        TArray<TSharedPtr<FJsonValue>> AppliedKeys;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SettingsJson)->Values)
        {
            AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
        TSharedPtr<FJsonObject> Data = PropertyHandlers_SettingsData(Asset, PathKey, SettingsStruct, Settings);
        Data->SetArrayField(TEXT("applied_keys"), AppliedKeys);
        return mcp_common::MakeSavedReply(Data, bSaved, Command, SaveError);
    }
}

namespace
{
    USoundConcurrency* PropertyHandlers_LoadSoundConcurrency(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
    {
        FString Path;
        if (!Params->TryGetStringField(TEXT("concurrency_path"), Path) || Path.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter: concurrency_path"));
            return nullptr;
        }
        return PropertyHandlers_LoadAsset<USoundConcurrency>(Path, TEXT("USoundConcurrency"), OutError);
    }
}

// MCP:set_sound_concurrency_properties - Author FSoundConcurrencySettings on a USoundConcurrency via reflection (partial merge + clamp).
//   @param concurrency_path - Full content path of the target USoundConcurrency.
//   @param settings - Object of FSoundConcurrencySettings fields to merge (maxCount(int>=1), bLimitToOwner(bool), resolutionRule(enum name), retriggerTime, volumeScale(0..1), volumeScaleMode(enum), bVolumeScaleCanRelease, volumeScaleAttackTime, volumeScaleReleaseTime, voiceStealReleaseTime); absent keys untouched.
//   @returns { concurrency_path: string, asset_class: string, settings: object, applied_keys: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundConcurrencyProperties(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    USoundConcurrency* Asset = PropertyHandlers_LoadSoundConcurrency(Params, Error);
    if (!Asset)
    {
        return Error;
    }
    return PropertyHandlers_MergeSettings(
        Params, Asset, TEXT("concurrency_path"), FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency,
        TEXT("set_sound_concurrency_properties"));
}

// MCP:get_sound_concurrency_properties - Read all FSoundConcurrencySettings fields from a USoundConcurrency.
//   @param concurrency_path - Full content path of the target USoundConcurrency.
//   @returns { concurrency_path: string, asset_class: string, settings: object }
TSharedPtr<FJsonObject> HandleGetSoundConcurrencyProperties(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    const USoundConcurrency* Asset = PropertyHandlers_LoadSoundConcurrency(Params, Error);
    if (!Asset)
    {
        return Error;
    }
    return mcp_common::MakeOkReply(PropertyHandlers_SettingsData(
        Asset, TEXT("concurrency_path"), FSoundConcurrencySettings::StaticStruct(), &Asset->Concurrency));
}

namespace
{
    USoundAttenuation* PropertyHandlers_LoadSoundAttenuation(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)
    {
        FString Path;
        if (!Params->TryGetStringField(TEXT("attenuation_path"), Path) || Path.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter: attenuation_path"));
            return nullptr;
        }
        return PropertyHandlers_LoadAsset<USoundAttenuation>(Path, TEXT("USoundAttenuation"), OutError);
    }
}

// MCP:set_sound_attenuation_properties - Author FSoundAttenuationSettings on a USoundAttenuation via reflection (partial merge + clamp).
//   @param attenuation_path - Full content path of the target USoundAttenuation.
//   @param settings - Object of FSoundAttenuationSettings/FBaseAttenuationSettings fields to merge; absent keys untouched. e.g. bAttenuate, bSpatialize, attenuationShape(Sphere/Capsule/Box/Cone), distanceAlgorithm, falloffDistance, dBAttenuationAtMax(-60..0), attenuationShapeExtents{x,y,z}, bAttenuateWithLPF, lPFFrequencyAtMin/Max, bEnableOcclusion, occlusionVolumeAttenuation, bEnableReverbSend, reverbWetLevelMin/Max. Nested plugin sub-objects (pluginSettings, audioLinkSettingsOverride) are NOT authored here.
//   @returns { attenuation_path: string, asset_class: string, settings: object, applied_keys: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundAttenuationProperties(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    USoundAttenuation* Asset = PropertyHandlers_LoadSoundAttenuation(Params, Error);
    if (!Asset)
    {
        return Error;
    }
    return PropertyHandlers_MergeSettings(
        Params, Asset, TEXT("attenuation_path"), FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation,
        TEXT("set_sound_attenuation_properties"));
}

// MCP:get_sound_attenuation_properties - Read all FSoundAttenuationSettings fields from a USoundAttenuation.
//   @param attenuation_path - Full content path of the target USoundAttenuation.
//   @returns { attenuation_path: string, asset_class: string, settings: object }
TSharedPtr<FJsonObject> HandleGetSoundAttenuationProperties(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Error;
    const USoundAttenuation* Asset = PropertyHandlers_LoadSoundAttenuation(Params, Error);
    if (!Asset)
    {
        return Error;
    }
    return mcp_common::MakeOkReply(PropertyHandlers_SettingsData(
        Asset, TEXT("attenuation_path"), FSoundAttenuationSettings::StaticStruct(), &Asset->Attenuation));
}

// </generated:property-handlers>

// ---------------------------------------------------------------------------- //
// set_sound_wave_properties  (REAL reflection-authored tunable edit)
//
// Loads a USoundWave (concrete) and partial-merges a JSON object of UPROPERTY
// fields into the Wave object ITSELF via FJsonObjectConverter (absent keys
// untouched — the reflection target is the object, not a sub-struct member),
// then re-applies ClampMin/ClampMax meta generically over USoundWave's own
// properties (the converter ignores clamp meta), and saves. Enum fields map by
// NAME automatically. The `bLooping` bitfield is authored manually via a
// dedicated `looping` bool param (bitfields don't round-trip through JSON).
// ---------------------------------------------------------------------------- //
// MCP:set_sound_wave_properties - Author USoundWave tunables on an imported sound wave via reflection (partial merge + clamp).
//   @param sound_wave_path - Full content path of the target USoundWave (e.g. /Game/Audio/MySound).
//   @param settings - Object of USoundWave/USoundBase UPROPERTY fields to merge; absent keys untouched. e.g. CompressionQuality(1..100), SampleRateQuality(Max/High/Medium/Low/Min), SoundGroup(SOUNDGROUP_Default/SOUNDGROUP_Effects/SOUNDGROUP_UI/SOUNDGROUP_Music/SOUNDGROUP_Voice), SoundAssetCompressionType(BinkAudio/ADPCM/PCM/Opus/PlatformSpecific/ProjectDefined), Volume(>=0), Pitch(0.125..4.0), Priority(0..100), VirtualizationMode(Disabled/PlayWhenSilent/Restart/SeekRestart), SoundClassObject(asset path), AttenuationSettings(asset path). Do NOT put `bLooping` here — use the `looping` param.
//   @param looping - Optional bool; sets the bLooping bitfield directly (when this sound is played directly, not via a cue).
//   @returns { sound_wave_path: string, asset_class: string, settings: object, applied_keys: [string], applied_looping: bool, looping: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundWaveProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SoundWavePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_wave_path"), SoundWavePath)) { return Missing; }

    USoundWave* Wave = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SoundWavePath, TEXT("USoundWave"), Wave)) { return Error; }

    const TSharedPtr<FJsonObject>* SettingsJsonPtr = nullptr;
    const bool bHasSettings =
        Params->TryGetObjectField(TEXT("settings"), SettingsJsonPtr) && SettingsJsonPtr && SettingsJsonPtr->IsValid();
    bool bHasLooping = false;
    bool bLooping = false;
    bHasLooping = Params->TryGetBoolField(TEXT("looping"), bLooping);

    if (!bHasSettings && !bHasLooping)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to set: provide a 'settings' object and/or a 'looping' bool"));
    }

    Wave->Modify();

    TArray<TSharedPtr<FJsonValue>> AppliedKeys;
    if (bHasSettings)
    {
        const TSharedPtr<FJsonObject>& SettingsJson = *SettingsJsonPtr;

        // The bLooping bitfield does not round-trip through JsonObjectToUStruct; it is
        // authored exclusively via the dedicated `looping` param. Drop it defensively
        // so a stray settings key can't half-write the bitfield.
        SettingsJson->RemoveField(TEXT("bLooping"));

        // Partial merge directly into the USoundWave object: only the keys present in
        // SettingsJson are written; every other property is left untouched.
        FString JsonErr;
        if (!mcp_common::JsonToStruct(
                SettingsJson,
                USoundWave::StaticClass(),
                Wave, JsonErr, 0, 0))
        {
            return mcp_common::MakeErrorReply(JsonErr);
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SettingsJson->Values)
        {
            AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
    }

    // bLooping is a uint8 : 1 bitfield — set it directly (idempotent fixed value).
    if (bHasLooping)
    {
        Wave->bLooping = bLooping ? 1 : 0;
    }

    // JsonObjectToUStruct ignores ClampMin/ClampMax meta, so re-apply it generically:
    // clamp every numeric property on USoundWave that declares a clamp range
    // (CompressionQuality 1..100, Volume >=0, Pitch 0.125..4.0, Priority 0..100, ...).
    mcp_common::ClampNumericByMeta(USoundWave::StaticClass(), Wave);

    UE_LOG(LogTemp, Verbose,
        TEXT("set_sound_wave_properties: %s applied %d setting key(s), looping=%s"),
        *Wave->GetPathName(), AppliedKeys.Num(), bHasLooping ? (bLooping ? TEXT("true") : TEXT("false")) : TEXT("(unchanged)"));

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Wave->GetOutermost(), Wave, SaveError);

    // Echo back the persisted properties.
    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartWaveReply(Wave);
    ResultObj->SetArrayField(TEXT("applied_keys"), AppliedKeys);
    ResultObj->SetBoolField(TEXT("applied_looping"), bHasLooping);
    ResultObj->SetBoolField(TEXT("looping"), Wave->bLooping != 0);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_sound_wave_properties"), SaveError);
}

// ---------------------------------------------------------------------------- //
// get_sound_wave_properties  (REAL read)
// ---------------------------------------------------------------------------- //
// MCP:get_sound_wave_properties - Read USoundWave tunables (the object's own UPROPERTY fields) from an imported sound wave.
//   @param sound_wave_path - Full content path of the target USoundWave.
//   @returns { sound_wave_path: string, asset_class: string, settings: object, looping: bool }
TSharedPtr<FJsonObject> HandleGetSoundWaveProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SoundWavePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_wave_path"), SoundWavePath)) { return Missing; }

    USoundWave* Wave = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SoundWavePath, TEXT("USoundWave"), Wave)) { return Error; }

    TSharedPtr<FJsonObject> ResultObj = AudioAsset_StartWaveReply(Wave);
    ResultObj->SetBoolField(TEXT("looping"), Wave->bLooping != 0);

    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------- //
// set_submix_effect_settings  (REAL reflection-authored preset Settings edit)
//
// add_submix_effect_to_chain appends an inert preset; nothing configures its
// Settings struct. This reaches the preset at SubmixEffectChain[effect_index]
// and partial-merges a JSON object into its 'Settings' FStructProperty via
// reflection — the struct type differs per concrete preset (it lives in the
// AudioMixer module), so we do NOT take an AudioMixer dependency and do NOT call
// the preset's typed AUDIOMIXER_API SetSettings; we discover the struct purely
// through reflection. Mirrors the sound-concurrency merge+clamp+save flow.
// ---------------------------------------------------------------------------- //
// MCP:set_submix_effect_settings - Configure a submix effect preset's Settings struct (by chain index) via reflection (partial merge + clamp).
//   @param submix_path - Full content path of the target USoundSubmix
//   @param effect_index - 0-based index into SubmixEffectChain (same order add_submix_effect_to_chain appended / get_submix_effect_chain reports)
//   @param settings - Object of fields to merge into the preset's Settings struct; absent keys untouched
//   @returns { submix_path: string, effect_index: int, preset_class: string, settings: object, saved: bool }
TSharedPtr<FJsonObject> HandleSetSubmixEffectSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }

    int32 EffectIndex = 0;
    if (!Params->TryGetNumberField(TEXT("effect_index"), EffectIndex))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: effect_index (int)"));
    }

    const TSharedPtr<FJsonObject>* SettingsJsonPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("settings"), SettingsJsonPtr) || !SettingsJsonPtr || !SettingsJsonPtr->IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: settings (object)"));
    }
    const TSharedPtr<FJsonObject>& SettingsJson = *SettingsJsonPtr;

    USoundSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SubmixPath, TEXT("USoundSubmix"), Submix)) { return Error; }

    // Honest bounds check against the live chain length.
    if (EffectIndex < 0 || EffectIndex >= Submix->SubmixEffectChain.Num())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("effect_index %d out of range; SubmixEffectChain has %d entr%s"),
                EffectIndex, Submix->SubmixEffectChain.Num(),
                Submix->SubmixEffectChain.Num() == 1 ? TEXT("y") : TEXT("ies")));
    }

    UObject* Preset = Submix->SubmixEffectChain[EffectIndex].Get();
    if (!Preset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Submix effect preset at index %d is null"), EffectIndex));
    }

    // Reach the Settings struct GENERICALLY: the concrete struct type differs per
    // preset class, so resolve it via reflection rather than a typed member.
    FStructProperty* SettingsProp = CastField<FStructProperty>(
        Preset->GetClass()->FindPropertyByName(TEXT("Settings")));
    if (!SettingsProp)
    {
        // HONEST GATE: this preset class exposes no 'Settings' FStructProperty.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Preset class %s has no 'Settings' FStructProperty"),
                *Preset->GetClass()->GetName()));
    }
    UScriptStruct* SettingsStruct = SettingsProp->Struct;
    void* SettingsData = SettingsProp->ContainerPtrToValuePtr<void>(Preset);

    UE_LOG(LogTemp, Verbose,
        TEXT("set_submix_effect_settings: submix=%s index=%d preset=%s settingsStruct=%s"),
        *Submix->GetPathName(), EffectIndex, *Preset->GetClass()->GetName(),
        *SettingsStruct->GetName());

    Submix->Modify();
    Preset->Modify();

    // Partial merge: only keys present in SettingsJson are written; the rest of the
    // Settings struct is left untouched. MergeJsonWithClamp re-applies the
    // ClampMin/ClampMax UPROPERTY meta the converter ignores (struct-agnostic).
    FString JsonErr;
    if (!mcp_common::MergeJsonWithClamp(
            SettingsJson, SettingsStruct, SettingsData, JsonErr, 0, 0))
    {
        return mcp_common::MakeErrorReply(JsonErr);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    // Read-back the persisted Settings via reflection.
    TSharedPtr<FJsonObject> SettingsOut = mcp_common::StructToJson(
        SettingsStruct, SettingsData, 0, 0);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetNumberField(TEXT("effect_index"), EffectIndex);
    ResultObj->SetStringField(TEXT("preset_class"), Preset->GetClass()->GetName());
    ResultObj->SetObjectField(TEXT("settings"), SettingsOut);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_submix_effect_settings"), SaveError);
}

// ---------------------------------------------------------------------------- //
// create_dialogue_wave
//
// UDialogueWave is a plain Engine UObject (not a UDataAsset, no reflected
// constructor), so create_data_asset cannot make one — this is its own thin
// NewObject + SavePackage wrapper, reusing the shared AudioAsset_CreateOrLoad
// helper above (idempotent find-or-create by class). ContextMappings (an
// EditAnywhere TArray<FDialogueContextMapping>) is intentionally NOT populated
// here — author it afterwards via the generic set_properties command on the
// returned object_path (UE 5.8 DialogueWave.h confirms EditAnywhere).
// Note the engine constructor seeds ContextMappings with one default entry.
// ---------------------------------------------------------------------------- //
// MCP:create_dialogue_wave - Create a UDialogueWave asset (idempotent). Populate ContextMappings afterwards via set_properties on object_path.
//   @param name - Name of the new DialogueWave asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @returns { name: string, package_path: string, asset_path: string, object_path: string, asset_class: string, context_mapping_count: int, already_existed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateDialogueWave(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    UDialogueWave* DialogueWave = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("UDialogueWave"), AssetName, PackagePath, bAlreadyExists, DialogueWave))
    {
        return Error;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DialogueWave->GetOutermost(), DialogueWave, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("asset_path"), DialogueWave->GetPathName());
    ResultObj->SetStringField(TEXT("object_path"), DialogueWave->GetPathName());
    ResultObj->SetStringField(TEXT("asset_class"), DialogueWave->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("context_mapping_count"), DialogueWave->ContextMappings.Num());
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_dialogue_wave"), SaveError);
}

// ---------------------------------------------------------------------------- //
// create_dialogue_voice
//
// UDialogueVoice is likewise a plain Engine UObject. Its only editable business
// properties are Gender/Plurality (TEnumAsByte enums, EditAnywhere) — authored
// afterwards via set_properties on the returned object_path if needed.
// LocalizationGUID has no edit specifier (not CPF_Edit) and is auto-generated by
// the engine constructor, so it is intentionally not exposed for authoring here.
// ---------------------------------------------------------------------------- //
// MCP:create_dialogue_voice - Create a UDialogueVoice asset (idempotent). Populate Gender/Plurality afterwards via set_properties on object_path.
//   @param name - Name of the new DialogueVoice asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @returns { name: string, package_path: string, asset_path: string, object_path: string, asset_class: string, gender: string, plurality: string, already_existed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateDialogueVoice(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName, PackagePath;
    bool bAlreadyExists = false;
    UDialogueVoice* DialogueVoice = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_CreateOrLoad(
            Params, TEXT("UDialogueVoice"), AssetName, PackagePath, bAlreadyExists, DialogueVoice))
    {
        return Error;
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DialogueVoice->GetOutermost(), DialogueVoice, SaveError);

    const UEnum* GenderEnum = StaticEnum<EGrammaticalGender::Type>();
    const UEnum* PluralityEnum = StaticEnum<EGrammaticalNumber::Type>();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), AssetName);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("asset_path"), DialogueVoice->GetPathName());
    ResultObj->SetStringField(TEXT("object_path"), DialogueVoice->GetPathName());
    ResultObj->SetStringField(TEXT("asset_class"), DialogueVoice->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("gender"), GenderEnum
        ? GenderEnum->GetNameStringByValue(static_cast<int64>(DialogueVoice->Gender.GetValue()))
        : TEXT("Unknown"));
    ResultObj->SetStringField(TEXT("plurality"), PluralityEnum
        ? PluralityEnum->GetNameStringByValue(static_cast<int64>(DialogueVoice->Plurality.GetValue()))
        : TEXT("Unknown"));
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExists);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_dialogue_voice"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_submix_effect_to_chain"), &HandleAddSubmixEffectToChain },
    { TEXT("create_dialogue_voice"), &HandleCreateDialogueVoice },
    { TEXT("create_dialogue_wave"), &HandleCreateDialogueWave },
    { TEXT("create_sound_attenuation"), &HandleCreateSoundAttenuation },
    { TEXT("create_sound_class"), &HandleCreateSoundClass },
    { TEXT("create_sound_concurrency"), &HandleCreateSoundConcurrency },
    { TEXT("create_sound_mix"), &HandleCreateSoundMix },
    { TEXT("create_sound_submix"), &HandleCreateSoundSubmix },
    { TEXT("get_sound_attenuation_properties"), &HandleGetSoundAttenuationProperties },
    { TEXT("get_sound_class_hierarchy"), &HandleGetSoundClassHierarchy },
    { TEXT("get_sound_concurrency_properties"), &HandleGetSoundConcurrencyProperties },
    { TEXT("get_sound_wave_properties"), &HandleGetSoundWaveProperties },
    { TEXT("get_submix_effect_chain"), &HandleGetSubmixEffectChain },
    { TEXT("set_sound_attenuation_properties"), &HandleSetSoundAttenuationProperties },
    { TEXT("set_sound_class_parent"), &HandleSetSoundClassParent },
    { TEXT("set_sound_concurrency_properties"), &HandleSetSoundConcurrencyProperties },
    { TEXT("set_sound_wave_properties"), &HandleSetSoundWaveProperties },
    { TEXT("set_submix_effect_settings"), &HandleSetSubmixEffectSettings },
};
} // namespace UEAgentAudioAssetCommands
