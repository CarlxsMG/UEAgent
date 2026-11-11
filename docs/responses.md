# Answers and refusals

What a tool call returns, and how to tell a call that worked from one that did not. Every agent gets the
short version of this page when it connects, in the server's `initialize` instructions.

## The answer

A call answers with one text block that holds a JSON object. Most tools answer with these fields:

| Field | |
|---|---|
| `success` | `true` when the call did what it reports. [Readback](#readback) can turn it to `false` |
| `data` | The tool's own fields: its **Returns** line in the [tool reference](tools/README.md) lists them |
| `error` | Why the call failed, when `success` is `false` |
| `readback` | What readback found, when it checked the call |

`data` can also carry two reports. Neither stops the call:

- `unknown_params`: the arguments the tool does not read, and so ignored. Usually a typo, or the
  parameter of a sibling tool.
- `aliased_params`: the arguments spelled the way a sibling tool spells them, which the server renamed
  before the call.

A call that ran and failed answers `success: false` with its `error`. It is not a tool error: the agent
reads `success` itself.

## Images

The capture tools (`capture_viewport`, `capture_high_res_screenshot`, `capture_pie_screenshot`,
`render_widget_preview`, `generate_asset_thumbnail`) write a PNG and answer with the JSON text block and
then an `image` block (`data` in base64, `mimeType`) with the picture. The JSON says what was sent, next
to the file's path:

| Field | |
|---|---|
| `image_returned` | Whether the image block is in the answer |
| `image_width`, `image_height` | The size of the image sent |
| `image_note` | Why the image was shrunk or left out |

A picture over the limits (1568 px on its longest side, about 1 MB encoded: **Image max dimension** and
**Image max bytes** in Editor Preferences > Plugins > UEAgent) is sent as a smaller JPEG copy; the file on
disk keeps its full resolution. `execute_batch` answers with its items' images in item order, at most 4;
a note names the rest. `return_image: false` on a call, **Return images** off, or `?images=off` at the
end of the server URL leave the image out: the answer is the text block alone.

## Readback

After the call, in the same pass on the game thread, readback re-reads the state the call changed. When
it could check the call, the answer carries a `readback` object:

| Field | |
|---|---|
| `checked` | Whether readback could check this call |
| `verdict` | `ok`, or the name of what went wrong, such as `wrote_nothing`, `zero_not_proven` or `ambiguous_world` |
| `requested`, `stored` | What the call asked for, and what readback found stored |
| `how` | Where readback read the stored value |
| `note` | Why the verdict, and how to check or what works instead |

A verdict that is not `ok` means the call did not do what it reported. Most findings also turn `success`
to `false` and put the reason in `error`; the others only inform, and leave `success` as the tool
answered it.

Here `set_blueprint_variable_default` answered `success: true`, but the default stayed empty:

```json
{
  "success": false,
  "data": {
    "blueprint_name": "BP_X",
    "variable_name": "ConfigRow",
    "default_value": null,
    "roundtrip_verified": true
  },
  "readback": {
    "checked": true,
    "verdict": "wrote_nothing",
    "requested": "/Game/X.X_C",
    "stored": null,
    "how": "in_response:default_value",
    "note": "roundtrip_verified is not the oracle: a None re-exports just as cleanly, so it confirms the failure instead of catching it. A path that does write: set the value on the CDO from Python (get_default_object + set_editor_property), without recompiling afterwards."
  },
  "error": "set_blueprint_variable_default: the call reported success but the state did not change (requested '/Game/X.X_C', stored None)."
}
```

## Refusals

A refusal comes before the call runs, so nothing changed. It arrives as a tool error (`isError: true` in
the MCP result) with `success: false`, the message in `error` and the details in `result`. Three checks
refuse a call:

- **Preconditions** refuse a call known to crash or freeze the editor in that situation, including the
  Python that `execute_python` is about to run. The message starts with `REFUSED:` and names the way that
  works; `result.refused_by` is `precondition` and `result.check` names the check.
- **The PIE guards** refuse, while Play-in-Editor runs, the calls that compile a Blueprint or write to
  disk: a compile can keep the play world alive, which the engine treats as fatal, and the engine refuses
  to save during PIE. `result.pie_active` is `true`, `result.reason` says why the tool is refused and
  `result.evidence` quotes the C++ line that classifies it. Stop PIE and retry; `force: true` runs the
  call anyway, at your own risk. Starting or stopping a PIE session that someone else started needs
  `force: true` too.
- **The tool permissions** refuse a tool the user turned off (Editor Preferences > Plugins > UEAgent >
  Permissions): `result.refused_by` is `policy`, `result.policy` says which setting (`read_only`,
  `code_execution`, `disabled_domain`, `disabled_tool`), with `result.command` and `result.domain`. The message
  tells the agent not to retry and to ask the user. Such a tool is also left out of `tools/list` and `find_tools`,
  and `describe_tool` answers the refusal. It applies however the tool is called: directly, through `call_tool`,
  as an `execute_batch` item, or by an old or retired name.

A Python script cannot be classified before it runs, so the Python tools check it as it runs. While
Play-in-Editor runs, a call the script makes that compiles a Blueprint or saves does not run: the script
gets a `RuntimeError` there, and the tool call fails with `pie_refused`, the calls that did not run, even
when the script catches the error. A script that raises fails the call too, with the traceback in
`command_result`.

A precondition refusal, as the agent reads it:

```json
{
  "success": false,
  "error": "REFUSED: create_material_expression: creating a `SetMaterialAttributes` node CRASHES THE EDITOR (assertion in Array.h, not an ensure): the node is created with an empty AttributeSetTypes array and GetInputName indexes Inputs[] out of range. Retrying does not help and it cannot be fixed afterwards: writing attribute_set_types through reflection does not grow Inputs either. WHAT WORKS: use the Displacement pin of MakeMaterialAttributes. If that node is really needed, place it once by hand in the Material Editor and wire it headless afterwards.",
  "result": {
    "refused": true,
    "refused_by": "precondition",
    "command": "create_material_expression",
    "check": "set_material_attributes",
    "hint": "Nothing was executed: the call never reached the editor. The message names the way that DOES work."
  }
}
```

A policy refusal, in read-only mode:

```json
{
  "success": false,
  "error": "REFUSED: UEAgent is in read-only mode (settings): only tools that read, or that only capture an image, can run, and spawn_actor changes the project or runs code. Nothing ran. The user chose this (Editor Preferences > Plugins > UEAgent > Permissions): do not retry it, through call_tool, execute_batch or Python either; tell the user what you need and ask them to allow it.",
  "result": {
    "refused": true,
    "refused_by": "policy",
    "policy": "read_only",
    "command": "spawn_actor",
    "domain": "Actor",
    "hint": "The user's settings turn this tool off. Do not retry; ask the user to allow it."
  }
}
```

Both examples on this page are recorded in the contract tests (`Resources/ContractTests`), which the C++
has to reproduce exactly: see [Architecture](architecture.md#the-contract-tests).

## In the editor

The **Activity** panel (status bar > MCP > Activity) sorts each call into one of four outcomes, the way
an agent reads its answer:

| Outcome | When |
|---|---|
| OK | `success` is `true`, and readback found nothing wrong |
| Readback warning | `success` is `true`, but the readback verdict is not `ok` |
| Failed | `success` is `false` |
| Refused | A precondition or a PIE guard refused the call before it ran |

`mcp.activity [N]` prints the last N calls to the log in the same form.
