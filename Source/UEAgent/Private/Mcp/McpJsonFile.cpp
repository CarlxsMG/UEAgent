// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpJsonFile.h"

#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpJsonFilePrivate
{
	using McpJsonFile::FLayout;

	static FString NormalizeUrl(FString Url)
	{
		Url.TrimStartAndEndInline();
		Url.ToLowerInline();
		// The query only picks the tool list (?tools=compact): the server is the same one.
		int32 Query = INDEX_NONE;
		if (Url.FindChar(TEXT('?'), Query))
		{
			Url.LeftInline(Query);
		}
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}
		return Url;
	}

	static void AppendIndent(FString& Out, const FLayout& Layout, int32 Depth)
	{
		for (int32 i = 0; i < Depth; ++i)
		{
			Out += Layout.Indent;
		}
	}

	// JSON.stringify escapes: quotes, backslash and control characters; everything else (UTF-8) as is.
	static void AppendQuoted(FString& Out, const FString& Text)
	{
		Out.AppendChar(TEXT('"'));
		for (const TCHAR C : Text)
		{
			switch (C)
			{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
			}
		}
		Out.AppendChar(TEXT('"'));
	}

	static void WriteObject(FString& Out, const TSharedPtr<FJsonObject>& Object, const FLayout& Layout, int32 Depth);

	static void WriteValue(FString& Out, const TSharedPtr<FJsonValue>& Value, const FLayout& Layout, int32 Depth)
	{
		if (!Value.IsValid())
		{
			Out += TEXT("null");
			return;
		}
		switch (Value->Type)
		{
		case EJson::Boolean:
			Out += Value->AsBool() ? TEXT("true") : TEXT("false");
			return;
		case EJson::Number:
		{
			// With StoreNumbersAsStrings the number carries its original text.
			FString Text;
			if (!Value->TryGetString(Text) || Text.IsEmpty())
			{
				Text = FString::SanitizeFloat(Value->AsNumber(), 0);
			}
			Out += Text;
			return;
		}
		case EJson::String:
			AppendQuoted(Out, Value->AsString());
			return;
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			if (Items.Num() == 0)
			{
				Out += TEXT("[]");
				return;
			}
			Out += TEXT("[");
			Out += Layout.Eol;
			for (int32 i = 0; i < Items.Num(); ++i)
			{
				AppendIndent(Out, Layout, Depth + 1);
				WriteValue(Out, Items[i], Layout, Depth + 1);
				if (i + 1 < Items.Num())
				{
					Out += TEXT(",");
				}
				Out += Layout.Eol;
			}
			AppendIndent(Out, Layout, Depth);
			Out += TEXT("]");
			return;
		}
		case EJson::Object:
			WriteObject(Out, Value->AsObject(), Layout, Depth);
			return;
		default:
			Out += TEXT("null");
			return;
		}
	}

	static void WriteObject(FString& Out, const TSharedPtr<FJsonObject>& Object, const FLayout& Layout, int32 Depth)
	{
		if (!Object.IsValid() || Object->Values.Num() == 0)
		{
			Out += TEXT("{}");
			return;
		}
		Out += TEXT("{");
		Out += Layout.Eol;
		int32 Index = 0;
		const int32 Count = Object->Values.Num();
		for (const auto& Pair : Object->Values)
		{
			AppendIndent(Out, Layout, Depth + 1);
			AppendQuoted(Out, FString(*Pair.Key));
			Out += TEXT(": ");
			WriteValue(Out, Pair.Value, Layout, Depth + 1);
			if (++Index < Count)
			{
				Out += TEXT(",");
			}
			Out += Layout.Eol;
		}
		AppendIndent(Out, Layout, Depth);
		Out += TEXT("}");
	}
}

bool McpJsonFile::Load(const FString& Path, FString& OutText, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
{
	if (!FFileHelper::LoadFileToString(OutText, *Path))
	{
		OutError = TEXT("could not be read");
		return false;
	}
	// Numbers keep their original text: when rewriting, whatever is not touched comes out identical.
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OutText), OutRoot, FJsonSerializer::EFlags::StoreNumbersAsStrings)
		|| !OutRoot.IsValid())
	{
		OutError = TEXT("is not valid JSON");
		return false;
	}
	return true;
}

// By hand rather than with TJsonWriter: Unreal's writer indents with tabs and reformats numbers,
// and these files are usually written by a client (two spaces) or by a person.
McpJsonFile::FLayout McpJsonFile::DetectLayout(const FString& Text)
{
	FLayout Layout;
	Layout.Eol = Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
	Layout.bTrailingEol = Text.EndsWith(TEXT("\n"));
	// The first indented line is at depth 1: its indentation is exactly one unit.
	int32 LineStart = Text.Find(TEXT("\n"));
	while (LineStart != INDEX_NONE)
	{
		int32 i = LineStart + 1;
		FString Lead;
		while (i < Text.Len() && (Text[i] == TEXT(' ') || Text[i] == TEXT('\t')))
		{
			Lead.AppendChar(Text[i++]);
		}
		if (!Lead.IsEmpty())
		{
			Layout.Indent = Lead[0] == TEXT('\t') ? FString(TEXT("\t")) : Lead;
			break;
		}
		LineStart = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, LineStart + 1);
	}
	return Layout;
}

FString McpJsonFile::Serialize(const TSharedPtr<FJsonObject>& Root, const FLayout& Layout)
{
	FString Out;
	McpJsonFilePrivate::WriteObject(Out, Root, Layout, 0);
	if (Layout.bTrailingEol)
	{
		Out += Layout.Eol;
	}
	return Out;
}

bool McpJsonFile::WriteAtomically(const FString& Path, const FString& Text, FString& OutError)
{
	const FString TempPath = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Text, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *TempPath);
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *TempPath, /*Replace*/ true, /*EvenIfReadOnly*/ false))
	{
		IFileManager::Get().Delete(*TempPath);
		OutError = FString::Printf(TEXT("Could not replace %s: is it read-only, or locked by source control?"), *Path);
		return false;
	}
	return true;
}

TSharedPtr<FJsonObject> McpJsonFile::ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TSharedPtr<FJsonObject>* Found = nullptr;
	return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : nullptr;
}

FString McpJsonFile::StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(Field, Value);
	}
	return Value;
}

bool McpJsonFile::HasAuthorizationHeader(const TSharedPtr<FJsonObject>& Entry)
{
	const TSharedPtr<FJsonObject> Headers = ObjectField(Entry, TEXT("headers"));
	if (!Headers.IsValid())
	{
		return false;
	}
	for (const auto& Pair : Headers->Values)
	{
		FString Value;
		if (FString(*Pair.Key).Equals(TEXT("Authorization"), ESearchCase::IgnoreCase)
			&& Pair.Value.IsValid() && Pair.Value->TryGetString(Value) && !Value.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
	}
	return false;
}

bool McpJsonFile::SameUrl(const FString& A, const FString& B)
{
	return !A.IsEmpty() && McpJsonFilePrivate::NormalizeUrl(A) == McpJsonFilePrivate::NormalizeUrl(B);
}
