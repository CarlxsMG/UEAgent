# Glossary

The words these docs, the tools' descriptions and their answers use.

| Term | What it means |
|---|---|
| **Activity** | The last 200 tool calls, newest first, each with its time, duration, outcome and detail: **Activity** in the MCP menu of the editor's status bar, or `mcp.activity` in the console. See [In the editor](responses.md#in-the-editor) |
| **Additive** | A tool that adds to the project without changing or removing what is there: its MCP annotations say neither read-only nor destructive |
| **Aliased parameter** | An argument spelled the way a sibling tool spells it. The server renames it before the call and lists it in `data.aliased_params` |
| **Annotations** | The hints every tool publishes over MCP: read-only, destructive or additive, idempotent, open world. `Tools/tool_hints.py` derives them from the code the tool runs and from its verb |
| **Catalog** | `Resources/command_catalog.json`, the list of tools the server serves, generated from the C++. The [tool reference](tools/README.md) and the [tool explorer](explorer.md) are the same catalog |
| **Compact tool list** | Three tools, `find_tools`, `describe_tool` and `call_tool`, that search, describe and run every tool, for a client with a tight cap on how many tools it lists (`?tools=compact`). See [Clients](clients.md#tool-lists) |
| **Core tool list** | What a client lists by default: the core tools, plus `find_tools`, `describe_tool`, `call_tool` and `enable_toolset`, which search, describe and run the rest or add a pack or a whole domain to the list. See [Clients](clients.md#tool-lists) |
| **Contract tests** | The recorded refusals, readbacks and registry probes in `Resources/ContractTests`, which the C++ has to reproduce exactly. See [Architecture](architecture.md#the-contract-tests) |
| **Destructive** | A tool that can change or remove what is already in the project |
| **Domain** | A group of tools about one part of the engine, such as Animation or Material: one page each in the [tool reference](tools/README.md) |
| **`execute_batch`** | Several calls in one pass on the game thread, with one answer per call. Like any call, the whole batch is one undo step |
| **`execute_python`** | Runs Python in the editor, with Unreal's Python API, for what no tool does. Its code is scanned before it runs, and while Play-in-Editor runs its calls that compile a Blueprint or save do not run |
| **`force`** | A parameter that runs a refused call anyway, at your own risk: the tools that PIE refuses have it, and so do some preconditions |
| **Idempotent** | A repeat call with the same arguments changes nothing more |
| **Open world** | A tool whose code talks to the network or runs arbitrary code |
| **Pack** | A few tools for one job the core tool list leaves out, such as `actors` or `play`: `enable_toolset` adds one to a client's list by name. See [Clients](clients.md#tool-lists) |
| **PIE** | Play-in-Editor. While it runs, the calls that compile a Blueprint or write to disk are refused unless they pass `force: true` |
| **Prompt** | A skill served over MCP: the client offers it to you, usually as a slash command, and it sends the skill to the agent with your request. See [Skills](skills.md#without-installing-them-prompts-and-resources) |
| **Precondition** | A check before a call runs. It refuses a call known to crash or freeze the editor in that situation, and its message says what works instead |
| **Read-only** | A tool that reads the editor and changes nothing |
| **Read Only Content Root** | A content folder, set in **Project Settings > Plugins > UEAgent**, under which no tool can save, rename, move or delete. Meant for vendored libraries. See [Settings](settings.md) |
| **Readback** | The re-read of the state after a call, in the same pass. A write that did not stick, a zero that proves nothing or a change in the wrong world is reported in `readback` instead of a plain `success: true`. See [Readback](responses.md#readback) |
| **Refusal** | The answer to a call that never ran, because a precondition or a PIE guard stopped it: a tool error with the reason in `error`. See [Refusals](responses.md#refusals) |
| **Unattended mode** | While a call runs, the editor's modal dialogs are suppressed and answered, so no call waits for a click |
| **Tool map** | The `ueagent-tool-map` skill, also the resource `ueagent://guide`: which core tool, pack or domain covers each kind of task, and the order a task calls them in. Its tables are generated from the catalog |
| **Undo step** | Every call is one undo transaction named after its tool ("UEAgent: spawn_actor"), so Ctrl+Z reverts the whole call. A call that changes nothing leaves no entry |
| **Unknown parameter** | An argument the tool does not read. The call runs without it, and `data.unknown_params` lists it |
| **Verdict** | `readback.verdict`: `ok`, or the name of what went wrong, such as `wrote_nothing` |
