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
		// one (e.g. get_blueprint_functions_list.filter_type, whose quotes left out 'all').
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
	const int32* Index = IndexByName.Find(ToolName);
	return Index ? &Tools[*Index] : nullptr;
}

bool FMcpToolCatalog::LooksLikeRotationName(const FString& ParamName)
{
	const FString Low = ParamName.ToLower();
	return Low == TEXT("rot") || Low.EndsWith(TEXT("rotation")) || Low.EndsWith(TEXT("rotator"));
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
			if (Tool.Name.IsEmpty() || Catalog->IndexByName.Contains(Tool.Name))
			{
				continue; // no name, or a duplicate: the first one wins, as in the catalog
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
			ToolsJson.Add(MakeShared<FJsonValueObject>(ToolJson));
			Tool.InputSchema = InputSchema;

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
