// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpActivity.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpActivityPrivate
{
	using McpActivity::EOutcome;

	static TArray<TSharedPtr<const McpActivity::FEntry>> Entries;
	static FSimpleMulticastDelegate Changed;

	/** The first line of Text, at most 300 characters. */
	static FString OneLine(const FString& Text)
	{
		FString Line = Text;
		int32 Break = INDEX_NONE;
		if (Line.FindChar(TEXT('\n'), Break))
		{
			Line.LeftInline(Break);
		}
		Line.TrimStartAndEndInline();
		if (Line.Len() > 300)
		{
			Line = Line.Left(297) + TEXT("...");
		}
		return Line;
	}

	/** The outcome as the model reads it: refused, success:false, a readback verdict that is not ok, or fine. */
	static void Classify(const TSharedPtr<FJsonObject>& Result, bool bRefused, EOutcome& OutOutcome, FString& OutDetail)
	{
		FString Error;
		bool bSuccess = true;
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		if (Result.IsValid())
		{
			Result->TryGetStringField(TEXT("error"), Error);
			Result->TryGetBoolField(TEXT("success"), bSuccess);
			Result->TryGetObjectField(TEXT("readback"), Readback);
		}
		if (bRefused || !bSuccess)
		{
			OutOutcome = bRefused ? EOutcome::Refused : EOutcome::Failed;
			OutDetail = OneLine(Error);
			return;
		}
		if (Readback && Readback->IsValid())
		{
			bool bChecked = false;
			FString Verdict;
			FString Note;
			(*Readback)->TryGetBoolField(TEXT("checked"), bChecked);
			(*Readback)->TryGetStringField(TEXT("verdict"), Verdict);
			(*Readback)->TryGetStringField(TEXT("note"), Note);
			if (bChecked && !Verdict.IsEmpty() && Verdict != TEXT("ok"))
			{
				OutOutcome = EOutcome::Warning;
				OutDetail = OneLine(Note.IsEmpty() ? Verdict : Verdict + TEXT(": ") + Note);
				return;
			}
		}
		OutOutcome = EOutcome::Ok;
		OutDetail.Reset();
	}

	static FAutoConsoleCommand ActivityCommand(
		TEXT("mcp.activity"),
		TEXT("Prints the last N MCP tool calls (default 20) to the log: time, tool, duration, outcome and detail."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const int32 Count = Args.Num() > 0 ? FMath::Max(1, FCString::Atoi(*Args[0])) : 20;
			UE_LOG(LogTemp, Display, TEXT("mcp.activity: the last %d of %d calls"), FMath::Min(Count, Entries.Num()), Entries.Num());
			for (int32 Index = FMath::Max(0, Entries.Num() - Count); Index < Entries.Num(); ++Index)
			{
				const McpActivity::FEntry& Entry = *Entries[Index];
				UE_LOG(LogTemp, Display, TEXT("  %s  %-40s %7.0f ms  %-7s %s"), *Entry.Started.ToString(TEXT("%H:%M:%S")),
					*Entry.Tool, Entry.Milliseconds, McpActivity::OutcomeName(Entry.Outcome), *Entry.Detail);
			}
		}));
}

void McpActivity::Record(const FString& Tool, const FDateTime& Started, double Milliseconds, const TSharedPtr<FJsonObject>& Result, bool bRefused)
{
	using namespace McpActivityPrivate;

	TSharedRef<FEntry> Entry = MakeShared<FEntry>();
	Entry->Started = Started;
	Entry->Tool = Tool;
	Entry->Milliseconds = Milliseconds;
	Classify(Result, bRefused, Entry->Outcome, Entry->Detail);
	Entries.Add(Entry);
	if (Entries.Num() > MaxEntries)
	{
		Entries.RemoveAt(0, Entries.Num() - MaxEntries);
	}
	Changed.Broadcast();
}

const TArray<TSharedPtr<const McpActivity::FEntry>>& McpActivity::GetEntries()
{
	return McpActivityPrivate::Entries;
}

void McpActivity::Clear()
{
	McpActivityPrivate::Entries.Reset();
	McpActivityPrivate::Changed.Broadcast();
}

FSimpleMulticastDelegate& McpActivity::OnChanged()
{
	return McpActivityPrivate::Changed;
}

const TCHAR* McpActivity::OutcomeName(EOutcome Outcome)
{
	switch (Outcome)
	{
	case EOutcome::Warning: return TEXT("Warning");
	case EOutcome::Failed:  return TEXT("Failed");
	case EOutcome::Refused: return TEXT("Refused");
	default:                return TEXT("OK");
	}
}
