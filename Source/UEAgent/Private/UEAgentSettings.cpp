// Copyright (c) 2026 CarlxsMG.

#include "UEAgentSettings.h"
#include "UEAgentProjectSettings.h"

#include "Mcp/McpToolCatalog.h"
#include "Mcp/McpToolPolicy.h"

#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

uint16 UUEAgentSettings::GetEffectiveHttpPort() const
{
	int32 Port = HttpPort;
	FParse::Value(FCommandLine::Get(), TEXT("McpHttpPort="), Port);
	return static_cast<uint16>(FMath::Clamp(Port, 1, 65535));
}

bool UUEAgentSettings::GetEffectiveHttpKeepAlive() const
{
	return bHttpKeepAlive && !FParse::Param(FCommandLine::Get(), TEXT("McpHttpNoKeepAlive"));
}

bool UUEAgentSettings::IsReadOnlyForcedByLaunch(FString* OutSource)
{
	if (FParse::Param(FCommandLine::Get(), TEXT("McpReadOnly")))
	{
		if (OutSource)
		{
			*OutSource = TEXT("-McpReadOnly");
		}
		return true;
	}
	const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAGENT_READONLY")).TrimStartAndEnd();
	if (Env == TEXT("1") || Env.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Env.Equals(TEXT("yes"), ESearchCase::IgnoreCase))
	{
		if (OutSource)
		{
			*OutSource = TEXT("UEAGENT_READONLY");
		}
		return true;
	}
	return false;
}

bool UUEAgentSettings::GetEffectiveReadOnly(FString& OutSource) const
{
	OutSource.Reset();
	if (IsReadOnlyForcedByLaunch(&OutSource))
	{
		return true;
	}
	if (bReadOnlyMode)
	{
		OutSource = TEXT("settings");
		return true;
	}
	return false;
}

void UUEAgentSettings::SetReadOnlyMode(bool bReadOnly)
{
	if (bReadOnlyMode != bReadOnly)
	{
		bReadOnlyMode = bReadOnly;
		SaveConfig();
	}
	FMcpToolPolicy::Rebuild();
}

TArray<FString> UUEAgentSettings::GetDomainOptions() const
{
	TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::GetCurrent();
	if (!Catalog.IsValid())
	{
		FString Error;
		Catalog = FMcpToolCatalog::LoadFromFile(FMcpToolCatalog::DefaultCatalogPath(), Error);
	}
	return Catalog.IsValid() ? Catalog->GetDomains() : TArray<FString>();
}

#if WITH_EDITOR
void UUEAgentSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	FMcpToolPolicy::Rebuild();
}
#endif

namespace UEAgentSettingsPrivate
{
	static FString TrimContentRoot(FString Root)
	{
		Root.TrimStartAndEndInline();
		while (Root.Len() > 1 && Root.EndsWith(TEXT("/")))
		{
			Root.LeftChopInline(1);
		}
		return Root;
	}
}

TArray<FString> UUEAgentProjectSettings::GetReadOnlyContentRoots() const
{
	TArray<FString> Roots;
	for (const FString& Root : ReadOnlyContentRoots)
	{
		const FString Trimmed = UEAgentSettingsPrivate::TrimContentRoot(Root);
		if (!Trimmed.IsEmpty())
		{
			Roots.AddUnique(Trimmed);
		}
	}
	return Roots;
}

FString UUEAgentProjectSettings::GetProjectContentRoot() const
{
	const FString Root = UEAgentSettingsPrivate::TrimContentRoot(ProjectContentRoot);
	return Root.IsEmpty() ? FString(TEXT("/Game")) : Root;
}

FName UUEAgentProjectSettings::GetForkOriginTag() const
{
	const FString Tag = ForkOriginTag.TrimStartAndEnd();
	return FName(Tag.IsEmpty() ? TEXT("mcp.forkOrigin") : *Tag);
}
