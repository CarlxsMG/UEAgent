#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Engine sound routing types (all Runtime/Engine, Build.cs already linked).
#include "Sound/AudioBus.h"              // UAudioBus + EAudioBusChannels
#include "Sound/SoundSourceBus.h"        // USoundSourceBus + ESourceBusChannels
#include "Sound/SoundSourceBusSend.h"    // FSoundSourceBusSendInfo + ESourceBusSendLevelControlMethod
#include "Sound/SoundBase.h"             // TArray<FSoundSourceBusSendInfo> BusSends / PreEffectBusSends / PostAttenuationBusSends
#include "Curves/CurveFloat.h"           // FRuntimeFloatCurve

#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "JsonObjectConverter.h"
#include "Misc/PackageName.h"

/**
 * Audio Routing commands: author UAudioBus and USoundSourceBus assets, the UAudioBus a source bus is wired
 * to, and the FSoundSourceBusSendInfo entries it declares (BusSends / PreEffectBusSends /
 * PostAttenuationBusSends, inherited from USoundBase).
 *
 * All commands are headless-safe: asset create/load + property edit + SavePackage. No PIE, no audio
 * device required.
 */
namespace UEAgentAudioRoutingCommands
{

// MCP-DOMAIN: Audio

// ============================================================================
//
// Audio Routing commands.
//
// API verification (UE 5.8 headers, in `Engine/Source`):
//   * UAudioBus                      : AudioBus.h       (ENGINE_API, MinimalAPI)
//   * EAudioBusChannels              : AudioBus.h       (Mono, Stereo, Quad,
//                                                             FivePointOne, SevenPointOne)
//   * USoundSourceBus                : SoundSourceBus.h (ENGINE_API on ctor/dtor
//                                                             + PostLoad + PostEditChangeProperty)
//   * ESourceBusChannels             : SoundSourceBus.h (Mono, Stereo)
//   * FSoundSourceBusSendInfo        : SoundSourceBusSend.h (USTRUCT, ENGINE_API on
//                                                             PostSerialize)
//   * USoundBase::BusSends           : SoundBase.h     (TArray<FSoundSourceBusSendInfo>)
//   * USoundBase::PreEffectBusSends  : SoundBase.h
//   * USoundBase::PostAttenuationBusSends : SoundBase.h
//
// NOTE: FSoundSourceBusSendInfo does NOT live on USoundSourceBus directly: the
// struct lives on USoundBase (the parent of USoundWave/USoundSourceBus) as three
// TArrays, which USoundSourceBus inherits unchanged. The SendInfo authoring
// commands therefore target SSB->BusSends (and friends) via the USoundBase
// reflection path.
//
// ============================================================================

namespace
{
	// Common create-or-load helper, scoped to this translation unit. Duplicates the
	// helper in UEAgentAudioAssetCommands.cpp (which is in an anonymous
	// namespace there and therefore not accessible from another .cpp). Refactor
	// is the integrator's call; this duplication is the documented trade-off.
	UObject* CreateOrLoadRoutingAudioAsset(
		const TSharedPtr<FJsonObject>& Params,
		UClass* AssetClass,
		FString& OutAssetName,
		FString& OutPackagePath,
		FString& OutFullPath,
		bool& bOutAlreadyExists,
		FString& OutError)
	{
		bOutAlreadyExists = false;

		if (!Params->TryGetStringField(TEXT("name"), OutAssetName) || OutAssetName.IsEmpty())
		{
			OutError = TEXT("Missing required parameter: name");
			return nullptr;
		}
		if (!Params->TryGetStringField(TEXT("package_path"), OutPackagePath) || OutPackagePath.IsEmpty())
		{
			OutPackagePath = TEXT("/Game/Audio");
		}

		OutFullPath = FString::Printf(TEXT("%s/%s"), *OutPackagePath, *OutAssetName);
		OutError = mcp_common::InvalidPackageName(OutFullPath);
		if (!OutError.IsEmpty())
		{
			return nullptr;
		}

		if (UEditorAssetLibrary::DoesAssetExist(OutFullPath))
		{
			UObject* Existing = StaticLoadObject(AssetClass, nullptr, *OutFullPath);
			if (Existing)
			{
				bOutAlreadyExists = true;
				return Existing;
			}
			OutError = FString::Printf(
				TEXT("Asset already exists at %s but could not be loaded as %s"),
				*OutFullPath, *AssetClass->GetName());
			return nullptr;
		}

		UPackage* Package = CreatePackage(*OutFullPath);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("Failed to create package: %s"), *OutFullPath);
			return nullptr;
		}
		Package->FullyLoad();

		UObject* NewAsset = NewObject<UObject>(
			Package, AssetClass, *OutAssetName,
			RF_Public | RF_Standalone | RF_Transactional);
		if (!NewAsset)
		{
			OutError = FString::Printf(TEXT("Failed to create asset of class %s"), *AssetClass->GetName());
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(NewAsset);
		return NewAsset;
	}

	// String -> EAudioBusChannels (case-insensitive enum parse; supports both
	// "Stereo" / "Quad" / "FiveDotOne" and the engine's canonical "FivePointOne").
	bool ParseAudioBusChannels(const FString& Token, EAudioBusChannels& Out)
	{
		const FString Norm = Token.ToLower();
		if (Norm == TEXT("mono"))           { Out = EAudioBusChannels::Mono;           return true; }
		if (Norm == TEXT("stereo"))         { Out = EAudioBusChannels::Stereo;         return true; }
		if (Norm == TEXT("quad"))           { Out = EAudioBusChannels::Quad;           return true; }
		if (Norm == TEXT("fivedotone") ||
		    Norm == TEXT("fivepointone") ||
		    Norm == TEXT("5.1") ||
		    Norm == TEXT("5dot1"))          { Out = EAudioBusChannels::FivePointOne;   return true; }
		if (Norm == TEXT("sevendotone") ||
		    Norm == TEXT("sevenpointone") ||
		    Norm == TEXT("7.1") ||
		    Norm == TEXT("7dot1"))          { Out = EAudioBusChannels::SevenPointOne;  return true; }
		return false;
	}

	const TCHAR* AudioBusChannelsToString(EAudioBusChannels Ch)
	{
		switch (Ch)
		{
		case EAudioBusChannels::Mono:          return TEXT("Mono");
		case EAudioBusChannels::Stereo:        return TEXT("Stereo");
		case EAudioBusChannels::Quad:          return TEXT("Quad");
		case EAudioBusChannels::FivePointOne:  return TEXT("FivePointOne");
		case EAudioBusChannels::SevenPointOne: return TEXT("SevenPointOne");
		}
		return TEXT("Unknown");
	}

	// String -> ESourceBusChannels (USoundSourceBus inherits only Mono/Stereo).
	bool ParseSourceBusChannels(const FString& Token, ESourceBusChannels& Out)
	{
		const FString Norm = Token.ToLower();
		if (Norm == TEXT("mono"))   { Out = ESourceBusChannels::Mono;   return true; }
		if (Norm == TEXT("stereo")) { Out = ESourceBusChannels::Stereo; return true; }
		return false;
	}

	const TCHAR* SourceBusChannelsToString(ESourceBusChannels Ch)
	{
		switch (Ch)
		{
		case ESourceBusChannels::Mono:   return TEXT("Mono");
		case ESourceBusChannels::Stereo: return TEXT("Stereo");
		}
		return TEXT("Unknown");
	}

	// Loads the TAsset at Path into OutAsset; when that fails, the reply "Could not load <TypeLabel> at <Path>".
	template <typename TAsset>
	TSharedPtr<FJsonObject> AudioRouting_LoadAsset(const FString& Path, const TCHAR* TypeLabel, TAsset*& OutAsset)
	{
		OutAsset = Cast<TAsset>(StaticLoadObject(TAsset::StaticClass(), nullptr, *Path));
		if (!OutAsset)
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load %s at %s"), TypeLabel, *Path));
		}
		return nullptr;
	}

	// asset_class, channels (name and value) and num_channels of a UAudioBus.
	void AudioRouting_WriteBusLayout(const TSharedPtr<FJsonObject>& Out, const UAudioBus* Bus)
	{
		Out->SetStringField(TEXT("asset_class"), Bus->GetClass()->GetName());
		Out->SetStringField(TEXT("channels"), AudioBusChannelsToString(Bus->AudioBusChannels));
		Out->SetNumberField(TEXT("channels_int"), static_cast<int32>(Bus->AudioBusChannels));
		Out->SetNumberField(TEXT("num_channels"), Bus->GetNumChannels());
	}

	// audio_bus_path and audio_bus_class of the bus a USoundSourceBus returns to; both "" without one.
	void AudioRouting_WriteReturnBus(const TSharedPtr<FJsonObject>& Out, const UAudioBus* Bus)
	{
		if (Bus)
		{
			Out->SetStringField(TEXT("audio_bus_path"), Bus->GetPathName());
			Out->SetStringField(TEXT("audio_bus_class"), Bus->GetClass()->GetName());
		}
		else
		{
			Out->SetStringField(TEXT("audio_bus_path"), TEXT(""));
			Out->SetStringField(TEXT("audio_bus_class"), TEXT(""));
		}
	}

	// asset_class, source_bus_channels, duration and the return bus of a USoundSourceBus.
	void AudioRouting_WriteSourceBusRouting(const TSharedPtr<FJsonObject>& Out, const USoundSourceBus* SSB)
	{
		Out->SetStringField(TEXT("asset_class"), SSB->GetClass()->GetName());
		Out->SetStringField(TEXT("source_bus_channels"), SourceBusChannelsToString(SSB->SourceBusChannels));
		Out->SetNumberField(TEXT("duration"), SSB->SourceBusDuration);
		AudioRouting_WriteReturnBus(Out, SSB->AudioBus.Get());
	}
}

// ---------------------------------------------------------------------------- //
// create_audio_bus
// ---------------------------------------------------------------------------- //
// MCP:create_audio_bus - Creates a UAudioBus asset at package_path/name and saves it, or loads the one already there (already_exists true) and saves it again; another asset class at that path is an error. channels, when given, sets the channel layout, overwriting an existing bus's layout; when omitted a new bus gets Stereo and an existing one keeps its own. An invalid channels value is refused before any asset is created or loaded. Change the layout later with set_audio_bus_channels and read it with get_audio_bus; create_sound_source_bus makes a source bus that plays this bus's audio.
//   @param name - Name of the new Audio Bus asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param channels - EAudioBusChannels: 'Mono' | 'Stereo' (default) | 'Quad' | 'FiveDotOne' | 'SevenDotOne'. The default applies to a new bus only.
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, channels: string, channels_int: int, num_channels: int, applied_channels: bool (this call wrote the layout: channels given, or the Stereo default on a new bus), created: bool, already_exists: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCreateAudioBus(const TSharedPtr<FJsonObject>& Params)
{
	// Parse optional channels before anything is created, so a bad value leaves no asset behind.
	EAudioBusChannels Ch = EAudioBusChannels::Stereo;
	FString ChannelsTok;
	const bool bChannelsGiven = Params->TryGetStringField(TEXT("channels"), ChannelsTok) && !ChannelsTok.IsEmpty();
	if (bChannelsGiven && !ParseAudioBusChannels(ChannelsTok, Ch))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(
				TEXT("Invalid channels '%s' (expected Mono|Stereo|Quad|FiveDotOne|SevenDotOne)"),
				*ChannelsTok));
	}

	FString AssetName, PackagePath, FullPath, Error;
	bool bAlreadyExists = false;

	UObject* Obj = CreateOrLoadRoutingAudioAsset(
		Params, UAudioBus::StaticClass(),
		AssetName, PackagePath, FullPath, bAlreadyExists, Error);
	if (!Obj)
	{
		return mcp_common::MakeErrorReply(Error);
	}

	UAudioBus* Bus = Cast<UAudioBus>(Obj);
	if (!Bus)
	{
		return mcp_common::MakeErrorReply(TEXT("Created object is not a UAudioBus"));
	}

	// A new bus takes the Stereo default (the engine default is Mono); an existing bus keeps its layout
	// unless channels was given.
	const bool bAppliedChannels = bChannelsGiven || !bAlreadyExists;
	if (bAppliedChannels)
	{
		Bus->Modify();
		Bus->AudioBusChannels = Ch;
		Bus->PostEditChange();
	}

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(Bus->GetOutermost(), Bus, SaveError);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("name"), AssetName);
	ResultObj->SetStringField(TEXT("package_path"), PackagePath);
	ResultObj->SetStringField(TEXT("asset_path"), Bus->GetPathName());
	AudioRouting_WriteBusLayout(ResultObj, Bus);
	ResultObj->SetBoolField(TEXT("applied_channels"), bAppliedChannels);
	ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
	ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

	UE_LOG(LogTemp, Verbose,
		TEXT("create_audio_bus: %s channels=%s saved=%d"),
		*Bus->GetPathName(), AudioBusChannelsToString(Bus->AudioBusChannels), bSaved ? 1 : 0);

	return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_audio_bus"), SaveError);
}

// ---------------------------------------------------------------------------- //
// get_audio_bus
// ---------------------------------------------------------------------------- //
// MCP:get_audio_bus - Read the channel layout of a UAudioBus asset: channels (Mono, Stereo, Quad, FivePointOne or SevenPointOne), channels_int and num_channels. Read-only. is_active is always false because no audio device is queried. Change the layout with set_audio_bus_channels.
//   @param bus_path - Asset path of the UAudioBus, e.g. "/Game/Audio/AB_Music".
//   @returns { bus_path: string, asset_class: string, channels: string, channels_int: int, num_channels: int, is_active: bool (always false: headless — no audio device) }
TSharedPtr<FJsonObject> HandleGetAudioBus(const TSharedPtr<FJsonObject>& Params)
{
	FString BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }

	UAudioBus* Bus = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(BusPath, TEXT("UAudioBus"), Bus))
	{
		return LoadError;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	AudioRouting_WriteBusLayout(ResultObj, Bus);
	// Runtime activity check requires an active FAudioDevice; in headless we report
	// "not active" honestly. A PIE/editor-restart session can re-check at runtime.
	ResultObj->SetBoolField(TEXT("is_active"), false);

	return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------- //
// set_audio_bus_channels
// ---------------------------------------------------------------------------- //
// MCP:set_audio_bus_channels - Change the channel layout (AudioBusChannels) of an existing UAudioBus asset and save it; success requires the save. Returns channels, channels_int and num_channels read back from the asset. To make a new bus use create_audio_bus.
//   @param bus_path - Asset path of the UAudioBus, e.g. "/Game/Audio/AB_Music".
//   @param channels - EAudioBusChannels: 'Mono' | 'Stereo' | 'Quad' | 'FiveDotOne' | 'SevenDotOne', case-insensitive; FivePointOne, 5.1, SevenPointOne and 7.1 are accepted too.
//   @returns { bus_path: string, asset_class: string, channels: string, channels_int: int, num_channels: int, saved: bool }
TSharedPtr<FJsonObject> HandleSetAudioBusChannels(const TSharedPtr<FJsonObject>& Params)
{
	FString BusPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("bus_path"), BusPath)) { return Missing; }

	FString ChannelsTok;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("channels"), ChannelsTok)) { return Missing; }

	EAudioBusChannels Ch;
	if (!ParseAudioBusChannels(ChannelsTok, Ch))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(
				TEXT("Invalid channels '%s' (expected Mono|Stereo|Quad|FiveDotOne|SevenDotOne)"),
				*ChannelsTok));
	}

	UAudioBus* Bus = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(BusPath, TEXT("UAudioBus"), Bus))
	{
		return LoadError;
	}

	Bus->Modify();
	Bus->AudioBusChannels = Ch;
	Bus->PostEditChange();

	const bool bSaved = mcp_common::SaveAssetPackage(Bus->GetOutermost(), Bus);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("bus_path"), Bus->GetPathName());
	AudioRouting_WriteBusLayout(ResultObj, Bus);

	UE_LOG(LogTemp, Verbose,
		TEXT("set_audio_bus_channels: %s -> %s saved=%d"),
		*Bus->GetPathName(), AudioBusChannelsToString(Bus->AudioBusChannels), bSaved ? 1 : 0);

	return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_audio_bus_channels"));
}

// ---------------------------------------------------------------------------- //
// create_sound_source_bus
// ---------------------------------------------------------------------------- //
// MCP:create_sound_source_bus - Creates a USoundSourceBus asset at package_path/name, or loads the one already there, writes its routing fields and saves it; another asset class at the path is an error. source_bus_channels and duration, when omitted, take the defaults (Stereo, 1.0 s) on a new asset and keep their value on an existing one, whose fields the given values overwrite. Every value is validated before any asset is created or loaded, so an invalid one leaves nothing behind. audio_bus_path, when given, must load as a UAudioBus and becomes the bus whose audio this source bus plays; when omitted that field is left as is. Edit later with set_sound_source_bus_audio_bus or set_sound_source_bus_send_info; read with get_sound_source_bus.
//   @param name - Name of the new Sound Source Bus asset
//   @param package_path - Target content path, defaults to /Game/Audio
//   @param source_bus_channels - ESourceBusChannels: 'Mono' | 'Stereo' (default). USoundSourceBus inherits only Mono/Stereo. The default applies to a new asset only.
//   @param duration - Duration in seconds, at least 0 (0.0 means indefinite); a negative value is an error. Default 1.0 on a new asset only.
//   @param audio_bus_path - Optional full content path of a UAudioBus to assign as the AudioBus return.
//   @returns { name: string, package_path: string, asset_path: string, asset_class: string, source_bus_channels: string, duration: float, audio_bus_path: string, audio_bus_class: string, applied_channels: bool, applied_duration: bool (each true when this call wrote the field: given, or the default on a new asset), applied_audio_bus: bool, created: bool, already_exists: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCreateSoundSourceBus(const TSharedPtr<FJsonObject>& Params)
{
	// Every optional field is parsed and validated before anything is created, so a bad value leaves no
	// asset behind.

	// source_bus_channels (default for a new asset: Stereo)
	ESourceBusChannels Ch = ESourceBusChannels::Stereo;
	FString ChTok;
	const bool bChannelsGiven = Params->TryGetStringField(TEXT("source_bus_channels"), ChTok) && !ChTok.IsEmpty();
	if (bChannelsGiven && !ParseSourceBusChannels(ChTok, Ch))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(
				TEXT("Invalid source_bus_channels '%s' (USoundSourceBus inherits only Mono|Stereo)"),
				*ChTok));
	}

	// duration (default for a new asset: 1.0). The property is ClampMin="0.0" and 0 means "play indefinitely",
	// so a negative value is refused rather than clamped into that very different meaning.
	double DurationD = 1.0;
	const bool bDurationGiven = Params->TryGetNumberField(TEXT("duration"), DurationD);
	if (bDurationGiven && (!FMath::IsFinite(DurationD) || DurationD < 0.0))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Invalid duration %g: it must be a finite number of seconds >= 0 (0 plays indefinitely)"), DurationD));
	}

	// audio_bus_path (optional; load UAudioBus and assign SSB->AudioBus)
	FString AudioBusPath;
	UAudioBus* AssignedBus = nullptr;
	bool bAppliedAudioBus = false;
	if (Params->TryGetStringField(TEXT("audio_bus_path"), AudioBusPath) && !AudioBusPath.IsEmpty())
	{
		if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(AudioBusPath, TEXT("UAudioBus"), AssignedBus))
		{
			return LoadError;
		}
		bAppliedAudioBus = true;
	}

	FString AssetName, PackagePath, FullPath, Error;
	bool bAlreadyExists = false;

	UObject* Obj = CreateOrLoadRoutingAudioAsset(
		Params, USoundSourceBus::StaticClass(),
		AssetName, PackagePath, FullPath, bAlreadyExists, Error);
	if (!Obj)
	{
		return mcp_common::MakeErrorReply(Error);
	}

	USoundSourceBus* SSB = Cast<USoundSourceBus>(Obj);
	if (!SSB)
	{
		return mcp_common::MakeErrorReply(TEXT("Created object is not a USoundSourceBus"));
	}

	// A new asset takes the defaults (Stereo, 1.0 s) for what was omitted; an existing one keeps its own
	// value for every field the request leaves out.
	const bool bAppliedChannels = bChannelsGiven || !bAlreadyExists;
	const bool bAppliedDuration = bDurationGiven || !bAlreadyExists;

	SSB->Modify();
	if (bAppliedChannels)
	{
		SSB->SourceBusChannels = Ch;
	}
	if (bAppliedDuration)
	{
		SSB->SourceBusDuration = static_cast<float>(DurationD);
	}
	if (bAppliedAudioBus)
	{
		SSB->AudioBus = AssignedBus;
	}
	SSB->PostEditChange();

	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(SSB->GetOutermost(), SSB, SaveError);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("name"), AssetName);
	ResultObj->SetStringField(TEXT("package_path"), PackagePath);
	ResultObj->SetStringField(TEXT("asset_path"), SSB->GetPathName());
	AudioRouting_WriteSourceBusRouting(ResultObj, SSB);
	ResultObj->SetBoolField(TEXT("applied_channels"), bAppliedChannels);
	ResultObj->SetBoolField(TEXT("applied_duration"), bAppliedDuration);
	ResultObj->SetBoolField(TEXT("applied_audio_bus"), bAppliedAudioBus);
	ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
	ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

	UE_LOG(LogTemp, Verbose,
		TEXT("create_sound_source_bus: %s channels=%s duration=%.3f audio_bus=%s saved=%d"),
		*SSB->GetPathName(),
		SourceBusChannelsToString(SSB->SourceBusChannels),
		SSB->SourceBusDuration,
		bAppliedAudioBus ? *AudioBusPath : TEXT("(none)"),
		bSaved ? 1 : 0);

	return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_sound_source_bus"), SaveError);
}

// ---------------------------------------------------------------------------- //
// get_sound_source_bus
// ---------------------------------------------------------------------------- //
// MCP:get_sound_source_bus - Read the routing fields of a USoundSourceBus asset: source_bus_channels (Mono or Stereo), duration in seconds (0 plays indefinitely) and the UAudioBus it outputs to (audio_bus_path and audio_bus_class, empty when none). Read-only. Writer: set_sound_source_bus_audio_bus.
//   @param ssb_path - Asset path of the USoundSourceBus, e.g. "/Game/Audio/SSB_Radio".
//   @returns { ssb_path: string, asset_class: string, source_bus_channels: string, duration: float, audio_bus_path: string, audio_bus_class: string }
TSharedPtr<FJsonObject> HandleGetSoundSourceBus(const TSharedPtr<FJsonObject>& Params)
{
	FString SSBPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ssb_path"), SSBPath)) { return Missing; }

	USoundSourceBus* SSB = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(SSBPath, TEXT("USoundSourceBus"), SSB))
	{
		return LoadError;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("ssb_path"), SSB->GetPathName());
	AudioRouting_WriteSourceBusRouting(ResultObj, SSB);

	return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------- //
// set_sound_source_bus_audio_bus
// ---------------------------------------------------------------------------- //
// MCP:set_sound_source_bus_audio_bus - Point the AudioBus output of a USoundSourceBus asset at a UAudioBus, or clear it when audio_bus_path is empty or omitted, then save; success requires the save. Returns audio_bus_path and audio_bus_class read back, plus cleared. A bus path that does not load is an error, not a clear.
//   @param ssb_path - Asset path of the USoundSourceBus, e.g. "/Game/Audio/SSB_Radio".
//   @param audio_bus_path - Asset path of the UAudioBus to assign, e.g. "/Game/Audio/AB_Music"; "" or omitted clears the assignment.
//   @returns { ssb_path: string, audio_bus_path: string, audio_bus_class: string, cleared: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundSourceBusAudioBus(const TSharedPtr<FJsonObject>& Params)
{
	FString SSBPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ssb_path"), SSBPath)) { return Missing; }

	// audio_bus_path is OPTIONAL: empty string / absent -> clear (null assignment).
	FString AudioBusPath;
	const bool bHasAudioBusPath = Params->TryGetStringField(TEXT("audio_bus_path"), AudioBusPath);

	UAudioBus* NewBus = nullptr;
	if (bHasAudioBusPath && !AudioBusPath.IsEmpty())
	{
		if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(AudioBusPath, TEXT("UAudioBus"), NewBus))
		{
			return LoadError;
		}
	}

	USoundSourceBus* SSB = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(SSBPath, TEXT("USoundSourceBus"), SSB))
	{
		return LoadError;
	}

	SSB->Modify();
	SSB->AudioBus = NewBus;
	SSB->PostEditChange();

	const bool bSaved = mcp_common::SaveAssetPackage(SSB->GetOutermost(), SSB);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("ssb_path"), SSB->GetPathName());
	AudioRouting_WriteReturnBus(ResultObj, SSB->AudioBus.Get());
	ResultObj->SetBoolField(TEXT("cleared"), NewBus == nullptr);

	UE_LOG(LogTemp, Verbose,
		TEXT("set_sound_source_bus_audio_bus: %s -> %s cleared=%d saved=%d"),
		*SSB->GetPathName(),
		NewBus ? *NewBus->GetPathName() : TEXT("(null)"),
		NewBus == nullptr ? 1 : 0,
		bSaved ? 1 : 0);

	return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_sound_source_bus_audio_bus"));
}

// ---------------------------------------------------------------------------- //
// set_sound_source_bus_send_info
//
// VERIFIED against UE 5.8 source:
//   FSoundSourceBusSendInfo lives on USoundBase (parent of USoundWave/USoundSourceBus)
//   as three TArrays:
//     * USoundBase::BusSends                  (SoundBase.h)
//     * USoundBase::PreEffectBusSends         (SoundBase.h)
//     * USoundBase::PostAttenuationBusSends   (SoundBase.h)
//   USoundSourceBus inherits all three unchanged.
//
// API: Replace SSB->BusSends[send_index] with a JSON-merged FSoundSourceBusSendInfo.
// ---------------------------------------------------------------------------- //
// MCP:set_sound_source_bus_send_info - Author a single FSoundSourceBusSendInfo entry on USoundBase::BusSends via reflection (partial merge + clamp).
//   @param ssb_path - Full content path of the target USoundSourceBus (USoundBase.BusSends is the target array; USoundSourceBus inherits it unchanged).
//   @param send_index - 0-based index into BusSends; the entry is REPLACED in place. If out of range, the entry is appended (resize+append).
//   @param send_info - Object of FSoundSourceBusSendInfo fields to merge (sourceBusSendLevelControlMethod(Linear|CustomCurve|Manual), soundSourceBus(path), audioBus(path), sendLevel(Manual), minSendLevel/maxSendLevel(Linear), minSendDistance/maxSendDistance(Linear), customSendLevelCurve(object), bEnableLPFCutoff(bool), lPFCutoff(20..20000Hz), bEnableHPFCutoff(bool), hPFCutoff(20..20000Hz)).
//   @returns { ssb_path: string, send_index: int, bus_sends_count: int, send_info: object, applied_keys: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundSourceBusSendInfo(const TSharedPtr<FJsonObject>& Params)
{
	FString SSBPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ssb_path"), SSBPath)) { return Missing; }

	int32 SendIndex = 0;
	if (!Params->TryGetNumberField(TEXT("send_index"), SendIndex))
	{
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: send_index (int)"));
	}

	const TSharedPtr<FJsonObject>* SendInfoJsonPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("send_info"), SendInfoJsonPtr) || !SendInfoJsonPtr || !SendInfoJsonPtr->IsValid())
	{
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: send_info (object)"));
	}
	const TSharedPtr<FJsonObject>& SendInfoJson = *SendInfoJsonPtr;

	USoundSourceBus* SSB = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(SSBPath, TEXT("USoundSourceBus"), SSB))
	{
		return LoadError;
	}

	// Resolve SoundSourceBus / AudioBus path fields into real references before the
	// JSON merge (JsonObjectToUStruct does not resolve /Game/... asset paths into
	// TObjectPtr<>; we'd get a null reference instead of the intended target).
	// We do this by *replacing* the JSON path strings with reflection-compatible
	// synthetic fields below. If those keys are absent, no replacement happens.
	//
	// The merge target is FSoundSourceBusSendInfo (Engine header). For the
	// TObjectPtr<USoundSourceBus> / TObjectPtr<UAudioBus> fields, the canonical
	// way to assign them in a real UStruct merge is to set the property directly
	// after the merge. We do both: partial-merge the JSON, then patch the
	// TObjectPtr fields explicitly.

	SSB->Modify();

	// Resize / default-construct the target entry so the merge has somewhere to land.
	if (SendIndex < 0)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("send_index must be >= 0, got %d"), SendIndex));
	}
	if (SendIndex >= SSB->BusSends.Num())
	{
		SSB->BusSends.SetNum(SendIndex + 1);
	}

	// Partial merge into the in-place entry.
	FString JsonErr;
	if (!mcp_common::MergeJsonWithClamp(
			SendInfoJson,
			FSoundSourceBusSendInfo::StaticStruct(),
			&SSB->BusSends[SendIndex], JsonErr, 0, 0))
	{
		return mcp_common::MakeErrorReply(JsonErr);
	}

	// Patch the TObjectPtr<> fields explicitly (JSON doesn't carry path resolution).
	FString SoundSourceBusPath;
	if (SendInfoJson->TryGetStringField(TEXT("soundSourceBus"), SoundSourceBusPath) && !SoundSourceBusPath.IsEmpty())
	{
		USoundSourceBus* Ref = Cast<USoundSourceBus>(
			StaticLoadObject(USoundSourceBus::StaticClass(), nullptr, *SoundSourceBusPath));
		SSB->BusSends[SendIndex].SoundSourceBus = Ref;
	}
	FString AudioBusPath;
	if (SendInfoJson->TryGetStringField(TEXT("audioBus"), AudioBusPath) && !AudioBusPath.IsEmpty())
	{
		UAudioBus* Ref = Cast<UAudioBus>(
			StaticLoadObject(UAudioBus::StaticClass(), nullptr, *AudioBusPath));
		SSB->BusSends[SendIndex].AudioBus = Ref;
	}

	SSB->PostEditChange();

	const bool bSaved = mcp_common::SaveAssetPackage(SSB->GetOutermost(), SSB);

	TSharedPtr<FJsonObject> SendInfoOut = mcp_common::StructToJson(
		FSoundSourceBusSendInfo::StaticStruct(),
		&SSB->BusSends[SendIndex], 0, 0);

	TArray<TSharedPtr<FJsonValue>> AppliedKeys;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : SendInfoJson->Values)
	{
		AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("ssb_path"), SSB->GetPathName());
	ResultObj->SetNumberField(TEXT("send_index"), SendIndex);
	ResultObj->SetNumberField(TEXT("bus_sends_count"), SSB->BusSends.Num());
	ResultObj->SetObjectField(TEXT("send_info"), SendInfoOut);
	ResultObj->SetArrayField(TEXT("applied_keys"), AppliedKeys);

	UE_LOG(LogTemp, Verbose,
		TEXT("set_sound_source_bus_send_info: %s index=%d count=%d saved=%d"),
		*SSB->GetPathName(), SendIndex, SSB->BusSends.Num(), bSaved ? 1 : 0);

	return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_sound_source_bus_send_info"));
}

// ---------------------------------------------------------------------------- //
// set_sound_source_bus_send_level_curve
//
// Custom FRuntimeFloatCurve authoring on a BusSends entry's CustomSendLevelCurve
// field. Same USoundBase::BusSends array target as send_info above.
// ---------------------------------------------------------------------------- //
// MCP:set_sound_source_bus_send_level_curve - Merge a FRuntimeFloatCurve JSON into BusSends[send_index].CustomSendLevelCurve of a USoundSourceBus asset (BusSends grows with default entries past its end) and save it; success requires the save. Returns the curve read back. The curve only takes effect when that send uses the CustomCurve control method: see set_sound_source_bus_send_info.
//   @param ssb_path - Full content path of the target USoundSourceBus (USoundBase.BusSends is the target array).
//   @param send_index - 0-based index into BusSends; the entry is mutated in place. Resized/extended with default-constructed entries if out of range.
//   @param distance_curve - FRuntimeFloatCurve fields by their UPROPERTY names, e.g. {"EditorCurveData": {"Keys": [{"Time": 0.0, "Value": 1.0}, {"Time": 1.0, "Value": 0.0}]}}; Time 0-1 maps to MinSendDistance-MaxSendDistance and Keys replaces the key list. Fields left out keep their current values; unknown keys are ignored without error.
//   @returns { ssb_path: string, send_index: int, bus_sends_count: int, distance_curve: object, applied_keys: [string], saved: bool }
TSharedPtr<FJsonObject> HandleSetSoundSourceBusSendLevelCurve(const TSharedPtr<FJsonObject>& Params)
{
	FString SSBPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("ssb_path"), SSBPath)) { return Missing; }

	int32 SendIndex = 0;
	if (!Params->TryGetNumberField(TEXT("send_index"), SendIndex))
	{
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: send_index (int)"));
	}

	const TSharedPtr<FJsonObject>* DistanceCurveJsonPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("distance_curve"), DistanceCurveJsonPtr) || !DistanceCurveJsonPtr || !DistanceCurveJsonPtr->IsValid())
	{
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: distance_curve (object)"));
	}
	const TSharedPtr<FJsonObject>& DistanceCurveJson = *DistanceCurveJsonPtr;

	USoundSourceBus* SSB = nullptr;
	if (TSharedPtr<FJsonObject> LoadError = AudioRouting_LoadAsset(SSBPath, TEXT("USoundSourceBus"), SSB))
	{
		return LoadError;
	}

	if (SendIndex < 0)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("send_index must be >= 0, got %d"), SendIndex));
	}
	if (SendIndex >= SSB->BusSends.Num())
	{
		SSB->BusSends.SetNum(SendIndex + 1);
	}

	SSB->Modify();

	FString JsonErr;
	if (!mcp_common::MergeJsonWithClamp(
			DistanceCurveJson,
			FRuntimeFloatCurve::StaticStruct(),
			&SSB->BusSends[SendIndex].CustomSendLevelCurve, JsonErr, 0, 0))
	{
		return mcp_common::MakeErrorReply(JsonErr);
	}

	SSB->PostEditChange();

	const bool bSaved = mcp_common::SaveAssetPackage(SSB->GetOutermost(), SSB);

	TSharedPtr<FJsonObject> CurveOut = mcp_common::StructToJson(
		FRuntimeFloatCurve::StaticStruct(),
		&SSB->BusSends[SendIndex].CustomSendLevelCurve, 0, 0);

	TArray<TSharedPtr<FJsonValue>> AppliedKeys;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : DistanceCurveJson->Values)
	{
		AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("ssb_path"), SSB->GetPathName());
	ResultObj->SetNumberField(TEXT("send_index"), SendIndex);
	ResultObj->SetNumberField(TEXT("bus_sends_count"), SSB->BusSends.Num());
	ResultObj->SetObjectField(TEXT("distance_curve"), CurveOut);
	ResultObj->SetArrayField(TEXT("applied_keys"), AppliedKeys);

	UE_LOG(LogTemp, Verbose,
		TEXT("set_sound_source_bus_send_level_curve: %s index=%d count=%d saved=%d"),
		*SSB->GetPathName(), SendIndex, SSB->BusSends.Num(), bSaved ? 1 : 0);

	return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_sound_source_bus_send_level_curve"));
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("create_audio_bus"), &HandleCreateAudioBus },
	{ TEXT("create_sound_source_bus"), &HandleCreateSoundSourceBus },
	{ TEXT("get_audio_bus"), &HandleGetAudioBus },
	{ TEXT("get_sound_source_bus"), &HandleGetSoundSourceBus },
	{ TEXT("set_audio_bus_channels"), &HandleSetAudioBusChannels },
	{ TEXT("set_sound_source_bus_audio_bus"), &HandleSetSoundSourceBusAudioBus },
	{ TEXT("set_sound_source_bus_send_info"), &HandleSetSoundSourceBusSendInfo },
	{ TEXT("set_sound_source_bus_send_level_curve"), &HandleSetSoundSourceBusSendLevelCurve },
};
} // namespace UEAgentAudioRoutingCommands
