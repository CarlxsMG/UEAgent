// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpAgentClients.h"

#include "Mcp/McpClaudeCode.h"
#include "Mcp/McpJsonFile.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpAgentClientsPrivate
{
	using namespace McpJsonFile;
	using McpAgentClients::EClient;
	using McpAgentClients::EState;

	// How each client writes a remote server. From the clients' documentation (2026-10-20):
	// cursor.com/docs/context/mcp, code.visualstudio.com/docs/agents/reference/mcp-configuration,
	// antigravity.google/docs/cli/gcli-migration.
	struct FSpec
	{
		const TCHAR* ServersKey;   // the object that holds the servers
		const TCHAR* UrlKey;       // the field with the server's URL
		const TCHAR* RequiredType; // the "type" the entry needs, or nullptr
		bool bTokenSupported;      // whether the file has a documented way to send the Authorization header
	};

	static FSpec SpecOf(EClient Client)
	{
		switch (Client)
		{
		case EClient::VSCode:      return { TEXT("servers"), TEXT("url"), TEXT("http"), true };
		case EClient::Antigravity: return { TEXT("mcpServers"), TEXT("serverUrl"), nullptr, false };
		default:                   return { TEXT("mcpServers"), TEXT("url"), nullptr, true };
		}
	}

	// VS Code asks for the token once, through an input, and keeps it in its secret storage.
	static const TCHAR* const VSCodeTokenInput = TEXT("ueagent-token");

	static FString AuthHeaderValue(EClient Client)
	{
		return Client == EClient::VSCode
			? FString::Printf(TEXT("Bearer ${input:%s}"), VSCodeTokenInput)
			: FString::Printf(TEXT("Bearer ${env:%s}"), McpClaudeCode::TokenEnvVar);
	}

	static bool PointsHere(EClient Client, const TSharedPtr<FJsonObject>& Entry, const FString& Url)
	{
		const FSpec Spec = SpecOf(Client);
		if (!Entry.IsValid() || !SameUrl(StringField(Entry, Spec.UrlKey), Url))
		{
			return false;
		}
		return !Spec.RequiredType || StringField(Entry, TEXT("type")).Equals(Spec.RequiredType, ESearchCase::IgnoreCase);
	}

	static FString DescribeTarget(EClient Client, const TSharedPtr<FJsonObject>& Entry)
	{
		const FString Command = StringField(Entry, TEXT("command"));
		if (!Command.IsEmpty())
		{
			return FString::Printf(TEXT("a stdio process (%s)"), *Command);
		}
		const FString Url = StringField(Entry, SpecOf(Client).UrlKey);
		return Url.IsEmpty() ? FString::Printf(TEXT("nowhere (it has no \"%s\")"), SpecOf(Client).UrlKey) : Url;
	}

	// The entry that counts: "unreal" if it points here; otherwise any entry that already points here
	// (no duplicate is made); otherwise "unreal" even if it points elsewhere (so it can be fixed).
	static FString FindEntry(EClient Client, const TSharedPtr<FJsonObject>& Servers, const FString& Url, TSharedPtr<FJsonObject>& OutEntry)
	{
		const TSharedPtr<FJsonObject> Own = ObjectField(Servers, McpClaudeCode::ServerName);
		if (PointsHere(Client, Own, Url))
		{
			OutEntry = Own;
			return McpClaudeCode::ServerName;
		}
		for (const auto& Pair : Servers->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Entry) && Entry && PointsHere(Client, *Entry, Url))
			{
				OutEntry = *Entry;
				return FString(*Pair.Key);
			}
		}
		OutEntry = Own;
		return Own.IsValid() ? FString(McpClaudeCode::ServerName) : FString();
	}

	// VS Code: the input the header names must be declared in the file's "inputs".
	static bool HasTokenInput(const TSharedPtr<FJsonObject>& Root)
	{
		const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(TEXT("inputs"), Inputs) && Inputs)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Inputs)
			{
				const TSharedPtr<FJsonObject>* Input = nullptr;
				if (Item.IsValid() && Item->TryGetObject(Input) && Input && StringField(*Input, TEXT("id")) == VSCodeTokenInput)
				{
					return true;
				}
			}
		}
		return false;
	}
}

TArray<McpAgentClients::EClient> McpAgentClients::All()
{
	return { EClient::Cursor, EClient::VSCode, EClient::Antigravity };
}

const TCHAR* McpAgentClients::DisplayName(EClient Client)
{
	switch (Client)
	{
	case EClient::VSCode:      return TEXT("VS Code");
	case EClient::Antigravity: return TEXT("Antigravity CLI");
	default:                   return TEXT("Cursor");
	}
}

const TCHAR* McpAgentClients::Id(EClient Client)
{
	switch (Client)
	{
	case EClient::VSCode:      return TEXT("vscode");
	case EClient::Antigravity: return TEXT("antigravity");
	default:                   return TEXT("cursor");
	}
}

bool McpAgentClients::FromId(const FString& InId, EClient& OutClient)
{
	for (const EClient Client : All())
	{
		if (InId.Equals(Id(Client), ESearchCase::IgnoreCase))
		{
			OutClient = Client;
			return true;
		}
	}
	return false;
}

const TCHAR* McpAgentClients::RelativePath(EClient Client)
{
	switch (Client)
	{
	case EClient::VSCode:      return TEXT(".vscode/mcp.json");
	case EClient::Antigravity: return TEXT(".agents/mcp_config.json");
	default:                   return TEXT(".cursor/mcp.json");
	}
}

FString McpAgentClients::ProjectConfigPath(EClient Client)
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / RelativePath(Client));
}

const TCHAR* McpAgentClients::StateName(EState State)
{
	switch (State)
	{
	case EState::Connected:       return TEXT("connected");
	case EState::MissingFile:     return TEXT("no file");
	case EState::MissingEntry:    return TEXT("no entry");
	case EState::WrongTarget:     return TEXT("points elsewhere");
	case EState::MissingAuth:     return TEXT("missing token");
	case EState::AuthUnsupported: return TEXT("token not supported");
	case EState::Unreadable:      return TEXT("unreadable");
	}
	return TEXT("?");
}

FString McpAgentClients::ClientUrl(uint16 Port)
{
	return McpClaudeCode::ServerUrl(Port) + TEXT("?tools=compact");
}

McpAgentClients::FStatus McpAgentClients::InspectProject(EClient Client)
{
	return Inspect(Client, ProjectConfigPath(Client), McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken());
}

McpAgentClients::FStatus McpAgentClients::Inspect(EClient Client, const FString& Path, uint16 Port, bool bRequiresToken)
{
	using namespace McpAgentClientsPrivate;

	FStatus Status;
	Status.Client = Client;
	Status.Path = Path;
	const FSpec Spec = SpecOf(Client);
	const FString Url = ClientUrl(Port);
	const FString File = FPaths::GetCleanFilename(Path);

	if (!FPaths::FileExists(Path))
	{
		Status.State = bRequiresToken && !Spec.bTokenSupported ? EState::AuthUnsupported : EState::MissingFile;
		Status.Detail = Status.State == EState::AuthUnsupported
			? FString::Printf(TEXT("%s documents no way to send the auth token this server requires, so Connect cannot set it up; see the docs (Connecting a client)."), DisplayName(Client))
			: FString::Printf(TEXT("There is no %s in the project."), RelativePath(Client));
		return Status;
	}
	FString Text, Error;
	TSharedPtr<FJsonObject> Root;
	if (!Load(Path, Text, Root, Error))
	{
		Status.State = EState::Unreadable;
		Status.Detail = FString::Printf(TEXT("The %s %s: left untouched."), RelativePath(Client), *Error);
		return Status;
	}
	const TSharedPtr<FJsonObject> Servers = ObjectField(Root, Spec.ServersKey);
	if (Root->HasField(Spec.ServersKey) && !Servers.IsValid())
	{
		Status.State = EState::Unreadable;
		Status.Detail = FString::Printf(TEXT("\"%s\" in %s is not an object: left untouched."), Spec.ServersKey, *File);
		return Status;
	}
	if (Servers.IsValid() && Servers->HasField(McpClaudeCode::ServerName) && !ObjectField(Servers, McpClaudeCode::ServerName).IsValid())
	{
		Status.State = EState::Unreadable;
		Status.Detail = FString::Printf(TEXT("The \"%s\" entry of %s is not an object: left untouched."), McpClaudeCode::ServerName, *File);
		return Status;
	}

	TSharedPtr<FJsonObject> Entry;
	if (Servers.IsValid())
	{
		Status.EntryName = FindEntry(Client, Servers, Url, Entry);
	}
	if (!Entry.IsValid())
	{
		Status.State = bRequiresToken && !Spec.bTokenSupported ? EState::AuthUnsupported : EState::MissingEntry;
		Status.Detail = Status.State == EState::AuthUnsupported
			? FString::Printf(TEXT("%s documents no way to send the auth token this server requires, so Connect cannot set it up; see the docs (Connecting a client)."), DisplayName(Client))
			: FString::Printf(TEXT("No entry in %s points to %s."), RelativePath(Client), *McpClaudeCode::ServerUrl(Port));
		return Status;
	}
	if (!PointsHere(Client, Entry, Url))
	{
		Status.State = EState::WrongTarget;
		Status.Detail = FString::Printf(TEXT("The \"%s\" entry of %s points to %s, not to this editor."), *Status.EntryName, RelativePath(Client), *DescribeTarget(Client, Entry));
		return Status;
	}
	if (bRequiresToken && !HasAuthorizationHeader(Entry))
	{
		Status.State = Spec.bTokenSupported ? EState::MissingAuth : EState::AuthUnsupported;
		Status.Detail = Spec.bTokenSupported
			? FString::Printf(TEXT("The \"%s\" entry of %s sends no Authorization header, and this server requires a token."), *Status.EntryName, RelativePath(Client))
			: FString::Printf(TEXT("The \"%s\" entry of %s points here, but this server requires a token and %s documents no way to send it."), *Status.EntryName, RelativePath(Client), DisplayName(Client));
		return Status;
	}
	Status.State = EState::Connected;
	Status.Detail = FString::Printf(TEXT("%s connects through the \"%s\" entry of %s."), DisplayName(Client), *Status.EntryName, RelativePath(Client));
	return Status;
}

bool McpAgentClients::Connect(EClient Client, const FString& Path, uint16 Port, bool bRequiresToken, FString& OutMessage)
{
	using namespace McpAgentClientsPrivate;

	const FSpec Spec = SpecOf(Client);
	const FString Url = ClientUrl(Port);
	const FStatus Before = Inspect(Client, Path, Port, bRequiresToken);
	if (Before.State == EState::Connected)
	{
		OutMessage = Before.Detail;
		return true;
	}
	if (Before.State == EState::Unreadable || Before.State == EState::AuthUnsupported)
	{
		OutMessage = FString::Printf(TEXT("Leaving %s untouched. %s"), *Path, *Before.Detail);
		return false;
	}

	FLayout Layout;
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
		Layout = DetectLayout(Text);
	}

	TSharedPtr<FJsonObject> Servers = ObjectField(Root, Spec.ServersKey);
	if (!Servers.IsValid())
	{
		Servers = MakeShared<FJsonObject>();
		Root->SetObjectField(Spec.ServersKey, Servers);
	}
	TArray<FString> KeysBefore;
	for (const auto& Pair : Servers->Values)
	{
		KeysBefore.Add(FString(*Pair.Key));
	}

	const FString Name = Before.EntryName.IsEmpty() ? FString(McpClaudeCode::ServerName) : Before.EntryName;
	TSharedPtr<FJsonObject> Entry = ObjectField(Servers, *Name);
	const bool bNewEntry = !Entry.IsValid();
	if (bNewEntry)
	{
		Entry = MakeShared<FJsonObject>();
		Servers->SetObjectField(Name, Entry);
	}

	TArray<FString> Changes;
	// What only makes sense for a stdio server is surplus in a remote one: with both, the client
	// would not know which one to use.
	TArray<FString> StdioKeys;
	for (const TCHAR* Key : { TEXT("command"), TEXT("args"), TEXT("env"), TEXT("cwd"), TEXT("envFile") })
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
	if (Spec.RequiredType)
	{
		const FString OldType = StringField(Entry, TEXT("type"));
		if (!OldType.Equals(Spec.RequiredType, ESearchCase::IgnoreCase))
		{
			Entry->SetStringField(TEXT("type"), Spec.RequiredType);
			Changes.Add(OldType.IsEmpty() ? FString::Printf(TEXT("type %s"), Spec.RequiredType) : FString::Printf(TEXT("type %s -> %s"), *OldType, Spec.RequiredType));
		}
	}
	const FString OldUrl = StringField(Entry, Spec.UrlKey);
	if (!SameUrl(OldUrl, Url))
	{
		Entry->SetStringField(Spec.UrlKey, Url);
		Changes.Add(OldUrl.IsEmpty() ? FString::Printf(TEXT("%s %s"), Spec.UrlKey, *Url) : FString::Printf(TEXT("%s %s -> %s"), Spec.UrlKey, *OldUrl, *Url));
	}
	if (bRequiresToken && !HasAuthorizationHeader(Entry))
	{
		TSharedPtr<FJsonObject> Headers = ObjectField(Entry, TEXT("headers"));
		if (!Headers.IsValid())
		{
			Headers = MakeShared<FJsonObject>();
			Entry->SetObjectField(TEXT("headers"), Headers);
		}
		// The token is never written in clear text: the file may travel in the repo.
		Headers->SetStringField(TEXT("Authorization"), AuthHeaderValue(Client));
		Changes.Add(Client == EClient::VSCode
			? FString::Printf(TEXT("Authorization header from the \"%s\" input"), VSCodeTokenInput)
			: FString::Printf(TEXT("Authorization header from ${env:%s}"), McpClaudeCode::TokenEnvVar));
		if (Client == EClient::VSCode && !HasTokenInput(Root))
		{
			TArray<TSharedPtr<FJsonValue>> Inputs;
			const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
			if (Root->TryGetArrayField(TEXT("inputs"), Existing) && Existing)
			{
				Inputs = *Existing;
			}
			const TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
			Input->SetStringField(TEXT("type"), TEXT("promptString"));
			Input->SetStringField(TEXT("id"), VSCodeTokenInput);
			Input->SetStringField(TEXT("description"), TEXT("UEAgent auth token (Editor Preferences > Plugins > UEAgent)"));
			Input->SetBoolField(TEXT("password"), true);
			Inputs.Add(MakeShared<FJsonValueObject>(Input));
			Root->SetArrayField(TEXT("inputs"), Inputs);
		}
	}

	// The client's folder (.cursor, .vscode, .agents) may not exist yet.
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	if (!WriteAtomically(Path, Serialize(Root, Layout), OutMessage))
	{
		return false;
	}

	// The proof is RE-READING, not having written: it must end up connected, with no entry lost.
	const FStatus After = Inspect(Client, Path, Port, bRequiresToken);
	TArray<FString> Lost;
	{
		FString Text, Error;
		TSharedPtr<FJsonObject> Reread;
		const TSharedPtr<FJsonObject> RereadServers = Load(Path, Text, Reread, Error) ? ObjectField(Reread, Spec.ServersKey) : nullptr;
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
	return true;
}

FString McpAgentClients::CodexAddCommand(uint16 Port, bool bRequiresToken)
{
	FString Command = FString::Printf(TEXT("codex mcp add %s --url \"%s\""), McpClaudeCode::ServerName, *ClientUrl(Port));
	if (bRequiresToken)
	{
		Command += FString::Printf(TEXT(" --bearer-token-env-var %s"), McpClaudeCode::TokenEnvVar);
	}
	return Command;
}

// Console: the same as the status-bar menu, for an editor without a UI or a test.
namespace McpAgentClientsConsole
{
	// "<client> [path]": with no path, the client's file in the project.
	static bool ParseArgs(const TArray<FString>& Args, McpAgentClients::EClient& OutClient, FString& OutPath)
	{
		if (Args.Num() == 0 || !McpAgentClients::FromId(Args[0], OutClient))
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] Name the client first: cursor, vscode or antigravity."));
			return false;
		}
		TArray<FString> Rest(Args);
		Rest.RemoveAt(0);
		OutPath = Rest.Num() > 0 ? FPaths::ConvertRelativePathToFull(FString::Join(Rest, TEXT(" "))) : McpAgentClients::ProjectConfigPath(OutClient);
		return true;
	}

	static FAutoConsoleCommand StatusCommand(
		TEXT("mcp.clients.status"),
		TEXT("Does each client's file in the project point to this MCP server? Usage: mcp.clients.status [cursor|vscode|antigravity [path]]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			TArray<TPair<McpAgentClients::EClient, FString>> Targets;
			if (Args.Num() == 0)
			{
				for (const McpAgentClients::EClient Client : McpAgentClients::All())
				{
					Targets.Add({ Client, McpAgentClients::ProjectConfigPath(Client) });
				}
			}
			else
			{
				McpAgentClients::EClient Client;
				FString Path;
				if (!ParseArgs(Args, Client, Path))
				{
					return;
				}
				Targets.Add({ Client, Path });
			}
			for (const auto& Target : Targets)
			{
				const McpAgentClients::FStatus Status = McpAgentClients::Inspect(Target.Key, Target.Value, McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken());
				UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.clients.status %s: %s -- %s (%s)"), McpAgentClients::Id(Target.Key), McpAgentClients::StateName(Status.State), *Status.Detail, *Status.Path);
			}
		}));

	static FAutoConsoleCommand ConnectCommand(
		TEXT("mcp.clients.connect"),
		TEXT("Writes or fixes the \"unreal\" entry of a client's file without touching the others. Usage: mcp.clients.connect cursor|vscode|antigravity [path]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			McpAgentClients::EClient Client;
			FString Path;
			if (!ParseArgs(Args, Client, Path))
			{
				return;
			}
			FString Message;
			if (McpAgentClients::Connect(Client, Path, McpClaudeCode::CurrentPort(), McpClaudeCode::CurrentRequiresToken(), Message))
			{
				UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.clients.connect %s: %s"), McpAgentClients::Id(Client), *Message);
			}
			else
			{
				UE_LOG(LogTemp, Error, TEXT("[MCP] mcp.clients.connect %s: %s"), McpAgentClients::Id(Client), *Message);
			}
		}));
}
