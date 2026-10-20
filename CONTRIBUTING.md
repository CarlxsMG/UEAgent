# Contributing to UEAgent

Thanks for helping. This file explains how the code is laid out, how a tool is added and what a
change has to pass. How a call travels through the editor is in
[docs/architecture.md](docs/architecture.md), and how a release is made in [RELEASING.md](RELEASING.md).

## Ground rules

- Open an issue before a large change, so the approach is agreed first.
- Code, comments and docs are in English. Comments say what the code does and why; its history
  lives in git.
- The C++ is the source of truth: never edit a generated file by hand.

## Layout

| Path | What lives there |
|---|---|
| `Source/UEAgent/Private/Commands/<Area>/UEAgent<Family>Commands.cpp` | One command family per file. Its handlers are functions of the family's namespace, and a `McpCommandFamilies::FRegistrar` at the end of the file lists every command string with the function that runs it. |
| `Source/UEAgent/Private/Commands/Common/McpCommonUtils.*` | The helpers every family shares (`mcp_common`). |
| `Source/UEAgent/Private/Mcp/` | The HTTP server, the tool catalog with its compact meta-tools, and the contract around each call (preconditions and readback). |
| `Tools/` | Python scripts that derive the catalog and the generated tables from the C++, the audits, and `catalog_server.py`, which lists the tools without the editor for MCP directories. |
| `Resources/command_catalog.json` | The tool catalog the server publishes (generated). |
| `Resources/ContractTests/` | The contract's test vectors. |
| `Resources/Skills/` | The agent skills the editor installs into a project (**MCP > Skills**), in the Agent Skills format. Their names start with `ueagent-`, and `Tools/check_skills.py` checks the format and that every tool they name is in the catalog. |
| `docs/tools/` | The tool reference (generated). |
| `docs/`, `mkdocs.yml` | The documentation site. `Tools/mkdocs_hooks.py` adds, when it is built, what GitHub does not show: the README sections and root files the pages include, the client tabs, the links from tool names to the reference, the tool chips, the tool explorer's data and `llms.txt`. |

## Adding a tool

1. Write the handler in its family's `.cpp`:
   `TSharedPtr<FJsonObject> HandleDoThing(const TSharedPtr<FJsonObject>& Params)`. Read required
   params with `mcp_common::ReadRequiredString` / `ReadRequiredNumber` / `ReadRequiredBool`, and
   answer with `mcp_common::MakeOkReply`, `MakeSavedReply`, `MakeOutcomeReply` or `MakeErrorReply`.
2. Annotate it right above the function. This text is what the agent reads in the catalog:

   ```cpp
   // MCP:do_thing - What it does, written for the agent that will call it.
   //   @param thing_path - What it is, and that it is required.
   //   @returns { thing_path: string, done: bool }
   ```

   The description, each `@param` and the `@returns` are one line each, and a command has one
   `@returns`: the generator reads one line, and `--check` fails on text that goes on below. Notes for
   whoever maintains the handler go under the `@returns` line, at its indentation.

   The server appends the `@returns` to the description it publishes, and publishes the tool's MCP
   annotations (read-only, destructive, idempotent, open world), which `Tools/tool_hints.py` derives
   from the code the handler runs and from its verb. When the name misleads that rule, say so on a
   line of the block, `//   @hints read-only` or `//   @hints additive idempotent`
   (`python Tools/tool_hints.py --why do_thing` shows the verdict and its evidence). A `read-only`
   that the code contradicts fails the generator.

3. Add its row to the family's `FRegistrar`: `{ TEXT("do_thing"), &HandleDoThing },`
4. Regenerate, in this order (the tables read the catalog):

   ```
   python Tools/integrate_property_handlers.py
   python Tools/codegen_command_catalog.py --write
   python Tools/codegen_param_table.py --write
   python Tools/codegen_param_aliases.py --write
   python Tools/codegen_pie_safety.py --write
   python Tools/codegen_tool_docs.py --write
   ```

A new family is a new `.cpp` with its namespace, a `// MCP-DOMAIN: <Domain>` marker and its
`FRegistrar`; nothing else changes. A new domain also gets a phrase in `DOMAIN_SUMMARY`
(`Tools/codegen_tool_docs.py`), which the reference uses to say what the domain covers. The server reads the
catalog at editor startup and on `mcp.http.restart`.

## Before a pull request

- The plugin builds with no warning of its own.
- The documentation site builds with no warning: `pip install mkdocs-material==9.7.7`, then `mkdocs build --strict`.
- Every generator passes `--check`; CI runs them on each push, with `Tools/check_doc_links.py`,
  `Tools/check_skills.py` and `ruff check Tools`.
- `python Tools/smoke_test.py --launch <a throwaway .uproject> --engine <UE root>` passes: it starts a
  headless editor and checks the server end to end over HTTP.
- A change to how `McpToolCatalog.cpp` builds a schema is made in `Tools/catalog_server.py` too:
  with the editor open, `python Tools/catalog_server.py --compare` must find no difference.
- The contract self-tests pass in the editor: `mcp.contract.selftest` on each vector file in
  `Resources/ContractTests`, and the automation tests `Automation RunTests UEAgent` (the client
  files, the skills installer and the rest).
- A change of behavior says how it was measured: the call before and after, in a headless editor
  when possible.
- The audits do not get worse: `Tools/audit_outcome_honesty.py` (is each mutating tool's `success`
  measured?) and `Tools/audit_runtime_mode.py` (which tools need a live editor).

## License of contributions

UEAgent is released under the [MIT License](LICENSE). By submitting a contribution you agree that it
is released under the same license, and you confirm that you wrote it or have the right to submit it
under that license.
