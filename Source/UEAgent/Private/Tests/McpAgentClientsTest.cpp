// Copyright (c) 2026 CarlxsMG.

// Test of the other clients' files (Mcp/McpAgentClients.h) on temporary files: it touches neither the
// project's files nor the server. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.AgentClients" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// As with the .mcp.json, what is tested above all is what must NOT change: the other entries, the
// other fields of the file, the indentation and the line endings.

#include "Mcp/McpAgentClients.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpAgentClientsTestPrivate
{
	static FString CaseDir(const TCHAR* Name)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpAgentClients") / Name);
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}

	static void WriteText(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static FString ReadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpAgentClientsTest, "UEAgent.AgentClients",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpAgentClientsTest::RunTest(const FString& Parameters)
{
	using namespace McpAgentClientsTestPrivate;
	using McpAgentClients::EClient;
	using McpAgentClients::EState;

	const uint16 Port = 55561;
	FString Message;
	auto StateIs = [this, Port](EClient Client, const FString& Path, bool bToken, EState Expected, const TCHAR* What)
	{
		const McpAgentClients::FStatus Status = McpAgentClients::Inspect(Client, Path, Port, bToken);
		TestTrue(FString::Printf(TEXT("%s: state %s (expected %s) -- %s"), What, McpAgentClients::StateName(Status.State),
			McpAgentClients::StateName(Expected), *Status.Detail), Status.State == Expected);
		return Status;
	};

	// 1. Cursor, no file and no folder: both are created, in two spaces and LF, with the server's URL (the core list);
	//    a second Connect does not write again.
	{
		const FString Path = CaseDir(TEXT("CursorNew")) / TEXT(".cursor/mcp.json");
		StateIs(EClient::Cursor, Path, false, EState::MissingFile, TEXT("1 before"));
		TestTrue(TEXT("1 connects"), McpAgentClients::Connect(EClient::Cursor, Path, Port, false, Message));
		AddInfo(Message);
		TestEqual(TEXT("1 content"), ReadText(Path), FString(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n")));
		StateIs(EClient::Cursor, Path, false, EState::Connected, TEXT("1 after"));
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		TestTrue(TEXT("1 idempotent"), McpAgentClients::Connect(EClient::Cursor, Path, Port, false, Message));
		TestTrue(TEXT("1 does not rewrite"), IFileManager::Get().GetTimeStamp(*Path) == Stamp);
	}

	// 2. VS Code, a file with another server, a setting and tabs with CRLF: "servers" gains the entry
	//    with "type": "http", and everything else comes out identical.
	{
		const FString Path = CaseDir(TEXT("VSCodeOther")) / TEXT("mcp.json");
		const FString Other = TEXT("\t\t\"github\": {\r\n\t\t\t\"type\": \"http\",\r\n\t\t\t\"url\": \"https://api.githubcopilot.com/mcp/\"\r\n\t\t}");
		WriteText(Path, FString(TEXT("{\r\n\t\"servers\": {\r\n")) + Other + TEXT("\r\n\t},\r\n\t\"note\": 1.50\r\n}\r\n"));
		StateIs(EClient::VSCode, Path, false, EState::MissingEntry, TEXT("2 before"));
		TestTrue(TEXT("2 connects"), McpAgentClients::Connect(EClient::VSCode, Path, Port, false, Message));
		AddInfo(Message);
		TestEqual(TEXT("2 content"), ReadText(Path),
			FString(TEXT("{\r\n\t\"servers\": {\r\n")) + Other
			+ TEXT(",\r\n\t\t\"unreal\": {\r\n\t\t\t\"type\": \"http\",\r\n\t\t\t\"url\": \"http://127.0.0.1:55561/mcp\"\r\n\t\t}\r\n\t},\r\n\t\"note\": 1.50\r\n}\r\n"));
	}

	// 3. VS Code with a token: the header names an input, and the input is declared once.
	{
		const FString Path = CaseDir(TEXT("VSCodeToken")) / TEXT("mcp.json");
		TestTrue(TEXT("3 connects"), McpAgentClients::Connect(EClient::VSCode, Path, Port, true, Message));
		const FString Text = ReadText(Path);
		TestTrue(TEXT("3 header from the input"), Text.Contains(TEXT("\"Authorization\": \"Bearer ${input:ueagent-token}\"")));
		TestTrue(TEXT("3 input declared"), Text.Contains(TEXT("\"id\": \"ueagent-token\"")) && Text.Contains(TEXT("\"password\": true")));
		StateIs(EClient::VSCode, Path, true, EState::Connected, TEXT("3 after"));
	}

	// 4. Cursor with a token: the header reads an environment variable, never the token itself.
	{
		const FString Path = CaseDir(TEXT("CursorToken")) / TEXT("mcp.json");
		TestTrue(TEXT("4 connects"), McpAgentClients::Connect(EClient::Cursor, Path, Port, true, Message));
		TestTrue(TEXT("4 header from the env"), ReadText(Path).Contains(TEXT("\"Authorization\": \"Bearer ${env:UEAGENT_TOKEN}\"")));
	}

	// 5. Antigravity: "serverUrl"; and with a token, no Connect at all (no documented header), the file untouched.
	{
		const FString Path = CaseDir(TEXT("Antigravity")) / TEXT("mcp_config.json");
		TestTrue(TEXT("5 connects"), McpAgentClients::Connect(EClient::Antigravity, Path, Port, false, Message));
		TestEqual(TEXT("5 content"), ReadText(Path), FString(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"serverUrl\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n")));
		const FString Before = ReadText(Path);
		StateIs(EClient::Antigravity, Path, true, EState::AuthUnsupported, TEXT("5 token"));
		TestFalse(TEXT("5 refuses with a token"), McpAgentClients::Connect(EClient::Antigravity, Path, Port, true, Message));
		TestEqual(TEXT("5 untouched"), ReadText(Path), Before);
	}

	// 6. An old stdio entry under "unreal": it is fixed (stdio fields out, url in), its own fields kept.
	//    The url takes the slot the removed "command" leaves (FJsonObject's map reuses it).
	{
		const FString Path = CaseDir(TEXT("CursorStdio")) / TEXT("mcp.json");
		WriteText(Path, TEXT("{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"command\": \"python\",\n      \"args\": [\n        \"server.py\"\n      ],\n      \"disabled\": false\n    }\n  }\n}\n"));
		StateIs(EClient::Cursor, Path, false, EState::WrongTarget, TEXT("6 before"));
		TestTrue(TEXT("6 connects"), McpAgentClients::Connect(EClient::Cursor, Path, Port, false, Message));
		AddInfo(Message);
		TestEqual(TEXT("6 content"), ReadText(Path), FString(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"url\": \"http://127.0.0.1:55561/mcp\",\n      \"disabled\": false\n    }\n  }\n}\n")));
	}

	// 7. Invalid JSON: unreadable, and left untouched.
	{
		const FString Path = CaseDir(TEXT("Broken")) / TEXT("mcp.json");
		const FString Text = TEXT("{ \"mcpServers\": { \"unreal\": ");
		WriteText(Path, Text);
		StateIs(EClient::Cursor, Path, false, EState::Unreadable, TEXT("7"));
		TestFalse(TEXT("7 refuses"), McpAgentClients::Connect(EClient::Cursor, Path, Port, false, Message));
		TestEqual(TEXT("7 untouched"), ReadText(Path), Text);
	}

	// 8. Another entry already points here: it counts, and no duplicate is added.
	{
		const FString Path = CaseDir(TEXT("OtherName")) / TEXT("mcp.json");
		const FString Text = TEXT("{\n  \"mcpServers\": {\n    \"ue\": {\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n");
		WriteText(Path, Text);
		const McpAgentClients::FStatus Status = StateIs(EClient::Cursor, Path, false, EState::Connected, TEXT("8"));
		TestEqual(TEXT("8 entry"), Status.EntryName, FString(TEXT("ue")));
		TestTrue(TEXT("8 connects"), McpAgentClients::Connect(EClient::Cursor, Path, Port, false, Message));
		TestEqual(TEXT("8 untouched"), ReadText(Path), Text);
	}

	// 9. The Codex command, with and without a token.
	TestEqual(TEXT("9 codex"), McpAgentClients::CodexAddCommand(Port, false), FString(TEXT("codex mcp add unreal --url \"http://127.0.0.1:55561/mcp\"")));
	TestTrue(TEXT("9 codex token"), McpAgentClients::CodexAddCommand(Port, true).EndsWith(TEXT(" --bearer-token-env-var UEAGENT_TOKEN")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
