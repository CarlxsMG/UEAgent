"""
codegen_property_handlers.py -- C++ of the set_/get_<x>_properties command pairs, from JSON descriptors.

A descriptor (Tools/property_descriptors/<x>.json) declares one command pair. emit_region() turns the
descriptors of one handler .cpp into the code of its generated region: the helpers their modes share,
once, then one unit per descriptor -- its own helpers in an anonymous namespace, followed by the
annotated HandleSet<X>Properties and HandleGet<X>Properties. integrate_property_handlers.py writes the
regions into the .cpp files; the catalog and bridge codegens then read that C++ like any other handler.

Modes (the descriptor's "mode"):
  typed_setter      An actor's component.
                      actor_class, article      the actor type, and "a" or "an" before its name in errors
                      component_class, accessor  the component type, and how the actor returns it
                      accessor_comment           optional comment in the accessor
                      header_comment             optional comment above the unit
                      fields                     one entry per param: json, type (FIELD_TYPES), the
                                                 ENGINE_API setter, the readback field the reader reads,
                                                 desc, and optionally reader_comment and bitfield
                      loctext_key                optional; defaults to Set<X>Properties
                      refuse_static              optional; true when the component's setters change nothing
                                                 on a registered Static component, so the set handler
                                                 refuses one before any write
                    The set handler reads and checks every supplied param before the first write: a
                    value of the wrong kind is an error and nothing changes.
  reflection_merge  An asset's settings struct, merged from the settings param through reflection.
                      asset_class, path_param    the asset type, and the param that holds its path
                      struct_type, struct_field  the settings struct, and the asset member holding it
                      settings_desc              the description of the settings param
                    Every settings key must name a field and hold a value of its kind, and the merge runs
                    on a copy of the struct: a call that fails changes nothing.
Both modes take cmd_base (the <x>), set_desc and get_desc, and optionally set_retired / get_retired: "<listed tool> -
<how>", which retires that command from the tool list (its @retired line) while it keeps running.

Every helper name carries the PropertyHandlers_ prefix and a unit's helpers carry its <X>, so they never
collide with the hand-written code of the file; each region sits in its family's namespace, so every
region that uses a mode carries its own copy of the mode's shared helpers.

Usage (prints the region code for the given descriptors, without the region markers):
  python Tools/codegen_property_handlers.py Tools/property_descriptors/sky_light.json [more.json ...]
"""

from __future__ import annotations

import argparse
import json
import re
import sys

PREFIX = "PropertyHandlers_"




def retired_lines(d: dict, key: str) -> list[str]:
    """The `@retired` line of a command the descriptor retires (set_retired / get_retired), or none."""
    return [f"//   @retired {d[key]}"] if d.get(key) else []

def camel_case(snake: str) -> str:
    """sky_atmosphere -> SkyAtmosphere."""
    return "".join(word.capitalize() for word in snake.split("_"))


def spaced(camel: str) -> str:
    """SkyAtmosphere -> Sky Atmosphere."""
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", camel)


def comment(lines: list[str], indent: str = "") -> list[str]:
    return [f"{indent}// {line}" for line in lines]


def wrap_call(head: str, items: list[str], tail: str, width: int = 120) -> list[str]:
    """head + the comma-separated items + tail, as lines of at most width characters; a continuation line is
    indented four spaces past head's own indent."""
    indent = " " * (len(head) - len(head.lstrip()) + 4)
    lines, line = [], head
    for i, item in enumerate(items):
        piece = item + (", " if i < len(items) - 1 else tail)
        if len(line) + len(piece.rstrip()) > width and line.strip():
            lines.append(line.rstrip())
            line = indent
        line += piece
    lines.append(line.rstrip())
    return lines


# ---------------------------------------------------------------------------
# typed_setter
# ---------------------------------------------------------------------------
# Per field type: the @param hint, the TOptional the set handler reads the param into, and the reader helper
# that fills it (unset when the param is absent or null, an error reply when it holds anything else).
FIELD_TYPES = {
    "float": {"hint": "number", "optional": "float", "reader": "ReadFloat"},
    "int": {"hint": "number", "optional": "int32", "reader": "ReadInt"},
    "bool": {"hint": "bool", "optional": "bool", "reader": "ReadBool"},
    "linear_color": {"hint": "array", "optional": "FLinearColor", "reader": "ReadLinearColor"},
    "color_bytes": {"hint": "array", "optional": "FColor", "reader": "ReadColorBytes"},
}
COLOR_TYPES = {"linear_color", "color_bytes"}

# The names a set handler declares itself; a field's variable (its json name in CamelCase) must not take one.
RESERVED_NAMES = {"Params", "ActorName", "Error", "Component", "Malformed", "Refused", "AppliedCount", "Transaction",
                  "Result"}

# The helpers of a typed_setter region: the resolver, the reader of each field type its fields use, and the
# refusal of a static component when a descriptor asks for it.
RESOLVE_COMPONENT = """\
    // The component GetComponent returns for the TActor the required actor_name param names in the editor
    // world. Returns nullptr with OutError set when the param is missing, no actor has that name, the actor
    // is not a TActor (Article and Kind name it in the error) or it has no component.
    template <typename TActor, typename TComponent>
    TComponent* PropertyHandlers_ResolveComponent(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* Article,
        const TCHAR* Kind,
        TComponent* (*GetComponent)(TActor&),
        FString& OutActorName,
        TSharedPtr<FJsonObject>& OutError)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter: actor_name"));
            return nullptr;
        }
        AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), OutActorName);
        if (!Actor)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *OutActorName));
            return nullptr;
        }
        TActor* Target = Cast<TActor>(Actor);
        if (!Target)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Actor '%s' is not %s %s"), *OutActorName, Article, Kind));
            return nullptr;
        }
        TComponent* Component = GetComponent(*Target);
        if (!Component)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("%s '%s' has no component"), Kind, *OutActorName));
        }
        return Component;
    }"""

COUNT_SUPPLIED = """\
    // How many of Supplied are true: the params a set call supplies.
    int32 PropertyHandlers_CountSupplied(std::initializer_list<bool> Supplied)
    {
        int32 Count = 0;
        for (const bool bSupplied : Supplied)
        {
            Count += bSupplied ? 1 : 0;
        }
        return Count;
    }"""

# What a param of the wrong kind holds, for the errors of the readers that check the JSON type themselves.
DESCRIBE_JSON = """\
    // What Value holds, for the error of a param reader that expected something else.
    FString PropertyHandlers_DescribeJson(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return TEXT("null");
        }
        switch (Value->Type)
        {
        case EJson::String:  return FString::Printf(TEXT("the string '%s'"), *Value->AsString());
        case EJson::Number:  return FString::Printf(TEXT("the number %s"), *FString::SanitizeFloat(Value->AsNumber()));
        case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
        case EJson::Array:   return FString::Printf(TEXT("an array of %d element(s)"), Value->AsArray().Num());
        case EJson::Object:  return TEXT("an object");
        default:             return TEXT("null");
        }
    }"""

READERS = {
    "ReadFloat": """\
    // The optional number param Key in Out, which stays unset when Key is absent or null. An error reply naming Key,
    // with nothing changed, when it holds anything but a finite JSON number within the float range.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadFloat(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<float>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        const double Number = Value->Type == EJson::Number ? Value->AsNumber() : 0.0;
        if (Value->Type != EJson::Number || !FMath::IsFinite(Number)
            || FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be a finite number within the float range (got %s). Nothing was changed."),
                Key, *PropertyHandlers_DescribeJson(Value)));
        }
        Out = static_cast<float>(Number);
        return nullptr;
    }""",
    "ReadInt": """\
    // The optional whole-number param Key in Out, which stays unset when Key is absent or null. An error reply naming
    // Key, with nothing changed, when it holds anything but a whole JSON number within the int32 range.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadInt(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<int32>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        const double Number = Value->Type == EJson::Number ? Value->AsNumber() : 0.0;
        if (Value->Type != EJson::Number || Number != FMath::FloorToDouble(Number)
            || Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be a whole number within the int32 range (got %s). Nothing was changed."),
                Key, *PropertyHandlers_DescribeJson(Value)));
        }
        Out = static_cast<int32>(Number);
        return nullptr;
    }""",
    "ReadBool": """\
    // The optional bool param Key in Out, which stays unset when Key is absent or null. An error reply naming Key,
    // with nothing changed, when it holds anything but a JSON true or false.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadBool(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<bool>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        if (Value->Type != EJson::Boolean)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be true or false (got %s). Nothing was changed."), Key, *PropertyHandlers_DescribeJson(Value)));
        }
        Out = Value->AsBool();
        return nullptr;
    }""",
    "ReadLinearColor": """\
    // The optional color param Key, [R, G, B] or [R, G, B, A] with A defaulting to 1, in Out, which stays unset when
    // Key is absent or null. The error reply of mcp_common::ReadJsonLinearColor, naming Key, with nothing changed,
    // when it is not an array of 3 or 4 finite numbers.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadLinearColor(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<FLinearColor>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        FLinearColor Color;
        if (TSharedPtr<FJsonObject> Malformed = mcp_common::ReadJsonLinearColor(Params, Key, FLinearColor(0.0f, 0.0f, 0.0f, 1.0f), Color))
        {
            return Malformed;
        }
        Out = Color;
        return nullptr;
    }""",
    "ReadColorBytes": """\
    // The optional color param Key of 0-255 bytes, [R, G, B] or [R, G, B, A] with A defaulting to 255, in Out, which
    // stays unset when Key is absent or null. An error reply naming Key, with nothing changed, when it is not an array
    // of 3 or 4 whole numbers from 0 to 255: a fraction is most likely a linear 0-1 color, which would turn black.
    TSharedPtr<FJsonObject> PropertyHandlers_ReadColorBytes(
        const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, TOptional<FColor>& Out)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return nullptr;
        }
        FString Problem;
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (!Value->TryGetArray(Items) || !Items || Items->Num() < 3 || Items->Num() > 4)
        {
            Problem = TEXT("got ") + PropertyHandlers_DescribeJson(Value);
        }
        uint8 Channels[4] = { 0, 0, 0, 255 };
        for (int32 Index = 0; Problem.IsEmpty() && Index < Items->Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Item = (*Items)[Index];
            const double Number = Item.IsValid() && Item->Type == EJson::Number ? Item->AsNumber() : -1.0;
            if (Number < 0.0 || Number > 255.0 || Number != FMath::FloorToDouble(Number))
            {
                Problem = FString::Printf(TEXT("element %d is %s"), Index, *PropertyHandlers_DescribeJson(Item));
            }
            else
            {
                Channels[Index] = static_cast<uint8>(Number);
            }
        }
        if (!Problem.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be an array of 3 or 4 whole numbers from 0 to 255, [R, G, B] or [R, G, B, A] (%s). Nothing was changed."),
                Key, *Problem));
        }
        Out = FColor(Channels[0], Channels[1], Channels[2], Channels[3]);
        return nullptr;
    }""",
}

REFUSE_STATIC = """\
    // The refusal of a set call on Component when it is registered with Static mobility, and null for any other: the
    // engine setters change nothing on such a component (USceneComponent::AreDynamicDataChangesAllowed, protected,
    // outside a construction script), so a write would count fields that never reach it. Kind and ActorName name the
    // actor in the error.
    TSharedPtr<FJsonObject> PropertyHandlers_RefuseStaticComponent(
        const USceneComponent& Component, const TCHAR* Kind, const FString& ActorName)
    {
        if (!Component.IsRegistered() || Component.Mobility != EComponentMobility::Static)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s '%s' has Static mobility, and the engine's setters do not change a registered static component; ")
            TEXT("nothing was changed. Make its component Stationary or Movable first (USceneComponent::SetMobility, ")
            TEXT("e.g. call_function with component_class)."),
            Kind, *ActorName));
    }"""


def typed_setter_prelude(descriptors: list[dict]) -> str:
    types = {f["type"] for d in descriptors for f in d["fields"]}
    helpers = [RESOLVE_COMPONENT, COUNT_SUPPLIED]
    if types - {"linear_color"}:
        helpers.append(DESCRIBE_JSON)
    helpers += [READERS[FIELD_TYPES[t]["reader"]] for t in FIELD_TYPES if t in types]
    if any(d.get("refuse_static") for d in descriptors):
        helpers.append(REFUSE_STATIC)
    return "namespace\n{\n" + "\n\n".join(helpers) + "\n}"


def reader_line(field: dict) -> str:
    """The reader's line for one field: its readback value under its json name."""
    name = field["json"]
    value = f"Component->{field['readback']}"
    if field["type"] in COLOR_TYPES:
        return f'Properties->SetArrayField(TEXT("{name}"), mcp_common::ColorToJsonArray({value}));'
    if field["type"] == "bool":
        # A uint32 : 1 bitfield reads back as a comparison.
        return f'Properties->SetBoolField(TEXT("{name}"), {value}{" != 0" if field.get("bitfield") else ""});'
    return f'Properties->SetNumberField(TEXT("{name}"), {value});'


def typed_setter_unit(d: dict) -> list[str]:
    base = d["cmd_base"]
    camel = camel_case(base)
    kind = d["actor_class"][1:]  # the engine's class name: ASkyAtmosphere -> SkyAtmosphere
    component = d["component_class"]
    getter = f"{PREFIX}Get{camel}Component"
    reader = f"{PREFIX}{camel}Json"
    fields = d["fields"]
    for field in fields:
        if field["type"] not in FIELD_TYPES:
            raise ValueError(f"{base}: field {field['json']} has unknown type {field['type']}")
    # The TOptional each field's param is read into, named after the param.
    variables = [camel_case(f["json"]) for f in fields]
    clashes = sorted(set(variables) & RESERVED_NAMES) + sorted({v for v in variables if variables.count(v) > 1})
    if clashes:
        raise ValueError(f"{base}: field variable name(s) {clashes} clash with the handler's own names or each other")

    def resolve(constness: str) -> list[str]:
        return [
            "    FString ActorName;",
            "    TSharedPtr<FJsonObject> Error;",
            f"    {constness}{component}* Component = {PREFIX}ResolveComponent(",
            f'        Params, TEXT("{d["article"]}"), TEXT("{kind}"), &{getter}, ActorName, Error);',
            "    if (!Component)",
            "    {",
            "        return Error;",
            "    }",
            "",
        ]

    out = comment(d.get("header_comment", []))
    out += [
        "namespace",
        "{",
        f"    {component}* {getter}({d['actor_class']}& Actor)",
        "    {",
        *comment(d.get("accessor_comment", []), "        "),
        f"        return Actor.{d['accessor']};",
        "    }",
        "",
        f"    TSharedPtr<FJsonObject> {reader}(const {component}* Component)",
        "    {",
        "        TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();",
    ]
    for field in fields:
        out += comment(field.get("reader_comment", []), "        ")
        out.append(f"        {reader_line(field)}")
    out += ["        return Properties;", "    }", "}", ""]

    out.append(f"// MCP:set_{base}_properties - {d['set_desc']}")
    out += retired_lines(d, "set_retired")
    out.append(f"//   @param actor_name - Label/name of the target {kind} actor.")
    out += [f"//   @param {f['json']} [{FIELD_TYPES[f['type']]['hint']}] - {f['desc']}" for f in fields]
    out.append("//   @returns { actor_name: string, properties: object, applied_count: int, applied: bool }")
    out.append(f"TSharedPtr<FJsonObject> HandleSet{camel}Properties(const TSharedPtr<FJsonObject>& Params)")
    out.append("{")
    out += resolve("")
    out.append("    // Every supplied param is read and checked before the first write, so a malformed one changes nothing.")
    for field, variable in zip(fields, variables):
        ftype = FIELD_TYPES[field["type"]]
        out.append(f"    TOptional<{ftype['optional']}> {variable};")
        out.append(f'    if (TSharedPtr<FJsonObject> Malformed = {PREFIX}{ftype["reader"]}(Params, TEXT("{field["json"]}"), {variable})) '
                   "{ return Malformed; }")
    out.append("")
    out += wrap_call(f"    const int32 AppliedCount = {PREFIX}CountSupplied({{ ",
                     [f"{v}.IsSet()" for v in variables], " });")
    out += [
        "    if (AppliedCount == 0)",
        "    {",
        "        return mcp_common::MakeErrorReply(",
        f'            TEXT("No properties provided. Supply at least one of: {", ".join(f["json"] for f in fields)}"));',
        "    }",
    ]
    if d.get("refuse_static"):
        out.append(f'    if (TSharedPtr<FJsonObject> Refused = {PREFIX}RefuseStaticComponent(*Component, TEXT("{kind}"), ActorName))')
        out += ["    {", "        return Refused;", "    }"]
    out.append("")
    loctext_key = d.get("loctext_key", f"Set{camel}Properties")
    out.append(f'    const FScopedTransaction Transaction(LOCTEXT("{loctext_key}", "MCP: Set {spaced(camel)} Properties"));')
    out.append("    Component->Modify();")
    for field, variable in zip(fields, variables):
        out += [
            f"    if ({variable})",
            "    {",
            f"        Component->{field['setter']}(*{variable});",
            "    }",
        ]
    out += [
        "",
        "    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();",
        '    Result->SetStringField(TEXT("actor_name"), ActorName);',
        f'    Result->SetObjectField(TEXT("properties"), {reader}(Component));',
        '    Result->SetNumberField(TEXT("applied_count"), AppliedCount);',
        '    Result->SetBoolField(TEXT("applied"), true);',
        "    return mcp_common::MakeOkReply(Result);",
        "}",
        "",
    ]

    returned = ", ".join(f["json"] + (":[R,G,B,A]" if f["type"] in COLOR_TYPES else "") for f in fields)
    out.append(f"// MCP:get_{base}_properties - {d['get_desc']}")
    out += retired_lines(d, "get_retired")
    out.append(f"//   @param actor_name - Label/name of the target {kind} actor.")
    out.append(f"//   @returns {{ actor_name: string, properties: {{ {returned} }} }}")
    out.append(f"TSharedPtr<FJsonObject> HandleGet{camel}Properties(const TSharedPtr<FJsonObject>& Params)")
    out.append("{")
    out += resolve("const ")
    out += [
        "    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();",
        '    Result->SetStringField(TEXT("actor_name"), ActorName);',
        f'    Result->SetObjectField(TEXT("properties"), {reader}(Component));',
        "    return mcp_common::MakeOkReply(Result);",
        "}",
    ]
    return out


# ---------------------------------------------------------------------------
# reflection_merge
# ---------------------------------------------------------------------------
REFLECTION_MERGE_PRELUDE = """\
namespace
{
    // The TAsset at Path, or nullptr with OutError set when none loads from there (ClassName names it in the error).
    template <typename TAsset>
    TAsset* PropertyHandlers_LoadAsset(const FString& Path, const TCHAR* ClassName, TSharedPtr<FJsonObject>& OutError)
    {
        TAsset* Asset = Cast<TAsset>(StaticLoadObject(TAsset::StaticClass(), nullptr, *Path));
        if (!Asset)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load %s at %s"), ClassName, *Path));
        }
        return Asset;
    }

    // The reply data of both handlers: the asset's path under PathKey, its class, and Settings -- the
    // SettingsStruct value it holds -- read back through reflection.
    TSharedPtr<FJsonObject> PropertyHandlers_SettingsData(
        const UObject* Asset, const TCHAR* PathKey, const UScriptStruct* SettingsStruct, const void* Settings)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(PathKey, Asset->GetPathName());
        Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
        Data->SetObjectField(TEXT("settings"), mcp_common::StructToJson(SettingsStruct, Settings));
        return Data;
    }

    // What Value must be for Field when it is not that, else empty. FJsonObjectConverter reads a number or a bool out
    // of any JSON value without an error -- a string that is not a number becomes 0, any string a bool -- so those
    // kinds are checked here; the converter refuses a wrong kind for the others itself.
    FString PropertyHandlers_ExpectedJsonKind(const FProperty& Field, const FJsonValue& Value)
    {
        if (Field.ArrayDim != 1)
        {
            return FString();
        }
        const FNumericProperty* Numeric = CastField<FNumericProperty>(&Field);
        if (CastField<FEnumProperty>(&Field) || (Numeric && Numeric->IsEnum()))
        {
            return Value.Type == EJson::String || Value.Type == EJson::Number ? FString() : FString(TEXT("an enum name"));
        }
        if (Numeric)
        {
            const bool bNumber = Value.Type == EJson::Number || (Value.Type == EJson::String && Value.AsString().IsNumeric());
            return bNumber ? FString() : FString(TEXT("a number"));
        }
        if (CastField<FBoolProperty>(&Field))
        {
            return Value.Type == EJson::Boolean ? FString() : FString(TEXT("true or false"));
        }
        return FString();
    }

    // Null when every key of Json names a field of SettingsStruct that is not deprecated -- matched case-insensitively,
    // as FJsonObjectConverter matches them -- and holds a value of its kind (PropertyHandlers_ExpectedJsonKind); else
    // the error reply naming the first key that does not, with the fields. The converter skips a key that names no
    // field without a word, so it would read as applied.
    TSharedPtr<FJsonObject> PropertyHandlers_CheckSettingsKeys(const FJsonObject& Json, const UScriptStruct* SettingsStruct)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json.Values)
        {
            const FProperty* Field = nullptr;
            for (TFieldIterator<FProperty> It(SettingsStruct); It && !Field; ++It)
            {
                if (!It->HasAnyPropertyFlags(CPF_Deprecated)
                    && SettingsStruct->GetAuthoredNameForField(*It).Equals(Pair.Key, ESearchCase::IgnoreCase))
                {
                    Field = *It;
                }
            }
            if (!Field)
            {
                TArray<FString> Fields;
                for (TFieldIterator<FProperty> It(SettingsStruct); It; ++It)
                {
                    if (!It->HasAnyPropertyFlags(CPF_Deprecated))
                    {
                        Fields.Add(SettingsStruct->GetAuthoredNameForField(*It));
                    }
                }
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("settings key '%s' matches no field of %s (keys are field names, matched case-insensitively); ")
                    TEXT("nothing was changed. Fields: %s"),
                    *Pair.Key, *SettingsStruct->GetName(), *FString::Join(Fields, TEXT(", "))));
            }
            if (!Pair.Value.IsValid() || Pair.Value->IsNull())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("settings.%s is null: give it a value or leave the key out; nothing was changed."), *Pair.Key));
            }
            const FString Expected = PropertyHandlers_ExpectedJsonKind(*Field, *Pair.Value);
            if (!Expected.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("settings.%s must be %s for %s (%s); nothing was changed."),
                    *Pair.Key, *Expected, *Field->GetName(), *Field->GetCPPType()));
            }
        }
        return nullptr;
    }

    // Merges the required settings param into Settings, the SettingsStruct value Asset holds: only the keys it
    // carries change, and MergeJsonWithClamp also enforces the ClampMin/ClampMax meta the JSON converter ignores.
    // An empty settings object, a key PropertyHandlers_CheckSettingsKeys refuses or a value that does not convert
    // is an error, with nothing changed. Then saves Asset and replies with PropertyHandlers_SettingsData and the
    // keys applied; the reply succeeds only when the save reached disk (Command names the command in its error).
    TSharedPtr<FJsonObject> PropertyHandlers_MergeSettings(
        const TSharedPtr<FJsonObject>& Params,
        UObject* Asset,
        const TCHAR* PathKey,
        const UScriptStruct* SettingsStruct,
        void* Settings,
        const TCHAR* Command)
    {
        const TSharedPtr<FJsonObject>* SettingsJson = nullptr;
        if (!Params->TryGetObjectField(TEXT("settings"), SettingsJson) || !SettingsJson || !SettingsJson->IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter: settings (object)"));
        }
        if ((*SettingsJson)->Values.Num() == 0)
        {
            return mcp_common::MakeErrorReply(TEXT("settings is empty: give at least one field to set; nothing was changed."));
        }
        if (TSharedPtr<FJsonObject> Refused = PropertyHandlers_CheckSettingsKeys(**SettingsJson, SettingsStruct))
        {
            return Refused;
        }

        // The merge runs on a copy that replaces Settings only once all of it converted: the converter writes field
        // by field and stops at the first value that does not convert, which would leave the asset half-written.
        FStructOnScope Merged(SettingsStruct);
        SettingsStruct->CopyScriptStruct(Merged.GetStructMemory(), Settings);
        FString MergeError;
        if (!mcp_common::MergeJsonWithClamp(*SettingsJson, SettingsStruct, Merged.GetStructMemory(), MergeError))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("settings does not convert to %s: %s; nothing was changed."),
                *SettingsStruct->GetName(), *MergeError.Replace(TEXT("\\n"), TEXT(" "))));
        }
        Asset->Modify();
        SettingsStruct->CopyScriptStruct(Settings, Merged.GetStructMemory());
        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);

        TArray<TSharedPtr<FJsonValue>> AppliedKeys;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SettingsJson)->Values)
        {
            AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
        TSharedPtr<FJsonObject> Data = PropertyHandlers_SettingsData(Asset, PathKey, SettingsStruct, Settings);
        Data->SetArrayField(TEXT("applied_keys"), AppliedKeys);
        return mcp_common::MakeSavedReply(Data, bSaved, Command, SaveError);
    }
}"""


def reflection_merge_prelude(descriptors: list[dict]) -> str:
    return REFLECTION_MERGE_PRELUDE


def reflection_merge_unit(d: dict) -> list[str]:
    base = d["cmd_base"]
    camel = camel_case(base)
    asset = d["asset_class"]
    path_param = d["path_param"]
    loader = f"{PREFIX}Load{camel}"
    settings = f'Asset, TEXT("{path_param}"), {d["struct_type"]}::StaticStruct(), &Asset->{d["struct_field"]}'

    def load(constness: str) -> list[str]:
        return [
            "    TSharedPtr<FJsonObject> Error;",
            f"    {constness}{asset}* Asset = {loader}(Params, Error);",
            "    if (!Asset)",
            "    {",
            "        return Error;",
            "    }",
        ]

    out = [
        "namespace",
        "{",
        f"    {asset}* {loader}(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutError)",
        "    {",
        "        FString Path;",
        f'        if (!Params->TryGetStringField(TEXT("{path_param}"), Path) || Path.IsEmpty())',
        "        {",
        f'            OutError = mcp_common::MakeErrorReply(TEXT("Missing required parameter: {path_param}"));',
        "            return nullptr;",
        "        }",
        f'        return {PREFIX}LoadAsset<{asset}>(Path, TEXT("{asset}"), OutError);',
        "    }",
        "}",
        "",
        f"// MCP:set_{base}_properties - {d['set_desc']}",
        *retired_lines(d, "set_retired"),
        f"//   @param {path_param} - Full content path of the target {asset}.",
        f"//   @param settings - {d['settings_desc']}",
        f"//   @returns {{ {path_param}: string, asset_class: string, settings: object, applied_keys: [string], saved: bool }}",
        f"TSharedPtr<FJsonObject> HandleSet{camel}Properties(const TSharedPtr<FJsonObject>& Params)",
        "{",
        *load(""),
        f"    return {PREFIX}MergeSettings(",
        f"        Params, {settings},",
        f'        TEXT("set_{base}_properties"));',
        "}",
        "",
        f"// MCP:get_{base}_properties - {d['get_desc']}",
        *retired_lines(d, "get_retired"),
        f"//   @param {path_param} - Full content path of the target {asset}.",
        f"//   @returns {{ {path_param}: string, asset_class: string, settings: object }}",
        f"TSharedPtr<FJsonObject> HandleGet{camel}Properties(const TSharedPtr<FJsonObject>& Params)",
        "{",
        *load("const "),
        f"    return mcp_common::MakeOkReply({PREFIX}SettingsData(",
        f"        {settings}));",
        "}",
    ]
    return out


# ---------------------------------------------------------------------------
# mode -> (the shared helpers of its descriptors, the unit of one descriptor)
MODES = {
    "typed_setter": (typed_setter_prelude, typed_setter_unit),
    "reflection_merge": (reflection_merge_prelude, reflection_merge_unit),
}


def emit_region(descriptors: list[dict]) -> str:
    """The code of one region: the shared helpers of each mode its descriptors use, then their units."""
    unknown = {d["mode"] for d in descriptors} - MODES.keys()
    if unknown:
        raise ValueError(f"unknown descriptor mode(s): {sorted(unknown)}")
    blocks = []
    for mode, (prelude, _) in MODES.items():
        of_mode = [d for d in descriptors if d["mode"] == mode]
        if of_mode:
            blocks.append(prelude(of_mode))
    blocks += ["\n".join(MODES[d["mode"]][1](d)) for d in descriptors]
    return "\n\n".join(blocks) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Print the generated region code for some descriptors.")
    parser.add_argument("descriptors", nargs="+", help="descriptor .json files, in region order")
    args = parser.parse_args(argv)
    descriptors = []
    for path in args.descriptors:
        with open(path, encoding="utf-8") as fh:
            descriptors.append(json.load(fh))
    sys.stdout.write(emit_region(descriptors))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
