// Copyright (c) 2026 CarlxsMG.

// Test of the catalog's retired and renamed tools (Mcp/McpToolCatalog.h) on a temporary catalog: it does not touch the
// catalog the server runs on. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.ToolCatalog" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// What must hold: a retired tool still runs under its own name but tools/list leaves it out, and a renamed tool's
// old name runs the new tool, with a note that says what to call; a rename that could not run anything is dropped.

#include "Mcp/McpToolCatalog.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpToolCatalogTest, "UEAgent.ToolCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpToolCatalogTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpToolCatalog"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = Dir / TEXT("command_catalog.json");
	FFileHelper::SaveStringToFile(TEXT(R"({
  "domains": [
    {
      "domain": "Test",
      "commands": [
        {"name": "listed_tool", "description": "Listed.", "returns": "{ ok }",
         "params": [{"name": "x", "jsonType": "string", "required": true, "description": "X."}]},
        {"name": "retired_tool", "description": "Retired.", "returns": "{ ok }", "params": [],
         "retired": {"use": "listed_tool", "how": "call it with x"}}
      ]
    }
  ],
  "renamed": {"old_tool": "listed_tool", "old_retired": "retired_tool", "dangling": "nowhere", "listed_tool": "retired_tool"}
})"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	FString Error;
	const TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::LoadFromFile(Path, Error);
	if (!TestTrue(FString::Printf(TEXT("the catalog loads (%s)"), *Error), Catalog.IsValid()))
	{
		return false;
	}

	// 1. The retired tool is not listed, and still found by its own name.
	TestEqual(TEXT("one listed tool"), Catalog->NumTools(), 1);
	TestEqual(TEXT("one retired tool"), Catalog->GetRetiredTools().Num(), 1);
	const FMcpToolCatalog::FTool* Retired = Catalog->FindTool(TEXT("retired_tool"));
	if (TestNotNull(TEXT("FindTool finds the retired tool"), Retired))
	{
		TestEqual(TEXT("its listed replacement"), Retired->RetiredFor, FString(TEXT("listed_tool")));
		TestTrue(TEXT("its schema is built"), Retired->InputSchema.IsValid());
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Catalog->GetToolsListUtf8().GetData()), Catalog->GetToolsListUtf8().Num());
	const FString List(Converted.Length(), Converted.Get());
	TestTrue(TEXT("tools/list carries the listed tool"), List.Contains(TEXT("\"listed_tool\"")));
	TestFalse(TEXT("tools/list leaves the retired tool out"), List.Contains(TEXT("retired_tool")));
	TestFalse(TEXT("tools/list leaves the old name out"), List.Contains(TEXT("old_tool")));

	// 2. The old name runs the new tool and says what to call; the listed and retired names run themselves.
	FString Note;
	TestEqual(TEXT("old name -> new name"), Catalog->ResolveName(TEXT("old_tool"), &Note), FString(TEXT("listed_tool")));
	TestTrue(FString::Printf(TEXT("the note names both (%s)"), *Note), Note.Contains(TEXT("old_tool")) && Note.Contains(TEXT("listed_tool")));
	TestEqual(TEXT("names compare without case"), Catalog->ResolveName(TEXT("OLD_TOOL")), FString(TEXT("listed_tool")));
	TestNull(TEXT("FindTool alone does not resolve an old name"), Catalog->FindTool(TEXT("old_tool")));
	TestEqual(TEXT("a retired tool runs under its own name"), Catalog->ResolveName(TEXT("retired_tool"), &Note), FString(TEXT("retired_tool")));
	TestTrue(FString::Printf(TEXT("its note names the listed tool and how (%s)"), *Note),
		Note.Contains(TEXT("listed_tool")) && Note.Contains(TEXT("call it with x")));
	TestEqual(TEXT("a listed tool runs itself"), Catalog->ResolveName(TEXT("listed_tool"), &Note), FString(TEXT("listed_tool")));
	TestTrue(TEXT("with no note"), Note.IsEmpty());

	// 3. A rename to a tool that does not exist, or of a name that is still a tool, is dropped.
	TestEqual(TEXT("two renames kept"), Catalog->GetRenamedTools().Num(), 2);
	TestEqual(TEXT("a rename to nothing runs nothing new"), Catalog->ResolveName(TEXT("dangling")), FString(TEXT("dangling")));
	TestEqual(TEXT("an old name to a retired tool is kept"), Catalog->ResolveName(TEXT("old_retired")), FString(TEXT("retired_tool")));

	IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

#endif
