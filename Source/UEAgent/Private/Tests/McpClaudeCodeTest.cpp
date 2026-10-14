// Test of the .mcp.json writer (Mcp/McpClaudeCode.h) on temporary files: it touches neither the
// project's .mcp.json nor the server. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.ClaudeCode" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// The file belongs to the team, so what is tested above all is what must NOT change: the other
// entries, their fields, the text of the numbers, the indentation and the line endings.

#include "Mcp/McpClaudeCode.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpClaudeCodeTestPrivate
{
	static FString RootDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpClaudeCode"));
	}

	static FString CaseDir(const TCHAR* Name)
	{
		const FString Dir = RootDir() / Name;
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

	static TSharedPtr<FJsonObject> ReadEntry(const FString& Path, const TCHAR* Name)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Servers = nullptr;
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReadText(Path)), Root) && Root.IsValid()
			&& Root->TryGetObjectField(TEXT("mcpServers"), Servers) && Servers
			&& (*Servers)->TryGetObjectField(Name, Entry) && Entry)
		{
			return *Entry;
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpClaudeCodeTest, "UEAgent.ClaudeCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpClaudeCodeTest::RunTest(const FString& Parameters)
{
	using namespace McpClaudeCodeTestPrivate;
	using McpClaudeCode::EApproval;
	using McpClaudeCode::EState;

	const uint16 Port = 55561;
	FString Message;
	auto StateIs = [this, Port](const FString& Path, bool bToken, EState Expected, const TCHAR* What)
	{
		const McpClaudeCode::FStatus Status = McpClaudeCode::Inspect(Path, Port, bToken);
		TestTrue(FString::Printf(TEXT("%s: state %s (expected %s) -- %s"), What, McpClaudeCode::StateName(Status.State),
			McpClaudeCode::StateName(Expected), *Status.Detail), Status.State == Expected);
		return Status;
	};

	// 1. No file: it is created in the `claude mcp add --scope project` format (two spaces, LF),
	//    and a second Connect does not write again.
	{
		const FString Path = CaseDir(TEXT("MissingFile")) / TEXT(".mcp.json");
		StateIs(Path, false, EState::MissingFile, TEXT("1 before"));
		TestTrue(TEXT("1 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		AddInfo(Message);
		TestEqual(TEXT("1 content"), ReadText(Path), FString(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n")));
		StateIs(Path, false, EState::Connected, TEXT("1 after"));
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		TestTrue(TEXT("1 idempotent"), McpClaudeCode::Connect(Path, Port, false, Message));
		TestTrue(TEXT("1 does not rewrite"), IFileManager::Get().GetTimeStamp(*Path) == Stamp);
	}

	// 2. Another server, tabs and CRLF, numbers with their text (1.50), an empty object and text with
	//    quotes and a non-ASCII character: everything comes out identical and the new entry is appended.
	{
		const FString Path = CaseDir(TEXT("OtherServer")) / TEXT(".mcp.json");
		const FString Other = TEXT(
			"\t\t\"other\": {\r\n\t\t\t\"type\": \"stdio\",\r\n\t\t\t\"command\": \"npx\",\r\n\t\t\t\"args\": [\r\n\t\t\t\t\"-y\",\r\n\t\t\t\t\"tool@1.0\"\r\n\t\t\t],\r\n"
			"\t\t\t\"timeout\": 1.50,\r\n\t\t\t\"retries\": 3,\r\n\t\t\t\"env\": {}\r\n\t\t}");
		WriteText(Path, FString(TEXT("{\r\n\t\"mcpServers\": {\r\n")) + Other + TEXT("\r\n\t},\r\n\t\"note\": \"a \\\"quoted\\\" \u00F1\"\r\n}\r\n"));
		StateIs(Path, false, EState::MissingEntry, TEXT("2 before"));
		TestTrue(TEXT("2 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		AddInfo(Message);
		TestEqual(TEXT("2 content"), ReadText(Path),
			FString(TEXT("{\r\n\t\"mcpServers\": {\r\n")) + Other
			+ TEXT(",\r\n\t\t\"unreal\": {\r\n\t\t\t\"type\": \"http\",\r\n\t\t\t\"url\": \"http://127.0.0.1:55561/mcp\"\r\n\t\t}\r\n\t},\r\n\t\"note\": \"a \\\"quoted\\\" \u00F1\"\r\n}\r\n"));
	}

	// 3. "streamable-http" (a documented alias of "http") plus a custom field: it is connected
	//    and left untouched.
	{
		const FString Path = CaseDir(TEXT("AlreadyConnected")) / TEXT(".mcp.json");
		const FString Text = TEXT("{\r\n\t\"mcpServers\": {\r\n\t\t\"unreal\": {\r\n\t\t\t\"type\": \"streamable-http\",\r\n\t\t\t\"url\": \"http://127.0.0.1:55561/mcp\",\r\n\t\t\t\"description\": \"x\"\r\n\t\t}\r\n\t}\r\n}\r\n");
		WriteText(Path, Text);
		const McpClaudeCode::FStatus Status = StateIs(Path, false, EState::Connected, TEXT("3"));
		TestEqual(TEXT("3 entry"), Status.EntryName, FString(TEXT("unreal")));
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		TestTrue(TEXT("3 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		TestEqual(TEXT("3 untouched"), ReadText(Path), Text);
		TestTrue(TEXT("3 does not rewrite"), IFileManager::Get().GetTimeStamp(*Path) == Stamp);
	}

	// 3b. A URL that asks for the compact tool list points at the same server: connected, and
	//     Connect keeps the query.
	{
		const FString Path = CaseDir(TEXT("CompactList")) / TEXT(".mcp.json");
		const FString Text = TEXT("{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp?tools=compact\"\n    }\n  }\n}\n");
		WriteText(Path, Text);
		StateIs(Path, false, EState::Connected, TEXT("3b"));
		TestTrue(TEXT("3b connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		TestEqual(TEXT("3b untouched"), ReadText(Path), Text);
	}

	// 4. An old stdio entry: it becomes HTTP, loses the stdio fields and keeps its own fields.
	{
		const FString Path = CaseDir(TEXT("OldStdio")) / TEXT(".mcp.json");
		WriteText(Path, TEXT("{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"stdio\",\n      \"command\": \"python\",\n      \"args\": [\n        \"server.py\"\n      ],\n"
			"      \"env\": {\n        \"A\": \"1\"\n      },\n      \"description\": \"keep me\"\n    }\n  }\n}\n"));
		StateIs(Path, false, EState::WrongTarget, TEXT("4 before"));
		TestTrue(TEXT("4 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		AddInfo(Message);
		StateIs(Path, false, EState::Connected, TEXT("4 after"));
		const TSharedPtr<FJsonObject> Entry = ReadEntry(Path, TEXT("unreal"));
		if (TestTrue(TEXT("4 entry"), Entry.IsValid()))
		{
			TestEqual(TEXT("4 type"), Entry->GetStringField(TEXT("type")), FString(TEXT("http")));
			TestEqual(TEXT("4 url"), Entry->GetStringField(TEXT("url")), FString(TEXT("http://127.0.0.1:55561/mcp")));
			TestEqual(TEXT("4 description"), Entry->GetStringField(TEXT("description")), FString(TEXT("keep me")));
			TestFalse(TEXT("4 no command"), Entry->HasField(TEXT("command")));
			TestFalse(TEXT("4 no args"), Entry->HasField(TEXT("args")));
			TestFalse(TEXT("4 no env"), Entry->HasField(TEXT("env")));
		}
	}

	// 5. Another port: the url is fixed in place, and a file with no final newline stays without one.
	{
		const FString Path = CaseDir(TEXT("WrongPort")) / TEXT(".mcp.json");
		WriteText(Path, TEXT("{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:8000/mcp\"\n    }\n  }\n}"));
		StateIs(Path, false, EState::WrongTarget, TEXT("5 before"));
		TestTrue(TEXT("5 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		AddInfo(Message);
		TestEqual(TEXT("5 content"), ReadText(Path), FString(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}")));
	}

	// 6. Another entry already points here (with a trailing slash): it counts as connected and is
	//    not duplicated.
	{
		const FString Path = CaseDir(TEXT("OtherName")) / TEXT(".mcp.json");
		const FString Text = TEXT("{\n  \"mcpServers\": {\n    \"my-unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp/\"\n    }\n  }\n}\n");
		WriteText(Path, Text);
		const McpClaudeCode::FStatus Status = StateIs(Path, false, EState::Connected, TEXT("6"));
		TestEqual(TEXT("6 entry"), Status.EntryName, FString(TEXT("my-unreal")));
		TestTrue(TEXT("6 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		TestEqual(TEXT("6 untouched"), ReadText(Path), Text);
	}

	// 7. With a token: the missing header is added from the environment variable, never in clear text.
	{
		const FString Path = CaseDir(TEXT("Token")) / TEXT(".mcp.json");
		WriteText(Path, TEXT("{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n"));
		StateIs(Path, false, EState::Connected, TEXT("7 without token"));
		StateIs(Path, true, EState::MissingAuth, TEXT("7 with token, before"));
		TestTrue(TEXT("7 connects"), McpClaudeCode::Connect(Path, Port, true, Message));
		AddInfo(Message);
		TestEqual(TEXT("7 content"), ReadText(Path), FString(TEXT(
			"{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp\",\n"
			"      \"headers\": {\n        \"Authorization\": \"Bearer ${UEAGENT_TOKEN}\"\n      }\n    }\n  }\n}\n")));
		StateIs(Path, true, EState::Connected, TEXT("7 with token, after"));
	}

	// 8-10. What cannot be read is not touched: broken JSON, an mcpServers that is not an object,
	//       an "unreal" that is not an object.
	{
		const TCHAR* const Broken[] = {
			TEXT("{ \"mcpServers\": "),
			TEXT("{\n  \"mcpServers\": []\n}\n"),
			TEXT("{\n  \"mcpServers\": {\n    \"unreal\": \"x\"\n  }\n}\n"),
		};
		int32 Index = 8;
		for (const TCHAR* Text : Broken)
		{
			const FString Path = CaseDir(*FString::Printf(TEXT("Unreadable%d"), Index)) / TEXT(".mcp.json");
			WriteText(Path, Text);
			StateIs(Path, false, EState::Unreadable, *FString::Printf(TEXT("%d"), Index));
			TestFalse(*FString::Printf(TEXT("%d does not connect"), Index), McpClaudeCode::Connect(Path, Port, false, Message));
			TestEqual(*FString::Printf(TEXT("%d untouched"), Index), ReadText(Path), FString(Text));
			TestFalse(*FString::Printf(TEXT("%d no temp file"), Index), FPaths::FileExists(Path + TEXT(".tmp")));
			++Index;
		}
	}

	// 11. No "mcpServers": it is appended at the end, without touching what was there.
	{
		const FString Path = CaseDir(TEXT("NoServers")) / TEXT(".mcp.json");
		WriteText(Path, TEXT("{\n  \"other\": 1\n}\n"));
		StateIs(Path, false, EState::MissingEntry, TEXT("11 before"));
		TestTrue(TEXT("11 connects"), McpClaudeCode::Connect(Path, Port, false, Message));
		TestEqual(TEXT("11 content"), ReadText(Path), FString(TEXT(
			"{\n  \"other\": 1,\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n")));
	}

	// 12. Claude Code approval: pending, approved, and a rejection wins even if another file approves it.
	{
		const FString Dir = CaseDir(TEXT("Approval"));
		const FString Path = Dir / TEXT(".mcp.json");
		WriteText(Path, TEXT("{\n  \"mcpServers\": {\n    \"unreal\": {\n      \"type\": \"http\",\n      \"url\": \"http://127.0.0.1:55561/mcp\"\n    }\n  }\n}\n"));
		TestTrue(TEXT("12 pending"), McpClaudeCode::Inspect(Path, Port, false).Approval == EApproval::Pending);
		WriteText(Dir / TEXT(".claude/settings.local.json"), TEXT("{\n  \"enabledMcpjsonServers\": [\"other\", \"unreal\"]\n}\n"));
		McpClaudeCode::FStatus Status = McpClaudeCode::Inspect(Path, Port, false);
		TestTrue(TEXT("12 approved"), Status.Approval == EApproval::Approved && Status.IsUsable());
		WriteText(Dir / TEXT(".claude/settings.json"), TEXT("{\n  \"disabledMcpjsonServers\": [\"unreal\"]\n}\n"));
		Status = McpClaudeCode::Inspect(Path, Port, false);
		TestTrue(TEXT("12 rejected"), Status.Approval == EApproval::Rejected && !Status.IsUsable() && Status.State == EState::Connected);
		TestTrue(TEXT("12 where"), Status.RejectedIn.EndsWith(TEXT("settings.json")));
		IFileManager::Get().Delete(*(Dir / TEXT(".claude/settings.json")));
		WriteText(Dir / TEXT(".claude/settings.local.json"), TEXT("{\n  \"enableAllProjectMcpServers\": true\n}\n"));
		TestTrue(TEXT("12 all"), McpClaudeCode::Inspect(Path, Port, false).Approval == EApproval::Approved);
	}

	// 13. The equivalent command, with single quotes so neither bash nor PowerShell expands the variable.
	TestEqual(TEXT("13 without token"), McpClaudeCode::AddCommand(Port, false),
		FString(TEXT("claude mcp add --transport http unreal http://127.0.0.1:55561/mcp")));
	TestEqual(TEXT("13 with token"), McpClaudeCode::AddCommand(Port, true),
		FString(TEXT("claude mcp add --transport http unreal http://127.0.0.1:55561/mcp --header 'Authorization: Bearer ${UEAGENT_TOKEN}'")));

	IFileManager::Get().DeleteDirectory(*RootDir(), /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
