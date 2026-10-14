# Contract vectors

Golden expectations for the two contracts the plugin applies around every command (code under
`Source/UEAgent/`):

- **Preconditions** (`Mcp/McpPreconditions.*`) run BEFORE a command and refuse a call that is
  known to crash or freeze the editor, naming the safe alternative in the message.
- **Readback** (`Mcp/McpReadback.*` + `Mcp/Readback/*`) runs AFTER a command and attaches a
  finding when the answer is misleading: a write that did not stick, a zero that proves nothing,
  a change that landed in the wrong world.

The vectors are the specification of that contract. A deliberate contract change is made in the
C++ and in its vectors in the same commit; the self-test is the proof that nothing else changed.

- `refuse_vectors.json`: `cases` for the precondition check. Each `check` step gives a command,
  its params and the recorded probe replies, and expects `refused`, the exact `message` (or only
  a `message_prefix`) and the probe calls made. `note` steps record an earlier result first, for
  checks that depend on what the session already did.
- `readback_vectors.json`: `records` of `readback`, `take_before`, `annotate_offload` and
  `note_result` calls, each with its probe replies in order and the session state before and
  after. The whole output is compared, and so are the probe calls and the final state.
- `registry_vectors.json`: REAL read-only probes, kept generic so they hold in any project: the
  Asset Registry lookup behind `get_asset_metadata` (`registry_probe`), and the asset-name guard the
  create commands run before creating a package (`name_conflict`: an object of another class at
  that path, which would make NewObject abort the editor; an empty `expect_message` means free).

The probes of the first two files are FAKE: each probe gets its recorded reply, so nothing reaches
a command handler and no editor state is touched.

A vector file may carry an optional top-level `"settings"` block with the project settings it was
written against. Today that is `read_only_content_roots` (e.g. `["/Game/VendorPack"]`): for the
duration of the run those roots replace the project's Read Only Content Roots, and the project's
own value is restored afterwards.

## Running

In the editor console (or through the `execute_console_command` tool):

    mcp.contract.selftest <path>/refuse_vectors.json
    mcp.contract.selftest <path>/registry_vectors.json
    mcp.contract.selftest <path>/readback_vectors.json

Headless, in an editor that exits on its own with code 0 when every check passes and 1 otherwise:

    UnrealEditor-Cmd.exe <project>.uproject -ExecCmds="mcp.contract.selftest <path>/readback_vectors.json quit" -unattended -nopause -nosplash -NullRHI -NoSound

The trailing `quit` is an argument of the self-test, not a separate console command: it forces the
exit, which is what carries the status code. Commas separate commands in `-ExecCmds`, so there is
no comma before `quit`.

Each run writes its report next to the vector file: `<file>.result.json` (ignored by git), with
`checks`, `passed` and `failures`. Readback runs also list `skipped` (records that do not apply to
the C++ implementation) and `readback_tables` (the commands in each readback table).

## Changing the prose

Outputs are compared exactly, so rewording a refusal message (`McpPreconditions.cpp`) or a
readback note (`McpReadbackText.h`) on purpose makes the vectors fail. To re-baseline only the
prose:

1. Run the self-test on `refuse_vectors.json` and `readback_vectors.json` (it writes the reports).
2. `python Tools/rebaseline_contract_prose.py` sorts the failures into prose-only and contract.
3. `python Tools/rebaseline_contract_prose.py --apply` rewrites the prose of the prose-only ones.
4. Run the self-test again: it must pass with no failures.

Only prose leaves (`message`, `note`, `detail`, `hint`, `error`, `reason`, `warning`,
`summary_text`) are ever rewritten. Any other difference (a verdict, flag, count, key, probe call
or session state) stays a failure: that is a contract change, and it is made by hand in the C++
and in the vectors.
