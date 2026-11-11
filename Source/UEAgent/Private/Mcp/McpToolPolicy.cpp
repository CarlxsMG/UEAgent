// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpToolPolicy.h"

#include "UEAgentSettings.h"

#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpToolPolicyPrivate
{
	static FCriticalSection GCurrentLock;
	static TSharedPtr<const FMcpToolPolicy> GCurrent;
	static TAtomic<uint32> GNextGeneration(1);

	/** The names of a list, trimmed, without empties or repeats (the case is not compared). */
	static TArray<FString> CleanNames(const TArray<FString>& Names)
	{
		TArray<FString> Out;
		for (const FString& Name : Names)
		{
			const FString Trimmed = Name.TrimStartAndEnd();
			if (!Trimmed.IsEmpty() && !Out.ContainsByPredicate([&Trimmed](const FString& Kept) { return Kept.Equals(Trimmed, ESearchCase::IgnoreCase); }))
			{
				Out.Add(Trimmed);
			}
		}
		return Out;
	}

	static bool SameSettings(const FMcpToolPolicy::FSettings& A, const FMcpToolPolicy::FSettings& B)
	{
		return A.bReadOnly == B.bReadOnly && A.ReadOnlySource == B.ReadOnlySource && A.bAllowCodeExecution == B.bAllowCodeExecution
			&& A.DisabledDomains == B.DisabledDomains && A.DisabledTools == B.DisabledTools;
	}
}

TSharedRef<const FMcpToolPolicy> FMcpToolPolicy::Make(const FSettings& InSettings)
{
	using namespace McpToolPolicyPrivate;

	TSharedRef<FMcpToolPolicy> Policy = MakeShared<FMcpToolPolicy>();
	Policy->Settings = InSettings;
	Policy->Settings.DisabledDomains = CleanNames(InSettings.DisabledDomains);
	Policy->Settings.DisabledTools = CleanNames(InSettings.DisabledTools);
	if (!Policy->Settings.bReadOnly)
	{
		Policy->Settings.ReadOnlySource.Reset();
	}
	for (const FString& Domain : Policy->Settings.DisabledDomains)
	{
		Policy->DisabledDomainSet.Add(Domain.ToLower());
	}
	for (const FString& Tool : Policy->Settings.DisabledTools)
	{
		Policy->DisabledToolSet.Add(Tool.ToLower());
	}
	Policy->Generation = GNextGeneration++;
	return Policy;
}

TSharedRef<const FMcpToolPolicy> FMcpToolPolicy::GetCurrent()
{
	using namespace McpToolPolicyPrivate;

	FScopeLock Lock(&GCurrentLock);
	if (!GCurrent.IsValid())
	{
		GCurrent = Make(FSettings());
	}
	return GCurrent.ToSharedRef();
}

void FMcpToolPolicy::SetCurrent(TSharedRef<const FMcpToolPolicy> NewPolicy)
{
	using namespace McpToolPolicyPrivate;

	FScopeLock Lock(&GCurrentLock);
	GCurrent = NewPolicy;
}

bool FMcpToolPolicy::Rebuild()
{
	using namespace McpToolPolicyPrivate;

	check(IsInGameThread());
	const UUEAgentSettings* UserSettings = GetDefault<UUEAgentSettings>();
	FSettings Wanted;
	Wanted.bReadOnly = UserSettings->GetEffectiveReadOnly(Wanted.ReadOnlySource);
	Wanted.bAllowCodeExecution = UserSettings->bAllowCodeExecution;
	Wanted.DisabledDomains = CleanNames(UserSettings->DisabledDomains);
	Wanted.DisabledTools = CleanNames(UserSettings->DisabledTools);
	if (!Wanted.bReadOnly)
	{
		Wanted.ReadOnlySource.Reset();
	}

	const TSharedRef<const FMcpToolPolicy> Current = GetCurrent();
	if (SameSettings(Current->GetSettings(), Wanted))
	{
		return false;
	}
	const TSharedRef<const FMcpToolPolicy> Next = Make(Wanted);
	SetCurrent(Next);
	UE_LOG(LogTemp, Display, TEXT("UEAgent: tool permissions changed: %s"), *Next->Describe());
	return true;
}

bool FMcpToolPolicy::IsRestrictive() const
{
	return Settings.bReadOnly || !Settings.bAllowCodeExecution || DisabledDomainSet.Num() > 0 || DisabledToolSet.Num() > 0;
}

FMcpToolPolicy::EVerdict FMcpToolPolicy::Evaluate(const FMcpToolCatalog::FTool& Tool, const FString& RequestedName) const
{
	if (DisabledToolSet.Num() > 0 && (DisabledToolSet.Contains(Tool.Name.ToLower()) || (!RequestedName.IsEmpty() && DisabledToolSet.Contains(RequestedName.ToLower()))))
	{
		return EVerdict::DisabledTool;
	}
	if (DisabledDomainSet.Num() > 0 && DisabledDomainSet.Contains(Tool.Domain.ToLower()))
	{
		return EVerdict::DisabledDomain;
	}
	if (Tool.bCodeExec && !Settings.bAllowCodeExecution)
	{
		return EVerdict::CodeExecution;
	}
	if (Settings.bReadOnly)
	{
		// execute_batch only runs the tools it names, and each item goes through this check when it is dispatched.
		if (Tool.Name == TEXT("execute_batch") || Tool.bObserves)
		{
			return EVerdict::Allowed;
		}
		bool bReadOnlyHint = false;
		if (Tool.bCodeExec || !Tool.Annotations.IsValid() || !Tool.Annotations->TryGetBoolField(TEXT("readOnlyHint"), bReadOnlyHint) || !bReadOnlyHint)
		{
			return EVerdict::ReadOnly;
		}
	}
	return EVerdict::Allowed;
}

FMcpToolPolicy::EVerdict FMcpToolPolicy::EvaluateName(const FMcpToolCatalog* Catalog, const FString& Name) const
{
	if (!IsRestrictive())
	{
		return EVerdict::Allowed;
	}
	const FString Resolved = Catalog ? Catalog->ResolveName(Name) : Name;
	const FMcpToolCatalog::FTool* Tool = Catalog ? Catalog->FindTool(Resolved) : nullptr;
	if (!Tool)
	{
		if (DisabledToolSet.Contains(Name.ToLower()) || DisabledToolSet.Contains(Resolved.ToLower()))
		{
			return EVerdict::DisabledTool;
		}
		return Settings.bReadOnly ? EVerdict::ReadOnly : EVerdict::Allowed;
	}
	return Evaluate(*Tool, Name);
}

const TCHAR* FMcpToolPolicy::VerdictName(EVerdict Verdict)
{
	switch (Verdict)
	{
	case EVerdict::ReadOnly:       return TEXT("read_only");
	case EVerdict::CodeExecution:  return TEXT("code_execution");
	case EVerdict::DisabledDomain: return TEXT("disabled_domain");
	case EVerdict::DisabledTool:   return TEXT("disabled_tool");
	default:                       return TEXT("");
	}
}

FString FMcpToolPolicy::MakeRefusalMessage(EVerdict Verdict, const FString& Command, const FString& Domain) const
{
	FString Why;
	switch (Verdict)
	{
	case EVerdict::ReadOnly:
		Why = FString::Printf(TEXT("UEAgent is in read-only mode%s: only tools that read, or that only capture an image, can run, and %s changes the project or runs code"),
			Settings.ReadOnlySource.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Settings.ReadOnlySource), *Command);
		break;
	case EVerdict::CodeExecution:
		Why = FString::Printf(TEXT("code execution is turned off in UEAgent's settings, and %s runs code or a process"), *Command);
		break;
	case EVerdict::DisabledDomain:
		Why = FString::Printf(TEXT("the %s domain, which %s belongs to, is turned off in UEAgent's settings"), *Domain, *Command);
		break;
	case EVerdict::DisabledTool:
		Why = FString::Printf(TEXT("%s is turned off in UEAgent's settings"), *Command);
		break;
	default:
		return FString();
	}
	return FString::Printf(TEXT("REFUSED: %s. Nothing ran. The user chose this (Editor Preferences > Plugins > UEAgent > Permissions): ")
		TEXT("do not retry it, through call_tool, execute_batch or Python either; tell the user what you need and ask them to allow it."), *Why);
}

TSharedRef<FJsonObject> FMcpToolPolicy::MakeRefusalData(EVerdict Verdict, const FString& Command, const FString& Domain) const
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("refused"), true);
	Data->SetStringField(TEXT("refused_by"), TEXT("policy"));
	Data->SetStringField(TEXT("policy"), VerdictName(Verdict));
	Data->SetStringField(TEXT("command"), Command);
	Data->SetStringField(TEXT("domain"), Domain);
	Data->SetStringField(TEXT("hint"), TEXT("The user's settings turn this tool off. Do not retry; ask the user to allow it."));
	return Data;
}

TSharedRef<FJsonObject> FMcpToolPolicy::MakeRefusalEnvelope(EVerdict Verdict, const FString& Command, const FString& Domain) const
{
	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("status"), TEXT("error"));
	Envelope->SetStringField(TEXT("error"), MakeRefusalMessage(Verdict, Command, Domain));
	Envelope->SetObjectField(TEXT("result"), MakeRefusalData(Verdict, Command, Domain));
	return Envelope;
}

TSharedRef<FJsonObject> FMcpToolPolicy::MakeRefusalPayload(EVerdict Verdict, const FString& Command, const FString& Domain) const
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetBoolField(TEXT("success"), false);
	Payload->SetStringField(TEXT("error"), MakeRefusalMessage(Verdict, Command, Domain));
	Payload->SetObjectField(TEXT("result"), MakeRefusalData(Verdict, Command, Domain));
	return Payload;
}

FString FMcpToolPolicy::Describe() const
{
	TArray<FString> Parts;
	if (Settings.bReadOnly)
	{
		Parts.Add(FString::Printf(TEXT("read-only (%s)"), *Settings.ReadOnlySource));
	}
	if (!Settings.bAllowCodeExecution)
	{
		Parts.Add(TEXT("code execution off"));
	}
	if (Settings.DisabledDomains.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("domains off: %s"), *FString::Join(Settings.DisabledDomains, TEXT(", "))));
	}
	if (Settings.DisabledTools.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("tools off: %s"), *FString::Join(Settings.DisabledTools, TEXT(", "))));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("every tool allowed"));
}

FString FMcpToolPolicy::Instructions() const
{
	if (!IsRestrictive())
	{
		return FString();
	}
	FString Text = Settings.bReadOnly
		? TEXT("UEAgent is in READ-ONLY mode: only tools that read, or that only capture an image, are listed and run; anything that changes the project or runs code is refused.")
		: TEXT("The user has turned some UEAgent tools off: they are not listed and are refused if called.");
	if (!Settings.bReadOnly && !Settings.bAllowCodeExecution)
	{
		Text += TEXT(" Code execution (Python, console commands, processes) is off.");
	}
	return Text + TEXT(" A refusal with refused_by \"policy\" is the user's choice: do not retry or work around it; ask the user.");
}

int32 FMcpToolPolicy::CountOff(const FMcpToolCatalog& Catalog) const
{
	int32 Off = 0;
	if (IsRestrictive())
	{
		for (const FMcpToolCatalog::FTool& Tool : Catalog.GetTools())
		{
			Off += IsAllowed(Tool) ? 0 : 1;
		}
	}
	return Off;
}
