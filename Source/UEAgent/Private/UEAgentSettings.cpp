#include "UEAgentSettings.h"
#include "UEAgentProjectSettings.h"

#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

uint16 UUEAgentSettings::GetEffectiveHttpPort() const
{
	int32 Port = HttpPort;
	FParse::Value(FCommandLine::Get(), TEXT("McpHttpPort="), Port);
	return static_cast<uint16>(FMath::Clamp(Port, 1, 65535));
}

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
