// Copyright (c) 2026 CarlxsMG.

// Test of the user's tool permissions (Mcp/McpToolPolicy.h): the verdict of each class of tool under each setting, and
// the lists tools/list and find_tools build with them, on a temporary catalog and on the shipped one. It restores the
// permissions the editor runs with. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.ToolPolicy" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI

#include "Mcp/McpMetaTools.h"
#include "Mcp/McpToolCatalog.h"
#include "Mcp/McpToolPolicy.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpToolPolicyTestPrivate
{
	/** The tool names of a serialized `{"tools":[...]}` list, in order. */
	static TArray<FString> ListedNames(const TArray<uint8>& Utf8)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()), Utf8.Num());
		TSharedPtr<FJsonObject> List;
		TArray<FString> Names;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Converted.Length(), Converted.Get())), List) || !List.IsValid())
		{
			return Names;
		}
		for (const TSharedPtr<FJsonValue>& Tool : List->GetArrayField(TEXT("tools")))
		{
			Names.Add(Tool->AsObject()->GetStringField(TEXT("name")));
		}
		return Names;
	}

	static FMcpToolPolicy::FSettings ReadOnly()
	{
		FMcpToolPolicy::FSettings Settings;
		Settings.bReadOnly = true;
		Settings.ReadOnlySource = TEXT("settings");
		return Settings;
	}

	static FMcpToolPolicy::FSettings NoCode()
	{
		FMcpToolPolicy::FSettings Settings;
		Settings.bAllowCodeExecution = false;
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpToolPolicyTest, "UEAgent.ToolPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpToolPolicyTest::RunTest(const FString& Parameters)
{
	using namespace McpToolPolicyTestPrivate;
	using EVerdict = FMcpToolPolicy::EVerdict;

	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpToolPolicy"));
	IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
	const FString Path = Dir / TEXT("command_catalog.json");
	FFileHelper::SaveStringToFile(TEXT(R"({
  "domains": [
    {
      "domain": "Alpha",
      "commands": [
        {"name": "get_thing", "description": "Reads.", "core": true, "params": [], "annotations": {"readOnlyHint": true}},
        {"name": "set_thing", "description": "Writes.", "core": true, "params": [], "annotations": {"readOnlyHint": false}},
        {"name": "capture_thing", "description": "Captures.", "params": [], "observes": true, "annotations": {"readOnlyHint": false}},
        {"name": "run_code", "description": "Runs code.", "core": true, "params": [], "codeExec": true, "annotations": {"readOnlyHint": false}},
        {"name": "execute_batch", "description": "Batch.", "params": [], "annotations": {"readOnlyHint": false}},
        {"name": "old_writer", "description": "Retired.", "params": [], "retired": {"use": "set_thing"}, "annotations": {"readOnlyHint": false}}
      ]
    },
    {
      "domain": "Beta",
      "commands": [
        {"name": "get_beta", "description": "Reads beta.", "params": [], "annotations": {"readOnlyHint": true}},
        {"name": "set_beta", "description": "Writes beta.", "params": []}
      ]
    }
  ],
  "renamed": {"put_thing": "set_thing"}
})"), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	FString Error;
	const TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::LoadFromFile(Path, Error);
	if (!TestTrue(FString::Printf(TEXT("the catalog loads (%s)"), *Error), Catalog.IsValid()))
	{
		return false;
	}

	// 1. The matrix: each class under each setting.
	struct FRow
	{
		const TCHAR* Tool;
		EVerdict Open;
		EVerdict ReadOnly;
		EVerdict NoCode;
	};
	const FRow Rows[] = {
		{ TEXT("get_thing"),     EVerdict::Allowed, EVerdict::Allowed,  EVerdict::Allowed },
		{ TEXT("set_thing"),     EVerdict::Allowed, EVerdict::ReadOnly, EVerdict::Allowed },
		{ TEXT("capture_thing"), EVerdict::Allowed, EVerdict::Allowed,  EVerdict::Allowed },
		{ TEXT("run_code"),      EVerdict::Allowed, EVerdict::ReadOnly, EVerdict::CodeExecution },
		{ TEXT("execute_batch"), EVerdict::Allowed, EVerdict::Allowed,  EVerdict::Allowed },
		{ TEXT("old_writer"),    EVerdict::Allowed, EVerdict::ReadOnly, EVerdict::Allowed },
		{ TEXT("put_thing"),     EVerdict::Allowed, EVerdict::ReadOnly, EVerdict::Allowed },
		{ TEXT("set_beta"),      EVerdict::Allowed, EVerdict::ReadOnly, EVerdict::Allowed },
		{ TEXT("no_such_tool"),  EVerdict::Allowed, EVerdict::ReadOnly, EVerdict::Allowed },
	};
	const TSharedRef<const FMcpToolPolicy> Open = FMcpToolPolicy::Make(FMcpToolPolicy::FSettings());
	const TSharedRef<const FMcpToolPolicy> ReadOnlyPolicy = FMcpToolPolicy::Make(ReadOnly());
	const TSharedRef<const FMcpToolPolicy> NoCodePolicy = FMcpToolPolicy::Make(NoCode());
	for (const FRow& Row : Rows)
	{
		TestEqual(FString::Printf(TEXT("%s with nothing off"), Row.Tool), (int32)Open->EvaluateName(Catalog.Get(), Row.Tool), (int32)Row.Open);
		TestEqual(FString::Printf(TEXT("%s in read-only mode"), Row.Tool), (int32)ReadOnlyPolicy->EvaluateName(Catalog.Get(), Row.Tool), (int32)Row.ReadOnly);
		TestEqual(FString::Printf(TEXT("%s with code execution off"), Row.Tool), (int32)NoCodePolicy->EvaluateName(Catalog.Get(), Row.Tool), (int32)Row.NoCode);
	}
	FMcpToolPolicy::FSettings Lists;
	Lists.DisabledDomains = { TEXT(" beta ") };
	Lists.DisabledTools = { TEXT("put_thing") };
	const TSharedRef<const FMcpToolPolicy> ListsPolicy = FMcpToolPolicy::Make(Lists);
	TestEqual(TEXT("a domain turned off, the case not compared"), (int32)ListsPolicy->EvaluateName(Catalog.Get(), TEXT("get_beta")), (int32)EVerdict::DisabledDomain);
	TestEqual(TEXT("a tool turned off by an old name, called by it"), (int32)ListsPolicy->EvaluateName(Catalog.Get(), TEXT("put_thing")), (int32)EVerdict::DisabledTool);
	TestEqual(TEXT("the other tools run"), (int32)ListsPolicy->EvaluateName(Catalog.Get(), TEXT("get_thing")), (int32)EVerdict::Allowed);
	TestTrue(TEXT("each snapshot has its own generation"), ReadOnlyPolicy->GetGeneration() != NoCodePolicy->GetGeneration());
	TestFalse(TEXT("nothing off is not restrictive"), Open->IsRestrictive());

	// 2. The refusal, in the shape of the other refusals.
	const TSharedRef<FJsonObject> Refusal = ReadOnlyPolicy->MakeRefusalPayload(EVerdict::ReadOnly, TEXT("set_thing"), TEXT("Alpha"));
	const TSharedPtr<FJsonObject> Data = Refusal->GetObjectField(TEXT("result"));
	TestFalse(TEXT("the refusal is not a success"), Refusal->GetBoolField(TEXT("success")));
	TestTrue(TEXT("refused_by policy"), Data->GetBoolField(TEXT("refused")) && Data->GetStringField(TEXT("refused_by")) == TEXT("policy"));
	TestEqual(TEXT("the policy named"), Data->GetStringField(TEXT("policy")), FString(TEXT("read_only")));
	TestTrue(TEXT("the message says not to retry and to ask the user"),
		Refusal->GetStringField(TEXT("error")).Contains(TEXT("do not retry")) && Refusal->GetStringField(TEXT("error")).Contains(TEXT("ask them")));

	// 3. The lists: full and core, in each mode.
	const TArray<FString> FullOpen = ListedNames(Catalog->BuildFullToolsListUtf8(*Open));
	TestEqual(TEXT("the full list with nothing off is the catalog's"), FullOpen.Num(), Catalog->NumTools());
	TestEqual(TEXT("the full list in read-only mode: reads, the capture and the batch"),
		ListedNames(Catalog->BuildFullToolsListUtf8(*ReadOnlyPolicy)), TArray<FString>({ TEXT("get_thing"), TEXT("capture_thing"), TEXT("execute_batch"), TEXT("get_beta") }));
	TestEqual(TEXT("the full list with code execution off loses run_code only"), ListedNames(Catalog->BuildFullToolsListUtf8(*NoCodePolicy)).Num(), Catalog->NumTools() - 1);
	const TArray<FString> CoreReadOnly = ListedNames(Catalog->BuildCoreToolsListUtf8(TSet<FString>(), *ReadOnlyPolicy));
	TestEqual(TEXT("the core list in read-only mode: the core read and the four meta-tools"), CoreReadOnly,
		TArray<FString>({ TEXT("get_thing"), McpMetaTools::FindTools, McpMetaTools::DescribeTool, McpMetaTools::CallTool, McpMetaTools::EnableToolset }));
	TestEqual(TEXT("the same list again (cached for this generation)"), ListedNames(Catalog->BuildCoreToolsListUtf8(TSet<FString>(), *ReadOnlyPolicy)), CoreReadOnly);
	TestEqual(TEXT("a domain enabled adds only what read-only mode allows"),
		ListedNames(Catalog->BuildCoreToolsListUtf8(TSet<FString>({ TEXT("Beta") }), *ReadOnlyPolicy)).Num(), CoreReadOnly.Num() + 1);
	TestEqual(TEXT("a domain turned off is not enabled"),
		ListedNames(Catalog->BuildCoreToolsListUtf8(TSet<FString>({ TEXT("Beta") }), *ListsPolicy)).Num(), 3 + 4);
	TestEqual(TEXT("turning it all back on restores the core list"),
		ListedNames(Catalog->BuildCoreToolsListUtf8(TSet<FString>(), *Open)), ListedNames(Catalog->GetCoreToolsListUtf8()));

	// 4. find_tools and describe_tool read the current permissions: set them for this part, then restore them.
	const TSharedRef<const FMcpToolPolicy> Saved = FMcpToolPolicy::GetCurrent();
	FMcpToolPolicy::SetCurrent(ListsPolicy);
	{
		bool bIsError = false;
		TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("query"), TEXT("beta"));
		const TSharedPtr<FJsonObject> Found = McpMetaTools::Answer(*Catalog, McpMetaTools::FindTools, Args, bIsError);
		TestEqual(TEXT("find_tools does not find a tool of a domain turned off"), Found.IsValid() ? Found->GetArrayField(TEXT("tools")).Num() : -1, 0);
		Args->SetStringField(TEXT("domain"), TEXT("Beta"));
		McpMetaTools::Answer(*Catalog, McpMetaTools::FindTools, Args, bIsError);
		TestTrue(TEXT("find_tools on a domain turned off is an error"), bIsError);
		TestTrue(TEXT("the other tools are still suggested"), McpMetaTools::SuggestNames(*Catalog, TEXT("set_thin")).Contains(TEXT("set_thing")));
		FMcpToolPolicy::SetCurrent(ReadOnlyPolicy);
		TSharedRef<FJsonObject> Describe = MakeShared<FJsonObject>();
		Describe->SetStringField(TEXT("name"), TEXT("set_thing"));
		const TSharedPtr<FJsonObject> Described = McpMetaTools::Answer(*Catalog, McpMetaTools::DescribeTool, Describe, bIsError);
		TestTrue(TEXT("describe_tool on a tool turned off answers the refusal"), bIsError && Described.IsValid() && Described->HasField(TEXT("result")));
		TestFalse(TEXT("an unknown name never suggests a tool turned off"), McpMetaTools::SuggestNames(*Catalog, TEXT("set_thin")).Contains(TEXT("set_thing")));
	}
	FMcpToolPolicy::SetCurrent(Saved);

	// 5. The shipped catalog: code execution off takes out exactly the six, read-only mode keeps the five captures.
	const TSharedPtr<const FMcpToolCatalog> Shipped = FMcpToolCatalog::LoadFromFile(FMcpToolCatalog::DefaultCatalogPath(), Error);
	if (TestTrue(FString::Printf(TEXT("the shipped catalog loads (%s)"), *Error), Shipped.IsValid()))
	{
		const TArray<FString> NoCodeList = ListedNames(Shipped->BuildFullToolsListUtf8(*NoCodePolicy));
		TestEqual(TEXT("code execution off lists all but the six"), NoCodeList.Num(), Shipped->NumTools() - 6);
		for (const TCHAR* Name : { TEXT("execute_python"), TEXT("execute_python_file"), TEXT("execute_console_command"), TEXT("call_function"),
			TEXT("run_external_project_script"), TEXT("launch_dedicated_server") })
		{
			TestFalse(FString::Printf(TEXT("%s is not listed with code execution off"), Name), NoCodeList.Contains(Name));
		}
		const TArray<FString> ReadOnlyList = ListedNames(Shipped->BuildFullToolsListUtf8(*ReadOnlyPolicy));
		for (const TCHAR* Name : { TEXT("capture_viewport"), TEXT("capture_high_res_screenshot"), TEXT("capture_pie_screenshot"),
			TEXT("render_widget_preview"), TEXT("generate_asset_thumbnail"), TEXT("get_project_info") })
		{
			TestTrue(FString::Printf(TEXT("%s is listed in read-only mode"), Name), ReadOnlyList.Contains(Name));
		}
		for (const TCHAR* Name : { TEXT("spawn_actor"), TEXT("undo"), TEXT("redo"), TEXT("begin_transaction"), TEXT("execute_python"), TEXT("save_asset") })
		{
			TestFalse(FString::Printf(TEXT("%s is not listed in read-only mode"), Name), ReadOnlyList.Contains(Name));
		}
		int32 Reads = 0;
		for (const FMcpToolCatalog::FTool& Tool : Shipped->GetTools())
		{
			bool bReadOnlyHint = false;
			Reads += (Tool.bObserves || Tool.Name == TEXT("execute_batch")
				|| (!Tool.bCodeExec && Tool.Annotations.IsValid() && Tool.Annotations->TryGetBoolField(TEXT("readOnlyHint"), bReadOnlyHint) && bReadOnlyHint)) ? 1 : 0;
		}
		TestEqual(TEXT("read-only mode lists the reads, the captures and execute_batch"), ReadOnlyList.Num(), Reads);
	}
	return true;
}

#endif
