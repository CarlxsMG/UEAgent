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
const TCHAR* const McpMetaTools::EnableToolset = TEXT("enable_toolset");

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
				const FString Description = Catalog.GetDomainDescription(D.Name);
				if (!Description.IsEmpty())
				{
					Entry->SetStringField(TEXT("description"), Description);
				}
				Entry->SetNumberField(TEXT("tools"), D.NumTools);
				DomainsJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			TArray<TSharedPtr<FJsonValue>> PacksJson;
			for (const FMcpToolCatalog::FPack& Pack : Catalog.GetPacks())
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("pack"), Pack.Name);
				Entry->SetStringField(TEXT("description"), Pack.Description);
				Entry->SetArrayField(TEXT("tools"), ToJsonStrings(Pack.ToolNames));
				PacksJson.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Result->SetNumberField(TEXT("total_tools"), Catalog.NumTools());
			Result->SetArrayField(TEXT("domains"), DomainsJson);
			if (PacksJson.Num() > 0)
			{
				Result->SetArrayField(TEXT("packs"), PacksJson);
			}
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
		FString RetiredNote;
		const FMcpToolCatalog::FTool* Tool = Catalog.FindTool(Catalog.ResolveName(Name, &RetiredNote));
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
		if (!RetiredNote.IsEmpty())
		{
			Result->SetStringField(TEXT("retired_name"), RetiredNote);
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

	/** A meta-tool's MCP annotations, all four stated. */
	struct FHints
	{
		bool bReadOnly = false;
		bool bDestructive = false;
		bool bIdempotent = false;
		bool bOpenWorld = false;
	};

	/** find_tools and describe_tool only read the catalog, and enable_toolset only this connection's list: a client may run
	 *  them without asking. call_tool does whatever the tool it names does, so it carries the worst of any tool: it can
	 *  delete, and execute_python_file reaches outside the editor. */
	constexpr FHints ReadsCatalog{ /*bReadOnly*/ true, /*bDestructive*/ false, /*bIdempotent*/ true, /*bOpenWorld*/ false };
	constexpr FHints RunsAnyTool{ /*bReadOnly*/ false, /*bDestructive*/ true, /*bIdempotent*/ false, /*bOpenWorld*/ true };

	static TSharedPtr<FJsonValue> MakeTool(const TCHAR* Name, const FString& Description, const TSharedRef<FJsonObject>& Properties,
		const TArray<FString>& Required, const FHints& Hints)
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
		TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetBoolField(TEXT("readOnlyHint"), Hints.bReadOnly);
		Annotations->SetBoolField(TEXT("destructiveHint"), Hints.bDestructive);
		Annotations->SetBoolField(TEXT("idempotentHint"), Hints.bIdempotent);
		Annotations->SetBoolField(TEXT("openWorldHint"), Hints.bOpenWorld);
		Tool->SetObjectField(TEXT("annotations"), Annotations);
		return MakeShared<FJsonValueObject>(Tool);
	}

	/** "actors (get_actors_by_class, spawn_actor), levels (load_level)": each pack with its tools. */
	static FString PacksLine(const FMcpToolCatalog& Catalog)
	{
		TArray<FString> Parts;
		for (const FMcpToolCatalog::FPack& Pack : Catalog.GetPacks())
		{
			Parts.Add(FString::Printf(TEXT("%s (%s)"), *Pack.Name, *FString::Join(Pack.ToolNames, TEXT(", "))));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	static TArray<FString> PackNames(const FMcpToolCatalog& Catalog)
	{
		TArray<FString> Names;
		for (const FMcpToolCatalog::FPack& Pack : Catalog.GetPacks())
		{
			Names.Add(Pack.Name);
		}
		return Names;
	}

	/** Reads one toolset argument (an array of names, or one string with names separated by commas) into Requested. */
	static void ReadNames(const TSharedPtr<FJsonObject>& Args, const TCHAR* Field, TArray<FString>& Requested)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (Args->TryGetArrayField(Field, Values) && Values)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					Requested.Add(Name);
				}
			}
		}
		FString Single;
		if (Args->TryGetStringField(Field, Single))
		{
			// One name as a string, or several separated by commas: a model sends either.
			TArray<FString> Parts;
			Single.ParseIntoArray(Parts, TEXT(","));
			Requested.Append(Parts);
		}
	}
}

TArray<TSharedPtr<FJsonValue>> McpMetaTools::BuildMetaTools(const FMcpToolCatalog& Catalog, EList List)
{
	using namespace McpMetaToolsPrivate;

	const TArray<FDomainCount> Domains = CountDomains(Catalog);
	TArray<FString> DomainNames;
	for (const FDomainCount& D : Domains)
	{
		DomainNames.Add(D.Name);
	}
	const bool bCore = List == EList::Core;

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
		TEXT("Searches this server's %d Unreal Editor tools and returns the best matches, each with its name, domain, parameters ")
		TEXT("(* marks a required one) and the first sentence of its description. Use it before guessing a tool name or writing ")
		TEXT("Python; when you already know the name, skip it: describe_tool and call_tool take the name directly.\n\n")
		TEXT("Each query word scores on a whole word of a tool's name first, then the start of one, then the domain, then the ")
		TEXT("description, and a tool that matches more of the words ranks higher; a query that spells a tool's name, such as ")
		TEXT("\"spawn actor\", puts that tool first. With `domain` the search stays inside it, and `domain` alone lists its tools. ")
		TEXT("With no arguments it lists the %d domains, each with what it covers and its size%s. It only reads the catalog, so it is cheap to repeat ")
		TEXT("with other words.\n\n")
		TEXT("Then describe_tool gives a match's full schema and call_tool runs it%s.\n\n")
		TEXT("Returns: { success, matches: int, tools: [{ name, domain, params?, summary }], more?: string (when matches exceed ")
		TEXT("`limit`) } -- with no arguments { success, total_tools, domains: [{ domain, description, tools }]%s }. An unknown `domain` is an ")
		TEXT("error that lists the domains."),
		Catalog.NumTools(), Domains.Num(), bCore ? TEXT(", and the packs with their tools") : TEXT(""),
		bCore ? TEXT(", or enable_toolset adds its pack or domain to your tool list") : TEXT(""),
		bCore ? TEXT(", packs: [{ pack, description, tools }]") : TEXT("")),
		FindProperties, {}, ReadsCatalog));
	Tools.Add(MakeTool(DescribeTool,
		TEXT("Returns one tool's full description, input JSON schema and annotations, as a full tool list carries them: every ")
		TEXT("parameter's type and meaning, the required ones, closed value sets and accepted aliases, and whether the tool only ")
		TEXT("reads or can delete. Use it before call_tool when the parameter line from find_tools does not tell you how to fill ")
		TEXT("an argument, such as its format or its allowed values; a tool already in your list has its schema there.\n\n")
		TEXT("An old name of a renamed tool, or the name of a retired one, also resolves, and `retired_name` then says what the ")
		TEXT("name became. It only reads the catalog. An unknown name is an error that lists up to five `closest` names.\n\n")
		TEXT("Returns: { success, name, domain, description, inputSchema, annotations?, retired_name? }"),
		DescribeProperties, { TEXT("name") }, ReadsCatalog));
	Tools.Add(MakeTool(CallTool,
		TEXT("Runs any of this server's tools by name: `name` is the tool and `arguments` its own parameters, e.g. ")
		TEXT("{\"name\": \"get_actors_by_class\", \"arguments\": {\"class_name\": \"PointLight\"}}. Use it for a tool that is not ")
		TEXT("in your tool list, once find_tools has found it (and describe_tool, when its parameters are unclear); call a listed ")
		TEXT("tool directly.\n\n")
		TEXT("The call takes the path of a direct one: the same validation, guards and readback, and the same effects, so it can ")
		TEXT("delete or overwrite whatever the named tool can; describe_tool shows that tool's annotations. Refused before ")
		TEXT("anything runs: no `name`, `name` set to call_tool itself, `arguments` that is not an object (a JSON string of one ")
		TEXT("is accepted), an unknown name (the error lists the closest ones) and a missing required parameter. An old name of a ")
		TEXT("renamed tool runs the new one, and the reply says so.\n\n")
		TEXT("Returns: the named tool's own reply, unchanged."),
		CallProperties, { TEXT("name") }, RunsAnyTool));
	if (bCore)
	{
		TSharedRef<FJsonObject> EnableProperties = MakeShared<FJsonObject>();
		auto NamesProperty = [](const FString& Description, const TArray<FString>& Names)
		{
			TSharedRef<FJsonObject> Schema = Property(TEXT("array"), Description);
			TSharedRef<FJsonObject> Items = MakeShared<FJsonObject>();
			Items->SetStringField(TEXT("type"), TEXT("string"));
			Items->SetArrayField(TEXT("enum"), ToJsonStrings(Names));
			Schema->SetObjectField(TEXT("items"), Items);
			return Schema;
		};
		EnableProperties->SetObjectField(TEXT("packs"), NamesProperty(
			TEXT("The packs to add, e.g. [\"actors\"] or [\"play\", \"levels\"]."), PackNames(Catalog)));
		EnableProperties->SetObjectField(TEXT("domains"), NamesProperty(
			TEXT("The whole domains to add, as find_tools lists them, e.g. [\"Material\"]."), DomainNames));
		Tools.Add(MakeTool(EnableToolset, FString::Printf(
			TEXT("Adds packs or whole domains to this connection's tool list, so their tools are called directly instead of ")
			TEXT("through call_tool; the %d core tools are always listed. Give `packs`, `domains` or both.\n\n")
			TEXT("A pack is the few tools of one job the core leaves out: %s. A domain is every tool of one area of the editor, ")
			TEXT("such as Material or Animation, and can add a hundred tools to your context: find_tools with no arguments lists ")
			TEXT("the domains and their sizes. Prefer a pack when one covers the job.\n\n")
			TEXT("It changes only which tools this connection lists, never the editor or the project, and lasts until the ")
			TEXT("connection's session ends; enabling the same one again adds nothing. The server announces the new list and your ")
			TEXT("client re-reads it; if your client does not, call_tool reaches the same tools. An unknown name is an error that ")
			TEXT("lists the packs and the domains.\n\n")
			TEXT("Returns: { success, packs?: [{ pack, already_listed, tools }], domains?: [{ domain, already_listed, tools }], ")
			TEXT("tools_added: int, list_changed: bool, note?: string }"),
			Catalog.NumCoreTools(), *PacksLine(Catalog)),
			EnableProperties, {}, ReadsCatalog));
	}
	return Tools;
}

TArray<uint8> McpMetaTools::BuildToolsListUtf8(const FMcpToolCatalog& Catalog)
{
	TSharedRef<FJsonObject> ListResult = MakeShared<FJsonObject>();
	ListResult->SetArrayField(TEXT("tools"), BuildMetaTools(Catalog, EList::Compact));
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
		TEXT("returns one tool's input schema and call_tool runs it. Search before guessing a tool name, and before writing ")
		TEXT("Python: execute_python is for what no tool does."), Catalog.NumTools());
}

FString McpMetaTools::CoreInstructions(const FMcpToolCatalog& Catalog)
{
	using namespace McpMetaToolsPrivate;

	return FString::Printf(
		TEXT("This connection lists %d core tools of the %d Unreal Editor tools -- find, read, change, save and undo any asset ")
		TEXT("or object, edit Blueprint graphs, and Python -- and four meta-tools for the rest: find_tools searches every tool, ")
		TEXT("describe_tool returns one's input schema, call_tool runs any of them, and enable_toolset adds a pack (%s) or a ")
		TEXT("whole domain to this list. Search before guessing a tool name, and before writing Python: most tools are not in ")
		TEXT("this list, and execute_python is for what no tool does."),
		Catalog.NumCoreTools(), Catalog.NumTools(), *FString::Join(PackNames(Catalog), TEXT(", ")));
}

bool McpMetaTools::ReadToolsets(const FMcpToolCatalog& Catalog, const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutToolsets, FString& OutError)
{
	using namespace McpMetaToolsPrivate;

	OutToolsets.Reset();
	TArray<FString> Requested;
	if (Args.IsValid())
	{
		// A name is looked up as a pack and as a domain whichever field holds it: no pack is named like a domain.
		static const TCHAR* const Fields[] = { TEXT("packs"), TEXT("pack"), TEXT("domains"), TEXT("domain") };
		for (const TCHAR* Field : Fields)
		{
			ReadNames(Args, Field, Requested);
		}
	}
	TArray<FString> Unknown;
	for (const FString& Name : Requested)
	{
		const FString Toolset = Catalog.FindToolset(Name);
		if (Toolset.IsEmpty())
		{
			Unknown.Add(Name.TrimStartAndEnd());
		}
		else
		{
			OutToolsets.AddUnique(Toolset);
		}
	}
	if (Unknown.Num() > 0 || OutToolsets.Num() == 0)
	{
		OutError = FString::Printf(TEXT("enable_toolset: %s. The packs are: %s. The domains are: %s"),
			Unknown.Num() > 0 ? *FString::Printf(TEXT("unknown pack or domain %s"), *FString::Join(Unknown, TEXT(", ")))
				: TEXT("`packs` and `domains` name nothing"),
			*FString::Join(PackNames(Catalog), TEXT(", ")), *FString::Join(Catalog.GetDomains(), TEXT(", ")));
		OutToolsets.Reset();
		return false;
	}
	return true;
}

TSharedRef<FJsonObject> McpMetaTools::ToolsetReply(const FMcpToolCatalog& Catalog, const TArray<FString>& Toolsets, const TSet<FString>& EnabledBefore, bool bHasSession)
{
	using namespace McpMetaToolsPrivate;

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	TArray<TSharedPtr<FJsonValue>> PacksJson;
	TArray<TSharedPtr<FJsonValue>> DomainsJson;
	for (const FString& Toolset : Toolsets)
	{
		const FMcpToolCatalog::FPack* Pack = Catalog.FindPack(Toolset);
		TArray<FString> Names;
		for (const FMcpToolCatalog::FTool& Tool : Catalog.GetTools())
		{
			if (!Tool.bCore && (Pack ? Tool.Pack == Pack->Name : Tool.Domain == Toolset))
			{
				Names.Add(Tool.Name);
			}
		}
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(Pack ? TEXT("pack") : TEXT("domain"), Toolset);
		Entry->SetBoolField(TEXT("already_listed"), EnabledBefore.Contains(Toolset));
		Entry->SetArrayField(TEXT("tools"), ToJsonStrings(Names));
		(Pack ? PacksJson : DomainsJson).Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (PacksJson.Num() > 0)
	{
		Result->SetArrayField(TEXT("packs"), PacksJson);
	}
	if (DomainsJson.Num() > 0)
	{
		Result->SetArrayField(TEXT("domains"), DomainsJson);
	}
	// What the list gains: a pack's tool already listed by an enabled domain (or the other way round) counts once.
	TSet<FString> EnabledAfter = EnabledBefore;
	for (const FString& Toolset : Toolsets)
	{
		EnabledAfter.Add(Toolset);
	}
	int32 Added = 0;
	for (const FMcpToolCatalog::FTool& Tool : Catalog.GetTools())
	{
		Added += FMcpToolCatalog::IsInCoreList(Tool, EnabledAfter) && !FMcpToolCatalog::IsInCoreList(Tool, EnabledBefore) ? 1 : 0;
	}
	Result->SetNumberField(TEXT("tools_added"), bHasSession ? Added : 0);
	Result->SetBoolField(TEXT("list_changed"), bHasSession && Added > 0);
	if (!bHasSession)
	{
		Result->SetStringField(TEXT("note"), TEXT("This connection sent no Mcp-Session-Id, so its tool list cannot change: call these tools ")
			TEXT("with call_tool, or add ?domains=<pack or domain> to the server URL to list them from the start."));
	}
	else if (Added > 0)
	{
		Result->SetStringField(TEXT("note"), TEXT("The tools are in this connection's list from now on; if your client does not show them, ")
			TEXT("it did not re-read the list: call them with call_tool."));
	}
	return Result;
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
