# Changelog

Notable changes to UEAgent, newest first. Versions follow [Semantic Versioning](https://semver.org).

## 2.5.0 - 2026-10-22

A client now lists 35 core tools instead of 1,622, and reaches the rest when it needs them. The six graph tools edit
every kind of graph asset, the property tools reach an asset's sub-objects, 75 tools that overlapped them leave the
list, and 10 tools get the verb their operation calls for. Every old name keeps working until 3.0.

### Changed

- **The core tool list is the default.** `tools/list` carries 35 core tools and 4 meta-tools: the core covers assets,
  actors and levels, any object's properties, Blueprint and other asset graphs, data tables, Play-in-Editor and Python.
  The meta-tools reach everything else: `find_tools` searches every tool, `describe_tool` gives one tool's schema,
  `call_tool` runs any tool, and the new `enable_toolset` adds whole domains to the client's list for its session.
  - The server announces the change with `notifications/tools/list_changed`, so a client that supports it shows the
    new tools straight away.
  - `initialize` on the core list opens a session (`Mcp-Session-Id`), which remembers the domains a client enabled.
  - `?domains=Material,Animation` at the end of the URL lists those domains from the start.
  - `?tools=all` gives a client every tool again, and `?tools=compact` only the first three meta-tools.
  - `UEAGENT_TOOLS=all` or `UEAGENT_TOOLS=compact` changes the default for every client.
  - **Connect** writes the server's own URL for every client, so the core list. Claude Code loads tools on demand:
    add `?tools=all` to its URL to give it every tool directly.
- **One graph API for every graph asset.** `get_graph_nodes`, `add_graph_node`, `remove_graph_node`, `connect_pins`,
  `disconnect_pin` and `set_pin_default_value` take `asset_path` (`blueprint_name` is still accepted). Besides
  Blueprints they now edit materials, material functions, sound cues, MetaSound Sources, PCG graphs and Control Rigs,
  through each editor's own tool, so its saves, checks and replies apply.
  - In an Animation Blueprint, `connect_pins` uses the graph's own schema, so pose links work, and `add_graph_node`
    takes any graph node class (`AnimGraphNode_Slot`).
  - The node ids `@input` and `@output` name a graph's own input and output: a material's main inputs, a sound cue's
    first node, a MetaSound's graph inputs and outputs, a PCG graph's input and output nodes.
  - `disconnect_pin` can break one link (`linked_node_id`, `linked_pin`) instead of every link of the pin.
  - `get_graph_nodes` gives every node its `object_path`, which the property tools take.
- **Sub-objects in the property tools.** `get_properties`, `set_properties` and `get_object_info` take the
  `object_path` the readers now give for each sub-object: graph nodes, material expressions, sound cue nodes, PCG node
  settings (`settings_path`), behavior tree nodes, Niagara renderers and simulation stages, camera rig nodes, Mass
  traits and physics bodies. `"/Game/Asset:SubObject"` works too. After a write, `set_properties` finishes the owning
  asset as its editor does: a graph node is reconstructed and its Blueprint marked modified, a sound cue recaches its
  values, a Niagara system recompiles.
- **75 tools leave the list.** They overlapped the graph and property tools above, and still run under their own names
  until 3.0. The [retired names](docs/tools/retired.md) page says which listed tool does the same, with the call.
  - Graph tools (44): `add_anim_graph_node`, `add_metasound_node`, `add_pcg_node`, `add_pcg_subgraph_node`,
    `add_rigvm_link`, `add_rigvm_node`, `add_sound_cue_node`, `connect_anim_graph_pins`,
    `connect_material_expressions`, `connect_material_function_pins`, `connect_metasound_graph_input_to_node`,
    `connect_metasound_graph_input_to_output`, `connect_metasound_node_to_graph_output`, `connect_metasound_nodes`,
    `connect_pcg_nodes`, `connect_sound_cue_nodes`, `connect_substrate_slab_to_material`,
    `connect_to_material_output`, `create_material_expression`, `create_substrate_slab_node`,
    `delete_material_expression`, `disconnect_anim_graph_pins`, `disconnect_material_expressions`,
    `disconnect_metasound_graph_input_from_output`, `disconnect_metasound_node_from_graph_output`,
    `disconnect_metasound_nodes`, `disconnect_pcg_nodes`, `disconnect_sound_cue_nodes`,
    `disconnect_to_material_output`, `get_material_graph_info`, `get_metasound_node_info`, `get_pcg_graph_info`,
    `get_rigvm_graph`, `get_sound_cue_graph`, `list_metasound_connections`, `list_metasound_nodes`,
    `remove_metasound_node`, `remove_pcg_node`, `remove_rigvm_link`, `remove_rigvm_node`, `remove_sound_cue_node`,
    `set_metasound_node_input_default`, `set_rigvm_pin_default`, `set_sound_cue_first_node`.
  - Property tools (31): `get_anim_graph_node_properties`, `get_behavior_tree_node_properties`,
    `get_cine_camera_settings`, `get_component_properties`, `get_input_settings`, `get_interchange_project_settings`,
    `get_landscape_grass_type_properties`, `get_light_properties`, `get_mass_trait_property`,
    `get_pcg_node_properties`, `get_post_process_settings`, `get_property_access_path`, `get_recast_navmesh_settings`,
    `get_reflection_capture_properties`, `get_sound_class_properties`, `set_anim_additive_settings`,
    `set_anim_graph_node_property`, `set_audio_3d_settings`, `set_camera_node_parameters`,
    `set_geometry_collection_connection_settings`, `set_hlod_layer_settings`, `set_mass_trait_property`,
    `set_material_expression_property`, `set_modulation_generator_settings`, `set_modulation_parameter_settings`,
    `set_niagara_renderer_properties`, `set_pcg_node_properties`,
    `set_recast_navmesh_settings`, `set_sound_cue_node_properties`, `set_submix_effect_settings`,
    `set_texture_settings`.
- **10 tools renamed to their verb.** `add_` puts a part in and `remove_` takes it out; `delete_` is for what
  `create_` makes. The old names run the new tools.
  - `delete_node` → `remove_graph_node`, `delete_blueprint_variable` → `remove_blueprint_variable`,
    `delete_curve_table_row` → `remove_curve_table_row`, `delete_data_table_row` → `remove_data_table_row`,
    `delete_material_function_input` → `remove_material_function_input`,
    `delete_material_function_output` → `remove_material_function_output`,
    `delete_mesh_triangles` → `remove_mesh_triangles`.
  - `compare_objects` → `diff_objects`, `compare_asset_versions` → `diff_asset_versions`,
    `does_foliage_type_exist` → `is_foliage_type_loadable`.
  - The catalog generator refuses a `delete_` tool next to an `add_` tool for the same thing, and `compare_` as a
    verb. The agent instructions say when to use `diff_`, `execute_` and `run_`.
- `set_metasound_graph_input_default` takes its value as text too, read as the input's own type.
- `connect_to_material_output` marks the material dirty.

### Fixed

- **A tool that hands its call to another reported a refusal as success.** When the PIE guard or a precondition refused
  the inner call, the outer tool answered success. It now fails, with the refusal's detail under `data`.
- **The graph tools edited a Control Rig as a plain Blueprint**, and could add an EventGraph page to it. They now edit
  its rig graph.
- `connect_material_function_pins` ignored `from_pin` and always wired the source's first output.
- `connect_to_material_output` wired `FrontMaterial` while Substrate was disabled; it now refuses.
- `set_rigvm_pin_default` compiled and saved after a value that did not land, and answered success. That is now an
  error, and nothing is compiled or saved.
- `set_metasound_node_input_default` reported `value_set: true` without comparing the value read back.
- `connect_pcg_nodes` answered success after a failed save.
- `remove_metasound_node` removed a graph input or output node and left the input or output declared with no node. It
  now refuses and names the tool that removes the input or output.
- `get_rigvm_graph` now lists each pin's default value and links; it only counted the links before.
- `get_material_graph_info` now lists each expression's output names, which `connect_pins` takes as `source_pin`.

## 2.4.3 - 2026-10-21

The 134 tool definitions Glama scored below 4 now say when to use the tool, what each parameter takes and what it
does. Rewriting them against their code found 58 defects, all fixed here: one of them killed the editor.

### Changed

- **134 tool definitions explain their use**: when to use the tool and what to use instead, what each parameter
  takes, and what is undone, saved or refused. Nine annotations move to match the code.
- **`run_functional_test_in_pie` reports what it can see.** It never ran the test: a Simulate session has no player
  controller, so the functional test manager cannot start a test there. It reads the test actor's `Result` in a
  session that is already running (pass, fail, running, not_run, unknown) instead of always saying "pass". It stops
  only a session it started, never one you had running. A test name must match one class exactly; a name that
  matches several is refused with the candidates.
- **These tools no longer delete whatever asset is at the path.** They refuse an existing asset and name its class:
  `create_texture_2d_from_data`, `create_texture_2d_array`, `create_render_target_2d_array`,
  `create_render_target_cube`, `create_static_texture_from_render_target`.
- **The streaming level tools match a path exactly.** `add_streaming_level`, `remove_streaming_level`,
  `set_streaming_level_loaded`, `set_streaming_level_visible` and `spawn_level_streaming_volume` used to fall back to
  the leaf name, and could act on a different level of the same name. A bare name that several levels share is
  refused.
- `set_cvars_from_json` reads each value back, so `applied` counts only what the cvar holds, with a `results` list.
  A JSON `true`/`false` sets an integer cvar to 1/0 (it used to read as 0), and any error makes the call fail.
- `create_render_job` and `render_sequence_to_movie` use the open editor level when no `map_path` is given, as their
  descriptions said, and refuse a map the renderer would reject (not a package path, or an unsaved level).
  `render_sequence_to_movie` reports `render_state`: it waits for PIE, renders, or ends without rendering (a
  failure).
- `list_metasound_node_types` takes `name_filter` and `max_results`. The full list is too long to come back inline.
- `set_chooser_row_outcome` writes the row's result, the value `evaluate_chooser_table` returns; it used to write
  only the first output column's cell.
- `generate_blueprint_docs` lists the components of the Components panel, and labels every Blueprint type and
  compile state. `create_asset_documentation` matches subclasses: a SoundCue is audio, a material instance is a
  material.
- `list_engine_ini_sections` honours `include_base`: false lists only the project's own sections.
- `play_widget_animation` refuses an unknown `play_mode`, a negative `num_loops` and a bad world selector; 0 loops
  until stopped, as in the engine.
- `is_point_inside_mesh` and `remove_section` refuse a malformed `query_point` or an unknown `track_type`, which
  they used to read as `[0,0,0]` or ignore.
- `validate_asset` counts the validators that ran (`validators_run`), not those enabled.
- `get_material_parameter_collection_instance_info` counts real overrides.
- `list_task_types` and its condition and evaluator siblings give each struct's real path.

### Fixed

- **`subscribe_to_delegate` killed the editor on a component's sparse delegate**, such as `OnComponentHit`. Sparse
  delegates are refused now by property type, and the name check ignores case.
- **These reported success, or a result, they had not measured:**
  - `refresh_data_registry_map` and `deactivate_external_subsystem` / `activate_external_subsystem` said `true`;
    they now compare before and after.
  - `remove_motion_warping_component`, `set_behavior_class` and `set_physics_constraint_limits` hard-coded their
    result; they read it back. `set_physics_constraint_limits` refuses a call with no limit.
  - `set_scalability_quality` said `set` without comparing; a mismatch fails.
  - `set_ability_cost` and `set_ability_cooldown` answered success when the read-back did not match.
  - `reimport_interchange_asset` answered success when nothing was reimported.
  - `remove_streaming_level` answered success with `removed` false.
  - `set_audio_volume` answered success with `volume_set` false.
  - `create_gameplay_effect_asset` reported the requested duration policy for an existing asset, not the asset's.
  - `get_usd_stage_actor_info` counted the actor's own root as a generated component.
  - `start_trace` said timing regions go into the trace; only bookmarks do.
- **These answered success when the save failed:** `remove_state_tree_parameter`, `add_anim_composite_segment`,
  `set_substrate_slab_scalar`. `rename_skeletal_mesh_socket` and `remove_skeletal_mesh_socket` now save the skeleton
  when the socket lives there.
- **Undo now restores** `add_state_task`, `add_consideration`, `add_state_enter_condition`, `set_substrate_slab_scalar`
  (and a bad pin leaves no node behind) and `attach_blueprint_component`, which used to drop its step so that undo
  removed the component added before.
- `set_project_settings_value` and `set_default_game_map` restore the value in memory when it cannot be saved to the
  config file.
- `add_anim_composite_segment` appends when no `start_pos` is given; it used to land at the start.
- `reimport_anim_sequence` checks every recorded source file before reimporting.
- `create_functional_test` refuses a Blueprint at the path that is not a Functional Test, and reports its real parent.
- `insert_usd_sublayer` answers success for a sublayer already there and for an in-memory stage.
- `remove_soundfield_effect` no longer needs a concrete effect class to clear a slot.
- `subscribe_to_delegate` finds an existing subscription by the resolved object, whatever the spelling.
- `disconnect_sound_cue_nodes` also breaks the wire in the cue editor's graph.
- `set_behavior_class` and `create_behavior_config` find a class by its bare name.
- `list_metasound_node_types` reads `is_native` from the MetaSound asset manager and gives each class's asset.

## 2.4.2 - 2026-10-21

The review behind 2.4.1 left eight known defects. This release fixes them, and your agent can now install UEAgent
for you.

### Added

- **Install with your agent.** Paste one line into Claude Code, Cursor, Codex or another agent opened in your
  project, and it installs UEAgent following [llms-install.md](llms-install.md). It finds the engine version,
  downloads and verifies the release, unzips it, opens the editor, connects itself and installs its skills; it asks
  before changing anything and leaves a git clone alone. See the README's Quick start and
  [Install with your agent](docs/agent-install.md).

### Changed

- **The audio component tools see every audio component**, not only each actor's first one. This covers
  `set_audio_volume`, `set_audio_3d_settings`, `play_sound`, `stop_sound`, `set_audio_sound` and the census in
  `get_audio_settings`.
  - A new optional `actor_name` picks the owner when several actors have a component of that name; that ambiguity is
    refused, with the owners listed.
  - The replies name the component they used (`audio_component_path`, `audio_owner_name`).
- **`get_audio_settings` reads every value from the engine.** The device values (sample rate, buffer size, channels,
  mixer) come from the main audio device, or are `null` with a note when there is none. `audio_api` is the module
  that created the device: it always said "XAudio2", and 5.8's Windows default is WASAPI. The fixed values it
  reported as readings (frame rate, bit depth, reverb, occlusion, Doppler…) are gone.
- **The tools that act on a running behavior tree or blackboard look in the PIE world first**, where an AI
  controller runs its tree, and say which world answered (`world`, `world_name`). This covers `run_behavior_tree`,
  `stop_behavior_tree`, `pause_behavior_tree`, `resume_behavior_tree`, `restart_behavior_tree`,
  `set_blackboard_value`, `get_blackboard_value` and `get_blackboard_data` given an actor. Before, only
  `get_behavior_tree_status` did.
- `set_morph_target_deltas` reports the deltas of every LOD it touched (`total_deltas`, `deltas_per_lod`), not only
  LOD 0.

### Fixed

- **`play_sound` and `play_sound_at_location` answered success when nothing played**, for example in an editor
  started with `-NoSound`. They now refuse with the reason. With a device, success means a new active sound was
  measured.
- **`add_blend_space_sample` saved a sample the engine rejected** (another skeleton, a position already taken), with
  the axis already widened. It now refuses before changing anything, and reads the sample back. It also refuses a
  position so far outside the axis that the engine's widening loop would never end.
- **`set_morph_target_deltas` made no undo step.** The engine creates morph targets without the transactional flag;
  the tool now sets it, so undo restores the old deltas. The step records the morph target only. Undoing a step that
  holds the mesh rebuilds the mesh, and the rebuild drops every morph target `create_skeletal_mesh_morph_target`
  added.
- **`remove_niagara_module_from_stack`, `duplicate_niagara_module_in_stack` and `set_niagara_module_enabled` read any
  `script_group` other than "spawn" as "update".** An unknown value is now refused, as `reorder_niagara_modules`
  already did.
- **`save_usd_stage` failed when no layer needed saving.** It now answers success with `layers_saved: 0`. A dirty
  in-memory layer, which has no file to save to, is a failure.
- `add_modulation_input_to_patch` is marked idempotent, as it is: a bus already among the inputs is not added again.
  Its definition and `set_modulation_parameter_settings`'s say when to use them.
- On the documentation site, a pipe inside code in a table showed with a backslash, as in
  `mcp.clients.connect cursor\|vscode\|antigravity` in the console commands. It reads as on GitHub now.

## 2.4.1 - 2026-10-21

The 146 tool definitions Glama scored lowest now say when to use the tool and what it does. Each one was rewritten
against its code, and that review found about 40 tools that reported success for work they had not done, or did
something other than what they said. Those tools are fixed.

### Changed

- **146 tool definitions explain their use.** Each one says:
  - when to use it, and which sibling to use instead (its inverse, the reader that checks it, its prerequisite);
  - what each parameter takes, and where a valid value comes from;
  - what is undone, saved or refused, and what it answers when there is nothing to do.
- `get_uv_health_report` replaces `approx_overlap_count` with `overlapping_triangle_count`, a real UV-space overlap
  test. The old field counted every shared corner. A new `overlap_test_complete` field says whether the test ran in
  full.
- `set_render_job_output` refuses an unknown format, and a resolution with only one side, instead of quietly using
  png or ignoring the resolution. `image_outputs` lists every image format the job will write.
- `list_usd_stage_layers` and `set_usd_edit_target` number the layers the same way. The default edit target is the
  root layer; index 0 used to be the session layer. The list marks the root, session and edit-target layers.
  `save_usd_stage` saves every dirty layer of the stack, not only the root, and reports each one.
- `register_input_key_mapping` and `unregister_input_key_mapping` act on the PIE player's controller and honour
  `player_controller_name`. Without PIE they refuse and say why.
- `remove_state` with `with_descendants` false moves the state's children to its parent; the parameter used to be
  ignored.
- `get_niagara_emitter_data_interfaces` lists the data interfaces the emitter's stacks use, as its name says. The
  emitter dependencies it used to list are a second array.
- `get_component_events` lists the class's real multicast delegates, inherited ones included; it used to return a
  fixed list.
- `list_niagara_modules`, `reorder_niagara_modules`, `remove_niagara_module_from_stack`,
  `duplicate_niagara_module_in_stack` and `set_niagara_module_enabled` work on the chosen stack, with stack
  positions, and read their result back.
  - `reorder_niagara_modules` changes the execution order now; before, it only shuffled the graph's nodes.
  - `remove_niagara_module_from_stack` removes the module's whole group and closes the gap.
- `get_behavior_tree_status` looks in the PIE world first, and says which world answered.
- `get_consideration_info` includes the consideration's instance data.

### Fixed

- **These tools reported success when the save failed:**
  - the StateTree removals: `remove_state`, `remove_task`, `remove_evaluator`, `remove_consideration`,
    `remove_state_transition`, `remove_state_tree_global_task`, `remove_state_enter_condition`,
    `remove_state_transition_condition`;
  - `disconnect_pcg_nodes`, `set_material_usage_flag`, `add_pose_search_channel_to_schema`;
  - `set_submix_effect_settings`, which now also saves a preset that is its own asset;
  - `set_skeletal_mesh_socket_transform`, which now saves the skeleton when the socket lives there.
- **Undo restores these changes**, because `Modify()` now runs before the change instead of after:
  - `set_modulation_patch_bypass`, `add_modulation_input_to_patch`, `remove_modulation_input_from_patch`;
  - `set_modulation_patch_output_parameter`, `set_modulation_parameter_settings`,
    `set_modulation_generator_settings`;
  - `set_static_mesh_socket_transform`.
- **These tools reported a result they had not measured:**
  - `apply_gameplay_effect` said an Instant effect was not applied; `applied` is now the engine's verdict, and a
    new `active` field says whether an effect stays active.
  - `send_gameplay_event_to_actor` hard-coded `sent` and `asc_resolved` and ignored labels. It now finds the actor
    by name or by a label only one actor has, and reports `sent` as measured.
  - `remove_bone` said it had removed a root or virtual bone the engine refused to remove.
  - `create_camera_rig_asset` and `create_camera_asset` said `saved: true` for an asset that already existed.
  - `reimport_asset` answered success when nothing was reimported. A missing source file is now refused before the
    reimport, which could crash the editor.
- **Pose Search:**
  - `build_pose_search_database_index` without waiting answered failure for a build it had started.
  - A database with no animations fails with a message that says so; the engine fails an empty index while its log
    says the build succeeded.
  - `create_pose_search_schema` and `add_pose_search_channel_to_schema` say when the schema does not finalize, and
    why. The default Pose channel needs mannequin bones.
  - `add_pose_search_channel_to_schema` no longer adds a second channel of a type the schema already has.
- **Other fixes:**
  - `create_usd_stage_actor` spawned the actor at scale 0.
  - `add_usd_reference`, `add_usd_payload` and `clear_usd_payloads` sent "/" for an empty target path instead of
    the default prim.
  - `add_water_body_to_zone` and `create_water_body`'s `water_zone_label` made a link that did not persist; they
    now go through the water body's zone override, and `remove_water_body_from_zone` clears it.
  - `get_level_sequences` reported as `total_count` the number it returned, not the number it found;
    `returned_count` is the new field.
  - `get_niagara_emitter_runtime_summary` gave the same module count for every stack.
- `run_eqs_query` and `save_game_to_memory` are marked read-only; they change nothing.
- `Tools/codegen_command_catalog.py` writes the catalog in LF on Windows too.

## 2.4.0 - 2026-10-21

UEAgent updates itself: the editor checks GitHub for a new release, downloads and verifies it, and installs it
when the editor closes, keeping the previous version as a backup.

### Added

- **Updates from the editor.** **MCP · 55561 > Check for updates** asks GitHub for the latest release, and
  **Download and install** downloads the zip for this engine, checks it against the release's `SHA256SUMS.txt`,
  unpacks it into `Saved/UEAgentUpdate` and checks that it holds the new version. The editor holds the plugin's
  DLLs, so the update installs when the editor closes, or right away with **Restart now**, which reopens it
  afterwards. A script (`Resources/Update/apply_update.ps1`) waits for the editor to close and swaps the folders.
  The previous version is kept in `Saved/UEAgentUpdate/Backup`, and if the new one cannot be moved in, the previous
  one is put back. The next startup reports the result, and a failed attempt is tried again at the next exit.
  - A git clone is never overwritten: the menu says to use `git pull`. A copy installed under the engine (Fab, the
    launcher) is updated from the launcher.
  - **Check for updates at startup** (Editor Preferences > Plugins > UEAgent, off by default) asks at most once a
    day and shows a notification only when there is something new.
  - Console: `mcp.update.check`, `mcp.update.status`, `mcp.update.download`, `mcp.update.cancel`.
  - Privacy: GitHub is UEAgent's only connection of its own, made only when you check or turn the startup check on.

- A documentation page, [Choosing an MCP server](docs/choosing.md): how a server inside the editor differs from one
  outside it, and the questions to ask any MCP server for Unreal, with UEAgent's answers and what it does not do.

### Changed

- `Tools/package_release.py --fab` leaves `LICENSE` and `README.md` out of the Fab archive. On Fab, UEAgent is free
  under Fab's Standard License, and those files say MIT; on GitHub it stays MIT.

### Fixed

- The image link previews show (GitHub, the documentation site) said 2,000+ tools, a count from before 2.0. It
  no longer gives a count, which changes with each release, and its source is in `.github/social-preview.html`.

## 2.3.0 - 2026-10-21

One tool per intent, one verb per operation, and every part you can add you can also read and remove. 167 tools that
did what another tool already does leave the list, 39 are renamed, and 38 new tools close the gaps. Nothing breaks: an
old or retired name keeps working until 3.0, and its reply says what to call instead.

### Added

- **`set_properties` reaches what the typed setters did.** The actor can be named by a label only it has, and a
  component by a class only one of its components has (`"Sun.DirectionalLightComponent"`). `save=true` saves an asset,
  and `clamp=true` keeps numbers inside the property's ClampMin/ClampMax and lists what it clamped. A blend space is
  resampled after a write. When a path does not resolve, the error says which part missed and lists the actor's
  components.
- **`spawn_actor` places any actor class**: an engine or plugin class by name or path, or a Blueprint. It writes the
  actor's properties in the same call (`{"ExponentialHeightFogComponent.FogDensity": 0.05}`) and reports
  `has_authority`.
- **One tool for each thing that has settings:**
  - `set_viewport_settings`, `set_section_properties`, `set_sequence_settings`;
  - `set_niagara_emitter_properties`, `set_niagara_component_variable`;
  - `set_gameplay_effect_modifier`, `set_gameplay_effect_component`;
  - `set_motion_warping_target`, and `create_water_body` with a body type.

  `set_material_instance_parameter` also takes static switches, component masks and layer parameters.
  `list_collision_channels` reports the query each custom channel answers to.
- **What could only be added can now be read and removed:**
  - **Widget Blueprints:** `remove_widget_from_blueprint`; widget-animation tracks and keys with
    `get_widget_animation_tracks`, `remove_widget_animation_track`, `remove_widget_animation_keyframe`.
  - **MetaSounds:** wire nodes to each other and to graph inputs with `connect_metasound_nodes` and
    `connect_metasound_graph_input_to_node`; also `disconnect_metasound_nodes`, `list_metasound_connections`,
    `remove_metasound_interface`.
  - **Other audio:** `remove_sound_cue_node`, `remove_submix_effect_from_chain`, `remove_sound_mix_class_override`.
  - **Blueprints:**
    - Timeline tracks: `add_blueprint_timeline_track`, `remove_blueprint_timeline_track`,
      `get_blueprint_timeline_info`.
    - `remove_function_parameter`, `remove_function_local_variable`, `remove_blueprint_macro`,
      `remove_ai_perception_sense`.
  - **Behavior Trees:** a node's settings, such as a MoveTo's blackboard key or a Wait's time, with
    `get_behavior_tree_node_properties` and `set_behavior_tree_node_property`.
  - **Animation and physics:** `remove_montage_segment`, `remove_anim_notify_name`, `remove_rigvm_link`,
    `add_physics_asset_body`, `remove_physics_asset_body`.
  - **Project and world:** `remove_collision_channel`, `remove_collision_profile`, `remove_pcg_graph_parameter`,
    `unload_world_partition_region`, `remove_asset_metadata_tag`, `set_chooser_cell`.
  - **USD:** `clear_usd_payloads`, `remove_usd_variant_set`, `remove_usd_variant`.
  - **Cameras, Niagara and Sequencer:** `remove_camera_node_child`, `remove_niagara_user_parameter`,
    `resume_level_sequence_runtime`.
- **Existing tools reach more:**
  - The section tools reach sections of root tracks (shots, camera cuts, audio, events) without a `binding_guid`.
  - `remove_sequence_track` removes a binding's track or the camera-cut track.
  - `delete_material_expression` and `disconnect_material_expressions` work inside material functions.
  - `add_function_parameter` adds a macro's inputs and outputs.
  - `set_camera_node_parameters` takes a child node.
- **Old and retired names keep working.** A call to one runs as before. Its reply carries `data.retired_name`, which
  names the tool that does the same and how; on a failure, the error text says so too, and so does `describe_tool`.
  These names are left out of `tools/list`, `find_tools` and the reference.
  [Retired names](docs/tools/retired.md) lists them.
- Every file under `Source/` starts with a copyright notice, which Fab requires of a code plugin, and
  `Tools/check_copyright.py` checks it in CI. `Tools/package_release.py --fab` builds the source archive
  Fab takes for an engine version (see RELEASING.md).

### Changed

- **One verb per operation.**

  | Verb | Use |
  |---|---|
  | `get_` | reads one thing |
  | `list_` | enumerates |
  | `find_` | searches |
  | `is_`, `has_` | answer yes or no |
  | `create_` | makes an asset |
  | `add_` | puts a part into an asset or an actor |
  | `spawn_` | places an actor |
  | `set_` | writes |
  | `remove_`, `delete_`, `clear_` | take away |

  39 tools were renamed to follow it, for example:
  - `inspect_object` → `get_object_info`
  - `search_assets_by_pattern` → `find_assets_by_name`
  - `query_gameplay_tags` → `find_gameplay_tags`
  - `configure_replication` → `set_actor_replication`
  - `place_player_start` → `spawn_player_start`

  The server's instructions state the verbs, and the catalog generator refuses a new tool that starts with a synonym.
- `set_properties` refuses a key inside a struct that names no field of it; before, it skipped the key silently.
- The skills use only tools that are still listed.

### Fixed

- `add_comment_node` keeps its text.
- `add_anim_notify_name` records an undo step on a skeleton duplicated from engine content.
- `get_material_graph_info` no longer counts a deleted node as an orphan.
- `add_widget_to_blueprint` named the runtime tool `remove_widget` as its inverse; it now names
  `remove_widget_from_blueprint`.
- Building the plugin warned 148 times that a monolithic header should not be used: three shared headers
  included `Json.h`, and two command families `UMG.h` and `EngineSettings.h`. They include the headers they
  use one by one, as IWYU and Fab require. On 5.7, the build also warned about `USDMemory.h` where the
  `SetMetadata` write opens USD's allocator scope; the warning does not apply there, and is turned off.
- The plugin's description in **Edit > Plugins** said about 2,100 tools, the count before 2.0.
- The documentation site's GitHub box could show an older release for as long as the browser tab lived: the
  theme kept the release, stars and forks for the whole session. It now asks GitHub again after an hour.

### Retired

167 tools, each still callable until 3.0, with its replacement:

| Retired | Use instead |
|---|---|
| Typed setters and readers of lights, fog, sky, clouds, decals, cine cameras, post-process, replication, sounds, Niagara systems, physical materials, montages, foliage types and other assets | `set_properties`, `get_properties` |
| Class-specific spawners | `spawn_actor`, `spawn_box_volume` |
| Per-setting tools for the viewport, Sequencer sections, sequences, Niagara emitters, Niagara component variables, GameplayEffects, material static parameters, motion warping and water bodies | The tools above |
| Four batch twins | `execute_batch` |

[Retired names](docs/tools/retired.md) gives the call that replaces each.

## 2.2.0 - 2026-10-20

UEAgent's skills come with the plugin and work in every client that reads the Agent Skills format,
and the editor connects more clients than Claude Code.

### Added

- **Skills for every client, installed from the editor.** **MCP > Skills** copies UEAgent's skills into
  the project: `.claude/skills` for Claude Code, `.agents/skills` for Codex, Cursor, VS Code,
  Antigravity CLI and Gemini CLI. They ship inside the plugin, so they always name the tools of the
  version installed. Each copy is stamped with that version: the menu offers **Update** after the
  plugin changes, replaces only the folders it installed, and never touches a skill of your own.
  Console: `mcp.skills.status`, `mcp.skills.install`, `mcp.skills.remove`.
- **Two skills to contribute.** `ueagent-report-issue` turns a bug or a missing tool into a GitHub issue,
  with the versions, the call and the editor log, and shows it before opening it. `ueagent-contribute-fix`
  turns a change to the plugin's code into a pull request, following the repository's rules.
- **Connect another client.** The same menu writes the `unreal` entry for Cursor (`.cursor/mcp.json`),
  VS Code (`.vscode/mcp.json`) and Antigravity CLI (`.agents/mcp_config.json`), with the compact tool
  list, and copies the `codex mcp add` command for Codex. As with Claude Code, only that entry is
  written and the rest of the file keeps its layout; with an auth token, Cursor reads it from
  `UEAGENT_TOKEN` and VS Code asks for it once. Console: `mcp.clients.status`, `mcp.clients.connect`.

### Changed

- The skills were renamed with a `ueagent-` prefix (`ueagent-skill-builder`, `ueagent-blueprint-actor`,
  `ueagent-project-audit`, `ueagent-level-lighting`) and no longer use anything only Claude Code
  understands.
- The startup hint appears only when no client of the project is connected.

### Removed

- **The Claude Code plugin marketplace** (`/plugin install ueagent --marketplace CarlxsMG/UEAgent`). It
  always followed the latest code, so its skills could name tools the installed plugin did not have.
  Remove it with `/plugin uninstall ueagent@ueagent` and `/plugin marketplace remove ueagent`, then
  install the skills from the editor's menu.

### Fixed

- `run_automation_test` did not find a test by the name `list_automation_tests` shows first
  (`full_test_path`, such as `UEAgent.ClaudeCode`); it accepts that name as well as `test_name`.

## 2.1.1 - 2026-10-20

A closer review of the 5.7 build, with new tests on both engines. Most of what it found was on 5.8 as well.

### Added

- The documentation site has a home page, a getting-started page and a tool explorer that searches every tool by
  name or by what it does and filters them by domain, by what a call does to the project and by whether
  Play-in-Editor refuses it. New pages cover answers and refusals, settings, a glossary, security and privacy, the
  changelog and contributing. In the reference, each tool shows its annotations and whether PIE refuses it, and a
  tool's name links to its entry wherever the docs mention it. Each page has its own description and link preview,
  and the site publishes `llms.txt` and the tool catalog as JSON.

### Fixed

- Stopping Play-in-Editor closed the editor after `create_widget` had made a widget in the PIE world. The call's
  undo step kept the PIE game instance and the widget, and the engine's check for leaked worlds failed on Stop. A
  call's undo step that holds objects of the running PIE session is no longer kept; what the call did stays.
  `end_transaction` closes such a transaction without an undo entry (`recorded: false`), and when PIE ends with them
  still in the undo history, the history is reset, as the engine does for its own PIE objects.
- `cancel_transaction` said it rolled the edits back, and left them in place. It now reverts what the transaction
  recorded and reports `rolled_back`. `execute_batch` with `transaction` and `stop_on_error` uses it when an item
  fails, so the items that ran before are reverted too; `transaction_rolled_back` says so.
- `reparent_widget` moved a widget into itself and saved the tree with a cycle. A new parent that is the widget or
  one of its descendants is now refused, and nothing changes.
- `duplicate_widget` answered success while the copy was lost: the next compile renamed it `TRASH_<name>_0`, and
  the reply said `duplicated: false`. The copy now gets its name after the Blueprint registers it, and a copy that
  is not in the tree is an error.
- `fire_mass_signal` with an `entity_handle` that names no live entity: index 0 closed the editor, and any other
  dead handle answered success. Both are now refused, and nothing is signalled.
- `read_data_table_from_package_file` raised an engine ensure on every call: it loaded the file for a diff outside
  `/Temp/`. It now reads a throwaway copy under `Saved/Diff`, as the editor does for a source-control diff, and deletes
  it afterwards.
- The descriptions of `import_font_file` and `set_audio_section_properties` say what each engine does.

## 2.1.0 - 2026-10-20

UEAgent runs on Unreal Engine 5.7 as well as 5.8: one source tree, and a precompiled zip for each engine.

### Added

- **Unreal Engine 5.7 support.** Where 5.8 added or renamed an API, the 5.7 build uses the 5.7 equivalent, so the
  tools behave the same on both engines. Each release ships `UEAgent-<version>-UE5.7-Win64.zip` next to the 5.8 zip.
  Both are built and tested separately: 2528 checks on 5.8, and 2490 on 5.7.
- A few things need 5.8, because 5.7 has no API for them. On 5.7 they answer an error that says so and change nothing:
  - `apply_adaptive_remesh`;
  - `add_field_node` with `node_type` RotatedRadialVector;
  - `add_chooser_column` with `column_type` Name or OutputName;
  - `set_modulation_parameter_settings` with `clamp_to_normalized_range`;
  - `set_audio_section_properties` with `play_until_finished`;
  - on USD stages, writing a path expression (`set_usd_attribute_value`) or an integer list op
    (`set_usd_prim_metadata`). On 5.7, whose USD is 25.08, writing such a value closes the editor; reading one
    works.

### Fixed

- On 5.7, `set_usd_prim_metadata` replacing a held dictionary (such as `customData`) closed the editor. The write now
  runs in USD's allocator scope.

## 2.0.2 - 2026-10-20

Tools that do the same kind of job now say which one to pick, and about forty tools stop reporting success for
work they skipped or only half did.

### Changed

- 113 descriptions say when to use the tool instead of its generic sibling, and when the generic one is the better
  pick. This covers the generic property tools (`get_properties`, `inspect_object`, `get_actor_properties_by_class`,
  `get_component_properties`, `set_properties`, `set_component_property`), `spawn_actor`, `spawn_blueprint_actor`,
  `add_component_to_actor`, `add_component_to_blueprint`, and the typed `get_/set_<type>_properties`, `spawn_*` and
  `add_*_component` tools.
- The `initialize` instructions add a short guide to choosing among similar tools.
- `spawn_niagara_component` places a real `ANiagaraActor`, the actor the editor places when a Niagara System is
  dragged into the level, with one undo step; `save_level` keeps it. It used to create a transient component
  owned by the level's WorldSettings: nothing was saved, and `actor_label` renamed the WorldSettings.
- `get_audio_component_properties` searches every audio component, not only each actor's first one, and takes an
  optional `actor_name` for a component name that several actors share.
- `get_light_properties` reads `light_intensity_units` (a unit name, on point, spot and rect lights) and
  `light_lighting_channel_mask` off the light; both were fixed values. `light_min_roughness` is gone (the engine
  property is deprecated), and the falloff keys appear only on point and spot lights.

### Fixed

- **Partial writes.** These tools validate every input before the first write; a refused call leaves the asset or
  actor unchanged: `set_foliage_type_properties`, `set_landscape_grass_type_properties`,
  `set_reflection_capture_properties`, `set_scene_capture_2d_properties`, `set_sound_attenuation_properties`,
  `set_sound_concurrency_properties`, `set_sound_wave_properties`, `set_pose_search_schema_properties` and
  `set_pcg_node_properties`.
- **Input that was skipped without an error is refused:**
  - an array of the wrong length in `set_exponential_height_fog_properties`, `set_sky_atmosphere_properties`,
    `set_volumetric_cloud_properties`, `set_sky_light_properties`, `set_widget_slot_properties`,
    `set_box_slot_properties`, `set_grid_slot_properties`, `add_spline_component` and `set_spline_points`;
  - a path that does not load in `set_static_mesh_properties`, `spawn_decal_actor` and
    `add_instanced_static_mesh_component`;
  - a key that names no field in the sound, pose search and PCG setters, whose `applied_keys` listed it as applied;
  - a `count` that `spawn_mass_entities` would ignore.
- **Text read only in part.** A value given as text is refused when the property's parser cannot read all of it. The
  number parser took "abc" as 0 and "12abc" as 12, and the call answered success. This covers `set_properties`,
  `set_component_property` and the other tools that share the JSON importer, plus `set_mass_trait_property`,
  `set_data_asset_property`, `set_anim_graph_node_property`, `set_project_settings_value`,
  `set_niagara_data_interface_default`, `set_material_expression_property` and the arguments of `call_function`.
  A refused value leaves the property as it was. The JSON importer behind `set_properties`, `set_component_property` and
  `set_blueprint_component_properties` also refuses a JSON string that is not a whole number, a decimal number or a
  bool where one is expected, at any depth of a struct, array or map, instead of writing 0 or false. A tag container
  given as an array refuses an unregistered tag instead of leaving it out.
- The sky light, sky atmosphere and volumetric cloud setters refuse a Static component, whose engine setters change
  nothing; they used to count the fields as applied.
- The StateTree setters (`set_task_property`, `set_condition_property`, `set_consideration_property`,
  `set_evaluator_property`, `set_state_tree_global_task_property`) answer `success: false` with `save_error` when the
  save fails; they answered success with `saved: false`.
- `set_mass_trait_property` is one undo step and sends the change notification.
- `set_physical_material_properties` refreshes the material in the running session, reports `changed` as measured
  (`updated` was always true), refuses a call with no field, and returns its values when the save fails.
- `set_pose_search_schema_properties` measures `changed` (it was true whenever a key was sent).
- `set_sound_wave_properties` sends the change notification, so compression settings rebuild the audio as a
  Details-panel edit does.
- `set_data_asset_property` writes a JSON object or array through the JSON importer; it used to write it as empty
  text.
- `create_procedural_mesh_component` makes the mesh component the root, so `location` applies; it used to leave the
  mesh at the origin.
- `add_motion_warping_component` and `add_gameplay_camera_component` add an instance component that `save_level`
  keeps, attached to the root, with one undo step. A second `UMotionWarpingComponent` name no longer collides with an
  existing object, which could close the editor.
- `set_properties` no longer says it skips gameplay-tag validation: it refuses an unregistered tag, like
  `set_component_property`.

## 2.0.1 - 2026-10-20

A patch release: an Interchange registration that closed the editor is refused, and three other Interchange tools
refuse what they used to accept silently.

### Fixed

- `register_interchange_translator` refuses a class that is not a concrete Interchange translator. It used to register
  any class, such as `/Script/Engine.Actor`, and the next format query (`get_interchange_supported_formats`, or the
  editor's import dialog) failed an engine assertion and closed the editor. The reply adds `already_registered` and
  `supported_formats`.
- `register_interchange_factory` refuses a class that is not a concrete Interchange factory; it used to answer success
  with `registered: false`. It also refuses a factory for an asset class that already has one: the Interchange
  manager keeps the first, so the call used to report success without registering anything. The reply adds
  `asset_class` and `already_registered`.
- `set_interchange_pipeline_stack_default` refuses a stack name that the import path does not have, and lists the ones
  it has; it used to write any name to `DefaultEngine.ini`. It also refuses a read-only `DefaultEngine.ini` before
  changing anything, and reports `previous_stack_name`, `changed` and `config_file`.
- `set_interchange_pipeline_properties` refuses an empty `properties` object, and skips properties that are read-only
  in the editor.
- Clearer descriptions for these four tools, `get_anim_sequence_info` and `remove_trigger`. The example class paths of
  the two registration tools named classes that do not exist.

## 2.0.0 - 2026-10-20

A consolidation release: 1742 tools instead of 2097, with one tool per job and every name verb-first. Old names are
not kept as aliases. [Migrating to 2.0](docs/migrating-to-2.0.md) lists every removed and renamed tool with the call
that replaces it.

### Breaking changes

- **342 tools are folded into a canonical tool.** Each canonical tool gained the parameters and reply fields of the
  tools folded into it, so no capability is lost. Some examples:
  - `delete_asset` takes `expected_class` and replaces the 22 typed deleters (`remove_anim_sequence`,
    `delete_state_tree`, `remove_render_target_2d`…).
  - `add_graph_node` replaces the 11 `add_*_node` tools.
  - `get_undo_state` replaces `can_undo` and `can_redo`; `undo` and `redo` take a count.
  - `call_function` replaces `call_object_method`, `call_library_function` and `call_component_function`.
  - `set_world_gravity` takes the gravity presets of `configure_gravity`.
  - `get_data_table_row` replaces the other row readers.
- **13 tools that could never do what their name said are removed.** Examples: `set_preserve_scale_ratio`,
  `list_mass_signals`, and `build_hlod` and `convert_map_to_world_partition`, which reported a build they never
  started. Two others are now fixed instead:
  `get_ability_tasks` lists the tasks, and `get_component_state_tree_path` returns the path.
- **160 tools are renamed** so each name starts with its verb and spells its words out:
  - domain-first names now lead with the verb (`interchange_import_asset` → `import_interchange_asset`,
    `metasound_add_interface` → `add_metasound_interface`);
  - bare names gain a verb (`batch` → `execute_batch`, `bookmark` → `add_trace_bookmark`, `editor_status` →
    `get_editor_status`);
  - abbreviations are spelled out (`set_bp_variable_default` → `set_blueprint_variable_default`, `add_bt_service` →
    `add_behavior_tree_service`, `get_rvt_info` → `get_runtime_virtual_texture_info`).
- **Calls that used to succeed and now behave differently:**
  - `duplicate_asset` refuses a destination name that is already taken, and a destination outside writable project
    content.
  - `configure_replication` applies only the settings it is given, and refuses a call with none.
  - `undo` and `redo` refuse a count below 1.
  - `remove_behavior_tree_child` without `kind` matches either kind of child.
  - `spawn_box_volume` sets the spatial-loading flag only when it is passed.
  - `list_derived_classes` no longer lists the base class itself.
  - `add_graph_node` with `custom_event` returns an event that already has that name instead of adding a duplicate.
  - `get_graph_nodes` with `node_id` returns the node at `nodes[0]`.
  - `set_skeletal_mesh_section_flags` refuses a call that sets no flag.
  - `create_modulation_generator` refuses an unknown waveform instead of falling back to Sine.
  - Several tools that used to accept bad values silently now refuse them: an out-of-range `mass_scale`, a blend-space
    axis whose minimum is not below its maximum, and an unknown collision shape.

### Fixed

- `list_cvars` with an empty filter lists every console variable instead of none.
- `find_free_input_key` returns the key's name in `name`, with its display name in `display_name`.
- `get_material_parameters` reports each parameter's effective value with an `overridden` flag; it used to report only
  overrides.
- `list_input_types` returns the value types it promised.
- `create_blend_space` writes every axis setting it accepts.
- `import_texture_file` honours `replace_existing: false`.
- `create_gameplay_effect_asset` creates the asset at `package_path/name`; it used to save the package at the folder
  itself, so the path it reported did not exist.
- `create_material_variant` and `create_material_instance` report the overrides they apply, and no longer write an
  override for a parameter the parent material does not have.
- Mass entity counts read 0 instead of -1 on a world that has not created an entity yet, and the spawn and despawn
  deltas are no longer one too high there.
- `add_blueprint_function` with `override=true` on an event returns the existing event node when called again, instead
  of adding a duplicate at 0,0.

## 1.2.10 - 2026-10-19

- Compiling a Widget Blueprint no longer raises the engine's "widget was added but did not get a GUID" ensure: before
  `compile_blueprint` or a widget tool compiles one, any widget or animation of its tree that has no variable GUID
  (one a script put there, for instance) is registered as the designer registers a new widget.

## 1.2.9 - 2026-10-19

- USD integer list ops (`inactiveIds` and other int, uint, int64 and uint64 list ops) read and write as JSON lists of
  numbers, a `pathExpression[]` array reads as an array of strings, and a path expression (or an array of them) is
  written from its text, an unparsable one refused with USD's parse error. They go through a new small module,
  UEAgentUsdSdk, built against the USD SDK the way the engine's own USD modules are.
- The double-slash guard no longer refuses a USD path expression such as `/World//Mesh` in the `value` that
  `set_usd_attribute_value` and `set_usd_prim_metadata` write; every other argument, including those of a `batch`
  item, is still checked.

## 1.2.8 - 2026-10-19

- A name created inside an asset no longer kills the editor when the asset's path and the name are long together: a
  component, widget, graph or state takes the asset's object path plus the name as its own object path, and the
  editor builds an Unreal name from it. A call whose asset path plus its longest other name argument passes 863
  characters is refused before it runs, naming both.
- Nested name fields written in camelCase (`newName`, `assetPath`) are checked like the snake_case ones.

## 1.2.7 - 2026-10-19

- A malformed vector is refused instead of read as zeros: 123 location, rotation, scale, center and color-style
  parameters of 85 tools take exactly their 2 or 3 finite numbers (a rotation also its `{pitch, yaw, roll}`
  object), and anything else -- another length, a string, a `null` in a `batch` item -- is refused before the
  tool runs. A color
  array (`set_anim_curve_color`, `set_niagara_component_color`, `set_niagara_module_input`) must be 3 or 4
  numbers.
- Names stop at 959 characters, leaving room for the prefixes and suffixes tools add before they build an Unreal
  name, and the limit now also applies to name fields nested inside objects (`bulk_rename_assets` rows,
  `build_blueprint_graph` nodes).
- An asset path is checked as the engine names it: the object path (package, a dot, the asset name) plus room for
  the objects created inside the asset must fit in an Unreal name, so an object path of at most 863 characters
  is accepted (less for a long asset name). The editor builds a name from every object it records while it
  creates an asset, so a package name under the limit could still kill it. 37 tools that join a folder and a
  name, or add a suffix (`_T`, `_Font`, `_Sub`), check the joined path before the engine builds it, a World
  Partition map includes its external actor packages, and copies and renames also measure the source asset's
  deepest object.
- USD dictionaries (`customData`, `assetInfo`) and list ops (`apiSchemas`, references, payloads) read as JSON,
  path expressions read as text, and `set_usd_prim_metadata` writes vectors, arrays, dictionaries and list ops;
  one reply's values share a size budget so it stays inline.
- `get_mesh_open_border_loops` reports `ambiguous_topology` for real (a bowtie vertex, or a boundary the engine
  could not close) and counts the open spans it used to drop, in `open_border_spans`.

## 1.2.6 - 2026-10-19

- A name of 1024 characters or more no longer kills the editor: Unreal's names hold at most 1023, and any
  string argument that becomes one (actor, asset and property names, content paths, tags, keys: 3634 parameters)
  is refused before the tool runs, as is a `batch` item's command name and a package path that only the joined
  pieces make too long. Also refused before they could crash: a widget name already held by another object in a
  Widget Blueprint's tree (`add_list_view`, `add_tile_view`, `add_tree_view`, `create_common_ui_stack`).
- Maps are saved as maps: a map package (`create_world_partition_map`, `set_data_layer_initial_runtime_state`,
  the StateTree component-override tools) goes through the editor's own map save, as a `.umap` with its
  external actor packages, instead of a stray `.uasset`.
- USD vectors, quaternions, matrices and arrays are read and written as JSON arrays; reading a large array stops
  at 600 numbers (fewer long strings) and says `length` and `truncated`, so the reply stays inline.
- `lod_index` chooses the geometry in the `get_mesh_*` static mesh queries and `copy_static_mesh_to_dynamic_mesh`
  (a LOD the mesh lacks is refused); the UV mapping tools read LOD 0 instead of a hi-res source.
- A call that changes nothing saves nothing and leaves no undo step, now also in the Sequencer setters, the
  StateTree, Smart Object, Niagara emitter and system setters, the MVVM list views and view models, EQS scoring,
  river widths and `scale_mesh_uvs`.
- The asset removers (`remove_anim_*`, `remove_blend_space`, the render target, pose search, subtitles, trigger
  and volume removers, `delete_state_tree`, `delete_material_function`, `delete_meta_sound` and more) refuse an
  asset of another type and answer a failed delete as a failure; a missing asset is still a success.
- 23 more replies said `saved: true` without a save; they say `saved: false`.
- 79 parameters of 70 tools that the tools cannot do without are published as required, and 14 parameters have
  their real JSON type (arrays, numbers); the catalog generator now also reads `HasField` guards and follows
  template helper calls.
- Niagara: light and decal renderers belong to their emitter, the property setters check every field before
  writing any, `add_niagara_simulation_stage` applies `iteration_count`, and a number given as a string or a
  bool is refused instead of read as 0.
- Values that were wrong: `add_weighted_value_keyframe` and the audio channel keys landed at display frames
  instead of seconds, and the weighted key went to the first float track whatever `property_name` said;
  `set_river_width_depth_at_point` did not update the spline; `set_water_body_properties` did not update the water
  mesh or landscape; a convex or k-DOP collision call with a bad value left a duplicated mesh behind.
- Changed: `set_viewport_view_mode` accepts every view mode that needs no extra setup and refuses the others with
  the reason, and `get_viewport_state` names them all; `create_metasound_source`, `run_eqs_query`,
  `set_eqs_test_scoring`, `bind_widget_to_viewmodel` and `enable_plugin` refuse an unknown name instead of
  falling back to a default; `delete_meta_sound` deletes only MetaSounds; another class under a widget or view
  model name is refused; `configure_post_processing` documents that Low equals Disabled.

## 1.2.5 - 2026-10-19

- More calls that killed the editor are refused before anything runs: an empty key in `set_string_table_entry`,
  an `input_key` before the first spline point in `set_river_width_depth_at_point` and
  `set_water_body_properties`, a static mesh with no source model in the `get_mesh_*` queries and
  `copy_static_mesh_to_dynamic_mesh`, a name held by another class in `create_material_instance_dynamic`, a
  link that would make a loop in `connect_sound_cue_nodes`, an empty renderer slot in `remove_niagara_renderer`,
  and USD tokens and type names of 1024 characters or more.
- Parameters that could not be left out now can: 27 parameters of 17 tools were published as required although
  the handler needs them only in some cases. `add_graph_node` demanded all of `event_name`, `function_name`,
  `target_class`, `variable_name`, `struct_type`, `enum_path`, `dispatcher` and `macro_graph_name` for any node;
  the others are in `overlap_scene`, `diff_data_tables`, `delete_render_job`, `list_states`,
  `set_component_parameter_override`, the static mesh collision tools (`dest_path`), two Niagara tools,
  `set_water_body_properties` (`input_key`), `get_blackboard_data` (`actor_name`), the landscape grass and
  foliage tools, and `set_spawn_ownership`, whose `sequence_path` is now an alias of `sequence_name` and takes a
  path.
- A failure is no longer answered as a success: an attach the engine refused, a link that did not take, a
  camera rig built with errors, a skeletal mesh LOD bake refused in Play-in-Editor, a view model that was not
  assigned, a USD attribute or metadata value USD refused, a MetaSound value of the wrong type, a light the
  engine would not change, a failed weld or polygroup paint, a removal that did not delete, and the save of
  about 20 Niagara tools, which was ignored.
- `saved: true` without a save is gone: the create tools that find their asset already there answer
  `saved: false` and save nothing, as do the other calls that change nothing.
- Undo and the dirty flag: the water, decal, light, EQS, Smart Object and Niagara emitter setters record the
  object before they write it, so the change is one undo step and the level or asset is marked for saving.
  Setters that write several values check them all first, so a refusal leaves nothing half-written.
- Values that were wrong: USD float, half, unsigned, 64-bit, token and asset values read back as 0 or null and
  could not be written; an audio section's start offset was converted at the display rate instead of the tick
  resolution; view mode names did not round-trip; `set_viewport_fov` was lost on the next camera reset;
  `get_material_graph_info` named most node inputs None; the blackboard listings left out inherited keys; the
  `get_mesh_*` queries reported a LOD they had not read; `configure_post_processing` set four console variables
  that UE 5.8 does not have and reported them on; `set_anim_transition_rule_from_comparison` failed on float
  variables and could leave a half-built rule; `end_behavior_on_actor` always said `ended: false`, and stopped
  every behavior when its class did not resolve; `link_media_texture_to_player` never saved the link;
  `create_material_instance_dynamic` handed out instances the next garbage collection could free; and
  `run_asset_action_utility` ran actions on the class default object, which the editor never does.
- Changed: `set_decal_sort_order` requires `sort_order` (a missing one wrote 0); `configure_post_processing`
  drops `anti_aliasing_enabled`, `global_illumination_enabled` and `color_grading_enabled`, which no 5.8 console
  variable backs, and reports each variable it sets as read back; `set_section_blend_type` refuses a blend type
  the section does not support; `add_list_view` reports `list_view_class` as `UListView`, like its siblings.
- 40 more integer parameters are checked against their range before the call runs.
- 22 more short tool descriptions are rewritten from the handlers' code.

## 1.2.4 - 2026-10-18

- Dynamic mesh edits can be undone: the 38 tools that edit a DynamicMeshActor's mesh (booleans, remeshes,
  normals, UVs, material IDs, transforms, appends, triangle deletes...) are one undo step each, and redo
  applies the edit again. The engine does not record a dynamic mesh's geometry in a transaction, so before
  only the actor was recorded and Ctrl+Z reverted an earlier step instead. An edit that changes nothing adds
  no undo step, and a failed one leaves the mesh as it was.
- A call that changes nothing leaves no undo step and keeps the redo history, also in
  `add_anim_bone_track` and `set_anim_bone_track_keys`, whose engine controller records a change whatever it
  writes.
- `add_attribute_to_set` and `create_attribute_set_asset` refuse a Play-in-Editor session, a read-only root
  and a `.uasset` read-only on disk before they touch anything, and a failed save of `add_attribute_to_set`
  takes the attribute out again.
- A failed `add_rigvm_link` leaves the package's dirty flag as it was when the graph is unchanged.
- `add_chooser_column_to_table` places a column as the Chooser editor does: filters before outputs, outputs
  before a trailing Randomize column.
- Answers that were wrong: `delete_triangle_from_mesh` and `delete_selected_triangles_from_mesh` reported the
  triangle ID space instead of the triangle count, `compact_mesh` always said `compacted: false`, and
  `add_anim_transform_curve` answered success if the engine refused its keys.

## 1.2.3 - 2026-10-18

- A number outside the integer range a tool reads it into is refused before the tool runs, in every tool
  and inside `batch`: the engine could not read it, and the tool went on with its default (a
  `max_results` of 3000000000 became 1000). The catalog gives each of the 292 parameters read as an
  integer its range (`intRange`).
- A valid no-op succeeds without saving in the animation tools that still saved one: `add_anim_bone_track`
  and `set_anim_bone_track_keys` (compared through the data model's content hash),
  `add_anim_transform_curve`, `add_pose_to_pose_asset`, `collapse_montage_segments`,
  `sort_montage_segments` and `refresh_anim_montage_cache`, which is undoable now.
  `add_anim_transform_curve` no longer resets the sequence's sampling rate and per-platform overrides.
- `set_anim_number_of_frames` also refuses a count that is not whole at a per-platform sampling rate, which
  the engine `check()`s when it compresses for that platform.
- Nothing is left half-written: a GameplayEffect, ability or cue setter whose save fails undoes its edit
  (a `.uasset` read-only on disk is refused before it), `add_rigvm_link` leaves the graph as it was when
  the link fails, and `set_camera_node_parameters` writes all of its properties or none (a value it could
  not parse used to be written as 0 or false).
- `remove_gameplay_effect_component` no longer writes the removed component back into the saved asset.
- `add_chooser_column_to_table` keeps a Randomize column last, as the Chooser editor does.
- `add_camera_component` turns on the `bOverride_` flag of each `post_process_settings` value it sets (a
  value without it has no effect), and refuses a key that is not a post-process setting. The camera's
  visualization components are created as instance components.
- The MPCI tools reach the Play-in-Editor world (`world_label` `pie`, `server`, `client`, `pie:N` or the
  PIE world's package name; a PIE world shares the editor world's object name), and every reply says which
  world it reached. `inspect_subsystem_instance` and `list_local_player_subsystems` read the PIE game
  instance and local player, through a new optional `world` parameter.
- `get_lighting_quality` reads the editor world and counts every light component of an actor, sky lights
  included; `find_nearest_ray_intersection_with_mesh` refuses a ray direction that cannot be normalized;
  `get_component_properties` reported `Unknown` for an instance component's creation method.

## 1.2.2 - 2026-10-15

- More calls that killed or hung the editor are refused before they touch anything:
  `copy_dynamic_mesh_to_static_mesh` with `lod_index` 8 or more (a negative one silently overwrote LOD 0),
  a huge `section_index` on the procedural mesh tools or `element_index` on
  `set_procedural_mesh_material`, `create_procedural_grid_mesh` with `num_x * num_y` past 1,000,000
  vertices, a huge `option_index` on the EQS tools, and `duplicate_anim_sequence` onto an existing asset
  (the engine destroyed the live object and built the copy in its memory; it now refuses, as
  `duplicate_mpc` does). A section or option index can be at most the current count, which appends one,
  and a number outside int32 is an error instead of silently becoming the default.
- The Sequencer tools that take `sequence_path` or its alias `sequence_name` accept either: ten of them
  published both as required, so a call with one was refused before it reached the tool
  (`add_shot_track`, `add_subsequence_track`, `add_camera_cut_section`, `get_sequence_track_count` and six
  more).
- A valid no-op succeeds without saving: the IK Rig and retargeter tools and the setters and add or remove
  tools of animation assets saved on a no-op, so during Play-in-Editor or under a read-only root a no-op
  failed; they now answer the flag false and `saved: false`, and leave the asset untouched. The create tools
  said `saved: true` for an asset that already existed.
- `create_anim_montage` and `add_montage_segment` added a second, empty `DefaultSlot` track (the engine's
  `IsValidSlot` is false for an empty track), and `add_montage_slot` added one on every call.
- Answers that were wrong: `is_bvh_valid_for_mesh` always answered false; `reimport_anim_sequence`,
  `rename_pose_in_pose_asset` and `set_pose_asset_base_pose` answered success when the engine refused;
  `set_ik_solver_bones` for a solver type with no start bone; `set_anim_number_of_frames` tripped an engine
  ensure on a count that is not whole at the sampling rate; `create_chooser_table` mapped an unknown
  `result_type` to Object; `add_subsequence_track` put its section on a cinematic shot track;
  `get_timeline_info`, `get_sequence_master_tracks` and `get_sequence_track_count` left out the camera cut
  track (the last two every binding track too); `get_niagara_data_interfaces` reported the object name
  where the other tools take the user parameter name.
- Undoable now: `connect_ik_goal_to_solver`, `set_retargeter_op_settings`, `set_blendspace_properties` and
  `set_blendspace_interpolation`. Every saving tool of animation assets reports `save_error`.

## 1.2.1 - 2026-10-15

- Calls that killed the editor are refused before they touch anything: a negative `section_index` on
  the procedural mesh tools, `remap_material_ids` with `compact: true` on a mesh whose material IDs
  reach past its material list, a negative `option_index` on `add_eqs_test` and `add_eqs_generator`,
  `add_spring_arm` and `add_camera_component` with the name of an existing component (they now take
  a free name), `set_anim_frame_rate` with an fps that rounds to 0, `move_ik_solver_in_stack` with
  `to_index` equal to the solver count, and `add_skeletal_mesh_socket` copying to the skeleton of a
  mesh without one.
- A failed save fails the call, with `save_error`, in the Sequencer tools that save a sequence, the
  animation sequence tools (`set_anim_frame_rate`, `set_anim_number_of_frames`,
  `duplicate_anim_sequence`, the remove and curve tools), the IK rig and retargeter tools, the
  Gameplay Cameras tools and the Chooser tools.
- Tools that answered success for nothing now say what happened. `set_anim_curve_keys` on a transform
  curve wrote nothing; `add_anim_bone_track` with an unknown bone replaced the root track;
  `generate_mesh_uvs` deleted every UV channel above the one it wrote; `apply_uniform_remesh` could
  not use `uv_preserving` smoothing and ignored `preserve_sharp_edges` and `auto_compact` set to
  false; the dynamic mesh boolean, normals, UV, material ID and transform tools and the render target
  draws measure what they did (an identity transform succeeds with `applied: false`);
  `add_metasound_graph_output` with an existing name reset its default and
  `add_metasound_graph_input` dropped it; `add_mass_trait`, `add_widget_to_blueprint` and
  `remove_string_table_entry` said added, saved or removed for nothing; `set_mid_scalar_param`,
  `set_mid_vector_param`, `set_mid_texture_param` and `copy_mid_params_from_source` accepted unknown
  parameters; `set_actor_material` accepted any slot; `set_anim_frame_rate` and
  `set_anim_number_of_frames` read back the wrong value and accepted a rate the engine refused;
  `set_anim_bone_track_keys` wrote an unknown bone's keys to the root track; `copy_mesh_uv_channel`,
  `repack_mesh_uvs`, `clear_render_target`, `export_render_target` and the behavior tree, level
  sequence and widget animation stop tools measure what they did; `add_niagara_data_interface` added
  a second parameter next to one of another class, and `remove_niagara_data_interface` also removed
  user parameters that are not data interfaces; `set_usd_prim_specifier` applied an unknown value as
  Def; and the geometry collection and Interchange pipeline tools report a failed save.
- Documented no-ops that answered failure now succeed: `remove_marked_frame`, `clear_marked_frames`,
  `add_sequence_folder`, `remove_sequence_folder` and `add_notify_track` on what is already there.
- Edits that were lost or half written: the Niagara data interface tools never dirtied or saved the
  system; the `set_game_mode_*` setters never dirtied a Blueprint GameMode; the gameplay effect and
  cue setters edited the class defaults and then had the save refused during Play-in-Editor (they
  refuse first); `create_level_sequence` shifted the keys of an existing sequence and accepted a rate
  below 1; `create_audio_bus` and `create_sound_source_bus` reset an existing asset's settings.
- Undoable now: the smart object slot tools, `add_eqs_test`, `add_spring_arm`,
  `add_camera_component`, `set_camera_asset_director`, `add_camera_node_child`,
  `set_control_bus_mix_stage_value`, `remove_niagara_data_interface`, `remove_anim_sync_marker`, the IK
  solver, goal, bone and retarget pose setters, and the Chooser tools. `remove_chooser_row` now saves,
  as `remove_chooser_column` does.
- Also fixed: `setup_foot_ik` binds the legs, `add_subsequence_track` refuses a sequence that would
  contain itself, `remove_widget` reaches the widgets `create_widget` makes in Play-in-Editor (new
  optional `target_world`), `add_chooser_column_to_table` adds a second column of the same type,
  `create_editable_skeletal_mesh` takes `/Game` and plugin paths, `define_usd_prim` refuses an unknown
  specifier, `get_sequence_tracks` lists the camera cut track, `create_sound_source_bus` refuses a
  negative duration, and a new audio bus is Stereo as documented.
- Schemas: `interp_mode` of `set_anim_curve_interp_mode` and `rotation` of `rotate_dynamic_mesh` are
  required, as the tools already demanded; `duration_seconds` of `set_gameplay_effect_duration` is
  optional (only `HasDuration` needs it); the six tag lists of
  `set_gameplay_effect_component_target_tag_requirements` are arrays.

## 1.2.0 - 2026-10-15

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
- In an alias chain, the catalog keeps `required` on the first spelling the annotation documents. The
  `data_table` alias of three DataTable tools is documented, and `set_attach_section_rules` requires
  `sequence_name`, its documented spelling, instead of `sequence_path`.
- 27 tools whose reply did not match what the call did now answer with what happened. Failures that
  answered success: `delete_asset` and `delete_level_sequence` fail when the file stays on disk (it
  brings the asset back on restart); the Chaos cut, fracture and cluster tools fail when they add
  nothing, and count the pieces they add (they reported the index of the first one); the tools that
  save fail when the save fails (`create_geometry_collection`, `fracture_geometry_collection`,
  `cluster_geometry_collection`, `map_input_action`, `add_mapping_to_context`,
  `set_meta_sound_node_property`); `configure_plugin_setting`, `set_soundfield_effect_settings` and the
  two Interchange resets fail when nothing was written; `export_assets` and `migrate_assets` measure
  what they wrote, asset by asset. Calls that find the work already done now succeed instead of
  failing: removing a bus or input that is not there, and mapping a key that is already mapped (which
  no longer appends a duplicate). The GAS `create_*` tools say `created: false` for a Blueprint that
  already existed and refuse one of another class at that path.
- 15 more Input and Soundfield tools stop answering success for a failure or a no-op: the Enhanced
  Input tools that edit an InputAction or a mapping context fail when the save fails (the two creates
  also answered with no `success` at all), `remove_input_trigger` and `remove_input_modifier` now
  save (they only marked the context dirty), `set_input_action_flags` with no flag is an error,
  removing a mapping that is not there succeeds without saving, and the runtime key remaps fail when
  the engine refuses them. `create_soundfield_submix`, `add_soundfield_effect_to_submix` and
  `remove_soundfield_effect` state their outcome, and fail when no soundfield effect class is loaded.
- `add_input_trigger` on the InputAction itself is reachable from the tool list: its schema required
  a context and a key, which that form takes neither of. Its `trigger_type`, and the `modifier_type`
  of `add_input_modifier`, are required in the schema instead of being refused by the handler, and a
  key with no context is refused rather than read as the action.
- A call that passes only the other spelling of a required parameter reaches the tool, which accepts
  it: the catalog marks each other spelling of an alias chain `aliasOf` the required one, and the
  server counts any of them, as it does for the dispatcher's aliases. `export_data_table_as_json`
  with only `data_table` was refused with "missing required parameter 'table_path'". That covers 81
  required parameters of 53 tools: the schema no longer lists them as required, and a call with no
  spelling of one is refused naming all of them.
- `codegen_command_catalog.py --check` fails on any difference between the catalog and what `--write`
  would write: it compared only the parameters' names, types, `required` and `aliasOf`, so a
  description or `@returns` edited without `--write` passed with the old text, and the tool reference
  rendered from it passed too. It also fails on a `@returns` that goes on below its line or is given
  twice, because the parser keeps one line and the last `@returns`. Nineteen tools had lost part of
  their returns that way, among them `frame_timing`, `stop_trace`, `capture_pie_state`,
  `spawn_box_volume` and the dry run of `consolidate_assets`; their returns are whole again and match
  the code.
- Every tool publishes MCP annotations: `readOnlyHint`, `destructiveHint`, `idempotentHint` and
  `openWorldHint`. They are derived from the code each tool runs and from its verb
  (`Tools/tool_hints.py`), so a reader that saves, compiles or writes a file is not read-only: 640
  tools are read-only, 403 additive, 1054 destructive, and only `execute_python` and
  `execute_python_file` reach outside the editor. A client can now tell `get_blueprint_info` from
  `delete_asset` without reading the prose.
- Every tool's description ends with what it answers (`Returns: { ... }`), in `tools/list` and in
  `describe_tool`: the catalog had the return shape of 2093 tools, and no client saw it.
- The 125 tool definitions an external review graded lowest were rewritten from their code: what each
  tool does, its side effects, what a wrong input does and which sibling tool to use instead. Several
  said what the code does not do and now match it, among them `get_ability_activation_info` (it reads
  only the asset tags), `set_anim_curve_keys` (it takes `[time, value]` arrays), `setup_foot_ik` (it
  binds no bone) and the `binding_name_or_guid` of 17 Sequencer tools (a GUID, never a name). No
  tool, parameter, type or accepted value changed.

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
