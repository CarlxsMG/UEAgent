// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h" // UE_VERSION_OLDER_THAN: the UE 5.7 paths

#include "Engine/Engine.h"
#include "EngineSettings.h"
#include "GameMapsSettings.h"
#include "GeneralProjectSettings.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyAccessUtil.h"
#include "UObject/PropertyPortFlags.h"   // PPF_SerializedAsImportText (the port flags SaveConfig itself uses)
#include "UObject/UObjectGlobals.h"      // GetConfigFilename(UObject*) -- SaveConfig's real destination file
#include "Misc/ConfigCacheIni.h"         // FConfigFile::Read / FConfigSection -- DISK reads, never GConfig
#include "Misc/Paths.h"                  // FPaths::FileExists / SourceConfigDir / ConvertRelativePathToFull
// Project-level collision channels/profiles.
#include "Engine/CollisionProfile.h"

/**
 * Project / Developer Settings commands.
 *
 * Wraps UDeveloperSettings-derived classes so an LLM can read and write the engine's project-wide settings
 * at runtime via GetDefault<T>() + ImportText / ExportText. Covers the generic get/set path (any
 * UDeveloperSettings) plus convenience wrappers for the most commonly touched settings: physics gravity,
 * default game map, and project metadata. It also manages the project's collision channels and profiles.
 *
 * PERSISTENCE. For a UCLASS(config=Engine, defaultconfig) class, SaveConfig() writes
 * Saved/Config/<Platform>/Engine.ini — a gitignored file — and Config/DefaultEngine.ini never changes.
 * So the settings setters persist through UObject::TryUpdateDefaultConfigFile() when the class is
 * CLASS_DefaultConfig (the same routine the editor's Project Settings panel uses), falling back to
 * SaveConfig() only for classes that genuinely live in a per-user/Saved ini — and they say which one
 * they used.
 *
 * VERIFICATION. Each settings setter then RE-READS the target .ini from disk with FConfigFile::Read and
 * reports disk_value / disk_matches_memory plus a verification block. GConfig is deliberately never used
 * for that read: it is the in-memory cache and would echo the write back. get_default_game_map reports
 * the disk value too, for the same reason.
 *
 * All commands are headless-safe — they touch CDO config objects directly and write the project's INI
 * files. No PIE, no render, no modal dialog.
 */
namespace UEAgentProjectSettingsCommands
{

// MCP-DOMAIN: Config

namespace
{
	/**
	 * Translate an asset-style path ("/Script/Engine.PhysicsSettings") into a
	 * UClass*. Falls back to the short class name against the engine's
	 * native loaded classes if no /Script/Module prefix is supplied.
	 */
	UClass* ResolveSettingsClass(const FString& ClassPath)
	{
		if (ClassPath.IsEmpty()) { return nullptr; }
		// Long path form: LoadClass<UObject>(nullptr, *ClassPath)
		if (ClassPath.StartsWith(TEXT("/")))
		{
			return LoadObject<UClass>(nullptr, *ClassPath);
		}
		// Short form: search native + blueprint classes (only native here).
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Candidate = *It;
			if (!Candidate || !Candidate->IsChildOf(UDeveloperSettings::StaticClass())) { continue; }
			if (Candidate->GetName() == ClassPath) { return Candidate; }
		}
		return nullptr;
	}

	/**
	 * Import a text value onto the CDO's property.
	 *
	 * Through the strict importer, so a value the property cannot read whole ("abc" for a number, which
	 * ImportText_Direct alone takes as 0) is reported as a failure and leaves the property as it was.
	 */
	bool WritePropertyByImport(UObject* CDO, FProperty* Prop, const FString& Value)
	{
		if (!CDO || !Prop) { return false; }
		FString Unused;
		return mcp_common::ImportPropertyTextStrict(Prop, Value, Prop->ContainerPtrToValuePtr<void>(CDO), CDO, Unused);
	}

	bool ReadPropertyAsString(UObject* CDO, FProperty* Prop, FString& Out)
	{
		if (!CDO || !Prop) { return false; }
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
		Prop->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
		return true;
	}

	// -----------------------------------------------------------------------
	// VERSIONED-INI PERSISTENCE
	//
	// For classes declared UCLASS(config=Engine, defaultconfig), CDO->SaveConfig() writes the
	// SAVED ini (Saved/Config/<Platform>/Engine.ini -- gitignored), while the file the project
	// actually ships is Config/DefaultEngine.ini: a tool would report success while the versioned
	// ini never changed, and re-reading the in-memory CDO would "confirm" it.
	//
	// UObject::TryUpdateDefaultConfigFile() is the routine the editor's own Project Settings
	// panel uses (as do the UCollisionProfile writers further down this file). Everything below writes through it when the class is
	// CLASS_DefaultConfig, and then RE-READS THE FILE FROM DISK with FConfigFile::Read --
	// deliberately NOT GConfig, which is the in-memory cache and would merely echo the write back.
	// -----------------------------------------------------------------------

	/** Where a settings CDO's config section was written, and whether the write was accepted. */
	struct FPSCfgWriteTarget
	{
		bool    bDefaultConfigClass = false; // CLASS_DefaultConfig -> versioned Config/Default*.ini
		bool    bWriteReported      = false; // TryUpdateDefaultConfigFile()'s own return value
		bool    bVersionedIni       = false; // does IniPath live under <Project>/Config ?
		FString IniPath;                     // absolute path of the file we targeted
		FString FailReason;
	};

	/** Raw result of looking one key up in one ini FILE (not GConfig). */
	struct FPSCfgIniProbe
	{
		bool bFileExists   = false;
		bool bSectionFound = false;
		bool bKeyFound     = false;
		TArray<FString> Values; // >1 for array/set/map properties (the +Key= lines)
	};

	/** The ini section SaveConfig would use for this property (mirrors UObject::SaveConfig). */
	FString PSCfgSectionForProperty(const UObject* CDO, const FProperty* Prop)
	{
		if (!CDO) { return FString(); }
		const UClass* BaseClass = CDO->GetClass();
		// SaveConfig: BaseClass = GetClass(), except globalconfig props which use the owner class.
		if (Prop && Prop->HasAnyPropertyFlags(CPF_GlobalConfig) && Prop->GetOwnerClass())
		{
			BaseClass = Prop->GetOwnerClass();
		}
		return BaseClass ? BaseClass->GetPathName() : FString();
	}

	/** The ini key SaveConfig would use (a ConsoleVariable meta replaces the property name). */
	FString PSCfgKeyForProperty(const FProperty* Prop)
	{
		if (!Prop) { return FString(); }
		const FString& CVarName = Prop->GetMetaData(TEXT("ConsoleVariable"));
		if (!CVarName.IsEmpty()) { return CVarName; }
		return Prop->GetName();
	}

	/**
	 * Persist a settings CDO to the ini it actually belongs in.
	 * CLASS_DefaultConfig -> TryUpdateDefaultConfigFile() (versioned Config/Default*.ini).
	 * Anything else       -> SaveConfig() + an explicit GConfig flush, and we say out loud,
	 *                        in the payload, that the destination is the per-user/Saved ini.
	 * Returns false only when the write itself was refused (read-only ini, non-config class).
	 */
	bool PSCfgPersistSettingsObject(UObject* CDO, FPSCfgWriteTarget& Out)
	{
		if (!CDO) { Out.FailReason = TEXT("null settings object"); return false; }
		UClass* Cls = CDO->GetClass();
		if (!Cls->HasAnyClassFlags(CLASS_Config))
		{
			Out.FailReason = FString::Printf(
				TEXT("%s is not a UCLASS(config=...) class: no ini is ever written for it"), *Cls->GetName());
			return false;
		}
		Out.bDefaultConfigClass = Cls->HasAnyClassFlags(CLASS_DefaultConfig);
		if (Out.bDefaultConfigClass)
		{
			Out.IniPath = CDO->GetDefaultConfigFilename();      // <Project>/Config/Default<Base>.ini
			Out.bWriteReported = CDO->TryUpdateDefaultConfigFile();
			if (!Out.bWriteReported)
			{
				Out.FailReason = FString::Printf(
					TEXT("TryUpdateDefaultConfigFile refused: '%s' exists and is read-only"), *Out.IniPath);
			}
		}
		else
		{
			// Not a defaultconfig class. SaveConfig's own destination is the honest answer even
			// though it is usually under Saved/Config (gitignored); the caller sees it in ini_path
			// and in versioned_ini=false.
			Out.IniPath = GetConfigFilename(CDO);
			CDO->SaveConfig();
			if (GConfig) { GConfig->Flush(false, Out.IniPath); } // force it out before we re-read the file
			Out.bWriteReported = true;
		}
		Out.IniPath = FPaths::ConvertRelativePathToFull(Out.IniPath);
		const FString SourceConfigDirFull = FPaths::ConvertRelativePathToFull(FPaths::SourceConfigDir());
		Out.bVersionedIni = !SourceConfigDirFull.IsEmpty() && Out.IniPath.StartsWith(SourceConfigDirFull, ESearchCase::IgnoreCase);
		return Out.bWriteReported;
	}

	/** Read one key out of one ini FILE. FConfigFile::Read == disk; GConfig would be the cache. */
	FPSCfgIniProbe PSCfgProbeIniOnDisk(const FString& IniPath, const FString& Section, const FString& Key)
	{
		FPSCfgIniProbe Probe;
		if (IniPath.IsEmpty() || Section.IsEmpty() || Key.IsEmpty()) { return Probe; }
		Probe.bFileExists = FPaths::FileExists(IniPath);
		if (!Probe.bFileExists) { return Probe; }

		FConfigFile Cfg;
		Cfg.Read(IniPath); // single-file DISK read, no hierarchy/platform merge, no GConfig
		const FConfigSection* Sec = Cfg.FindSection(Section);
		if (!Sec) { return Probe; }
		Probe.bSectionFound = true;

		const FName KeyName(*Key);
		for (const TPair<FName, FConfigValue>& Pair : *Sec)
		{
			if (Pair.Key == KeyName)
			{
				Probe.Values.Add(Pair.Value.GetSavedValue()); // raw text as written, not the expanded value
			}
		}
		Probe.bKeyFound = Probe.Values.Num() > 0;
		return Probe;
	}

	/** Ini text vs ExportText text -- trim, then drop one layer of surrounding double quotes. */
	FString PSCfgNormalizeIniText(const FString& In)
	{
		FString S = In;
		S.TrimStartAndEndInline();
		if (S.Len() >= 2 && S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\"")))
		{
			S = S.Mid(1, S.Len() - 2);
		}
		return S;
	}

	/**
	 * re-read ONE property out of the ini file and report, honestly, whether disk agrees
	 * with memory. Fills OutBlock with the evidence and returns the verdict for that property.
	 *
	 * The three non-obvious verdicts, spelled out so nobody has to guess:
	 *  - property is not UPROPERTY(config)  -> NotVerifiable (no ini line exists, ever).
	 *  - key absent AND the value equals the parent-class default -> Verified: SaveConfig
	 *    deliberately omits inherited values identical to the super CDO, and the effective
	 *    value read back from that ini IS the requested one.
	 *  - array/set/map -> NotVerifiable: the key appears once per element (+Key= lines) and a
	 *    1:1 text compare against a single ExportText string would be made up.
	 */
	mcp_common::EMcpVerification PSCfgVerifyPropertyOnDisk(
		UObject* CDO, FProperty* Prop, const FString& IniPath,
		TSharedPtr<FJsonObject>& OutBlock, FString& OutDetail)
	{
		OutBlock = MakeShared<FJsonObject>();
		if (!CDO || !Prop)
		{
			OutDetail = TEXT("no property to verify");
			return mcp_common::EMcpVerification::NotVerifiable;
		}

		const FString Section = PSCfgSectionForProperty(CDO, Prop);
		const FString Key     = PSCfgKeyForProperty(Prop);
		OutBlock->SetStringField(TEXT("property_name"), Prop->GetName());
		OutBlock->SetStringField(TEXT("ini_section"), Section);
		OutBlock->SetStringField(TEXT("ini_key"), Key);

		const bool bIsConfigProp = Prop->HasAnyPropertyFlags(CPF_Config);
		OutBlock->SetBoolField(TEXT("property_is_config"), bIsConfigProp);
		if (!bIsConfigProp)
		{
			OutDetail = FString::Printf(
				TEXT("'%s' is not UPROPERTY(config): no ini line exists for it, the write is in-memory only"),
				*Prop->GetName());
			OutBlock->SetBoolField(TEXT("disk_present"), false);
			OutBlock->SetBoolField(TEXT("disk_matches_memory"), false);
			return mcp_common::EMcpVerification::NotVerifiable;
		}

		// What the CDO holds right now, in both text forms (PPF_None is what this file's
		// round_tripped_value uses; PPF_SerializedAsImportText is what SaveConfig writes).
		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);
		FString MemPlain, MemImport;
		Prop->ExportTextItem_Direct(MemPlain,  ValuePtr, nullptr, nullptr, PPF_None);
		Prop->ExportTextItem_Direct(MemImport, ValuePtr, nullptr, nullptr, PPF_SerializedAsImportText);
		OutBlock->SetStringField(TEXT("memory_value"), MemPlain);

		const FPSCfgIniProbe Probe = PSCfgProbeIniOnDisk(IniPath, Section, Key);
		OutBlock->SetStringField(TEXT("ini_path"), IniPath);
		OutBlock->SetBoolField(TEXT("ini_file_exists"), Probe.bFileExists);
		OutBlock->SetBoolField(TEXT("ini_section_found"), Probe.bSectionFound);
		OutBlock->SetBoolField(TEXT("disk_present"), Probe.bKeyFound);

		OutBlock->SetArrayField(TEXT("disk_values"), mcp_common::StringsToJson(Probe.Values));
		OutBlock->SetStringField(TEXT("disk_value"), Probe.Values.Num() > 0 ? Probe.Values[0] : FString());

		if (!Probe.bFileExists)
		{
			OutDetail = FString::Printf(TEXT("ini file not on disk: %s"), *IniPath);
			OutBlock->SetBoolField(TEXT("disk_matches_memory"), false);
			return mcp_common::EMcpVerification::Failed;
		}

		if (!Probe.bKeyFound)
		{
			// SaveConfig omits an INHERITED property whose value equals the super CDO's --
			// the key's absence is then the correct on-disk state, not a lost write.
			bool bEqualsParentDefault = false;
			UClass* OwnerClass = Prop->GetOwnerClass();
			UClass* SuperClass = CDO->GetClass()->GetSuperClass();
			if (OwnerClass && SuperClass && OwnerClass != CDO->GetClass() && SuperClass->IsChildOf(OwnerClass))
			{
				if (UObject* SuperCDO = SuperClass->GetDefaultObject())
				{
					const mcp_common::FPropertyDiffResult Diff =
						mcp_common::ComparePropertyValues(Prop, CDO, SuperCDO);
					bEqualsParentDefault = !Diff.bDiffers;
				}
			}
			OutBlock->SetBoolField(TEXT("equals_parent_class_default"), bEqualsParentDefault);
			OutBlock->SetBoolField(TEXT("disk_matches_memory"), bEqualsParentDefault);
			if (bEqualsParentDefault)
			{
				OutDetail = FString::Printf(
					TEXT("key '%s' intentionally absent from [%s] in %s: the value equals the parent-class default, which SaveConfig omits; the effective on-disk value is the requested one"),
					*Key, *Section, *IniPath);
				return mcp_common::EMcpVerification::Verified;
			}
			OutDetail = FString::Printf(
				TEXT("key '%s' is NOT present under [%s] in %s -- nothing on disk backs the in-memory value '%s'"),
				*Key, *Section, *IniPath, *MemPlain);
			return mcp_common::EMcpVerification::Failed;
		}

		// Containers write one line per element; comparing them against a single ExportText
		// string would be an invented result, so we report the lines and decline the verdict.
		const bool bContainer = Prop->IsA(FArrayProperty::StaticClass())
			|| Prop->IsA(FSetProperty::StaticClass())
			|| Prop->IsA(FMapProperty::StaticClass());
		if (bContainer)
		{
			OutDetail = FString::Printf(
				TEXT("container property: %d line(s) for key '%s' under [%s] in %s; per-element text comparison not attempted"),
				Probe.Values.Num(), *Key, *Section, *IniPath);
			OutBlock->SetBoolField(TEXT("disk_matches_memory"), false);
			return mcp_common::EMcpVerification::NotVerifiable;
		}

		const FString DiskNorm = PSCfgNormalizeIniText(Probe.Values[0]);
		const bool bMatches =
			DiskNorm.Equals(PSCfgNormalizeIniText(MemPlain), ESearchCase::CaseSensitive) ||
			DiskNorm.Equals(PSCfgNormalizeIniText(MemImport), ESearchCase::CaseSensitive);
		OutBlock->SetBoolField(TEXT("disk_matches_memory"), bMatches);
		if (bMatches)
		{
			OutDetail = FString::Printf(TEXT("%s -> [%s] %s = %s"), *IniPath, *Section, *Key, *Probe.Values[0]);
			return mcp_common::EMcpVerification::Verified;
		}
		OutDetail = FString::Printf(
			TEXT("disk has '%s', memory has '%s' ([%s] %s in %s)"),
			*Probe.Values[0], *MemPlain, *Section, *Key, *IniPath);
		return mcp_common::EMcpVerification::Failed;
	}

	/**
	 * The CDO of the settings class ClassName names (see ResolveSettingsClass) and its property PropertyName. Null on
	 * success, else the error reply.
	 */
	TSharedPtr<FJsonObject> PSCfgResolveSettingsProperty(const FString& ClassName, const FString& PropertyName,
		UObject*& OutCDO, FProperty*& OutProp)
	{
		UClass* SettingsClass = ResolveSettingsClass(ClassName);
		if (!SettingsClass)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("UDeveloperSettings-derived class not found: %s"), *ClassName));
		}
		OutCDO = SettingsClass->GetDefaultObject();
		if (!OutCDO)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("No CDO for %s"), *ClassName));
		}
		OutProp = SettingsClass->FindPropertyByName(FName(*PropertyName));
		if (!OutProp)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *ClassName));
		}
		return nullptr;
	}

	/** Copy the write target's facts into a handler payload (same field names everywhere). */
	void PSCfgAddWriteTargetFields(const TSharedPtr<FJsonObject>& Data, const FPSCfgWriteTarget& Target)
	{
		if (!Data.IsValid()) { return; }
		Data->SetStringField(TEXT("ini_path"), Target.IniPath);
		Data->SetBoolField(TEXT("is_default_config_class"), Target.bDefaultConfigClass);
		Data->SetBoolField(TEXT("versioned_ini"), Target.bVersionedIni);
		Data->SetBoolField(TEXT("write_reported"), Target.bWriteReported);
		Data->SetStringField(TEXT("persist_method"),
			Target.bDefaultConfigClass ? TEXT("UObject::TryUpdateDefaultConfigFile") : TEXT("UObject::SaveConfig"));
		if (!Target.FailReason.IsEmpty())
		{
			Data->SetStringField(TEXT("persist_error"), Target.FailReason);
		}
	}

	// Collision channel/profile helpers ---------------------------------------

	bool ProjSettingsParseResponse(const FString& In, ECollisionResponse& Out)
	{
		if (In.Equals(TEXT("Block"), ESearchCase::IgnoreCase))   { Out = ECR_Block; return true; }
		if (In.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase))  { Out = ECR_Ignore; return true; }
		if (In.Equals(TEXT("Overlap"), ESearchCase::IgnoreCase)) { Out = ECR_Overlap; return true; }
		return false;
	}

	FString ProjSettingsResponseToString(ECollisionResponse R)
	{
		switch (R)
		{
		case ECR_Block:   return TEXT("Block");
		case ECR_Ignore:  return TEXT("Ignore");
		case ECR_Overlap: return TEXT("Overlap");
		default:          return TEXT("Unknown");
		}
	}

	/** Reflects into UCollisionProfile's private DefaultChannelResponses TArray<FCustomChannelSetup>. */
	FScriptArrayHelper ProjSettingsGetChannelArrayHelper(UCollisionProfile* Profile, FArrayProperty*& OutArrProp)
	{
		OutArrProp = CastField<FArrayProperty>(
			UCollisionProfile::StaticClass()->FindPropertyByName(TEXT("DefaultChannelResponses")));
		check(OutArrProp);
		void* ArrPtr = OutArrProp->ContainerPtrToValuePtr<void>(Profile);
		return FScriptArrayHelper(OutArrProp, ArrPtr);
	}

	bool ProjSettingsParseCollisionEnabled(const FString& In, ECollisionEnabled::Type& Out)
	{
		if (In.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase))      { Out = ECollisionEnabled::NoCollision; return true; }
		if (In.Equals(TEXT("QueryOnly"), ESearchCase::IgnoreCase))       { Out = ECollisionEnabled::QueryOnly; return true; }
		if (In.Equals(TEXT("PhysicsOnly"), ESearchCase::IgnoreCase))     { Out = ECollisionEnabled::PhysicsOnly; return true; }
		if (In.Equals(TEXT("QueryAndPhysics"), ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::QueryAndPhysics; return true; }
		if (In.Equals(TEXT("ProbeOnly"), ESearchCase::IgnoreCase))       { Out = ECollisionEnabled::ProbeOnly; return true; }
		if (In.Equals(TEXT("QueryAndProbe"), ESearchCase::IgnoreCase))   { Out = ECollisionEnabled::QueryAndProbe; return true; }
		return false;
	}

	FString ProjSettingsCollisionEnabledToString(ECollisionEnabled::Type In)
	{
		switch (In)
		{
		case ECollisionEnabled::NoCollision:      return TEXT("NoCollision");
		case ECollisionEnabled::QueryOnly:        return TEXT("QueryOnly");
		case ECollisionEnabled::PhysicsOnly:      return TEXT("PhysicsOnly");
		case ECollisionEnabled::QueryAndPhysics:  return TEXT("QueryAndPhysics");
		case ECollisionEnabled::ProbeOnly:        return TEXT("ProbeOnly");
		case ECollisionEnabled::QueryAndProbe:    return TEXT("QueryAndProbe");
		default:                                  return TEXT("Unknown");
		}
	}

	/** The project's collision profile object. Null on success, else the error reply. */
	TSharedPtr<FJsonObject> ProjSettingsGetCollisionProfile(UCollisionProfile*& OutProfile)
	{
		OutProfile = UCollisionProfile::Get();
		if (!OutProfile)
		{
			return mcp_common::MakeErrorReply(TEXT("UCollisionProfile::Get() returned null"));
		}
		return nullptr;
	}
}

// ---------------------------------------------------------------------------
// list_project_setting_sections
//   Walks every loaded UClass, filters for UDeveloperSettings derivatives,
//   returns their class name + CDO container / category. Headless-safe;
//   does not load anything new from disk.
// ---------------------------------------------------------------------------
// MCP:list_project_setting_sections - List every loaded, concrete UDeveloperSettings subclass (the Project/Editor Settings sections) without loading new modules, read-only. Returns count and sections [{class_name, class_path, container_name, category_name, section_name, section_text}]; pass class_name to get_project_settings_value or set_project_settings_value.
//   @returns { count: int, sections: [object] }
TSharedPtr<FJsonObject> HandleListProjectSettingSections(
	const TSharedPtr<FJsonObject>& /*Params*/)
{
	TArray<TSharedPtr<FJsonValue>> SectionArr;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Candidate = *It;
		if (!Candidate) { continue; }
		if (Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) { continue; }
		if (!Candidate->IsChildOf(UDeveloperSettings::StaticClass())) { continue; }
		const UDeveloperSettings* CDO = GetDefault<UDeveloperSettings>(Candidate);
		if (!CDO) { continue; }
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("class_name"), Candidate->GetName());
		Entry->SetStringField(TEXT("class_path"), Candidate->GetPathName());
		Entry->SetStringField(TEXT("container_name"), CDO->GetContainerName().ToString());
		Entry->SetStringField(TEXT("category_name"), CDO->GetCategoryName().ToString());
		Entry->SetStringField(TEXT("section_name"), CDO->GetSectionName().ToString());
		Entry->SetStringField(TEXT("section_text"), CDO->GetSectionText().ToString());
		SectionArr.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), SectionArr.Num());
	Data->SetArrayField(TEXT("sections"), SectionArr);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_project_settings_value {class_name, property_name}
//   Returns the property's text form (round-trippable for primitives and
//   common structs). For UObject* properties returns "".
// ---------------------------------------------------------------------------
// MCP:get_project_settings_value - Read one property of a settings class default object (CDO) as its ExportText string; this is the in-memory value, not re-read from the .ini on disk. Returns class_name, property_name and value; an unknown class or property is an error. set_project_settings_value is the writer.
//   @param class_name - Short name of a loaded UDeveloperSettings subclass without the U prefix (e.g. "PhysicsSettings"), or a /Script/Module.Class path
//   @param property_name - Name of the property on the settings class, e.g. "DefaultGravityZ"
//   @returns { class_name: string, property_name: string, value: string }
TSharedPtr<FJsonObject> HandleGetProjectSettingsValue(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName, PropertyName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
	UObject* CDO = nullptr;
	FProperty* Prop = nullptr;
	if (TSharedPtr<FJsonObject> Error = PSCfgResolveSettingsProperty(ClassName, PropertyName, CDO, Prop)) { return Error; }
	FString Value;
	if (!ReadPropertyAsString(CDO, Prop, Value))
	{
		return mcp_common::MakeErrorReply(
			TEXT("Failed to export property value"));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), ClassName);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	Data->SetStringField(TEXT("value"), Value);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_project_settings_value {class_name, property_name, value, verify_disk?}
//   Writes via ImportText, then persists to the VERSIONED Config/Default*.ini via
//   TryUpdateDefaultConfigFile when the class is `defaultconfig` (SaveConfig only for
//   classes that genuinely live in the per-user/Saved ini), and finally RE-READS THE
//   FILE FROM DISK to prove the write landed. See the PSCfg* block at the top.
// ---------------------------------------------------------------------------
// MCP:set_project_settings_value - Write a UDeveloperSettings property, persist it to the VERSIONED Config/Default*.ini (TryUpdateDefaultConfigFile for `defaultconfig` classes, SaveConfig otherwise) and re-read that .ini FROM DISK to prove it landed.
//   @param class_name - Class short name or /Script/Module.Class path
//   @param property_name - Name of the property on the settings class
//   @param value - New value as a string (ImportText format)
//   @param verify_disk - Optional bool, default true. Re-read the target .ini with FConfigFile::Read (never GConfig, which is the in-memory cache and would just echo the write back) and report disk_value / disk_matches_memory.
//   @returns { class_name: string, property_name: string, round_tripped_value: string, ini_path: string, is_default_config_class: bool, versioned_ini: bool, write_reported: bool, persist_method: string, ini_section: string, ini_key: string, property_is_config: bool, disk_present: bool, disk_value: string, disk_values: [string], disk_matches_memory: bool, verification: object, verification_checks: [object] }
TSharedPtr<FJsonObject> HandleSetProjectSettingsValue(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassName, PropertyName, Value;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }
	UObject* CDO = nullptr;
	FProperty* Prop = nullptr;
	if (TSharedPtr<FJsonObject> Error = PSCfgResolveSettingsProperty(ClassName, PropertyName, CDO, Prop)) { return Error; }
	// The importer's own verdict decides -- a value the property cannot parse is an
	// error, not a silent no-op reported as success.
	if (!WritePropertyByImport(CDO, Prop, Value))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Could not import value '%s' into %s::%s (%s)"),
			*Value, *ClassName, *PropertyName, *Prop->GetClass()->GetName()));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), ClassName);
	Data->SetStringField(TEXT("property_name"), PropertyName);
	FString Roundtrip;
	ReadPropertyAsString(CDO, Prop, Roundtrip);
	Data->SetStringField(TEXT("round_tripped_value"), Roundtrip);

	// Persist to the ini the class actually belongs in (see the PSCfg* block above).
	FPSCfgWriteTarget Target;
	const bool bPersisted = PSCfgPersistSettingsObject(CDO, Target);
	PSCfgAddWriteTargetFields(Data, Target);

	bool bVerifyDisk = true;
	Params->TryGetBoolField(TEXT("verify_disk"), bVerifyDisk);

	if (!bPersisted)
	{
		mcp_common::AddVerificationEvidence(Data, mcp_common::EMcpVerification::Failed,
			TEXT("UObject::TryUpdateDefaultConfigFile"), Target.FailReason);
		return mcp_common::MakeOutcomeReply(Data, false, Target.FailReason);
	}
	if (!bVerifyDisk)
	{
		mcp_common::AddVerificationEvidence(Data, mcp_common::EMcpVerification::NotAttempted,
			TEXT("FConfigFile::Read"), TEXT("verify_disk=false: the .ini was written but not re-read"));
		return mcp_common::MakeOutcomeReply(Data, true);
	}

	TSharedPtr<FJsonObject> DiskBlock;
	FString Detail;
	const mcp_common::EMcpVerification Verdict =
		PSCfgVerifyPropertyOnDisk(CDO, Prop, Target.IniPath, DiskBlock, Detail);
	// Flatten the evidence onto the payload -- this tool touches exactly one property.
	if (DiskBlock.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : DiskBlock->Values)
		{
			if (Pair.Key == TEXT("property_name")) { continue; } // already at the top level
			Data->SetField(Pair.Key, Pair.Value);
		}
	}
	const bool bOk = (Verdict != mcp_common::EMcpVerification::Failed);
	return mcp_common::MakeVerifiedReply(Data, bOk, Verdict, TEXT("FConfigFile::Read"), Detail,
		bOk ? FString() : Detail);
}

// ---------------------------------------------------------------------------
// set_default_game_map {editor_startup_map?, game_default_map?, verify_disk?}
//   Writes to UGameMapsSettings::EditorStartupMap / GameDefaultMap. At least
//   one must be supplied.
//
//   Persists through TryUpdateDefaultConfigFile() and verifies with a real FConfigFile::Read
//   of the file on disk (see VERSIONED-INI PERSISTENCE above).
// ---------------------------------------------------------------------------
// MCP:set_default_game_map - Set UGameMapsSettings EditorStartupMap / GameDefaultMap, persist them to the VERSIONED Config/DefaultEngine.ini (TryUpdateDefaultConfigFile) and re-read that .ini FROM DISK to prove it landed.
//   @param editor_startup_map - Optional long package path of the editor startup map
//   @param game_default_map - Optional long package path of the default game map
//   @param verify_disk - Optional bool, default true. Re-read Config/DefaultEngine.ini with FConfigFile::Read (never GConfig, which is the in-memory cache) and report disk_value per map.
//   @returns { editor_startup_map: string, game_default_map: string, ini_path: string, is_default_config_class: bool, versioned_ini: bool, write_reported: bool, persist_method: string, disk_value: { editor_startup_map: string, game_default_map: string }, disk_checks: [object], verification: object, verification_checks: [object] }
TSharedPtr<FJsonObject> HandleSetDefaultGameMap(
	const TSharedPtr<FJsonObject>& Params)
{
	FString EditorMap, GameMap;
	Params->TryGetStringField(TEXT("editor_startup_map"), EditorMap);
	Params->TryGetStringField(TEXT("game_default_map"), GameMap);
	if (EditorMap.IsEmpty() && GameMap.IsEmpty())
	{
		return mcp_common::MakeErrorReply(
			TEXT("editor_startup_map or game_default_map is required"));
	}
	UGameMapsSettings* Settings = GetMutableDefault<UGameMapsSettings>();
	if (!Settings)
	{
		return mcp_common::MakeErrorReply(TEXT("No UGameMapsSettings CDO"));
	}
	if (!EditorMap.IsEmpty()) { Settings->EditorStartupMap = FSoftObjectPath(EditorMap); }
	if (!GameMap.IsEmpty())   { UGameMapsSettings::SetGameDefaultMap(GameMap); }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("editor_startup_map"), Settings->EditorStartupMap.ToString());
	Data->SetStringField(TEXT("game_default_map"), UGameMapsSettings::GetGameDefaultMap());

	FPSCfgWriteTarget Target;
	const bool bPersisted = PSCfgPersistSettingsObject(Settings, Target);
	PSCfgAddWriteTargetFields(Data, Target);

	bool bVerifyDisk = true;
	Params->TryGetBoolField(TEXT("verify_disk"), bVerifyDisk);

	if (!bPersisted)
	{
		mcp_common::AddVerificationEvidence(Data, mcp_common::EMcpVerification::Failed,
			TEXT("UObject::TryUpdateDefaultConfigFile"), Target.FailReason);
		return mcp_common::MakeOutcomeReply(Data, false, Target.FailReason);
	}
	if (!bVerifyDisk)
	{
		mcp_common::AddVerificationEvidence(Data, mcp_common::EMcpVerification::NotAttempted,
			TEXT("FConfigFile::Read"), TEXT("verify_disk=false: DefaultEngine.ini was written but not re-read"));
		return mcp_common::MakeOutcomeReply(Data, true);
	}

	// Verify ONLY the keys the caller actually asked us to change -- claiming a verdict on the
	// other one would be noise at best and a fake check at worst.
	TSharedPtr<FJsonObject> DiskValues = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> DiskChecks;
	bool bAnyFailed = false;
	FString FirstFailDetail;

	auto VerifyOne = [&](const TCHAR* PropName, const TCHAR* JsonKey)
	{
		FProperty* Prop = UGameMapsSettings::StaticClass()->FindPropertyByName(FName(PropName));
		TSharedPtr<FJsonObject> Block;
		FString Detail;
		const mcp_common::EMcpVerification Verdict =
			PSCfgVerifyPropertyOnDisk(Settings, Prop, Target.IniPath, Block, Detail);
		if (Block.IsValid())
		{
			Block->SetStringField(TEXT("requested_key"), JsonKey);
			DiskChecks.Add(MakeShared<FJsonValueObject>(Block));
			FString DiskVal;
			Block->TryGetStringField(TEXT("disk_value"), DiskVal);
			DiskValues->SetStringField(JsonKey, DiskVal);
		}
		mcp_common::AddVerificationEvidence(Data, Verdict, TEXT("FConfigFile::Read"), Detail);
		if (Verdict == mcp_common::EMcpVerification::Failed)
		{
			bAnyFailed = true;
			if (FirstFailDetail.IsEmpty()) { FirstFailDetail = Detail; }
		}
	};

	if (!EditorMap.IsEmpty()) { VerifyOne(TEXT("EditorStartupMap"), TEXT("editor_startup_map")); }
	if (!GameMap.IsEmpty())   { VerifyOne(TEXT("GameDefaultMap"),   TEXT("game_default_map")); }

	Data->SetObjectField(TEXT("disk_value"), DiskValues);
	Data->SetArrayField(TEXT("disk_checks"), DiskChecks);
	return mcp_common::MakeOutcomeReply(Data, !bAnyFailed, FirstFailDetail);
}

// ---------------------------------------------------------------------------
// get_default_game_map {include_disk_value?}
//   Reads back UGameMapsSettings::EditorStartupMap / GameDefaultMap. Inverse
//   of set_default_game_map — same two keys, identical round-trip.
//
//   It also reports what Config/DefaultEngine.ini says. memory_matches_disk=false
//   means "the CDO holds this value but no ini line backs it" — the state a
//   SaveConfig()-only write leaves the project in.
// ---------------------------------------------------------------------------
// MCP:get_default_game_map - Read UGameMapsSettings EditorStartupMap and GameDefaultMap from the in-memory CDO, and (by default) ALSO from Config/DefaultEngine.ini on disk so a memory-only write cannot masquerade as a persisted one.
//   @param include_disk_value - Optional bool, default true. Also read the same two keys straight out of Config/DefaultEngine.ini with FConfigFile::Read (never GConfig) and report disk_value + memory_matches_disk.
//   @returns { editor_startup_map: string, game_default_map: string, ini_path: string, disk_value: { editor_startup_map: string, game_default_map: string }, memory_matches_disk: bool, disk_checks: [object] }
TSharedPtr<FJsonObject> HandleGetDefaultGameMap(
	const TSharedPtr<FJsonObject>& Params)
{
	UGameMapsSettings* Settings = GetMutableDefault<UGameMapsSettings>();
	if (!Settings)
	{
		return mcp_common::MakeErrorReply(TEXT("No UGameMapsSettings CDO"));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("editor_startup_map"), Settings->EditorStartupMap.ToString());
	Data->SetStringField(TEXT("game_default_map"), UGameMapsSettings::GetGameDefaultMap());

	// Echoing the CDO alone hides a memory-only write: the mutated CDO reads back like a
	// confirmation. Reading the .ini too makes the difference
	// between "changed in memory" and "changed in the project" visible in one call.
	bool bIncludeDisk = true;
	if (Params.IsValid()) { Params->TryGetBoolField(TEXT("include_disk_value"), bIncludeDisk); }
	if (!bIncludeDisk)
	{
		return mcp_common::MakeOkReply(Data);
	}

	const FString IniPath = FPaths::ConvertRelativePathToFull(Settings->GetDefaultConfigFilename());
	Data->SetStringField(TEXT("ini_path"), IniPath);

	TSharedPtr<FJsonObject> DiskValues = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> DiskChecks;
	bool bAllMatch = true;

	auto ProbeOne = [&](const TCHAR* PropName, const TCHAR* JsonKey)
	{
		FProperty* Prop = UGameMapsSettings::StaticClass()->FindPropertyByName(FName(PropName));
		TSharedPtr<FJsonObject> Block;
		FString Detail;
		const mcp_common::EMcpVerification Verdict =
			PSCfgVerifyPropertyOnDisk(Settings, Prop, IniPath, Block, Detail);
		if (Block.IsValid())
		{
			DiskChecks.Add(MakeShared<FJsonValueObject>(Block));
			FString DiskVal;
			Block->TryGetStringField(TEXT("disk_value"), DiskVal);
			DiskValues->SetStringField(JsonKey, DiskVal);
		}
		mcp_common::AddVerificationEvidence(Data, Verdict, TEXT("FConfigFile::Read"), Detail);
		if (Verdict != mcp_common::EMcpVerification::Verified) { bAllMatch = false; }
	};

	ProbeOne(TEXT("EditorStartupMap"), TEXT("editor_startup_map"));
	ProbeOne(TEXT("GameDefaultMap"),   TEXT("game_default_map"));

	Data->SetObjectField(TEXT("disk_value"), DiskValues);
	Data->SetArrayField(TEXT("disk_checks"), DiskChecks);
	Data->SetBoolField(TEXT("memory_matches_disk"), bAllMatch);
	// Reading never fails as an operation -- the finding lives in memory_matches_disk, not in success.
	return mcp_common::MakeOutcomeReply(Data, true);
}

// ---------------------------------------------------------------------------
// get_project_info
//   Returns the project name/description/company from UGeneralProjectSettings.
// ---------------------------------------------------------------------------
// MCP:get_project_info - Read the project metadata from the in-memory UGeneralProjectSettings defaults (Project Settings, Description), read-only. Returns project_name, project_version, company_name, company_distinguished_name, copyright_notice, description, homepage, licensing_terms, privacy_policy and support_contact, as strings that may be empty.
//   @returns { project_name: string, project_version: string, company_name: string, company_distinguished_name: string, copyright_notice: string, description: string, homepage: string, licensing_terms: string, privacy_policy: string, support_contact: string }
TSharedPtr<FJsonObject> HandleGetProjectInfo(
	const TSharedPtr<FJsonObject>& /*Params*/)
{
	const UGeneralProjectSettings* Settings = GetDefault<UGeneralProjectSettings>();
	if (!Settings)
	{
		return mcp_common::MakeErrorReply(TEXT("No UGeneralProjectSettings CDO"));
	}
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("project_name"),        Settings->ProjectName);
	Data->SetStringField(TEXT("project_version"),     Settings->ProjectVersion);
	Data->SetStringField(TEXT("company_name"),        Settings->CompanyName);
	Data->SetStringField(TEXT("company_distinguished_name"),
		Settings->CompanyDistinguishedName);
	Data->SetStringField(TEXT("copyright_notice"),    Settings->CopyrightNotice);
	Data->SetStringField(TEXT("description"),         Settings->Description);
	Data->SetStringField(TEXT("homepage"),            Settings->Homepage);
	Data->SetStringField(TEXT("licensing_terms"),     Settings->LicensingTerms);
	Data->SetStringField(TEXT("privacy_policy"),      Settings->PrivacyPolicy);
	Data->SetStringField(TEXT("support_contact"),     Settings->SupportContact);
	return mcp_common::MakeOkReply(Data);
}

// Project-level collision channels and profiles (UCollisionProfile), without the error-prone raw ini-array
// editing that set_project_settings_value would otherwise require.

// ---------------------------------------------------------------------------
// add_collision_channel
//   Registers a custom collision channel by claiming the first free
//   ECC_GameTraceChannelN (N=1..50) slot and naming it. No public
//   UCollisionProfile API exists to add a channel, so this reflects into the
//   private DefaultChannelResponses array (FCustomChannelSetup is itself a
//   fully public struct -- once reflection gets us the array, its elements'
//   fields are plain public C++ member access, no further FProperty needed).
// ---------------------------------------------------------------------------
// MCP:add_collision_channel - Register a custom project collision channel (claims the first free ECC_GameTraceChannelN slot). Idempotent on channel_name.
//   @param channel_name [string!] - Friendly name for the new channel, e.g. "Harvestable"
//   @param default_response [string] - "Block" | "Ignore" | "Overlap". Defaults to "Block".
//   @param trace_type [bool] - True for a trace channel, false for an object channel. Defaults to false.
//   @param static_object [bool] - Whether this channel represents a static object type. Defaults to false.
//   @returns { channel_name: string, channel: string, default_response: string, trace_type: bool, static_object: bool, already_existed: bool, note: string }
TSharedPtr<FJsonObject> HandleAddCollisionChannel(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ChannelName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("channel_name"), ChannelName)) { return Missing; }
	FString ResponseStr = TEXT("Block");
	Params->TryGetStringField(TEXT("default_response"), ResponseStr);
	ECollisionResponse DefaultResponse;
	if (!ProjSettingsParseResponse(ResponseStr, DefaultResponse))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown default_response '%s'. Use Block, Ignore, or Overlap."), *ResponseStr));
	}
	bool bTraceType = false;
	Params->TryGetBoolField(TEXT("trace_type"), bTraceType);
	bool bStaticObject = false;
	Params->TryGetBoolField(TEXT("static_object"), bStaticObject);

	UCollisionProfile* Profile = nullptr;
	if (TSharedPtr<FJsonObject> Error = ProjSettingsGetCollisionProfile(Profile)) { return Error; }

	FArrayProperty* ArrProp = nullptr;
	FScriptArrayHelper Helper = ProjSettingsGetChannelArrayHelper(Profile, ArrProp);

	const FName ChannelFName(*ChannelName);
	for (int32 i = 0; i < Helper.Num(); ++i)
	{
		FCustomChannelSetup* Elem = reinterpret_cast<FCustomChannelSetup*>(Helper.GetRawPtr(i));
		if (Elem->Name == ChannelFName)
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("channel_name"), ChannelName);
			Result->SetStringField(TEXT("channel"), StaticEnum<ECollisionChannel>()->GetNameStringByValue((int64)Elem->Channel.GetValue()));
			Result->SetStringField(TEXT("default_response"), ProjSettingsResponseToString(Elem->DefaultResponse));
			Result->SetBoolField(TEXT("trace_type"), Elem->bTraceType);
			Result->SetBoolField(TEXT("static_object"), Elem->bStaticObject);
			Result->SetBoolField(TEXT("already_existed"), true);
			return mcp_common::MakeOkReply(Result);
		}
	}

	// Claim the first free ECC_GameTraceChannelN slot (N=1..50, the custom channel pool).
	ECollisionChannel FreeChannel = ECC_MAX;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	for (int32 CandidateValue = (int32)ECC_GameTraceChannel1; CandidateValue <= (int32)ECC_GameTraceChannel18; ++CandidateValue) // UE 5.7: 18 custom slots
#else
	for (int32 CandidateValue = (int32)ECC_GameTraceChannel1; CandidateValue <= (int32)ECC_GameTraceChannel50; ++CandidateValue)
#endif
	{
		bool bOccupied = false;
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			FCustomChannelSetup* Elem = reinterpret_cast<FCustomChannelSetup*>(Helper.GetRawPtr(i));
			if ((int32)Elem->Channel.GetValue() == CandidateValue) { bOccupied = true; break; }
		}
		if (!bOccupied) { FreeChannel = (ECollisionChannel)CandidateValue; break; }
	}
	if (FreeChannel == ECC_MAX)
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		return mcp_common::MakeErrorReply(TEXT("No free custom collision channel slots (all 18 GameTraceChannel slots in use)"));
#else
		return mcp_common::MakeErrorReply(TEXT("No free custom collision channel slots (all 50 GameTraceChannel slots in use)"));
#endif
	}

	const int32 NewIndex = Helper.AddValue();
	FCustomChannelSetup* NewElem = reinterpret_cast<FCustomChannelSetup*>(Helper.GetRawPtr(NewIndex));
	NewElem->Channel = FreeChannel;
	NewElem->Name = ChannelFName;
	NewElem->DefaultResponse = DefaultResponse;
	NewElem->bTraceType = bTraceType;
	NewElem->bStaticObject = bStaticObject;

	Profile->TryUpdateDefaultConfigFile();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("channel_name"), ChannelName);
	Result->SetStringField(TEXT("channel"), StaticEnum<ECollisionChannel>()->GetNameStringByValue((int64)FreeChannel));
	Result->SetStringField(TEXT("default_response"), ProjSettingsResponseToString(DefaultResponse));
	Result->SetBoolField(TEXT("trace_type"), bTraceType);
	Result->SetBoolField(TEXT("static_object"), bStaticObject);
	Result->SetBoolField(TEXT("already_existed"), false);
	Result->SetStringField(TEXT("note"), TEXT("Persisted to DefaultEngine.ini. UCollisionProfile::LoadProfileConfig is not ENGINE_API-exported, so components already live in memory won't pick this up until the editor restarts; new actors/components created after this call will see it immediately (loaded from the in-memory CDO)."));
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// list_collision_channels
//   Enumerates every ECollisionChannel slot (built-in + engine + custom
//   game channels), its default response, and whether a custom game slot is
//   in use. Pure public API, no reflection.
// ---------------------------------------------------------------------------
// MCP:list_collision_channels - List every collision channel (built-in + custom) with its default response.
//   @returns { channels: [{channel: string, friendly_name: string, value: int, default_response: string, is_custom_game_slot: bool, in_use: bool}], count: int }
TSharedPtr<FJsonObject> HandleListCollisionChannels(
	const TSharedPtr<FJsonObject>& /*Params*/)
{
	UCollisionProfile* Profile = nullptr;
	if (TSharedPtr<FJsonObject> Error = ProjSettingsGetCollisionProfile(Profile)) { return Error; }
	UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();
	if (!ChannelEnum)
	{
		return mcp_common::MakeErrorReply(TEXT("StaticEnum<ECollisionChannel>() returned null"));
	}

	TArray<TSharedPtr<FJsonValue>> ChannelArr;
	for (int32 Idx = 0; Idx < ChannelEnum->NumEnums() - 1; ++Idx) // skip the implicit _MAX entry
	{
		const int64 Value = ChannelEnum->GetValueByIndex(Idx);
		if (Value < 0 || Value > 63) { continue; } // deprecated/out-of-range aliases, not a real slot
		const FString RawName = ChannelEnum->GetNameStringByIndex(Idx); // e.g. "ECC_GameTraceChannel1"
		FString RawShortName = RawName;
		RawShortName.RemoveFromStart(TEXT("ECC_"));
		const FName Friendly = Profile->ReturnChannelNameFromContainerIndex((int32)Value);
		const bool bIsGameSlot = RawName.StartsWith(TEXT("ECC_GameTraceChannel"));
		const bool bInUse = bIsGameSlot && !Friendly.ToString().Equals(RawShortName, ESearchCase::IgnoreCase);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("channel"), RawName);
		Entry->SetStringField(TEXT("friendly_name"), Friendly.ToString());
		Entry->SetNumberField(TEXT("value"), Value);
		Entry->SetStringField(TEXT("default_response"),
			ProjSettingsResponseToString(FCollisionResponseContainer::GetDefaultResponseContainer().GetResponse((ECollisionChannel)Value)));
		Entry->SetBoolField(TEXT("is_custom_game_slot"), bIsGameSlot);
		if (bIsGameSlot)
		{
			Entry->SetBoolField(TEXT("in_use"), bInUse);
		}
		ChannelArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("channels"), ChannelArr);
	Result->SetNumberField(TEXT("count"), ChannelArr.Num());
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// add_collision_profile
//   Creates a new collision profile via the public
//   FCollisionProfilePrivateAccessor::AddProfileTemplate (handles
//   SaveCustomResponses + LoadProfileConfig + TryUpdateDefaultConfigFile
//   internally, safely, since it's called from within the Engine module).
//   Updating an EXISTING profile (idempotent path) reflects into the
//   private Profiles array instead, since AddProfileTemplate no-ops on a
//   name collision by design.
// ---------------------------------------------------------------------------
// MCP:add_collision_profile - Create a project collision profile, or replace an existing one of that name in full (omitted fields reset, custom responses cleared, can_modify not checked), then write Config/DefaultEngine.ini via TryUpdateDefaultConfigFile, unverified. Returns custom_response_count and already_existed; read it back with get_collision_profile.
//   @param profile_name [string!] - Name of the collision profile to create or replace, e.g. "MyPawnProfile"
//   @param object_type_name [string!] - Channel this profile's object type resolves to, e.g. "WorldDynamic" or a custom channel's friendly name
//   @param collision_enabled [string] - "NoCollision" | "QueryOnly" | "PhysicsOnly" | "QueryAndPhysics" | "ProbeOnly" | "QueryAndProbe". Defaults to "QueryAndPhysics".
//   @param custom_responses [array] - [{channel: string, response: "Block"|"Ignore"|"Overlap"}, ...] per-channel overrides
//   @param can_modify [bool] - Whether this profile is user-editable in the editor UI. Defaults to true.
//   @param help_message [string] - Editor tooltip text.
//   @returns { profile_name: string, object_type_name: string, collision_enabled: string, custom_response_count: int, already_existed: bool }
TSharedPtr<FJsonObject> HandleAddCollisionProfile(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ProfileName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("profile_name"), ProfileName)) { return Missing; }
	FString ObjectTypeName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_type_name"), ObjectTypeName)) { return Missing; }

	UCollisionProfile* Profile = nullptr;
	if (TSharedPtr<FJsonObject> Error = ProjSettingsGetCollisionProfile(Profile)) { return Error; }

	FCollisionResponseTemplate ExistingCheck;
	const bool bAlreadyExists = Profile->GetProfileTemplate(FName(*ProfileName), ExistingCheck);

	FCollisionResponseTemplate Template;
	Template.Name = FName(*ProfileName);
	Template.ObjectTypeName = FName(*ObjectTypeName);

	FString CollisionEnabledStr = TEXT("QueryAndPhysics");
	Params->TryGetStringField(TEXT("collision_enabled"), CollisionEnabledStr);
	ECollisionEnabled::Type CollisionEnabledValue;
	if (!ProjSettingsParseCollisionEnabled(CollisionEnabledStr, CollisionEnabledValue))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown collision_enabled '%s'"), *CollisionEnabledStr));
	}
	Template.CollisionEnabled = CollisionEnabledValue;

	bool bCanModify = true;
	Params->TryGetBoolField(TEXT("can_modify"), bCanModify);
	Template.bCanModify = bCanModify;

	FString HelpMessage;
	Params->TryGetStringField(TEXT("help_message"), HelpMessage);
	Template.HelpMessage = HelpMessage;

	const TArray<TSharedPtr<FJsonValue>>* CustomResponsesArr = nullptr;
	if (Params->TryGetArrayField(TEXT("custom_responses"), CustomResponsesArr) && CustomResponsesArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *CustomResponsesArr)
		{
			const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
			if (!V.IsValid() || !V->TryGetObject(EntryObjPtr) || !EntryObjPtr || !EntryObjPtr->IsValid())
			{
				continue;
			}
			FString ChanName, RespName;
			if (!(*EntryObjPtr)->TryGetStringField(TEXT("channel"), ChanName)) { continue; }
			if (!(*EntryObjPtr)->TryGetStringField(TEXT("response"), RespName)) { continue; }
			ECollisionResponse Resp;
			if (!ProjSettingsParseResponse(RespName, Resp))
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("Unknown response '%s' for channel '%s'. Use Block, Ignore, or Overlap."), *RespName, *ChanName));
			}
			Template.CustomResponses.Add(FResponseChannel(FName(*ChanName), Resp));
		}
	}

	if (bAlreadyExists)
	{
		FArrayProperty* ProfilesArrProp = CastField<FArrayProperty>(
			UCollisionProfile::StaticClass()->FindPropertyByName(TEXT("Profiles")));
		if (!ProfilesArrProp)
		{
			return mcp_common::MakeErrorReply(TEXT("Could not reflect UCollisionProfile::Profiles"));
		}
		void* ArrPtr = ProfilesArrProp->ContainerPtrToValuePtr<void>(Profile);
		FScriptArrayHelper Helper(ProfilesArrProp, ArrPtr);
		bool bReplaced = false;
		for (int32 i = 0; i < Helper.Num(); ++i)
		{
			FCollisionResponseTemplate* Elem = reinterpret_cast<FCollisionResponseTemplate*>(Helper.GetRawPtr(i));
			if (Elem->Name == Template.Name)
			{
				*Elem = Template;
				bReplaced = true;
				break;
			}
		}
		if (!bReplaced)
		{
			return mcp_common::MakeErrorReply(TEXT("Profile disappeared between existence check and update"));
		}
	}
	else
	{
		if (!FCollisionProfilePrivateAccessor::AddProfileTemplate(Template))
		{
			return mcp_common::MakeErrorReply(TEXT("AddProfileTemplate failed"));
		}
	}

	Profile->TryUpdateDefaultConfigFile();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("profile_name"), ProfileName);
	Result->SetStringField(TEXT("object_type_name"), ObjectTypeName);
	Result->SetStringField(TEXT("collision_enabled"), CollisionEnabledStr);
	Result->SetNumberField(TEXT("custom_response_count"), Template.CustomResponses.Num());
	Result->SetBoolField(TEXT("already_existed"), bAlreadyExists);
	return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// get_collision_profile
//   Pure public API read: UCollisionProfile::GetProfileTemplate.
// ---------------------------------------------------------------------------
// MCP:get_collision_profile - Read one project collision profile by name, read-only: object_type_name, collision_enabled, can_modify, help_message and custom_responses [{channel, response}], which lists only the per-channel overrides. An unknown name is an error; add_collision_profile is the writer.
//   @param profile_name [string!] - Name of an existing collision profile, e.g. "BlockAll" or "Pawn"
//   @returns { profile_name: string, object_type_name: string, collision_enabled: string, can_modify: bool, help_message: string, custom_responses: [{channel: string, response: string}] }
TSharedPtr<FJsonObject> HandleGetCollisionProfile(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ProfileName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("profile_name"), ProfileName)) { return Missing; }

	UCollisionProfile* Profile = nullptr;
	if (TSharedPtr<FJsonObject> Error = ProjSettingsGetCollisionProfile(Profile)) { return Error; }

	FCollisionResponseTemplate Found;
	if (!Profile->GetProfileTemplate(FName(*ProfileName), Found))
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("Collision profile not found: %s"), *ProfileName));
	}

	TArray<TSharedPtr<FJsonValue>> CustomArr;
	for (const FResponseChannel& RC : Found.CustomResponses)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("channel"), RC.Channel.ToString());
		Entry->SetStringField(TEXT("response"), ProjSettingsResponseToString(RC.Response));
		CustomArr.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("profile_name"), ProfileName);
	Result->SetStringField(TEXT("object_type_name"), Found.ObjectTypeName.ToString());
	Result->SetStringField(TEXT("collision_enabled"), ProjSettingsCollisionEnabledToString(Found.CollisionEnabled));
	Result->SetBoolField(TEXT("can_modify"), Found.bCanModify);
	Result->SetStringField(TEXT("help_message"), Found.HelpMessage);
	Result->SetArrayField(TEXT("custom_responses"), CustomArr);
	return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_collision_channel"), &HandleAddCollisionChannel },
	{ TEXT("add_collision_profile"), &HandleAddCollisionProfile },
	{ TEXT("get_collision_profile"), &HandleGetCollisionProfile },
	{ TEXT("get_default_game_map"), &HandleGetDefaultGameMap },
	{ TEXT("get_project_info"), &HandleGetProjectInfo },
	{ TEXT("get_project_settings_value"), &HandleGetProjectSettingsValue },
	{ TEXT("list_collision_channels"), &HandleListCollisionChannels },
	{ TEXT("list_project_setting_sections"), &HandleListProjectSettingSections },
	{ TEXT("set_default_game_map"), &HandleSetDefaultGameMap },
	{ TEXT("set_project_settings_value"), &HandleSetProjectSettingsValue },
};
} // namespace UEAgentProjectSettingsCommands
