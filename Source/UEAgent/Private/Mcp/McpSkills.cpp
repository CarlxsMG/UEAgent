#include "Mcp/McpSkills.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpSkillsPrivate
{
	using McpSkills::ESkillState;

	static const TCHAR* const SkillFile = TEXT("SKILL.md");
	static const TCHAR* const StampKey = TEXT("ueagent-version");
	// Every skill this plugin ships is named like this, so a folder without the prefix is never ours.
	static const TCHAR* const NamePrefix = TEXT("ueagent-");

	static TArray<FString> SubDirs(const FString& Dir)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("*")), /*Files*/ false, /*Directories*/ true);
		Names.Sort();
		return Names;
	}

	// The frontmatter's lines: between the opening "---" (first line) and the next "---" line.
	// Returns false when the file does not start with one.
	static bool FrontmatterLines(const FString& Text, TArray<FString>& OutLines, int32& OutCloseLine, TArray<FString>& OutAll)
	{
		// Split by hand: every line, the empty ones included, and no extra one for the final line break.
		FString Body = Text.Replace(TEXT("\r\n"), TEXT("\n"));
		if (Body.EndsWith(TEXT("\n")))
		{
			Body.LeftChopInline(1);
		}
		Body.ParseIntoArray(OutAll, TEXT("\n"), /*CullEmpty*/ false);
		if (OutAll.Num() == 0 || OutAll[0].TrimEnd() != TEXT("---"))
		{
			return false;
		}
		for (int32 i = 1; i < OutAll.Num(); ++i)
		{
			if (OutAll[i].TrimEnd() == TEXT("---"))
			{
				OutCloseLine = i;
				return true;
			}
			OutLines.Add(OutAll[i]);
		}
		return false;
	}

	// The stamp of a copy, or empty when the folder is not ours.
	static FString ReadStamp(const FString& SkillDir)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(SkillDir / SkillFile)))
		{
			return FString();
		}
		TArray<FString> Lines, All;
		int32 Close = INDEX_NONE;
		if (!FrontmatterLines(Text, Lines, Close, All))
		{
			return FString();
		}
		const FString Key = FString(StampKey) + TEXT(":");
		for (const FString& Line : Lines)
		{
			FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.StartsWith(Key))
			{
				Trimmed.RightChopInline(Key.Len());
				Trimmed.TrimStartAndEndInline();
				Trimmed.TrimQuotesInline();
				return Trimmed;
			}
		}
		return FString();
	}

	// SKILL.md with the stamp in its frontmatter, under "metadata". The shipped files carry no stamp
	// (and the check that runs in CI keeps them without one); if a "metadata" map is there, the stamp
	// joins it.
	static bool Stamp(const FString& Text, const FString& Version, FString& OutText)
	{
		TArray<FString> Lines, All;
		int32 Close = INDEX_NONE;
		if (!FrontmatterLines(Text, Lines, Close, All))
		{
			return false;
		}
		const FString Eol = Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
		const FString StampLine = FString::Printf(TEXT("  %s: \"%s\""), StampKey, *Version);
		int32 MetadataLine = INDEX_NONE;
		for (int32 i = 1; i < Close; ++i)
		{
			if (All[i].TrimEnd() == TEXT("metadata:"))
			{
				MetadataLine = i;
				break;
			}
		}
		if (MetadataLine != INDEX_NONE)
		{
			All.Insert(StampLine, MetadataLine + 1);
		}
		else
		{
			All.Insert(StampLine, Close);
			All.Insert(TEXT("metadata:"), Close);
		}
		OutText = FString::Join(All, *Eol);
		if (Text.EndsWith(TEXT("\n")))
		{
			OutText += Eol;
		}
		return true;
	}

	static bool IsOurs(const FString& Name, const FString& SkillDir)
	{
		return Name.StartsWith(NamePrefix) && !ReadStamp(SkillDir).IsEmpty();
	}

	static bool DeleteDir(const FString& Dir)
	{
		return IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true) && !IFileManager::Get().DirectoryExists(*Dir);
	}
}

TArray<McpSkills::ETarget> McpSkills::AllTargets()
{
	return { ETarget::ClaudeCode, ETarget::Agents };
}

const TCHAR* McpSkills::TargetId(ETarget Target)
{
	return Target == ETarget::ClaudeCode ? TEXT("claude") : TEXT("agents");
}

bool McpSkills::TargetFromId(const FString& Id, ETarget& OutTarget)
{
	for (const ETarget Target : AllTargets())
	{
		if (Id.Equals(TargetId(Target), ESearchCase::IgnoreCase))
		{
			OutTarget = Target;
			return true;
		}
	}
	return false;
}

const TCHAR* McpSkills::TargetRelativeDir(ETarget Target)
{
	return Target == ETarget::ClaudeCode ? TEXT(".claude/skills") : TEXT(".agents/skills");
}

const TCHAR* McpSkills::TargetClients(ETarget Target)
{
	return Target == ETarget::ClaudeCode ? TEXT("Claude Code") : TEXT("Codex, Cursor, VS Code, Antigravity CLI and Gemini CLI");
}

FString McpSkills::ProjectTargetDir(ETarget Target)
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TargetRelativeDir(Target));
}

FString McpSkills::ShippedDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAgent"));
	return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Skills")) : FString();
}

FString McpSkills::PluginVersion()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAgent"));
	return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString();
}

TArray<FString> McpSkills::ShippedNames(const FString& Dir)
{
	TArray<FString> Names;
	for (const FString& Name : McpSkillsPrivate::SubDirs(Dir))
	{
		if (FPaths::FileExists(Dir / Name / McpSkillsPrivate::SkillFile))
		{
			Names.Add(Name);
		}
	}
	return Names;
}

int32 McpSkills::FStatus::Count(ESkillState State) const
{
	return Skills.FilterByPredicate([State](const FSkill& Skill) { return Skill.State == State; }).Num();
}

bool McpSkills::FStatus::AnyInstalled() const
{
	return Count(ESkillState::Current) + Count(ESkillState::Outdated) + Retired.Num() > 0;
}

bool McpSkills::FStatus::NeedsUpdate() const
{
	return AnyInstalled() && (Count(ESkillState::Outdated) + Count(ESkillState::Missing) + Retired.Num() > 0);
}

FString McpSkills::FStatus::Summary() const
{
	if (Skills.Num() == 0)
	{
		return TEXT("This plugin ships no skills (Resources/Skills is missing).");
	}
	if (!AnyInstalled())
	{
		const int32 Foreign = Count(ESkillState::Foreign);
		return Foreign > 0
			? FString::Printf(TEXT("Not installed (%d folder(s) with our names are someone else's)."), Foreign)
			: TEXT("Not installed.");
	}
	TArray<FString> Parts;
	Parts.Add(FString::Printf(TEXT("%d of %d installed for %s"), Count(ESkillState::Current), Skills.Num(), *Version));
	if (const int32 Outdated = Count(ESkillState::Outdated))
	{
		Parts.Add(FString::Printf(TEXT("%d from another version"), Outdated));
	}
	if (Retired.Num() > 0)
	{
		Parts.Add(FString::Printf(TEXT("%d no longer shipped"), Retired.Num()));
	}
	if (const int32 Foreign = Count(ESkillState::Foreign))
	{
		Parts.Add(FString::Printf(TEXT("%d someone else's, left alone"), Foreign));
	}
	return FString::Join(Parts, TEXT(", ")) + (NeedsUpdate() ? TEXT(": update them.") : TEXT("."));
}

McpSkills::FStatus McpSkills::Inspect(const FString& InShippedDir, const FString& TargetDir, const FString& Version)
{
	using namespace McpSkillsPrivate;

	FStatus Status;
	Status.Dir = TargetDir;
	Status.Version = Version;
	const TArray<FString> Shipped = ShippedNames(InShippedDir);
	for (const FString& Name : Shipped)
	{
		FSkill Skill;
		Skill.Name = Name;
		const FString Dir = TargetDir / Name;
		if (!IFileManager::Get().DirectoryExists(*Dir))
		{
			Skill.State = ESkillState::Missing;
		}
		else if (IsOurs(Name, Dir))
		{
			Skill.InstalledVersion = ReadStamp(Dir);
			Skill.State = Skill.InstalledVersion == Version ? ESkillState::Current : ESkillState::Outdated;
		}
		else
		{
			Skill.State = ESkillState::Foreign;
		}
		Status.Skills.Add(Skill);
	}
	for (const FString& Name : SubDirs(TargetDir))
	{
		if (!Shipped.Contains(Name) && IsOurs(Name, TargetDir / Name))
		{
			Status.Retired.Add(Name);
		}
	}
	return Status;
}

McpSkills::FStatus McpSkills::InspectProject(ETarget Target)
{
	return Inspect(ShippedDir(), ProjectTargetDir(Target), PluginVersion());
}

bool McpSkills::Install(const FString& InShippedDir, const FString& TargetDir, const FString& Version, FString& OutMessage)
{
	using namespace McpSkillsPrivate;

	const FStatus Before = Inspect(InShippedDir, TargetDir, Version);
	if (Before.Skills.Num() == 0)
	{
		OutMessage = FString::Printf(TEXT("No skills to install: %s has none."), *InShippedDir);
		return false;
	}
	if (Version.IsEmpty())
	{
		OutMessage = TEXT("The plugin's version is unknown, so the copies could not be stamped; nothing was installed.");
		return false;
	}
	IFileManager::Get().MakeDirectory(*TargetDir, /*Tree*/ true);

	TArray<FString> Added, Updated, Deleted, Skipped, Failed;
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	for (const FSkill& Skill : Before.Skills)
	{
		if (Skill.State == ESkillState::Foreign)
		{
			Skipped.Add(Skill.Name);
			continue;
		}
		if (Skill.State == ESkillState::Current)
		{
			continue;
		}
		const FString Source = InShippedDir / Skill.Name;
		const FString Dest = TargetDir / Skill.Name;
		// An old copy is replaced whole: a file the new version dropped must not stay behind.
		FString Text, Stamped;
		if ((Skill.State == ESkillState::Outdated && !DeleteDir(Dest))
			|| !PlatformFile.CopyDirectoryTree(*Dest, *Source, /*bOverwriteAllExisting*/ true)
			|| !FFileHelper::LoadFileToString(Text, *(Source / SkillFile))
			|| !Stamp(Text, Version, Stamped)
			|| !FFileHelper::SaveStringToFile(Stamped, *(Dest / SkillFile), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Failed.Add(Skill.Name);
			continue;
		}
		(Skill.State == ESkillState::Outdated ? Updated : Added).Add(Skill.Name);
	}
	for (const FString& Name : Before.Retired)
	{
		(DeleteDir(TargetDir / Name) ? Deleted : Failed).Add(Name);
	}

	// The proof is re-reading the folder, not having copied.
	const FStatus After = Inspect(InShippedDir, TargetDir, Version);
	const int32 Expected = After.Skills.Num() - After.Count(ESkillState::Foreign);
	const bool bOk = Failed.Num() == 0 && After.Count(ESkillState::Current) == Expected && After.Retired.Num() == 0;

	TArray<FString> Parts;
	if (Added.Num() > 0)   { Parts.Add(FString::Printf(TEXT("installed %s"), *FString::Join(Added, TEXT(", ")))); }
	if (Updated.Num() > 0) { Parts.Add(FString::Printf(TEXT("updated %s"), *FString::Join(Updated, TEXT(", ")))); }
	if (Deleted.Num() > 0) { Parts.Add(FString::Printf(TEXT("deleted %s, no longer shipped"), *FString::Join(Deleted, TEXT(", ")))); }
	if (Parts.Num() == 0 && bOk) { Parts.Add(TEXT("every skill was already current")); }
	OutMessage = FString::Printf(TEXT("%s: %s (version %s)."), *TargetDir, *FString::Join(Parts, TEXT("; ")), *Version);
	if (Skipped.Num() > 0)
	{
		OutMessage += FString::Printf(TEXT(" Left alone, because they are not ours: %s."), *FString::Join(Skipped, TEXT(", ")));
	}
	if (!bOk)
	{
		OutMessage += Failed.Num() > 0
			? FString::Printf(TEXT(" FAILED: %s (is the folder read-only, or open in another program?)."), *FString::Join(Failed, TEXT(", ")))
			: FString::Printf(TEXT(" Re-reading the folder does not add up: %s"), *After.Summary());
	}
	return bOk;
}

bool McpSkills::Remove(const FString& TargetDir, FString& OutMessage)
{
	using namespace McpSkillsPrivate;

	TArray<FString> Deleted, Failed;
	for (const FString& Name : SubDirs(TargetDir))
	{
		if (IsOurs(Name, TargetDir / Name))
		{
			(DeleteDir(TargetDir / Name) ? Deleted : Failed).Add(Name);
		}
	}
	if (Deleted.Num() == 0 && Failed.Num() == 0)
	{
		OutMessage = FString::Printf(TEXT("%s holds no skills installed by UEAgent: nothing was deleted."), *TargetDir);
		return true;
	}
	OutMessage = FString::Printf(TEXT("%s: deleted %s."), *TargetDir, Deleted.Num() > 0 ? *FString::Join(Deleted, TEXT(", ")) : TEXT("nothing"));
	if (Failed.Num() > 0)
	{
		OutMessage += FString::Printf(TEXT(" FAILED to delete %s (read-only, or open in another program?)."), *FString::Join(Failed, TEXT(", ")));
	}
	return Failed.Num() == 0;
}

// Console: the same as the status-bar menu, for an editor without a UI or a test.
namespace McpSkillsConsole
{
	// "claude", "agents" or "all" (the default); then, optionally, a folder instead of the project's.
	static bool ParseArgs(const TArray<FString>& Args, TArray<TPair<FString, FString>>& OutTargets)
	{
		const FString Which = Args.Num() > 0 ? Args[0] : TEXT("all");
		const FString Dir = Args.Num() > 1 ? FPaths::ConvertRelativePathToFull(Args[1]) : FString();
		McpSkills::ETarget Target;
		if (Which.Equals(TEXT("all"), ESearchCase::IgnoreCase))
		{
			if (!Dir.IsEmpty())
			{
				UE_LOG(LogTemp, Error, TEXT("[MCP] A folder goes with one target: claude or agents."));
				return false;
			}
			for (const McpSkills::ETarget Each : McpSkills::AllTargets())
			{
				OutTargets.Add({ McpSkills::TargetId(Each), McpSkills::ProjectTargetDir(Each) });
			}
			return true;
		}
		if (!McpSkills::TargetFromId(Which, Target))
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] Unknown target '%s': claude, agents or all."), *Which);
			return false;
		}
		OutTargets.Add({ McpSkills::TargetId(Target), Dir.IsEmpty() ? McpSkills::ProjectTargetDir(Target) : Dir });
		return true;
	}

	static FAutoConsoleCommand StatusCommand(
		TEXT("mcp.skills.status"),
		TEXT("Are this plugin's skills installed in the project, and current? Usage: mcp.skills.status [claude|agents|all [folder]]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			TArray<TPair<FString, FString>> Targets;
			if (!ParseArgs(Args, Targets))
			{
				return;
			}
			for (const auto& Target : Targets)
			{
				const McpSkills::FStatus Status = McpSkills::Inspect(McpSkills::ShippedDir(), Target.Value, McpSkills::PluginVersion());
				UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.skills.status %s: %s (%s)"), *Target.Key, *Status.Summary(), *Status.Dir);
			}
		}));

	static FAutoConsoleCommand InstallCommand(
		TEXT("mcp.skills.install"),
		TEXT("Installs or updates this plugin's skills in the project. Usage: mcp.skills.install [claude|agents|all [folder]]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			TArray<TPair<FString, FString>> Targets;
			if (!ParseArgs(Args, Targets))
			{
				return;
			}
			for (const auto& Target : Targets)
			{
				FString Message;
				if (McpSkills::Install(McpSkills::ShippedDir(), Target.Value, McpSkills::PluginVersion(), Message))
				{
					UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.skills.install %s: %s"), *Target.Key, *Message);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("[MCP] mcp.skills.install %s: %s"), *Target.Key, *Message);
				}
			}
		}));

	static FAutoConsoleCommand RemoveCommand(
		TEXT("mcp.skills.remove"),
		TEXT("Deletes the skills this plugin installed in the project, and nothing else. Usage: mcp.skills.remove [claude|agents|all [folder]]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			TArray<TPair<FString, FString>> Targets;
			if (!ParseArgs(Args, Targets))
			{
				return;
			}
			for (const auto& Target : Targets)
			{
				FString Message;
				if (McpSkills::Remove(Target.Value, Message))
				{
					UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.skills.remove %s: %s"), *Target.Key, *Message);
				}
				else
				{
					UE_LOG(LogTemp, Error, TEXT("[MCP] mcp.skills.remove %s: %s"), *Target.Key, *Message);
				}
			}
		}));
}
