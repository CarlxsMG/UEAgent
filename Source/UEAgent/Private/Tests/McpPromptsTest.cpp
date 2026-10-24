// Copyright (c) 2026 CarlxsMG.

// Test of the skills served as MCP prompts and resources (Mcp/McpPrompts.h), on a temporary folder, then on the
// plugin's own Resources/Skills. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.Prompts" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// What matters is the text a client gets (the skill without its frontmatter, the request, the skill's other files)
// and that a name or a URI is only ever looked up among the shipped skills, never followed as a path.

#include "Mcp/McpPrompts.h"
#include "Mcp/McpSkills.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpPromptsTestPrivate
{
	static void WriteText(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static TSharedPtr<FJsonObject> Params(const FString& Field, const FString& Value)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(Field, Value);
		return Object;
	}

	static FString MessageText(const TSharedPtr<FJsonObject>& Prompt)
	{
		const TArray<TSharedPtr<FJsonValue>>* Messages = nullptr;
		if (!Prompt.IsValid() || !Prompt->TryGetArrayField(TEXT("messages"), Messages) || Messages->Num() != 1)
		{
			return FString();
		}
		const TSharedPtr<FJsonObject> Message = (*Messages)[0]->AsObject();
		return Message->GetStringField(TEXT("role")) == TEXT("user")
			? Message->GetObjectField(TEXT("content"))->GetStringField(TEXT("text")) : FString();
	}

	static FString ResourceText(const TSharedPtr<FJsonObject>& Contents)
	{
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!Contents.IsValid() || !Contents->TryGetArrayField(TEXT("contents"), List) || List->Num() != 1)
		{
			return FString();
		}
		return (*List)[0]->AsObject()->GetStringField(TEXT("text"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpPromptsTest, "UEAgent.Prompts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpPromptsTest::RunTest(const FString& Parameters)
{
	using namespace McpPromptsTestPrivate;

	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpPrompts"));
	IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
	WriteText(Dir / TEXT("ueagent-alpha/SKILL.md"), TEXT("---\r\nname: ueagent-alpha\r\ndescription: \"Alpha skill.\"\r\n---\r\n\r\n\r\n# Alpha\r\n\r\nFill in template.md.\r\n"));
	WriteText(Dir / TEXT("ueagent-alpha/template.md"), TEXT("template"));
	WriteText(Dir / TEXT("ueagent-tool-map/SKILL.md"), TEXT("---\nname: ueagent-tool-map\ndescription: The map.\n---\n\n# Tool map\n\nMap.\n"));
	// No description: it is not served.
	WriteText(Dir / TEXT("ueagent-broken/SKILL.md"), TEXT("---\nname: ueagent-broken\n---\n\nBody.\n"));
	int32 Code = 0;
	FString Error, Uri;

	// 1. One prompt per skill that reads, with its title, description and the optional request.
	{
		const TArray<TSharedPtr<FJsonValue>> Prompts = McpPrompts::ListPrompts(Dir)->GetArrayField(TEXT("prompts"));
		TestEqual(TEXT("1 two prompts"), Prompts.Num(), 2);
		if (Prompts.Num() == 2)
		{
			const TSharedPtr<FJsonObject> Alpha = Prompts[0]->AsObject();
			TestEqual(TEXT("1 name"), Alpha->GetStringField(TEXT("name")), FString(TEXT("ueagent-alpha")));
			TestEqual(TEXT("1 title"), Alpha->GetStringField(TEXT("title")), FString(TEXT("Alpha")));
			TestEqual(TEXT("1 description unquoted"), Alpha->GetStringField(TEXT("description")), FString(TEXT("Alpha skill.")));
			const TSharedPtr<FJsonObject> Argument = Alpha->GetArrayField(TEXT("arguments"))[0]->AsObject();
			TestEqual(TEXT("1 request argument"), Argument->GetStringField(TEXT("name")), FString(TEXT("request")));
			TestFalse(TEXT("1 optional"), Argument->GetBoolField(TEXT("required")));
		}
	}

	// 2. prompts/get: the skill without its frontmatter, the request, the skill's other files; errors for the rest.
	{
		TSharedPtr<FJsonObject> Get = Params(TEXT("name"), TEXT("ueagent-alpha"));
		TestEqual(TEXT("2 no request"), MessageText(McpPrompts::GetPrompt(Dir, Get, Code, Error)),
			FString(TEXT("Use the UEAgent skill `ueagent-alpha` below.\n\n# Alpha\n\nFill in template.md.\n\n---\n\nThe skill's file `template.md`:\n\ntemplate\n")));
		TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
		Arguments->SetStringField(TEXT("request"), TEXT("  a door  "));
		Get->SetObjectField(TEXT("arguments"), Arguments);
		TestTrue(TEXT("2 request"), MessageText(McpPrompts::GetPrompt(Dir, Get, Code, Error))
			.StartsWith(TEXT("Use the UEAgent skill `ueagent-alpha` below for this request: a door\n\n# Alpha\n")));
		Arguments->SetNumberField(TEXT("request"), 5);
		TestTrue(TEXT("2 a request that is not a string is left out"), MessageText(McpPrompts::GetPrompt(Dir, Get, Code, Error))
			.StartsWith(TEXT("Use the UEAgent skill `ueagent-alpha` below.\n")));

		TestFalse(TEXT("2 no name"), McpPrompts::GetPrompt(Dir, MakeShared<FJsonObject>(), Code, Error).IsValid());
		TestEqual(TEXT("2 no name is Invalid params"), Code, -32602);
		TestFalse(TEXT("2 unknown"), McpPrompts::GetPrompt(Dir, Params(TEXT("name"), TEXT("ueagent-broken")), Code, Error).IsValid());
		TestTrue(TEXT("2 the error lists the prompts"), Code == -32602 && Error.Contains(TEXT("ueagent-alpha, ueagent-tool-map")));
		TestFalse(TEXT("2 a path is not a name"), McpPrompts::GetPrompt(Dir, Params(TEXT("name"), TEXT("../McpPrompts/ueagent-alpha")), Code, Error).IsValid());
		TestFalse(TEXT("2 the case counts"), McpPrompts::GetPrompt(Dir, Params(TEXT("name"), TEXT("UEAGENT-ALPHA")), Code, Error).IsValid());
	}

	// 3. resources/list: the guide first, then the other skills, then their files.
	{
		const TArray<TSharedPtr<FJsonValue>> Resources = McpPrompts::ListResources(Dir)->GetArrayField(TEXT("resources"));
		TArray<FString> Uris;
		for (const TSharedPtr<FJsonValue>& Resource : Resources)
		{
			Uris.Add(Resource->AsObject()->GetStringField(TEXT("uri")));
		}
		TestEqual(TEXT("3 uris"), FString::Join(Uris, TEXT(" ")),
			FString(TEXT("ueagent://guide ueagent://skills/ueagent-alpha ueagent://skills/ueagent-alpha/template.md")));
		if (Resources.Num() == 3)
		{
			TestEqual(TEXT("3 guide title"), Resources[0]->AsObject()->GetStringField(TEXT("title")), FString(TEXT("Tool map")));
			TestEqual(TEXT("3 markdown"), Resources[2]->AsObject()->GetStringField(TEXT("mimeType")), FString(TEXT("text/markdown")));
		}
	}

	// 4. resources/read: each listed URI, and nothing else.
	{
		TestEqual(TEXT("4 guide"), ResourceText(McpPrompts::ReadResource(Dir, Params(TEXT("uri"), TEXT("ueagent://guide")), Code, Error, Uri)),
			FString(TEXT("# Tool map\n\nMap.\n")));
		TestEqual(TEXT("4 skill"), ResourceText(McpPrompts::ReadResource(Dir, Params(TEXT("uri"), TEXT("ueagent://skills/ueagent-alpha")), Code, Error, Uri)),
			FString(TEXT("# Alpha\n\nFill in template.md.\n")));
		TestEqual(TEXT("4 file"), ResourceText(McpPrompts::ReadResource(Dir, Params(TEXT("uri"), TEXT("ueagent://skills/ueagent-alpha/template.md")), Code, Error, Uri)),
			FString(TEXT("template")));
		static const TCHAR* const UnlistedUris[] = { TEXT("ueagent://skills/ueagent-tool-map"), TEXT("ueagent://skills/ueagent-alpha/SKILL.md"),
			TEXT("ueagent://skills/../ueagent-alpha"), TEXT("ueagent://skills/ueagent-alpha/../ueagent-tool-map/SKILL.md"), TEXT("file:///C:/Windows/win.ini") };
		for (const TCHAR* Unlisted : UnlistedUris)
		{
			const bool bRead = McpPrompts::ReadResource(Dir, Params(TEXT("uri"), Unlisted), Code, Error, Uri).IsValid();
			TestTrue(FString::Printf(TEXT("4 not found: %s"), Unlisted), !bRead && Code == -32002 && Uri == Unlisted);
		}
		TestFalse(TEXT("4 no uri"), McpPrompts::ReadResource(Dir, MakeShared<FJsonObject>(), Code, Error, Uri).IsValid());
		TestEqual(TEXT("4 no uri is Invalid params"), Code, -32602);
	}

	// 5. The plugin's own skills: every one is served, the tool map as the guide.
	{
		const FString Shipped = McpSkills::ShippedDir();
		const int32 NumShipped = McpSkills::ShippedNames(Shipped).Num();
		TestTrue(TEXT("5 skills shipped"), NumShipped > 0);
		TestEqual(TEXT("5 one prompt per skill"), McpPrompts::ListPrompts(Shipped)->GetArrayField(TEXT("prompts")).Num(), NumShipped);
		TestTrue(TEXT("5 the guide reads"), ResourceText(McpPrompts::ReadResource(Shipped, Params(TEXT("uri"), McpPrompts::GuideUri), Code, Error, Uri))
			.Contains(TEXT("| Domain | Tools | What it covers |")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
