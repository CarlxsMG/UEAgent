#include "Mcp/McpClaudeCode.h"
#include "Mcp/McpJsonFile.h"

#include "UEAgentBridge.h"
#include "UEAgentSettings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

const TCHAR* const McpClaudeCode::ServerName = TEXT("unreal");
const TCHAR* const McpClaudeCode::TokenEnvVar = TEXT("UEAGENT_TOKEN");

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpClaudeCodePrivate
{
	using namespace McpJsonFile;

	// The two "type" values with which Claude Code speaks Streamable HTTP. "streamable-http" is a
	// documented alias of "http" and it works: an entry like that is NOT "fixed".
	static bool IsHttpType(const FString& Type)
	{
		return Type.Equals(TEXT("http"), ESearchCase::IgnoreCase) || Type.Equals(TEXT("streamable-http"), ESearchCase::IgnoreCase);
	}

	static FString AuthHeaderValue()
	{
		return FString::Printf(TEXT("Bearer ${%s}"), McpClaudeCode::TokenEnvVar);
	}

	static bool PointsHere(const TSharedPtr<FJsonObject>& Entry, const FString& Url)
	{
		return Entry.IsValid() && IsHttpType(StringField(Entry, TEXT("type"))) && SameUrl(StringField(Entry, TEXT("url")), Url);
	}

	// One line saying where an entry points, for the messages.
	static FString DescribeTarget(const TSharedPtr<FJsonObject>& Entry)
	{
		const FString Command = StringField(Entry, TEXT("command"));
		if (!Command.IsEmpty())
		{
			return FString::Printf(TEXT("a stdio process (%s)"), *Command);
		}
		const FString Url = StringField(Entry, TEXT("url"));
		const FString Type = StringField(Entry, TEXT("type"));
		if (Url.IsEmpty())
		{
			return TEXT("nowhere (it has no url)");
		}
		return Type.IsEmpty() ? FString::Printf(TEXT("%s (no type)"), *Url) : FString::Printf(TEXT("%s (type %s)"), *Url, *Type);
	}

	// What did Claude Code decide about the entry? Only the project settings (next to the
	// .mcp.json) are read; the user's settings (~/.claude) can approve it too, so Pending is not an
	// error. A rejection wins over any approval, whichever file it is in.
	static McpClaudeCode::EApproval ReadApproval(const FString& McpJsonPath, const FString& EntryName, FString& OutRejectedIn)
	{
		using McpClaudeCode::EApproval;
		const FString ClaudeDir = FPaths::GetPath(McpJsonPath) / TEXT(".claude");
		bool bApproved = false;
		for (const TCHAR* File : { TEXT("settings.local.json"), TEXT("settings.json") })
		{
			const FString SettingsPath = ClaudeDir / File;
			FString Text, Error;
			TSharedPtr<FJsonObject> Root;
			if (!FPaths::FileExists(SettingsPath) || !Load(SettingsPath, Text, Root, Error))
			{
				continue;
			}
			auto Names = [&Root, &EntryName](const TCHAR* Field)
			{
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				if (Root->TryGetArrayField(Field, List) && List)
				{
					for (const TSharedPtr<FJsonValue>& Item : *List)
					{
						FString Name;
						if (Item.IsValid() && Item->TryGetString(Name) && Name.Equals(EntryName, ESearchCase::CaseSensitive))
						{
							return true;
						}
					}
				}
				return false;
			};
			if (Names(TEXT("disabledMcpjsonServers")))
			{
				OutRejectedIn = FPaths::ConvertRelativePathToFull(SettingsPath);
				return EApproval::Rejected;
			}
			bool bAll = false;
			if (Names(TEXT("enabledMcpjsonServers")) || (Root->TryGetBoolField(TEXT("enableAllProjectMcpServers"), bAll) && bAll))
			{
				bApproved = true;
			}
		}
		return bApproved ? EApproval::Approved : EApproval::Pending;
	}

	// The entry that counts: the one named "unreal" if it points here; otherwise any entry that
	// already points here (no duplicate is made); otherwise "unreal" even if it points elsewhere
	// (so it can be fixed).
	static FString FindEntry(const TSharedPtr<FJsonObject>& Servers, const FString& Url, TSharedPtr<FJsonObject>& OutEntry)
	{
		const TSharedPtr<FJsonObject> Own = ObjectField(Servers, McpClaudeCode::ServerName);
		if (PointsHere(Own, Url))
		{
			OutEntry = Own;
			return McpClaudeCode::ServerName;
		}
		for (const auto& Pair : Servers->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Entry) && Entry && PointsHere(*Entry, Url))
			{
				OutEntry = *Entry;
				return FString(*Pair.Key);
			}
		}
		OutEntry = Own;
		return Own.IsValid() ? FString(McpClaudeCode::ServerName) : FString();
	}
}

FString McpClaudeCode::ServerUrl(uint16 Port)
{
	return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port);
}

FString McpClaudeCode::ProjectConfigPath()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".mcp.json"));
}

uint16 McpClaudeCode::CurrentPort()
{
	if (GEditor)
	{
		if (const UUEAgentBridge* Bridge = GEditor->GetEditorSubsystem<UUEAgentBridge>())
		{
			return Bridge->GetHttpPort();
		}
	}
	return GetDefault<UUEAgentSettings>()->GetEffectiveHttpPort();
}

bool McpClaudeCode::CurrentRequiresToken()
{
	return !GetDefault<UUEAgentSettings>()->AuthToken.IsEmpty();
}

McpClaudeCode::FStatus McpClaudeCode::InspectProject()
{
	return Inspect(ProjectConfigPath(), CurrentPort(), CurrentRequiresToken());
}

const TCHAR* McpClaudeCode::StateName(EState State)
{
	switch (State)
	{
	case EState::Connected:    return TEXT("connected");
	case EState::MissingFile:  return TEXT("no .mcp.json");
	case EState::MissingEntry: return TEXT("no entry");
	case EState::WrongTarget:  return TEXT("points elsewhere");
	case EState::MissingAuth:  return TEXT("missing token");
	case EState::Unreadable:   return TEXT("unreadable");
	}
	return TEXT("?");
}

McpClaudeCode::FStatus McpClaudeCode::Inspect(const FString& Path, uint16 Port, bool bRequiresToken)
{
	using namespace McpClaudeCodePrivate;

	FStatus Status;
	Status.Path = Path;
	const FString Url = ServerUrl(Port);

	if (!FPaths::FileExists(Path))
	{
		Status.State = EState::MissingFile;
		Status.Detail = TEXT("There is no .mcp.json in the project folder.");
		return Status;
	}
	FString Text, Error;
	TSharedPtr<FJsonObject> Root;
	if (!Load(Path, Text, Root, Error))
	{
		Status.State = EState::Unreadable;
		Status.Detail = FString::Printf(TEXT("The .mcp.json %s: left untouched."), *Error);
		return Status;
	}
	if (!Root->HasField(TEXT("mcpServers")))
	{
		Status.State = EState::MissingEntry;
		Status.Detail = TEXT("The .mcp.json declares no server (it has no \"mcpServers\").");
		return Status;
	}
	const TSharedPtr<FJsonObject> Servers = ObjectField(Root, TEXT("mcpServers"));
	if (!Servers.IsValid())
	{
		Status.State = EState::Unreadable;
		Status.Detail = TEXT("\"mcpServers\" is not an object: left untouched.");
		return Status;
	}
	if (Servers->HasField(ServerName) && !ObjectField(Servers, ServerName).IsValid())
	{
		Status.State = EState::Unreadable;
		Status.Detail = FString::Printf(TEXT("The \"%s\" entry is not an object: left untouched."), ServerName);
		return Status;
	}

	TSharedPtr<FJsonObject> Entry;
	Status.EntryName = FindEntry(Servers, Url, Entry);
	if (!Entry.IsValid())
	{
		Status.State = EState::MissingEntry;
		Status.Detail = FString::Printf(TEXT("No entry in the .mcp.json points to %s."), *Url);
		return Status;
	}
	Status.Approval = ReadApproval(Path, Status.EntryName, Status.RejectedIn);
	const FString RejectedNote = Status.Approval == EApproval::Rejected
		? FString::Printf(TEXT(" Also, Claude Code has it REJECTED in %s (disabledMcpjsonServers): remove it from there or it will not start."), *Status.RejectedIn)
		: FString();
	if (!PointsHere(Entry, Url))
	{
		Status.State = EState::WrongTarget;
		Status.Detail = FString::Printf(TEXT("The \"%s\" entry points to %s, not to %s.%s"), *Status.EntryName, *DescribeTarget(Entry), *Url, *RejectedNote);
		return Status;
	}
	if (bRequiresToken && !HasAuthorizationHeader(Entry))
	{
		Status.State = EState::MissingAuth;
		Status.Detail = FString::Printf(TEXT("The \"%s\" entry sends no Authorization header, and this server requires a token.%s"), *Status.EntryName, *RejectedNote);
		return Status;
	}
	Status.State = EState::Connected;
	switch (Status.Approval)
	{
	case EApproval::Rejected:
		Status.Detail = FString::Printf(TEXT("The \"%s\" entry of the .mcp.json points here, but Claude Code has it REJECTED in %s (disabledMcpjsonServers): remove it from there or it will not start."),
			*Status.EntryName, *Status.RejectedIn);
		break;
	case EApproval::Approved:
		Status.Detail = FString::Printf(TEXT("Claude Code connects through the \"%s\" entry of the .mcp.json, already approved."), *Status.EntryName);
		break;
	default:
		Status.Detail = FString::Printf(TEXT("Claude Code connects through the \"%s\" entry of the .mcp.json; if you never approved it, Claude Code will ask when opened in this folder."), *Status.EntryName);
		break;
	}
	return Status;
}

bool McpClaudeCode::Connect(const FString& Path, uint16 Port, bool bRequiresToken, FString& OutMessage)
{
	using namespace McpClaudeCodePrivate;

	const FString Url = ServerUrl(Port);
	const FStatus Before = Inspect(Path, Port, bRequiresToken);
	if (Before.State == EState::Connected)
	{
		OutMessage = Before.Detail;
		return true;
	}
	if (Before.State == EState::Unreadable)
	{
		OutMessage = FString::Printf(TEXT("Leaving %s untouched. %s"), *Path, *Before.Detail);
		return false;
	}

	FLayout Format;
	TSharedPtr<FJsonObject> Root;
	if (Before.State == EState::MissingFile)
	{
		Root = MakeShared<FJsonObject>();
	}
	else
	{
		FString Text, Error;
		if (!Load(Path, Text, Root, Error))
		{
			OutMessage = FString::Printf(TEXT("Leaving %s untouched: it %s."), *Path, *Error); // it changed in the meantime
			return false;
		}
		Format = DetectLayout(Text);
	}

	TSharedPtr<FJsonObject> Servers = ObjectField(Root, TEXT("mcpServers"));
	if (!Servers.IsValid())
	{
		Servers = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);
	}
	TArray<FString> KeysBefore;
	for (const auto& Pair : Servers->Values)
	{
		KeysBefore.Add(FString(*Pair.Key));
	}

	const FString Name = Before.EntryName.IsEmpty() ? FString(ServerName) : Before.EntryName;
	TSharedPtr<FJsonObject> Entry = ObjectField(Servers, *Name);
	const bool bNewEntry = !Entry.IsValid();
	if (bNewEntry)
	{
		Entry = MakeShared<FJsonObject>();
		Servers->SetObjectField(Name, Entry);
	}

	TArray<FString> Changes;
	// What only makes sense for a stdio server is surplus in an HTTP one: with both, Claude Code
	// would not know which one to use.
	TArray<FString> StdioKeys;
	for (const TCHAR* Key : { TEXT("command"), TEXT("args"), TEXT("env"), TEXT("cwd") })
	{
		if (Entry->HasField(Key))
		{
			Entry->RemoveField(Key);
			StdioKeys.Add(Key);
		}
	}
	if (StdioKeys.Num() > 0)
	{
		Changes.Add(FString::Printf(TEXT("removed %s, which were stdio-only"), *FString::Join(StdioKeys, TEXT(", "))));
	}
	const FString OldType = StringField(Entry, TEXT("type"));
	if (!IsHttpType(OldType))
	{
		Entry->SetStringField(TEXT("type"), TEXT("http"));
		Changes.Add(OldType.IsEmpty() ? FString(TEXT("type http")) : FString::Printf(TEXT("type %s -> http"), *OldType));
	}
	const FString OldUrl = StringField(Entry, TEXT("url"));
	if (!SameUrl(OldUrl, Url))
	{
		Entry->SetStringField(TEXT("url"), Url);
		Changes.Add(OldUrl.IsEmpty() ? FString::Printf(TEXT("url %s"), *Url) : FString::Printf(TEXT("url %s -> %s"), *OldUrl, *Url));
	}
	if (bRequiresToken && !HasAuthorizationHeader(Entry))
	{
		TSharedPtr<FJsonObject> Headers = ObjectField(Entry, TEXT("headers"));
		if (!Headers.IsValid())
		{
			Headers = MakeShared<FJsonObject>();
			Entry->SetObjectField(TEXT("headers"), Headers);
		}
		// The token is never written in clear text: the file travels in the repo.
		Headers->SetStringField(TEXT("Authorization"), AuthHeaderValue());
		Changes.Add(FString::Printf(TEXT("Authorization header from ${%s}"), TokenEnvVar));
	}

	// Through a temporary file: a failure halfway through cannot leave the team's .mcp.json half-written.
	if (!WriteAtomically(Path, Serialize(Root, Format), OutMessage))
	{
		return false;
	}

	// The proof is RE-READING, not having written: it must end up connected, with no entry lost.
	const FStatus After = Inspect(Path, Port, bRequiresToken);
	TArray<FString> Lost;
	{
		FString Text, Error;
		TSharedPtr<FJsonObject> Reread;
		const TSharedPtr<FJsonObject> RereadServers = Load(Path, Text, Reread, Error) ? ObjectField(Reread, TEXT("mcpServers")) : nullptr;
		for (const FString& Key : KeysBefore)
		{
			if (!RereadServers.IsValid() || !RereadServers->HasField(Key))
			{
				Lost.Add(Key);
			}
		}
	}
	if (After.State != EState::Connected || Lost.Num() > 0)
	{
		OutMessage = FString::Printf(TEXT("Wrote %s, but re-reading it does not add up: %s%s"), *Path, *After.Detail,
			Lost.Num() > 0 ? *FString::Printf(TEXT(" Entries that were there are missing: %s."), *FString::Join(Lost, TEXT(", "))) : TEXT(""));
		return false;
	}

	const FString ChangesText = FString::Join(Changes, TEXT(", "));
	OutMessage = Before.State == EState::MissingFile
		? FString::Printf(TEXT("Created %s with the \"%s\" entry (%s)."), *Path, *Name, *ChangesText)
		: bNewEntry
			? FString::Printf(TEXT("Added the \"%s\" entry to %s (%s)."), *Name, *Path, *ChangesText)
			: FString::Printf(TEXT("Fixed the \"%s\" entry of %s (%s)."), *Name, *Path, *ChangesText);
	if (After.Approval == EApproval::Pending)
	{
		OutMessage += FString::Printf(TEXT(" When you open Claude Code in this folder, approve the \"%s\" server."), *Name);
	}
	else if (After.Approval == EApproval::Rejected)
	{
		OutMessage += FString::Printf(TEXT(" WARNING: Claude Code has it REJECTED in %s (disabledMcpjsonServers); remove it from there or it will not start."), *After.RejectedIn);
	}
	return true;
}

FString McpClaudeCode::AddCommand(uint16 Port, bool bRequiresToken)
{
	FString Command = FString::Printf(TEXT("claude mcp add --transport http %s %s"), ServerName, *ServerUrl(Port));
	if (bRequiresToken)
	{
		// Single quotes: neither bash nor PowerShell expands inside them, so it reaches Claude Code as is.
		Command += FString::Printf(TEXT(" --header 'Authorization: Bearer ${%s}'"), TokenEnvVar);
	}
	return Command;
}

// Console: the same as the status-bar menu, for whoever does not see it (or for an editor without
// a UI). With no argument, the project's .mcp.json; with one, that path.
namespace McpClaudeCodeConsole
{
	static FString PathFromArgs(const TArray<FString>& Args)
	{
		return Args.Num() > 0
			? FPaths::ConvertRelativePathToFull(FString::Join(Args, TEXT(" ")))
			: McpClaudeCode::ProjectConfigPath();
	}

	static FAutoConsoleCommand StatusCommand(
		TEXT("mcp.claude.status"),
		TEXT("Does the project's .mcp.json point to this MCP server? Usage: mcp.claude.status [path to .mcp.json]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const McpClaudeCode::FStatus Status = McpClaudeCode::Inspect(PathFromArgs(Args), McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken());
			UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.claude.status: %s -- %s (%s)"), McpClaudeCode::StateName(Status.State), *Status.Detail, *Status.Path);
		}));

	static FAutoConsoleCommand ConnectCommand(
		TEXT("mcp.claude.connect"),
		TEXT("Writes or fixes the \"unreal\" entry of the .mcp.json without touching the others. Usage: mcp.claude.connect [path to .mcp.json]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FString Message;
			const bool bOk = McpClaudeCode::Connect(PathFromArgs(Args), McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken(), Message);
			if (bOk)
			{
				UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.claude.connect: %s"), *Message);
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("[MCP] mcp.claude.connect: %s"), *Message);
			}
		}));
}
