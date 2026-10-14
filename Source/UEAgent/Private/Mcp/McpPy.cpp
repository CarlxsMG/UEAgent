#include "Mcp/McpPy.h"

#include <charconv>

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpPyPrivate
{
	static const FString EmptyString;

	static bool IsIntegral(double Value)
	{
		return FMath::IsFinite(Value) && Value == FMath::FloorToDouble(Value) && FMath::Abs(Value) < 9.2e18;
	}

	static FString IntText(double Value)
	{
		return FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	}

	/** Python: is this character written as-is inside a string repr? */
	static bool IsPrintable(TCHAR C)
	{
		if (C < 0x20 || C == 0x7F || (C >= 0x80 && C < 0xA0))
		{
			return false;
		}
		// Separators and format characters that Python escapes (Zl, Zp, common Cf, NBSP-like Zs).
		switch (C)
		{
		case 0x00A0: case 0x00AD: case 0x2028: case 0x2029: case 0x200B: case 0x200C: case 0x200D:
		case 0x200E: case 0x200F: case 0x2060: case 0xFEFF: case 0x3000: case 0x2007: case 0x202F:
			return false;
		default:
			return !(C >= 0x2000 && C <= 0x200A);
		}
	}

	static void AppendPadded(FString& Out, const FString& Text, int32 Width, bool bLeftAlign, TCHAR Pad = TEXT(' '))
	{
		const int32 Missing = Width - Text.Len();
		if (Missing <= 0)
		{
			Out += Text;
			return;
		}
		if (bLeftAlign)
		{
			Out += Text;
			Out += FString::ChrN(Missing, Pad);
		}
		else
		{
			Out += FString::ChrN(Missing, Pad);
			Out += Text;
		}
	}

	static FString JsonEscape(const FString& Value)
	{
		FString Out;
		Out.Reserve(Value.Len() + 2);
		Out.AppendChar(TEXT('"'));
		for (const TCHAR C : Value)
		{
			switch (C)
			{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<int32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
			}
		}
		Out.AppendChar(TEXT('"'));
		return Out;
	}
}

namespace McpPy
{
	using namespace McpPyPrivate;

	// ---------------------------------------------------------------------- construction
	FV None() { return MakeShared<FJsonValueNull>(); }
	FV S(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
	FV N(double Value) { return MakeShared<FJsonValueNumber>(Value); }
	FV B(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }
	FV L(const TArray<FV>& Values) { return MakeShared<FJsonValueArray>(Values); }

	FV L(const TArray<FString>& Values)
	{
		TArray<FV> Items;
		Items.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Items.Add(S(Value));
		}
		return L(Items);
	}

	FV O(const FO& Object) { return Object.IsValid() ? FV(MakeShared<FJsonValueObject>(Object)) : None(); }

	FO Dict(std::initializer_list<TPair<FString, FV>> Fields)
	{
		FO Object = MakeShared<FJsonObject>();
		for (const TPair<FString, FV>& Field : Fields)
		{
			Object->SetField(Field.Key, Field.Value.IsValid() ? Field.Value : None());
		}
		return Object;
	}

	// ---------------------------------------------------------------------- types
	bool IsNone(const FV& Value) { return !Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None; }
	bool IsStr(const FV& Value) { return Value.IsValid() && Value->Type == EJson::String; }
	bool IsBool(const FV& Value) { return Value.IsValid() && Value->Type == EJson::Boolean; }
	bool IsNum(const FV& Value) { return Value.IsValid() && Value->Type == EJson::Number; }
	bool IsInt(const FV& Value) { return IsNum(Value) && IsIntegral(Value->AsNumber()); }
	bool IsList(const FV& Value) { return Value.IsValid() && Value->Type == EJson::Array; }
	bool IsDict(const FV& Value) { return Value.IsValid() && Value->Type == EJson::Object && Value->AsObject().IsValid(); }
	bool IsTrue(const FV& Value) { return IsBool(Value) && Value->AsBool(); }
	bool IsFalse(const FV& Value) { return IsBool(Value) && !Value->AsBool(); }

	// ---------------------------------------------------------------------- access
	FV Get(const FO& Object, const FString& Key)
	{
		if (!Object.IsValid())
		{
			return None();
		}
		const FV Value = Object->TryGetField(Key);
		return Value.IsValid() ? Value : None();
	}

	FV Get(const FV& Object, const FString& Key) { return Get(AsDict(Object), Key); }
	FO AsDict(const FV& Value) { return IsDict(Value) ? Value->AsObject() : nullptr; }
	TArray<FV> AsList(const FV& Value) { return IsList(Value) ? Value->AsArray() : TArray<FV>(); }
	FString AsStr(const FV& Value) { return IsStr(Value) ? Value->AsString() : FString(); }
	double AsNum(const FV& Value) { return IsNum(Value) ? Value->AsNumber() : (IsBool(Value) ? (Value->AsBool() ? 1.0 : 0.0) : 0.0); }

	// ---------------------------------------------------------------------- semantics
	bool Truthy(const FV& Value)
	{
		if (IsNone(Value))
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Boolean: return Value->AsBool();
		case EJson::Number:  return Value->AsNumber() != 0.0;
		case EJson::String:  return !Value->AsString().IsEmpty();
		case EJson::Array:   return Value->AsArray().Num() > 0;
		case EJson::Object:  return Value->AsObject().IsValid() && Value->AsObject()->Values.Num() > 0;
		default:             return false;
		}
	}

	int32 Len(const FV& Value)
	{
		if (IsStr(Value)) { return Value->AsString().Len(); }
		if (IsList(Value)) { return Value->AsArray().Num(); }
		if (IsDict(Value)) { return Value->AsObject()->Values.Num(); }
		return 0;
	}

	FString FloatRepr(double Value)
	{
		if (FMath::IsNaN(Value))
		{
			return TEXT("nan");
		}
		if (!FMath::IsFinite(Value))
		{
			return Value > 0 ? TEXT("inf") : TEXT("-inf");
		}
		if (Value == 0.0)
		{
			return FMath::IsNegativeOrNegativeZero(Value) ? TEXT("-0.0") : TEXT("0.0");
		}
		// The shortest digits that round-trip to the same double, in scientific notation.
		char Buffer[64];
		const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), Value, std::chars_format::scientific);
		const FString Sci(static_cast<int32>(Result.ptr - Buffer), Buffer);
		bool bNegative = false;
		FString Mantissa = Sci;
		int32 EIndex = INDEX_NONE;
		Mantissa.FindChar(TEXT('e'), EIndex);
		const int32 Exponent = FCString::Atoi(*Sci.Mid(EIndex + 1));
		Mantissa = Sci.Left(EIndex);
		if (Mantissa.StartsWith(TEXT("-")))
		{
			bNegative = true;
			Mantissa = Mantissa.Mid(1);
		}
		const FString Digits = Mantissa.Replace(TEXT("."), TEXT(""));
		const int32 DecimalPoint = Exponent + 1; // position of the decimal point relative to the digits

		FString Out;
		if (DecimalPoint <= -4 || DecimalPoint > 16)
		{
			// Exponential notation, like repr: 1e+16, 1.5e-05.
			Out = Digits.Left(1);
			if (Digits.Len() > 1)
			{
				Out += TEXT(".") + Digits.Mid(1);
			}
			const int32 E = DecimalPoint - 1;
			Out += FString::Printf(TEXT("e%c%02d"), E < 0 ? TEXT('-') : TEXT('+'), FMath::Abs(E));
		}
		else if (DecimalPoint <= 0)
		{
			Out = TEXT("0.") + FString::ChrN(-DecimalPoint, TEXT('0')) + Digits;
		}
		else if (DecimalPoint >= Digits.Len())
		{
			Out = Digits + FString::ChrN(DecimalPoint - Digits.Len(), TEXT('0')) + TEXT(".0");
		}
		else
		{
			Out = Digits.Left(DecimalPoint) + TEXT(".") + Digits.Mid(DecimalPoint);
		}
		return bNegative ? TEXT("-") + Out : Out;
	}

	FString ReprStr(const FString& Value)
	{
		const TCHAR Quote = (Value.Contains(TEXT("'")) && !Value.Contains(TEXT("\""))) ? TEXT('"') : TEXT('\'');
		FString Out;
		Out.Reserve(Value.Len() + 2);
		Out.AppendChar(Quote);
		for (const TCHAR C : Value)
		{
			if (C == Quote || C == TEXT('\\'))
			{
				Out.AppendChar(TEXT('\\'));
				Out.AppendChar(C);
			}
			else if (C == TEXT('\n')) { Out += TEXT("\\n"); }
			else if (C == TEXT('\r')) { Out += TEXT("\\r"); }
			else if (C == TEXT('\t')) { Out += TEXT("\\t"); }
			else if (!IsPrintable(C))
			{
				Out += (C < 0x100) ? FString::Printf(TEXT("\\x%02x"), static_cast<int32>(C)) : FString::Printf(TEXT("\\u%04x"), static_cast<int32>(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		Out.AppendChar(Quote);
		return Out;
	}

	FString Str(const FV& Value)
	{
		if (IsNone(Value)) { return TEXT("None"); }
		switch (Value->Type)
		{
		case EJson::Boolean: return Value->AsBool() ? TEXT("True") : TEXT("False");
		case EJson::Number:  return IsIntegral(Value->AsNumber()) ? IntText(Value->AsNumber()) : FloatRepr(Value->AsNumber());
		case EJson::String:  return Value->AsString();
		default:             return Repr(Value);
		}
	}

	FString Repr(const FV& Value)
	{
		if (IsStr(Value))
		{
			return ReprStr(Value->AsString());
		}
		if (IsList(Value))
		{
			TArray<FString> Parts;
			for (const FV& Item : Value->AsArray())
			{
				Parts.Add(Repr(Item));
			}
			return TEXT("[") + FString::Join(Parts, TEXT(", ")) + TEXT("]");
		}
		if (IsDict(Value))
		{
			TArray<FString> Parts;
			for (const auto& Field : Value->AsObject()->Values)
			{
				Parts.Add(ReprStr(FString(Field.Key)) + TEXT(": ") + Repr(Field.Value));
			}
			return TEXT("{") + FString::Join(Parts, TEXT(", ")) + TEXT("}");
		}
		return Str(Value);
	}

	bool Equals(const FV& A, const FV& B)
	{
		if (IsNone(A) || IsNone(B))
		{
			return IsNone(A) && IsNone(B);
		}
		const bool bNumA = IsNum(A) || IsBool(A);
		const bool bNumB = IsNum(B) || IsBool(B);
		if (bNumA && bNumB)
		{
			return AsNum(A) == AsNum(B);
		}
		if (A->Type != B->Type)
		{
			return false;
		}
		switch (A->Type)
		{
		case EJson::String: return A->AsString().Equals(B->AsString(), ESearchCase::CaseSensitive);
		case EJson::Array:
		{
			const TArray<FV>& ItemsA = A->AsArray();
			const TArray<FV>& ItemsB = B->AsArray();
			if (ItemsA.Num() != ItemsB.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < ItemsA.Num(); ++Index)
			{
				if (!Equals(ItemsA[Index], ItemsB[Index]))
				{
					return false;
				}
			}
			return true;
		}
		case EJson::Object:
			return Canonical(A) == Canonical(B);
		default:
			return false;
		}
	}

	// ---------------------------------------------------------------------- text
	FString F(const TCHAR* Template, const TArray<FString>& Args)
	{
		FString Out;
		for (const TCHAR* P = Template; *P; ++P)
		{
			if (P[0] == TEXT('{') && P[1] == TEXT('{'))
			{
				Out.AppendChar(TEXT('{'));
				++P;
			}
			else if (P[0] == TEXT('}') && P[1] == TEXT('}'))
			{
				Out.AppendChar(TEXT('}'));
				++P;
			}
			else if (P[0] == TEXT('{'))
			{
				int32 Index = 0;
				const TCHAR* Q = P + 1;
				while (FChar::IsDigit(*Q))
				{
					Index = Index * 10 + (*Q - TEXT('0'));
					++Q;
				}
				if (*Q == TEXT('}'))
				{
					Out += Args.IsValidIndex(Index) ? Args[Index] : FString();
					P = Q;
				}
				else
				{
					Out.AppendChar(*P);
				}
			}
			else
			{
				Out.AppendChar(*P);
			}
		}
		return Out;
	}

	FString Percent(const TCHAR* Template, const TArray<FV>& Args)
	{
		FString Out;
		int32 ArgIndex = 0;
		for (const TCHAR* P = Template; *P; ++P)
		{
			if (*P != TEXT('%'))
			{
				Out.AppendChar(*P);
				continue;
			}
			++P;
			if (*P == TEXT('%'))
			{
				Out.AppendChar(TEXT('%'));
				continue;
			}
			bool bLeft = false;
			bool bZero = false;
			while (*P == TEXT('-') || *P == TEXT('0') || *P == TEXT('+') || *P == TEXT(' '))
			{
				bLeft |= *P == TEXT('-');
				bZero |= *P == TEXT('0');
				++P;
			}
			int32 Width = 0;
			while (FChar::IsDigit(*P))
			{
				Width = Width * 10 + (*P - TEXT('0'));
				++P;
			}
			int32 Precision = -1;
			if (*P == TEXT('.'))
			{
				++P;
				Precision = 0;
				while (FChar::IsDigit(*P))
				{
					Precision = Precision * 10 + (*P - TEXT('0'));
					++P;
				}
			}
			const FV Arg = Args.IsValidIndex(ArgIndex) ? Args[ArgIndex] : None();
			++ArgIndex;
			FString Text;
			switch (*P)
			{
			case TEXT('s'):
				Text = Str(Arg);
				if (Precision >= 0) { Text.LeftInline(Precision); }
				break;
			case TEXT('r'):
				Text = Repr(Arg);
				if (Precision >= 0) { Text.LeftInline(Precision); }
				break;
			case TEXT('d'):
			case TEXT('i'):
				Text = FString::Printf(TEXT("%lld"), static_cast<long long>(AsNum(Arg)));
				break;
			case TEXT('x'):
				Text = FString::Printf(TEXT("%llx"), static_cast<long long>(AsNum(Arg)));
				break;
			case TEXT('f'):
				Text = FString::Printf(TEXT("%.*f"), Precision < 0 ? 6 : Precision, AsNum(Arg));
				break;
			case TEXT('e'):
				Text = FString::Printf(TEXT("%.*e"), Precision < 0 ? 6 : Precision, AsNum(Arg));
				break;
			case TEXT('g'):
				Text = FString::Printf(TEXT("%.*g"), Precision < 0 ? 6 : Precision, AsNum(Arg));
				break;
			default:
				Text = FString(TEXT("%")) + *P; // unsupported specifier: left visible
				break;
			}
			AppendPadded(Out, Text, Width, bLeft, (bZero && !bLeft) ? TEXT('0') : TEXT(' '));
			if (*P == TEXT('\0'))
			{
				break;
			}
		}
		return Out;
	}

	FString Join(const TArray<FString>& Parts, const FString& Separator) { return FString::Join(Parts, *Separator); }

	TArray<FString> Sorted(TArray<FString> Values)
	{
		Values.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
		return Values;
	}

	TArray<FString> Split(const FString& Value, const FString& Separator, int32 MaxSplit)
	{
		TArray<FString> Parts;
		if (Separator.IsEmpty())
		{
			Parts.Add(Value);
			return Parts;
		}
		int32 Start = 0;
		while (MaxSplit < 0 || Parts.Num() < MaxSplit)
		{
			const int32 Found = Value.Find(Separator, ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
			if (Found == INDEX_NONE)
			{
				break;
			}
			Parts.Add(Value.Mid(Start, Found - Start));
			Start = Found + Separator.Len();
		}
		Parts.Add(Value.Mid(Start));
		return Parts;
	}

	TArray<FString> RSplit(const FString& Value, const FString& Separator, int32 MaxSplit)
	{
		if (MaxSplit < 0)
		{
			return Split(Value, Separator, -1);
		}
		TArray<FString> Parts;
		int32 End = Value.Len();
		while (Parts.Num() < MaxSplit)
		{
			const int32 Found = Value.Find(Separator, ESearchCase::CaseSensitive, ESearchDir::FromEnd, End);
			if (Found == INDEX_NONE)
			{
				break;
			}
			Parts.Insert(Value.Mid(Found + Separator.Len(), End - Found - Separator.Len()), 0);
			End = Found;
		}
		Parts.Insert(Value.Left(End), 0);
		return Parts;
	}

	bool StrEq(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
	bool StartsWith(const FString& Value, const FString& Prefix) { return Value.StartsWith(Prefix, ESearchCase::CaseSensitive); }
	bool EndsWith(const FString& Value, const FString& Suffix) { return Value.EndsWith(Suffix, ESearchCase::CaseSensitive); }
	bool Contains(const FString& Value, const FString& Needle) { return Value.Contains(Needle, ESearchCase::CaseSensitive); }

	bool InList(const TArray<FString>& Values, const FString& Value)
	{
		return Values.ContainsByPredicate([&Value](const FString& Candidate) { return Candidate.Equals(Value, ESearchCase::CaseSensitive); });
	}

	FString Strip(const FString& Value) { return Value.TrimStartAndEnd(); }
	FString Lower(const FString& Value) { return Value.ToLower(); }

	// ---------------------------------------------------------------------- helpers
	/**
	 * Is `data` ONLY the parameter report the bridge adds (aliased_params / unknown_params, in
	 * DispatchSingleCommand)? Then it is not the payload: the bridge creates it on a response that
	 * had no `data`, and taking it for the payload would leave the checks reading an object without
	 * a single field from the handler.
	 */
	static bool IsOnlyBridgeParamReport(const FO& Data)
	{
		if (!Data.IsValid() || Data->Values.Num() == 0)
		{
			return false;
		}
		for (const auto& Field : Data->Values)
		{
			const FString Key(Field.Key);
			if (!StrEq(Key, TEXT("aliased_params")) && !StrEq(Key, TEXT("unknown_params")))
			{
				return false;
			}
		}
		return true;
	}

	FO Payload(const FO& Reply)
	{
		if (!Reply.IsValid())
		{
			return MakeShared<FJsonObject>();
		}
		const FV Inner = Reply->TryGetField(TEXT("data"));
		if (IsDict(Inner) && !IsOnlyBridgeParamReport(Inner->AsObject()))
		{
			return Inner->AsObject();
		}
		return Reply;
	}

	FO Payload(const FV& Reply) { return IsDict(Reply) ? Payload(Reply->AsObject()) : MakeShared<FJsonObject>(); }

	FString TextOf(const FV& Value) { return IsStr(Value) ? Strip(Value->AsString()) : FString(); }

	bool IsEmpty(const FV& Value)
	{
		if (IsNone(Value)) { return true; }
		if (IsBool(Value) || IsNum(Value)) { return false; }
		if (IsStr(Value))
		{
			const FString Text = Lower(Strip(Value->AsString()));
			return Text.IsEmpty() || Text == TEXT("()") || Text == TEXT("none") || Text == TEXT("null");
		}
		if (IsList(Value) || IsDict(Value)) { return Len(Value) == 0; }
		return false;
	}

	FV Clone(const FV& Value)
	{
		if (!Value.IsValid())
		{
			return None();
		}
		switch (Value->Type)
		{
		case EJson::Array:
		{
			TArray<FV> Items;
			for (const FV& Item : Value->AsArray())
			{
				Items.Add(Clone(Item));
			}
			return L(Items);
		}
		case EJson::Object: return O(Clone(Value->AsObject()));
		case EJson::String: return S(Value->AsString());
		case EJson::Number: return N(Value->AsNumber());
		case EJson::Boolean: return B(Value->AsBool());
		default: return None();
		}
	}

	FO Clone(const FO& Value)
	{
		FO Copy = MakeShared<FJsonObject>();
		if (Value.IsValid())
		{
			for (const auto& Field : Value->Values)
			{
				Copy->SetField(FString(Field.Key), Clone(Field.Value));
			}
		}
		return Copy;
	}

	FString Canonical(const FV& Value)
	{
		if (IsNone(Value))
		{
			return TEXT("null");
		}
		switch (Value->Type)
		{
		case EJson::Object:
		{
			const FO Object = Value->AsObject();
			TArray<FString> Keys;
			for (const auto& Field : Object->Values)
			{
				Keys.Add(FString(Field.Key));
			}
			Keys = Sorted(Keys);
			TArray<FString> Parts;
			for (const FString& Key : Keys)
			{
				Parts.Add(JsonEscape(Key) + TEXT(":") + Canonical(Object->TryGetField(Key)));
			}
			return TEXT("{") + FString::Join(Parts, TEXT(",")) + TEXT("}");
		}
		case EJson::Array:
		{
			TArray<FString> Parts;
			for (const FV& Item : Value->AsArray())
			{
				Parts.Add(Canonical(Item));
			}
			return TEXT("[") + FString::Join(Parts, TEXT(",")) + TEXT("]");
		}
		case EJson::String:  return JsonEscape(Value->AsString());
		case EJson::Number:  return IsIntegral(Value->AsNumber()) ? IntText(Value->AsNumber()) : FloatRepr(Value->AsNumber());
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		default:             return TEXT("null");
		}
	}
}
