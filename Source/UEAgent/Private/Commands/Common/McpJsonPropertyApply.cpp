// ============================================================================================
// THE FAILURE THAT JUSTIFIES THIS FILE
// ============================================================================================
//
// mcp_common::SetObjectProperty (McpCommonUtils.cpp) delegates to the engine's
// FJsonObjectConverter with bStrictMode=false. The verbatim investigation of that behavior is in
// Private/Commands/UI/UEAgentWidgetCommands.cpp, reading
// Engine/Source/Runtime/JsonUtilities/Private/JsonObjectConverter.cpp
// (JsonAttributesToUStructWithContainer): the converter ITERATES THE STRUCT'S PROPERTIES and looks
// each one up in the JSON object. Direct consequence: if the JSON carries names that match no
// property, the loop finds nothing to copy, there is no error to report in non-strict mode, and
// the call RETURNS TRUE HAVING WRITTEN ZERO FIELDS.
//
// The concrete case: {"R":1,"G":0,"B":0} against an FSlateColor, whose reflected
// fields are SpecifiedColor (FLinearColor) and ColorUseRule (enum) -- neither "R", "G" nor "B"
// exists there. UTextBlock::ColorAndOpacity is exactly that type, so the silent no-op
// reports property_set:true while the widget's color stays the same. The same hole exists
// nested inside another struct (WidgetStyle.Normal.TintColor), so a TOP-LEVEL special case
// does not close it.
//
// => THIS FILE INVERTS THE DIRECTION OF THE ITERATION: it walks THE JSON KEYS and looks up
//   the FProperty for each one. A key with no matching FProperty cannot go unnoticed: it is an
//   entry in Report.Ignored. And no code path returns true without having added something to
//   Report.Applied, so "success" never means "zero writes".
//
// The convenience shortcuts (flat FSlateColor, uniform FMargin, FSlateBrush from a path...)
// apply AT ANY DEPTH because the dispatch is recursive: the shortcut lives in the handling of
// the type, not in a top-level pre-check.
//
// File helpers carry the _JsonApply suffix on purpose: with bUseUnity=true (UEAgent.Build.cs)
// several .cpp files merge into one blob and anonymous-namespace names collide.
// ============================================================================================

#include "Commands/Common/McpJsonPropertyApply.h"

#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"
#include "UObject/Class.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "Engine/Blueprint.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/Font.h"
#include "Engine/FontFace.h"
#include "Materials/MaterialInterface.h"
#include "GameplayTagContainer.h"

// Slate/UMG: they go here, not in the public header (see the comment in McpJsonPropertyApply.h).
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "Fonts/SlateFontInfo.h"
#include "Layout/Margin.h"

namespace mcp_common
{
namespace
{
    // ----------------------------------------------------------------------------------------
    // Reading JSON values without lies
    // ----------------------------------------------------------------------------------------

    // FJsonValue::AsNumber() silently returns 0 when the value is not a number, which is
    // exactly the kind of silence this file exists to eliminate. Every number is read
    // through here: it accepts number, bool and numeric string, and fails explicitly otherwise.
    bool TryReadNumber_JsonApply(const TSharedPtr<FJsonValue>& Value, double& OutNumber)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Number)
        {
            OutNumber = Value->AsNumber();
            return true;
        }
        if (Value->Type == EJson::Boolean)
        {
            OutNumber = Value->AsBool() ? 1.0 : 0.0;
            return true;
        }
        if (Value->Type == EJson::String)
        {
            const FString Text = Value->AsString().TrimStartAndEnd();
            return !Text.IsEmpty() && LexTryParseString(OutNumber, *Text);
        }
        return false;
    }

    bool TryReadBool_JsonApply(const TSharedPtr<FJsonValue>& Value, bool& OutBool)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        if (Value->Type == EJson::Boolean)
        {
            OutBool = Value->AsBool();
            return true;
        }
        if (Value->Type == EJson::Number)
        {
            OutBool = Value->AsNumber() != 0.0;
            return true;
        }
        if (Value->Type == EJson::String)
        {
            // FString::ToBool() returns false for anything it does not recognize, so the
            // list of accepted literals is checked by hand instead of trusting it.
            const FString Text = Value->AsString().TrimStartAndEnd();
            if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("yes"), ESearchCase::IgnoreCase)
                || Text.Equals(TEXT("on"), ESearchCase::IgnoreCase) || Text == TEXT("1"))
            {
                OutBool = true;
                return true;
            }
            if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("no"), ESearchCase::IgnoreCase)
                || Text.Equals(TEXT("off"), ESearchCase::IgnoreCase) || Text == TEXT("0"))
            {
                OutBool = false;
                return true;
            }
        }
        return false;
    }

    const TCHAR* JsonTypeName_JsonApply(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("<absent>");
        }
        switch (Value->Type)
        {
        case EJson::Null:    return TEXT("null");
        case EJson::String:  return TEXT("string");
        case EJson::Number:  return TEXT("number");
        case EJson::Boolean: return TEXT("bool");
        case EJson::Array:   return TEXT("array");
        case EJson::Object:  return TEXT("object");
        default:             return TEXT("<unknown>");
        }
    }

    // Text representation for ImportText_Direct. Only meaningful for scalars: an
    // array/object is not converted but rejected, so "[object]" never reaches the property.
    bool TryReadScalarAsText_JsonApply(const TSharedPtr<FJsonValue>& Value, FString& OutText)
    {
        if (!Value.IsValid())
        {
            return false;
        }
        switch (Value->Type)
        {
        case EJson::String:  OutText = Value->AsString();                       return true;
        case EJson::Number:  OutText = LexToString(Value->AsNumber());          return true;
        case EJson::Boolean: OutText = Value->AsBool() ? TEXT("true") : TEXT("false"); return true;
        case EJson::Null:    OutText.Reset();                                   return true;
        default:             return false;
        }
    }

    // ----------------------------------------------------------------------------------------
    // Report paths
    // ----------------------------------------------------------------------------------------

    // With an empty PathPrefix the report would hold "" entries -- useless to the caller. It is
    // replaced by the property name, which is the best description available there.
    FString ReportPath_JsonApply(const FString& PathPrefix, const FProperty* Property)
    {
        if (!PathPrefix.IsEmpty())
        {
            return PathPrefix;
        }
        return Property ? Property->GetName() : FString(TEXT("<property>"));
    }

    FString ChildPath_JsonApply(const FString& PathPrefix, const FString& Key)
    {
        return PathPrefix.IsEmpty() ? Key : PathPrefix + TEXT(".") + Key;
    }

    void ReportError_JsonApply(FJsonApplyReport& Report, const FString& Path, const FString& Reason)
    {
        Report.Errors.Add(TPair<FString, FString>(Path, Reason));
    }

    // ----------------------------------------------------------------------------------------
    // Lenient property lookup
    // ----------------------------------------------------------------------------------------

    // FName comparison is case-insensitive by construction (NameTypes.h
    // "Names are case-insensitive, but case-preserving"), so FindPropertyByName ALREADY covers
    // "tintcolor" -> TintColor without iterating anything. What it does NOT cover is UE's `b`
    // convention for booleans, and that is what the two extra attempts add.
    FProperty* FindPropertyRelaxed_JsonApply(UStruct* OwnerStruct, const FString& Key)
    {
        if (!OwnerStruct || Key.IsEmpty())
        {
            return nullptr;
        }

        if (FProperty* Exact = OwnerStruct->FindPropertyByName(FName(*Key)))
        {
            return Exact;
        }

        // "IsEnabled" -> bIsEnabled
        if (FProperty* Prefixed = OwnerStruct->FindPropertyByName(FName(*(TEXT("b") + Key))))
        {
            if (CastField<FBoolProperty>(Prefixed))
            {
                return Prefixed;
            }
        }

        // The reverse: the caller writes "bIsEnabled" and the property is named IsEnabled.
        if (Key.Len() > 1 && (Key[0] == TEXT('b') || Key[0] == TEXT('B')))
        {
            if (FProperty* Stripped = OwnerStruct->FindPropertyByName(FName(*Key.RightChop(1))))
            {
                if (CastField<FBoolProperty>(Stripped))
                {
                    return Stripped;
                }
            }
        }

        return nullptr;
    }

    // Names available at one level, so the error says what COULD be written.
    FString DescribeAvailableProperties_JsonApply(UStruct* OwnerStruct, int32 MaxNames = 24)
    {
        if (!OwnerStruct)
        {
            return FString();
        }
        TArray<FString> Names;
        for (TFieldIterator<FProperty> It(OwnerStruct); It && Names.Num() < MaxNames; ++It)
        {
            Names.Add(It->GetName());
        }
        if (Names.Num() == 0)
        {
            return FString();
        }
        FString Joined = FString::Join(Names, TEXT(", "));
        if (Names.Num() >= MaxNames)
        {
            Joined += TEXT(", ...");
        }
        return Joined;
    }

    // ----------------------------------------------------------------------------------------
    // Text import (the last resort for any type)
    // ----------------------------------------------------------------------------------------

    // ImportText_Direct returns nullptr when it cannot parse (UnrealType.h). It is given an
    // FStringOutputDevice instead of GWarn so the REAL reason goes into the report instead of
    // being buried in the log. OwnerObject is null: only instanced properties use it to
    // pick an outer, and here the writes go to loose value pointers.
    bool ImportTextInto_JsonApply(FProperty* Property, void* ValuePtr, const FString& Text, FString& OutReason)
    {
        if (!Property || !ValuePtr)
        {
            OutReason = TEXT("null property or value pointer");
            return false;
        }

        FStringOutputDevice ErrorCapture;
        const TCHAR* Consumed = Property->ImportText_Direct(*Text, ValuePtr, nullptr, PPF_None, &ErrorCapture);
        if (Consumed == nullptr)
        {
            OutReason = ErrorCapture.IsEmpty()
                ? FString::Printf(TEXT("'%s' is not a valid value for type %s"), *Text, *Property->GetCPPType())
                : FString::Printf(TEXT("'%s' rejected by type %s: %s"), *Text, *Property->GetCPPType(), *ErrorCapture);
            return false;
        }
        return true;
    }

    // ----------------------------------------------------------------------------------------
    // Enums
    // ----------------------------------------------------------------------------------------

    FString DescribeEnumValues_JsonApply(const UEnum* Enum, int32 MaxNames = 32)
    {
        if (!Enum)
        {
            return FString();
        }
        TArray<FString> Names;
        const int32 Count = Enum->NumEnums();
        for (int32 Index = 0; Index < Count && Names.Num() < MaxNames; ++Index)
        {
            const FString Name = Enum->GetNameStringByIndex(Index);
            // The synthetic _MAX that UHT appends is not a value anyone should write.
            if (Name.IsEmpty() || Name.EndsWith(TEXT("_MAX")))
            {
                continue;
            }
            Names.Add(Name);
        }
        FString Joined = FString::Join(Names, TEXT(", "));
        if (Names.Num() >= MaxNames)
        {
            Joined += TEXT(", ...");
        }
        return Joined;
    }

    // Accepts a short name ("Box"), a full name ("ESlateBrushDrawType::Box") or an integer.
    // A name the enum does not have NEVER becomes a silent 0: it is a failure with the
    // list of valid values in the reason.
    bool ResolveEnumValue_JsonApply(const UEnum* Enum, const TSharedPtr<FJsonValue>& Value,
                                    int64& OutValue, FString& OutReason)
    {
        if (!Enum)
        {
            OutReason = TEXT("the property does not declare a UEnum");
            return false;
        }
        if (!Value.IsValid())
        {
            OutReason = TEXT("missing JSON value");
            return false;
        }

        if (Value->Type == EJson::String)
        {
            const FString Name = Value->AsString().TrimStartAndEnd();
            // GetValueByNameString handles short and full names (Class.h) and returns
            // INDEX_NONE without logging as long as ErrorIfNotFound is not passed.
            const int64 ByName = Enum->GetValueByNameString(Name);
            if (ByName != INDEX_NONE)
            {
                OutValue = ByName;
                return true;
            }
            // An integer that arrives as text is still an integer.
            int64 Parsed = 0;
            if (LexTryParseString(Parsed, *Name) && Enum->IsValidEnumValue(Parsed))
            {
                OutValue = Parsed;
                return true;
            }
            OutReason = FString::Printf(TEXT("'%s' is not a value of %s. Valid: %s"),
                                        *Name, *Enum->GetName(), *DescribeEnumValues_JsonApply(Enum));
            return false;
        }

        double AsNumber = 0.0;
        if (TryReadNumber_JsonApply(Value, AsNumber))
        {
            const int64 Candidate = static_cast<int64>(AsNumber);
            // IsValidEnumValueOrBitfield, so combinations of a UENUM(Flags) are not rejected.
            if (!Enum->IsValidEnumValueOrBitfield(Candidate))
            {
                OutReason = FString::Printf(TEXT("%lld is not a value of %s. Valid: %s"),
                                            Candidate, *Enum->GetName(), *DescribeEnumValues_JsonApply(Enum));
                return false;
            }
            OutValue = Candidate;
            return true;
        }

        OutReason = FString::Printf(TEXT("expected the name or number of a value of %s, got %s"),
                                    *Enum->GetName(), JsonTypeName_JsonApply(Value));
        return false;
    }

    // ----------------------------------------------------------------------------------------
    // Colors
    // ----------------------------------------------------------------------------------------

    bool TryParseHexColor_JsonApply(const FString& Raw, FColor& OutSrgb)
    {
        FString Hex = Raw.TrimStartAndEnd();
        if (Hex.StartsWith(TEXT("#")))
        {
            Hex.RightChopInline(1);
        }
        else if (Hex.StartsWith(TEXT("0x"), ESearchCase::IgnoreCase))
        {
            Hex.RightChopInline(2);
        }

        if (Hex.Len() != 6 && Hex.Len() != 8)
        {
            return false;
        }
        for (int32 Index = 0; Index < Hex.Len(); ++Index)
        {
            const TCHAR Char = Hex[Index];
            const bool bHexDigit = (Char >= TEXT('0') && Char <= TEXT('9'))
                                || (Char >= TEXT('a') && Char <= TEXT('f'))
                                || (Char >= TEXT('A') && Char <= TEXT('F'));
            if (!bHexDigit)
            {
                return false;
            }
        }
        // FColor::FromHex silently returns black on garbage, which is why validation comes first.
        OutSrgb = FColor::FromHex(Hex);
        return true;
    }

    // The channels of [r,g,b(,a)] or {R,G,B[,A]}; a channel the value omits keeps the caller's value.
    bool TryReadColorChannels_JsonApply(const TSharedPtr<FJsonValue>& Value, double& R, double& G, double& B, double& A)
    {
        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
            if (Items.Num() < 3)
            {
                return false;
            }
            if (!TryReadNumber_JsonApply(Items[0], R) || !TryReadNumber_JsonApply(Items[1], G)
                || !TryReadNumber_JsonApply(Items[2], B))
            {
                return false;
            }
            if (Items.Num() >= 4 && !TryReadNumber_JsonApply(Items[3], A))
            {
                return false;
            }
            return true;
        }

        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            if (!Object.IsValid())
            {
                return false;
            }
            if (!Object->HasField(TEXT("R")) && !Object->HasField(TEXT("G")) && !Object->HasField(TEXT("B")))
            {
                return false;
            }
            Object->TryGetNumberField(TEXT("R"), R);
            Object->TryGetNumberField(TEXT("G"), G);
            Object->TryGetNumberField(TEXT("B"), B);
            Object->TryGetNumberField(TEXT("A"), A);
            return true;
        }

        return false;
    }

    // Object {R,G,B[,A]}, array [r,g,b(,a)] or string "#RRGGBB[AA]".
    // It deliberately does NOT recognize the full form {"SpecifiedColor":...,"ColorUseRule":...}:
    // that one goes through the generic recursion, which handles it.
    bool TryParseLinearColor_JsonApply(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutColor)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        if (Value->Type == EJson::String)
        {
            FColor Srgb;
            if (!TryParseHexColor_JsonApply(Value->AsString(), Srgb))
            {
                return false;
            }
            // A hand-written hex is sRGB (it is what any color picker shows), so
            // converting is correct: assigning it raw would give a visibly lighter color.
            OutColor = FLinearColor::FromSRGBColor(Srgb);
            return true;
        }

        double R = 0.0;
        double G = 0.0;
        double B = 0.0;
        double A = 1.0;
        if (!TryReadColorChannels_JsonApply(Value, R, G, B, A))
        {
            return false;
        }
        OutColor = FLinearColor(static_cast<float>(R), static_cast<float>(G),
                                static_cast<float>(B), static_cast<float>(A));
        return true;
    }

    // No FMath::RoundToInt on purpose: its float/double overloads return different types and
    // the Clamp after it becomes ambiguous. Clamping and rounding by hand is enough here.
    uint8 ToByteChannel_JsonApply(double Value)
    {
        const double Rounded = Value < 0.0 ? 0.0 : (Value > 255.0 ? 255.0 : Value + 0.5);
        return static_cast<uint8>(Rounded);
    }

    // FColor is four uint8s: its numbers are read in 0..255, which is its native semantics.
    bool TryParseColor_JsonApply(const TSharedPtr<FJsonValue>& Value, FColor& OutColor)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        if (Value->Type == EJson::String)
        {
            return TryParseHexColor_JsonApply(Value->AsString(), OutColor);
        }

        double R = 0.0;
        double G = 0.0;
        double B = 0.0;
        double A = 255.0;
        if (!TryReadColorChannels_JsonApply(Value, R, G, B, A))
        {
            return false;
        }
        OutColor = FColor(ToByteChannel_JsonApply(R), ToByteChannel_JsonApply(G),
                          ToByteChannel_JsonApply(B), ToByteChannel_JsonApply(A));
        return true;
    }

    // ----------------------------------------------------------------------------------------
    // Margins and vectors
    // ----------------------------------------------------------------------------------------

    // Bare number = uniform; [h,v]; [l,t,r,b]. The object {Left,Top,Right,Bottom} is NOT caught
    // here: its reflected fields are named exactly that (Layout/Margin.h), so the generic
    // recursion writes it correctly AND also reports a typo in any of the four.
    bool TryParseMargin_JsonApply(const TSharedPtr<FJsonValue>& Value, FMargin& OutMargin)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        double Uniform = 0.0;
        if (Value->Type != EJson::Array && Value->Type != EJson::Object && TryReadNumber_JsonApply(Value, Uniform))
        {
            OutMargin = FMargin(static_cast<float>(Uniform));
            return true;
        }

        if (Value->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
            TArray<double> Numbers;
            Numbers.Reserve(Items.Num());
            for (const TSharedPtr<FJsonValue>& Item : Items)
            {
                double Number = 0.0;
                if (!TryReadNumber_JsonApply(Item, Number))
                {
                    return false;
                }
                Numbers.Add(Number);
            }
            if (Numbers.Num() == 1)
            {
                OutMargin = FMargin(static_cast<float>(Numbers[0]));
                return true;
            }
            if (Numbers.Num() == 2)
            {
                OutMargin = FMargin(static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]));
                return true;
            }
            if (Numbers.Num() == 4)
            {
                OutMargin = FMargin(static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]),
                                    static_cast<float>(Numbers[2]), static_cast<float>(Numbers[3]));
                return true;
            }
        }

        return false;
    }

    bool TryReadNumberArray_JsonApply(const TSharedPtr<FJsonValue>& Value, TArray<double>& OutNumbers)
    {
        if (!Value.IsValid() || Value->Type != EJson::Array)
        {
            return false;
        }
        OutNumbers.Reset();
        for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
        {
            double Number = 0.0;
            if (!TryReadNumber_JsonApply(Item, Number))
            {
                return false;
            }
            OutNumbers.Add(Number);
        }
        return true;
    }

    // ----------------------------------------------------------------------------------------
    // Asset loading
    // ----------------------------------------------------------------------------------------

    // Accepts "/Game/Foo.Foo", the shorthand "/Game/Foo" and the exported form Class'/Game/Foo.Foo_C'.
    UObject* LoadObjectFromPath_JsonApply(const FString& RawPath, FString& OutReason)
    {
        FString Path = RawPath.TrimStartAndEnd();

        int32 FirstQuote = INDEX_NONE;
        int32 LastQuote = INDEX_NONE;
        if (Path.FindChar(TEXT('\''), FirstQuote) && Path.FindLastChar(TEXT('\''), LastQuote) && LastQuote > FirstQuote)
        {
            Path = Path.Mid(FirstQuote + 1, LastQuote - FirstQuote - 1);
        }
        if (Path.IsEmpty())
        {
            OutReason = TEXT("empty asset path");
            return nullptr;
        }

        if (UObject* Direct = FSoftObjectPath(Path).TryLoad())
        {
            return Direct;
        }

        // "/Game/Foo" without an object name is what everyone writes: normalize to
        // "/Game/Foo.Foo" before giving up.
        if (!Path.Contains(TEXT(".")))
        {
            const FString ShortName = FPackageName::GetShortName(Path);
            if (!ShortName.IsEmpty())
            {
                if (UObject* Normalized = FSoftObjectPath(Path + TEXT(".") + ShortName).TryLoad())
                {
                    return Normalized;
                }
            }
        }

        if (UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *Path))
        {
            return Loaded;
        }

        OutReason = FString::Printf(TEXT("could not load any object at '%s'"), *RawPath);
        return nullptr;
    }

    // A Blueprint path loads the UBlueprint, not its class: a class property needs the
    // GeneratedClass, and getting this wrong is the most common caller error.
    UClass* ResolveClassFromObject_JsonApply(UObject* Loaded)
    {
        if (!Loaded)
        {
            return nullptr;
        }
        if (UClass* AsClass = Cast<UClass>(Loaded))
        {
            return AsClass;
        }
        if (UBlueprint* AsBlueprint = Cast<UBlueprint>(Loaded))
        {
            return AsBlueprint->GeneratedClass;
        }
        return nullptr;
    }

    // The object a reference property is set to: null (JSON null or an empty path) clears it; otherwise the
    // loaded asset, or its class when ClassProperty is set. False after reporting a value that does not fit.
    template <typename ClassPropertyType>
    bool ResolveReferenceValue_JsonApply(FObjectPropertyBase* ReferenceProperty, ClassPropertyType* ClassProperty,
                                         const TSharedPtr<FJsonValue>& Value, const TCHAR* ReferenceKind,
                                         const FString& Path, FJsonApplyReport& Report, UObject*& OutTarget)
    {
        OutTarget = nullptr;
        if (Value->Type == EJson::Null)
        {
            return true;
        }
        if (Value->Type != EJson::String)
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("%s is written as the asset path in a string (or null to clear it), got %s"),
                                ReferenceKind, JsonTypeName_JsonApply(Value)));
            return false;
        }

        const FString AssetPath = Value->AsString().TrimStartAndEnd();
        if (AssetPath.IsEmpty())
        {
            return true;
        }

        FString Reason;
        UObject* Loaded = LoadObjectFromPath_JsonApply(AssetPath, Reason);
        if (!Loaded)
        {
            ReportError_JsonApply(Report, Path, Reason);
            return false;
        }

        if (ClassProperty)
        {
            UClass* AsClass = ResolveClassFromObject_JsonApply(Loaded);
            if (!AsClass)
            {
                ReportError_JsonApply(Report, Path,
                    FString::Printf(TEXT("'%s' is a %s and this property expects a class"),
                                    *AssetPath, *Loaded->GetClass()->GetName()));
                return false;
            }
            if (ClassProperty->MetaClass && !AsClass->IsChildOf(ClassProperty->MetaClass))
            {
                ReportError_JsonApply(Report, Path,
                    FString::Printf(TEXT("class '%s' does not derive from %s"),
                                    *AsClass->GetName(), *ClassProperty->MetaClass->GetName()));
                return false;
            }
            OutTarget = AsClass;
            return true;
        }

        if (ReferenceProperty->PropertyClass && !Loaded->IsA(ReferenceProperty->PropertyClass))
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("'%s' is a %s and this property expects a %s"),
                                *AssetPath, *Loaded->GetClass()->GetName(),
                                *ReferenceProperty->PropertyClass->GetName()));
            return false;
        }
        OutTarget = Loaded;
        return true;
    }

    // ----------------------------------------------------------------------------------------
    // Structs: shortcuts + generic recursion
    // ----------------------------------------------------------------------------------------

    // Stores a whole value of the struct's type and reports the struct as applied.
    template <typename StructType>
    bool WriteStructValue_JsonApply(void* StructPtr, StructType NewValue, const FString& PathPrefix, FJsonApplyReport& Report)
    {
        *static_cast<StructType*>(StructPtr) = MoveTemp(NewValue);
        Report.Applied.Add(PathPrefix);
        return true;
    }

    // Ordered distribution of an array of numbers over a struct's numeric fields. Covers the
    // LWC variants (FVector3f, FRotator3f...) that do not match the core struct by pointer, without
    // inventing a cast: each field is written through its own FNumericProperty.
    bool ApplyNumberArrayToNumericStruct_JsonApply(UScriptStruct* Struct, void* StructPtr,
                                                   const TArray<double>& Numbers, const FString& PathPrefix,
                                                   FJsonApplyReport& Report)
    {
        if (!Struct || !StructPtr || Numbers.Num() == 0)
        {
            return false;
        }

        TArray<FNumericProperty*> Fields;
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            FNumericProperty* Numeric = CastField<FNumericProperty>(*It);
            if (!Numeric)
            {
                // Something is not a number: this is not a "vector" struct and a
                // positional distribution would mean nothing.
                return false;
            }
            Fields.Add(Numeric);
        }
        if (Fields.Num() == 0 || Numbers.Num() > Fields.Num())
        {
            return false;
        }

        for (int32 Index = 0; Index < Numbers.Num(); ++Index)
        {
            FNumericProperty* Field = Fields[Index];
            void* FieldPtr = Field->ContainerPtrToValuePtr<void>(StructPtr);
            if (Field->IsFloatingPoint())
            {
                Field->SetFloatingPointPropertyValue(FieldPtr, Numbers[Index]);
            }
            else
            {
                Field->SetIntPropertyValue(FieldPtr, static_cast<int64>(Numbers[Index]));
            }
            Report.Applied.Add(ChildPath_JsonApply(PathPrefix, Field->GetName()));
        }
        return true;
    }

    // The heart of the inversion: THE JSON KEYS are walked. Each key with no matching FProperty
    // goes to Ignored, and the whole only returns true if some leaf was actually written.
    bool ApplyJsonObjectToStruct_JsonApply(UScriptStruct* Struct, void* StructPtr,
                                           const TSharedPtr<FJsonObject>& Object, const FString& PathPrefix,
                                           FJsonApplyReport& Report)
    {
        if (!Object.IsValid())
        {
            ReportError_JsonApply(Report, PathPrefix, TEXT("invalid JSON object"));
            return false;
        }
        if (Object->Values.Num() == 0)
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("empty JSON object: nothing to write into %s"), *Struct->GetName()));
            return false;
        }

        bool bAnyApplied = false;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Object->Values)
        {
            const FString EntryPath = ChildPath_JsonApply(PathPrefix, Entry.Key);

            FProperty* Child = FindPropertyRelaxed_JsonApply(Struct, Entry.Key);
            if (!Child)
            {
                // THIS is the no-op the engine converter swallows silently.
                Report.Ignored.Add(EntryPath);
                continue;
            }

            void* ChildPtr = Child->ContainerPtrToValuePtr<void>(StructPtr);
            if (ApplyJsonToProperty(Child, ChildPtr, Entry.Value, EntryPath, Report))
            {
                bAnyApplied = true;
            }
        }
        return bAnyApplied;
    }

    bool ApplyGameplayTag_JsonApply(void* StructPtr, const TSharedPtr<FJsonValue>& Value,
                                    const FString& PathPrefix, FJsonApplyReport& Report)
    {
        FString TagName;
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            TagName = Value->AsString();
        }
        else if (Value.IsValid() && Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            if (Object.IsValid())
            {
                Object->TryGetStringField(TEXT("TagName"), TagName);
            }
        }
        else if (Value.IsValid() && Value->Type == EJson::Null)
        {
            return WriteStructValue_JsonApply(StructPtr, FGameplayTag(), PathPrefix, Report);
        }
        else
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("an FGameplayTag is written as a string \"A.B.C\" or {\"TagName\":\"A.B.C\"}, got %s"),
                                JsonTypeName_JsonApply(Value)));
            return false;
        }

        TagName.TrimStartAndEndInline();
        if (TagName.IsEmpty())
        {
            // Clearing the tag is a legitimate write.
            return WriteStructValue_JsonApply(StructPtr, FGameplayTag(), PathPrefix, Report);
        }

        // Without validation against the registry, a misspelled tag stays dead-set and nobody
        // notices until the game does not react. Here it is a named failure.
        const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/ false);
        if (!Tag.IsValid())
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("tag '%s' is not in the registry; register it (create_gameplay_tag / a tag table) or fix the spelling"),
                                *TagName));
            return false;
        }

        return WriteStructValue_JsonApply(StructPtr, Tag, PathPrefix, Report);
    }

    bool ApplyGameplayTagContainer_JsonApply(void* StructPtr, const TSharedPtr<FJsonValue>& Value,
                                             const FString& PathPrefix, FJsonApplyReport& Report)
    {
        const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();

        FGameplayTagContainer Container;
        bool bAnyInvalid = false;
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *PathPrefix, Index);
            const TSharedPtr<FJsonValue>& Item = Items[Index];
            if (!Item.IsValid() || Item->Type != EJson::String)
            {
                ReportError_JsonApply(Report, ItemPath,
                    FString::Printf(TEXT("expected a tag name, got %s"), JsonTypeName_JsonApply(Item)));
                bAnyInvalid = true;
                continue;
            }
            FString TagName = Item->AsString();
            TagName.TrimStartAndEndInline();
            if (TagName.IsEmpty())
            {
                continue;
            }
            const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/ false);
            if (!Tag.IsValid())
            {
                ReportError_JsonApply(Report, ItemPath,
                    FString::Printf(TEXT("tag '%s' is not in the registry"), *TagName));
                bAnyInvalid = true;
                continue;
            }
            Container.AddTag(Tag);
        }

        if (bAnyInvalid)
        {
            // Writing a half container would be worse than not writing: the caller would believe
            // the full list was set.
            return false;
        }

        return WriteStructValue_JsonApply(StructPtr, MoveTemp(Container), PathPrefix, Report);
    }

    bool ApplySlateBrushFromPath_JsonApply(void* StructPtr, const FString& RawPath,
                                           const FString& PathPrefix, FJsonApplyReport& Report)
    {
        FSlateBrush* Brush = static_cast<FSlateBrush*>(StructPtr);

        FString TrimmedPath = RawPath.TrimStartAndEnd();
        if (TrimmedPath.IsEmpty())
        {
            Brush->SetResourceObject(nullptr);
            Report.Applied.Add(ChildPath_JsonApply(PathPrefix, TEXT("ResourceObject")));
            return true;
        }

        FString LoadReason;
        UObject* Loaded = LoadObjectFromPath_JsonApply(TrimmedPath, LoadReason);
        if (!Loaded)
        {
            ReportError_JsonApply(Report, PathPrefix, LoadReason);
            return false;
        }

        UTexture2D* AsTexture2D = Cast<UTexture2D>(Loaded);
        if (!AsTexture2D && !Loaded->IsA<UTexture>() && !Loaded->IsA<UMaterialInterface>())
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("'%s' is a %s; an FSlateBrush accepts a texture or a material"),
                                *TrimmedPath, *Loaded->GetClass()->GetName()));
            return false;
        }

        Brush->SetResourceObject(Loaded);
        Report.Applied.Add(ChildPath_JsonApply(PathPrefix, TEXT("ResourceObject")));

        if (AsTexture2D)
        {
            // A brush with a zero ImageSize draws nothing: for a texture, the imported size is
            // the only default that does not force a second call.
            const FIntPoint Size = AsTexture2D->GetImportedSize();
            // FSlateBrush::ImageSize is an FDeprecateSlateVector2D (derives from FVector2f) and its
            // operator= from FVector2f is the only non-deprecated one (SlateVector2.h).
            Brush->ImageSize = FVector2f(static_cast<float>(Size.X), static_cast<float>(Size.Y));
            Brush->DrawAs = ESlateBrushDrawType::Image;
            Report.Applied.Add(ChildPath_JsonApply(PathPrefix, TEXT("ImageSize")));
            Report.Applied.Add(ChildPath_JsonApply(PathPrefix, TEXT("DrawAs")));
        }
        return true;
    }

    bool ApplySlateFontFromPath_JsonApply(void* StructPtr, const FString& RawPath,
                                          const FString& PathPrefix, FJsonApplyReport& Report)
    {
        FSlateFontInfo* FontInfo = static_cast<FSlateFontInfo*>(StructPtr);

        FString TrimmedPath = RawPath.TrimStartAndEnd();
        if (TrimmedPath.IsEmpty())
        {
            FontInfo->FontObject = nullptr;
            Report.Applied.Add(ChildPath_JsonApply(PathPrefix, TEXT("FontObject")));
            return true;
        }

        FString LoadReason;
        UObject* Loaded = LoadObjectFromPath_JsonApply(TrimmedPath, LoadReason);
        if (!Loaded)
        {
            ReportError_JsonApply(Report, PathPrefix, LoadReason);
            return false;
        }
        if (!Loaded->IsA<UFont>() && !Loaded->IsA<UFontFace>())
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("'%s' is a %s; an FSlateFontInfo accepts a UFont or a UFontFace"),
                                *TrimmedPath, *Loaded->GetClass()->GetName()));
            return false;
        }

        FontInfo->FontObject = Loaded;
        Report.Applied.Add(ChildPath_JsonApply(PathPrefix, TEXT("FontObject")));
        return true;
    }

    bool ApplySoftPathString_JsonApply(UScriptStruct* Struct, void* StructPtr, const TSharedPtr<FJsonValue>& Value,
                                       const FString& PathPrefix, FJsonApplyReport& Report)
    {
        if (Value->Type == EJson::Null)
        {
            static_cast<FSoftObjectPath*>(StructPtr)->Reset();
            Report.Applied.Add(PathPrefix);
            return true;
        }
        if (Value->Type != EJson::String)
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("a %s is written as the asset path in a string, got %s"),
                                *Struct->GetName(), JsonTypeName_JsonApply(Value)));
            return false;
        }

        FString Path = Value->AsString().TrimStartAndEnd();
        if (Path.IsEmpty())
        {
            static_cast<FSoftObjectPath*>(StructPtr)->Reset();
            Report.Applied.Add(PathPrefix);
            return true;
        }

        // A soft path does not force loading anything, but it does have to be a path: SetPath
        // leaves the object empty if it is not, and that would again be a phantom write.
        FSoftObjectPath NewPath(Path);
        if (NewPath.IsNull())
        {
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("'%s' is not a valid asset path"), *Path));
            return false;
        }

        return WriteStructValue_JsonApply(StructPtr, MoveTemp(NewPath), PathPrefix, Report);
    }

    bool ApplyJsonToStructValue_JsonApply(UScriptStruct* Struct, void* StructPtr, FProperty* OwningProperty,
                                          const TSharedPtr<FJsonValue>& Value, const FString& PathPrefix,
                                          FJsonApplyReport& Report)
    {
        if (!Struct || !StructPtr)
        {
            ReportError_JsonApply(Report, PathPrefix, TEXT("null struct or value pointer"));
            return false;
        }
        // The guard goes up here and not in the generic path: the shortcuts below dereference
        // Value directly.
        if (!Value.IsValid())
        {
            ReportError_JsonApply(Report, PathPrefix, TEXT("missing JSON value"));
            return false;
        }

        // --- Shortcuts per struct type. They apply AT ANY DEPTH because the dispatch is
        //     recursive: the shortcut lives here, not in a top-level pre-check. ---

        // FSlateColor: the header's case. Its reflected fields are SpecifiedColor/ColorUseRule, so
        // a flat {R,G,B,A} matches nothing and the engine converter drops it silently.
        if (Struct == FSlateColor::StaticStruct())
        {
            FLinearColor Parsed;
            if (TryParseLinearColor_JsonApply(Value, Parsed))
            {
                return WriteStructValue_JsonApply(StructPtr, FSlateColor(Parsed), PathPrefix, Report);
            }
            // The full form {"SpecifiedColor":...,"ColorUseRule":...} goes through the generic path.
        }
        else if (Struct == TBaseStructure<FLinearColor>::Get())
        {
            FLinearColor Parsed;
            if (TryParseLinearColor_JsonApply(Value, Parsed))
            {
                return WriteStructValue_JsonApply(StructPtr, Parsed, PathPrefix, Report);
            }
        }
        else if (Struct == TBaseStructure<FColor>::Get())
        {
            FColor Parsed;
            if (TryParseColor_JsonApply(Value, Parsed))
            {
                return WriteStructValue_JsonApply(StructPtr, Parsed, PathPrefix, Report);
            }
        }
        else if (Struct == FMargin::StaticStruct())
        {
            FMargin Parsed;
            if (TryParseMargin_JsonApply(Value, Parsed))
            {
                return WriteStructValue_JsonApply(StructPtr, Parsed, PathPrefix, Report);
            }
        }
        else if (Struct == FGameplayTag::StaticStruct())
        {
            return ApplyGameplayTag_JsonApply(StructPtr, Value, PathPrefix, Report);
        }
        else if (Struct == FGameplayTagContainer::StaticStruct())
        {
            if (Value.IsValid() && Value->Type == EJson::Array)
            {
                return ApplyGameplayTagContainer_JsonApply(StructPtr, Value, PathPrefix, Report);
            }
            if (Value.IsValid() && Value->Type == EJson::String)
            {
                // A single bare tag is the natural shorthand for a one-item list.
                TArray<TSharedPtr<FJsonValue>> Wrapped;
                Wrapped.Add(Value);
                const TSharedPtr<FJsonValue> AsArray = MakeShared<FJsonValueArray>(Wrapped);
                return ApplyGameplayTagContainer_JsonApply(StructPtr, AsArray, PathPrefix, Report);
            }
        }
        else if (Struct == FSoftObjectPath::StaticStruct() || Struct == FSoftClassPath::StaticStruct())
        {
            return ApplySoftPathString_JsonApply(Struct, StructPtr, Value, PathPrefix, Report);
        }
        else if (Struct == FSlateBrush::StaticStruct())
        {
            if (Value.IsValid() && Value->Type == EJson::String)
            {
                return ApplySlateBrushFromPath_JsonApply(StructPtr, Value->AsString(), PathPrefix, Report);
            }
            // Object -> generic recursion further down.
        }
        else if (Struct == FSlateFontInfo::StaticStruct())
        {
            if (Value.IsValid() && Value->Type == EJson::String)
            {
                return ApplySlateFontFromPath_JsonApply(StructPtr, Value->AsString(), PathPrefix, Report);
            }
        }
        else if (Struct == TBaseStructure<FVector2D>::Get())
        {
            TArray<double> Numbers;
            if (TryReadNumberArray_JsonApply(Value, Numbers) && Numbers.Num() >= 2)
            {
                return WriteStructValue_JsonApply(StructPtr, FVector2D(Numbers[0], Numbers[1]), PathPrefix, Report);
            }
        }
        else if (Struct == TBaseStructure<FVector>::Get())
        {
            TArray<double> Numbers;
            if (TryReadNumberArray_JsonApply(Value, Numbers) && Numbers.Num() >= 3)
            {
                return WriteStructValue_JsonApply(StructPtr, FVector(Numbers[0], Numbers[1], Numbers[2]), PathPrefix, Report);
            }
        }
        else if (Struct == TBaseStructure<FVector4>::Get())
        {
            TArray<double> Numbers;
            if (TryReadNumberArray_JsonApply(Value, Numbers) && Numbers.Num() >= 3)
            {
                return WriteStructValue_JsonApply(StructPtr,
                    FVector4(Numbers[0], Numbers[1], Numbers[2], Numbers.Num() >= 4 ? Numbers[3] : 1.0), PathPrefix, Report);
            }
        }
        else if (Struct == TBaseStructure<FRotator>::Get())
        {
            TArray<double> Numbers;
            if (TryReadNumberArray_JsonApply(Value, Numbers) && Numbers.Num() >= 3)
            {
                // Order of FRotator's reflected fields: Pitch, Yaw, Roll
                // (CoreUObject/Public/UObject/NoExportTypes.h). The array follows that order.
                return WriteStructValue_JsonApply(StructPtr, FRotator(Numbers[0], Numbers[1], Numbers[2]), PathPrefix, Report);
            }
        }
        else if (Struct == TBaseStructure<FIntPoint>::Get())
        {
            TArray<double> Numbers;
            if (TryReadNumberArray_JsonApply(Value, Numbers) && Numbers.Num() >= 2)
            {
                return WriteStructValue_JsonApply(StructPtr,
                    FIntPoint(static_cast<int32>(Numbers[0]), static_cast<int32>(Numbers[1])), PathPrefix, Report);
            }
        }

        // --- Generic path ---

        if (Value->Type == EJson::Object)
        {
            return ApplyJsonObjectToStruct_JsonApply(Struct, StructPtr, Value->AsObject(), PathPrefix, Report);
        }

        if (Value->Type == EJson::Array)
        {
            TArray<double> Numbers;
            if (TryReadNumberArray_JsonApply(Value, Numbers)
                && ApplyNumberArrayToNumericStruct_JsonApply(Struct, StructPtr, Numbers, PathPrefix, Report))
            {
                return true;
            }
            ReportError_JsonApply(Report, PathPrefix,
                FString::Printf(TEXT("an array does not describe a %s; use an object with its fields (%s)"),
                                *Struct->GetName(), *DescribeAvailableProperties_JsonApply(Struct)));
            return false;
        }

        if (Value->Type == EJson::String && OwningProperty)
        {
            // Last resort: UE's text format, "(X=1,Y=2)".
            FString Reason;
            if (ImportTextInto_JsonApply(OwningProperty, StructPtr, Value->AsString(), Reason))
            {
                Report.Applied.Add(PathPrefix);
                return true;
            }
            ReportError_JsonApply(Report, PathPrefix, Reason);
            return false;
        }

        ReportError_JsonApply(Report, PathPrefix,
            FString::Printf(TEXT("cannot write a %s from %s; use an object with its fields (%s)"),
                            *Struct->GetName(), JsonTypeName_JsonApply(Value),
                            *DescribeAvailableProperties_JsonApply(Struct)));
        return false;
    }

    // ----------------------------------------------------------------------------------------
    // Temporary value of an FProperty (for maps and sets)
    // ----------------------------------------------------------------------------------------

    // Buffer with the size and alignment the property asks for, constructed and destroyed by the
    // property itself. It exists to be able to WRITE FIRST AND INSERT AFTERWARDS into maps and
    // sets: once an element is added with AddDefaultValue_Invalid_NeedsRehash the container is
    // left unindexed, and removing it again requires a hash that does not exist yet.
    class FScratchValue_JsonApply
    {
    public:
        explicit FScratchValue_JsonApply(FProperty* InProperty)
            : Property(InProperty)
        {
            if (!Property)
            {
                return;
            }
            Buffer = FMemory::Malloc(static_cast<SIZE_T>(Property->GetElementSize()),
                                     static_cast<uint32>(Property->GetMinAlignment()));
            if (Buffer)
            {
                Property->InitializeValue(Buffer);
            }
        }

        ~FScratchValue_JsonApply()
        {
            if (Buffer)
            {
                if (Property)
                {
                    Property->DestroyValue(Buffer);
                }
                FMemory::Free(Buffer);
            }
        }

        FScratchValue_JsonApply(const FScratchValue_JsonApply&) = delete;
        FScratchValue_JsonApply& operator=(const FScratchValue_JsonApply&) = delete;

        bool IsValid() const { return Buffer != nullptr; }
        void* Get() const { return Buffer; }

        // Back to the default value between iterations: without this, whatever the current
        // iteration does not write would carry over from the one before and mixed pairs
        // would be inserted.
        void Clear() const
        {
            if (Buffer && Property)
            {
                Property->ClearValue(Buffer);
            }
        }

    private:
        FProperty* Property = nullptr;
        void* Buffer = nullptr;
    };

    // ----------------------------------------------------------------------------------------
    // Map keys (they always arrive as strings in JSON)
    // ----------------------------------------------------------------------------------------

    bool IsSupportedMapKeyType_JsonApply(const FProperty* KeyProperty)
    {
        return KeyProperty
            && (CastField<FStrProperty>(KeyProperty)
             || CastField<FNameProperty>(KeyProperty)
             || CastField<FEnumProperty>(KeyProperty)
             || CastField<FNumericProperty>(KeyProperty)   // includes FByteProperty with or without an enum
             || CastField<FStructProperty>(KeyProperty)    // FGameplayTag, FSoftObjectPath... via ImportText
             || CastField<FObjectPropertyBase>(KeyProperty));
    }

    bool WriteMapKey_JsonApply(FProperty* KeyProperty, void* KeyPtr, const FString& Key, FString& OutReason)
    {
        if (FStrProperty* StrKey = CastField<FStrProperty>(KeyProperty))
        {
            StrKey->SetPropertyValue(KeyPtr, Key);
            return true;
        }
        if (FNameProperty* NameKey = CastField<FNameProperty>(KeyProperty))
        {
            NameKey->SetPropertyValue(KeyPtr, FName(*Key));
            return true;
        }
        if (FEnumProperty* EnumKey = CastField<FEnumProperty>(KeyProperty))
        {
            int64 Resolved = 0;
            const TSharedPtr<FJsonValue> AsString = MakeShared<FJsonValueString>(Key);
            if (!ResolveEnumValue_JsonApply(EnumKey->GetEnum(), AsString, Resolved, OutReason))
            {
                return false;
            }
            EnumKey->GetUnderlyingProperty()->SetIntPropertyValue(KeyPtr, Resolved);
            return true;
        }
        if (FByteProperty* ByteKey = CastField<FByteProperty>(KeyProperty))
        {
            if (ByteKey->Enum)
            {
                int64 Resolved = 0;
                const TSharedPtr<FJsonValue> AsString = MakeShared<FJsonValueString>(Key);
                if (!ResolveEnumValue_JsonApply(ByteKey->Enum, AsString, Resolved, OutReason))
                {
                    return false;
                }
                ByteKey->SetIntPropertyValue(KeyPtr, Resolved);
                return true;
            }
        }
        if (FNumericProperty* NumericKey = CastField<FNumericProperty>(KeyProperty))
        {
            double Number = 0.0;
            if (!LexTryParseString(Number, *Key))
            {
                OutReason = FString::Printf(TEXT("key '%s' is not a number"), *Key);
                return false;
            }
            if (NumericKey->IsFloatingPoint())
            {
                NumericKey->SetFloatingPointPropertyValue(KeyPtr, Number);
            }
            else
            {
                NumericKey->SetIntPropertyValue(KeyPtr, static_cast<int64>(Number));
            }
            return true;
        }

        // Structs and object references: their own ImportText knows how to read the text form.
        return ImportTextInto_JsonApply(KeyProperty, KeyPtr, Key, OutReason);
    }

} // anonymous namespace

// ============================================================================================
// ResolvePropertyPath
// ============================================================================================

bool ResolvePropertyPath(UStruct* OwnerStruct, void* Container, const FString& Path,
                         FProperty*& OutLeafProp, void*& OutLeafPtr, FString& OutError)
{
    OutLeafProp = nullptr;
    OutLeafPtr = nullptr;
    OutError.Reset();

    if (!OwnerStruct || !Container)
    {
        OutError = TEXT("null container or owner UStruct");
        return false;
    }
    if (Path.IsEmpty())
    {
        OutError = TEXT("empty property path");
        return false;
    }

    TArray<FString> Segments;
    Path.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/ true);
    if (Segments.Num() == 0)
    {
        OutError = FString::Printf(TEXT("path '%s' contains no segment"), *Path);
        return false;
    }

    UStruct* CurrentStruct = OwnerStruct;
    void* CurrentContainer = Container;
    FProperty* CurrentProperty = nullptr;
    void* CurrentValuePtr = nullptr;

    for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
    {
        const FString RawSegment = Segments[SegmentIndex];

        // Split "Name[3][1]" into name + indices.
        FString Name = RawSegment;
        TArray<int32> Indices;
        {
            int32 BracketPos = INDEX_NONE;
            if (RawSegment.FindChar(TEXT('['), BracketPos))
            {
                Name = RawSegment.Left(BracketPos);
                FString Rest = RawSegment.RightChop(BracketPos);
                while (!Rest.IsEmpty())
                {
                    int32 Open = INDEX_NONE;
                    int32 Close = INDEX_NONE;
                    if (!Rest.FindChar(TEXT('['), Open) || !Rest.FindChar(TEXT(']'), Close) || Close < Open)
                    {
                        OutError = FString::Printf(TEXT("malformed index in segment '%s' of path '%s'"),
                                                   *RawSegment, *Path);
                        return false;
                    }
                    const FString IndexText = Rest.Mid(Open + 1, Close - Open - 1).TrimStartAndEnd();
                    int32 ParsedIndex = 0;
                    if (IndexText.IsEmpty() || !LexTryParseString(ParsedIndex, *IndexText))
                    {
                        OutError = FString::Printf(TEXT("'%s' is not a numeric index in segment '%s'"),
                                                   *IndexText, *RawSegment);
                        return false;
                    }
                    Indices.Add(ParsedIndex);
                    Rest = Rest.RightChop(Close + 1);
                }
            }
        }

        if (Name.IsEmpty())
        {
            OutError = FString::Printf(TEXT("unnamed segment in path '%s'"), *Path);
            return false;
        }
        if (!CurrentStruct)
        {
            OutError = FString::Printf(TEXT("cannot descend into '%s': the previous segment of '%s' is neither a struct nor an object"),
                                       *Name, *Path);
            return false;
        }

        FProperty* Property = FindPropertyRelaxed_JsonApply(CurrentStruct, Name);
        if (!Property)
        {
            const FString Available = DescribeAvailableProperties_JsonApply(CurrentStruct);
            OutError = Available.IsEmpty()
                ? FString::Printf(TEXT("segment '%s' of path '%s' does not exist in %s"),
                                  *Name, *Path, *CurrentStruct->GetName())
                : FString::Printf(TEXT("segment '%s' of path '%s' does not exist in %s. Available: %s"),
                                  *Name, *Path, *CurrentStruct->GetName(), *Available);
            return false;
        }

        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);

        for (int32 Index : Indices)
        {
            FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
            if (!ArrayProperty)
            {
                OutError = FString::Printf(TEXT("'%s' (%s) is not an array, so [%d] means nothing in path '%s'"),
                                           *Name, *Property->GetCPPType(), Index, *Path);
                return false;
            }
            FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
            if (!ArrayHelper.IsValidIndex(Index))
            {
                OutError = FString::Printf(TEXT("index %d out of range in '%s' of path '%s' (%d elements)"),
                                           Index, *Name, *Path, ArrayHelper.Num());
                return false;
            }
            ValuePtr = ArrayHelper.GetElementPtr(Index);
            Property = ArrayProperty->Inner;
        }

        CurrentProperty = Property;
        CurrentValuePtr = ValuePtr;

        const bool bIsLastSegment = (SegmentIndex + 1 == Segments.Num());
        if (bIsLastSegment)
        {
            break;
        }

        if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            CurrentStruct = StructProperty->Struct;
            CurrentContainer = ValuePtr;
        }
        else if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            // Descending through a reference means FOLLOWING THE POINTER, not writing over it.
            UObject* Inner = ObjectProperty->GetObjectPropertyValue(ValuePtr);
            if (!Inner)
            {
                OutError = FString::Printf(TEXT("segment '%s' of path '%s' is null: there is no object to keep descending through"),
                                           *Name, *Path);
                return false;
            }
            CurrentStruct = Inner->GetClass();
            CurrentContainer = Inner;
        }
        else
        {
            OutError = FString::Printf(TEXT("segment '%s' of path '%s' is of type %s: cannot descend through it"),
                                       *Name, *Path, *Property->GetCPPType());
            return false;
        }
    }

    OutLeafProp = CurrentProperty;
    OutLeafPtr = CurrentValuePtr;
    return OutLeafProp != nullptr && OutLeafPtr != nullptr;
}

// ============================================================================================
// ApplyJsonToProperty
// ============================================================================================

bool ApplyJsonToProperty(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value,
                         const FString& PathPrefix, FJsonApplyReport& Report)
{
    if (!Property)
    {
        ReportError_JsonApply(Report, PathPrefix.IsEmpty() ? FString(TEXT("<no path>")) : PathPrefix,
                              TEXT("null property"));
        return false;
    }
    const FString Path = ReportPath_JsonApply(PathPrefix, Property);

    if (!ValuePtr)
    {
        ReportError_JsonApply(Report, Path, TEXT("null value pointer"));
        return false;
    }
    if (!Value.IsValid())
    {
        ReportError_JsonApply(Report, Path, TEXT("missing JSON value"));
        return false;
    }

    // --- Structs (with their shortcuts) ---
    if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
    {
        return ApplyJsonToStructValue_JsonApply(StructProperty->Struct, ValuePtr, Property, Value, Path, Report);
    }

    // --- Enums. They go BEFORE FNumericProperty because FByteProperty derives from it and a
    //     TEnumAsByte<> must be resolved by name, not as the number 0. ---
    if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
    {
        int64 Resolved = 0;
        FString Reason;
        if (!ResolveEnumValue_JsonApply(EnumProperty->GetEnum(), Value, Resolved, Reason))
        {
            ReportError_JsonApply(Report, Path, Reason);
            return false;
        }
        EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Resolved);
        Report.Applied.Add(Path);
        return true;
    }
    if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
    {
        if (ByteProperty->Enum)
        {
            int64 Resolved = 0;
            FString Reason;
            if (!ResolveEnumValue_JsonApply(ByteProperty->Enum, Value, Resolved, Reason))
            {
                ReportError_JsonApply(Report, Path, Reason);
                return false;
            }
            ByteProperty->SetIntPropertyValue(ValuePtr, Resolved);
            Report.Applied.Add(Path);
            return true;
        }
        // A uint8 without an enum is a number: it falls into the numeric branch below.
    }

    // --- Object references. Order matters: the soft and class variants derive from the
    //     hard ones, so the most specific is checked first. ---
    if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
    {
        UObject* Target = nullptr;
        if (!ResolveReferenceValue_JsonApply(SoftObjectProperty, CastField<FSoftClassProperty>(Property), Value,
                                             TEXT("a soft reference"), Path, Report, Target))
        {
            return false;
        }
        // FSoftObjectProperty stores an FSoftObjectPtr; a null target clears it.
        *static_cast<FSoftObjectPtr*>(ValuePtr) = Target ? FSoftObjectPtr(Target) : FSoftObjectPtr();
        Report.Applied.Add(Path);
        return true;
    }

    if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
    {
        UObject* Target = nullptr;
        if (!ResolveReferenceValue_JsonApply(ObjectProperty, CastField<FClassProperty>(Property), Value,
                                             TEXT("an object reference"), Path, Report, Target))
        {
            return false;
        }
        ObjectProperty->SetObjectPropertyValue(ValuePtr, Target);
        Report.Applied.Add(Path);
        return true;
    }

    // --- Arrays ---
    if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
    {
        if (Value->Type != EJson::Array)
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("expected a JSON array for %s, got %s"),
                                *Property->GetCPPType(), JsonTypeName_JsonApply(Value)));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
        FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
        ArrayHelper.Resize(Items.Num());

        if (Items.Num() == 0)
        {
            // Emptying the array IS a write.
            Report.Applied.Add(Path);
            return true;
        }

        bool bAnyApplied = false;
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
            if (ApplyJsonToProperty(ArrayProperty->Inner, ArrayHelper.GetElementPtr(Index),
                                    Items[Index], ElementPath, Report))
            {
                bAnyApplied = true;
            }
        }
        return bAnyApplied;
    }

    // --- Maps ---
    if (FMapProperty* MapProperty = CastField<FMapProperty>(Property))
    {
        if (Value->Type != EJson::Object)
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("expected a JSON object for %s, got %s"),
                                *Property->GetCPPType(), JsonTypeName_JsonApply(Value)));
            return false;
        }
        // The key-type check goes BEFORE emptying anything: a key type that cannot be
        // written from JSON must not cost the caller the content it already had.
        if (!IsSupportedMapKeyType_JsonApply(MapProperty->KeyProp))
        {
            const FString KeyTypeName = MapProperty->KeyProp
                ? MapProperty->KeyProp->GetCPPType()
                : FString(TEXT("<unknown>"));
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("this map's keys are of type %s, which cannot be written from a JSON key (always a string)"),
                                *KeyTypeName));
            return false;
        }

        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid())
        {
            ReportError_JsonApply(Report, Path, TEXT("invalid JSON object"));
            return false;
        }

        FScriptMapHelper MapHelper(MapProperty, ValuePtr);
        MapHelper.EmptyValues();

        if (Object->Values.Num() == 0)
        {
            Report.Applied.Add(Path);
            return true;
        }

        // Each pair is built SEPARATELY and only inserted if key and value were really written.
        // Adding and then RemoveAt does not work: AddDefaultValue_Invalid_NeedsRehash leaves the
        // map unindexed on purpose ("the set needs rehashing", UnrealType.h), and RemoveAt needs
        // the element's hash to unlink it -- undoing halfway is exactly what cannot be done.
        FScratchValue_JsonApply KeyScratch(MapProperty->KeyProp);
        FScratchValue_JsonApply ValueScratch(MapProperty->ValueProp);
        if (!KeyScratch.IsValid() || !ValueScratch.IsValid())
        {
            ReportError_JsonApply(Report, Path, TEXT("could not allocate the temporary pair to write the map"));
            MapHelper.Rehash();
            return false;
        }

        bool bAnyApplied = false;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Object->Values)
        {
            const FString EntryPath = FString::Printf(TEXT("%s[\"%s\"]"), *Path, *Entry.Key);

            KeyScratch.Clear();
            ValueScratch.Clear();

            FString Reason;
            if (!WriteMapKey_JsonApply(MapProperty->KeyProp, KeyScratch.Get(), Entry.Key, Reason))
            {
                ReportError_JsonApply(Report, EntryPath, Reason);
                continue;
            }
            if (!ApplyJsonToProperty(MapProperty->ValueProp, ValueScratch.Get(), Entry.Value, EntryPath, Report))
            {
                continue;
            }

            const int32 PairIndex = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
            MapProperty->KeyProp->CopyCompleteValue(MapHelper.GetKeyPtr(PairIndex), KeyScratch.Get());
            MapProperty->ValueProp->CopyCompleteValue(MapHelper.GetValuePtr(PairIndex), ValueScratch.Get());
            bAnyApplied = true;
        }

        // Mandatory after AddDefaultValue_Invalid_NeedsRehash: until here the map has no index.
        MapHelper.Rehash();
        return bAnyApplied;
    }

    // --- Sets ---
    if (FSetProperty* SetProperty = CastField<FSetProperty>(Property))
    {
        if (Value->Type != EJson::Array)
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("expected a JSON array for %s, got %s"),
                                *Property->GetCPPType(), JsonTypeName_JsonApply(Value)));
            return false;
        }

        const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
        FScriptSetHelper SetHelper(SetProperty, ValuePtr);
        SetHelper.EmptyElements();

        if (Items.Num() == 0)
        {
            Report.Applied.Add(Path);
            return true;
        }

        // Same reason as for the map: write into a separate element and only insert it if it worked.
        FScratchValue_JsonApply ElementScratch(SetProperty->ElementProp);
        if (!ElementScratch.IsValid())
        {
            ReportError_JsonApply(Report, Path, TEXT("could not allocate the temporary element to write the set"));
            SetHelper.Rehash();
            return false;
        }

        bool bAnyApplied = false;
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            const FString ElementPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);

            ElementScratch.Clear();
            if (!ApplyJsonToProperty(SetProperty->ElementProp, ElementScratch.Get(), Items[Index], ElementPath, Report))
            {
                continue;
            }

            const int32 ElementIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
            SetProperty->ElementProp->CopyCompleteValue(SetHelper.GetElementPtr(ElementIndex), ElementScratch.Get());
            bAnyApplied = true;
        }

        // Same as for the map: without Rehash the set stays unindexed and lookups fail.
        SetHelper.Rehash();
        return bAnyApplied;
    }

    // --- FText ---
    if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
    {
        if (Value->Type == EJson::String)
        {
            TextProperty->SetPropertyValue(ValuePtr, FText::FromString(Value->AsString()));
            Report.Applied.Add(Path);
            return true;
        }
        if (Value->Type == EJson::Null)
        {
            TextProperty->SetPropertyValue(ValuePtr, FText::GetEmpty());
            Report.Applied.Add(Path);
            return true;
        }
        if (Value->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Object = Value->AsObject();
            if (!Object.IsValid())
            {
                ReportError_JsonApply(Report, Path, TEXT("invalid JSON object"));
                return false;
            }

            FString TableId;
            FString Key;
            Object->TryGetStringField(TEXT("StringTable"), TableId);
            Object->TryGetStringField(TEXT("Key"), Key);

            if (!TableId.IsEmpty())
            {
                if (Key.IsEmpty())
                {
                    ReportError_JsonApply(Report, Path, TEXT("a string-table FText needs \"Key\" as well as \"StringTable\""));
                    return false;
                }
                // FromStringTable(FName, FTextKey, EStringTableLoadingPolicy) -- Text.h.
                TextProperty->SetPropertyValue(ValuePtr, FText::FromStringTable(FName(*TableId), Key));
                Report.Applied.Add(Path);
                return true;
            }

            FString Namespace;
            FString Source;
            Object->TryGetStringField(TEXT("Namespace"), Namespace);
            if (!Object->TryGetStringField(TEXT("Source"), Source))
            {
                ReportError_JsonApply(Report, Path,
                    TEXT("an FText given as an object needs {\"Namespace\",\"Key\",\"Source\"} or {\"StringTable\",\"Key\"}"));
                return false;
            }
            if (Key.IsEmpty())
            {
                ReportError_JsonApply(Report, Path, TEXT("a localizable FText needs \"Key\""));
                return false;
            }
            // AsLocalizable_Advanced is the one that exists with and without WITH_EDITORONLY_DATA
            // (Text.h/568); ChangeKey only exists in the editor, so it is no use for a
            // runtime-module function.
            TextProperty->SetPropertyValue(ValuePtr, FText::AsLocalizable_Advanced(Namespace, Key, FStringView(Source)));
            Report.Applied.Add(Path);
            return true;
        }

        FString AsText;
        if (TryReadScalarAsText_JsonApply(Value, AsText))
        {
            TextProperty->SetPropertyValue(ValuePtr, FText::FromString(AsText));
            Report.Applied.Add(Path);
            return true;
        }
        ReportError_JsonApply(Report, Path,
            FString::Printf(TEXT("cannot build an FText from %s"), JsonTypeName_JsonApply(Value)));
        return false;
    }

    // --- FName / FString ---
    if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
    {
        FString AsText;
        if (!TryReadScalarAsText_JsonApply(Value, AsText))
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("an FName is written with a scalar value, got %s"), JsonTypeName_JsonApply(Value)));
            return false;
        }
        // A default FName() IS NAME_None; written this way so the ternary does not mix EName and FName.
        NameProperty->SetPropertyValue(ValuePtr, AsText.IsEmpty() ? FName() : FName(*AsText));
        Report.Applied.Add(Path);
        return true;
    }
    if (FStrProperty* StringProperty = CastField<FStrProperty>(Property))
    {
        FString AsText;
        if (!TryReadScalarAsText_JsonApply(Value, AsText))
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("an FString is written with a scalar value, got %s"), JsonTypeName_JsonApply(Value)));
            return false;
        }
        StringProperty->SetPropertyValue(ValuePtr, AsText);
        Report.Applied.Add(Path);
        return true;
    }

    // --- Booleans ---
    if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
    {
        bool bParsed = false;
        if (!TryReadBool_JsonApply(Value, bParsed))
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("expected true/false (or 1/0, or \"true\"/\"false\"), got %s"),
                                JsonTypeName_JsonApply(Value)));
            return false;
        }
        BoolProperty->SetPropertyValue(ValuePtr, bParsed);
        Report.Applied.Add(Path);
        return true;
    }

    // --- Numerics ---
    if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
    {
        double Number = 0.0;
        if (!TryReadNumber_JsonApply(Value, Number))
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("expected a number for %s, got %s"),
                                *Property->GetCPPType(), JsonTypeName_JsonApply(Value)));
            return false;
        }
        if (NumericProperty->IsFloatingPoint())
        {
            NumericProperty->SetFloatingPointPropertyValue(ValuePtr, Number);
        }
        else
        {
            NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(Number));
        }
        Report.Applied.Add(Path);
        return true;
    }

    // --- Any other type: the property's own ImportText ---
    {
        FString AsText;
        if (!TryReadScalarAsText_JsonApply(Value, AsText))
        {
            ReportError_JsonApply(Report, Path,
                FString::Printf(TEXT("there is no way to write %s (%s) from %s"),
                                *Property->GetName(), *Property->GetCPPType(), JsonTypeName_JsonApply(Value)));
            return false;
        }
        FString Reason;
        if (!ImportTextInto_JsonApply(Property, ValuePtr, AsText, Reason))
        {
            ReportError_JsonApply(Report, Path, Reason);
            return false;
        }
        Report.Applied.Add(Path);
        return true;
    }
}

// ============================================================================================
// ApplyJsonPathToObject
// ============================================================================================

bool ApplyJsonPathToObject(UObject* Object, const FString& DottedPath, const TSharedPtr<FJsonValue>& Value,
                           FJsonApplyReport& Report, FString& OutError)
{
    OutError.Reset();

    if (!Object)
    {
        OutError = TEXT("null object");
        return false;
    }

    FProperty* LeafProperty = nullptr;
    void* LeafPtr = nullptr;
    if (!ResolvePropertyPath(Object->GetClass(), Object, DottedPath, LeafProperty, LeafPtr, OutError))
    {
        return false;
    }

    // Remember where the report was, so the caller can be told what failed IN THIS call and
    // is not burdened with what earlier calls left on the same report.
    const int32 IgnoredBefore = Report.Ignored.Num();
    const int32 ErrorsBefore = Report.Errors.Num();

    if (ApplyJsonToProperty(LeafProperty, LeafPtr, Value, DottedPath, Report))
    {
        return true;
    }

    TArray<FString> Reasons;
    for (int32 Index = ErrorsBefore; Index < Report.Errors.Num(); ++Index)
    {
        Reasons.Add(FString::Printf(TEXT("%s: %s"), *Report.Errors[Index].Key, *Report.Errors[Index].Value));
    }
    for (int32 Index = IgnoredBefore; Index < Report.Ignored.Num(); ++Index)
    {
        Reasons.Add(FString::Printf(TEXT("%s: no property with that name"), *Report.Ignored[Index]));
    }

    OutError = Reasons.Num() > 0
        ? FString::Printf(TEXT("nothing was written to '%s' -- %s"), *DottedPath, *FString::Join(Reasons, TEXT(" | ")))
        : FString::Printf(TEXT("nothing was written to '%s'"), *DottedPath);
    return false;
}

} // namespace mcp_common
