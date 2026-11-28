"""
param_schema.py -- the JSON Schema detail of a tool parameter, read off its catalog entry.

codegen_command_catalog.py builds each param from the C++ (name, jsonType, required, intRange,
vectorShape) and the `@param` prose (description). This module adds what a client needs to build a
correct call and that the prose only says in words:

  * `jsonType` "integer" for a number every read of which lands in an integer variable (intRange);
  * `items`, the schema of an array's elements (number for a vector, string for a list of names...,
    `{}` when nothing says), so no array is published without one;
  * `enum`, the closed set of values of a string, when the prose PROVABLY lists all of them;
  * `default`, the value the handler uses when the param is left out, when the prose states one
    literal of the param's type and nothing else;
  * a description for an alias the annotation does not document.

Every rule here stays on the side of publishing nothing: a wrong enum turns a call that works into a
validation error, and a wrong default misleads the agent, so a doubtful case is left out.

`problems` lists what a param's schema must never be (an array without items, a default that does not
fit its type or enum); codegen_command_catalog.py fails on them, and catalog_server.py --check checks
the schemas the server publishes.
"""

from __future__ import annotations

import json
import re

TYPES = ("string", "number", "integer", "boolean", "array", "object")

# ---------------------------------------------------------------------------
# enum
# ---------------------------------------------------------------------------
# The rule the server used to apply to the prose at load time: quoted lower-case tokens and a trigger word.
_ENUM_TRIGGER = re.compile(r"\bone of\b|\beither\b|\(default\)|\bor\b", re.IGNORECASE)
_QUOTED = re.compile(r"'([^'\n]*)'")
_ENUM_TOKEN = re.compile(r"^[a-z][a-z0-9_]{1,39}$")
_UNQUOTED_ALTERNATIVES = re.compile(r"\b(?:to|one of|either)\s+((?:[a-z]+\s*,\s*)+(?:or\s+)?[a-z]+)", re.IGNORECASE)
_ALTERNATIVE_SEPARATOR = re.compile(r",|\bor\b")

# The lists written as `'a' | 'b' | 'c'`, `"A"|"B"`, `A | B | C`, or `one of "A", "B" or "C"`, each value
# optionally followed by a parenthesised note (`'pie' (the PIE world)`).
_TOKEN = r"[A-Za-z][A-Za-z0-9_]{0,63}"
_NOTE = r"(?:\s*\((?:[^()]|\([^()]*\))*\))?"
_PIPE_ITEM = rf"(?:'{_TOKEN}'|\"{_TOKEN}\"|{_TOKEN}){_NOTE}"
_PIPE_RUN = re.compile(rf"(?<![\w/.|'\"]){_PIPE_ITEM}(?:\s*\|\s*{_PIPE_ITEM})+(?![\w/]|\.\w|\s*\|)")
_QITEM = rf"(?:'{_TOKEN}'|\"{_TOKEN}\"){_NOTE}"
_ONE_OF_RUN = re.compile(rf"\b(?:one of|either)\s+({_QITEM}(?:\s*,\s*{_QITEM})*(?:\s*,?\s*or\s+{_QITEM}))", re.IGNORECASE)
_RUN_VALUE = re.compile(rf"'({_TOKEN})'|\"({_TOKEN})\"|(?<![\w(])({_TOKEN})(?![\w'\"])")
_ANY_QUOTED = re.compile(r"'([^'\n]*)'|\"([^\"\n]*)\"")
# Prose that says the list is not the whole set.
_OPEN_SET = re.compile(
    r"\be\.g\.|\betc\b|\bsuch as\b|\bfor example\b|\bor any\b|\bor an?\b|\bcustom\b|\bwildcard|\bpattern\b"
    r"|\bany other\b(?!\s+(?:value|text|name)s?\s+(?:is|are)\s+(?:refused|rejected|an error|invalid))",
    re.IGNORECASE)


def _legacy_enum(description: str) -> list[str]:
    if not _ENUM_TRIGGER.search(description):
        return []
    quoted = _QUOTED.findall(description)
    if not quoted or not all(_ENUM_TOKEN.match(value) for value in quoted):
        return []
    values = list(dict.fromkeys(quoted))
    if not 2 <= len(values) <= 12:
        return []
    # Alternatives listed without quotes: then the quoted values are only some of the valid ones.
    alternatives = _UNQUOTED_ALTERNATIVES.search(_QUOTED.sub("<Q>", description))
    if alternatives:
        known = {value.lower() for value in values}
        for word in _ALTERNATIVE_SEPARATOR.split(alternatives.group(1)):
            word = word.strip()
            if word and word.lower() not in known:
                return []
    return values


def _run_values(run: str) -> list[str]:
    run = re.sub(r"\((?:[^()]|\([^()]*\))*\)", " ", run)  # the notes are not values
    run = re.sub(r"\b(?:or)\b", ",", run)
    return list(dict.fromkeys(next(g for g in m.groups() if g) for m in _RUN_VALUE.finditer(run)))


def _listed_enum(description: str) -> list[str]:
    if _OPEN_SET.search(description):
        return []
    runs = [m.group(0) for m in _PIPE_RUN.finditer(description)]
    runs += [m.group(1) for m in _ONE_OF_RUN.finditer(description)]
    if len(runs) != 1:
        return []  # none, or two lists: which one is this param's is a guess
    values = _run_values(runs[0])
    if not 2 <= len(values) <= 30:
        return []
    # Every quoted word of the prose is one of the values: a quoted word outside the list is a value it left out.
    quoted = {a or b for a, b in _ANY_QUOTED.findall(description)}
    if not quoted <= set(values):
        return []
    return values


# Prose that names spellings the handler takes besides the listed ones ("5.1 is accepted too", "Aliases x, y").
_ALIASES_ACCEPTED = re.compile(r"\baccepted\b|\baliase?s?\b|\bsynonyms?\b", re.IGNORECASE)


def enum_values(description: str) -> list[str]:
    """The closed set of values a string param's description names, or [] unless it is provably one."""
    if not description or _ALIASES_ACCEPTED.search(description):
        return []  # an enum of the listed values would refuse the aliases the handler takes
    if _PIPE_RUN.search(description) or _ONE_OF_RUN.search(description):
        return _listed_enum(description)  # a written-out list decides, quoted words elsewhere are not values
    return _legacy_enum(description)


# ---------------------------------------------------------------------------
# items
# ---------------------------------------------------------------------------
_NUMBER_ITEM = {"type": "number"}
_STRING_WORDS = frozenset({
    "names", "paths", "tags", "labels", "ids", "guids", "classes", "files", "roots", "entries", "states", "args",
    "kinds", "rules", "sections", "properties", "widgets", "prefixes", "plugins", "filters", "identifiers",
    "strings", "channels", "assets", "categories", "folders", "packages", "modules", "bones", "sockets",
    "libraries", "patterns", "actors", "materials", "tokens",
})
_NUMBER_NOUNS = frozenset({"numbers", "floats", "doubles", "widths", "depths", "weights", "thresholds", "values"})
_INTEGER_NOUNS = frozenset({"integers", "indices", "indexes"})
# Mesh elements are numbered: their ids are integers, where a node's or an asset's id is a string.
_INTEGER_ID_OWNERS = frozenset({"triangle", "vertex", "polygroup", "edge", "element", "section", "material"})
_OBJECT_NOUNS = frozenset({"objects", "dicts", "descriptors", "specs", "rows", "pairs"})
_FIRST_BRACKET = re.compile(r"\[\s*(\[|\{|'|\"|-?\d|[A-Za-z])")
_NUMERIC_TUPLE = re.compile(
    r"\[\s*(?:-?\d+(?:\.\d+)?|[A-Za-z][\w()]*)(?:\s*,\s*(?:-?\d+(?:\.\d+)?|[A-Za-z][\w()]*|\.\.\.|\([A-Za-z]\)?))+\s*\]")
_LIST_OF = re.compile(r"\b(?:array|list|set)\s+of\s+((?:[\w-]+\s+){0,5}[\w-]+)", re.IGNORECASE)
_ELEMENT_OPENER = re.compile(r"\b(?:array|list|set)\s+of\s+(?:\w+\s+){0,2}$|\beach\s+(?:an?\s+)?$", re.IGNORECASE)
_OR_OBJECT = re.compile(r"\bor\s+(?:an?\s+)?(?:objects?|dicts?)\b|\bor\s+\{", re.IGNORECASE)
_INTEGER_TEXT = re.compile(r"\binteger", re.IGNORECASE)
# "each item is either a plain string or an object": elements of two types
_MIXED_ELEMENTS = re.compile(r"\b(?:either|or)\s+(?:an?\s+)?(?:plain\s+)?string\b", re.IGNORECASE)


def _last_word(name: str) -> str:
    return name.lower().rsplit("_", 1)[-1]


def _bracket_schema(text: str, first: re.Match) -> dict | None:
    """The element schema the first bracketed example says, or None when it says none."""
    opener = first.group(1)
    if opener == "{":
        return {"type": "object"}
    if opener in ("'", '"'):
        return {"type": "string"}
    element_level = bool(_ELEMENT_OPENER.search(text[:first.start()]))
    if opener == "[":
        if _NUMERIC_TUPLE.match(text, first.start() + 1) and not _OR_OBJECT.search(text):
            return {"type": "array", "items": dict(_NUMBER_ITEM)}
        return {}
    if not _NUMERIC_TUPLE.match(text, first.start()):
        return None
    if element_level:
        # "List of [x,y,z] locations": the tuple is one element
        return {} if _OR_OBJECT.search(text) else {"type": "array", "items": dict(_NUMBER_ITEM)}
    return dict(_NUMBER_ITEM)


def items_schema(name: str, description: str, vector_shape: str | None, rotation: bool) -> dict:
    """The schema of an array param's elements; `{}` (any element) when nothing says which."""
    if vector_shape or rotation:
        return dict(_NUMBER_ITEM)
    text = description or ""
    if _MIXED_ELEMENTS.search(text):
        return {}
    first = _FIRST_BRACKET.search(text)
    brace = text.find("{")
    if first and (brace < 0 or first.start() < brace):
        found = _bracket_schema(text, first)
        if found is not None:
            return found
    elif brace >= 0 and re.search(r"\b(?:array|list) of (?:\w+ )?\{|\bobjects?\s*\{|^\s*(?:\w+\s+){0,2}\{", text, re.IGNORECASE):
        return {"type": "object"}
    if _last_word(name) == "ids" and name.lower().split("_")[0] in _INTEGER_ID_OWNERS:
        return {"type": "integer"}
    listed = _LIST_OF.search(text)
    words = [w.lower() for w in listed.group(1).split()] if listed else []
    for word in words:
        if word in _OBJECT_NOUNS:
            return {"type": "object"} if not _OR_OBJECT.search(text) and "string" not in text.lower() else {}
        if word in _INTEGER_NOUNS or (word == "ids" and _INTEGER_TEXT.search(text)):
            return {"type": "integer"}
        if word in _NUMBER_NOUNS:
            return {"type": "integer"} if _INTEGER_TEXT.search(text) else dict(_NUMBER_ITEM)
        if word in _STRING_WORDS:
            return {"type": "string"}
    last = _last_word(name)
    if "{" in text or re.search(r"\bobjects\b", text, re.IGNORECASE):
        return {}
    if last in _STRING_WORDS:
        return {"type": "string"}
    if last in _NUMBER_NOUNS:
        return dict(_NUMBER_ITEM)
    return {}


# ---------------------------------------------------------------------------
# default
# ---------------------------------------------------------------------------
_DEFAULT_WORD = re.compile(r"\bdefaults?\b(?:\s+(?:to|is|value is)\b)?\s*[:=]?\s*", re.IGNORECASE)
_MARKED_DEFAULT = re.compile(rf"(?:'({_TOKEN})'|\"({_TOKEN})\"|(?<![\w'\"])({_TOKEN}))\s*\(default\b[);,:]?", re.IGNORECASE)
# Prose where the value left out is not one default: the current value is kept, or a default holds only sometimes.
_NO_SINGLE_DEFAULT = re.compile(
    r"\bunchanged\b|\bkeeps?\s+(?:the\s+|its\s+)?current\b|\bleaves?\s+(?:it|the\s+\w+)\s+as\s+is\b"
    r"|\bdefault\s+applies\b|\bnew\s+\w+\s+only\b|\bdefault\s+(?:depends|varies)\b",
    re.IGNORECASE)


def _literal(text: str):
    """The literal that starts `text` and is followed by the end of a phrase, or None."""
    end = r"(?=\s*(?:$|[);,:=]|\.(?:\s|$)|\s+(?:=|when|if|on|unless|for|and|in|of|means|or|-|—)\b|\s*\())"
    m = re.match(r"'([^'\n]*)'|\"([^\"\n]*)\"", text)
    if m:
        return ("str", m.group(1) if m.group(1) is not None else m.group(2))
    m = re.match(rf"(true|false){end}", text)
    if m:
        return ("bool", m.group(1) == "true")
    m = re.match(r"\[[^\[\]]*\]", text)
    if m:
        try:
            value = json.loads(m.group(0))
        except ValueError:
            return None
        return ("list", value) if re.match(rf"\[[^\[\]]*\]{end}", text) else None
    m = re.match(rf"(-?\d+(?:\.\d+)?)(?!\s*(?:,\s*-?\d|\.\.|%|x\b|\*|/))(?![\w.]\w){end}", text)
    if m:
        number = float(m.group(1))
        return ("num", int(number) if "." not in m.group(1) else number)
    m = re.match(rf"(/[A-Za-z][\w/]*){end}", text)
    if m:
        return ("path", m.group(1))
    m = re.match(rf"({_TOKEN}){end}", text)
    if m:
        return ("word", m.group(1))
    return None


def _phrases(description: str) -> tuple[list, bool]:
    """Every literal the prose gives as THE default, and whether a default phrase gave none."""
    found = []
    unparsed = False
    for m in _DEFAULT_WORD.finditer(description):
        before = description[:m.start()].rstrip()
        if before and before[-1] not in "(,;:.—-":
            continue  # "engine default 0", "the class default": another thing's default
        rest = description[m.end():]
        if rest.startswith(")") or rest.startswith(";") or rest.startswith(","):
            continue  # the `X (default)` form, read by _MARKED_DEFAULT
        literal = _literal(rest)
        if literal is None:
            unparsed = True
        else:
            found.append(literal)
    for m in _MARKED_DEFAULT.finditer(description):
        quoted = m.group(1) or m.group(2)
        found.append(("str", quoted) if quoted else ("word", m.group(3)))
    return found, unparsed


def _fits(kind: str, value, json_type: str, enum: list[str], items: dict, int_range) -> tuple[bool, object]:
    if json_type == "string":
        if kind in ("str", "path") or (kind == "word" and value in enum):
            return (not enum or value in enum), value
        return False, None
    if json_type == "boolean":
        return kind == "bool", value
    if json_type in ("number", "integer"):
        if kind != "num":
            return False, None
        if json_type == "integer":
            if float(value) != int(value):
                return False, None
            value = int(value)
            if int_range and not int_range[0] <= value <= int_range[1]:
                return False, None
        return True, value
    if json_type == "array" and kind == "list":
        item_type = items.get("type")
        if item_type in ("number", "integer"):
            ok = all(isinstance(v, (int, float)) and not isinstance(v, bool) for v in value)
            if item_type == "integer":
                ok = ok and all(float(v) == int(v) for v in value)
            return ok, value
        if item_type == "string":
            return all(isinstance(v, str) for v in value), value
    return False, None


def default_value(param: dict):
    """(True, value) when the description states one default literal that fits the param, else (False, None)."""
    json_type = param.get("jsonType")
    description = param.get("description") or ""
    if param.get("required") or param.get("aliasOf") or json_type not in TYPES or json_type == "object":
        return False, None
    if _NO_SINGLE_DEFAULT.search(description):
        return False, None
    found, unparsed = _phrases(description)
    if not found or unparsed:
        return False, None
    values = []
    for kind, value in found:
        ok, fitted = _fits(kind, value, json_type, param.get("enum") or [], param.get("items") or {},
                           param.get("intRange"))
        if not ok:
            return False, None
        values.append(fitted)
    if len({json.dumps(v) for v in values}) != 1:
        return False, None
    return True, values[0]


# ---------------------------------------------------------------------------
# the whole param
# ---------------------------------------------------------------------------
def looks_like_rotation(name: str) -> bool:
    """As FMcpToolCatalog's LooksLikeRotationName: an array the server also accepts as {roll, pitch, yaw}."""
    low = name.lower()
    return low == "rot" or low.endswith("rotation") or low.endswith("rotator")


# A number the annotation calls an int, unless the prose also lets a fraction through (rounded, truncated, a float).
_SAYS_INTEGER = re.compile(r"\b(?:integer|whole number|int32|int)\b", re.IGNORECASE)
_TAKES_FRACTION = re.compile(r"float|fraction|decimal|\bdouble\b|rounded|truncat|non-integer|seconds", re.IGNORECASE)


def says_integer(description: str) -> bool:
    return bool(_SAYS_INTEGER.search(description)) and not _TAKES_FRACTION.search(description)


def alias_description(canonical: str) -> str:
    return f"Alias of `{canonical}`: the same value with the same meaning. Pass one of the two."


def refine(param: dict) -> dict:
    """The param with its schema detail, its fields in the catalog's order."""
    json_type = param["jsonType"]
    if json_type == "number" and (param.get("intRange") or says_integer(param.get("description") or "")):
        json_type = "integer"
    out = {"name": param["name"], "jsonType": json_type, "required": param["required"]}
    for key in ("intRange", "nameLike", "vectorShape"):
        if key in param:
            out[key] = param[key]
    types = json_type.split("|")
    description = param.get("description") or ""
    if "array" in types:
        out["items"] = items_schema(param["name"], description, param.get("vectorShape"),
                                    json_type == "array" and looks_like_rotation(param["name"]))
    if json_type == "string":
        enum = enum_values(description)
        if enum:
            out["enum"] = enum
    if not description.strip() and param.get("aliasOf"):
        description = alias_description(param["aliasOf"])
    out["description"] = description
    for key in param:
        if key not in out and key not in ("enum", "items", "default"):
            out[key] = param[key]
    if "enum" in out and _states_other_default(out):
        del out["enum"]  # the prose names a default the list leaves out: the list is not the whole set
    ok, value = default_value(out)
    if ok:
        out["default"] = value
    return out


def _states_other_default(param: dict) -> bool:
    found, _ = _phrases(param.get("description") or "")
    return any(kind in ("str", "word") and value not in param["enum"] for kind, value in found)


# ---------------------------------------------------------------------------
# validation
# ---------------------------------------------------------------------------
def _valid_items(items) -> bool:
    if not isinstance(items, dict):
        return False
    if not items:
        return True
    if items.get("type") not in TYPES:
        return False
    return items["type"] != "array" or _valid_items(items.get("items"))


def _matches(value, schema: dict) -> bool:
    kind = schema.get("type")
    if kind is None:
        return True
    if kind == "string":
        return isinstance(value, str) and (not schema.get("enum") or value in schema["enum"])
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if kind == "array":
        return isinstance(value, list) and all(_matches(v, schema.get("items") or {}) for v in value)
    if kind == "object":
        return isinstance(value, dict)
    return False


def problems(param: dict) -> list[str]:
    """What is wrong with a catalog param's schema detail; [] when nothing is."""
    out = []
    types = (param.get("jsonType") or "any").split("|")
    if any(t not in TYPES + ("any",) for t in types) or ("any" in types and len(types) > 1):
        out.append(f"jsonType `{param.get('jsonType')}` is not a JSON type or a union of them")
    if "array" in types and not _valid_items(param.get("items")):
        out.append("an array without a valid `items`")
    if "items" in param and "array" not in types:
        out.append("`items` on a param that is no array")
    if "enum" in param:
        enum = param["enum"]
        if param.get("jsonType") != "string" or not isinstance(enum, list) or len(enum) < 2 \
                or not all(isinstance(v, str) for v in enum) or len(set(enum)) != len(enum):
            out.append("`enum` is not a list of two or more distinct strings on a string param")
    if "default" in param:
        schema = {"type": param.get("jsonType"), "enum": param.get("enum"), "items": param.get("items")}
        if len(types) != 1 or not _matches(param["default"], schema):
            out.append(f"default {json.dumps(param['default'])} does not fit the param's type or enum")
        rng = param.get("intRange")
        if rng and isinstance(param["default"], (int, float)) and not rng[0] <= param["default"] <= rng[1]:
            out.append(f"default {param['default']} is outside intRange {rng}")
        if param.get("required"):
            out.append("a required param with a default")
    if not (param.get("description") or "").strip():
        out.append("no description")
    return out
