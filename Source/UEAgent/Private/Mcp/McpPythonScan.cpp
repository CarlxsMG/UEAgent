// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpPythonScan.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpPythonScanPrivate
{
	using namespace McpPythonScan;

	static bool IsIdStart(TCHAR C) { return FChar::IsAlpha(C) || C == TEXT('_') || C >= 128; }
	static bool IsIdChar(TCHAR C)  { return FChar::IsAlnum(C) || C == TEXT('_') || C >= 128; }

	static bool Is(const FToken& Token, ETokenType Type, const TCHAR* Text)
	{
		return Token.Type == Type && Token.Text.Equals(Text, ESearchCase::CaseSensitive);
	}

	static bool IsKeyword(const FString& Word)
	{
		static const TCHAR* const Keywords[] = {
			TEXT("False"), TEXT("None"), TEXT("True"), TEXT("and"), TEXT("as"), TEXT("assert"), TEXT("async"),
			TEXT("await"), TEXT("break"), TEXT("class"), TEXT("continue"), TEXT("def"), TEXT("del"), TEXT("elif"),
			TEXT("else"), TEXT("except"), TEXT("finally"), TEXT("for"), TEXT("from"), TEXT("global"), TEXT("if"),
			TEXT("import"), TEXT("in"), TEXT("is"), TEXT("lambda"), TEXT("nonlocal"), TEXT("not"), TEXT("or"),
			TEXT("pass"), TEXT("raise"), TEXT("return"), TEXT("try"), TEXT("while"), TEXT("with"), TEXT("yield") };
		for (const TCHAR* Keyword : Keywords)
		{
			if (Word.Equals(Keyword, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	static bool IsStringPrefix(const FString& Word)
	{
		static const TCHAR* const Prefixes[] = {
			TEXT("r"), TEXT("u"), TEXT("b"), TEXT("f"), TEXT("t"),
			TEXT("br"), TEXT("rb"), TEXT("fr"), TEXT("rf"), TEXT("tr"), TEXT("rt") };
		for (const TCHAR* Prefix : Prefixes)
		{
			if (Word.Equals(Prefix, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static int32 HexValue(TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) { return C - TEXT('0'); }
		if (C >= TEXT('a') && C <= TEXT('f')) { return 10 + C - TEXT('a'); }
		if (C >= TEXT('A') && C <= TEXT('F')) { return 10 + C - TEXT('A'); }
		return -1;
	}

	/** Decodes the escape sequences of a plain Python string (not raw, bytes or f-string). */
	static FString DecodeEscapes(const FString& Raw)
	{
		FString Out;
		Out.Reserve(Raw.Len());
		for (int32 I = 0; I < Raw.Len(); ++I)
		{
			const TCHAR C = Raw[I];
			if (C != TEXT('\\') || I + 1 >= Raw.Len())
			{
				Out.AppendChar(C);
				continue;
			}
			const TCHAR E = Raw[++I];
			auto ReadHex = [&Raw, &I](int32 Digits) -> int32
			{
				int32 Value = 0;
				for (int32 D = 0; D < Digits; ++D)
				{
					if (I + 1 >= Raw.Len() || HexValue(Raw[I + 1]) < 0)
					{
						return -1;
					}
					Value = Value * 16 + HexValue(Raw[++I]);
				}
				return Value;
			};
			switch (E)
			{
			case TEXT('\n'): break;                                 // line continuation inside the string
			case TEXT('\r'): if (I + 1 < Raw.Len() && Raw[I + 1] == TEXT('\n')) { ++I; } break;
			case TEXT('\\'): Out.AppendChar(TEXT('\\')); break;
			case TEXT('\''): Out.AppendChar(TEXT('\'')); break;
			case TEXT('"'):  Out.AppendChar(TEXT('"')); break;
			case TEXT('n'):  Out.AppendChar(TEXT('\n')); break;
			case TEXT('t'):  Out.AppendChar(TEXT('\t')); break;
			case TEXT('r'):  Out.AppendChar(TEXT('\r')); break;
			case TEXT('a'):  Out.AppendChar(TEXT('\a')); break;
			case TEXT('b'):  Out.AppendChar(TEXT('\b')); break;
			case TEXT('f'):  Out.AppendChar(TEXT('\f')); break;
			case TEXT('v'):  Out.AppendChar(TEXT('\v')); break;
			case TEXT('x'):
			case TEXT('u'):
			case TEXT('U'):
			{
				const int32 Code = ReadHex(E == TEXT('x') ? 2 : (E == TEXT('u') ? 4 : 8));
				if (Code < 0)
				{
					Out.AppendChar(TEXT('\\'));
					Out.AppendChar(E);
				}
				else
				{
					Out.AppendChar(static_cast<TCHAR>(Code)); // enough to compare ASCII identifiers
				}
				break;
			}
			default:
				if (E >= TEXT('0') && E <= TEXT('7'))
				{
					int32 Value = E - TEXT('0');
					for (int32 D = 0; D < 2 && I + 1 < Raw.Len() && Raw[I + 1] >= TEXT('0') && Raw[I + 1] <= TEXT('7'); ++D)
					{
						Value = Value * 8 + (Raw[++I] - TEXT('0'));
					}
					Out.AppendChar(static_cast<TCHAR>(Value));
				}
				else
				{
					// Unknown escape: Python keeps the backslash (with a warning).
					Out.AppendChar(TEXT('\\'));
					Out.AppendChar(E);
				}
			}
		}
		return Out;
	}

	/** Reads a string literal that starts at Begin (its prefix) with the quote at QuotePos. */
	static bool ReadString(const FString& Code, int32 Begin, int32 QuotePos, TArray<FToken>& Out, int32& OutNext)
	{
		const int32 N = Code.Len();
		const FString Prefix = Code.Mid(Begin, QuotePos - Begin).ToLower();
		const bool bRaw = Prefix.Contains(TEXT("r"));
		const bool bBytes = Prefix.Contains(TEXT("b"));
		const bool bTemplate = Prefix.Contains(TEXT("f")) || Prefix.Contains(TEXT("t"));
		const TCHAR Quote = Code[QuotePos];
		const bool bTriple = QuotePos + 2 < N && Code[QuotePos + 1] == Quote && Code[QuotePos + 2] == Quote;

		int32 K = QuotePos + (bTriple ? 3 : 1);
		const int32 ContentBegin = K;
		int32 ContentEnd = INDEX_NONE;
		while (ContentEnd == INDEX_NONE)
		{
			if (K >= N)
			{
				return false; // unterminated string
			}
			const TCHAR C = Code[K];
			if (C == TEXT('\\'))
			{
				K += 2; // in raw strings too: the backslash stops the next quote from closing
				continue;
			}
			if (bTriple)
			{
				if (C == Quote && K + 2 < N && Code[K + 1] == Quote && Code[K + 2] == Quote)
				{
					ContentEnd = K;
					K += 3;
					break;
				}
			}
			else
			{
				if (C == Quote)
				{
					ContentEnd = K;
					K += 1;
					break;
				}
				if (C == TEXT('\n') || C == TEXT('\r'))
				{
					return false; // unterminated single-line string
				}
			}
			++K;
		}

		FToken& Token = Out.AddDefaulted_GetRef();
		Token.Type = ETokenType::String;
		Token.Begin = Begin;
		Token.End = K;
		Token.Text = Code.Mid(Begin, K - Begin);
		Token.bBytes = bBytes;
		Token.bFString = bTemplate;
		const FString Content = Code.Mid(ContentBegin, ContentEnd - ContentBegin);
		Token.StringValue = (bRaw || bBytes || bTemplate) ? Content : DecodeEscapes(Content);
		OutNext = K;
		return true;
	}

	static int32 ReadNumber(const FString& Code, int32 Begin, TArray<FToken>& Out)
	{
		const int32 N = Code.Len();
		int32 J = Begin;
		auto IsDigitOrSep = [&Code](int32 Index) { return FChar::IsDigit(Code[Index]) || Code[Index] == TEXT('_'); };
		if (Code[J] == TEXT('0') && J + 1 < N && FCString::Strchr(TEXT("xXoObB"), Code[J + 1]))
		{
			J += 2;
			while (J < N && (HexValue(Code[J]) >= 0 || Code[J] == TEXT('_')))
			{
				++J;
			}
		}
		else
		{
			while (J < N && IsDigitOrSep(J)) { ++J; }
			if (J < N && Code[J] == TEXT('.'))
			{
				++J;
				while (J < N && IsDigitOrSep(J)) { ++J; }
			}
			if (J < N && (Code[J] == TEXT('e') || Code[J] == TEXT('E')))
			{
				int32 K = J + 1;
				if (K < N && (Code[K] == TEXT('+') || Code[K] == TEXT('-'))) { ++K; }
				if (K < N && FChar::IsDigit(Code[K]))
				{
					J = K;
					while (J < N && IsDigitOrSep(J)) { ++J; }
				}
			}
		}
		if (J < N && (Code[J] == TEXT('j') || Code[J] == TEXT('J')))
		{
			++J;
		}
		FToken& Token = Out.AddDefaulted_GetRef();
		Token.Type = ETokenType::Number;
		Token.Begin = Begin;
		Token.End = J;
		Token.Text = Code.Mid(Begin, J - Begin);
		return J;
	}

	struct FCall
	{
		FString Name;
		bool bAttribute = false;
		int32 NameIndex = INDEX_NONE;
		int32 OpenIndex = INDEX_NONE;
		int32 CloseIndex = INDEX_NONE;
	};

	enum class EArgKind : uint8 { Positional, Starred, Keyword, DoubleStar };

	struct FArg
	{
		EArgKind Kind = EArgKind::Positional;
		int32 Begin = 0;      // tokens [Begin, End)
		int32 End = 0;
		FString KeywordName;
		int32 ValueBegin = 0; // where the value starts (for a keyword argument, after the '=')
	};

	static TArray<FCall> FindCalls(const TArray<FToken>& Tokens)
	{
		// Closing index of each '(' (the other brackets only count for depth).
		TArray<int32> Closing;
		Closing.Init(INDEX_NONE, Tokens.Num());
		TArray<int32> Stack;
		for (int32 Index = 0; Index < Tokens.Num(); ++Index)
		{
			const FToken& Token = Tokens[Index];
			if (Token.Type != ETokenType::Op)
			{
				continue;
			}
			if (Token.Text == TEXT("(") || Token.Text == TEXT("[") || Token.Text == TEXT("{"))
			{
				Stack.Push(Index);
			}
			else if ((Token.Text == TEXT(")") || Token.Text == TEXT("]") || Token.Text == TEXT("}")) && Stack.Num() > 0)
			{
				Closing[Stack.Pop()] = Index;
			}
		}

		TArray<FCall> Calls;
		for (int32 Index = 0; Index + 1 < Tokens.Num(); ++Index)
		{
			const FToken& Token = Tokens[Index];
			if (Token.Type != ETokenType::Name || IsKeyword(Token.Text) || !Is(Tokens[Index + 1], ETokenType::Op, TEXT("(")))
			{
				continue;
			}
			// `def f(` / `class C(` are not calls.
			if (Index > 0 && (Is(Tokens[Index - 1], ETokenType::Name, TEXT("def")) || Is(Tokens[Index - 1], ETokenType::Name, TEXT("class"))))
			{
				continue;
			}
			FCall& Call = Calls.AddDefaulted_GetRef();
			Call.Name = Token.Text;
			Call.bAttribute = Index > 0 && Is(Tokens[Index - 1], ETokenType::Op, TEXT("."));
			Call.NameIndex = Index;
			Call.OpenIndex = Index + 1;
			Call.CloseIndex = Closing[Index + 1];
		}
		return Calls;
	}

	static TArray<FArg> SplitArgs(const TArray<FToken>& Tokens, const FCall& Call)
	{
		TArray<FArg> Args;
		if (Call.CloseIndex == INDEX_NONE)
		{
			return Args;
		}
		int32 Depth = 0;
		int32 Start = Call.OpenIndex + 1;
		for (int32 Index = Call.OpenIndex + 1; Index <= Call.CloseIndex; ++Index)
		{
			bool bEnd = Index == Call.CloseIndex;
			if (!bEnd && Tokens[Index].Type == ETokenType::Op)
			{
				const FString& Text = Tokens[Index].Text;
				if (Text == TEXT("(") || Text == TEXT("[") || Text == TEXT("{"))
				{
					++Depth;
				}
				else if (Text == TEXT(")") || Text == TEXT("]") || Text == TEXT("}"))
				{
					--Depth;
				}
				else if (Text == TEXT(",") && Depth == 0)
				{
					bEnd = true;
				}
			}
			if (!bEnd)
			{
				continue;
			}
			if (Index > Start)
			{
				FArg& Arg = Args.AddDefaulted_GetRef();
				Arg.Begin = Start;
				Arg.End = Index;
				Arg.ValueBegin = Start;
				const FToken& First = Tokens[Start];
				if (Is(First, ETokenType::Op, TEXT("*")))
				{
					Arg.Kind = EArgKind::Starred;
				}
				else if (Is(First, ETokenType::Op, TEXT("**")))
				{
					Arg.Kind = EArgKind::DoubleStar;
				}
				else if (First.Type == ETokenType::Name && Start + 1 < Index && Is(Tokens[Start + 1], ETokenType::Op, TEXT("=")))
				{
					Arg.Kind = EArgKind::Keyword;
					Arg.KeywordName = First.Text;
					Arg.ValueBegin = Start + 2;
				}
			}
			Start = Index + 1;
		}
		return Args;
	}

	/** Is the range a text constant (str, not bytes or f-string)? Joins implicit concatenation. */
	static bool AsConstantString(const TArray<FToken>& Tokens, int32 Begin, int32 End, FString& OutValue)
	{
		if (Begin >= End)
		{
			return false;
		}
		OutValue.Reset();
		for (int32 Index = Begin; Index < End; ++Index)
		{
			const FToken& Token = Tokens[Index];
			if (Token.Type != ETokenType::String || Token.bBytes || Token.bFString)
			{
				return false;
			}
			OutValue += Token.StringValue;
		}
		return true;
	}

	/** An int/float literal equal to 0 (a complex literal or a -0 does not count). */
	static bool IsZeroLiteral(const TArray<FToken>& Tokens, int32 Begin, int32 End)
	{
		if (End - Begin != 1 || Tokens[Begin].Type != ETokenType::Number)
		{
			return false;
		}
		const FString Text = Tokens[Begin].Text.Replace(TEXT("_"), TEXT("")).ToLower();
		if (Text.EndsWith(TEXT("j")))
		{
			return false;
		}
		if (Text.StartsWith(TEXT("0x")) || Text.StartsWith(TEXT("0o")) || Text.StartsWith(TEXT("0b")))
		{
			const FString Digits = Text.Mid(2);
			if (Digits.IsEmpty())
			{
				return false;
			}
			for (const TCHAR C : Digits)
			{
				if (C != TEXT('0'))
				{
					return false;
				}
			}
			return true;
		}
		return FCString::Atod(*Text) == 0.0;
	}

	static void AddUniqueExact(TArray<FString>& Values, const FString& Value)
	{
		if (!ContainsExact(Values, Value))
		{
			Values.Add(Value);
		}
	}

	static bool IsAssignOp(const FToken& Token)
	{
		static const TCHAR* const Ops[] = {
			TEXT("="), TEXT("+="), TEXT("-="), TEXT("*="), TEXT("/="), TEXT("//="), TEXT("%="), TEXT("**="),
			TEXT(">>="), TEXT("<<="), TEXT("&="), TEXT("^="), TEXT("|="), TEXT("@=") };
		if (Token.Type != ETokenType::Op)
		{
			return false;
		}
		for (const TCHAR* Op : Ops)
		{
			if (Token.Text == Op)
			{
				return true;
			}
		}
		return false;
	}
}

bool McpPythonScan::ContainsExact(const TArray<FString>& Names, const FString& Name)
{
	return Names.ContainsByPredicate([&Name](const FString& Candidate) { return Candidate.Equals(Name, ESearchCase::CaseSensitive); });
}

bool McpPythonScan::Tokenize(const FString& Code, TArray<FToken>& OutTokens)
{
	using namespace McpPythonScanPrivate;

	OutTokens.Reset();
	const int32 N = Code.Len();
	int32 Depth = 0;
	int32 I = 0;
	while (I < N)
	{
		const TCHAR C = Code[I];
		if (C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\f'))
		{
			++I;
			continue;
		}
		if (C == TEXT('\\'))
		{
			// Line continuation: backslash + newline. A lone backslash outside a string is not Python.
			int32 J = I + 1;
			if (J < N && Code[J] == TEXT('\r')) { ++J; }
			if (J < N && Code[J] == TEXT('\n')) { ++J; }
			if (J == I + 1)
			{
				return false;
			}
			I = J;
			continue;
		}
		if (C == TEXT('\r') || C == TEXT('\n'))
		{
			// Outside brackets a newline ends the statement: `f` + newline + `(x)` is NOT a call.
			if (Depth == 0 && OutTokens.Num() > 0 && OutTokens.Last().Type != ETokenType::Newline)
			{
				FToken& Token = OutTokens.AddDefaulted_GetRef();
				Token.Type = ETokenType::Newline;
				Token.Begin = I;
				Token.End = I + 1;
			}
			++I;
			continue;
		}
		if (C == TEXT('#'))
		{
			while (I < N && Code[I] != TEXT('\n') && Code[I] != TEXT('\r'))
			{
				++I;
			}
			continue;
		}
		if (IsIdStart(C))
		{
			int32 J = I;
			while (J < N && IsIdChar(Code[J]))
			{
				++J;
			}
			const FString Word = Code.Mid(I, J - I);
			if (J < N && (Code[J] == TEXT('\'') || Code[J] == TEXT('"')) && IsStringPrefix(Word))
			{
				if (!ReadString(Code, I, J, OutTokens, I))
				{
					return false;
				}
				continue;
			}
			FToken& Token = OutTokens.AddDefaulted_GetRef();
			Token.Type = ETokenType::Name;
			Token.Text = Word;
			Token.Begin = I;
			Token.End = J;
			I = J;
			continue;
		}
		if (C == TEXT('\'') || C == TEXT('"'))
		{
			if (!ReadString(Code, I, I, OutTokens, I))
			{
				return false;
			}
			continue;
		}
		if (FChar::IsDigit(C) || (C == TEXT('.') && I + 1 < N && FChar::IsDigit(Code[I + 1])))
		{
			I = ReadNumber(Code, I, OutTokens);
			continue;
		}

		static const TCHAR* const Ops3[] = { TEXT("**="), TEXT("//="), TEXT(">>="), TEXT("<<="), TEXT("...") };
		static const TCHAR* const Ops2[] = {
			TEXT("->"), TEXT(":="), TEXT("**"), TEXT("//"), TEXT("<<"), TEXT(">>"), TEXT("<="), TEXT(">="), TEXT("=="),
			TEXT("!="), TEXT("+="), TEXT("-="), TEXT("*="), TEXT("/="), TEXT("%="), TEXT("&="), TEXT("|="), TEXT("^="), TEXT("@=") };
		int32 Length = 1;
		for (const TCHAR* Op : Ops3)
		{
			if (I + 3 <= N && FCString::Strncmp(*Code + I, Op, 3) == 0) { Length = 3; break; }
		}
		if (Length == 1)
		{
			for (const TCHAR* Op : Ops2)
			{
				if (I + 2 <= N && FCString::Strncmp(*Code + I, Op, 2) == 0) { Length = 2; break; }
			}
		}
		if (Length == 1)
		{
			if (C == TEXT('(') || C == TEXT('[') || C == TEXT('{'))
			{
				++Depth;
			}
			else if (C == TEXT(')') || C == TEXT(']') || C == TEXT('}'))
			{
				if (--Depth < 0)
				{
					return false;
				}
			}
		}
		FToken& Token = OutTokens.AddDefaulted_GetRef();
		Token.Type = ETokenType::Op;
		Token.Text = Code.Mid(I, Length);
		Token.Begin = I;
		Token.End = I + Length;
		I += Length;
	}
	return Depth == 0;
}

TArray<FString> McpPythonScan::CalledNames(const FString& Code)
{
	using namespace McpPythonScanPrivate;

	TArray<FString> Names;
	TArray<FToken> Tokens;
	if (!Tokenize(Code, Tokens))
	{
		return Names;
	}
	for (const FCall& Call : FindCalls(Tokens))
	{
		AddUniqueExact(Names, Call.Name);
	}
	return Names;
}

TArray<FString> McpPythonScan::CallStringArguments(const FString& Code)
{
	using namespace McpPythonScanPrivate;

	TArray<FString> Values;
	TArray<FToken> Tokens;
	if (!Tokenize(Code, Tokens))
	{
		return Values;
	}
	for (const FCall& Call : FindCalls(Tokens))
	{
		for (const FArg& Arg : SplitArgs(Tokens, Call))
		{
			FString Value;
			if ((Arg.Kind == EArgKind::Positional || Arg.Kind == EArgKind::Keyword)
				&& AsConstantString(Tokens, Arg.ValueBegin, Arg.End, Value))
			{
				AddUniqueExact(Values, Value);
			}
		}
	}
	return Values;
}

TArray<FString> McpPythonScan::PositionalRotatorCalls(const FString& Code)
{
	using namespace McpPythonScanPrivate;

	TArray<FString> Offenders;
	TArray<FToken> Tokens;
	if (!Tokenize(Code, Tokens))
	{
		return Offenders; // let the editor report the syntax error, with its line number
	}
	for (const FCall& Call : FindCalls(Tokens))
	{
		if (!Call.Name.Equals(TEXT("Rotator"), ESearchCase::CaseSensitive) || Call.CloseIndex == INDEX_NONE)
		{
			continue;
		}
		int32 StartIndex = Call.NameIndex;
		if (Call.bAttribute)
		{
			// Only `unreal.Rotator`, and `unreal` must be a bare name (not `x.unreal`).
			const int32 Base = Call.NameIndex - 2;
			if (Base < 0 || !Is(Tokens[Base], ETokenType::Name, TEXT("unreal")))
			{
				continue;
			}
			if (Base > 0 && Is(Tokens[Base - 1], ETokenType::Op, TEXT(".")))
			{
				continue;
			}
			StartIndex = Base;
		}

		bool bStarred = false;
		TArray<FArg> Positional;
		for (const FArg& Arg : SplitArgs(Tokens, Call))
		{
			if (Arg.Kind == EArgKind::Starred)
			{
				bStarred = true; // `Rotator(*rot)` is undecidable: do not refuse blindly
			}
			else if (Arg.Kind == EArgKind::Positional)
			{
				Positional.Add(Arg);
			}
		}
		if (bStarred || Positional.Num() == 0)
		{
			continue;
		}
		bool bAllZero = true;
		for (const FArg& Arg : Positional)
		{
			bAllZero &= IsZeroLiteral(Tokens, Arg.Begin, Arg.End);
		}
		if (bAllZero)
		{
			continue; // Rotator(0, 0, 0) is unambiguous
		}
		Offenders.Add(Code.Mid(Tokens[StartIndex].Begin, Tokens[Call.CloseIndex].End - Tokens[StartIndex].Begin));
	}
	return Offenders;
}

TArray<FString> McpPythonScan::PropertyWrites(const FString& Code, const FString& PropertyName)
{
	using namespace McpPythonScanPrivate;

	TArray<FString> Sites;
	TArray<FToken> Tokens;
	if (!Tokenize(Code, Tokens))
	{
		return Sites; // does not compile, so it does not run
	}

	for (const FCall& Call : FindCalls(Tokens))
	{
		if (!Call.Name.Equals(TEXT("set_editor_property"), ESearchCase::CaseSensitive))
		{
			continue;
		}
		const TArray<FArg> Args = SplitArgs(Tokens, Call);
		// The first positional argument (or a starred one, which is not a constant).
		const FArg* First = Args.FindByPredicate([](const FArg& Arg) { return Arg.Kind == EArgKind::Positional || Arg.Kind == EArgKind::Starred; });
		FString Value;
		if (First && First->Kind == EArgKind::Positional && AsConstantString(Tokens, First->Begin, First->End, Value)
			&& Value.Equals(PropertyName, ESearchCase::CaseSensitive))
		{
			Sites.Add(TEXT("set_editor_property (positional)"));
			continue;
		}
		for (const FArg& Arg : Args)
		{
			if (Arg.Kind == EArgKind::Keyword
				&& (Arg.KeywordName.Equals(TEXT("name"), ESearchCase::CaseSensitive) || Arg.KeywordName.Equals(TEXT("property_name"), ESearchCase::CaseSensitive))
				&& AsConstantString(Tokens, Arg.ValueBegin, Arg.End, Value)
				&& Value.Equals(PropertyName, ESearchCase::CaseSensitive))
			{
				Sites.Add(TEXT("set_editor_property (named)"));
			}
		}
	}

	// `x.<prop> = v` and `x.<prop> += v`. Reading it, comparing it (==) or using it as a subscript does not count.
	for (int32 Index = 0; Index + 2 < Tokens.Num(); ++Index)
	{
		if (Is(Tokens[Index], ETokenType::Op, TEXT(".")) && Tokens[Index + 1].Type == ETokenType::Name
			&& Tokens[Index + 1].Text.Equals(PropertyName, ESearchCase::CaseSensitive) && IsAssignOp(Tokens[Index + 2]))
		{
			Sites.Add(FString(TEXT("assignment to .")) + PropertyName);
		}
	}
	return Sites;
}
