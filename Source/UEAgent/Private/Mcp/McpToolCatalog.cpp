// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpToolCatalog.h"

#include "Mcp/McpMetaTools.h"

#include "McpPieSafety.gen.h"
#include "McpParamAliases.gen.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Algo/AllOf.h"

// Namespace name unique per file: the module builds as unity, and two same-named helpers in
// anonymous namespaces of different files collide in the same blob (see Build.cs).
namespace McpToolCatalogPrivate
{
	static FCriticalSection GCurrentLock;
	static TSharedPtr<const FMcpToolCatalog> GCurrent;

	// Description of the `force` escape hatch, added to the schema of every PIE-unsafe command.
	static const TCHAR* const ForceDescription =
		TEXT("Optional. This command is REFUSED while a Play-in-Editor session is running: it either compiles a Blueprint ")
		TEXT("(the reinstancer can retain a world, and CheckForWorldGCLeaks is Fatal -- it KILLS the editor) or writes to disk ")
		TEXT("(Editor.AllowSavingAssetsDuringPIE=0 makes the engine refuse the write, so it would report success without writing ")
		TEXT("a byte). Stop PIE and retry instead; force=true runs it anyway, at your own risk.");

	// Prefix for rotation parameters (prepended to their description). The three rotation
	// notations that meet here use three different orders and none of them warns about it:
	// the wire array is [Pitch, Yaw, Roll].
	static const TCHAR* const RotationPrefix =
		TEXT("PREFERRED: the OBJECT {\"roll\":R,\"pitch\":P,\"yaw\":Y} (any subset, absent axes are 0). The ARRAY form below ")
		TEXT("is [pitch, yaw, roll] -- its index 2 is ROLL, NOT yaw, which once put a PlayerStart on its side. ");

	static TSharedPtr<FJsonObject> MakeTypeSchema(const FString& JsonType)
	{
		auto Typed = [](const TCHAR* Type)
		{
			TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("type"), Type);
			return S;
		};
		auto AnyObject = [&Typed]()
		{
			TSharedPtr<FJsonObject> S = Typed(TEXT("object"));
			S->SetBoolField(TEXT("additionalProperties"), true);
			return S;
		};

		if (JsonType == TEXT("string"))  { return Typed(TEXT("string")); }
		if (JsonType == TEXT("number"))  { return Typed(TEXT("number")); }
		if (JsonType == TEXT("integer")) { return Typed(TEXT("integer")); }
		if (JsonType == TEXT("boolean")) { return Typed(TEXT("boolean")); }
		if (JsonType == TEXT("array"))   { return Typed(TEXT("array")); }
		if (JsonType == TEXT("object"))  { return AnyObject(); }

		// "any": the handler forwards the JsonValue as is. The six JSON types are written as
		// TYPED branches rather than a `{}`: an empty branch leaves the client that generates the
		// call without a schema, and its JSON comes out broken ("input JSON failed to parse", seen
		// repeatedly on set_blueprint_component_array_element).
		TArray<TSharedPtr<FJsonValue>> Branches;
		for (const TCHAR* Type : { TEXT("string"), TEXT("number"), TEXT("integer"), TEXT("boolean"), TEXT("array") })
		{
			Branches.Add(MakeShared<FJsonValueObject>(Typed(Type)));
		}
		Branches.Add(MakeShared<FJsonValueObject>(AnyObject()));
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetArrayField(TEXT("anyOf"), Branches);
		return S;
	}

	static TSharedPtr<FJsonObject> MakeRotationSchema()
	{
		TSharedPtr<FJsonObject> Axes = MakeShared<FJsonObject>();
		for (const TCHAR* Axis : { TEXT("roll"), TEXT("pitch"), TEXT("yaw") })
		{
			TSharedPtr<FJsonObject> Num = MakeShared<FJsonObject>();
			Num->SetStringField(TEXT("type"), TEXT("number"));
			Axes->SetObjectField(Axis, Num);
		}
		TSharedPtr<FJsonObject> AsObject = MakeShared<FJsonObject>();
		AsObject->SetStringField(TEXT("type"), TEXT("object"));
		AsObject->SetObjectField(TEXT("properties"), Axes);
		AsObject->SetBoolField(TEXT("additionalProperties"), false);

		TSharedPtr<FJsonObject> AsArray = MakeShared<FJsonObject>();
		AsArray->SetStringField(TEXT("type"), TEXT("array"));

		TArray<TSharedPtr<FJsonValue>> Branches;
		Branches.Add(MakeShared<FJsonValueObject>(AsObject));
		Branches.Add(MakeShared<FJsonValueObject>(AsArray));
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetArrayField(TEXT("anyOf"), Branches);
		return S;
	}

	/** Deep copy (an FJsonObject shared between two properties would serialize fine, but
	 *  adding a `description` to one would also add it to the other). */
	static TSharedPtr<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Source)
	{
		FString Text;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Source.ToSharedRef(), Writer);
		TSharedPtr<FJsonObject> Copy;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Copy);
		return Copy;
	}

	/** All matches of capture group Group (or of the whole match if Group = 0). */
	static TArray<FString> FindAll(const FRegexPattern& Pattern, const FString& Text, int32 Group)
	{
		TArray<FString> Out;
		FRegexMatcher Matcher(Pattern, Text);
		while (Matcher.FindNext())
		{
			Out.Add(Matcher.GetCaptureGroup(Group));
		}
		return Out;
	}

	/**
	 * Closed set of values found in a description, or empty if it is not PROVABLY an enum. A
	 * false enum turns a call that works today into a hard validation error, so when in doubt
	 * the parameter stays a plain string.
	 */
	static TArray<FString> ExtractEnumValues(const FString& Desc)
	{
		const TArray<FString> None;
		if (Desc.IsEmpty())
		{
			return None;
		}

		// Trigger words (case-insensitive)
		{
			const FRegexPattern Trigger(TEXT("\\bone of\\b|\\beither\\b|\\(default\\)|\\bor\\b"), ERegexPatternFlags::CaseInsensitive);
			FRegexMatcher Matcher(Trigger, Desc);
			if (!Matcher.FindNext())
			{
				return None;
			}
		}

		// Every quoted value must be a short lower-case token (anchored, case-sensitive)
		const FRegexPattern Quoted(TEXT("'([^']*)'"));
		const TArray<FString> QuotedValues = FindAll(Quoted, Desc, 1);
		if (QuotedValues.Num() == 0)
		{
			return None;
		}
		const FRegexPattern TokenOk(TEXT("^[a-z][a-z0-9_]{1,39}$"));
		for (const FString& Value : QuotedValues)
		{
			FRegexMatcher Matcher(TokenOk, Value);
			if (!Matcher.FindNext())
			{
				return None;
			}
		}

		TArray<FString> Values;
		for (const FString& Value : QuotedValues)
		{
			Values.AddUnique(Value);
		}
		if (Values.Num() < 2 || Values.Num() > 12)
		{
			return None;
		}

		// Alternatives listed WITHOUT quotes ("Filter to all, functions, or events"): there the
		// quoted values are a SUBSET of the valid ones, and pinning the enum would reject a good
		// one (e.g. a filter_type whose quotes named 'functions' and 'events' but left out 'all').
		FString Replaced;
		{
			FRegexMatcher Matcher(Quoted, Desc);
			int32 Cursor = 0;
			while (Matcher.FindNext())
			{
				Replaced += Desc.Mid(Cursor, Matcher.GetMatchBeginning() - Cursor);
				Replaced += TEXT("<Q>");
				Cursor = Matcher.GetMatchEnding();
			}
			Replaced += Desc.Mid(Cursor);
		}
		const FRegexPattern UnquotedAlts(
			TEXT("\\b(?:to|one of|either)\\s+((?:[a-z]+\\s*,\\s*)+(?:or\\s+)?[a-z]+)"), ERegexPatternFlags::CaseInsensitive);
		FRegexMatcher AltsMatcher(UnquotedAlts, Replaced);
		if (AltsMatcher.FindNext())
		{
			// Split the group on "," or the word "or" (this regex is case-SENSITIVE)
			const FString Group = AltsMatcher.GetCaptureGroup(1);
			const FRegexPattern Separator(TEXT(",|\\bor\\b"));
			FRegexMatcher SepMatcher(Separator, Group);
			int32 Cursor = 0;
			TArray<FString> Words;
			while (SepMatcher.FindNext())
			{
				Words.Add(Group.Mid(Cursor, SepMatcher.GetMatchBeginning() - Cursor));
				Cursor = SepMatcher.GetMatchEnding();
			}
			Words.Add(Group.Mid(Cursor));
			for (FString& Word : Words)
			{
				Word.TrimStartAndEndInline();
				if (!Word.IsEmpty() && !Values.Contains(Word))
				{
					return None;
				}
			}
		}
		return Values;
	}

	static bool IsNullOrMissing(const TSharedPtr<FJsonObject>& Args, const FString& Key)
	{
		if (!Args.IsValid())
		{
			return true;
		}
		const TSharedPtr<FJsonValue> Value = Args->TryGetField(Key);
		return !Value.IsValid() || Value->IsNull();
	}

	/** How a refusal of a vector parameter names its shape (FMcpToolCatalog::CheckVectorShapes). */
	struct FVectorShapeText
	{
		/** Numbers in the array form. */
		int32 Count;
		/** What the parameter should have been. */
		const TCHAR* Expected;
		/** The form the reader takes. */
		const TCHAR* ReadAs;
		/** What the reader makes of most other values. */
		const TCHAR* Zero;
		/** The WHAT WORKS line. */
		const TCHAR* WhatWorks;
	};

	static const FVectorShapeText& VectorShapeText(FMcpToolCatalog::EVectorShape Shape)
	{
		static const FVectorShapeText Vector = { 3, TEXT("an array of 3 numbers"), TEXT("[X, Y, Z]"), TEXT("0, 0, 0"),
			TEXT("an array of 3 numbers, [X, Y, Z]") };
		static const FVectorShapeText Vector2D = { 2, TEXT("an array of 2 numbers"), TEXT("[X, Y]"), TEXT("0, 0"),
			TEXT("an array of 2 numbers, [X, Y]") };
		static const FVectorShapeText Rotator = { 3, TEXT("an array of 3 numbers or an object of roll, pitch and yaw"),
			TEXT("[Pitch, Yaw, Roll] or {\"roll\": R, \"pitch\": P, \"yaw\": Y}"), TEXT("a zero rotation"),
			TEXT("{\"roll\": R, \"pitch\": P, \"yaw\": Y} with any of the three axes, or an array of 3 numbers, [Pitch, Yaw, Roll]") };
		return Shape == FMcpToolCatalog::EVectorShape::Vector2D ? Vector2D
			: Shape == FMcpToolCatalog::EVectorShape::Rotator ? Rotator : Vector;
	}

	/** A JSON number the readers can use: finite (the JSON reader also parses NaN and Inf tokens as numbers) and, when
	 *  bFloat, within the float range, since the array form is narrowed through float and a larger number becomes
	 *  infinite. */
	static bool IsReadableNumber(const TSharedPtr<FJsonValue>& Value, bool bFloat)
	{
		return Value.IsValid() && Value->Type == EJson::Number && FMath::IsFinite(Value->AsNumber())
			&& (!bFloat || FMath::Abs(Value->AsNumber()) <= static_cast<double>(TNumericLimits<float>::Max()));
	}

	/** What a JSON value is, for a refusal message: "a string", "an array of 2 elements", "NaN"... */
	static FString DescribeJsonValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		switch (Value->Type)
		{
		case EJson::String:  return TEXT("a string");
		case EJson::Boolean: return TEXT("a boolean");
		case EJson::Object:  return TEXT("an object");
		case EJson::Null:    return TEXT("null");
		case EJson::Number:
		{
			const double Number = Value->AsNumber();
			return FMath::IsNaN(Number) ? TEXT("NaN") : !FMath::IsFinite(Number) ? TEXT("an infinite number")
				: FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()) ? TEXT("a number outside the float range")
				: TEXT("a number");
		}
		case EJson::Array:
		{
			const int32 Count = Value->AsArray().Num();
			return FString::Printf(TEXT("an array of %d element%s"), Count, Count == 1 ? TEXT("") : TEXT("s"));
		}
		default:             return TEXT("a value of no JSON type");
		}
	}

	/** The words of a key, in lower case: split at each '_' (keeping empty words, as the generator's split does) and at
	 *  each camelCase hump, before an upper-case letter that follows a lower-case letter or digit, or that starts a
	 *  word after a run of capitals ("newName" -> new, name; "HTTPServer" -> http, server). */
	static void SplitKeyWords(const FString& Key, TArray<FString>& OutWords)
	{
		OutWords.Reset();
		FString Word;
		for (int32 Index = 0; Index < Key.Len(); ++Index)
		{
			const TCHAR C = Key[Index];
			if (C == TEXT('_'))
			{
				OutWords.Add(Word.ToLower());
				Word.Reset();
				continue;
			}
			if (FChar::IsUpper(C) && !Word.IsEmpty())
			{
				const TCHAR Previous = Key[Index - 1];
				const bool bNextIsLower = Index + 1 < Key.Len() && FChar::IsLower(Key[Index + 1]);
				if (FChar::IsLower(Previous) || FChar::IsDigit(Previous) || (FChar::IsUpper(Previous) && bNextIsLower))
				{
					OutWords.Add(Word.ToLower());
					Word.Reset();
				}
			}
			Word.AppendChar(C);
		}
		OutWords.Add(Word.ToLower());
	}

	/** The length of the asset object path a content path names, as far as the call says: an object path
	 *  ("/Game/F/BP.BP", "/Game/F/BP.BP_C:Sub") up to the end of its asset, any other path as given (a package path,
	 *  or the folder a create tool makes the asset in, whose join with the name InvalidPackageName measures). */
	static int32 AssetObjectPathLength(const FString& Path)
	{
		int32 Dot = INDEX_NONE;
		if (!Path.FindChar(TEXT('.'), Dot))
		{
			return Path.Len();
		}
		int32 End = Dot + 1;
		while (End < Path.Len() && Path[End] != TEXT(':') && Path[End] != TEXT('.'))
		{
			++End;
		}
		return End;
	}

	/**
	 * The first string of NameLengthLimit characters or more inside Value whose nearest object key is a name
	 * (bNameKey, FMcpToolCatalog::IsNameLikeKey), and where it sits: the walk of the double_slash_path precondition,
	 * at the same depth limit. A string reached from the top-level argument through arrays only is the param's own
	 * (FParam::bNameLike), so the walk starts with bNameKey false.
	 */
	static bool FindLongNestedName(const FMcpToolCatalog& Catalog, const FString& Domain, const TSharedPtr<FJsonValue>& Value,
		const FString& Where, bool bNameKey, int32 Depth, FString& OutWhere, int32& OutLength)
	{
		if (!Value.IsValid() || Depth > 16)
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			const int32 Length = Value->AsString().Len();
			if (bNameKey && Length >= FMcpToolCatalog::NameLengthLimit)
			{
				OutWhere = Where;
				OutLength = Length;
				return true;
			}
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (FindLongNestedName(Catalog, Domain, Items[Index], FString::Printf(TEXT("%s[%d]"), *Where, Index), bNameKey,
					Depth + 1, OutWhere, OutLength))
				{
					return true;
				}
			}
			return false;
		}
		if (Value->Type == EJson::Object && Value->AsObject().IsValid())
		{
			for (const auto& Field : Value->AsObject()->Values)
			{
				const FString Key(*Field.Key);
				if (FindLongNestedName(Catalog, Domain, Field.Value, Where + TEXT(".") + Key, Catalog.IsNameLikeKey(Key, Domain),
					Depth + 1, OutWhere, OutLength))
				{
					return true;
				}
			}
		}
		return false;
	}
}

const FMcpToolCatalog::FParam* FMcpToolCatalog::FTool::FindParam(const FString& ParamName) const
{
	return Params.FindByPredicate([&ParamName](const FParam& P) { return P.Name.Equals(ParamName, ESearchCase::IgnoreCase); });
}

FString FMcpToolCatalog::FTool::PublishedDescription() const
{
	return Returns.IsEmpty() ? Description : Description + TEXT("\n\nReturns: ") + Returns;
}

FString FMcpToolCatalog::DefaultCatalogPath()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAgent"));
	const FString BaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("UEAgent"));
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(BaseDir, TEXT("Resources"), TEXT("command_catalog.json")));
}

TSharedPtr<const FMcpToolCatalog> FMcpToolCatalog::GetCurrent()
{
	FScopeLock Lock(&McpToolCatalogPrivate::GCurrentLock);
	return McpToolCatalogPrivate::GCurrent;
}

void FMcpToolCatalog::SetCurrent(TSharedPtr<const FMcpToolCatalog> NewCatalog)
{
	FScopeLock Lock(&McpToolCatalogPrivate::GCurrentLock);
	McpToolCatalogPrivate::GCurrent = MoveTemp(NewCatalog);
}

const FMcpToolCatalog::FTool* FMcpToolCatalog::FindTool(const FString& ToolName) const
{
	if (const int32* Index = IndexByName.Find(ToolName))
	{
		return &Tools[*Index];
	}
	const int32* RetiredIndex = RetiredIndexByName.Find(ToolName);
	return RetiredIndex ? &RetiredTools[*RetiredIndex] : nullptr;
}

FString FMcpToolCatalog::ResolveCallName(const FString& Name, FString* OutNote)
{
	const TSharedPtr<const FMcpToolCatalog> Catalog = GetCurrent();
	if (!Catalog.IsValid())
	{
		if (OutNote)
		{
			OutNote->Reset();
		}
		return Name;
	}
	return Catalog->ResolveName(Name, OutNote);
}

FString FMcpToolCatalog::ResolveName(const FString& Name, FString* OutNote) const
{
	if (OutNote)
	{
		OutNote->Reset();
	}
	if (const FString* NewName = RenamedTools.Find(Name))
	{
		if (OutNote)
		{
			*OutNote = FString::Printf(
				TEXT("%s is the old name of %s, which ran this call. Call it %s: the old name is not in the tool list and stops working in UEAgent 3.0."),
				*Name, **NewName, **NewName);
		}
		return *NewName;
	}
	if (OutNote)
	{
		if (const int32* Index = RetiredIndexByName.Find(Name))
		{
			const FTool& Tool = RetiredTools[*Index];
			*OutNote = FString::Printf(
				TEXT("%s is retired: it is not in the tool list and stops working in UEAgent 3.0. %s does the same%s%s"),
				*Tool.Name, *Tool.RetiredFor, Tool.RetiredHow.IsEmpty() ? TEXT(".") : TEXT(": "), *Tool.RetiredHow);
		}
	}
	return Name;
}

bool FMcpToolCatalog::LooksLikeRotationName(const FString& ParamName)
{
	const FString Low = ParamName.ToLower();
	return Low == TEXT("rot") || Low.EndsWith(TEXT("rotation")) || Low.EndsWith(TEXT("rotator"));
}

bool FMcpToolCatalog::IsNameLikeKey(const FString& Key, const FString& Domain) const
{
	if (!bHasNameRule)
	{
		return false;
	}
	// The generator's _is_name_like on a param name, without the description half (a nested key has none).
	const FString Low = Key.ToLower();
	TArray<FString> Words;
	McpToolCatalogPrivate::SplitKeyWords(Key, Words);
	auto IsNumber = [](const FString& Word) { return !Word.IsEmpty() && Algo::AllOf(Word, [](TCHAR C) { return FChar::IsDigit(C); }); };
	while (Words.Num() > 1 && (IsNumber(Words.Last()) || Words.Last().Len() == 1))
	{
		Words.Pop();
	}
	if (Words.IsEmpty())
	{
		return false;
	}
	if (NotNameDomains.Contains(Domain) && !Words.Contains(NotNameDomainKeepWord))
	{
		return false;
	}
	for (const FString& Word : Words)
	{
		if (NameDiskWords.Contains(Word))
		{
			return false;
		}
	}
	for (const FString& Suffix : NameDiskSuffixes)
	{
		if (Low.EndsWith(Suffix))
		{
			return false;
		}
	}
	return NameWords.Contains(Words.Last());
}

TSharedPtr<const FMcpToolCatalog> FMcpToolCatalog::LoadFromFile(const FString& Path, FString& OutError)
{
	using namespace McpToolCatalogPrivate;

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		OutError = FString::Printf(TEXT("could not read the tool catalog '%s'"), *Path);
		return nullptr;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		OutError = FString::Printf(TEXT("the catalog '%s' is not valid JSON"), *Path);
		return nullptr;
	}
	const TArray<TSharedPtr<FJsonValue>>* Domains = nullptr;
	if (!Root->TryGetArrayField(TEXT("domains"), Domains) || !Domains)
	{
		OutError = FString::Printf(TEXT("the catalog '%s' has no 'domains'"), *Path);
		return nullptr;
	}

	TSharedRef<FMcpToolCatalog> Catalog = MakeShared<FMcpToolCatalog>();
	Catalog->SourcePath = Path;
	TArray<TSharedPtr<FJsonValue>> ToolsJson;

	// The generator's name-word rule, for the keys of objects inside arguments (IsNameLikeKey).
	const TSharedPtr<FJsonObject>* NameRule = nullptr;
	if (Root->TryGetObjectField(TEXT("nameLikeRule"), NameRule) && NameRule && NameRule->IsValid())
	{
		auto ReadWords = [&NameRule](const TCHAR* Field, auto& Out)
		{
			TArray<FString> Words;
			(*NameRule)->TryGetStringArrayField(Field, Words);
			for (const FString& Word : Words)
			{
				Out.Add(Word.ToLower());
			}
		};
		ReadWords(TEXT("nameWords"), Catalog->NameWords);
		ReadWords(TEXT("diskWords"), Catalog->NameDiskWords);
		ReadWords(TEXT("diskPathSuffixes"), Catalog->NameDiskSuffixes);
		ReadWords(TEXT("notNameDomains"), Catalog->NotNameDomains);
		(*NameRule)->TryGetStringField(TEXT("notNameDomainKeepWord"), Catalog->NotNameDomainKeepWord);
		Catalog->bHasNameRule = Catalog->NameWords.Num() > 0;
	}

	for (const TSharedPtr<FJsonValue>& DomainValue : *Domains)
	{
		const TSharedPtr<FJsonObject>* DomainObj = nullptr;
		if (!DomainValue.IsValid() || !DomainValue->TryGetObject(DomainObj))
		{
			continue;
		}
		const FString DomainName = (*DomainObj)->GetStringField(TEXT("domain"));
		const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
		if (!(*DomainObj)->TryGetArrayField(TEXT("commands"), Commands) || !Commands)
		{
			continue;
		}

		for (const TSharedPtr<FJsonValue>& CommandValue : *Commands)
		{
			const TSharedPtr<FJsonObject>* CommandObj = nullptr;
			if (!CommandValue.IsValid() || !CommandValue->TryGetObject(CommandObj))
			{
				continue;
			}

			FTool Tool;
			Tool.Domain = DomainName;
			(*CommandObj)->TryGetStringField(TEXT("name"), Tool.Name);
			(*CommandObj)->TryGetStringField(TEXT("description"), Tool.Description);
			if (Tool.Name.IsEmpty() || Catalog->IndexByName.Contains(Tool.Name) || Catalog->RetiredIndexByName.Contains(Tool.Name))
			{
				continue; // no name, or a duplicate: the first one wins, as in the catalog
			}
			const TSharedPtr<FJsonObject>* RetiredObj = nullptr;
			if ((*CommandObj)->TryGetObjectField(TEXT("retired"), RetiredObj) && RetiredObj && RetiredObj->IsValid())
			{
				(*RetiredObj)->TryGetStringField(TEXT("use"), Tool.RetiredFor);
				(*RetiredObj)->TryGetStringField(TEXT("how"), Tool.RetiredHow);
			}
			(*CommandObj)->TryGetStringField(TEXT("returns"), Tool.Returns);
			Tool.Returns.TrimStartAndEndInline();
			const TSharedPtr<FJsonObject>* AnnotationsObj = nullptr;
			if ((*CommandObj)->TryGetObjectField(TEXT("annotations"), AnnotationsObj) && AnnotationsObj && AnnotationsObj->IsValid())
			{
				Tool.Annotations = CloneObject(*AnnotationsObj);
			}

			const TArray<TSharedPtr<FJsonValue>>* ParamsJson = nullptr;
			if ((*CommandObj)->TryGetArrayField(TEXT("params"), ParamsJson) && ParamsJson)
			{
				for (const TSharedPtr<FJsonValue>& ParamValue : *ParamsJson)
				{
					const TSharedPtr<FJsonObject>* ParamObj = nullptr;
					if (!ParamValue.IsValid() || !ParamValue->TryGetObject(ParamObj))
					{
						continue;
					}
					FParam Param;
					(*ParamObj)->TryGetStringField(TEXT("name"), Param.Name);
					(*ParamObj)->TryGetStringField(TEXT("jsonType"), Param.JsonType);
					(*ParamObj)->TryGetBoolField(TEXT("required"), Param.bRequired);
					(*ParamObj)->TryGetStringField(TEXT("description"), Param.Description);
					(*ParamObj)->TryGetStringField(TEXT("aliasOf"), Param.AliasOf);
					const TArray<TSharedPtr<FJsonValue>>* IntRange = nullptr;
					if ((*ParamObj)->TryGetArrayField(TEXT("intRange"), IntRange) && IntRange && IntRange->Num() == 2
						&& (*IntRange)[0].IsValid() && (*IntRange)[1].IsValid()
						&& (*IntRange)[0]->TryGetNumber(Param.IntRangeMin) && (*IntRange)[1]->TryGetNumber(Param.IntRangeMax))
					{
						Param.bHasIntRange = true;
					}
					(*ParamObj)->TryGetBoolField(TEXT("nameLike"), Param.bNameLike);
					FString VectorShape;
					if ((*ParamObj)->TryGetStringField(TEXT("vectorShape"), VectorShape))
					{
						Param.VectorShape = VectorShape == TEXT("vector") ? EVectorShape::Vector
							: VectorShape == TEXT("vector2d") ? EVectorShape::Vector2D
							: VectorShape == TEXT("rotator") ? EVectorShape::Rotator
							: EVectorShape::None;
					}
					if (Param.Name.IsEmpty() || Tool.FindParam(Param.Name))
					{
						continue;
					}
					if (Param.JsonType.IsEmpty())
					{
						Param.JsonType = TEXT("any");
					}
					if (Param.JsonType == TEXT("string"))
					{
						Param.EnumValues = ExtractEnumValues(Param.Description);
					}
					Param.bIsRotation = Param.JsonType == TEXT("array") && LooksLikeRotationName(Param.Name);
					Tool.Params.Add(MoveTemp(Param));
				}
			}

			// ---- schema ---------------------------------------------------------------
			TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			TMap<FString, TSharedPtr<FJsonObject>> PropertyByName;
			for (const FParam& Param : Tool.Params)
			{
				TSharedPtr<FJsonObject> Schema = Param.bIsRotation ? MakeRotationSchema() : MakeTypeSchema(Param.JsonType);
				FString Description = Param.Description;
				if (Param.bIsRotation && !Description.Contains(TEXT("\"roll\"")) && !Description.Contains(TEXT("\"pitch\"")))
				{
					Description = FString(RotationPrefix) + Description;
				}
				if (!Description.IsEmpty())
				{
					Schema->SetStringField(TEXT("description"), Description);
				}
				if (Param.EnumValues.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> EnumJson;
					for (const FString& Value : Param.EnumValues)
					{
						EnumJson.Add(MakeShared<FJsonValueString>(Value));
					}
					Schema->SetArrayField(TEXT("enum"), EnumJson);
				}
				Properties->SetObjectField(Param.Name, Schema);
				PropertyByName.Add(Param.Name, Schema);
			}

			// `force` on the PIE-unsafe commands: the bridge READS it (CheckPIESafetyGuard); the
			// schema is what lets the client SEND it.
			if (McpPieSafety::Find(FName(*Tool.Name)) && !Tool.FindParam(TEXT("force")))
			{
				TSharedPtr<FJsonObject> Schema = MakeTypeSchema(TEXT("boolean"));
				Schema->SetStringField(TEXT("description"), ForceDescription);
				Properties->SetObjectField(TEXT("force"), Schema);
			}

			// The handler's own spellings (`!TryGet(a) && !TryGet(b)`): the catalog publishes one
			// of them as required and marks the others `aliasOf` it. The handler accepts any of
			// them, so any of them satisfies the requirement.
			for (const FParam& Param : Tool.Params)
			{
				const FParam* CanonicalParam = Param.AliasOf.IsEmpty() ? nullptr : Tool.FindParam(Param.AliasOf);
				if (CanonicalParam && CanonicalParam != &Param && CanonicalParam->bRequired)
				{
					Tool.RelaxedRequired.FindOrAdd(CanonicalParam->Name).AddUnique(Param.Name);
				}
			}

			// Aliases: only when the alias is not already a parameter of the command (and the
			// canonical name is). The C++ dispatcher does the rewrite under its four conditions;
			// this only makes it REACHABLE from the schema.
			for (const McpParamAliases::FAlias& Rule : McpParamAliases::For(FName(*Tool.Name)))
			{
				const FString Alias = Rule.Alias.ToString();
				const FString Canonical = Rule.Canonical.ToString();
				const FParam* CanonicalParam = Tool.FindParam(Canonical);
				if (!CanonicalParam || Tool.FindParam(Alias) || Properties->HasField(Alias))
				{
					continue;
				}
				TSharedPtr<FJsonObject> Schema = CloneObject(PropertyByName.FindRef(CanonicalParam->Name));
				Schema->SetStringField(TEXT("description"), FString::Printf(
					TEXT("Alias of `%s` (same value, same meaning). Accepted because sibling tools spell this parameter differently; prefer `%s`."),
					*CanonicalParam->Name, *CanonicalParam->Name));
				Properties->SetObjectField(Alias, Schema);
				if (CanonicalParam->bRequired)
				{
					Tool.RelaxedRequired.FindOrAdd(CanonicalParam->Name).Add(Alias);
				}
			}

			TArray<TSharedPtr<FJsonValue>> Required;
			for (const FParam& Param : Tool.Params)
			{
				if (Param.bRequired && !Tool.RelaxedRequired.Contains(Param.Name))
				{
					Required.Add(MakeShared<FJsonValueString>(Param.Name));
				}
			}

			TSharedPtr<FJsonObject> InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
			InputSchema->SetObjectField(TEXT("properties"), Properties);
			if (Required.Num() > 0)
			{
				InputSchema->SetArrayField(TEXT("required"), Required);
			}

			TSharedPtr<FJsonObject> ToolJson = MakeShared<FJsonObject>();
			ToolJson->SetStringField(TEXT("name"), Tool.Name);
			ToolJson->SetStringField(TEXT("description"), Tool.PublishedDescription());
			ToolJson->SetObjectField(TEXT("inputSchema"), InputSchema);
			if (Tool.Annotations.IsValid())
			{
				ToolJson->SetObjectField(TEXT("annotations"), Tool.Annotations);
			}
			Tool.InputSchema = InputSchema;

			// A retired tool still runs, so its schema is built like any other, but only FindTool sees it.
			if (!Tool.RetiredFor.IsEmpty())
			{
				Catalog->RetiredIndexByName.Add(Tool.Name, Catalog->RetiredTools.Num());
				Catalog->RetiredTools.Add(MoveTemp(Tool));
				continue;
			}
			ToolsJson.Add(MakeShared<FJsonValueObject>(ToolJson));

			Catalog->TotalParams += Tool.Params.Num();
			Catalog->IndexByName.Add(Tool.Name, Catalog->Tools.Num());
			Catalog->Tools.Add(MoveTemp(Tool));
		}
	}

	if (Catalog->Tools.Num() == 0)
	{
		OutError = FString::Printf(TEXT("the catalog '%s' has no tools"), *Path);
		return nullptr;
	}

	// Old name -> new name. An old name that is still a tool, or a new name that is not one, is skipped: the call
	// would run something other than what the old name did, or nothing.
	const TSharedPtr<FJsonObject>* RenamedObj = nullptr;
	if (Root->TryGetObjectField(TEXT("renamed"), RenamedObj) && RenamedObj && RenamedObj->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*RenamedObj)->Values)
		{
			FString NewName;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(NewName) && !Catalog->FindTool(Pair.Key) && Catalog->FindTool(NewName))
			{
				Catalog->RenamedTools.Add(Pair.Key, NewName);
			}
		}
	}

	TSharedPtr<FJsonObject> ListResult = MakeShared<FJsonObject>();
	ListResult->SetArrayField(TEXT("tools"), ToolsJson);
	FString Serialized;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	FJsonSerializer::Serialize(ListResult.ToSharedRef(), Writer);
	const FTCHARToUTF8 Utf8(*Serialized);
	Catalog->ToolsListUtf8.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Catalog->CompactToolsListUtf8 = McpMetaTools::BuildToolsListUtf8(*Catalog);

	return Catalog;
}

FString FMcpToolCatalog::CheckRequired(const FTool& Tool, const TSharedPtr<FJsonObject>& Args) const
{
	using namespace McpToolCatalogPrivate;

	for (const FParam& Param : Tool.Params)
	{
		if (!Param.bRequired || !IsNullOrMissing(Args, Param.Name))
		{
			continue;
		}
		if (const TArray<FString>* Aliases = Tool.RelaxedRequired.Find(Param.Name))
		{
			bool bViaAlias = false;
			for (const FString& Alias : *Aliases)
			{
				bViaAlias |= !IsNullOrMissing(Args, Alias);
			}
			if (bViaAlias)
			{
				continue;
			}
			return FString::Printf(TEXT("%s: missing required parameter '%s' (accepted spellings: %s, %s)"),
				*Tool.Name, *Param.Name, *Param.Name, *FString::Join(*Aliases, TEXT(", ")));
		}
		return FString::Printf(TEXT("%s: missing required parameter '%s'"), *Tool.Name, *Param.Name);
	}
	return FString();
}

FString FMcpToolCatalog::CheckIntegerRanges(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return FString();
	}
	const TSharedPtr<const FMcpToolCatalog> Catalog = GetCurrent();
	const FTool* Tool = Catalog.IsValid() ? Catalog->FindTool(CommandType) : nullptr;
	if (!Tool)
	{
		return FString();
	}
	for (const FParam& Param : Tool->Params)
	{
		const TSharedPtr<FJsonValue> Value = Param.bHasIntRange ? Params->TryGetField(Param.Name) : nullptr;
		// Only a JSON number: that is what the schema asks for, and what TryGetNumberField(int32) range-checks.
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			continue;
		}
		const double Number = Value->AsNumber();
		// The engine accepts [Min, Max] and rounds half away from zero, so 2147483647.4 already fails its read.
		if (Number < Param.IntRangeMin || Number > Param.IntRangeMax)
		{
			return FString::Printf(
				TEXT("%s: %s is %s, outside the integer range the tool reads it into (%.0f to %.0f). Out of that range the value ")
				TEXT("cannot be read, and the tool would go on with its default instead. WHAT WORKS: a whole number from %.0f to %.0f."),
				*CommandType, *Param.Name, *FString::SanitizeFloat(Number, 0), Param.IntRangeMin, Param.IntRangeMax,
				Param.IntRangeMin, Param.IntRangeMax);
		}
	}
	return FString();
}

FString FMcpToolCatalog::CheckNameLengths(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	using namespace McpToolCatalogPrivate;

	if (!Params.IsValid())
	{
		return FString();
	}
	const TSharedPtr<const FMcpToolCatalog> Catalog = GetCurrent();
	const FTool* Tool = Catalog.IsValid() ? Catalog->FindTool(CommandType) : nullptr;
	if (!Tool)
	{
		return FString();
	}
	FString Where;
	int32 Length = 0;
	for (const FParam& Param : Tool->Params)
	{
		const TSharedPtr<FJsonValue> Value = Param.bNameLike ? Params->TryGetField(Param.Name) : nullptr;
		if (!Value.IsValid())
		{
			continue;
		}
		// The string itself, or each string element of an array: the handlers make names of both.
		if (Value->Type == EJson::String)
		{
			Where = Param.Name;
			Length = Value->AsString().Len();
		}
		else if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num() && Length < NameLengthLimit; ++Index)
			{
				if (Items[Index].IsValid() && Items[Index]->Type == EJson::String)
				{
					Where = FString::Printf(TEXT("%s[%d]"), *Param.Name, Index);
					Length = Items[Index]->AsString().Len();
				}
			}
		}
		if (Length >= NameLengthLimit)
		{
			break;
		}
	}
	// A name inside an object of an argument (a rename row, a node spec): the key says whether it is one. A batch's
	// items are checked when each is dispatched, against its own tool.
	if (Length < NameLengthLimit && CommandType != TEXT("execute_batch"))
	{
		for (const auto& Field : Params->Values)
		{
			const FString Key(*Field.Key);
			if (FindLongNestedName(*Catalog, Tool->Domain, Field.Value, Key, /*bNameKey=*/false, 1, Where, Length))
			{
				break;
			}
		}
	}
	// FName's checkf fires at NAME_SIZE characters (UnrealNames.cpp, FindOrStoreString), and the tool may add a prefix or
	// suffix of up to NameAffixMargin characters before it builds one.
	if (Length >= NameLengthLimit)
	{
		return FString::Printf(
			TEXT("%s: %s is %d characters long. An FName holds at most %d characters, and the engine aborts the editor when it builds ")
			TEXT("one from a longer string (UnrealNames.cpp, NAME_SIZE); a tool may add a prefix or suffix of up to %d characters to a ")
			TEXT("name before it builds one, so a name or path of %d characters or more is refused. Refused before the tool ran: ")
			TEXT("nothing was changed. WHAT WORKS: a name or path of at most %d characters."),
			*CommandType, *Where, Length, NAME_SIZE - 1, NameAffixMargin, NameLengthLimit, NameLengthLimit - 1);
	}
	if (CommandType == TEXT("execute_batch"))
	{
		return FString();
	}
	// A name the call gives to an object it creates inside an asset (a component, a widget, a graph, a state machine)
	// becomes part of that object's path, "<asset object path>_C:<name>_GEN_VARIABLE", and the transaction that records
	// the creation makes an FName of the whole path (FTransactionObjectId::SetObject): the asset's path and the name
	// can each fit and still join past NAME_SIZE.
	const FParam* PathParam = nullptr;
	int32 PathLength = 0;
	for (const FParam& Param : Tool->Params)
	{
		FString Text;
		if (Param.bNameLike && Params->TryGetStringField(Param.Name, Text) && Text.StartsWith(TEXT("/")))
		{
			const int32 ObjectPathLength = AssetObjectPathLength(Text);
			if (!PathParam || ObjectPathLength > PathLength)
			{
				PathParam = &Param;
				PathLength = ObjectPathLength;
			}
		}
	}
	const FParam* NameParam = nullptr;
	int32 NameLength = 0;
	for (const FParam& Param : Tool->Params)
	{
		const TSharedPtr<FJsonValue> Value = (PathParam && &Param != PathParam && Param.bNameLike) ? Params->TryGetField(Param.Name) : nullptr;
		if (!Value.IsValid())
		{
			continue;
		}
		int32 Longest = -1;
		if (Value->Type == EJson::String)
		{
			Longest = Value->AsString().Len();
		}
		else if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				if (Item.IsValid() && Item->Type == EJson::String)
				{
					Longest = FMath::Max(Longest, Item->AsString().Len());
				}
			}
		}
		if (Longest > NameLength || (!NameParam && Longest >= 0))
		{
			NameParam = &Param;
			NameLength = Longest;
		}
	}
	if (PathParam && NameParam && PathLength + NameLength + SubobjectPathMargin >= NAME_SIZE)
	{
		const int32 Together = NAME_SIZE - SubobjectPathMargin;
		const int32 LongestName = Together - 1 - PathLength;
		const FString WhatWorks = LongestName > 0
			? FString::Printf(TEXT("a %s of at most %d characters with this %s, or a shorter %s"), *NameParam->Name, LongestName,
				*PathParam->Name, *PathParam->Name)
			: FString::Printf(TEXT("a shorter %s"), *PathParam->Name);
		return FString::Printf(
			TEXT("%s: %s (an asset path of %d characters) and %s (%d characters) are %d characters together. An object the tool creates ")
			TEXT("inside that asset is named after both, and its path takes up to %d characters more (a component template ")
			TEXT("'<asset>_C:<name>_GEN_VARIABLE', a widget '<asset>:WidgetTree.<name>', the graphs and nodes under a named state ")
			TEXT("machine); the engine makes an FName of that path when it records the creation, an FName holds at most %d characters, ")
			TEXT("and a longer one aborts the editor (UnrealNames.cpp, NAME_SIZE). The two must stay under %d characters together. ")
			TEXT("Refused before the tool ran: nothing was changed. WHAT WORKS: %s."),
			*CommandType, *PathParam->Name, PathLength, *NameParam->Name, NameLength, PathLength + NameLength, SubobjectPathMargin,
			NAME_SIZE - 1, Together, *WhatWorks);
	}
	return FString();
}

FString FMcpToolCatalog::CheckVectorShapes(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	using namespace McpToolCatalogPrivate;

	if (!Params.IsValid())
	{
		return FString();
	}
	const TSharedPtr<const FMcpToolCatalog> Catalog = GetCurrent();
	const FTool* Tool = Catalog.IsValid() ? Catalog->FindTool(CommandType) : nullptr;
	if (!Tool)
	{
		return FString();
	}
	for (const FParam& Param : Tool->Params)
	{
		const TSharedPtr<FJsonValue> Value = Param.VectorShape != EVectorShape::None ? Params->TryGetField(Param.Name) : nullptr;
		// An absent value keeps the tool's documented default. A null falls through to "not an array": the handlers that
		// test the param with HasField take a null as present, and the reader makes it 0, 0, 0. tools/call drops a null
		// argument before the dispatch (McpHttpServer), so one arrives here from a batch item or an in-process call.
		if (!Value.IsValid())
		{
			continue;
		}
		const FVectorShapeText& Shape = VectorShapeText(Param.VectorShape);
		FString Where = Param.Name;
		FString Given;
		FString Expected = Shape.Expected;
		if (Value->Type == EJson::Array)
		{
			// The readers take the first Count numbers and drop the rest; an element of another type is lexed or zeroed.
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			if (Items.Num() != Shape.Count)
			{
				Given = DescribeJsonValue(Value);
			}
			for (int32 Index = 0; Given.IsEmpty() && Index < Items.Num(); ++Index)
			{
				if (!IsReadableNumber(Items[Index], /*bFloat=*/true))
				{
					Where = FString::Printf(TEXT("%s[%d]"), *Param.Name, Index);
					Given = DescribeJsonValue(Items[Index]);
					Expected = TEXT("a finite number within the float range");
				}
			}
		}
		else if (Value->Type == EJson::Object && Param.VectorShape == EVectorShape::Rotator)
		{
			// The object form: any subset of the three axes (the schema allows no other key), each a number. The keys
			// are matched as the dispatch's rotation rewrite matches them (FMcpToolCatalog::NormalizeRotationParams).
			for (const auto& Axis : Value->AsObject()->Values)
			{
				const FString Key = FString(*Axis.Key).ToLower().TrimStartAndEnd();
				if (Key != TEXT("roll") && Key != TEXT("pitch") && Key != TEXT("yaw"))
				{
					Given = FString::Printf(TEXT("an object with the key \"%s\""), *Axis.Key);
					Expected = TEXT("an object of roll, pitch and yaw");
					break;
				}
				if (!IsReadableNumber(Axis.Value, /*bFloat=*/false))
				{
					Where = FString::Printf(TEXT("%s.%s"), *Param.Name, *Axis.Key);
					Given = DescribeJsonValue(Axis.Value);
					Expected = TEXT("a finite number");
					break;
				}
			}
		}
		else
		{
			Given = DescribeJsonValue(Value);
		}
		if (!Given.IsEmpty())
		{
			return FString::Printf(
				TEXT("%s: %s is %s, not %s. The tool reads %s as %s, and any other value would have been read as something the call ")
				TEXT("did not ask for (most of them as %s) with no error. Refused before the tool ran: nothing was changed. WHAT ")
				TEXT("WORKS: %s, or leave %s out for its default."),
				*CommandType, *Where, *Given, *Expected, *Param.Name, Shape.ReadAs, Shape.Zero, Shape.WhatWorks, *Param.Name);
		}
	}
	return FString();
}

int32 FMcpToolCatalog::NormalizeRotationParams(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return 0;
	}
	const TSharedPtr<const FMcpToolCatalog> Catalog = GetCurrent();
	const FTool* Tool = Catalog.IsValid() ? Catalog->FindTool(CommandType) : nullptr;

	TArray<TPair<FString, TSharedPtr<FJsonValue>>> Rewrites;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Params->Values)
	{
		if (!Field.Value.IsValid() || Field.Value->Type != EJson::Object || !LooksLikeRotationName(Field.Key))
		{
			continue;
		}
		// If the catalog knows the parameter, it must be array-typed: a `rotation` with another
		// meaning (a struct, an options object) is left alone. If it does not know it -- a batch
		// sub-command whose name does not match -- only the shape of the object decides.
		if (Tool)
		{
			if (const FParam* Param = Tool->FindParam(Field.Key))
			{
				if (!Param->bIsRotation)
				{
					continue;
				}
			}
		}

		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		bool bAllAxes = true;
		bool bAny = false;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Axis : Field.Value->AsObject()->Values)
		{
			const FString Key = Axis.Key.ToLower().TrimStartAndEnd();
			if (!Axis.Value.IsValid() || Axis.Value->Type != EJson::Number)
			{
				bAllAxes = false;
				break;
			}
			if (Key == TEXT("pitch"))     { Pitch = Axis.Value->AsNumber(); }
			else if (Key == TEXT("yaw"))  { Yaw = Axis.Value->AsNumber(); }
			else if (Key == TEXT("roll")) { Roll = Axis.Value->AsNumber(); }
			else { bAllAxes = false; break; }
			bAny = true;
		}
		if (!bAllAxes || !bAny)
		{
			continue;
		}
		TArray<TSharedPtr<FJsonValue>> Wire = {
			MakeShared<FJsonValueNumber>(Pitch),
			MakeShared<FJsonValueNumber>(Yaw),
			MakeShared<FJsonValueNumber>(Roll) };
		Rewrites.Emplace(Field.Key, MakeShared<FJsonValueArray>(Wire));
	}
	for (TPair<FString, TSharedPtr<FJsonValue>>& Rewrite : Rewrites)
	{
		Params->SetField(Rewrite.Key, Rewrite.Value);
	}
	return Rewrites.Num();
}
