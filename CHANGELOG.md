# Changelog

Notable changes to UEAgent, newest first. Versions follow [Semantic Versioning](https://semver.org).

## Unreleased

- A Claude Code plugin, `ueagent`, installed from this repository
  (`/plugin install ueagent --marketplace CarlxsMG/UEAgent`): `skill-builder` turns an editor
  workflow into a new skill, and `blueprint-actor`, `project-audit` and `level-lighting` are skills
  made with it.
- `Tools/catalog_server.py` serves the tool list over stdio without the editor, the same list the
  editor serves, so that MCP directories such as Glama can list the tools.
- The 408 tool definitions that said the least were rewritten from their code: every description now
  says what the tool reads or changes, what it returns and its side effects, and every parameter says
  what it takes. About 60 of them described something the code does not do, and now match it. No
  tool, parameter, type or accepted value changed.
- Fixed, from reading the code behind those definitions:
  - `reimport_anim_sequence` passed `true` as the source file index, so it reimported source 1 and
    could open dialogs; it now reimports the first source, automated.
  - `validate_anim_state_machine` and `get_anim_state_machine_info` counted transitions as states and
    `transition_count` was always 0, so every machine of two or more nodes got the "no transitions"
    warning. States, conduits and transitions are now counted apart.
  - `list_usd_prims` ignored `recursive`. With `recursive: true` it now lists the whole subtree; its
    default is now false, which lists the direct children as before.
  - `create_attribute_set_asset` and `add_attribute_to_set` saved without compiling and wrote each
    default as a bare number the attribute struct cannot read, so every default stayed 0, and they
    counted a name the class already had as added. They now compile, the compiled default is
    `default_value`, an existing name is reported apart, and the result is read back from the
    compiled class.
  - `set_tree_parameter` failed on text parameters; it sets them now.
  - `remove_tree_binding` always failed, saying UE 5.8 has no API to remove a binding; it removes it.
  - `configure_ai_sight_radius` ignored `lose_sight_radius`.
  - `list_engine_ini_sections` asked GConfig for `Engine.ini`, which it does not resolve, so it found
    no sections; it reads the INI as `get_engine_ini_value` does and says whether it was `found`.
  - `create_modulation_parameter` also accepts the engine spellings `LPFFrequency` and
    `HPFFrequency`, which its error message listed but the code refused.
- Schemas that now say what the code requires: `system_path` on 69 Niagara tools, `state_name` on 28
  StateTree tools, `value` on six StateTree tools and `set_meta_sound_node_property` (any JSON),
  `warmup_time`, `local_space`, `screen_size`, and `emitter_name` and `renderer_index` of
  `set_niagara_decal_renderer_material` are required; `transition_index` of `get_condition_info`,
  `default_value` of `set_chooser_column_default` and `variety_index` of
  `set_landscape_grass_type_properties` are optional; and the parameters read through helpers have
  their real types (`vector` and `color` arrays, scale and offset ranges, material function input
  props, post process volume numbers). `set_mesh_uvs_from_projection` lists `polygroup_layer`.

## 1.1.0 - 2026-10-14

- Documentation: architecture, FAQ, examples, and a Privacy section in the README, all on a website
  with search (carlxsmg.github.io/UEAgent). Questions go to GitHub Discussions.
- A logo, and the plugin's icon in the editor's Plugins window.
- Every tool call is one undo step, named after the tool: Ctrl+Z reverts a call, and a `batch` as a
  whole. `mcp.UndoStepPerCall 0` turns it off.
- An **Activity** panel (status bar > MCP > Activity) lists the last 200 tool calls with their duration
  and outcome; `mcp.activity` prints them to the log.
- `Tools/smoke_test.py` checks a running server end to end over HTTP, and with `--launch` starts a
  headless editor to run the checks that edit the level too.
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
