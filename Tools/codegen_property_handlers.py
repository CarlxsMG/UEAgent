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
  reflection_merge  An asset's settings struct, merged from the settings param through reflection.
                      asset_class, path_param    the asset type, and the param that holds its path
                      struct_type, struct_field  the settings struct, and the asset member holding it
                      settings_desc              the description of the settings param
Both modes take cmd_base (the <x>), set_desc and get_desc.

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


def camel_case(snake: str) -> str:
    """sky_atmosphere -> SkyAtmosphere."""
    return "".join(word.capitalize() for word in snake.split("_"))


def spaced(camel: str) -> str:
    """SkyAtmosphere -> Sky Atmosphere."""
    return re.sub(r"(?<=[a-z])(?=[A-Z])", " ", camel)


def comment(lines: list[str], indent: str = "") -> list[str]:
    return [f"{indent}// {line}" for line in lines]


# ---------------------------------------------------------------------------
# typed_setter
# ---------------------------------------------------------------------------
# Per field type: the @param hint, the condition that reads (and parses) the param into a temporary,
# and the setter's argument.
FIELD_TYPES = {
    "float": {
        "hint": "number",
        "read": 'Params->TryGetNumberField(TEXT("{json}"), Number)',
        "value": "static_cast<float>(Number)",
    },
    "int": {
        "hint": "number",
        "read": 'Params->TryGetNumberField(TEXT("{json}"), Number)',
        "value": "static_cast<int32>(Number)",
    },
    "bool": {
        "hint": "bool",
        "read": 'Params->TryGetBoolField(TEXT("{json}"), bValue)',
        "value": "bValue",
    },
    "linear_color": {
        "hint": "array",
        "read": f'Params->TryGetArrayField(TEXT("{{json}}"), Values) && {PREFIX}ParseLinearColor(Values, LinearColor)',
        "value": "LinearColor",
    },
    "color_bytes": {
        "hint": "array",
        "read": f'Params->TryGetArrayField(TEXT("{{json}}"), Values) && {PREFIX}ParseColorBytes(Values, ByteColor)',
        "value": "ByteColor",
    },
}
COLOR_TYPES = {"linear_color", "color_bytes"}

# The temporaries the reads fill, in declaration order; a set handler declares the ones its fields use.
TEMPORARIES = [
    ("Number", "double Number = 0.0;"),
    ("bValue", "bool bValue = false;"),
    ("Values", "const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;"),
    ("LinearColor", "FLinearColor LinearColor(ForceInit);"),
    ("ByteColor", "FColor ByteColor(ForceInit);"),
]

# The helpers of a typed_setter region: the resolver, and each color parser its fields need.
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

PARSE_LINEAR_COLOR = """\
    // A color param, [R, G, B] or [R, G, B, A] with A defaulting to 1; false when it has fewer than three entries.
    bool PropertyHandlers_ParseLinearColor(const TArray<TSharedPtr<FJsonValue>>* Values, FLinearColor& OutColor)
    {
        FVector4 Rgba;
        if (!ParseColorVec4(Values, 1.0, Rgba))
        {
            return false;
        }
        OutColor = FLinearColor(Rgba.X, Rgba.Y, Rgba.Z, Rgba.W);
        return true;
    }"""

PARSE_COLOR_BYTES = """\
    // An [R, G, B] param of 0-255 bytes as an opaque FColor, each channel clamped; false when it has fewer than
    // three entries.
    bool PropertyHandlers_ParseColorBytes(const TArray<TSharedPtr<FJsonValue>>* Values, FColor& OutColor)
    {
        if (!Values || Values->Num() < 3)
        {
            return false;
        }
        auto Channel = [Values](int32 Index)
        {
            return static_cast<uint8>(FMath::Clamp(static_cast<int32>((*Values)[Index]->AsNumber()), 0, 255));
        };
        OutColor = FColor(Channel(0), Channel(1), Channel(2), 255);
        return true;
    }"""


def typed_setter_prelude(descriptors: list[dict]) -> str:
    types = {f["type"] for d in descriptors for f in d["fields"]}
    helpers = [RESOLVE_COMPONENT]
    if "linear_color" in types:
        helpers.append(PARSE_LINEAR_COLOR)
    if "color_bytes" in types:
        helpers.append(PARSE_COLOR_BYTES)
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
    reads = [FIELD_TYPES[f["type"]]["read"].format(json=f["json"]) for f in fields]

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
    out.append(f"//   @param actor_name - Label/name of the target {kind} actor.")
    out += [f"//   @param {f['json']} [{FIELD_TYPES[f['type']]['hint']}] - {f['desc']}" for f in fields]
    out.append("//   @returns { actor_name: string, properties: object, applied_count: int, applied: bool }")
    out.append(f"TSharedPtr<FJsonObject> HandleSet{camel}Properties(const TSharedPtr<FJsonObject>& Params)")
    out.append("{")
    out += resolve("")
    loctext_key = d.get("loctext_key", f"Set{camel}Properties")
    out.append(f'    const FScopedTransaction Transaction(LOCTEXT("{loctext_key}", "MCP: Set {spaced(camel)} Properties"));')
    out.append("    Component->Modify();")
    out.append("")
    out.append("    int32 AppliedCount = 0;")
    out += [f"    {decl}" for name, decl in TEMPORARIES if any(re.search(rf"\b{name}\b", read) for read in reads)]
    out.append("")
    for field, read in zip(fields, reads):
        out += [
            f"    if ({read})",
            "    {",
            f"        Component->{field['setter']}({FIELD_TYPES[field['type']]['value']});",
            "        ++AppliedCount;",
            "    }",
        ]
    out += [
        "",
        "    if (AppliedCount == 0)",
        "    {",
        "        return mcp_common::MakeErrorReply(",
        f'            TEXT("No properties provided. Supply at least one of: {", ".join(f["json"] for f in fields)}"));',
        "    }",
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

    // Merges the required settings param into Settings, the SettingsStruct value Asset holds: only the keys it
    // carries change, and MergeJsonWithClamp also enforces the ClampMin/ClampMax meta the JSON converter ignores.
    // Then saves Asset and replies with PropertyHandlers_SettingsData and the keys applied; the reply succeeds
    // only when the save reached disk (Command names the command in its error).
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

        Asset->Modify();
        FString MergeError;
        if (!mcp_common::MergeJsonWithClamp(*SettingsJson, SettingsStruct, Settings, MergeError))
        {
            return mcp_common::MakeErrorReply(MergeError);
        }
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
