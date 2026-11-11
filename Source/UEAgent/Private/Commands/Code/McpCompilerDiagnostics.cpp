// Copyright (c) 2026 CarlxsMG.

#include "Commands/Code/McpCompilerDiagnostics.h"

namespace McpCompilerDiagnostics
{
namespace DiagnosticsParsing
{
    /** Digits of S from Pos on; advances Pos past them. False when there is none. */
    bool ReadNumber(const FString& S, int32& Pos, int32& Out)
    {
        const int32 Start = Pos;
        int64 Value = 0;
        while (Pos < S.Len() && FChar::IsDigit(S[Pos]))
        {
            Value = FMath::Min<int64>(Value * 10 + (S[Pos] - TEXT('0')), MAX_int32);
            ++Pos;
        }
        Out = static_cast<int32>(Value);
        return Pos > Start;
    }

    void SkipSpaces(const FString& S, int32& Pos)
    {
        while (Pos < S.Len() && FChar::IsWhitespace(S[Pos]))
        {
            ++Pos;
        }
    }

    /**
     * What follows the location: `fatal error C1083: msg`, `error C2065: msg`, `error: msg`, `warning: msg`.
     * Fills Severity, Code and Message; false when Rest does not start with a severity.
     */
    bool ReadSeverityCodeMessage(const FString& Rest, FDiagnostic& Out)
    {
        static const TCHAR* const Severities[] = { TEXT("fatal error"), TEXT("error"), TEXT("warning") };
        for (const TCHAR* Severity : Severities)
        {
            if (!Rest.StartsWith(Severity, ESearchCase::CaseSensitive))
            {
                continue;
            }
            int32 Pos = FCString::Strlen(Severity);
            if (Pos < Rest.Len() && FChar::IsAlnum(Rest[Pos]))
            {
                return false; // "errors", "warnings": a summary line, not a diagnostic
            }
            SkipSpaces(Rest, Pos);
            FString Code;
            if (Pos < Rest.Len() && Rest[Pos] != TEXT(':'))
            {
                const int32 CodeStart = Pos;
                while (Pos < Rest.Len() && !FChar::IsWhitespace(Rest[Pos]) && Rest[Pos] != TEXT(':'))
                {
                    ++Pos;
                }
                Code = Rest.Mid(CodeStart, Pos - CodeStart);
                SkipSpaces(Rest, Pos);
            }
            if (Pos >= Rest.Len() || Rest[Pos] != TEXT(':'))
            {
                return false;
            }
            FString Message = Rest.Mid(Pos + 1).TrimStartAndEnd();
            // clang puts its warning flag at the end: "unused variable 'y' [-Wunused-variable]".
            if (Code.IsEmpty() && Message.EndsWith(TEXT("]")))
            {
                const int32 Open = Message.Find(TEXT("[-W"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
                if (Open != INDEX_NONE)
                {
                    Code = Message.Mid(Open + 1, Message.Len() - Open - 2);
                    Message = Message.Left(Open).TrimEnd();
                }
            }
            Out.Severity = FCString::Strcmp(Severity, TEXT("warning")) == 0 ? TEXT("warning") : TEXT("error");
            Out.Code = Code;
            Out.Message = Message;
            return true;
        }
        return false;
    }

    /**
     * The next index of C in S after From, or INDEX_NONE. Not FString::Find with a start position: it clamps the
     * start to the last character, so a match there is found again and again.
     */
    int32 NextIndexOf(const FString& S, TCHAR C, int32 From)
    {
        for (int32 i = From + 1; i < S.Len(); ++i)
        {
            if (S[i] == C)
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    /** `File(12): ...` and `File(12,5): ...` (MSVC, clang-cl). */
    bool ParseMsvc(const FString& S, FDiagnostic& Out)
    {
        for (int32 Open = NextIndexOf(S, TEXT('('), 0); Open > 0; Open = NextIndexOf(S, TEXT('('), Open))
        {
            int32 Pos = Open + 1;
            int32 Line = 0, Column = 0;
            if (!ReadNumber(S, Pos, Line))
            {
                continue;
            }
            if (Pos < S.Len() && S[Pos] == TEXT(','))
            {
                ++Pos;
                if (!ReadNumber(S, Pos, Column))
                {
                    continue;
                }
            }
            if (Pos >= S.Len() || S[Pos] != TEXT(')'))
            {
                continue;
            }
            ++Pos;
            SkipSpaces(S, Pos);
            if (Pos >= S.Len() || S[Pos] != TEXT(':'))
            {
                continue;
            }
            ++Pos;
            SkipSpaces(S, Pos);
            FDiagnostic D;
            if (!ReadSeverityCodeMessage(S.Mid(Pos), D))
            {
                return false;
            }
            D.File = S.Left(Open).TrimStartAndEnd();
            D.Line = Line;
            D.Column = Column;
            Out = MoveTemp(D);
            return true;
        }
        return false;
    }

    /** `file:12:5: error: ...` (clang, gcc). The drive letter's colon of a Windows path is not a line number. */
    bool ParseClang(const FString& S, FDiagnostic& Out)
    {
        for (int32 Colon = NextIndexOf(S, TEXT(':'), 0); Colon > 0; Colon = NextIndexOf(S, TEXT(':'), Colon))
        {
            int32 Pos = Colon + 1;
            int32 Line = 0, Column = 0;
            if (!ReadNumber(S, Pos, Line) || Pos >= S.Len() || S[Pos] != TEXT(':'))
            {
                continue;
            }
            ++Pos;
            const int32 AfterLine = Pos;
            if (ReadNumber(S, Pos, Column))
            {
                if (Pos >= S.Len() || S[Pos] != TEXT(':'))
                {
                    continue;
                }
                ++Pos;
            }
            else
            {
                Pos = AfterLine; // gcc may give the line only: "file:12: error: ..."
            }
            SkipSpaces(S, Pos);
            FDiagnostic D;
            if (!ReadSeverityCodeMessage(S.Mid(Pos), D))
            {
                return false;
            }
            D.File = S.Left(Colon).TrimStartAndEnd();
            D.Line = Line;
            D.Column = Column;
            Out = MoveTemp(D);
            return true;
        }
        return false;
    }

    /** `Module.obj : error LNK2019: ...` and `LINK : fatal error LNK1104: ...`. */
    bool ParseLinker(const FString& S, FDiagnostic& Out)
    {
        const int32 Sep = S.Find(TEXT(" : "));
        if (Sep <= 0)
        {
            return false;
        }
        FDiagnostic D;
        if (!ReadSeverityCodeMessage(S.Mid(Sep + 3).TrimStart(), D) || !D.Code.StartsWith(TEXT("LNK")))
        {
            return false;
        }
        D.File = S.Left(Sep).TrimStartAndEnd();
        Out = MoveTemp(D);
        return true;
    }
}

bool ParseLine(const FString& Line, FDiagnostic& Out)
{
    const FString S = Line.TrimStartAndEnd();
    if (S.IsEmpty())
    {
        return false;
    }
    return DiagnosticsParsing::ParseMsvc(S, Out) || DiagnosticsParsing::ParseClang(S, Out)
        || DiagnosticsParsing::ParseLinker(S, Out);
}

void Parse(const FString& Text, TArray<FDiagnostic>& OutErrors, TArray<FDiagnostic>& OutWarnings)
{
    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
    for (const FString& Line : Lines)
    {
        FDiagnostic D;
        if (!ParseLine(Line, D))
        {
            continue;
        }
        TArray<FDiagnostic>& Into = D.Severity == TEXT("warning") ? OutWarnings : OutErrors;
        // A build tool can print the same diagnostic twice (the compiler's output, then its own summary).
        if (!Into.Contains(D))
        {
            Into.Add(MoveTemp(D));
        }
    }
}
}
