# Changelog

Notable changes to UEAgent, newest first. Versions follow [Semantic Versioning](https://semver.org).

## Unreleased

- Documentation: architecture, FAQ, examples, and a Privacy section in the README, all on a website
  with search (carlxsmg.github.io/UEAgent). Questions go to GitHub Discussions.
- A logo, and the plugin's icon in the editor's Plugins window.
- Every tool call is one undo step, named after the tool: Ctrl+Z reverts a call, and a `batch` as a
  whole. `mcp.UndoStepPerCall 0` turns it off.
- `initialize` gives every agent short instructions (readback verdicts, refusals, `batch`), in the
  full tool list too.
- Fixed: the undo tools read the editor's undo buffer backwards. `get_undo_state` swapped the undo
  and redo counts, `get_transaction_history` reported every entry as undone, and the labels of
  `can_undo`, `can_redo` and `get_undo_state` came back empty.
- `Tools/package_release.py` builds and self-tests the release archives, and RELEASING.md describes
  a release. The CI also checks the documentation's links and lints `Tools/`.

## 1.0.0 - 2026-10-14

First public release.

- An MCP server inside the Unreal Editor (Streamable HTTP on 127.0.0.1:55561), with more than 2,000
  tools in 55 domains: see the [tool reference](docs/tools/README.md).
- A contract around every call: preconditions refuse the calls known to crash or freeze the editor,
  and readback re-reads the state after each call.
- Several agents can share one editor.
- One-click connection for Claude Code from the editor's status bar.
- A compact tool list for clients that cap how many tools a server may list: `find_tools`,
  `describe_tool` and `call_tool` reach the whole catalog (`?tools=compact` in the URL, or
  `UEAGENT_TOOLS=compact`).
- Read Only Content Roots: content no tool may save, rename, move or delete.
- Released under the MIT License.
