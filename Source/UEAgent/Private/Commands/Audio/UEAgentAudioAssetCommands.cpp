// Copyright (c) 2026 CarlxsMG.

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
#include "UObject/StructOnScope.h"      // FStructOnScope (the scratch copy a settings merge runs on)
#include "UObject/UnrealType.h"         // FProperty / FNumericProperty (staged sound wave properties)
#include "UObject/EnumProperty.h"       // FEnumProperty (the JSON kind a settings key of an enum must hold)
#include "ScopedTransaction.h"          // remove_submix_effect_from_chain: one undo step

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

    // The values of one settings object staged for a write into an object: each converted into a copy of the current
    // value of the property its key names. The copies are freed with it.
    struct FAudioAssetStagedProperties
    {
        TArray<TPair<FProperty*, void*>> Values;

        FAudioAssetStagedProperties() = default;
        FAudioAssetStagedProperties(const FAudioAssetStagedProperties&) = delete;
        FAudioAssetStagedProperties& operator=(const FAudioAssetStagedProperties&) = delete;
        ~FAudioAssetStagedProperties()
        {
            for (const TPair<FProperty*, void*>& Value : Values)
            {
                Value.Key->DestroyAndFreeValue(Value.Value);
            }
        }
    };

    // Value, a value of Property, clamped to Property's ClampMin/ClampMax meta, which FJsonObjectConverter ignores:
    // what mcp_common::ClampNumericByMeta does to each property of a container.
    void AudioAsset_ClampToMeta(const FProperty& Property, void* Value)
    {
        const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
        const bool bHasMin = Property.HasMetaData(TEXT("ClampMin"));
        const bool bHasMax = Property.HasMetaData(TEXT("ClampMax"));
        if (!Numeric || (!bHasMin && !bHasMax))
        {
            return;
        }
        if (Numeric->IsFloatingPoint())
        {
            double Number = Numeric->GetFloatingPointPropertyValue(Value);
            if (bHasMin) { Number = FMath::Max(Number, FCString::Atod(*Property.GetMetaData(TEXT("ClampMin")))); }
            if (bHasMax) { Number = FMath::Min(Number, FCString::Atod(*Property.GetMetaData(TEXT("ClampMax")))); }
            Numeric->SetFloatingPointPropertyValue(Value, Number);
        }
        else if (Numeric->IsInteger())
        {
            int64 Number = Numeric->GetSignedIntPropertyValue(Value);
            if (bHasMin) { Number = FMath::Max<int64>(Number, FCString::Atoi64(*Property.GetMetaData(TEXT("ClampMin")))); }
            if (bHasMax) { Number = FMath::Min<int64>(Number, FCString::Atoi64(*Property.GetMetaData(TEXT("ClampMax")))); }
            Numeric->SetIntPropertyValue(Value, Number);
        }
    }

    // What Value must be for Property when it is not that, else empty. FJsonObjectConverter reads a number or a bool
    // out of any JSON value without an error -- a string that is not a number becomes 0, any string a bool -- so
    // those kinds are checked here; the converter refuses a wrong kind for the others itself.
    FString AudioAsset_ExpectedJsonKind(const FProperty& Property, const FJsonValue& Value)
    {
        if (Property.ArrayDim != 1)
        {
            return FString();
        }
        const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
        if (CastField<FEnumProperty>(&Property) || (Numeric && Numeric->IsEnum()))
        {
            return Value.Type == EJson::String || Value.Type == EJson::Number ? FString() : FString(TEXT("an enum name"));
        }
        if (Numeric)
        {
            const bool bNumber = Value.Type == EJson::Number || (Value.Type == EJson::String && Value.AsString().IsNumeric());
            return bNumber ? FString() : FString(TEXT("a number"));
        }
        if (CastField<FBoolProperty>(&Property))
        {
            return Value.Type == EJson::Boolean ? FString() : FString(TEXT("true or false"));
        }
        return FString();
    }

    // Stages every key of Json for a write into Object, before anything is written (Param names the param in the
    // errors). Null when each key names a property of Object's class -- matched case-insensitively, as
    // FJsonObjectConverter matches them -- that is neither deprecated nor holds instanced subobjects (the staging
    // conversion would create those outside Object), holds a value of its kind (AudioAsset_ExpectedJsonKind), and the
    // value converts; it is then clamped (AudioAsset_ClampToMeta). Else the error reply naming the key, with nothing
    // changed: merging into Object directly would skip a key that names no property without a word, and stop at
    // the first value that does not convert with the keys before it already written.
    TSharedPtr<FJsonObject> AudioAsset_StageProperties(const FJsonObject& Json, UObject& Object, const TCHAR* Param,
        FAudioAssetStagedProperties& OutStaged)
    {
        const UClass* Class = Object.GetClass();
        auto IsWritable = [](const FProperty& Property)
        {
            return !Property.HasAnyPropertyFlags(
                CPF_Deprecated | CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
        };
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json.Values)
        {
            FProperty* Property = nullptr;
            for (TFieldIterator<FProperty> It(Class); It && !Property; ++It)
            {
                if (Class->GetAuthoredNameForField(*It).Equals(Pair.Key, ESearchCase::IgnoreCase))
                {
                    Property = *It;
                }
            }
            if (!Property || !IsWritable(*Property))
            {
                TArray<FString> Names;
                for (TFieldIterator<FProperty> It(Class); It; ++It)
                {
                    if (IsWritable(**It))
                    {
                        Names.Add(Class->GetAuthoredNameForField(*It));
                    }
                }
                const FString Problem = Property
                    ? FString::Printf(TEXT("names %s, which is deprecated or holds instanced subobjects and is not written here"), *Property->GetName())
                    : FString::Printf(TEXT("names no property of %s"), *Class->GetName());
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s key '%s' %s; nothing was changed. Writable properties (keys match them case-insensitively): %s"),
                    Param, *Pair.Key, *Problem, *FString::Join(Names, TEXT(", "))));
            }
            if (!Pair.Value.IsValid() || Pair.Value->IsNull())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s.%s is null: give it a value or leave the key out; nothing was changed."), Param, *Pair.Key));
            }
            const FString Expected = AudioAsset_ExpectedJsonKind(*Property, *Pair.Value);
            if (!Expected.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s.%s must be %s for %s (%s); nothing was changed."),
                    Param, *Pair.Key, *Expected, *Property->GetName(), *Property->GetCPPType()));
            }
            void* Value = Property->AllocateAndInitializeValue();
            OutStaged.Values.Emplace(Property, Value);
            Property->CopyCompleteValue(Value, Property->ContainerPtrToValuePtr<void>(&Object));
            FText Reason;
            if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, Value, 0, 0, false, &Reason))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s.%s does not convert to %s (%s): %s; nothing was changed."), Param, *Pair.Key,
                    *Property->GetName(), *Property->GetCPPType(), *Reason.ToString().Replace(TEXT("\n"), TEXT(" "))));
            }
            AudioAsset_ClampToMeta(*Property, Value);
        }
        return nullptr;
    }

    // Whether Object already holds Value, a value of Property, in every element of it.
    bool AudioAsset_HoldsValue(const FProperty& Property, const UObject& Object, const void* Value)
    {
        for (int32 Index = 0; Index < Property.ArrayDim; ++Index)
        {
            if (!Property.Identical(Property.ContainerPtrToValuePtr<void>(&Object, Index),
                    static_cast<const uint8*>(Value) + Index * Property.GetElementSize(), PPF_None))
            {
                return false;
            }
        }
        return true;
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
// MCP:create_sound_class - Create a USoundClass asset at package_path/name (package_path defaults to /Game/Audio) and save it. A sound class already at that path is loaded instead (already_exists true, created false) and the volume and pitch you pass overwrite its values before it is saved again; omitted ones keep their current values, the engine defaults on a new class. Idempotent. The reply reads volume and pitch back from the asset, and applied_volume / applied_pitch say which were written. An invalid path, or an asset of another class there, is an error; a failed save (for example during PIE) makes success false with saved=false and save_error. Edit the other properties with set_properties, read them with get_sound_class_properties, and wire the hierarchy with set_sound_class_parent.
//   @param name - Name of the new Sound Class asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param volume - Optional Volume multiplier on FSoundClassProperties; also written to a class that already exists, unchanged when omitted
//   @param pitch - Optional Pitch multiplier on FSoundClassProperties; also written to a class that already exists, unchanged when omitted
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, volume: number, pitch: number, applied_volume: bool, applied_pitch: bool, created: bool, already_exists: bool, saved: bool, save_error?: string }
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
// MCP:create_sound_mix - Create a USoundMix asset at package_path/name (package_path defaults to /Game/Audio) and save it; a sound mix already there is loaded instead (already_exists true) and the eq_enabled and duration you pass overwrite its values before it is saved. Omitted fields keep their current values; the reply reads eq_enabled and duration back from the asset. An invalid path, or an asset of another class there, is an error; a failed save is an error with the change kept in memory. Idempotent. Add per-class volume and pitch adjusters with set_sound_mix_class_override; create the classes with create_sound_class.
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
// MCP:create_sound_attenuation - Create a USoundAttenuation asset at package_path/name, or load the one already there, then force bAttenuate on, apply the optional falloff_distance and sphere radius, and save it (re-running edits the existing asset). Returns the values read back; set_properties edits every other field.
//   @param name - Name of the new Sound Attenuation asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param falloff_distance - FalloffDistance in Unreal units (cm): how far beyond the inner shape the volume fades out. Unchanged when omitted.
//   @param radius - Inner sphere radius in Unreal units (cm); sets the shape to Sphere and AttenuationShapeExtents to (radius, 0, 0). Unchanged when omitted.
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
// MCP:create_sound_concurrency - Creates a Sound Concurrency asset (USoundConcurrency, the rule that caps how many sounds of a group play at once) at package_path/name, or loads the one already there instead of making a duplicate (created and already_exists say which), then saves it; success is false, with save_error, when the save fails. With max_count it also sets MaxCount, which on an existing asset overwrites the old value, as one undo step; without it an existing asset is left as it was. Idempotent: the same call again gives the same asset and value. An invalid path, or an asset of another class at that path, is an error. The other concurrency settings (resolution rule, volume scaling, retrigger time) are edited with set_properties on the asset_path this returns, and read with get_properties; sibling audio assets come from create_sound_class, create_sound_attenuation and create_sound_mix.
//   @param name - Asset name of the Sound Concurrency, without a folder (e.g. "SCon_Footsteps").
//   @param package_path - Content folder for the asset, e.g. "/Game/Audio/Concurrency"; defaults to /Game/Audio.
//   @param max_count - Optional whole number: most sounds of this group that play at once (MaxCount); values below 1 become 1 and fractions are truncated. Unchanged when omitted.
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
// MCP:create_sound_submix - Create an empty USoundSubmix asset at package_path/name, or load the one already there, and save it. Returns asset_path, created, already_exists and saved; add effects with add_submix_effect_to_chain and read them with get_submix_effect_chain.
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
// MCP:add_submix_effect_to_chain - Append one effect preset to the end of a USoundSubmix's effect chain and save the submix. effect_preset_path wins: that existing USoundEffectSubmixPreset asset is appended by reference. Otherwise a new preset of effect_preset_class, or, when neither is given, of a concrete class found by reflection (SubmixEffectDynamicsProcessorPreset when it is loaded), is created inside the submix's own package under a unique <Submix>_Effect name; resolved_from says which of the three ran. Every call appends another entry, duplicates included. A submix or preset that does not load, or a class that is not a concrete preset subclass, is an error; a failed save makes success false with saved=false and save_error. Read the chain with get_submix_effect_chain and tune an entry with set_submix_effect_settings; a soundfield submix takes add_soundfield_effect_to_submix. Inverse: remove_submix_effect_from_chain.
//   @param submix_path - Full content path of the target USoundSubmix
//   @param effect_preset_path - Optional existing USoundEffectSubmixPreset asset to append by reference; wins over effect_preset_class
//   @param effect_preset_class - Optional concrete USoundEffectSubmixPreset subclass (short name or path) to instantiate in the submix's package; with neither preset param a class is picked by reflection
//   @returns { submix_path: string, effect_name: string, effect_class: string, resolved_from: string, effect_added: bool, chain_length: int, saved: bool, save_error?: string }
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

// MCP:remove_submix_effect_from_chain - Remove the entry at effect_index from a USoundSubmix's effect chain, in one undo step, notify the submix of the edit (as the Details panel does, so a running audio device rebuilds the chain) and save the submix. Inverse of add_submix_effect_to_chain; the later entries move up one index. The preset itself is not deleted: a preset add_submix_effect_to_chain made inside the submix package stays in it unreferenced, one appended by path stays where it was. An index out of range or a submix that does not load is an error and nothing changes; a failed save makes success false with save_error. Read the chain first with get_submix_effect_chain; a soundfield submix's effects: remove_soundfield_effect.
//   @param submix_path - Full content path of the target USoundSubmix
//   @param effect_index [int] - 0-based position in the chain, as get_submix_effect_chain lists it
//   @returns { submix_path: string, effect_index: int, removed_effect: string, removed_effect_class: string, removed: bool, chain_length: int, chain: [string], saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleRemoveSubmixEffectFromChain(const TSharedPtr<FJsonObject>& Params)
{
    FString SubmixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("submix_path"), SubmixPath)) { return Missing; }
    int32 EffectIndex = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("effect_index"), EffectIndex)) { return Missing; }

    USoundSubmix* Submix = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SubmixPath, TEXT("USoundSubmix"), Submix)) { return Error; }
    if (!Submix->SubmixEffectChain.IsValidIndex(EffectIndex))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("effect_index %d is out of range: the chain has %d entries; nothing was changed"), EffectIndex, Submix->SubmixEffectChain.Num()));
    }

    const USoundEffectSubmixPreset* Removed = Submix->SubmixEffectChain[EffectIndex];
    const FString RemovedName = Removed ? Removed->GetPathName() : FString();
    const FString RemovedClass = Removed ? Removed->GetClass()->GetName() : FString();

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveSubmixEffect", "MCP: Remove Submix Effect"));
    Submix->Modify();
    Submix->SubmixEffectChain.RemoveAt(EffectIndex);
    if (FProperty* ChainProperty = USoundSubmix::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(USoundSubmix, SubmixEffectChain)))
    {
        FPropertyChangedEvent ChangedEvent(ChainProperty, EPropertyChangeType::ArrayRemove);
        // PostEditChangeProperty is protected on USoundSubmix; UObject declares it public.
        static_cast<UObject*>(Submix)->PostEditChangeProperty(ChangedEvent);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    TArray<FString> Chain;
    for (const TObjectPtr<USoundEffectSubmixPreset>& Entry : Submix->SubmixEffectChain)
    {
        Chain.Add(Entry ? Entry->GetName() : FString(TEXT("None")));
    }
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetNumberField(TEXT("effect_index"), EffectIndex);
    ResultObj->SetStringField(TEXT("removed_effect"), RemovedName);
    ResultObj->SetStringField(TEXT("removed_effect_class"), RemovedClass);
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetNumberField(TEXT("chain_length"), Submix->SubmixEffectChain.Num());
    ResultObj->SetArrayField(TEXT("chain"), mcp_common::StringsToJson(Chain));
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_submix_effect_from_chain"), SaveError);
}

// ---------------------------------------------------------------------------- //
// get_submix_effect_chain  (REAL read)
// ---------------------------------------------------------------------------- //
// MCP:get_submix_effect_chain - Read the effect chain of a USoundSubmix asset in order: one entry per slot with the name, class and path of its USoundEffectSubmixPreset ("None" for an empty slot), plus chain_length. Read-only; add_submix_effect_to_chain appends to it.
//   @param submix_path - Asset path of the USoundSubmix, e.g. "/Game/Audio/SM_Master.SM_Master".
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
// MCP:set_sound_class_parent - Make one USoundClass the child of another, wiring ParentClass and ChildClasses, and save both packages. A previous parent loses the child in memory but is not saved; only self-parenting is refused, not cycles. Returns child_count, saved_parent and saved_child, succeeding even when a save fails.
//   @param child_sound_class_path - Asset path of the USoundClass to re-parent, e.g. "/Game/Audio/SC_Footsteps.SC_Footsteps".
//   @param parent_sound_class_path - Asset path of the USoundClass that becomes its parent, e.g. "/Game/Audio/SC_SFX.SC_SFX".
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

    // What Value must be for Field when it is not that, else empty. FJsonObjectConverter reads a number or a bool out
    // of any JSON value without an error -- a string that is not a number becomes 0, any string a bool -- so those
    // kinds are checked here; the converter refuses a wrong kind for the others itself.
    FString PropertyHandlers_ExpectedJsonKind(const FProperty& Field, const FJsonValue& Value)
    {
        if (Field.ArrayDim != 1)
        {
            return FString();
        }
        const FNumericProperty* Numeric = CastField<FNumericProperty>(&Field);
        if (CastField<FEnumProperty>(&Field) || (Numeric && Numeric->IsEnum()))
        {
            return Value.Type == EJson::String || Value.Type == EJson::Number ? FString() : FString(TEXT("an enum name"));
        }
        if (Numeric)
        {
            const bool bNumber = Value.Type == EJson::Number || (Value.Type == EJson::String && Value.AsString().IsNumeric());
            return bNumber ? FString() : FString(TEXT("a number"));
        }
        if (CastField<FBoolProperty>(&Field))
        {
            return Value.Type == EJson::Boolean ? FString() : FString(TEXT("true or false"));
        }
        return FString();
    }

    // Null when every key of Json names a field of SettingsStruct that is not deprecated -- matched case-insensitively,
    // as FJsonObjectConverter matches them -- and holds a value of its kind (PropertyHandlers_ExpectedJsonKind); else
    // the error reply naming the first key that does not, with the fields. The converter skips a key that names no
    // field without a word, so it would read as applied.
    TSharedPtr<FJsonObject> PropertyHandlers_CheckSettingsKeys(const FJsonObject& Json, const UScriptStruct* SettingsStruct)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json.Values)
        {
            const FProperty* Field = nullptr;
            for (TFieldIterator<FProperty> It(SettingsStruct); It && !Field; ++It)
            {
                if (!It->HasAnyPropertyFlags(CPF_Deprecated)
                    && SettingsStruct->GetAuthoredNameForField(*It).Equals(Pair.Key, ESearchCase::IgnoreCase))
                {
                    Field = *It;
                }
            }
            if (!Field)
            {
                TArray<FString> Fields;
                for (TFieldIterator<FProperty> It(SettingsStruct); It; ++It)
                {
                    if (!It->HasAnyPropertyFlags(CPF_Deprecated))
                    {
                        Fields.Add(SettingsStruct->GetAuthoredNameForField(*It));
                    }
                }
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("settings key '%s' matches no field of %s (keys are field names, matched case-insensitively); ")
                    TEXT("nothing was changed. Fields: %s"),
                    *Pair.Key, *SettingsStruct->GetName(), *FString::Join(Fields, TEXT(", "))));
            }
            if (!Pair.Value.IsValid() || Pair.Value->IsNull())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("settings.%s is null: give it a value or leave the key out; nothing was changed."), *Pair.Key));
            }
            const FString Expected = PropertyHandlers_ExpectedJsonKind(*Field, *Pair.Value);
            if (!Expected.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("settings.%s must be %s for %s (%s); nothing was changed."),
                    *Pair.Key, *Expected, *Field->GetName(), *Field->GetCPPType()));
            }
        }
        return nullptr;
    }

    // Merges the required settings param into Settings, the SettingsStruct value Asset holds: only the keys it
    // carries change, and MergeJsonWithClamp also enforces the ClampMin/ClampMax meta the JSON converter ignores.
    // An empty settings object, a key PropertyHandlers_CheckSettingsKeys refuses or a value that does not convert
    // is an error, with nothing changed. Then saves Asset and replies with PropertyHandlers_SettingsData and the
    // keys applied; the reply succeeds only when the save reached disk (Command names the command in its error).
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
        if ((*SettingsJson)->Values.Num() == 0)
        {
            return mcp_common::MakeErrorReply(TEXT("settings is empty: give at least one field to set; nothing was changed."));
        }
        if (TSharedPtr<FJsonObject> Refused = PropertyHandlers_CheckSettingsKeys(**SettingsJson, SettingsStruct))
        {
            return Refused;
        }

        // The merge runs on a copy that replaces Settings only once all of it converted: the converter writes field
        // by field and stops at the first value that does not convert, which would leave the asset half-written.
        FStructOnScope Merged(SettingsStruct);
        SettingsStruct->CopyScriptStruct(Merged.GetStructMemory(), Settings);
        FString MergeError;
        if (!mcp_common::MergeJsonWithClamp(*SettingsJson, SettingsStruct, Merged.GetStructMemory(), MergeError))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("settings does not convert to %s: %s; nothing was changed."),
                *SettingsStruct->GetName(), *MergeError.Replace(TEXT("\n"), TEXT(" "))));
        }
        Asset->Modify();
        SettingsStruct->CopyScriptStruct(Settings, Merged.GetStructMemory());
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

// MCP:set_sound_concurrency_properties - Merge fields into a SoundConcurrency asset's Concurrency settings (maxCount, resolutionRule, volume scaling...; absent keys kept), clamp to ClampMin/ClampMax and save; set_properties neither clamps nor saves. Keys match field names case-insensitively. An empty settings object, a key that names no field (the error lists them), a null value, or a value that is not of the field's kind or does not convert is an error and nothing changes. A failed save makes success false.
//   @retired set_properties - object_path=<concurrency>, properties={"Concurrency":{"MaxCount":..,...}}, clamp=true, save=true
//   @param concurrency_path - Full content path of the target USoundConcurrency.
//   @param settings - Object of FSoundConcurrencySettings fields to merge (maxCount(int>=1), bLimitToOwner(bool), resolutionRule(enum name), retriggerTime, volumeScale(0..1), volumeScaleMode(enum), bVolumeScaleCanRelease, volumeScaleAttackTime, volumeScaleReleaseTime, voiceStealReleaseTime); absent keys untouched. Keys match the field names case-insensitively; an empty object, a key that names no field, a null value or a value that does not convert is an error and nothing is changed.
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

// MCP:get_sound_concurrency_properties - Read the whole FSoundConcurrencySettings struct (maxCount, bLimitToOwner, resolutionRule, retriggerTime, volume scaling, voice stealing) of a USoundConcurrency asset as one settings object, with the keys set_sound_concurrency_properties takes, plus asset_class. A convenience over get_properties on its Concurrency property, shaped for that write tool. Read-only.
//   @retired get_properties - object_path=<concurrency asset>, property_names=["Concurrency"]
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

// MCP:set_sound_attenuation_properties - Merge fields into a SoundAttenuation asset's Attenuation settings (absent keys kept), clamp to ClampMin/ClampMax and save; set_properties neither clamps nor saves. Keys match field names case-insensitively. An empty settings object, a key that names no field (the error lists them), a null value, or a value that is not of the field's kind or does not convert is an error and nothing changes. Plugin sub-objects are not reached. A failed save makes success false.
//   @retired set_properties - object_path=<attenuation>, properties={"Attenuation":{"FalloffDistance":..,...}}, clamp=true, save=true
//   @param attenuation_path - Full content path of the target USoundAttenuation.
//   @param settings - Object of FSoundAttenuationSettings/FBaseAttenuationSettings fields to merge; absent keys untouched. e.g. bAttenuate, bSpatialize, attenuationShape(Sphere/Capsule/Box/Cone), distanceAlgorithm, falloffDistance, dBAttenuationAtMax(-60..0), attenuationShapeExtents{x,y,z}, bAttenuateWithLPF, lPFFrequencyAtMin/Max, bEnableOcclusion, occlusionVolumeAttenuation, bEnableReverbSend, reverbWetLevelMin/Max. Nested plugin sub-objects (pluginSettings, audioLinkSettingsOverride) are NOT authored here. Keys match the field names case-insensitively; an empty object, a key that names no field, a null value or a value that does not convert is an error and nothing is changed.
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

// MCP:get_sound_attenuation_properties - Read the whole FSoundAttenuationSettings struct (shape, falloff, spatialization, LPF, occlusion, reverb send) of a USoundAttenuation asset as one settings object, with the keys set_sound_attenuation_properties takes, plus asset_class. A convenience over get_properties on its Attenuation property, shaped for that write tool; it reads the asset, not what an audio component applies (get_audio_component_properties). Read-only.
//   @retired get_properties - object_path=<attenuation asset>, property_names=["Attenuation"]
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
// Loads a USoundWave (concrete) and writes a JSON object of UPROPERTY fields into
// the Wave object ITSELF (absent keys untouched -- the reflection target is the
// object, not a sub-struct member). Every key is staged first through
// FJsonObjectConverter, into a copy of the property's value clamped to its
// ClampMin/ClampMax meta (the converter ignores clamp meta), so an unknown key or
// a value that does not convert fails the call with nothing written. Enum fields
// map by NAME automatically. Each property whose value changes then goes through
// PreEditChange / PostEditChangeProperty, as an edit in the details panel does,
// so USoundWave's reactions run in-session. The `bLooping` bitfield is authored
// through a dedicated `looping` bool param, never through settings. Saves.
// ---------------------------------------------------------------------------- //
// MCP:set_sound_wave_properties - Merge USoundWave/USoundBase fields into a SoundWave asset (absent keys kept), set bLooping via looping, clamp to ClampMin/ClampMax and save; set_properties neither clamps nor saves. Each property whose value changes runs PostEditChangeProperty, as a details-panel edit does: compression settings rebuild the audio and MaxDistance follows AttenuationSettings. Keys match property names case-insensitively. A key that names no property (the error lists them), a deprecated or instanced-subobject one, a null value, a value of the wrong kind or that does not convert, bLooping inside settings, or a looping that is not true or false is an error and nothing changes. A failed save makes success false.
//   @retired set_properties - object_path=<sound wave>, properties={"Volume":..,"bLooping":..,...}, clamp=true, save=true
//   @param sound_wave_path - Full content path of the target USoundWave (e.g. /Game/Audio/MySound).
//   @param settings - Object of USoundWave/USoundBase UPROPERTY fields to merge, keyed by property name (matched case-insensitively); absent keys untouched. e.g. CompressionQuality(1..100), SampleRateQuality(Max/High/Medium/Low/Min), SoundGroup(SOUNDGROUP_Default/SOUNDGROUP_Effects/SOUNDGROUP_UI/SOUNDGROUP_Music/SOUNDGROUP_Voice), SoundAssetCompressionType(BinkAudio/ADPCM/PCM/Opus/PlatformSpecific/ProjectDefined), Volume(>=0), Pitch(0.125..4.0), Priority(0..100), VirtualizationMode(Disabled/PlayWhenSilent/Restart/SeekRestart), SoundClassObject(asset path), AttenuationSettings(asset path). A key that names no property, a deprecated one or one holding instanced subobjects, a null value, a value that does not convert, a settings value that is not an object, or `bLooping` (use the `looping` param) is an error and nothing is changed.
//   @param looping - Optional bool; sets the bLooping bitfield directly (when this sound is played directly, not via a cue). A value that is not true or false is an error and nothing is changed.
//   @returns { sound_wave_path: string, asset_class: string, settings: object, applied_keys: [string], applied_looping: bool, looping: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleSetSoundWaveProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SoundWavePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_wave_path"), SoundWavePath)) { return Missing; }

    USoundWave* Wave = nullptr;
    if (TSharedPtr<FJsonObject> Error = AudioAsset_Load(SoundWavePath, TEXT("USoundWave"), Wave)) { return Error; }

    // Both params are optional (null counts as absent), but one that holds another JSON type is refused, not skipped.
    const TSharedPtr<FJsonObject>* SettingsJsonPtr = nullptr;
    const bool bHasSettings =
        Params->TryGetObjectField(TEXT("settings"), SettingsJsonPtr) && SettingsJsonPtr && SettingsJsonPtr->IsValid();
    if (!bHasSettings && Params->HasField(TEXT("settings")) && !Params->HasTypedField<EJson::Null>(TEXT("settings")))
    {
        return mcp_common::MakeErrorReply(
            TEXT("settings must be an object of USoundWave property names and values; nothing was changed."));
    }
    const TSharedPtr<FJsonObject> SettingsJson = bHasSettings ? *SettingsJsonPtr : nullptr;
    bool bLooping = false;
    const bool bHasLooping = Params->TryGetBoolField(TEXT("looping"), bLooping);
    if (Params->HasField(TEXT("looping")) && !Params->HasTypedField<EJson::Null>(TEXT("looping"))
        && !Params->HasTypedField<EJson::Boolean>(TEXT("looping")))
    {
        return mcp_common::MakeErrorReply(TEXT("looping must be true or false; nothing was changed."));
    }

    if ((!SettingsJson.IsValid() || SettingsJson->Values.Num() == 0) && !bHasLooping)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to set: provide a non-empty 'settings' object and/or a 'looping' bool"));
    }

    FAudioAssetStagedProperties Staged;
    if (SettingsJson.IsValid())
    {
        // bLooping has its own param; taking it here as well would give the bitfield two sources.
        if (SettingsJson->HasField(TEXT("bLooping")))
        {
            return mcp_common::MakeErrorReply(
                TEXT("settings.bLooping is set through the looping param, not settings; nothing was changed."));
        }
        if (TSharedPtr<FJsonObject> Refused = AudioAsset_StageProperties(*SettingsJson, *Wave, TEXT("settings"), Staged))
        {
            return Refused;
        }
    }

    Wave->Modify();

    // A property whose value changes goes through PreEditChange and PostEditChangeProperty, as an edit in the details
    // panel does: USoundWave reacts there -- CompressionQuality, SampleRateQuality and LoadingBehavior rebuild the
    // compressed audio, SoundAssetCompressionType switches the codec, and every change recomputes MaxDistance.
    for (const TPair<FProperty*, void*>& Value : Staged.Values)
    {
        if (AudioAsset_HoldsValue(*Value.Key, *Wave, Value.Value))
        {
            continue;
        }
        Wave->PreEditChange(Value.Key);
        Value.Key->CopyCompleteValue(Value.Key->ContainerPtrToValuePtr<void>(Wave), Value.Value);
        FPropertyChangedEvent ChangedEvent(Value.Key, EPropertyChangeType::ValueSet);
        Wave->PostEditChangeProperty(ChangedEvent);
    }

    // bLooping is a uint8 : 1 bitfield, set directly.
    if (bHasLooping && (Wave->bLooping != 0) != bLooping)
    {
        FProperty* LoopingProperty = USoundWave::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(USoundWave, bLooping));
        Wave->PreEditChange(LoopingProperty);
        Wave->bLooping = bLooping ? 1 : 0;
        FPropertyChangedEvent ChangedEvent(LoopingProperty, EPropertyChangeType::ValueSet);
        Wave->PostEditChangeProperty(ChangedEvent);
    }

    TArray<TSharedPtr<FJsonValue>> AppliedKeys;
    if (SettingsJson.IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SettingsJson->Values)
        {
            AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
    }

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
// MCP:get_sound_wave_properties - Read every reflected property of a USoundWave asset, its USoundBase fields included (CompressionQuality, SampleRateQuality, SoundGroup, Volume, Pitch, Priority, SoundClassObject, AttenuationSettings and the rest), as the settings object FJsonObjectConverter exports, so keys start lower-case (compressionQuality); settings also carries bLooping, echoed as looping. Like get_object_info on the asset, but keyed as set_sound_wave_properties accepts it; get_properties reads a few named fields. Read-only: it loads the asset if needed and changes nothing. A path that does not load as a USoundWave is an error. Write the same fields back with set_sound_wave_properties (bLooping through its looping param); a sound class is read with get_sound_class_properties and a level's audio component with get_audio_component_properties.
//   @retired get_object_info - object_path=<sound wave>: every property; get_properties for named ones
//   @param sound_wave_path - Full content path of the target USoundWave, e.g. "/Game/Audio/MySound".
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
// MCP:set_submix_effect_settings - Change the Settings struct of one effect preset in a USoundSubmix's effect chain, picked by chain index, through reflection: only the keys given are written, each clamped to its ClampMin/ClampMax, and settings echoes the whole struct read back. Read the chain first with get_submix_effect_chain; add_submix_effect_to_chain appends a preset and remove_submix_effect_from_chain takes one out. One undo step, then the submix package is saved, and so is the preset's own package when the preset is a separate asset appended by path (preset_in_submix_package=false) -- that asset is shared, so every submix that uses it hears the change. submix_saved and preset_saved report each save, and saved is both; a failed save of either makes success false with save_error. Repeating the same values changes nothing more. An index out of range, an empty slot, a preset class with no Settings struct or a value that does not convert is an error.
//   @param submix_path - Full content path of the target USoundSubmix, e.g. "/Game/Audio/Submix_Music"
//   @param effect_index - 0-based index into SubmixEffectChain (same order add_submix_effect_to_chain appended / get_submix_effect_chain reports)
//   @param settings - Object of fields to merge into the preset's Settings struct, keyed by property name (case-insensitive), e.g. {"ThresholdDb": -12, "Ratio": 4} for a dynamics processor; absent keys untouched, unknown keys ignored
//   @returns { submix_path: string, effect_index: int, preset_class: string, preset_path: string, preset_in_submix_package: bool, submix_saved: bool, preset_saved: bool, settings: object, saved: bool, save_error?: string }
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
    const bool bSubmixSaved = mcp_common::SaveAssetPackage(Submix->GetOutermost(), Submix, SaveError);

    // A preset appended by path is its own asset in its own package, which the submix save does not write: it
    // is saved too, or the change would sit dirty in memory while the reply said saved.
    UPackage* const PresetPackage = Preset->GetOutermost();
    const bool bPresetInSubmixPackage = (PresetPackage == Submix->GetOutermost());
    bool bPresetSaved = bSubmixSaved;
    if (!bPresetInSubmixPackage)
    {
        FString PresetSaveError;
        bPresetSaved = mcp_common::SaveAssetPackage(PresetPackage, Preset, PresetSaveError);
        if (!bPresetSaved)
        {
            SaveError = SaveError.IsEmpty()
                ? FString::Printf(TEXT("preset %s: %s"), *Preset->GetPathName(), *PresetSaveError)
                : FString::Printf(TEXT("%s; preset %s: %s"), *SaveError, *Preset->GetPathName(), *PresetSaveError);
        }
    }

    // Read-back the persisted Settings via reflection.
    TSharedPtr<FJsonObject> SettingsOut = mcp_common::StructToJson(
        SettingsStruct, SettingsData, 0, 0);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("submix_path"), Submix->GetPathName());
    ResultObj->SetNumberField(TEXT("effect_index"), EffectIndex);
    ResultObj->SetStringField(TEXT("preset_class"), Preset->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("preset_path"), Preset->GetPathName());
    ResultObj->SetBoolField(TEXT("preset_in_submix_package"), bPresetInSubmixPackage);
    ResultObj->SetBoolField(TEXT("submix_saved"), bSubmixSaved);
    ResultObj->SetBoolField(TEXT("preset_saved"), bPresetSaved);
    ResultObj->SetObjectField(TEXT("settings"), SettingsOut);

    return mcp_common::MakeSavedReply(ResultObj, bSubmixSaved && bPresetSaved, TEXT("set_submix_effect_settings"), SaveError);
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
    { TEXT("remove_submix_effect_from_chain"), &HandleRemoveSubmixEffectFromChain },
    { TEXT("create_dialogue_voice"), &HandleCreateDialogueVoice },
    { TEXT("create_dialogue_wave"), &HandleCreateDialogueWave },
    { TEXT("create_sound_attenuation"), &HandleCreateSoundAttenuation },
    { TEXT("create_sound_class"), &HandleCreateSoundClass },
    { TEXT("create_sound_concurrency"), &HandleCreateSoundConcurrency },
    { TEXT("create_sound_mix"), &HandleCreateSoundMix },
    { TEXT("create_sound_submix"), &HandleCreateSoundSubmix },
    { TEXT("get_sound_attenuation_properties"), &HandleGetSoundAttenuationProperties },
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
