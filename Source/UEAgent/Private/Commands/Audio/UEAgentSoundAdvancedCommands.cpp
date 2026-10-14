#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// MetaSound (MetasoundEngine / MetasoundFrontend runtime modules)
#include "MetasoundBuilderSubsystem.h"       // UMetaSoundBuilderSubsystem
#include "MetasoundBuilderBase.h"            // UMetaSoundBuilderBase, EMetaSoundBuilderResult
#include "MetasoundSource.h"                 // UMetaSoundSource
#include "Metasound.h"                       // UMetaSoundPatch
#include "MetasoundDocumentInterface.h"      // IMetaSoundDocumentInterface
#include "MetasoundFrontendDocument.h"       // FMetasoundFrontendDocument, FMetasoundFrontendClassInterface
#include "MetasoundFrontendLiteral.h"        // FMetasoundFrontendLiteral

// MetaSound editor (MetasoundEditor -- UncookedOnly module, editor only)
#include "MetasoundEditorSubsystem.h"        // UMetaSoundEditorSubsystem::BuildToAsset / FindOrBeginBuilding

// Engine sound asset classes (all in Runtime/Engine, already a Build.cs dep)
#include "Sound/SoundClass.h"               // USoundClass, FSoundClassProperties
#include "Sound/SoundMix.h"                 // USoundMix, FSoundClassAdjuster

// Engine / editor
#include "Editor.h"                          // GEditor
#include "Engine/Engine.h"                   // GEngine
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/ScriptInterface.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

/**
 * Advanced sound authoring commands: MetaSound presets, USoundClass properties and USoundMix class
 * overrides.
 *
 * MetaSound presets go through the builder API: UMetaSoundBuilderSubsystem::CreateSourcePresetBuilder /
 * CreatePatchPresetBuilder make a transient preset builder and UMetaSoundEditorSubsystem::BuildToAsset
 * persists it as a new asset; an existing preset is edited through FindOrBeginBuilding,
 * UMetaSoundBuilderBase::SetGraphInputDefault / GetGraphInputDefault and BuildAndOverwriteMetaSound.
 * Sound classes and mixes are plain Engine assets edited in place: USoundClass::Properties
 * (FSoundClassProperties) and the FSoundClassAdjuster entries of USoundMix::SoundClassEffects.
 */
namespace UEAgentSoundAdvancedCommands
{

// MCP-DOMAIN: Audio

namespace
{
    // The MetaSound preset at PresetPath (presets are UMetaSoundSource with bIsPreset=true) and a mutable
    // builder for it from the editor subsystem's FindOrBeginBuilding: null once both are set, otherwise the
    // error reply.
    TSharedPtr<FJsonObject> SoundAdvanced_OpenPreset(const FString& PresetPath, UMetaSoundSource*& OutSource,
        UMetaSoundBuilderBase*& OutBuilder)
    {
        OutSource = mcp_common::ResolveAsset<UMetaSoundSource>(PresetPath);
        if (!OutSource)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load MetaSound preset at %s"), *PresetPath));
        }

        UMetaSoundEditorSubsystem* EditorSubsystem =
            GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>() : nullptr;
        if (!EditorSubsystem)
        {
            return mcp_common::MakeErrorReply(TEXT("UMetaSoundEditorSubsystem unavailable (editor subsystem not initialized)"));
        }

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface = OutSource;
        EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
        OutBuilder = EditorSubsystem->FindOrBeginBuilding(DocInterface, FindResult);
        if (!OutBuilder || FindResult != EMetaSoundBuilderResult::Succeeded)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("FindOrBeginBuilding failed to obtain a builder for '%s'"), *PresetPath));
        }
        return nullptr;
    }

    // Loads the TAsset at Path into OutAsset; when that fails, the reply "Could not load <TypeLabel> at <Path>".
    template <typename TAsset>
    TSharedPtr<FJsonObject> SoundAdvanced_LoadAsset(const FString& Path, const TCHAR* TypeLabel, TAsset*& OutAsset)
    {
        OutAsset = Cast<TAsset>(StaticLoadObject(TAsset::StaticClass(), nullptr, *Path));
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load %s at %s"), TypeLabel, *Path));
        }
        return nullptr;
    }

    // The FSoundClassProperties fields set_ and get_sound_class_properties publish, read from SoundClass.
    void SoundAdvanced_WriteSoundClassProperties(const TSharedPtr<FJsonObject>& Out, const USoundClass& SoundClass)
    {
        const FSoundClassProperties& Properties = SoundClass.Properties;
        Out->SetNumberField(TEXT("volume"), Properties.Volume);
        Out->SetNumberField(TEXT("pitch"), Properties.Pitch);
        Out->SetNumberField(TEXT("low_pass_frequency"), Properties.LowPassFilterFrequency);
        Out->SetNumberField(TEXT("attenuation_distance_scale"), Properties.AttenuationDistanceScale);
        Out->SetNumberField(TEXT("lfe_bleed"), Properties.LFEBleed);
        Out->SetNumberField(TEXT("voice_center_channel_volume"), Properties.VoiceCenterChannelVolume);
        Out->SetNumberField(TEXT("radio_filter_volume"), Properties.RadioFilterVolume);
        Out->SetNumberField(TEXT("radio_filter_volume_threshold"), Properties.RadioFilterVolumeThreshold);
        Out->SetNumberField(TEXT("default_2d_reverb_send_amount"), Properties.Default2DReverbSendAmount);
        Out->SetBoolField(TEXT("apply_effects"), Properties.bApplyEffects != 0);
        Out->SetBoolField(TEXT("always_play"), Properties.bAlwaysPlay != 0);
        Out->SetBoolField(TEXT("is_ui_sound"), Properties.bIsUISound != 0);
        Out->SetBoolField(TEXT("is_music"), Properties.bIsMusic != 0);
        Out->SetBoolField(TEXT("center_channel_only"), Properties.bCenterChannelOnly != 0);
        Out->SetBoolField(TEXT("apply_ambient_volumes"), Properties.bApplyAmbientVolumes != 0);
        Out->SetBoolField(TEXT("reverb"), Properties.bReverb != 0);
        Out->SetStringField(TEXT("output_target"),
            StaticEnum<EAudioOutputTarget::Type>()->GetNameStringByValue(static_cast<int64>(Properties.OutputTarget.GetValue())));
        Out->SetStringField(TEXT("loading_behavior"),
            StaticEnum<ESoundWaveLoadingBehavior>()->GetNameStringByValue(static_cast<int64>(Properties.LoadingBehavior)));
    }
}

// ---------------------------------------------------------------------------
// create_metasound_preset
// Creates a MetaSound preset from an existing source or patch asset.
// Uses CreateSourcePresetBuilder or CreatePatchPresetBuilder to create a
// transient preset builder, then BuildToAsset to persist it as a new asset.
// ---------------------------------------------------------------------------
// MCP:create_metasound_preset - Creates a MetaSound preset from an existing source or patch asset.  Uses UMetaSoundBuilderSubsystem::CreateSourcePresetBuilder (or CreatePatchPresetBuilder) to create a transient preset builder referencing the source, then BuildToAsset to persist it as a new /Game asset.
//   @param preset_name - Name of the new MetaSound preset asset.
//   @param package_path - Destination /Game folder (default /Game/MetaSounds).
//   @param source_asset_path - Content path of the existing MetaSound source/patch to use as the preset's template.
//   @param preset_type - "source" (default) or "patch".
//   @returns { preset_path: string, preset_type: string, success: bool, saved: bool, already_exists: bool }

TSharedPtr<FJsonObject> HandleCreateMetasoundPreset(
    const TSharedPtr<FJsonObject>& Params)
{
    FString PresetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("preset_name"), PresetName)) { return Missing; }

    FString PackagePath = TEXT("/Game/MetaSounds");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString SourceAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_asset_path"), SourceAssetPath)) { return Missing; }

    FString PresetType = TEXT("source");
    Params->TryGetStringField(TEXT("preset_type"), PresetType);

    // Idempotent: if the preset asset already exists, return it. BuildToAsset
    // below CANNOT create over an existing asset (it fails), so a re-run would
    // otherwise report "BuildToAsset failed" even though the preset is present.
    const FString PresetFullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *PresetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PresetFullPath)) { return Invalid; }
    if (UEditorAssetLibrary::DoesAssetExist(PresetFullPath))
    {
        UObject* Existing = UEditorAssetLibrary::LoadAsset(PresetFullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("preset_path"), Existing->GetPathName());
            ResultObj->SetStringField(TEXT("preset_type"), PresetType);
            ResultObj->SetBoolField(TEXT("success"), true);
            ResultObj->SetBoolField(TEXT("already_exists"), true);
            ResultObj->SetBoolField(TEXT("saved"), true);
            return mcp_common::MakeOkReply(ResultObj);
        }
    }

    UMetaSoundBuilderSubsystem* BuilderSubsystem =
        GEngine ? GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>() : nullptr;
    if (!BuilderSubsystem)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UMetaSoundBuilderSubsystem unavailable (engine subsystem not initialized)"));
    }

    UMetaSoundEditorSubsystem* EditorSubsystem =
        GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>() : nullptr;
    if (!EditorSubsystem)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UMetaSoundEditorSubsystem unavailable (editor subsystem not initialized)"));
    }

    UMetaSoundBuilderBase* PresetBuilder = nullptr;
    EMetaSoundBuilderResult CreateResult = EMetaSoundBuilderResult::Failed;
    const FName BuilderFName(*FString::Printf(TEXT("MCP_Preset_%s"), *PresetName));

    if (PresetType == TEXT("patch"))
    {
        UMetaSoundPatch* Patch = mcp_common::ResolveAsset<UMetaSoundPatch>(SourceAssetPath);
        if (!Patch)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load UMetaSoundPatch at %s"), *SourceAssetPath));
        }

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface = Patch;
        UMetaSoundPatchBuilder* PatchBuilder = BuilderSubsystem->CreatePatchPresetBuilder(
            BuilderFName, DocInterface, CreateResult);
        if (!PatchBuilder || CreateResult != EMetaSoundBuilderResult::Succeeded)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("CreatePatchPresetBuilder failed for '%s'"), *SourceAssetPath));
        }
        PresetBuilder = PatchBuilder;
    }
    else
    {
        UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(SourceAssetPath);
        if (!Source)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load UMetaSoundSource at %s"), *SourceAssetPath));
        }

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface = Source;
        UMetaSoundSourceBuilder* SourceBuilder = BuilderSubsystem->CreateSourcePresetBuilder(
            BuilderFName, DocInterface, CreateResult);
        if (!SourceBuilder || CreateResult != EMetaSoundBuilderResult::Succeeded)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("CreateSourcePresetBuilder failed for '%s'"), *SourceAssetPath));
        }
        PresetBuilder = SourceBuilder;
    }

    // BuildToAsset persists the preset as a new /Game asset.
    EMetaSoundBuilderResult BuildResult = EMetaSoundBuilderResult::Failed;
    TScriptInterface<IMetaSoundDocumentInterface> PresetDoc = EditorSubsystem->BuildToAsset(
        PresetBuilder,
        /*Author*/ TEXT("UEAgent"),
        /*AssetName*/ PresetName,
        /*PackagePath*/ PackagePath,
        BuildResult,
        /*TemplateSoundWave*/ nullptr);

    if (BuildResult != EMetaSoundBuilderResult::Succeeded || !PresetDoc.GetObject())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BuildToAsset failed for preset '%s' at '%s'"), *PresetName, *PackagePath));
    }

    UObject* PresetObj = PresetDoc.GetObject();
    const FString PresetPath = PresetObj->GetPathName();

    // Ensure the package is persisted.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(PresetObj->GetOutermost(), PresetObj, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("preset_path"), PresetPath);
    ResultObj->SetStringField(TEXT("preset_type"), PresetType);
    ResultObj->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_metasound_preset"), SaveError);
}

// ---------------------------------------------------------------------------
// set_metasound_preset_parameter
// Sets a parameter default value on a MetaSound preset. Loads the preset,
// obtains a builder via FindOrBeginBuilding, calls SetGraphInputDefault, then
// BuildAndOverwriteMetaSound to persist.
// ---------------------------------------------------------------------------
// MCP:set_metasound_preset_parameter - Sets a parameter default value on a MetaSound preset.  Loads the preset, obtains a builder via FindOrBeginBuilding, calls SetGraphInputDefault with the appropriate literal type, then BuildAndOverwriteMetaSound to persist.
//   @param preset_path - Content path of the MetaSound preset asset.
//   @param parameter_name - Name of the graph input parameter to set.
//   @param parameter_type - Literal type for the value: "float" (default), "bool", "int", "string".
//   @param value - Value to set. Interpreted according to parameter_type.
//   @returns { preset_path: string, parameter_name: string, success: bool, verified: bool, value_read_back: string, saved: bool }

TSharedPtr<FJsonObject> HandleSetMetasoundPresetParameter(
    const TSharedPtr<FJsonObject>& Params)
{
    FString PresetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("preset_path"), PresetPath)) { return Missing; }

    FString ParameterName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParameterName)) { return Missing; }

    FString ParameterType = TEXT("float");
    Params->TryGetStringField(TEXT("parameter_type"), ParameterType);

    UMetaSoundSource* Source = nullptr;
    UMetaSoundBuilderBase* Builder = nullptr;
    if (TSharedPtr<FJsonObject> OpenError = SoundAdvanced_OpenPreset(PresetPath, Source, Builder)) { return OpenError; }

    // Build the literal from the supplied value based on parameter_type.
    FMetasoundFrontendLiteral Literal;
    const FString TypeLower = ParameterType.ToLower();

    if (TypeLower == TEXT("bool"))
    {
        bool B = false;
        Params->TryGetBoolField(TEXT("value"), B);
        Literal.Set(B);
    }
    else if (TypeLower == TEXT("int"))
    {
        int32 I = 0;
        Params->TryGetNumberField(TEXT("value"), I);
        Literal.Set(I);
    }
    else if (TypeLower == TEXT("string"))
    {
        FString S;
        Params->TryGetStringField(TEXT("value"), S);
        Literal.Set(S);
    }
    else
    {
        // Default: float
        double V = 0.0;
        Params->TryGetNumberField(TEXT("value"), V);
        Literal.Set(static_cast<float>(V));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetMetasoundPresetParam", "MetaSound Set Preset Parameter"));
    Source->Modify();

    EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
    Builder->SetGraphInputDefault(FName(*ParameterName), Literal, SetResult);
    if (SetResult != EMetaSoundBuilderResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetGraphInputDefault failed for parameter '%s'"), *ParameterName));
    }

    // BuildAndOverwriteMetaSound persists the changes back to the asset.
    TScriptInterface<IMetaSoundDocumentInterface> OverwriteDoc = Source;
    Builder->BuildAndOverwriteMetaSound(OverwriteDoc);

    // Verify by reading back.
    EMetaSoundBuilderResult GetResult = EMetaSoundBuilderResult::Failed;
    const FMetasoundFrontendLiteral ReadBack = Builder->GetGraphInputDefault(FName(*ParameterName), GetResult);

    // Save the package.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Source->GetOutermost(), Source, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("preset_path"), Source->GetPathName());
    ResultObj->SetStringField(TEXT("parameter_name"), ParameterName);
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("verified"), GetResult == EMetaSoundBuilderResult::Succeeded);
    ResultObj->SetStringField(TEXT("value_read_back"), ReadBack.ToString());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_metasound_preset_parameter"), SaveError);
}

// ---------------------------------------------------------------------------
// get_metasound_preset_parameters
// Gets all parameter defaults from a MetaSound preset. Loads the preset,
// obtains a builder, iterates GetGraphInputNames, and reads each default.
// ---------------------------------------------------------------------------
// MCP:get_metasound_preset_parameters - Gets all parameter defaults from a MetaSound preset.  Loads the preset, obtains a builder, iterates GetGraphInputNames, and reads each default value along with its type from the document interface.
//   @param preset_path - Content path of the MetaSound preset asset.
//   @returns { preset_path: string, parameters: [{name, type, value}], parameter_count: int }

TSharedPtr<FJsonObject> HandleGetMetasoundPresetParameters(
    const TSharedPtr<FJsonObject>& Params)
{
    FString PresetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("preset_path"), PresetPath)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    UMetaSoundBuilderBase* Builder = nullptr;
    if (TSharedPtr<FJsonObject> OpenError = SoundAdvanced_OpenPreset(PresetPath, Source, Builder)) { return OpenError; }

    // Enumerate inputs from the authoritative document interface rather than the
    // builder's GetGraphInputNames: for a PRESET whose own graph declares no inputs
    // (it inherits the referenced asset's interface) GetGraphInputNames returns
    // Failed, which is not an error — the preset simply has no builder-local inputs.
    const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();
    const FMetasoundFrontendClassInterface& Interface = Doc.RootGraph.GetDefaultInterface();

    TArray<TSharedPtr<FJsonValue>> ParametersArr;
    for (const FMetasoundFrontendClassInput& In : Interface.Inputs)
    {
        const FName InputName = In.Name;

        // Prefer the live builder default; tolerate a per-input read failure.
        EMetaSoundBuilderResult GetResult = EMetaSoundBuilderResult::Failed;
        const FMetasoundFrontendLiteral DefaultVal = Builder->GetGraphInputDefault(InputName, GetResult);

        TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
        ParamObj->SetStringField(TEXT("name"), InputName.ToString());
        ParamObj->SetStringField(TEXT("type"), In.TypeName.ToString());
        ParamObj->SetStringField(TEXT("value"),
            GetResult == EMetaSoundBuilderResult::Succeeded ? DefaultVal.ToString() : TEXT("(inherited)"));
        ParametersArr.Add(MakeShared<FJsonValueObject>(ParamObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("preset_path"), Source->GetPathName());
    ResultObj->SetArrayField(TEXT("parameters"), ParametersArr);
    ResultObj->SetNumberField(TEXT("parameter_count"), ParametersArr.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// set_sound_class_properties
// Sets sound class properties (volume, pitch, low_pass_frequency,
// attenuation_distance_scale) on a USoundClass asset.
// ---------------------------------------------------------------------------
// MCP:set_sound_class_properties - Sets properties on a USoundClass asset.  Loads the USoundClass, sets the specified FSoundClassProperties fields, and saves the package.
//   @param sound_class_path - Content path of the USoundClass asset.
//   @param volume - Properties.Volume.
//   @param pitch - Properties.Pitch.
//   @param low_pass_frequency - Properties.LowPassFilterFrequency.
//   @param attenuation_distance_scale - Properties.AttenuationDistanceScale.
//   @param lfe_bleed - Properties.LFEBleed.
//   @param voice_center_channel_volume - Properties.VoiceCenterChannelVolume.
//   @param radio_filter_volume - Properties.RadioFilterVolume.
//   @param radio_filter_volume_threshold - Properties.RadioFilterVolumeThreshold.
//   @param default_2d_reverb_send_amount - Properties.Default2DReverbSendAmount.
//   @param apply_effects - Properties.bApplyEffects.
//   @param always_play - Properties.bAlwaysPlay.
//   @param is_ui_sound - Properties.bIsUISound.
//   @param is_music - Properties.bIsMusic.
//   @param center_channel_only - Properties.bCenterChannelOnly.
//   @param apply_ambient_volumes - Properties.bApplyAmbientVolumes.
//   @param reverb - Properties.bReverb.
//   @param output_target - Properties.OutputTarget enum name (Speaker|Controller|ControllerFallbackToSpeaker); case-insensitive, ignored if unrecognized.
//   @param loading_behavior - Properties.LoadingBehavior enum name (Inherited|RetainOnLoad|PrimeOnLoad|LoadOnDemand|ForceInline); case-insensitive, ignored if unrecognized.
//   @returns { sound_class_path: string, success: bool, volume: float, pitch: float, low_pass_frequency: float, attenuation_distance_scale: float, lfe_bleed: float, voice_center_channel_volume: float, radio_filter_volume: float, radio_filter_volume_threshold: float, default_2d_reverb_send_amount: float, apply_effects: bool, always_play: bool, is_ui_sound: bool, is_music: bool, center_channel_only: bool, apply_ambient_volumes: bool, reverb: bool, output_target: string, loading_behavior: string, applied_volume: bool, applied_pitch: bool, applied_low_pass_frequency: bool, applied_attenuation_distance_scale: bool, applied_lfe_bleed: bool, applied_voice_center_channel_volume: bool, applied_radio_filter_volume: bool, applied_radio_filter_volume_threshold: bool, applied_default_2d_reverb_send_amount: bool, applied_apply_effects: bool, applied_always_play: bool, applied_is_ui_sound: bool, applied_is_music: bool, applied_center_channel_only: bool, applied_apply_ambient_volumes: bool, applied_reverb: bool, applied_output_target: bool, applied_loading_behavior: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetSoundClassProperties(
    const TSharedPtr<FJsonObject>& Params)
{
    FString SoundClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_class_path"), SoundClassPath)) { return Missing; }

    USoundClass* SoundClass = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundAdvanced_LoadAsset(SoundClassPath, TEXT("USoundClass"), SoundClass)) { return LoadError; }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSoundClassProps", "Set Sound Class Properties"));
    SoundClass->Modify();

    bool bAppliedVolume = false;
    bool bAppliedPitch = false;
    bool bAppliedLowPass = false;
    bool bAppliedAttenDist = false;
    bool bAppliedLFEBleed = false;
    bool bAppliedVoiceCenter = false;
    bool bAppliedRadioFilterVolume = false;
    bool bAppliedRadioFilterThreshold = false;
    bool bAppliedDefault2DReverb = false;
    bool bAppliedApplyEffects = false;
    bool bAppliedAlwaysPlay = false;
    bool bAppliedIsUISound = false;
    bool bAppliedIsMusic = false;
    bool bAppliedCenterChannelOnly = false;
    bool bAppliedApplyAmbientVolumes = false;
    bool bAppliedReverb = false;
    bool bAppliedOutputTarget = false;
    bool bAppliedLoadingBehavior = false;

    double VolumeD = 0.0;
    if (Params->TryGetNumberField(TEXT("volume"), VolumeD))
    {
        SoundClass->Properties.Volume = static_cast<float>(VolumeD);
        bAppliedVolume = true;
    }

    double PitchD = 0.0;
    if (Params->TryGetNumberField(TEXT("pitch"), PitchD))
    {
        SoundClass->Properties.Pitch = static_cast<float>(PitchD);
        bAppliedPitch = true;
    }

    double LowPassD = 0.0;
    if (Params->TryGetNumberField(TEXT("low_pass_frequency"), LowPassD))
    {
        SoundClass->Properties.LowPassFilterFrequency = static_cast<float>(LowPassD);
        bAppliedLowPass = true;
    }

    double AttenDistD = 0.0;
    if (Params->TryGetNumberField(TEXT("attenuation_distance_scale"), AttenDistD))
    {
        SoundClass->Properties.AttenuationDistanceScale = static_cast<float>(AttenDistD);
        bAppliedAttenDist = true;
    }

    double LFEBleedD = 0.0;
    if (Params->TryGetNumberField(TEXT("lfe_bleed"), LFEBleedD))
    {
        SoundClass->Properties.LFEBleed = static_cast<float>(LFEBleedD);
        bAppliedLFEBleed = true;
    }

    double VoiceCenterD = 0.0;
    if (Params->TryGetNumberField(TEXT("voice_center_channel_volume"), VoiceCenterD))
    {
        SoundClass->Properties.VoiceCenterChannelVolume = static_cast<float>(VoiceCenterD);
        bAppliedVoiceCenter = true;
    }

    double RadioFilterVolumeD = 0.0;
    if (Params->TryGetNumberField(TEXT("radio_filter_volume"), RadioFilterVolumeD))
    {
        SoundClass->Properties.RadioFilterVolume = static_cast<float>(RadioFilterVolumeD);
        bAppliedRadioFilterVolume = true;
    }

    double RadioFilterThresholdD = 0.0;
    if (Params->TryGetNumberField(TEXT("radio_filter_volume_threshold"), RadioFilterThresholdD))
    {
        SoundClass->Properties.RadioFilterVolumeThreshold = static_cast<float>(RadioFilterThresholdD);
        bAppliedRadioFilterThreshold = true;
    }

    double Default2DReverbD = 0.0;
    if (Params->TryGetNumberField(TEXT("default_2d_reverb_send_amount"), Default2DReverbD))
    {
        SoundClass->Properties.Default2DReverbSendAmount = static_cast<float>(Default2DReverbD);
        bAppliedDefault2DReverb = true;
    }

    bool bApplyEffectsB = false;
    if (Params->TryGetBoolField(TEXT("apply_effects"), bApplyEffectsB))
    {
        SoundClass->Properties.bApplyEffects = bApplyEffectsB ? 1 : 0;
        bAppliedApplyEffects = true;
    }

    bool bAlwaysPlayB = false;
    if (Params->TryGetBoolField(TEXT("always_play"), bAlwaysPlayB))
    {
        SoundClass->Properties.bAlwaysPlay = bAlwaysPlayB ? 1 : 0;
        bAppliedAlwaysPlay = true;
    }

    bool bIsUISoundB = false;
    if (Params->TryGetBoolField(TEXT("is_ui_sound"), bIsUISoundB))
    {
        SoundClass->Properties.bIsUISound = bIsUISoundB ? 1 : 0;
        bAppliedIsUISound = true;
    }

    bool bIsMusicB = false;
    if (Params->TryGetBoolField(TEXT("is_music"), bIsMusicB))
    {
        SoundClass->Properties.bIsMusic = bIsMusicB ? 1 : 0;
        bAppliedIsMusic = true;
    }

    bool bCenterChannelOnlyB = false;
    if (Params->TryGetBoolField(TEXT("center_channel_only"), bCenterChannelOnlyB))
    {
        SoundClass->Properties.bCenterChannelOnly = bCenterChannelOnlyB ? 1 : 0;
        bAppliedCenterChannelOnly = true;
    }

    bool bApplyAmbientVolumesB = false;
    if (Params->TryGetBoolField(TEXT("apply_ambient_volumes"), bApplyAmbientVolumesB))
    {
        SoundClass->Properties.bApplyAmbientVolumes = bApplyAmbientVolumesB ? 1 : 0;
        bAppliedApplyAmbientVolumes = true;
    }

    bool bReverbB = false;
    if (Params->TryGetBoolField(TEXT("reverb"), bReverbB))
    {
        SoundClass->Properties.bReverb = bReverbB ? 1 : 0;
        bAppliedReverb = true;
    }

    FString OutputTargetStr;
    if (Params->TryGetStringField(TEXT("output_target"), OutputTargetStr) && !OutputTargetStr.IsEmpty())
    {
        const int64 EnumVal = StaticEnum<EAudioOutputTarget::Type>()->GetValueByNameString(OutputTargetStr);
        if (EnumVal != INDEX_NONE)
        {
            SoundClass->Properties.OutputTarget = static_cast<EAudioOutputTarget::Type>(EnumVal);
            bAppliedOutputTarget = true;
        }
    }

    FString LoadingBehaviorStr;
    if (Params->TryGetStringField(TEXT("loading_behavior"), LoadingBehaviorStr) && !LoadingBehaviorStr.IsEmpty())
    {
        const int64 EnumVal = StaticEnum<ESoundWaveLoadingBehavior>()->GetValueByNameString(LoadingBehaviorStr);
        if (EnumVal != INDEX_NONE)
        {
            SoundClass->Properties.LoadingBehavior = static_cast<ESoundWaveLoadingBehavior>(EnumVal);
            bAppliedLoadingBehavior = true;
        }
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(SoundClass->GetOutermost(), SoundClass, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("sound_class_path"), SoundClass->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    SoundAdvanced_WriteSoundClassProperties(ResultObj, *SoundClass);
    ResultObj->SetBoolField(TEXT("applied_volume"), bAppliedVolume);
    ResultObj->SetBoolField(TEXT("applied_pitch"), bAppliedPitch);
    ResultObj->SetBoolField(TEXT("applied_low_pass_frequency"), bAppliedLowPass);
    ResultObj->SetBoolField(TEXT("applied_attenuation_distance_scale"), bAppliedAttenDist);
    ResultObj->SetBoolField(TEXT("applied_lfe_bleed"), bAppliedLFEBleed);
    ResultObj->SetBoolField(TEXT("applied_voice_center_channel_volume"), bAppliedVoiceCenter);
    ResultObj->SetBoolField(TEXT("applied_radio_filter_volume"), bAppliedRadioFilterVolume);
    ResultObj->SetBoolField(TEXT("applied_radio_filter_volume_threshold"), bAppliedRadioFilterThreshold);
    ResultObj->SetBoolField(TEXT("applied_default_2d_reverb_send_amount"), bAppliedDefault2DReverb);
    ResultObj->SetBoolField(TEXT("applied_apply_effects"), bAppliedApplyEffects);
    ResultObj->SetBoolField(TEXT("applied_always_play"), bAppliedAlwaysPlay);
    ResultObj->SetBoolField(TEXT("applied_is_ui_sound"), bAppliedIsUISound);
    ResultObj->SetBoolField(TEXT("applied_is_music"), bAppliedIsMusic);
    ResultObj->SetBoolField(TEXT("applied_center_channel_only"), bAppliedCenterChannelOnly);
    ResultObj->SetBoolField(TEXT("applied_apply_ambient_volumes"), bAppliedApplyAmbientVolumes);
    ResultObj->SetBoolField(TEXT("applied_reverb"), bAppliedReverb);
    ResultObj->SetBoolField(TEXT("applied_output_target"), bAppliedOutputTarget);
    ResultObj->SetBoolField(TEXT("applied_loading_behavior"), bAppliedLoadingBehavior);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_sound_class_properties"), SaveError);
}

// ---------------------------------------------------------------------------
// get_sound_class_properties
// Reads sound class properties and hierarchy info from a USoundClass asset.
// ---------------------------------------------------------------------------
// MCP:get_sound_class_properties - Gets sound class properties and hierarchy info from a USoundClass.
//   @param sound_class_path - Content path of the USoundClass asset.
//   @returns { sound_class_path: string, volume: float, pitch: float, low_pass_frequency: float, attenuation_distance_scale: float, lfe_bleed: float, voice_center_channel_volume: float, radio_filter_volume: float, radio_filter_volume_threshold: float, default_2d_reverb_send_amount: float, apply_effects: bool, always_play: bool, is_ui_sound: bool, is_music: bool, center_channel_only: bool, apply_ambient_volumes: bool, reverb: bool, output_target: string, loading_behavior: string, parent_class: string, parent_class_path: string, child_classes: [{name, path}], child_count: int }

TSharedPtr<FJsonObject> HandleGetSoundClassProperties(
    const TSharedPtr<FJsonObject>& Params)
{
    FString SoundClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_class_path"), SoundClassPath)) { return Missing; }

    USoundClass* SoundClass = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundAdvanced_LoadAsset(SoundClassPath, TEXT("USoundClass"), SoundClass)) { return LoadError; }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("sound_class_path"), SoundClass->GetPathName());
    SoundAdvanced_WriteSoundClassProperties(ResultObj, *SoundClass);

    // Parent class.
    if (USoundClass* Parent = SoundClass->ParentClass.Get())
    {
        ResultObj->SetStringField(TEXT("parent_class"), Parent->GetName());
        ResultObj->SetStringField(TEXT("parent_class_path"), Parent->GetPathName());
    }
    else
    {
        ResultObj->SetStringField(TEXT("parent_class"), TEXT("None"));
    }

    // Child classes.
    TArray<TSharedPtr<FJsonValue>> ChildrenArr;
    for (const TObjectPtr<USoundClass>& ChildPtr : SoundClass->ChildClasses)
    {
        if (USoundClass* Child = ChildPtr.Get())
        {
            TSharedPtr<FJsonObject> ChildObj = MakeShared<FJsonObject>();
            ChildObj->SetStringField(TEXT("name"), Child->GetName());
            ChildObj->SetStringField(TEXT("path"), Child->GetPathName());
            ChildrenArr.Add(MakeShared<FJsonValueObject>(ChildObj));
        }
    }
    ResultObj->SetArrayField(TEXT("child_classes"), ChildrenArr);
    ResultObj->SetNumberField(TEXT("child_count"), SoundClass->ChildClasses.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// set_sound_mix_class_override
// Sets a sound class adjuster in a USoundMix. Loads the USoundMix and
// USoundClass, adds or updates an FSoundClassAdjuster in SoundClassEffects.
// ---------------------------------------------------------------------------
// MCP:set_sound_mix_class_override - Sets a sound class adjuster override in a USoundMix.  Loads the USoundMix and USoundClass, adds or updates an FSoundClassAdjuster in the mix's SoundClassEffects array, then saves.
//   @param sound_mix_path - Content path of the USoundMix asset.
//   @param sound_class_path - Content path of the USoundClass to adjust.
//   @param volume_adjuster - Adjuster.VolumeAdjuster (default 1.0).
//   @param pitch_adjuster - Adjuster.PitchAdjuster (default 1.0).
//   @param apply_to_children - Adjuster.bApplyToChildren (default false).
//   @returns { sound_mix_path: string, sound_class_path: string, success: bool, volume_adjuster: float, pitch_adjuster: float, apply_to_children: bool, total_adjusters: int, saved: bool }

TSharedPtr<FJsonObject> HandleSetSoundMixClassOverride(
    const TSharedPtr<FJsonObject>& Params)
{
    FString SoundMixPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_mix_path"), SoundMixPath)) { return Missing; }

    FString SoundClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sound_class_path"), SoundClassPath)) { return Missing; }

    USoundMix* SoundMix = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundAdvanced_LoadAsset(SoundMixPath, TEXT("USoundMix"), SoundMix)) { return LoadError; }

    USoundClass* SoundClass = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = SoundAdvanced_LoadAsset(SoundClassPath, TEXT("USoundClass"), SoundClass)) { return LoadError; }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetSoundMixOverride", "Set Sound Mix Class Override"));
    SoundMix->Modify();

    // Read optional adjuster values (defaults match FSoundClassAdjuster defaults).
    double VolumeAdjusterD = 1.0;
    Params->TryGetNumberField(TEXT("volume_adjuster"), VolumeAdjusterD);

    double PitchAdjusterD = 1.0;
    Params->TryGetNumberField(TEXT("pitch_adjuster"), PitchAdjusterD);

    bool bApplyToChildren = false;
    Params->TryGetBoolField(TEXT("apply_to_children"), bApplyToChildren);

    // Find existing adjuster for this sound class, or add a new one.
    FSoundClassAdjuster* ExistingAdjuster = nullptr;
    for (FSoundClassAdjuster& Adjuster : SoundMix->SoundClassEffects)
    {
        if (Adjuster.SoundClassObject == SoundClass)
        {
            ExistingAdjuster = &Adjuster;
            break;
        }
    }

    if (ExistingAdjuster)
    {
        // Update existing.
        ExistingAdjuster->VolumeAdjuster = static_cast<float>(VolumeAdjusterD);
        ExistingAdjuster->PitchAdjuster = static_cast<float>(PitchAdjusterD);
        ExistingAdjuster->bApplyToChildren = bApplyToChildren ? 1 : 0;
    }
    else
    {
        // Add new.
        FSoundClassAdjuster NewAdjuster;
        NewAdjuster.SoundClassObject = SoundClass;
        NewAdjuster.VolumeAdjuster = static_cast<float>(VolumeAdjusterD);
        NewAdjuster.PitchAdjuster = static_cast<float>(PitchAdjusterD);
        NewAdjuster.bApplyToChildren = bApplyToChildren ? 1 : 0;
        NewAdjuster.LowPassFilterFrequency = 20000.0f;
        NewAdjuster.VoiceCenterChannelVolumeAdjuster = 1.0f;
        SoundMix->SoundClassEffects.Add(NewAdjuster);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(SoundMix->GetOutermost(), SoundMix, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("sound_mix_path"), SoundMix->GetPathName());
    ResultObj->SetStringField(TEXT("sound_class_path"), SoundClass->GetPathName());
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetNumberField(TEXT("volume_adjuster"), static_cast<float>(VolumeAdjusterD));
    ResultObj->SetNumberField(TEXT("pitch_adjuster"), static_cast<float>(PitchAdjusterD));
    ResultObj->SetBoolField(TEXT("apply_to_children"), bApplyToChildren);
    ResultObj->SetNumberField(TEXT("total_adjusters"), SoundMix->SoundClassEffects.Num());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_sound_mix_class_override"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_metasound_preset"), &HandleCreateMetasoundPreset },
    { TEXT("get_metasound_preset_parameters"), &HandleGetMetasoundPresetParameters },
    { TEXT("get_sound_class_properties"), &HandleGetSoundClassProperties },
    { TEXT("set_metasound_preset_parameter"), &HandleSetMetasoundPresetParameter },
    { TEXT("set_sound_class_properties"), &HandleSetSoundClassProperties },
    { TEXT("set_sound_mix_class_override"), &HandleSetSoundMixClassOverride },
};
} // namespace UEAgentSoundAdvancedCommands
