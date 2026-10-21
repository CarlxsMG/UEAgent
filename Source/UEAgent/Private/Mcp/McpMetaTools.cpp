// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpMetaTools.h"

#include "Mcp/McpToolCatalog.h"

#include "Algo/LevenshteinDistance.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

const TCHAR* const McpMetaTools::FindTools = TEXT("find_tools");
const TCHAR* const McpMetaTools::DescribeTool = TEXT("describe_tool");
const TCHAR* const McpMetaTools::CallTool = TEXT("call_tool");

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpMetaToolsPrivate
{
	constexpr int32 DefaultLimit = 25;
	constexpr int32 MaxLimit = 300;        // above the largest domain, so a domain always fits in one call
	constexpr int32 MaxSummaryChars = 200;

	struct FDomainCount
	{
		FString Name;
		int32 NumTools = 0;
	};

	/** The catalog's domains, in catalog order, with how many tools each one has. */
	static TArray<FDomainCount> CountDomains(const FMcpToolCatalog& Catalog)
	{
		TArray<FDomainCount> Domains;
		for (const FMcpToolCatalog::FTool& Tool : Catalog.GetTools())
		{
			FDomainCount* Domain = Domains.FindByPredicate([&Tool](const FDomainCount& D) { return D.Name == Tool.Domain; });
			if (!Domain)
			{
				Domain = &Domains.AddDefaulted_GetRef();
				Domain->Name = Tool.Domain;
			}
			++Domain->NumTools;
		}
		return Domains;
	}

	/** Lower-case words of Text, split on anything that is not a letter or a digit. */
	static TArray<FString> SplitWords(const FString& Text)
	{
		TArray<FString> Words;
		FString Current;
		for (const TCHAR C : Text)
		{
			if (FChar::IsAlnum(C))
			{
				Current.AppendChar(FChar::ToLower(C));
			}
			else if (!Current.IsEmpty())
			{
				Words.Add(MoveTemp(Current));
				Current.Reset();
			}
		}
		if (!Current.IsEmpty())
		{
			Words.Add(MoveTemp(Current));
		}
		return Words;
	}

	/** The words of a query worth matching: no repeats, and none of the ones every description has. */
	static TArray<FString> QueryWords(const FString& Query)
	{
		static const TCHAR* const StopWords[] = {
			TEXT("a"), TEXT("an"), TEXT("and"), TEXT("as"), TEXT("at"), TEXT("by"), TEXT("for"), TEXT("from"), TEXT("in"),
			TEXT("into"), TEXT("is"), TEXT("it"), TEXT("of"), TEXT("on"), TEXT("or"), TEXT("the"), TEXT("to"), TEXT("with") };
		TArray<FString> Words;
		for (const FString& Word : SplitWords(Query))
		{
			bool bStop = false;
			for (const TCHAR* Stop : StopWords)
			{
				bStop |= Word == Stop;
			}
			if (!bStop)
			{
				Words.AddUnique(Word);
			}
		}
		return Words;
	}

	/** A word and its singular forms: "actors" also finds "actor", "properties" "property", "meshes" "mesh". */
	static TArray<FString> WordForms(const FString& Word)
	{
		TArray<FString> Forms = { Word };
		if (Word.Len() > 3)
		{
			if (Word.EndsWith(TEXT("ies")))
			{
				Forms.Add(Word.LeftChop(3) + TEXT("y"));
			}
			else if (Word.EndsWith(TEXT("es")))
			{
				Forms.Add(Word.LeftChop(1));
				Forms.Add(Word.LeftChop(2));
			}
			else if (Word.EndsWith(TEXT("s")) && !Word.EndsWith(TEXT("ss")))
			{
				Forms.Add(Word.LeftChop(1));
			}
		}
		return Forms;
	}

	/** Does Text have a word that starts with Prefix ("set" starts "settings" but not "asset")? */
	static bool StartsAWord(const FString& Text, const FString& Prefix)
	{
		int32 From = 0;
		for (;;)
		{
			const int32 At = Text.Find(Prefix, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
			if (At == INDEX_NONE)
			{
				return false;
			}
			if (At == 0 || !FChar::IsAlnum(Text[At - 1]))
			{
				return true;
			}
			From = At + 1;
		}
	}

	/** How well one query word (in all its forms) matches a tool: a word of its name, then the name, domain, description. */
	static int32 WordScore(const FMcpToolCatalog::FTool& Tool, const TArray<FString>& NameWords, const TArray<FString>& Forms)
	{
		int32 Best = 0;
		for (const FString& Form : Forms)
		{
			if (NameWords.Contains(Form))
			{
				return 6;
			}
			if (StartsAWord(Tool.Name, Form))
			{
				Best = FMath::Max(Best, 4);
			}
			else if (Tool.Domain.Contains(Form))
			{
				Best = FMath::Max(Best, 2);
			}
			else if (StartsAWord(Tool.Description, Form))
			{
				Best = FMath::Max(Best, 1);
			}
		}
		return Best;
	}

	struct FHit
	{
		const FMcpToolCatalog::FTool* Tool = nullptr;
		int32 WordsMatched = 0;
		int32 Score = 0;
	};

	/**
	 * The tools that match at least one word (all of Domain's tools if there are no words), best
	 * first: the most words matched, then the strongest matches, then by name.
	 */
	static TArray<FHit> Search(const FMcpToolCatalog& Catalog, const TArray<FString>& Words, const FString& Domain)
	{
		TArray<TArray<FString>> FormsPerWord;
		for (const FString& Word : Words)
		{
			FormsPerWord.Add(WordForms(Word));
		}
		const FString AsName = FString::Join(Words, TEXT("_"));

		TArray<FHit> Hits;
		for (const FMcpToolCatalog::FTool& Tool : Catalog.GetTools())
		{
			if (!Domain.IsEmpty() && !Tool.Domain.Equals(Domain, ESearchCase::IgnoreCase))
			{
				continue;
			}
			FHit Hit;
			Hit.Tool = &Tool;
			if (Words.Num() > 0)
			{
				TArray<FString> NameWords;
				Tool.Name.ToLower().ParseIntoArray(NameWords, TEXT("_"));
				for (const TArray<FString>& Forms : FormsPerWord)
				{
					const int32 Score = WordScore(Tool, NameWords, Forms);
					Hit.WordsMatched += Score > 0 ? 1 : 0;
					Hit.Score += Score;
				}
				if (Hit.WordsMatched == 0)
				{
					continue;
				}
				if (Tool.Name.Equals(AsName, ESearchCase::IgnoreCase))
				{
					Hit.Score += 100;
				}
			}
			Hits.Add(Hit);
		}
		Hits.Sort([](const FHit& A, const FHit& B)
		{
			if (A.WordsMatched != B.WordsMatched)
			{
				return A.WordsMatched > B.WordsMatched;
			}
			if (A.Score != B.Score)
			{
				return A.Score > B.Score;
			}
			return A.Tool->Name.Compare(B.Tool->Name, ESearchCase::IgnoreCase) < 0;
		});
		return Hits;
	}

	/** The description's first sentence, cut at a word to at most MaxSummaryChars. */
	static FString Summary(const FString& Description)
	{
		int32 End = Description.Len();
		for (int32 Index = 0; Index + 1 < Description.Len(); ++Index)
		{
			const TCHAR C = Description[Index];
			if ((C != TEXT('.') && C != TEXT('!') && C != TEXT('?')) || !FChar::IsWhitespace(Description[Index + 1]))
			{
				continue;
			}
			const FString Before = Description.Mid(FMath::Max(0, Index - 3), 3);
			if (C == TEXT('.') && (Before.Equals(TEXT("e.g"), ESearchCase::IgnoreCase) || Before.Equals(TEXT("i.e"), ESearchCase::IgnoreCase)))
			{
				continue;
			}
			End = Index + 1;
			break;
		}
		FString Text = Description.Left(End).TrimEnd();
		if (Text.Len() > MaxSummaryChars)
		{
			int32 Cut = MaxSummaryChars;
			while (Cut > MaxSummaryChars / 2 && !FChar::IsWhitespace(Text[Cut]))
			{
				--Cut;
			}
			Text = Text.Left(Cut).TrimEnd() + TEXT("...");
		}
		return Text;
	}

	/** "a*, b, c": the tool's parameters in catalog order, * marking the required ones. */
	static FString ParamsLine(const FMcpToolCatalog::FTool& Tool)
	{
		TArray<FString> Names;
		for (const FMcpToolCatalog::FParam& Param : Tool.Params)
		{
			Names.Add(Param.bRequired ? Param.Name + TEXT("*") : Param.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	static TSharedRef<FJsonObject> Failure(const FString& Error)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), false);
		Result->SetStringField(TEXT("error"), Error);
		return Result;
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonStrings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Json;
		for (const FString& Value : Values)
		{
			Json.Add(MakeShared<FJsonValueString>(Value));
		}
		return Json;
	}

	static TSharedRef<FJsonObject> AnswerFindTools(const FMcpToolCatalog& Catalog, const TSharedPtr<FJsonObject>& Args, bool& bOutIsError)
	{
		FString Query;
		FString Domain;
		double Limit = DefaultLimit;
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("query"), Query);
			Args->TryGetStringField(TEXT("domain"), Domain);
			Args->TryGetNumberField(TEXT("limit"), Limit);
		}
		const int32 MaxTools = FMath::Clamp(FMath::FloorToInt32(Limit), 1, MaxLimit);

		const TArray<FDomainCount> Domains = CountDomains(Catalog);
		Domain.TrimStartAndEndInline();
		if (!Domain.IsEmpty())
		{
			const FDomainCount* Known = Domains.FindByPredicate([&Domain](const FDomainCount& D) { return D.Name.Equals(Domain, ESearchCase::IgnoreCase); });
			if (!Known)
			{
				TArray<FString> Names;
				for (const FDomainCount& D : Domains)
				{
					Names.Add(D.Name);
				}
				bOutIsError = true;
				return Failure(FString::Printf(TEXT("Unknown domain '%s'. The domains are: %s"), *Domain, *FString::Join(Names, TEXT(", "))));
			}
			Domain = Known->Name;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		const TArray<FString> Words = QueryWords(Query);
		if (Words.Num() == 0 && Domain.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> DomainsJson;
			for (const FDomainCount& D : Domains)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("domain"), D.Name);
				Entry->SetNumberField(TEXT("tools"), D.NumTools);
				DomainsJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetNumberField(TEXT("total_tools"), Catalog.NumTools());
			Result->SetArrayField(TEXT("domains"), DomainsJson);
			Result->SetStringField(TEXT("next"), TEXT("Pass `query` (words from a tool's name or purpose) and/or `domain` to list tools."));
			return Result;
		}

		const TArray<FHit> Hits = Search(Catalog, Words, Domain);
		TArray<TSharedPtr<FJsonValue>> ToolsJson;
		for (int32 Index = 0; Index < FMath::Min(MaxTools, Hits.Num()); ++Index)
		{
			const FMcpToolCatalog::FTool& Tool = *Hits[Index].Tool;
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Tool.Name);
			Entry->SetStringField(TEXT("domain"), Tool.Domain);
			const FString Params = ParamsLine(Tool);
			if (!Params.IsEmpty())
			{
				Entry->SetStringField(TEXT("params"), Params);
			}
			Entry->SetStringField(TEXT("summary"), Summary(Tool.Description));
			ToolsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		Result->SetNumberField(TEXT("matches"), Hits.Num());
		Result->SetArrayField(TEXT("tools"), ToolsJson);
		if (Hits.Num() > MaxTools)
		{
			Result->SetStringField(TEXT("more"), FString::Printf(
				TEXT("%d more: add words to the query, or raise `limit` (up to %d)."), Hits.Num() - MaxTools, MaxLimit));
		}
		else if (Hits.Num() == 0)
		{
			Result->SetStringField(TEXT("next"), TEXT("No tool matches: try other words, or find_tools with no arguments for the domains."));
		}
		return Result;
	}

	static TSharedRef<FJsonObject> AnswerDescribeTool(const FMcpToolCatalog& Catalog, const TSharedPtr<FJsonObject>& Args, bool& bOutIsError)
	{
		FString Name;
		if (Args.IsValid())
		{
			Args->TryGetStringField(TEXT("name"), Name);
		}
		Name.TrimStartAndEndInline();
		if (Name.IsEmpty())
		{
			bOutIsError = true;
			return Failure(TEXT("describe_tool: missing required parameter 'name'"));
		}
		const FMcpToolCatalog::FTool* Tool = Catalog.FindTool(Name);
		if (!Tool)
		{
			bOutIsError = true;
			TSharedRef<FJsonObject> Result = Failure(FString::Printf(TEXT("Unknown tool: %s"), *Name));
			Result->SetArrayField(TEXT("closest"), ToJsonStrings(McpMetaTools::SuggestNames(Catalog, Name)));
			return Result;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("name"), Tool->Name);
		Result->SetStringField(TEXT("domain"), Tool->Domain);
		Result->SetStringField(TEXT("description"), Tool->PublishedDescription());
		Result->SetObjectField(TEXT("inputSchema"), Tool->InputSchema);
		if (Tool->Annotations.IsValid())
		{
			Result->SetObjectField(TEXT("annotations"), Tool->Annotations);
		}
		return Result;
	}

	static TSharedRef<FJsonObject> Property(const TCHAR* Type, const FString& Description)
	{
		TSharedRef<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), Type);
		Schema->SetStringField(TEXT("description"), Description);
		return Schema;
	}

	static TSharedPtr<FJsonValue> MakeTool(const TCHAR* Name, const FString& Description, const TSharedRef<FJsonObject>& Properties,
		const TArray<FString>& Required, bool bReadOnly)
	{
		TSharedRef<FJsonObject> InputSchema = MakeShared<FJsonObject>();
		InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		InputSchema->SetObjectField(TEXT("properties"), Properties);
		if (Required.Num() > 0)
		{
			InputSchema->SetArrayField(TEXT("required"), ToJsonStrings(Required));
		}
		TSharedRef<FJsonObject> Tool = MakeShared<FJsonObject>();
		Tool->SetStringField(TEXT("name"), Name);
		Tool->SetStringField(TEXT("description"), Description);
		Tool->SetObjectField(TEXT("inputSchema"), InputSchema);
		if (bReadOnly)
		{
			// Lets a client run them without asking: they only read the catalog.
			TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
			Annotations->SetBoolField(TEXT("readOnlyHint"), true);
			Tool->SetObjectField(TEXT("annotations"), Annotations);
		}
		return MakeShared<FJsonValueObject>(Tool);
	}
}

TArray<uint8> McpMetaTools::BuildToolsListUtf8(const FMcpToolCatalog& Catalog)
{
	using namespace McpMetaToolsPrivate;

	const TArray<FDomainCount> Domains = CountDomains(Catalog);
	TArray<FString> DomainNames;
	for (const FDomainCount& D : Domains)
	{
		DomainNames.Add(D.Name);
	}

	TSharedRef<FJsonObject> FindProperties = MakeShared<FJsonObject>();
	FindProperties->SetObjectField(TEXT("query"), Property(TEXT("string"),
		TEXT("Words from the tool's name or purpose, e.g. \"spawn actor\" or \"material parameter\". A plural also finds the singular.")));
	TSharedRef<FJsonObject> DomainProperty = Property(TEXT("string"), TEXT("Only this domain's tools; without a query, lists them all."));
	DomainProperty->SetArrayField(TEXT("enum"), ToJsonStrings(DomainNames));
	FindProperties->SetObjectField(TEXT("domain"), DomainProperty);
	FindProperties->SetObjectField(TEXT("limit"), Property(TEXT("integer"),
		FString::Printf(TEXT("How many tools to return (default %d, at most %d)."), DefaultLimit, MaxLimit)));

	TSharedRef<FJsonObject> DescribeProperties = MakeShared<FJsonObject>();
	DescribeProperties->SetObjectField(TEXT("name"), Property(TEXT("string"), TEXT("The tool's exact name, as find_tools returns it.")));

	TSharedRef<FJsonObject> CallProperties = MakeShared<FJsonObject>();
	CallProperties->SetObjectField(TEXT("name"), Property(TEXT("string"), TEXT("The tool to run, as find_tools returns it.")));
	TSharedRef<FJsonObject> ArgumentsProperty = Property(TEXT("object"), TEXT("The tool's own parameters, as describe_tool lists them."));
	ArgumentsProperty->SetBoolField(TEXT("additionalProperties"), true);
	CallProperties->SetObjectField(TEXT("arguments"), ArgumentsProperty);

	TArray<TSharedPtr<FJsonValue>> Tools;
	Tools.Add(MakeTool(FindTools, FString::Printf(
		TEXT("Searches this server's %d Unreal Editor tools (%d domains) and returns the best matches: name, domain, parameters ")
		TEXT("(* = required) and a one-line summary. With no arguments it lists the domains. Then describe_tool gives a tool's ")
		TEXT("full schema and call_tool runs it."), Catalog.NumTools(), Domains.Num()),
		FindProperties, {}, /*bReadOnly*/ true));
	Tools.Add(MakeTool(DescribeTool,
		TEXT("One tool's full description and input JSON schema: every parameter's type and meaning, the required ones, closed ")
		TEXT("value sets and accepted aliases. Read it before call_tool unless the parameter list from find_tools is enough."),
		DescribeProperties, { TEXT("name") }, /*bReadOnly*/ true));
	Tools.Add(MakeTool(CallTool,
		TEXT("Runs one of this server's tools by name, with `arguments` holding that tool's own parameters. The call goes through ")
		TEXT("the same validation, guards and readback as a tool listed directly."),
		CallProperties, { TEXT("name") }, /*bReadOnly*/ false));

	TSharedRef<FJsonObject> ListResult = MakeShared<FJsonObject>();
	ListResult->SetArrayField(TEXT("tools"), Tools);
	FString Serialized;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	FJsonSerializer::Serialize(ListResult, Writer);
	const FTCHARToUTF8 Utf8(*Serialized);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Bytes;
}

FString McpMetaTools::Instructions(const FMcpToolCatalog& Catalog)
{
	return FString::Printf(
		TEXT("This connection lists three meta-tools instead of the %d Unreal Editor tools: find_tools searches them, describe_tool ")
		TEXT("returns one tool's input schema and call_tool runs it. Search before guessing a tool name."), Catalog.NumTools());
}

TSharedPtr<FJsonObject> McpMetaTools::Answer(const FMcpToolCatalog& Catalog, const FString& ToolName, const TSharedPtr<FJsonObject>& Args, bool& bOutIsError)
{
	using namespace McpMetaToolsPrivate;

	bOutIsError = false;
	if (ToolName.Equals(FindTools, ESearchCase::IgnoreCase))
	{
		return AnswerFindTools(Catalog, Args, bOutIsError);
	}
	if (ToolName.Equals(DescribeTool, ESearchCase::IgnoreCase))
	{
		return AnswerDescribeTool(Catalog, Args, bOutIsError);
	}
	return nullptr;
}

bool McpMetaTools::UnwrapCall(const TSharedPtr<FJsonObject>& Args, FString& OutToolName, TSharedPtr<FJsonObject>& OutArguments, FString& OutError)
{
	OutToolName.Reset();
	OutArguments = MakeShared<FJsonObject>();
	if (Args.IsValid())
	{
		Args->TryGetStringField(TEXT("name"), OutToolName);
	}
	OutToolName.TrimStartAndEndInline();
	if (OutToolName.IsEmpty())
	{
		OutError = TEXT("call_tool: missing required parameter 'name' (the tool to run; find_tools searches them)");
		return false;
	}
	if (OutToolName.Equals(CallTool, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("call_tool: `name` is the tool to run, not call_tool itself");
		return false;
	}

	const TSharedPtr<FJsonValue> Inner = Args->TryGetField(TEXT("arguments"));
	TSharedPtr<FJsonObject> InnerObject;
	if (Inner.IsValid() && Inner->Type == EJson::String)
	{
		// Some models send the arguments as a JSON string instead of an object.
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Inner->AsString()), InnerObject);
	}
	else if (Inner.IsValid() && Inner->Type == EJson::Object)
	{
		InnerObject = Inner->AsObject();
	}
	if (Inner.IsValid() && !Inner->IsNull() && !InnerObject.IsValid())
	{
		OutError = FString::Printf(TEXT("call_tool: `arguments` must be an object with %s's parameters"), *OutToolName);
		return false;
	}
	if (InnerObject.IsValid())
	{
		// The same rule as a direct tools/call: a null argument is an absent one.
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : InnerObject->Values)
		{
			if (Field.Value.IsValid() && !Field.Value->IsNull())
			{
				OutArguments->SetField(Field.Key, Field.Value);
			}
		}
	}
	return true;
}

TArray<FString> McpMetaTools::SuggestNames(const FMcpToolCatalog& Catalog, const FString& UnknownName, int32 MaxNames)
{
	using namespace McpMetaToolsPrivate;

	TArray<FString> Names;
	const FString Wanted = UnknownName.ToLower();
	if (Wanted.IsEmpty() || MaxNames <= 0)
	{
		return Names;
	}

	// Near spellings first: a typo or a wrong separator is a few edits away.
	const int32 MaxEdits = FMath::Clamp(Wanted.Len() / 4, 1, 3);
	TArray<TPair<int32, const FString*>> Near;
	for (const FMcpToolCatalog::FTool& Tool : Catalog.GetTools())
	{
		if (FMath::Abs(Tool.Name.Len() - Wanted.Len()) > MaxEdits)
		{
			continue;
		}
		const int32 Edits = Algo::LevenshteinDistance(Tool.Name.ToLower(), Wanted);
		if (Edits <= MaxEdits)
		{
			Near.Emplace(Edits, &Tool.Name);
		}
	}
	Near.Sort([](const TPair<int32, const FString*>& A, const TPair<int32, const FString*>& B)
	{
		return A.Key != B.Key ? A.Key < B.Key : A.Value->Compare(*B.Value, ESearchCase::IgnoreCase) < 0;
	});
	for (const TPair<int32, const FString*>& Candidate : Near)
	{
		if (Names.Num() >= MaxNames)
		{
			return Names;
		}
		Names.Add(*Candidate.Value);
	}

	// Then the names that share the most words with it.
	const TArray<FString> Words = QueryWords(UnknownName);
	for (const FHit& Hit : Words.Num() > 0 ? Search(Catalog, Words, FString()) : TArray<FHit>())
	{
		if (Names.Num() >= MaxNames)
		{
			break;
		}
		Names.AddUnique(Hit.Tool->Name);
	}
	return Names;
}
