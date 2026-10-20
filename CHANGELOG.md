# Changelog

Notable changes to UEAgent, newest first. Versions follow [Semantic Versioning](https://semver.org).

## Unreleased

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
