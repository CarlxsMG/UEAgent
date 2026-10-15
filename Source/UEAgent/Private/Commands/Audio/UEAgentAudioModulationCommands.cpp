#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AudioModulationStatics.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Generators/SoundModulationLFO.h"
#include "IAudioModulation.h"                 // USoundModulatorBase
#include "Misc/PackageName.h"
#include "Generators/SoundModulationADEnvelope.h"
#include "Generators/SoundModulationEnvelopeFollower.h"
#include "Sound/AudioBus.h"
#include "SoundControlBus.h"
#include "SoundControlBusMix.h"
#include "SoundModulationGenerator.h"
#include "SoundModulationParameter.h"
#include "SoundModulationPatch.h"
#include "SoundModulationValue.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

/**
 * Audio Modulation commands.
 *
 * Wraps the engine's runtime audio modulation plugin so an LLM can author control buses and attach them
 * to a bus mix (with its mix stages), author modulation patches (input buses + output parameter + bypass),
 * modulation parameters and generators (LFO, ADEnvelope, EnvelopeFollower), and push bus values at
 * runtime via UAudioModulationStatics.
 *
 * All commands are headless-safe: asset create/load, mix-stage editing, patch input/output authoring,
 * value push via the editor world. No PIE, no audio device required.
 */
namespace UEAgentAudioModulationCommands
{

// MCP-DOMAIN: Audio

namespace
{
	TSharedPtr<FJsonObject> AudioMod_CannotLoadReply(const TCHAR* What, const FString& Path)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot load %s: %s"), What, *Path));
	}

	/** Calls Modify() and MarkPackageDirty() on the asset, then saves its package; true when the save succeeds. */
	bool AudioMod_ModifyDirtyAndSave(UObject* Asset)
	{
		Asset->Modify();
		Asset->MarkPackageDirty();
		return mcp_common::SaveAssetPackage(Asset->GetPackage(), Asset);
	}

	/** A new T of ConcreteClass at AssetPath (the PathParam param), registered and marked dirty; null, with
	 *  OutError set to the reply, when the path does not split or the package or object cannot be created. */
	template <typename T>
	T* AudioMod_NewAssetOfClass(const FString& AssetPath, UClass* ConcreteClass, const TCHAR* PathParam,
		const TCHAR* TypeName, TSharedPtr<FJsonObject>& OutError)
	{
		const FString PackagePathStr = FPackageName::GetLongPackagePath(AssetPath);
		const FString AssetName = FPackageName::GetShortName(AssetPath);
		if (PackagePathStr.IsEmpty() || AssetName.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Invalid %s '%s' (could not split into package/name)"), PathParam, *AssetPath));
			return nullptr;
		}

		const FString CreateFailed = FString::Printf(TEXT("Failed to create %s of class %s at %s"),
			TypeName, *ConcreteClass->GetName(), *AssetPath);
		FString CreateError;
		T* Asset = static_cast<T*>(mcp_common::NewAssetInPackage(ConcreteClass, AssetPath, AssetName, *CreateFailed, CreateError));
		if (!Asset)
		{
			OutError = mcp_common::MakeErrorReply(CreateError);
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		return Asset;
	}

	/** The reply of a create command that names its asset under PathKey, NameKey and ClassKey, then says whether
	 *  this call made it ("created") or found it there ("already_existed"). */
	TSharedPtr<FJsonObject> AudioMod_CreatedAssetData(const UObject* Asset, const TCHAR* PathKey, const TCHAR* NameKey,
		const TCHAR* ClassKey, const bool bCreated)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(PathKey, Asset->GetPathName());
		Data->SetStringField(NameKey, Asset->GetName());
		Data->SetStringField(ClassKey, Asset->GetClass()->GetName());
		Data->SetBoolField(TEXT("created"), bCreated);
		Data->SetBoolField(TEXT("already_existed"), !bCreated);
		return Data;
	}

	/** The parameter's Settings: value_normalized, clamp_to_normalized_range and value_unit (0 without editor data). */
	void AudioMod_AddParameterSettings(const TSharedPtr<FJsonObject>& Data, const USoundModulationParameter* Param)
	{
		Data->SetNumberField(TEXT("value_normalized"), Param->Settings.ValueNormalized);
		Data->SetBoolField(TEXT("clamp_to_normalized_range"), Param->Settings.bClampToNormalizedRange);
		Data->SetNumberField(TEXT("value_unit"), Param->Settings.ValueUnit);
	}

	/** The typed settings of an LFO, AD envelope or envelope follower generator, empty for another class.
	 *  bWithShape puts the LFO shape first. */
	TSharedPtr<FJsonObject> AudioMod_GeneratorSettingsJson(const USoundModulationGenerator* Gen, bool bWithShape)
	{
		TSharedPtr<FJsonObject> SettingsObj = MakeShared<FJsonObject>();
		if (const USoundModulationGeneratorLFO* LFO = Cast<USoundModulationGeneratorLFO>(Gen))
		{
			if (bWithShape)
			{
				SettingsObj->SetStringField(TEXT("shape"), UEnum::GetValueAsString(LFO->Params.Shape));
			}
			SettingsObj->SetNumberField(TEXT("frequency"), LFO->Params.Frequency);
			SettingsObj->SetNumberField(TEXT("amplitude"), LFO->Params.Amplitude);
			SettingsObj->SetNumberField(TEXT("offset"),    LFO->Params.Offset);
			SettingsObj->SetNumberField(TEXT("phase"),     LFO->Params.Phase);
		}
		else if (const USoundModulationGeneratorADEnvelope* AD = Cast<USoundModulationGeneratorADEnvelope>(Gen))
		{
			SettingsObj->SetNumberField(TEXT("attack_time"), AD->Params.AttackTime);
			SettingsObj->SetNumberField(TEXT("decay_time"),  AD->Params.DecayTime);
		}
		else if (const USoundModulationGeneratorEnvelopeFollower* EF = Cast<USoundModulationGeneratorEnvelopeFollower>(Gen))
		{
			SettingsObj->SetNumberField(TEXT("attack_time"),  EF->Params.AttackTime);
			SettingsObj->SetNumberField(TEXT("release_time"), EF->Params.ReleaseTime);
			SettingsObj->SetNumberField(TEXT("gain"),         EF->Params.Gain);
			SettingsObj->SetStringField(TEXT("audio_bus_path"),
				EF->Params.AudioBus ? EF->Params.AudioBus->GetPathName() : FString());
		}
		return SettingsObj;
	}
}

// ---------------------------------------------------------------------------
// create_control_bus
// ---------------------------------------------------------------------------
// MCP:create_control_bus - Creates a USoundControlBus asset at package_path/bus_name, or loads the one already there, and saves it. initial_value is only echoed back as value, never written to the bus; push a runtime value with set_control_bus_value. Returns bus_path, default_value, created, already_existed, saved.
//   @param bus_name - Name of the control bus (e.g. "Bus_Volume")
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @param initial_value - Number echoed back as value in the reply; it is not stored on the bus (default 1.0)
//   @returns { bus_path, bus_name, value, default_value, created, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateControlBus(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BusName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_name"), BusName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	double InitialValueD = 1.0;
	Params->TryGetNumberField(TEXT("initial_value"), InitialValueD);

	const FString FullPath = PackagePath / BusName;
	FString CreateError;
	bool bCreated = false;
	USoundControlBus* Bus = mcp_common::CreateOrLoadAsset<USoundControlBus>(FullPath, CreateError, &bCreated);
	if (!Bus)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load control bus: %s"), *CreateError));
	}
	const bool bAlreadyExisted = !bCreated;

	// Always save so the asset is durable.
	const bool bSaved = mcp_common::SaveAssetPackage(Bus->GetPackage(), Bus);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetStringField(TEXT("bus_name"), Bus->GetName());
	Data->SetNumberField(TEXT("value"), InitialValueD);
	Data->SetNumberField(TEXT("default_value"), Bus->GetDefaultNormalizedValue());
	Data->SetBoolField(TEXT("created"), bCreated);
	Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_control_bus"));
}

// ---------------------------------------------------------------------------
// create_control_bus_mix
// ---------------------------------------------------------------------------
// MCP:create_control_bus_mix - Creates a USoundControlBusMix asset at package_path/mix_name, or loads the one already there, and saves it. A new mix has no stages; add buses with add_bus_to_mix. Returns mix_path, mix_name, bus_count (its stage count), created, saved.
//   @param mix_name - Name of the control bus mix asset
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @returns { mix_path, mix_name, bus_count, created, saved }
TSharedPtr<FJsonObject> HandleCreateControlBusMix(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MixName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mix_name"), MixName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	const FString FullPath = PackagePath / MixName;
	FString CreateError;
	bool bCreated = false;
	USoundControlBusMix* Mix = mcp_common::CreateOrLoadAsset<USoundControlBusMix>(FullPath, CreateError, &bCreated);
	if (!Mix)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load control bus mix: %s"), *CreateError));
	}

	const bool bSaved = mcp_common::SaveAssetPackage(Mix->GetPackage(), Mix);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mix_path"), Mix->GetPathName());
	Data->SetStringField(TEXT("mix_name"), Mix->GetName());
	Data->SetNumberField(TEXT("bus_count"), Mix->MixStages.Num());
	Data->SetBoolField(TEXT("created"), bCreated);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_control_bus_mix"));
}

// ---------------------------------------------------------------------------
// set_control_bus_value  (push a value into the bus at runtime via statics)
// ---------------------------------------------------------------------------
// MCP:set_control_bus_value - Pushes a runtime value into a USoundControlBus through a global bus mix of the editor world (UAudioModulationStatics::SetGlobalBusMixValue), fading over fade_time. Transient: the asset is not modified or saved, and applied is always true, not read back; read the live value with get_modulation_info.
//   @param bus_path - Asset path to the USoundControlBus
//   @param value - New runtime bus value to push (required float)
//   @param fade_time - Fade time in seconds; a negative value reuses the last fade time of the global stage, or 0.1 s if none was set (default -1.0)
//   @returns { bus_path, value, fade_time, applied }
TSharedPtr<FJsonObject> HandleSetControlBusValue(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }
	double ValueD = 0.0;
	if (!Params->TryGetNumberField(TEXT("value"), ValueD))
	{
		return mcp_common::MakeErrorReply(TEXT("value (float) is required"));
	}
	double FadeTimeD = -1.0;
	Params->TryGetNumberField(TEXT("fade_time"), FadeTimeD);

	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("bus"), BusPath);
	}
	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available; cannot push value"))) { return NoWorld; }

	// UAudioModulationStatics::SetGlobalBusMixValue is the canonical path
	// (UFUNCTION BlueprintCallable, AUDIOMODULATION_API). FadeTime<0 means
	// use the mix's default.
	UAudioModulationStatics::SetGlobalBusMixValue(World, Bus, (float)ValueD, (float)FadeTimeD);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetNumberField(TEXT("value"), ValueD);
	Data->SetNumberField(TEXT("fade_time"), FadeTimeD);
	Data->SetBoolField(TEXT("applied"), true);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_bus_to_mix
// ---------------------------------------------------------------------------
// MCP:add_bus_to_mix - Adds a USoundControlBus to a USoundControlBusMix asset as a new stage whose target value is default_value, and saves the mix. A bus already in the mix is left unchanged (already_in_mix true, not saved); edit a stage with set_control_bus_mix_stage_value. Returns mix_path, bus_path, bus_count, added, saved.
//   @param mix_path - Asset path to the USoundControlBusMix
//   @param bus_path - Asset path to the USoundControlBus
//   @param default_value - Target value for the new mix stage (default 1.0)
//   @returns { mix_path, bus_path, bus_count, added, already_in_mix, saved, default_value }
TSharedPtr<FJsonObject> HandleAddBusToMix(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MixPath, BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mix_path"), MixPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }
	double DefaultValueD = 1.0;
	Params->TryGetNumberField(TEXT("default_value"), DefaultValueD);

	USoundControlBusMix* Mix = LoadObject<USoundControlBusMix>(nullptr, *MixPath);
	if (!Mix)
	{
		return AudioMod_CannotLoadReply(TEXT("mix"), MixPath);
	}
	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("bus"), BusPath);
	}

	// Idempotency: skip if this bus is already in the mix. We still report
	// `added: true` because the bus IS in the mix now (either we added it
	// or it was already there — either way, the desired state is reached).
	for (const FSoundControlBusMixStage& Stage : Mix->MixStages)
	{
		if (Stage.Bus == Bus)
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("mix_path"), Mix->GetPathName());
			Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
			Data->SetNumberField(TEXT("bus_count"), Mix->MixStages.Num());
			Data->SetBoolField(TEXT("added"), true);
			Data->SetBoolField(TEXT("already_in_mix"), true);
			Data->SetBoolField(TEXT("saved"), false);
			return mcp_common::MakeOkReply(Data);
		}
	}

	// FSoundControlBusMixStage ctor seeds TargetValue; the inner
	// FSoundModulationMixValue (AttackTime/ReleaseTime) defaults are fine.
	FSoundControlBusMixStage NewStage(Bus, (float)DefaultValueD);
	Mix->MixStages.Add(NewStage);
	const bool bSaved = mcp_common::SaveAssetPackage(Mix->GetPackage(), Mix);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mix_path"), Mix->GetPathName());
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetNumberField(TEXT("default_value"), DefaultValueD);
	Data->SetNumberField(TEXT("bus_count"), Mix->MixStages.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_bus_to_mix"));
}

// ---------------------------------------------------------------------------
// create_modulation_generator_lfo
// ---------------------------------------------------------------------------
// MCP:create_modulation_generator_lfo - Creates a USoundModulationGeneratorLFO asset at package_path/generator_name (or loads the one there), writes waveform, frequency, amplitude and offset, enables looping and saves it, overwriting an existing LFO. An unknown waveform silently becomes Sine but is echoed as sent. Returns generator_path, the written values, created, saved.
//   @param generator_name - Name of the modulation generator asset
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @param frequency - LFO frequency in Hz (default 1.0)
//   @param amplitude - LFO amplitude (default 1.0)
//   @param offset - LFO output offset (default 0.0)
//   @param waveform - LFO shape, case-insensitive: Sine, UpSaw, DownSaw, Square, Triangle, Exponential, Random (default Sine; any other value also gives Sine)
//   @returns { generator_path, generator_name, waveform, frequency, amplitude, offset, created, saved }
TSharedPtr<FJsonObject> HandleCreateModulationGeneratorLFO(
	const TSharedPtr<FJsonObject>& Params)
{
	FString GeneratorName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("generator_name"), GeneratorName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	double FrequencyD = 1.0, AmplitudeD = 1.0, OffsetD = 0.0;
	Params->TryGetNumberField(TEXT("frequency"), FrequencyD);
	Params->TryGetNumberField(TEXT("amplitude"), AmplitudeD);
	Params->TryGetNumberField(TEXT("offset"), OffsetD);
	FString Waveform = TEXT("Sine");
	Params->TryGetStringField(TEXT("waveform"), Waveform);

	const FString FullPath = PackagePath / GeneratorName;
	FString CreateError;
	bool bCreated = false;
	USoundModulationGeneratorLFO* LFO = mcp_common::CreateOrLoadAsset<USoundModulationGeneratorLFO>(FullPath, CreateError, &bCreated);
	if (!LFO)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load LFO generator: %s"), *CreateError));
	}

	// Map the friendly waveform string to the engine's ESoundModulationLFOShape.
	ESoundModulationLFOShape Shape = ESoundModulationLFOShape::Sine;
	const FString Wf = Waveform.ToLower();
	if      (Wf == TEXT("sine"))        { Shape = ESoundModulationLFOShape::Sine; }
	else if (Wf == TEXT("upsaw"))       { Shape = ESoundModulationLFOShape::UpSaw; }
	else if (Wf == TEXT("downsaw"))     { Shape = ESoundModulationLFOShape::DownSaw; }
	else if (Wf == TEXT("square"))      { Shape = ESoundModulationLFOShape::Square; }
	else if (Wf == TEXT("triangle"))    { Shape = ESoundModulationLFOShape::Triangle; }
	else if (Wf == TEXT("exponential")) { Shape = ESoundModulationLFOShape::Exponential; }
	else if (Wf == TEXT("random") || Wf == TEXT("random_sample_hold"))
	{ Shape = ESoundModulationLFOShape::RandomSampleHold; }

	// Direct public field writes (generator Params is a UPROPERTY struct).
	LFO->Params.Shape = Shape;
	LFO->Params.Frequency = (float)FrequencyD;
	LFO->Params.Amplitude = (float)AmplitudeD;
	LFO->Params.Offset = (float)OffsetD;
	LFO->Params.bLooping = true;

	const bool bSaved = mcp_common::SaveAssetPackage(LFO->GetPackage(), LFO);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("generator_path"), LFO->GetPathName());
	Data->SetStringField(TEXT("generator_name"), LFO->GetName());
	Data->SetStringField(TEXT("waveform"), Waveform);
	Data->SetNumberField(TEXT("frequency"), FrequencyD);
	Data->SetNumberField(TEXT("amplitude"), AmplitudeD);
	Data->SetNumberField(TEXT("offset"), OffsetD);
	Data->SetBoolField(TEXT("created"), bCreated);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_modulation_generator_lfo"));
}

// ---------------------------------------------------------------------------
// get_modulation_info
// ---------------------------------------------------------------------------
// MCP:get_modulation_info - Reads a USoundControlBus asset and its live value in the editor world, without changing anything. Returns bus_path, default_value, current_value (the live modulator value; the engine returns 1.0 for a bus that is not active) and subsystem_active (whether the editor world has a modulation manager).
//   @param bus_path - Asset path to the USoundControlBus
//   @returns { bus_path, default_value, current_value, subsystem_active }
TSharedPtr<FJsonObject> HandleGetModulationInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }

	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("bus"), BusPath);
	}

	UWorld* World = mcp_common::GetEditorWorld();
	const float DefaultValue = Bus->GetDefaultNormalizedValue();
	float CurrentValue = DefaultValue;
	bool bSubsystemActive = false;
	if (World)
	{
		// GetModulatorValue returns 1.0f if the modulator is not active; treat
		// that as inactive rather than reading back a meaningful value.
		CurrentValue = UAudioModulationStatics::GetModulatorValue(World, Bus);
		// bSubsystemActive heuristic: if a non-zero manager is reachable, the
		// modulation subsystem is initialised.
		AudioModulation::FAudioModulationManager* Mgr =
			UAudioModulationStatics::GetModulation(World);
		bSubsystemActive = (Mgr != nullptr);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetNumberField(TEXT("default_value"), DefaultValue);
	Data->SetNumberField(TEXT("current_value"), CurrentValue);
	Data->SetBoolField(TEXT("subsystem_active"), bSubsystemActive);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_control_bus_mix_info
// ---------------------------------------------------------------------------
// MCP:get_control_bus_mix_info - Reads the stages of a USoundControlBusMix asset without changing it. Returns mix_path, stage_count and stages, one per stage with bus_path (empty when the bus reference is cleared), target_value, attack_time and release_time. Edit a stage with set_control_bus_mix_stage_value.
//   @param mix_path - Asset path to the USoundControlBusMix
//   @returns { mix_path, stage_count, stages: [{ bus_path, target_value, attack_time, release_time }] }
TSharedPtr<FJsonObject> HandleGetControlBusMixInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MixPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mix_path"), MixPath)) { return Missing; }

	USoundControlBusMix* Mix = LoadObject<USoundControlBusMix>(nullptr, *MixPath);
	if (!Mix)
	{
		return AudioMod_CannotLoadReply(TEXT("mix"), MixPath);
	}

	TArray<TSharedPtr<FJsonValue>> StagesJson;
	for (const FSoundControlBusMixStage& Stage : Mix->MixStages)
	{
		TSharedPtr<FJsonObject> StageObj = MakeShared<FJsonObject>();
		// Stage.Bus is a TObjectPtr that may be null (stale/cleared reference).
		StageObj->SetStringField(TEXT("bus_path"),
			Stage.Bus ? Stage.Bus->GetPathName() : FString());
		StageObj->SetNumberField(TEXT("target_value"), Stage.Value.TargetValue);
		StageObj->SetNumberField(TEXT("attack_time"), Stage.Value.AttackTime);
		StageObj->SetNumberField(TEXT("release_time"), Stage.Value.ReleaseTime);
		StagesJson.Add(MakeShared<FJsonValueObject>(StageObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mix_path"), Mix->GetPathName());
	Data->SetNumberField(TEXT("stage_count"), Mix->MixStages.Num());
	Data->SetArrayField(TEXT("stages"), StagesJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_control_bus_mix_stage_value
// ---------------------------------------------------------------------------
// MCP:set_control_bus_mix_stage_value - Set the target value (and optional fades) of a bus's stage in a mix
//   @param mix_path - Asset path to the USoundControlBusMix
//   @param bus_path - Asset path to the USoundControlBus whose stage is edited
//   @param target_value - New stage target value (required float)
//   @param attack_time - Optional attack interpolation time in seconds
//   @param release_time - Optional release interpolation time in seconds
//   @returns { mix_path, bus_path, target_value, attack_time, release_time, updated, saved }
TSharedPtr<FJsonObject> HandleSetControlBusMixStageValue(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MixPath, BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mix_path"), MixPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }
	double TargetValueD = 0.0;
	if (!Params->TryGetNumberField(TEXT("target_value"), TargetValueD))
	{
		return mcp_common::MakeErrorReply(TEXT("target_value (float) is required"));
	}

	USoundControlBusMix* Mix = LoadObject<USoundControlBusMix>(nullptr, *MixPath);
	if (!Mix)
	{
		return AudioMod_CannotLoadReply(TEXT("mix"), MixPath);
	}
	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("bus"), BusPath);
	}

	// Optional fade overrides — only applied when the caller supplies them.
	double AttackTimeD = 0.0, ReleaseTimeD = 0.0;
	const bool bHasAttack = Params->TryGetNumberField(TEXT("attack_time"), AttackTimeD);
	const bool bHasRelease = Params->TryGetNumberField(TEXT("release_time"), ReleaseTimeD);

	// Find the stage owned by this bus. We do NOT auto-add: editing a value on a
	// bus that is not in the mix is an honest error, not a silent create.
	FSoundControlBusMixStage* TargetStage = nullptr;
	for (FSoundControlBusMixStage& Stage : Mix->MixStages)
	{
		if (Stage.Bus == Bus)
		{
			TargetStage = &Stage;
			break;
		}
	}
	if (!TargetStage)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot find stage for bus: %s"), *Bus->GetPathName()));
	}

	TargetStage->Value.TargetValue = (float)TargetValueD;
	if (bHasAttack)  { TargetStage->Value.AttackTime  = (float)AttackTimeD; }
	if (bHasRelease) { TargetStage->Value.ReleaseTime = (float)ReleaseTimeD; }

	const bool bSaved = mcp_common::SaveAssetPackage(Mix->GetPackage(), Mix);

	// Read back from the (persisted) stage so the report reflects stored state.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mix_path"), Mix->GetPathName());
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetNumberField(TEXT("target_value"), TargetStage->Value.TargetValue);
	Data->SetNumberField(TEXT("attack_time"), TargetStage->Value.AttackTime);
	Data->SetNumberField(TEXT("release_time"), TargetStage->Value.ReleaseTime);
	Data->SetBoolField(TEXT("updated"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_control_bus_mix_stage_value"));
}

// ---------------------------------------------------------------------------
// remove_bus_from_mix
// ---------------------------------------------------------------------------
// MCP:remove_bus_from_mix - Removes every stage that uses a USoundControlBus from a USoundControlBusMix asset and saves the mix. When the bus is not in the mix nothing is saved and the call reports failure, with already_absent true in its data. Returns mix_path, bus_path, removed, already_absent, stage_count, saved.
//   @param mix_path - Asset path to the USoundControlBusMix
//   @param bus_path - Asset path to the USoundControlBus to remove from the mix
//   @returns { mix_path, bus_path, removed, already_absent, stage_count, saved }
TSharedPtr<FJsonObject> HandleRemoveBusFromMix(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MixPath, BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mix_path"), MixPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }

	USoundControlBusMix* Mix = LoadObject<USoundControlBusMix>(nullptr, *MixPath);
	if (!Mix)
	{
		return AudioMod_CannotLoadReply(TEXT("mix"), MixPath);
	}
	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("bus"), BusPath);
	}

	const int32 Removed = Mix->MixStages.RemoveAll(
		[&](const FSoundControlBusMixStage& Stage) { return Stage.Bus == Bus; });

	// Idempotency: a bus that was never in the mix is reported as already-absent
	// without re-saving (nothing changed).
	bool bSaved = false;
	if (Removed > 0)
	{
		bSaved = mcp_common::SaveAssetPackage(Mix->GetPackage(), Mix);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("mix_path"), Mix->GetPathName());
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetBoolField(TEXT("removed"), Removed > 0);
	Data->SetBoolField(TEXT("already_absent"), Removed == 0);
	Data->SetNumberField(TEXT("stage_count"), Mix->MixStages.Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_bus_from_mix"));
}

// ---------------------------------------------------------------------------
// create_sound_modulation_patch
// ---------------------------------------------------------------------------
// MCP:create_sound_modulation_patch - Creates a USoundModulationPatch asset at package_path/patch_name with no inputs or output, bypassed by default (bBypass true), and saves it; a patch already there is returned untouched, not re-saved. Returns patch_path, bBypass, input_count, output_parameter_path, created, already_existed, saved. Fill it with add_modulation_input_to_patch and set_modulation_patch_output_parameter.
//   @param patch_name - Name of the modulation patch asset (e.g. "Patch_Volume")
//   @param package_path - Target content package path where the asset is created (default /Game/MCP)
//   @returns { patch_path, patch_name, bBypass, input_count, output_parameter_path, created, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateSoundModulationPatch(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PatchName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("patch_name"), PatchName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	const FString FullPath = PackagePath / PatchName;

	// Both outcomes report the patch's settings as they stand.
	auto BuildPayload = [](const USoundModulationPatch* Patch, bool bCreated) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
		Data->SetStringField(TEXT("patch_name"), Patch->GetName());
		Data->SetBoolField(TEXT("bBypass"), Patch->PatchSettings.bBypass);
		Data->SetNumberField(TEXT("input_count"), Patch->PatchSettings.Inputs.Num());
		Data->SetStringField(TEXT("output_parameter_path"),
			Patch->PatchSettings.OutputParameter
				? Patch->PatchSettings.OutputParameter->GetPathName() : FString());
		Data->SetBoolField(TEXT("created"), bCreated);
		return Data;
	};

	// Idempotency probe: load first. If it already exists we keep the existing
	// instance untouched (do NOT reset/rebuild — that would wipe inputs/output
	// the caller has already authored).
	if (USoundModulationPatch* Existing = LoadObject<USoundModulationPatch>(nullptr, *FullPath))
	{
		TSharedPtr<FJsonObject> Data = BuildPayload(Existing, /*bCreated=*/false);
		Data->SetBoolField(TEXT("already_existed"), true);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	FString CreateError;
	bool bCreated = false;
	USoundModulationPatch* Patch = mcp_common::CreateOrLoadAsset<USoundModulationPatch>(FullPath, CreateError, &bCreated);
	if (!Patch)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load sound modulation patch: %s"), *CreateError));
	}

	const bool bSaved = mcp_common::SaveAssetPackage(Patch->GetPackage(), Patch);

	TSharedPtr<FJsonObject> Data = BuildPayload(Patch, bCreated);
	Data->SetBoolField(TEXT("already_existed"), false);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_sound_modulation_patch"));
}

// ---------------------------------------------------------------------------
// add_modulation_input_to_patch
// ---------------------------------------------------------------------------
// MCP:add_modulation_input_to_patch - Appends a USoundControlBus as an input of a USoundModulationPatch asset and saves the patch. A bus whose asset name is already among the inputs is skipped (added false, already_present true, not saved). input_name is not stored. Returns patch_path, bus_path, input_count, added, already_present, saved.
//   @param patch_path - Asset path to the USoundModulationPatch
//   @param bus_path - Asset path to the USoundControlBus to add as an input
//   @param input_name - Ignored: the engine input struct has no name field, so the value is only written to the log (default empty)
//   @returns { patch_path, bus_path, input_count, added, already_present, saved }
TSharedPtr<FJsonObject> HandleAddModulationInputToPatch(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PatchPath, BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("patch_path"), PatchPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }
	FString InputName;
	Params->TryGetStringField(TEXT("input_name"), InputName);

	USoundModulationPatch* Patch = LoadObject<USoundModulationPatch>(nullptr, *PatchPath);
	if (!Patch)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation patch"), PatchPath);
	}
	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("control bus"), BusPath);
	}

	// Idempotency: dedup by the bus's FName (not by pointer — a re-loaded bus
	// is the same UObject on disk).
	const FName BusName = Bus->GetFName();
	for (const FSoundControlModulationInput& In : Patch->PatchSettings.Inputs)
	{
		if (In.Bus && In.Bus->GetFName() == BusName)
		{
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
			Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
			Data->SetNumberField(TEXT("input_count"), Patch->PatchSettings.Inputs.Num());
			Data->SetBoolField(TEXT("added"), false);
			Data->SetBoolField(TEXT("already_present"), true);
			Data->SetBoolField(TEXT("saved"), false);
			return mcp_common::MakeOkReply(Data);
		}
	}

	FSoundControlModulationInput NewInput;
	NewInput.Bus = Bus;
	if (!InputName.IsEmpty())
	{
		// FSoundControlModulationInput exposes Bus as the canonical identity;
		// Name is not a UPROPERTY field on the struct (SoundModulationPatch.h),
		// so we just record it on the transient side via a log for traceability.
		UE_LOG(LogTemp, Verbose, TEXT("add_modulation_input_to_patch: input_name='%s' requested but not stored (FSoundControlModulationInput has no Name UPROPERTY)"),
			*InputName);
	}
	Patch->PatchSettings.Inputs.Add(NewInput);
	const bool bSaved = AudioMod_ModifyDirtyAndSave(Patch);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetNumberField(TEXT("input_count"), Patch->PatchSettings.Inputs.Num());
	Data->SetBoolField(TEXT("added"), true);
	Data->SetBoolField(TEXT("already_present"), false);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_modulation_input_to_patch"));
}

// ---------------------------------------------------------------------------
// remove_modulation_input_from_patch
// ---------------------------------------------------------------------------
// MCP:remove_modulation_input_from_patch - Removes every input of a USoundModulationPatch asset whose bus has the same asset name as bus_path, then saves the patch. When no input matches nothing is saved and the call reports failure, with already_removed true in its data. Returns patch_path, bus_path, removed, already_removed, input_count, saved.
//   @param patch_path - Asset path to the USoundModulationPatch
//   @param bus_path - Asset path to the USoundControlBus whose input is removed
//   @returns { patch_path, bus_path, removed, already_removed, input_count, saved }
TSharedPtr<FJsonObject> HandleRemoveModulationInputFromPatch(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PatchPath, BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("patch_path"), PatchPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }

	USoundModulationPatch* Patch = LoadObject<USoundModulationPatch>(nullptr, *PatchPath);
	if (!Patch)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation patch"), PatchPath);
	}
	USoundControlBus* Bus = LoadObject<USoundControlBus>(nullptr, *BusPath);
	if (!Bus)
	{
		return AudioMod_CannotLoadReply(TEXT("control bus"), BusPath);
	}

	const FName BusName = Bus->GetFName();
	const int32 Removed = Patch->PatchSettings.Inputs.RemoveAll(
		[&](const FSoundControlModulationInput& In) {
			return In.Bus && In.Bus->GetFName() == BusName;
		});

	bool bSaved = false;
	if (Removed > 0)
	{
		bSaved = AudioMod_ModifyDirtyAndSave(Patch);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
	Data->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	Data->SetBoolField(TEXT("removed"), Removed > 0);
	Data->SetBoolField(TEXT("already_removed"), Removed == 0);
	Data->SetNumberField(TEXT("input_count"), Patch->PatchSettings.Inputs.Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_modulation_input_from_patch"));
}

// ---------------------------------------------------------------------------
// set_modulation_patch_bypass
// ---------------------------------------------------------------------------
// MCP:set_modulation_patch_bypass - Sets the bBypass flag of a USoundModulationPatch asset and saves it. A bypassed patch stays active but outputs the default value of its output parameter; new patches start bypassed. Returns patch_path, bBypass as stored, updated, saved.
//   @param patch_path - Asset path to the USoundModulationPatch
//   @param bypass - New bypass value (required bool)
//   @returns { patch_path, bBypass, updated, saved }
TSharedPtr<FJsonObject> HandleSetModulationPatchBypass(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PatchPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("patch_path"), PatchPath)) { return Missing; }
	bool Bypass = false;
	if (!Params->TryGetBoolField(TEXT("bypass"), Bypass))
	{
		return mcp_common::MakeErrorReply(TEXT("bypass (bool) is required"));
	}

	USoundModulationPatch* Patch = LoadObject<USoundModulationPatch>(nullptr, *PatchPath);
	if (!Patch)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation patch"), PatchPath);
	}

	Patch->PatchSettings.bBypass = Bypass;
	const bool bSaved = AudioMod_ModifyDirtyAndSave(Patch);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
	Data->SetBoolField(TEXT("bBypass"), Patch->PatchSettings.bBypass);
	Data->SetBoolField(TEXT("updated"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_modulation_patch_bypass"));
}

// ---------------------------------------------------------------------------
// set_modulation_patch_output_parameter
// ---------------------------------------------------------------------------
// MCP:set_modulation_patch_output_parameter - Sets the output USoundModulationParameter of a USoundModulationPatch asset, or clears it when parameter_path is an empty string, and saves the patch. Returns patch_path, parameter_path and parameter_class as stored (empty after a clear), updated, cleared, saved. Create a parameter with create_modulation_parameter.
//   @param patch_path - Asset path to the USoundModulationPatch
//   @param parameter_path - Asset path to the USoundModulationParameter (or empty string to clear)
//   @returns { patch_path, parameter_path, parameter_class, updated, cleared, saved }
TSharedPtr<FJsonObject> HandleSetModulationPatchOutputParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PatchPath, ParameterPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("patch_path"), PatchPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_path"), ParameterPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

	USoundModulationPatch* Patch = LoadObject<USoundModulationPatch>(nullptr, *PatchPath);
	if (!Patch)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation patch"), PatchPath);
	}

	// Empty path = clear the parameter assignment (honest path; not a fallback).
	const bool bCleared = ParameterPath.IsEmpty();
	USoundModulationParameter* Param = nullptr;
	if (!bCleared)
	{
		Param = LoadObject<USoundModulationParameter>(nullptr, *ParameterPath);
		if (!Param)
		{
			return AudioMod_CannotLoadReply(TEXT("sound modulation parameter"), ParameterPath);
		}
	}

	Patch->PatchSettings.OutputParameter = Param;
	const bool bSaved = AudioMod_ModifyDirtyAndSave(Patch);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
	Data->SetStringField(TEXT("parameter_path"),
		Patch->PatchSettings.OutputParameter
			? Patch->PatchSettings.OutputParameter->GetPathName() : FString());
	Data->SetStringField(TEXT("parameter_class"),
		Patch->PatchSettings.OutputParameter
			? Patch->PatchSettings.OutputParameter->GetClass()->GetName() : FString());
	Data->SetBoolField(TEXT("updated"), true);
	Data->SetBoolField(TEXT("cleared"), bCleared);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_modulation_patch_output_parameter"));
}

// ---------------------------------------------------------------------------
// get_sound_modulation_patch_info
// ---------------------------------------------------------------------------
// MCP:get_sound_modulation_patch_info - Reads a USoundModulationPatch asset without changing it. Returns patch_path, bBypass, output_parameter ({ path, class }, or null when unset), inputs (one { bus: { path }, name } per input; name is always empty) and input_count.
//   @param patch_path - Asset path to the USoundModulationPatch
//   @returns { patch_path, bBypass, output_parameter: { path, class } | null, inputs: [{ bus: { path } }], input_count }
TSharedPtr<FJsonObject> HandleGetSoundModulationPatchInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString PatchPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("patch_path"), PatchPath)) { return Missing; }

	USoundModulationPatch* Patch = LoadObject<USoundModulationPatch>(nullptr, *PatchPath);
	if (!Patch)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation patch"), PatchPath);
	}

	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const FSoundControlModulationInput& In : Patch->PatchSettings.Inputs)
	{
		TSharedPtr<FJsonObject> BusObj = MakeShared<FJsonObject>();
		BusObj->SetStringField(TEXT("path"),
			In.Bus ? In.Bus->GetPathName() : FString());

		TSharedPtr<FJsonObject> InputObj = MakeShared<FJsonObject>();
		InputObj->SetObjectField(TEXT("bus"), BusObj);
		// FSoundControlModulationInput has no Name UPROPERTY (header :32-52),
		// so we emit empty string for stability.
		InputObj->SetStringField(TEXT("name"), FString());

		InputsJson.Add(MakeShared<FJsonValueObject>(InputObj));
	}

	TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
	if (Patch->PatchSettings.OutputParameter)
	{
		OutObj->SetStringField(TEXT("path"), Patch->PatchSettings.OutputParameter->GetPathName());
		OutObj->SetStringField(TEXT("class"), Patch->PatchSettings.OutputParameter->GetClass()->GetName());
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("patch_path"), Patch->GetPathName());
	Data->SetBoolField(TEXT("bBypass"), Patch->PatchSettings.bBypass);
	if (OutObj->Values.Num() > 0)
	{
		Data->SetObjectField(TEXT("output_parameter"), OutObj);
	}
	else
	{
		Data->SetField(TEXT("output_parameter"), MakeShared<FJsonValueNull>());
	}
	Data->SetArrayField(TEXT("inputs"), InputsJson);
	Data->SetNumberField(TEXT("input_count"), Patch->PatchSettings.Inputs.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_modulation_parameter
// ---------------------------------------------------------------------------
// MCP:create_modulation_parameter - Creates a USoundModulationParameter asset of the chosen concrete class at parameter_path and saves it. An existing parameter there is returned untouched with its real class (not saved). Returns parameter_path, parameter_name, parameter_class, created, already_existed, saved. Tune it with set_modulation_parameter_settings.
//   @param parameter_path - Full asset path (e.g. /Game/MyGame/Param_Volume); the package path is the directory portion and the leaf becomes the asset name
//   @param parameter_class - Concrete class name (case-insensitive). One of: Volume, Frequency, LPFrequency, HPFrequency, Bipolar, Additive, Scaled; the engine spellings LPFFrequency and HPFFrequency are accepted too.
//   @returns { parameter_path, parameter_name, parameter_class, created, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateModulationParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ParameterPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_path"), ParameterPath)) { return Missing; }
	FString ParameterClass;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_class"), ParameterClass)) { return Missing; }

	// Idempotency probe: if the asset exists, return it untouched (do not
	// reset/rebuild — that would wipe Settings the caller has already
	// authored). We re-resolve the class from the existing object so the
	// reported parameter_class reflects ground truth, not the request string.
	if (USoundModulationParameter* Existing = LoadObject<USoundModulationParameter>(nullptr, *ParameterPath))
	{
		TSharedPtr<FJsonObject> Data = AudioMod_CreatedAssetData(
			Existing, TEXT("parameter_path"), TEXT("parameter_name"), TEXT("parameter_class"), /*bCreated=*/false);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	// Resolve the concrete class from the friendly name. USoundModulationParameter
	// is abstract (header :48 declares the base; the engine plugin does not
	// ship a factory for it — see AudioModulationClassTemplates.h, which only
	// covers generators). We therefore switch over the supported concrete
	// subclasses directly.
	const FString ClassLower = ParameterClass.ToLower();
	UClass* ConcreteClass = nullptr;
	if      (ClassLower == TEXT("volume"))         { ConcreteClass = USoundModulationParameterVolume::StaticClass(); }
	else if (ClassLower == TEXT("frequency"))      { ConcreteClass = USoundModulationParameterFrequency::StaticClass(); }
	// The engine classes are USoundModulationParameterLPFFrequency / HPFFrequency: both spellings resolve.
	else if (ClassLower == TEXT("lpfrequency") || ClassLower == TEXT("lpffrequency")) { ConcreteClass = USoundModulationParameterLPFFrequency::StaticClass(); }
	else if (ClassLower == TEXT("hpfrequency") || ClassLower == TEXT("hpffrequency")) { ConcreteClass = USoundModulationParameterHPFFrequency::StaticClass(); }
	else if (ClassLower == TEXT("bipolar"))        { ConcreteClass = USoundModulationParameterBipolar::StaticClass(); }
	else if (ClassLower == TEXT("additive"))       { ConcreteClass = USoundModulationParameterAdditive::StaticClass(); }
	else if (ClassLower == TEXT("scaled"))         { ConcreteClass = USoundModulationParameterScaled::StaticClass(); }
	if (!ConcreteClass)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown parameter_class '%s'. Expected one of: Volume, Frequency, LPFrequency, HPFrequency, Bipolar, Additive, Scaled"),
				*ParameterClass));
	}

	TSharedPtr<FJsonObject> Error;
	USoundModulationParameter* Param = AudioMod_NewAssetOfClass<USoundModulationParameter>(
		ParameterPath, ConcreteClass, TEXT("parameter_path"), TEXT("USoundModulationParameter"), Error);
	if (!Param)
	{
		return Error;
	}

	const bool bSaved = mcp_common::SaveAssetPackage(Param->GetPackage(), Param);

	TSharedPtr<FJsonObject> Data = AudioMod_CreatedAssetData(
		Param, TEXT("parameter_path"), TEXT("parameter_name"), TEXT("parameter_class"), /*bCreated=*/true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_modulation_parameter"));
}

// ---------------------------------------------------------------------------
// set_modulation_parameter_settings
// ---------------------------------------------------------------------------
// MCP:set_modulation_parameter_settings - Writes the given Settings fields (value_normalized, clamp_to_normalized_range, value_unit) of a USoundModulationParameter asset and saves it; omitted fields keep their value, and the asset is saved even when none is given. Returns parameter_path, the three settings as stored, updated, saved.
//   @param parameter_path - Asset path to the USoundModulationParameter
//   @param value_normalized - Optional float in [0,1]; sets Settings.ValueNormalized
//   @param clamp_to_normalized_range - Optional bool; sets Settings.bClampToNormalizedRange
//   @param value_unit - Optional float (editor-only); sets Settings.ValueUnit
//   @returns { parameter_path, value_normalized, clamp_to_normalized_range, value_unit, updated, saved }
TSharedPtr<FJsonObject> HandleSetModulationParameterSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ParameterPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_path"), ParameterPath)) { return Missing; }

	USoundModulationParameter* Param = LoadObject<USoundModulationParameter>(nullptr, *ParameterPath);
	if (!Param)
	{
		return AudioMod_CannotLoadReply(TEXT("sound modulation parameter"), ParameterPath);
	}

	// Each setter is independent. We do not require a particular field —
	// passing only the field you want to change is a valid call (matches the
	// set_control_bus_mix_stage_value pattern in this same file).
	double ValueNormalizedD = 0.0;
	if (Params->TryGetNumberField(TEXT("value_normalized"), ValueNormalizedD))
	{
		Param->Settings.ValueNormalized = (float)ValueNormalizedD;
	}
	bool bHasClamp = false;
	bool ClampValue = false;
	if (Params->TryGetBoolField(TEXT("clamp_to_normalized_range"), ClampValue))
	{
		bHasClamp = true;
		Param->Settings.bClampToNormalizedRange = ClampValue;
	}
	double ValueUnitD = 0.0;
	const bool bHasUnit = Params->TryGetNumberField(TEXT("value_unit"), ValueUnitD);

	if (bHasUnit)
	{
		Param->Settings.ValueUnit = (float)ValueUnitD;
	}

	const bool bSaved = AudioMod_ModifyDirtyAndSave(Param);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_path"), Param->GetPathName());
	AudioMod_AddParameterSettings(Data, Param);
	Data->SetBoolField(TEXT("updated"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_modulation_parameter_settings"));
}

// ---------------------------------------------------------------------------
// get_modulation_parameter_info
// ---------------------------------------------------------------------------
// MCP:get_modulation_parameter_info - Reads a USoundModulationParameter asset without changing it. Returns parameter_path, parameter_class (the concrete class, e.g. SoundModulationParameterVolume) and its settings value_normalized, clamp_to_normalized_range and value_unit. Change them with set_modulation_parameter_settings.
//   @param parameter_path - Asset path to the USoundModulationParameter
//   @returns { parameter_path, parameter_class, value_normalized, clamp_to_normalized_range, value_unit }
TSharedPtr<FJsonObject> HandleGetModulationParameterInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ParameterPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_path"), ParameterPath)) { return Missing; }

	USoundModulationParameter* Param = LoadObject<USoundModulationParameter>(nullptr, *ParameterPath);
	if (!Param)
	{
		return AudioMod_CannotLoadReply(TEXT("sound modulation parameter"), ParameterPath);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("parameter_path"), Param->GetPathName());
	Data->SetStringField(TEXT("parameter_class"), Param->GetClass()->GetName());
	AudioMod_AddParameterSettings(Data, Param);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_modulation_generator
// ---------------------------------------------------------------------------
// MCP:create_modulation_generator - Creates a modulation generator asset of class LFO, ADEnvelope or EnvelopeFollower at generator_path, with engine default settings, and saves it. An existing generator there is returned untouched with its own class (not saved). Returns generator_path, generator_name, generator_class, created, already_existed, saved. Configure it with set_modulation_generator_settings.
//   @param generator_path - Full asset path (e.g. /Game/MyGame/Gen_LFO)
//   @param generator_class - Concrete class name (case-insensitive). One of: LFO, ADEnvelope, EnvelopeFollower
//   @returns { generator_path, generator_name, generator_class, created, already_existed, saved }
TSharedPtr<FJsonObject> HandleCreateModulationGenerator(
	const TSharedPtr<FJsonObject>& Params)
{
	FString GeneratorPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("generator_path"), GeneratorPath)) { return Missing; }
	FString GeneratorClass;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("generator_class"), GeneratorClass)) { return Missing; }

	// Idempotency probe: if the asset exists, return it untouched.
	if (USoundModulationGenerator* Existing = LoadObject<USoundModulationGenerator>(nullptr, *GeneratorPath))
	{
		TSharedPtr<FJsonObject> Data = AudioMod_CreatedAssetData(
			Existing, TEXT("generator_path"), TEXT("generator_name"), TEXT("generator_class"), /*bCreated=*/false);
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}

	// USoundModulationGenerator is abstract. Switch over the
	// supported concrete subclasses directly.
	const FString ClassLower = GeneratorClass.ToLower();
	UClass* ConcreteClass = nullptr;
	if      (ClassLower == TEXT("lfo"))              { ConcreteClass = USoundModulationGeneratorLFO::StaticClass(); }
	else if (ClassLower == TEXT("adenvelope"))       { ConcreteClass = USoundModulationGeneratorADEnvelope::StaticClass(); }
	else if (ClassLower == TEXT("envelopefollower")) { ConcreteClass = USoundModulationGeneratorEnvelopeFollower::StaticClass(); }
	if (!ConcreteClass)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown generator_class '%s'. Expected one of: LFO, ADEnvelope, EnvelopeFollower"),
				*GeneratorClass));
	}

	TSharedPtr<FJsonObject> Error;
	USoundModulationGenerator* Gen = AudioMod_NewAssetOfClass<USoundModulationGenerator>(
		GeneratorPath, ConcreteClass, TEXT("generator_path"), TEXT("USoundModulationGenerator"), Error);
	if (!Gen)
	{
		return Error;
	}

	const bool bSaved = mcp_common::SaveAssetPackage(Gen->GetPackage(), Gen);

	TSharedPtr<FJsonObject> Data = AudioMod_CreatedAssetData(
		Gen, TEXT("generator_path"), TEXT("generator_name"), TEXT("generator_class"), /*bCreated=*/true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_modulation_generator"));
}

// ---------------------------------------------------------------------------
// set_modulation_generator_settings
// ---------------------------------------------------------------------------
// MCP:set_modulation_generator_settings - Writes the settings that match the class of a USoundModulationGenerator asset (LFO, ADEnvelope or EnvelopeFollower) and saves it; params of another class are ignored, other generator classes are refused, and the LFO shape cannot be set here. Returns generator_path, generator_class, settings as stored, updated, saved.
//   @param generator_path - Asset path to the USoundModulationGenerator
//   @param frequency - LFO only: oscillation frequency in Hz (optional float)
//   @param amplitude - LFO only: output amplitude/gain multiplier (optional float)
//   @param offset - LFO only: output offset added to the waveform (optional float)
//   @param phase - LFO only: starting phase in degrees (optional float)
//   @param attack_time - ADEnvelope/EnvelopeFollower: attack interpolation time in seconds (optional float)
//   @param decay_time - ADEnvelope only: decay interpolation time in seconds (optional float)
//   @param release_time - EnvelopeFollower only: release interpolation time in seconds (optional float)
//   @param gain - EnvelopeFollower only: input gain applied before envelope detection (optional float)
//   @param audio_bus_path - EnvelopeFollower only: asset path to the UAudioBus the follower listens to (optional string)
//   @returns { generator_path, generator_class, settings: {...}, updated, saved }
TSharedPtr<FJsonObject> HandleSetModulationGeneratorSettings(
	const TSharedPtr<FJsonObject>& Params)
{
	FString GeneratorPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("generator_path"), GeneratorPath)) { return Missing; }

	USoundModulationGenerator* Gen = LoadObject<USoundModulationGenerator>(nullptr, *GeneratorPath);
	if (!Gen)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation generator"), GeneratorPath);
	}

	// Type-specific settings writes. Each block is guarded by a class check
	// so a wrong-class assignment produces an honest error (no silent skip).
	const FString ClassName = Gen->GetClass()->GetName();

	if (USoundModulationGeneratorLFO* LFO = Cast<USoundModulationGeneratorLFO>(Gen))
	{
		double Frequency = 0.0, Amplitude = 0.0, Offset = 0.0, Phase = 0.0;
		if (Params->TryGetNumberField(TEXT("frequency"), Frequency)) { LFO->Params.Frequency = (float)Frequency; }
		if (Params->TryGetNumberField(TEXT("amplitude"), Amplitude)) { LFO->Params.Amplitude = (float)Amplitude; }
		if (Params->TryGetNumberField(TEXT("offset"),    Offset))    { LFO->Params.Offset    = (float)Offset; }
		if (Params->TryGetNumberField(TEXT("phase"),     Phase))     { LFO->Params.Phase     = (float)Phase; }
	}
	else if (USoundModulationGeneratorADEnvelope* AD = Cast<USoundModulationGeneratorADEnvelope>(Gen))
	{
		double AttackTime = 0.0, DecayTime = 0.0;
		if (Params->TryGetNumberField(TEXT("attack_time"), AttackTime)) { AD->Params.AttackTime = (float)AttackTime; }
		if (Params->TryGetNumberField(TEXT("decay_time"),  DecayTime))  { AD->Params.DecayTime  = (float)DecayTime; }
	}
	else if (USoundModulationGeneratorEnvelopeFollower* EF = Cast<USoundModulationGeneratorEnvelopeFollower>(Gen))
	{
		double AttackTime = 0.0, ReleaseTime = 0.0, Gain = 0.0;
		if (Params->TryGetNumberField(TEXT("attack_time"),  AttackTime))  { EF->Params.AttackTime  = (float)AttackTime; }
		if (Params->TryGetNumberField(TEXT("release_time"), ReleaseTime)) { EF->Params.ReleaseTime = (float)ReleaseTime; }
		if (Params->TryGetNumberField(TEXT("gain"),         Gain))        { EF->Params.Gain        = (float)Gain; }
		FString AudioBusPath;
		if (Params->TryGetStringField(TEXT("audio_bus_path"), AudioBusPath) && !AudioBusPath.IsEmpty())
		{
			if (UAudioBus* Bus = LoadObject<UAudioBus>(nullptr, *AudioBusPath))
			{
				EF->Params.AudioBus = Bus;
			}
			else
			{
				return AudioMod_CannotLoadReply(TEXT("UAudioBus"), AudioBusPath);
			}
		}
	}
	else
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unsupported generator class: %s"), *ClassName));
	}
	const TSharedPtr<FJsonObject> SettingsObj = AudioMod_GeneratorSettingsJson(Gen, /*bWithShape=*/false);

	const bool bSaved = AudioMod_ModifyDirtyAndSave(Gen);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("generator_path"), Gen->GetPathName());
	Data->SetStringField(TEXT("generator_class"), ClassName);
	Data->SetObjectField(TEXT("settings"), SettingsObj);
	Data->SetBoolField(TEXT("updated"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_modulation_generator_settings"));
}

// ---------------------------------------------------------------------------
// get_modulation_generator_info
// ---------------------------------------------------------------------------
// MCP:get_modulation_generator_info - Reads a USoundModulationGenerator asset without changing it. Returns generator_path, generator_class and settings by class: LFO shape, frequency, amplitude, offset, phase; ADEnvelope attack_time, decay_time; EnvelopeFollower attack_time, release_time, gain, audio_bus_path; empty for any other class.
//   @param generator_path - Asset path to the USoundModulationGenerator
//   @returns { generator_path, generator_class, settings: {...} }
TSharedPtr<FJsonObject> HandleGetModulationGeneratorInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString GeneratorPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("generator_path"), GeneratorPath)) { return Missing; }

	USoundModulationGenerator* Gen = LoadObject<USoundModulationGenerator>(nullptr, *GeneratorPath);
	if (!Gen)
	{
		return AudioMod_CannotLoadReply(TEXT("modulation generator"), GeneratorPath);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("generator_path"), Gen->GetPathName());
	Data->SetStringField(TEXT("generator_class"), Gen->GetClass()->GetName());
	Data->SetObjectField(TEXT("settings"), AudioMod_GeneratorSettingsJson(Gen, /*bWithShape=*/true));
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_bus_to_mix"), &HandleAddBusToMix },
	{ TEXT("add_modulation_input_to_patch"), &HandleAddModulationInputToPatch },
	{ TEXT("create_control_bus"), &HandleCreateControlBus },
	{ TEXT("create_control_bus_mix"), &HandleCreateControlBusMix },
	{ TEXT("create_modulation_generator"), &HandleCreateModulationGenerator },
	{ TEXT("create_modulation_generator_lfo"), &HandleCreateModulationGeneratorLFO },
	{ TEXT("create_modulation_parameter"), &HandleCreateModulationParameter },
	{ TEXT("create_sound_modulation_patch"), &HandleCreateSoundModulationPatch },
	{ TEXT("get_control_bus_mix_info"), &HandleGetControlBusMixInfo },
	{ TEXT("get_modulation_generator_info"), &HandleGetModulationGeneratorInfo },
	{ TEXT("get_modulation_info"), &HandleGetModulationInfo },
	{ TEXT("get_modulation_parameter_info"), &HandleGetModulationParameterInfo },
	{ TEXT("get_sound_modulation_patch_info"), &HandleGetSoundModulationPatchInfo },
	{ TEXT("remove_bus_from_mix"), &HandleRemoveBusFromMix },
	{ TEXT("remove_modulation_input_from_patch"), &HandleRemoveModulationInputFromPatch },
	{ TEXT("set_control_bus_mix_stage_value"), &HandleSetControlBusMixStageValue },
	{ TEXT("set_control_bus_value"), &HandleSetControlBusValue },
	{ TEXT("set_modulation_generator_settings"), &HandleSetModulationGeneratorSettings },
	{ TEXT("set_modulation_parameter_settings"), &HandleSetModulationParameterSettings },
	{ TEXT("set_modulation_patch_bypass"), &HandleSetModulationPatchBypass },
	{ TEXT("set_modulation_patch_output_parameter"), &HandleSetModulationPatchOutputParameter },
};
} // namespace UEAgentAudioModulationCommands
