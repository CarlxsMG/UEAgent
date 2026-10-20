---
name: ueagent-project-audit
description: Audit an Unreal project, or one content folder, with UEAgent's MCP tools - Blueprints that do not compile, graph bugs that compile clean, unused assets, broken references, leftover redirectors and Map Check messages - and report what to fix, most serious first. Use when the user asks to check the health of the project, find what is broken, or clean it up.
---

# Project audit

Reads the project and reports what is broken and what can go, most serious first. It changes
nothing: fixes are proposed, and run only once the user agrees. It audits the folder the user named,
or `/Game` if they named none.

UEAgent's tools are the MCP tools of the Unreal Editor's server (named `unreal` when it was connected
from the editor's **MCP** menu). Your client may show them with a prefix, such as
`mcp__unreal__run_map_check`. With the compact tool list, run them through `call_tool`.

## Steps

1. **Blueprints in error**: `find_erroring_blueprints` with `path` and `include_warnings: true`. Keep
   `compile_all` off unless the user asks for a full recompile: it is heavy.
2. **Bugs that compile clean**: `lint_blueprint_graph` with `blueprint_name`, for the Blueprints the
   user cares about most or the ones changed recently.
3. **Broken and orphaned references**: `audit_asset_references` with `folder_path`. It reads the Asset
   Registry only, so it is cheap even on a large project.
4. **Unused assets**: `find_unused_assets` with `folder_path`. Each candidate comes with the reasons
   to keep it anyway: report those, and never call an asset "safe to delete" when it has one.
5. **The current level**: `run_map_check`.

## Report

Group the findings, in this order:

1. **Broken**: Blueprints in error, broken references, Map Check errors.
2. **Risky**: lint findings, compile warnings, Map Check warnings.
3. **Cleanup**: unused assets with no reason to keep them, and redirectors.

Give each finding its asset path, what is wrong and the fix.

## Fixes, only when the user agrees

- Redirectors: `fixup_redirectors` with `path`. It re-saves the packages that reference them, so
  confirm first; `delete_redirectors: true` removes the redirectors too.
- Never delete assets from this skill: list them, and let the user decide.

## Pitfalls

- `find_erroring_blueprints` reads `path`: another parameter name is ignored, and the scan covers the
  whole of `/Game`.
- `run_map_check` checks the level open in the editor, not every level in the project.
