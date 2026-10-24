// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpPrompts.h"

#include "Mcp/McpSkills.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

const TCHAR* const McpPrompts::GuideUri = TEXT("ueagent://guide");
const TCHAR* const McpPrompts::GuideSkill = TEXT("ueagent-tool-map");

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpPromptsPrivate
{
	static const TCHAR* const SkillsUriPrefix = TEXT("ueagent://skills/");
	static const TCHAR* const RequestArgument = TEXT("request");
	static const TCHAR* const Markdown = TEXT("text/markdown");

	/** Every shipped skill that reads (a SKILL.md with a description), in name order. */
	static TArray<McpSkills::FShippedSkill> ReadAll(const FString& SkillsDir)
	{
		TArray<McpSkills::FShippedSkill> Skills;
		for (const FString& Name : McpSkills::ShippedNames(SkillsDir))
		{
			McpSkills::FShippedSkill Skill;
			if (McpSkills::ReadShipped(SkillsDir, Name, Skill))
			{
				Skills.Add(MoveTemp(Skill));
			}
		}
		return Skills;
	}

	static bool IsGuide(const FString& SkillName)
	{
		return SkillName.Equals(McpPrompts::GuideSkill, ESearchCase::CaseSensitive);
	}

	static FString MimeType(const FString& File)
	{
		return File.EndsWith(TEXT(".md")) ? FString(Markdown) : FString(TEXT("text/plain"));
	}

	static TSharedPtr<FJsonValue> Resource(const FString& Uri, const FString& Name, const FString& Title, const FString& Description, const FString& Mime)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("uri"), Uri);
		Entry->SetStringField(TEXT("name"), Name);
		if (!Title.IsEmpty())
		{
			Entry->SetStringField(TEXT("title"), Title);
		}
		Entry->SetStringField(TEXT("description"), Description);
		Entry->SetStringField(TEXT("mimeType"), Mime);
		return MakeShared<FJsonValueObject>(Entry);
	}

	static TSharedRef<FJsonObject> Contents(const FString& Uri, const FString& Mime, const FString& Text)
	{
		TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
		Content->SetStringField(TEXT("uri"), Uri);
		Content->SetStringField(TEXT("mimeType"), Mime);
		Content->SetStringField(TEXT("text"), Text);
		TArray<TSharedPtr<FJsonValue>> List;
		List.Add(MakeShared<FJsonValueObject>(Content));
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("contents"), List);
		return Result;
	}
}

TSharedRef<FJsonObject> McpPrompts::ListPrompts(const FString& SkillsDir)
{
	using namespace McpPromptsPrivate;

	TArray<TSharedPtr<FJsonValue>> Prompts;
	for (const McpSkills::FShippedSkill& Skill : ReadAll(SkillsDir))
	{
		TSharedRef<FJsonObject> Argument = MakeShared<FJsonObject>();
		Argument->SetStringField(TEXT("name"), RequestArgument);
		Argument->SetStringField(TEXT("description"), TEXT("What you want done, in your own words. Optional."));
		Argument->SetBoolField(TEXT("required"), false);
		TArray<TSharedPtr<FJsonValue>> Arguments;
		Arguments.Add(MakeShared<FJsonValueObject>(Argument));

		TSharedRef<FJsonObject> Prompt = MakeShared<FJsonObject>();
		Prompt->SetStringField(TEXT("name"), Skill.Name);
		Prompt->SetStringField(TEXT("title"), Skill.Title);
		Prompt->SetStringField(TEXT("description"), Skill.Description);
		Prompt->SetArrayField(TEXT("arguments"), Arguments);
		Prompts.Add(MakeShared<FJsonValueObject>(Prompt));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("prompts"), Prompts);
	return Result;
}

TSharedPtr<FJsonObject> McpPrompts::GetPrompt(const FString& SkillsDir, const TSharedPtr<FJsonObject>& Params, int32& OutCode, FString& OutError)
{
	using namespace McpPromptsPrivate;

	OutCode = -32602;
	FString Name;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		OutError = TEXT("Invalid params: prompts/get needs params.name");
		return nullptr;
	}
	McpSkills::FShippedSkill Skill;
	if (!McpSkills::ReadShipped(SkillsDir, Name, Skill))
	{
		TArray<FString> Names;
		for (const McpSkills::FShippedSkill& Each : ReadAll(SkillsDir))
		{
			Names.Add(Each.Name);
		}
		OutError = FString::Printf(TEXT("Invalid params: no prompt '%s'. The prompts are: %s"), *Name, *FString::Join(Names, TEXT(", ")));
		return nullptr;
	}

	// The prompt's one argument. Prompt arguments are strings: any other value is left out.
	FString Request;
	const TSharedPtr<FJsonObject>* Arguments = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), Arguments) && Arguments && Arguments->IsValid())
	{
		const TSharedPtr<FJsonValue> Value = (*Arguments)->TryGetField(RequestArgument);
		if (Value.IsValid() && Value->Type == EJson::String)
		{
			Request = Value->AsString().TrimStartAndEnd();
		}
	}

	FString Text = FString::Printf(TEXT("Use the UEAgent skill `%s` below"), *Skill.Name);
	Text += Request.IsEmpty() ? FString(TEXT(".")) : FString::Printf(TEXT(" for this request: %s"), *Request);
	Text += TEXT("\n\n") + Skill.Body;
	// A client has no folder to open a skill's other files from: they follow, each after a rule.
	for (const FString& File : Skill.Files)
	{
		FString FileText;
		if (McpSkills::ReadShippedFile(SkillsDir, Skill, File, FileText))
		{
			if (!FileText.EndsWith(TEXT("\n")))
			{
				FileText += TEXT("\n");
			}
			Text += FString::Printf(TEXT("\n---\n\nThe skill's file `%s`:\n\n%s"), *File, *FileText);
		}
	}

	TSharedRef<FJsonObject> Content = MakeShared<FJsonObject>();
	Content->SetStringField(TEXT("type"), TEXT("text"));
	Content->SetStringField(TEXT("text"), Text);
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), TEXT("user"));
	Message->SetObjectField(TEXT("content"), Content);
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(MakeShared<FJsonValueObject>(Message));

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("description"), Skill.Description);
	Result->SetArrayField(TEXT("messages"), Messages);
	return Result;
}

TSharedRef<FJsonObject> McpPrompts::ListResources(const FString& SkillsDir)
{
	using namespace McpPromptsPrivate;

	const TArray<McpSkills::FShippedSkill> Skills = ReadAll(SkillsDir);
	TArray<TSharedPtr<FJsonValue>> Resources;
	for (const McpSkills::FShippedSkill& Skill : Skills)
	{
		if (IsGuide(Skill.Name))
		{
			Resources.Add(Resource(GuideUri, TEXT("guide"), Skill.Title, Skill.Description, Markdown));
		}
	}
	for (const McpSkills::FShippedSkill& Skill : Skills)
	{
		if (!IsGuide(Skill.Name))
		{
			Resources.Add(Resource(SkillsUriPrefix + Skill.Name, Skill.Name, Skill.Title, Skill.Description, Markdown));
		}
	}
	for (const McpSkills::FShippedSkill& Skill : Skills)
	{
		for (const FString& File : Skill.Files)
		{
			Resources.Add(Resource(FString(SkillsUriPrefix) + Skill.Name + TEXT("/") + File, Skill.Name + TEXT("/") + File, FString(),
				FString::Printf(TEXT("The file %s of the %s skill, which its instructions refer to."), *File, *Skill.Name), MimeType(File)));
		}
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("resources"), Resources);
	return Result;
}

TSharedPtr<FJsonObject> McpPrompts::ReadResource(const FString& SkillsDir, const TSharedPtr<FJsonObject>& Params, int32& OutCode, FString& OutError, FString& OutUri)
{
	using namespace McpPromptsPrivate;

	OutCode = -32602;
	OutUri.Reset();
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("uri"), OutUri) || OutUri.IsEmpty())
	{
		OutError = TEXT("Invalid params: resources/read needs params.uri");
		return nullptr;
	}

	// ueagent://guide, or ueagent://skills/<name>[/<file>]; the tool map has the guide's URI only.
	FString SkillName = OutUri.Equals(GuideUri, ESearchCase::CaseSensitive) ? FString(GuideSkill) : FString();
	FString File;
	if (SkillName.IsEmpty() && OutUri.StartsWith(SkillsUriPrefix, ESearchCase::CaseSensitive))
	{
		const FString Rest = OutUri.RightChop(FCString::Strlen(SkillsUriPrefix));
		if (!Rest.Split(TEXT("/"), &SkillName, &File))
		{
			SkillName = Rest;
		}
		if (File.IsEmpty() && IsGuide(SkillName))
		{
			SkillName.Reset();
		}
	}
	McpSkills::FShippedSkill Skill;
	if (!SkillName.IsEmpty() && McpSkills::ReadShipped(SkillsDir, SkillName, Skill))
	{
		if (File.IsEmpty())
		{
			return Contents(OutUri, Markdown, Skill.Body);
		}
		FString Text;
		if (McpSkills::ReadShippedFile(SkillsDir, Skill, File, Text))
		{
			return Contents(OutUri, MimeType(File), Text);
		}
	}
	OutCode = -32002;
	OutError = FString::Printf(TEXT("Resource not found: %s"), *OutUri);
	return nullptr;
}
