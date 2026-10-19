# Migrating to 2.0

UEAgent 2.0 has 1742 tools instead of 2097. Near-duplicate tools were folded into one canonical tool, tools that
could never do anything were removed, and every name now starts with its verb and spells its words out. Nothing
a removed tool did is lost unless the table says so: call the tool in the second column the way the third
column says. The old names are gone, with no aliases.

## Folded into another tool

| Removed | Use | How |
|---|---|---|
| `diff_blueprint_graphs` | `diff_blueprints` |  |
| `get_asset_dependents` | `get_asset_referencers` | with `exclude_engine=true`, `dependency_type="all"`; params `asset_name` → `asset_path`; reply `dependents` → `referencers`, `dependents_count` → `returned`, `total_dependents` → `total`, `asset_name` → `asset_path`, `asset_path` → `resolved_package`; The removed tool defaulted to exclude_engine true and dependency_type all. Per referencer asset_type -> asset_class. resolved_package is the PACKAGE (no .Object suffix). An ambiguous bare name is now an error listing the candidates instead of the first /Game match. |
| `get_blueprint_components` | `get_blueprint_components` | with `include_native=false`, `include_inherited=false`; reply `component_count` → `components_count`; The NAME now belongs to the former get_blueprint_scs_tree; include_native=false + include_inherited=false limits it to the Blueprint's own SCS as the removed tool did. Per component: type -> class, parent -> attach_parent, location/rotation/scale -> transform.loc/rot/scale; children, attach_socket, parent_is_native and is_scene_component are unchanged. |
| `get_blueprint_components_list` | `get_blueprint_components` | with `include_native=false`, `include_inherited=false`; drop `include_inherited` |
| `list_meta_sound_references` | `get_asset_referencers` | with `include_soft_references=true`; params `metasound_path` → `asset_path`; reply `metasound_path` → `asset_path`, `count` → `total`, `references` → `referencers`; references[].package_name -> referencers[].package_name. The removed tool listed hard and soft referencers (the default Package query), hence include_soft_references true. get_asset_referencers does not check that the asset is a MetaSound Source, and caps the list at max_results (default 5000). |
| `add_anim_bone_track` | `set_anim_bone_track_keys` | Call without keys: num_keys identity keys are written (default 2), identity_keys=true. num_keys below 1 is now refused (the removed tool clamped it to 1). |
| `add_pose_search_roled_skeleton` | `set_pose_search_schema_skeleton` | reply `skeleton_path` → `new_skeleton_path`; A role that already has an entry was a no-op in the removed tool (added false, nothing saved); the canonical tool REPLACES that entry's skeleton (and its mirror table when mirror_data_table_path is given) and saves, still without pushing a duplicate entry. role is optional in the canonical tool (empty = default role). |
| `add_rig_control` | `add_rig_element` | with `element_type="control"`; params `control_name` → `element_name`; control_name is also accepted as an alias of element_name, and element_type may be left out when control_type is given. parent_type still defaults to Null for a control. |
| `add_rig_null` | `add_rig_element` | with `element_type="null"`; params `null_name` → `element_name`; null_name is also accepted as an alias of element_name. parent_type still defaults to Bone for a null. rotation and scale are now honoured for a null as well (they default to identity, as before). |
| `clear_motion_warping_targets` | `remove_motion_warping_target` | with `all=true`; params `actor_name` → `actor_path`; reply `new_count` → `warp_target_count`, `actor_name` → `actor_path`; actor_name is also accepted as an alias of actor_path, so the rename is optional. The reply's actor_path is the actor's full object path, not the bare object name the removed tool echoed as actor_name. |
| `get_anim_curve_names` | `get_anim_sequence_info` | params `sequence_path` → `anim_sequence_path`; reply `sequence_path` → `anim_sequence_path`, `bone_track_count` → `num_bone_tracks`, `float_curve_count` → `num_float_curves`; sequence_path is also accepted as an alias. |
| `get_animation_blueprint_info` | `get_anim_blueprint_info` | params `blueprint_name` → `anim_blueprint_path`; drop `load_full_data`; reply `blueprint_path` → `asset_path`, `blueprint_class` → `asset_class`, `has_target_skeleton` → `target_skeleton_set`; The canonical tool always loads the asset (load_full_data dropped); a load failure is an error, so loaded_successfully/is_valid/load_error are gone. blueprint_name (the echo of the request) is not returned. generated_class is the class NAME, not its path. A short name several anim blueprints share is now refused with the candidates (the removed tool took the first match). |
| `get_ik_retarget_chain_details` | `get_ik_rig_info` | reply `start_bone` → `chains.0.start_bone`, `end_bone` → `chains.0.end_bone`, `goal_name` → `chains.0.goal_name`; With found=false the chains list is empty, so chains.0.* is absent where the removed tool answered "None" for each field. |
| `get_ik_solver_bones` | `get_ik_solver_details` |  |
| `get_ik_solver_settings` | `get_ik_solver_details` | reply `ik_rig_path` → `asset_path`, `solver_type` → `solver_type_canonical`; solver_type of the canonical tool is the solver struct name (IKRigFullBodyIKSolver); the removed tool's solver_type (FullBodyIK, Limb, ...) is solver_type_canonical. The out-of-range error no longer lists the valid range. |
| `get_notify_track_names` | `get_anim_notifies` |  |
| `get_skeleton_asset_info` | `get_skeleton_info` | with `load_full_data=true`; params `skeleton_path` → `skeleton_name`; reply `asset_path` → `skeleton_path`, `bones` → `bones[].name`; bones is now up to 100 objects {name, index, parent_index} (bones_truncated) instead of the first 10 names. skeleton_path is also accepted as an alias of skeleton_name. The skeleton is resolved through the Asset Registry (object path, package path, or a short name under /Game; an ambiguous short name is an error). |
| `is_skeleton_shared` | `get_skeleton_info` | params `skeleton_path` → `skeleton_name`; reply `asset_path` → `skeleton_path`; The meshes are reported on every call, without loading the skeleton. skeleton_path is also accepted as an alias of skeleton_name. |
| `list_anim_state_machines` | `get_anim_blueprint_info` | reply `count` → `state_machine_count`; state_machines entries keep name and graph_name and add state_count. |
| `list_warp_targets_on_component` | `inspect_motion_warping_component` |  |
| `scaffold_aim_offset_blend_space` | `create_blend_space` | with `blend_space_type="AimOffset"`; params `asset_name` → `name`, `yaw_min` → `axis_min`, `yaw_max` → `axis_max`, `pitch_min` → `y_axis_min`, `pitch_max` → `y_axis_max`, `num_grid_points_yaw` → `axis_grid`, `num_grid_points_pitch` → `y_axis_grid`; reply `yaw_min` → `axis_min`, `yaw_max` → `axis_max`, `pitch_min` → `y_axis_min`, `pitch_max` → `y_axis_max`, `num_grid_points_yaw` → `axis_grid_num`, `num_grid_points_pitch` → `y_axis_grid_num`; Same defaults as before (Yaw and Pitch -90..90), now written to the asset. A failed save is now success=false (the removed tool succeeded with saved=false). An existing asset is returned unchanged with saved=false instead of being re-saved. |
| `scaffold_blend_space_2d` | `create_blend_space` | with `blend_space_type="BlendSpace"`; params `asset_name` → `name`, `axis_x_label` → `axis_name`, `axis_y_label` → `y_axis_name`, `min_x` → `axis_min`, `max_x` → `axis_max`, `min_y` → `y_axis_min`, `max_y` → `y_axis_max`, `num_grid_points_x` → `axis_grid`, `num_grid_points_y` → `y_axis_grid`; reply `axis_x_label` → `axis_name`, `axis_y_label` → `y_axis_name`, `min_x` → `axis_min`, `max_x` → `axis_max`, `min_y` → `y_axis_min`, `max_y` → `y_axis_max`, `num_grid_points_x` → `axis_grid_num`, `num_grid_points_y` → `y_axis_grid_num`; The axes are now WRITTEN, not echoed. Defaults differ: the removed tool echoed X label 'X' and Y label 'Y'; create_blend_space names X 'Speed' and keeps the engine's 'None' Y label unless y_axis_name is given (ranges 0..100 either way). note is no longer returned. An existing asset is returned unchanged with saved=false instead of being re-saved. |
| `set_anim_curve_keys` | `add_anim_float_curve` | reply `note` → `message`; add_anim_float_curve creates a missing curve instead of refusing it, and writes [[0,0],[1,1]] when keys is empty or omitted (the removed tool refused both). curve_type keeps its name: float (default) is written, transform is still refused with a pointer to add_anim_transform_curve, any other value is an error. num_keys is now the number of keys sent, not a read-back (get_anim_curve_data reads them back). |
| `set_anim_montage_rate_scale` | `set_montage_properties` | reply `applied` → `changed`; name stays accepted as an alias of montage_path. A call with neither rate_scale nor blend_out_trigger_time succeeded with applied=false; set_montage_properties refuses a call that supplies no field. |
| `set_montage_blend_times` | `set_montage_properties` | reply `applied` → `changed`; A call with neither blend_in nor blend_out succeeded with applied=false; set_montage_properties refuses a call that supplies no field. |
| `set_motion_warping_target_location` | `add_motion_warping_target` | params `actor_name` → `actor_path`, `warp_target_name` → `target_name`; reply `new_count` → `warp_target_count`, `warp_target_name` → `target_name`, `actor_name` → `actor_path`, `location_set` → `success`; actor_name and warp_target_name are accepted as aliases, so the renames are optional. location without rotation sets rotation [0,0,0] in both tools (source 'location'); location + rotation gives source 'location_and_rotation'. |
| `set_motion_warping_target_rotation` | `add_motion_warping_target` | params `actor_name` → `actor_path`, `warp_target_name` → `target_name`; reply `previous_pitch` → `previous_rotation.0`, `previous_yaw` → `previous_rotation.1`, `previous_roll` → `previous_rotation.2`, `new_pitch` → `rotation.0`, `new_yaw` → `rotation.1`, `new_roll` → `rotation.2`, `warp_target_name` → `target_name`, `actor_name` → `actor_path`; Pass rotation alone (no location, no component_path): the existing target keeps its Location (source 'rotation'); a missing target is an error, as before. The removed tool treated an omitted rotation as [0,0,0]: add "rotation": [0,0,0] when the call had none (a call with neither location nor rotation is refused). rotation.* is read back from the stored target. |
| `set_skeletal_mesh_allow_cpu_access` | `set_skeletal_mesh_lod_settings` | with `lod_index="all"`; params `b_allow` → `allow_cpu_access`; reply `b_allow` → `settings_applied.allow_cpu_access`; b_allow defaulted to true in the removed tool: add "allow_cpu_access": true when the call had no b_allow. With lod_index "all" (or -1) the write goes through USkeletalMeshEditorSubsystem::SetAllowCPUAccess, as before. |
| `set_skeletal_mesh_lod_screen_size` | `set_skeletal_mesh_lod_settings` | reply `screen_size` → `settings_applied.screen_size`, `applied` → `saved`; The canonical tool saves the mesh (the removed one only dirtied it); a failed save is an error. lod_index was required by the removed tool and defaults to 0 here. |
| `set_skeletal_mesh_section_recompute_tangent` | `set_skeletal_mesh_section_flags` | recompute_tangent defaulted to true in the removed tool, but the canonical tool needs at least one flag: add "recompute_tangent": true when the call had none. |
| `set_skeletal_mesh_section_visible_in_ray_tracing` | `set_skeletal_mesh_section_flags` | params `b_visible` → `visible_in_ray_tracing`; reply `b_visible` → `visible_in_ray_tracing`; b_visible is also accepted as an alias, so the rename is optional. It defaulted to true in the removed tool, but the canonical tool needs at least one flag: add "visible_in_ray_tracing": true when the call had no b_visible. |
| `validate_anim_state_machine` | `get_anim_state_machine_info` |  |
| `wire_aim_offset` | `create_blend_space` | with `blend_space_type="BlendSpace1D"`; params `asset_name` → `name`, `min_value` → `axis_min`, `max_value` → `axis_max`, `num_grid_points` → `axis_grid`; reply `min_value` → `axis_min`, `max_value` → `axis_max`, `num_grid_points` → `axis_grid_num`; The axis is now WRITTEN, not echoed. wire_aim_offset's echo defaults were axis 'Yaw' -90..90 with 4 points; create_blend_space BlendSpace1D defaults to 'Speed' 0..100 with the engine's 4, so a call that relied on those defaults must add axis_name='Yaw', axis_min=-90, axis_max=90 (a real 1D aim offset: blend_space_type='AimOffset1D', which defaults to Yaw -90..90). wire_aim_offset sorted min/max and clamped the grid to 1-10; create_blend_space refuses axis_min >= axis_max and an axis_grid below 1 or non-integer. skeleton_path is optional (default the engine SkeletalCube_Skeleton). An existing asset is returned unchanged with saved=false instead of being re-saved. |
| `clear_and_fill_render_target` | `clear_render_target` | with `color=[1, 1, 1, 1]`; reply `drawn` → `cleared`, `canvas_width` → `width`, `canvas_height` → `height`, `fill_color` → `clear_color`; Add color [1,1,1,1] ONLY when the old call omitted color (clear_and_fill defaulted to white, clear_render_target defaults to black); a given color passes through. The fill now goes through ClearRenderTarget2D instead of a canvas DrawTile: same solid result, and like before it needs a render-target resource (fails under -nullrhi). |
| `draw_material_to_render_target_region` | `draw_material_to_render_target` | with `position=[0, 0]`; Add position [0,0] ONLY when the old call gave neither position nor size, so the call keeps the canvas rectangle path; position/size pass through otherwise. |
| `get_asset_disk_size` | `get_asset_file_info` | params `asset_path` → `asset_name`; reply `asset_path` → `asset_name`, `size_bytes` → `file_size_bytes`, `size_kb` → `file_size_kb`, `size_mb` → `file_size_mb`, `exists_on_disk` → `file_exists`; A package or object path is stat'ed even when the Asset Registry does not list it, with the .umap fallback, as before. |
| `get_asset_last_modified` | `get_asset_file_info` | A package with no file on disk is now success with file_exists=false and a message (and no last_modified) instead of an error. |
| `get_asset_metadata` | `get_asset_by_name` | with `include_tags=true`, `load_full_data=false`; reply `asset_class_path` → `class`, `package_path` → `package`; asset_name was a path RELATIVE to /Game with '/Game/' prepended by the tool: prefix it with '/Game/' (e.g. 'MyGame/X/BP.BP' -> '/Game/MyGame/X/BP.BP'); get_asset_by_name accepts the object path. Old `class` was the short class name of the loaded object; get_asset_by_name `class` is the full AssetClassPath (what asset_class_path held). The asset is no longer loaded. |
| `get_assets_metadata_batch` | `get_assets_by_names` | with `include_tags=true`, `load_full_data=false`; params `asset_paths` → `asset_names`; reply `results` → `resolved`, `results[].asset_path` → `resolved[].asset_name`, `results[].asset_class_path` → `resolved[].class`, `results[].package_path` → `resolved[].package`; Each asset_paths entry was RELATIVE to /Game: prefix '/Game/'. max_results passes through, but an out-of-range value is now refused instead of clamped, and omitting it processes every entry (old default 500): add max_results 500 to reproduce the old cap. |
| `get_render_target_2d_array_info` | `get_render_target_info` | render_target_class says which render-target class was read. |
| `get_render_target_cube_info` | `get_render_target_info` |  |
| `get_static_mesh_lightmap` | `get_static_mesh_info` |  |
| `get_static_mesh_sockets` | `get_static_mesh_info` | reply `socket_count` → `num_sockets`; num_sockets counts Mesh->Sockets including null slots; socket_count counted the listed (non-null) sockets, i.e. the length of sockets. |
| `get_texture_array_info` | `get_texture_info` | params `texture_array_path` → `texture_path`; reply `name` → `texture_name`, `size_x` → `width`, `size_y` → `height`; texture_class is UTexture2DArray for an array. |
| `import_asset` | `import_texture_file` | params `source_file_path` → `file_path`, `destination_path` → `package_path`; drop `replace_existing`; reply `source_file_path` → `file_path`, `destination_path` → `package_path`, `imported_object_paths` → `asset_path`; imported_object_paths was a one-element array; asset_path is that string. replace_existing is dropped because import_asset ignored it and always overwrote, which is import_texture_file's default; import_texture_file now honours replace_existing=false (refuses when the destination holds an asset). Non-image extensions are still refused up front. |
| `list_meta_sounds` | `get_assets_by_type` | with `asset_type="MetaSoundSource"`; reply `sounds` → `assets`; Each entry keeps path, name and class (class is the full AssetClassPath in both). get_assets_by_type caps at max_results (default 1000; list_meta_sounds was uncapped): pass max_results 50000 to keep every result, and read total_found / results_truncated. |
| `set_texture_sampler_settings` | `set_texture_settings` | Same params (address_x, address_y, filter, mip_gen_settings, lod_bias, max_texture_size, flip_green) and the same read-back fields and *_warning fields; success still follows the save. |
| `create_modulation_generator_lfo` | `create_modulation_generator` | with `generator_class="LFO"`; reply `waveform` → `settings.shape`, `frequency` → `settings.frequency`, `amplitude` → `settings.amplitude`, `offset` → `settings.offset`; generator_name and package_path (default /Game/MCP) are accepted as they are. The removed tool wrote frequency 1.0, amplitude 1.0, offset 0.0 and waveform Sine whenever they were omitted, overwriting an existing LFO's values; the canonical writes only the LFO fields given (a new LFO starts at the same engine defaults), and with none given an existing generator is returned untouched, not saved. Pass all four to reproduce the overwrite. settings.shape is the read-back enum string (e.g. ESoundModulationLFOShape::Square), not an echo of waveform. An unknown waveform is now an error (it silently became Sine). |
| `get_meta_sound_node_input_default` | `get_metasound_node_info` | reply `type` → `inputs.0.type_name`, `has_value` → `inputs.0.has_value`, `literal_type` → `inputs.0.literal_type`, `value` → `inputs.0.value`; input_name is now an optional filter; with it, inputs[] holds only that input, so index 0 is it. metasound_path, node_id and input_name keep their names. An unknown input_name is still an error listing the inputs. |
| `get_metasound_graph_info` | `get_metasound_info` | Same field names: asset_path, graph_class_name, node_count, connection_count, variable_count, input_count, output_count, inputs, outputs. |
| `get_metasound_preset_parameters` | `get_metasound_info` | with `include_defaults=true`; params `preset_path` → `metasound_path`; reply `preset_path` → `asset_path`, `parameters` → `inputs`, `parameter_count` → `input_count`; parameters[].{name,type,value} -> inputs[].{name,type,value}; value keeps the builder's literal string, "(inherited)" when the builder has none. |
| `get_sound_class_hierarchy` | `get_sound_class_properties` | reply `parent` → `parent_class`, `parent_path` → `parent_class_path`, `children` → `child_classes[].name`; children were names; child_classes are {name, path} objects. child_count counts only the children listed (null references skipped), where the removed tool also counted null ones. name and sound_class_path keep their names. |
| `set_metasound_preset_parameter` | `set_metasound_graph_input_default` | params `preset_path` → `metasound_path`, `parameter_name` → `input_name`; reply `preset_path` → `asset_path`, `parameter_name` → `input_name`, `success` → `value_set`; parameter_type and value keep their names and encoding (value in the JSON type parameter_type names). An absent or empty parameter_type was float in the removed tool; in the canonical it selects the typed-field mode, where value is also read as a float, so the result is the same. preset_path/parameter_name are also still accepted as aliases. changed, verified, value_read_back, saved, save_error and note keep their names. |
| `set_subtitle_cue_text` | `set_subtitle_cue` | asset_path, cue_index and text keep their names; text is optional in the canonical (the removed tool refused a call without it). The reply's text is the cue's text as stored, present when text was given, with applied_text true. |
| `add_bt_task_child` | `add_behavior_tree_child` | task_class selects a task child; do not pass composite with it (both is an error). |
| `get_blackboard_component` | `get_blackboard_data` | keys is now always present (empty when the component has no asset); key_count and entries are added. |
| `get_blackboard_keys` | `get_blackboard_data` | Precedence differs when BOTH params are given: get_blackboard_keys read the actor, get_blackboard_data reads the asset -- drop blackboard_path from such calls. With actor_name, an actor with no blackboard asset gives keys [] as before; the reply also has entries (current values) and the component fields. |
| `get_blackboard_keys_asset` | `get_blackboard_data` | reply `blackboard_path` → `blackboard_asset`; name, parent, keys and key_count are identical. |
| `remove_bt_task_child` | `remove_behavior_tree_child` | with `kind="task"`; kind=task keeps the old matching exactly (a composite of the same name untouched, an index on a composite edge refused). |
| `add_actor_begin_overlap_event` | `add_graph_node` | with `node_type="event"`, `event_name="ReceiveActorBeginOverlap"`, `compile=true` |
| `add_branch_node` | `add_graph_node` | with `node_type="branch"`, `compile=true` |
| `add_break_struct_node` | `add_graph_node` | with `node_type="break_struct"` |
| `add_call_library_function` | `add_graph_node` | with `node_type="call_function"`, `compile=true`; The function is looked up STRICTLY on function_library (no fallback to other libraries); function_library/function come back as before. |
| `add_cast_node` | `add_graph_node` | with `node_type="cast"`, `compile=true`; params `target_class_name` → `target_class` |
| `add_custom_event_node` | `add_graph_node` | with `node_type="custom_event"`, `compile=true` |
| `add_get_component_by_class_node` | `add_graph_node` | with `node_type="get_component_by_class"`; params `component_class` → `target_class` |
| `add_macro_instance_node` | `add_graph_node` | with `node_type="macro_instance"`; params `macro` → `macro_graph_name`; When macro_graph was given, macro was the macro LIBRARY path: pass it as macro_library and macro_graph as macro_graph_name (a mechanical rename cannot tell). "<library>:<Graph>" and bare graph names in macro work unchanged as macro_graph_name. |
| `add_make_struct_node` | `add_graph_node` | with `node_type="make_struct"` |
| `add_select_node` | `add_graph_node` | with `node_type="select"` |
| `add_sequence_node` | `add_graph_node` | with `node_type="sequence"`, `compile=true` |
| `create_actor_action_utility` | `create_editor_utility_blueprint` | with `parent_class="ActorActionUtility"` |
| `create_asset_action_utility` | `create_editor_utility_blueprint` | with `parent_class="AssetActionUtility"` |
| `create_game_instance_class_bp` | `create_blueprint` | with `path="/Game/MCP"`, `save=false`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`, `package_class` → `generated_class`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `create_game_mode_class_bp` | `create_blueprint` | with `path="/Game/MCP"`, `save=false`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`, `package_class` → `generated_class`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `create_game_state_class_bp` | `create_blueprint` | with `path="/Game/MCP"`, `save=false`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`, `package_class` → `generated_class`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `create_hud_class_bp` | `create_blueprint` | with `path="/Game/MCP"`, `save=false`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`, `package_class` → `generated_class`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `create_player_controller_class_bp` | `create_blueprint` | with `path="/Game/MCP"`, `save=false`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`, `package_class` → `generated_class`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `create_player_state_class_bp` | `create_blueprint` | with `path="/Game/MCP"`, `save=false`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`, `package_class` → `generated_class`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `create_subsystem_class_bp` | `create_blueprint` | with `path="/Game/MCP"`; params `asset_name` → `name`, `package_path` → `path`; reply `asset_name` → `name`, `asset_path` → `path`; package_path in the reply is now the asset's PACKAGE (folder + name), not the folder. |
| `get_blueprint_functions_list` | `get_blueprint_functions` | with `include_ubergraphs=true`; params `include_ubergraph_pages` → `include_ubergraphs`; reply `functions_count` → `function_count`, `function_graphs_count` → `own_function_graph_count`, `event_graphs_count` → `event_graph_count`, `macro_graphs_count` → `macro_graph_count`, `interface_graphs_count` → `interface_graph_count`, `ubergraph_pages_count` → `ubergraph_count`; include_ubergraph_pages defaulted to true, so include_ubergraphs:true is added. Per entry: type -> graph_type with lower-case values (Function->function, Macro->macro, Interface->interface, Event->event_graph, Ubergraph->ubergraph), nodes_count -> node_count. The canonical also lists delegate_signature graphs. |
| `get_blueprint_variables_list` | `get_blueprint_variables` |  |
| `get_compile_errors` | `compile_blueprint` | with `return_messages=true`; name, num_errors, num_warnings and succeeded keep their names; messages[].message -> messages[].text (severity unchanged; node_id, node_title and graph_name are new). |
| `get_node_pin_info` | `get_graph_nodes` | reply `node_id` → `nodes.0.node_id`, `class` → `nodes.0.class`, `title` → `nodes.0.title`, `pos_x` → `nodes.0.pos_x`, `pos_y` → `nodes.0.pos_y`, `comment` → `nodes.0.comment`, `pins` → `nodes.0.pins` |
| `implement_interface_function` | `add_blueprint_function` | with `override=true` |
| `override_interface_member` | `add_blueprint_function` | with `override=true`; params `member_name` → `function_name`; drop `interface_path`, `position_x`, `position_y` |
| `remove_blueprint_component_node` | `remove_component_from_blueprint` |  |
| `reparent_blueprint` | `reparent_blueprint` | The NAME now belongs to the former reparent_blueprint_safe (guarded, auto_rollback default true, never saves). Asking for the parent it already has is now a no-op with reparented false, and a failed post-check rolls back unless auto_rollback=false. |
| `set_blueprint_component_property` | `set_blueprint_component_properties` | reply `component_name` → `results.0.component_name`, `property_name` → `results.0.property_name`, `component_class` → `results.0.component_class`, `source` → `results.0.source`, `override_persisted` → `results.0.override_persisted` |
| `get_camera_fov` | `get_cameras` | reply `fov` → `cameras.0.fov`; camera_name passes through as the filter; an unknown camera is still an error, and a camera without a camera component still reports fov 90. |
| `get_camera_transform` | `get_cameras` | reply `location` → `cameras.0.location`, `rotation` → `cameras.0.rotation`; camera_name passes through as the filter. |
| `evaluate_chooser_table_multi` | `evaluate_chooser_table` | with `all_results=true`; Errors name evaluate_chooser_table instead of evaluate_chooser_table_multi. |
| `set_physics_gravity` | `set_project_settings_value` | with `class_name="PhysicsSettings"`, `property_name="DefaultGravityZ"`; params `gravity_z` → `value`; reply `gravity_z` → `round_tripped_value`; value is a string: pass the number as text ("-980"); round_tripped_value is the property's text ("-980.000000"), not a number. |
| `add_data_table_row` | `set_data_table_row` | with `create_if_missing=true`; Same reply fields (updated_existing, columns_written, row_count, saved, row_data, unknown_columns). |
| `clear_data_registry_sources` | `remove_data_registry_source` | with `all=true`; reply `cleared` → `removed`; A failed save now fails the call (success false, saved false, save_error) instead of answering success with save_error. |
| `get_active_culture` | `get_culture_info` | Call with no culture_name; is_active comes back true. |
| `get_data_registry_source_count` | `get_data_registry_info` | source_count now counts every DataSources[] slot, null ones included, as get_data_registry_source_count did. |
| `get_data_table_row_as_json` | `get_data_table_row` | with `include_native_json=true` |
| `get_string_table_metadata` | `get_string_table_entries` | reply `metadata` → `entries.0.metadata`, `metadata_count` → `entries.0.metadata_count`; key passes through as the filter. A missing key gives entry_exists=false and an empty entries list (the old tool still returned an empty metadata object). |
| `list_data_registry_sources` | `get_data_registry_info` | Each source keeps index, class ('null' for a null slot) and source_table (now read for soft and hard SourceTable properties; the old tool never read the soft TSoftObjectPtr of a DataTable source), plus name and data_table. index is now the true DataSources[] slot and null slots are listed. |
| `read_data_table_row_complex` | `get_data_table_row` | with `include_export_text=true`; params `data_table` → `table_path`; read_data_table_row_complex accepted table_path as an alias of data_table; when both were given data_table won. |
| `add_light` | `spawn_actor` | params `light_type` → `type`; reply `light_type` → `class`, `actor_name` → `name`; light_type echoed the input ('Point'); spawn_actor's class is the class name ('PointLight'). spawn_actor is undoable and takes world (default editor); add_light was editor-only and not undoable. |
| `can_redo` | `get_undo_state` | reply `label` → `next_redo`, `reason` → `redo_reason` |
| `can_undo` | `get_undo_state` | reply `label` → `next_undo`, `reason` → `undo_reason` |
| `clear_undo_barriers` | `remove_undo_barrier` | with `all=true`; reply `had_barrier` → `removed` |
| `configure_plugin_setting` | `set_engine_ini_value` | params `setting_name` → `key`, `setting_value` → `value`; reply `setting_name` → `key`, `setting_value` → `value`, `configuration_success` → `applied`, `config_file_used` → `config_file_path`; A read-back mismatch is now success with applied false (it was success false). configuration_message and configuration_timestamp are gone; config_file_path is the saved .ini path (it was the GConfig key "Engine"). |
| `disable_plugin` | `set_plugin_enabled` | with `enabled=false` |
| `get_allow_cinematic_control` | `get_viewport_state` |  |
| `get_near_clip_plane` | `get_viewport_state` |  |
| `get_view_type` | `get_viewport_state` |  |
| `get_viewport_exposure` | `get_viewport_state` | reply `fixed` → `exposure.fixed`, `ev100` → `exposure.ev100` |
| `get_viewport_fov` | `get_viewport_state` |  |
| `list_cvars_with_prefix` | `list_cvars` | with `include_commands=true`, `max_results=100`; params `max` → `max_results` |
| `new_level` | `load_level` | with `default_template=true`; drop `level_name`; reply `template_package` → `target_package`; level_name (ignored by new_level) is dropped, and with it level_name_ignored, requested_level_name and message; loaded_package, is_engine_template, warning, level_name and world_name keep their names. load_level shows the editor's progress dialog where new_level did not. The template is opened with default_template=true: a filename under /Engine/ is refused by the load_level precondition. |
| `redo_multiple` | `redo` | count below 1 is now refused (redo_multiple clamped it to 1 silently); redo also returns applied, can_redo and next_redo |
| `search_cvars` | `list_cvars` | with `include_commands=true`, `max_results=50`; params `term` → `filter`, `max` → `max_results`; filter follows the engine's console search: a term with spaces matches names containing every word, and a one-character term matches as a prefix (search_cvars then also required the whole term as a substring). |
| `set_packaging_setting` | `set_engine_ini_value` | with `section="/Script/UnrealEd.ProjectPackagingSettings"`, `config_file="Game"`; params `setting` → `key`; reply `setting` → `key`, `verified` → `applied`, `applied` → `value`, `read_back` → `readback_value`, `ini_file` → `config_file_path`; applied is now the read-back verdict (the old verified); the old applied was the written text (true for a bool) and is now value, which for a bool is the text True/False, as is readback_value (the old read_back was a bool). config_file_path is the saved .ini path where ini_file was the GConfig key. |
| `start_gauntlet_run` | `run_automation_test` | gauntlet_started / gauntlet_subprocess / automation_equivalent were constants (true/false/true) and the bracketed "not an actual Gauntlet subprocess" text is no longer appended to note. |
| `undo_multiple` | `undo` | count below 1 is now refused (undo_multiple clamped it to 1 silently); undo also returns applied, can_undo and next_undo |
| `add_niagara_decal_renderer` | `add_niagara_renderer` | with `renderer_type="Decal"`; reply `source_mode` → `properties.SourceMode`, `renderer_visibility` → `properties.RendererVisibility`, `decal_screen_size_fade` → `properties.DecalScreenSizeFade`; source_mode, renderer_visibility and decal_screen_size_fade move into the object param `properties` as decal_source_mode, renderer_visibility and decal_screen_size_fade (material_path stays a top-level param). An unknown source mode is refused as a value that does not convert, still with nothing created. |
| `add_niagara_emitter_handle` | `add_niagara_emitter_from_template` | params `emitter_path` → `emitter_asset_path` |
| `add_niagara_light_renderer` | `add_niagara_renderer` | with `renderer_type="Light"`; reply `radius_scale` → `properties.RadiusScale`, `default_exponent` → `properties.DefaultExponent`, `specular_scale` → `properties.SpecularScale`, `diffuse_scale` → `properties.DiffuseScale`, `color_add_x` → `properties.ColorAdd`, `color_add_y` → `properties.ColorAdd`, `color_add_z` → `properties.ColorAdd`; The light settings move into the object param `properties` with the same keys (use_inverse_squared_falloff, affects_translucency, allow_mega_lights, mega_lights_cast_shadows, alpha_scales_brightness, radius_scale, default_exponent, specular_scale, diffuse_scale, color_add); color_add becomes an object {"X": r, "Y": g, "Z": b} instead of an [r, g, b] array. A Light is seeded with affects_translucency and alpha_scales_brightness true, as the removed tool seeded it. The read-back values are in properties under the UPROPERTY names (RadiusScale, DefaultExponent, SpecularScale, DiffuseScale, ColorAdd as text "X=.. Y=.. Z=.."). |
| `add_niagara_module_to_graph` | `add_niagara_module_to_stack` | reply `added` → `success` |
| `get_frame_count` | `get_editor_status` | reply `frame_count` → `frame_counter` |
| `get_game_instance_info` | `get_engine_info` | reply `exists` → `game_instance_exists` |
| `get_niagara_data_interface_info` | `get_niagara_data_interfaces` | reply `parameter_name` → `data_interfaces.0.parameter_name`, `data_interface_class` → `data_interfaces.0.data_interface_class`, `data_interface_class_path` → `data_interfaces.0.data_interface_class_path`, `path_name` → `data_interfaces.0.path_name`, `properties` → `data_interfaces.0.properties`, `property_count` → `data_interfaces.0.property_count`; Without parameter_name the removed tool was an error (missing parameter); get_niagara_data_interfaces then lists every data interface. |
| `get_niagara_emitter_handle_info` | `get_niagara_emitter_handles` | reply `emitter_name` → `emitter_handles.0.name`, `emitter_id` → `emitter_handles.0.id`, `emitter_id_name` → `emitter_handles.0.id_name`, `is_enabled` → `emitter_handles.0.is_enabled`, `is_valid` → `emitter_handles.0.is_valid`, `emitter_version` → `emitter_handles.0.emitter_version`, `is_local_space` → `emitter_handles.0.is_local_space`, `is_deprecated` → `emitter_handles.0.is_deprecated`; With neither emitter_name nor emitter_index the removed tool was an error; get_niagara_emitter_handles then lists every handle. |
| `get_niagara_emitter_sim_target` | `get_niagara_emitter_runtime_summary` | reply `is_local_space` → `local_space`, `is_deterministic` → `deterministic` |
| `get_niagara_module_info` | `list_niagara_modules` | reply `module_name` → `modules.0.module_name`, `node_guid` → `modules.0.node_guid`, `stack_index` → `modules.0.index`, `function_script` → `modules.0.function_script`, `function_script_class` → `modules.0.function_script_class`, `is_enabled` → `modules.0.is_enabled`, `input_pin_count` → `modules.0.input_pin_count`, `output_pin_count` → `modules.0.output_pin_count`, `is_deprecated` → `modules.0.is_deprecated` |
| `get_niagara_module_stack` | `list_niagara_modules` | with `script_group="all"` |
| `get_niagara_scalability_settings` | `get_niagara_effect_type` | A system with no effect type was an error in the removed tool; get_niagara_effect_type answers success with effect_type_path "" and the defaults (max_distance 0, max_instances 0, cull_by_distance false, scale_spawn_count 1). |
| `get_niagara_system_warmup_time` | `get_niagara_system_info` |  |
| `list_niagara_simulation_stages` | `get_niagara_simulation_stages` |  |
| `set_niagara_decal_renderer_material` | `set_niagara_renderer_material` | reply `new_material_path` → `material_path`; set_niagara_renderer_material accepts any renderer class with a Material property, so a call on a Sprite/Mesh/Ribbon renderer that the removed tool refused (not a Decal renderer) now succeeds; a Light renderer is still refused. |
| `set_niagara_emitter_local_space` | `set_niagara_emitter_sim_target` | reply `applied` → `changed` |
| `export_asset_data` | `generate_asset_report` | with `visible_only=true`; params `export_format` → `report_format`; reply `export_format` → `report_format`, `export_success` → `report_success`, `export_message` → `report_message`, `export_file_path` → `report_file_path`, `export_file_name` → `report_file_name`, `export_file_size_bytes` → `report_file_size_bytes`, `export_file_size_mb` → `report_file_size_mb`, `export_timestamp` → `report_timestamp`, `export_status` → `report_status`, `export_directory` → `report_directory`; The file lands in Saved/Reports/Assets as <Name>_Report_<timestamp> (it was Saved/Exports/Assets/<Name>_<timestamp>); the JSON file's timestamp key is report_timestamp (was export_timestamp). |
| `reset_game_mode_override` | `set_game_mode_override` | with `game_mode_class="None"`; reset, world_name and previous_default_game_mode keep their names; previous_default_game_mode is now the class PATH, not its short name. |
| `set_game_mode_game_state_class` | `set_game_mode_classes` |  |
| `set_game_mode_hud_class` | `set_game_mode_classes` |  |
| `set_game_mode_player_state_class` | `set_game_mode_classes` |  |
| `set_game_mode_spectator_class` | `set_game_mode_classes` |  |
| `create_attribute_set_blueprint` | `create_attribute_set_blueprint` | The empty-set creator is removed and create_attribute_set_asset takes its name; a call without attributes does the same, and the reply adds attributes_added, added/skipped/failed_attribute_names, attributes_applied and saved. Calls need no rewrite. |
| `create_gameplay_cue_notify_actor` | `create_gameplay_cue_notify` | with `notify_type="Actor"`; The reply adds notify_type. |
| `create_gameplay_cue_notify_burst` | `create_gameplay_cue_notify` | with `notify_type="Burst"`; The reply adds notify_type. |
| `create_gameplay_cue_notify_looping` | `create_gameplay_cue_notify` | with `notify_type="Looping"`; The reply adds notify_type. |
| `set_gameplay_cue_notify_burst_properties` | `set_gameplay_cue_notify_static_properties` | The note is emitted when the CDO is a Burst notify. The Static setter does not refuse a plain Static notify (the Burst one did). |
| `set_gameplay_cue_notify_looping_properties` | `set_gameplay_cue_notify_actor_properties` | The note is emitted only for a Looping CDO. The Actor setter does not refuse a plain Actor notify (the Looping one did). |
| `set_gameplay_effect_duration_policy` | `set_gameplay_effect_duration` | params `effect_path` → `ge_blueprint_path`, `duration_magnitude` → `duration_seconds`; reply `effect_path` → `ge_blueprint_path`, `duration_magnitude` → `duration_seconds`; HasDuration without duration_magnitude used 0 seconds; set_gameplay_effect_duration requires duration_seconds for HasDuration, so pass 0 explicitly. Native UGameplayEffect assets and Blueprint GEs are both accepted (native_asset says which); PIE/read-only are refused before the edit and a failed save undoes it. |
| `set_gameplay_effect_modifier_attribute_based_up_to_channel` | `set_gameplay_effect_modifier_attribute_based` | with `calculation_type="AttributeMagnitudeEvaluatedUpToChannel"`; Same params (final_channel default 0) and reply fields. |
| `find_gameplay_tag` | `get_gameplay_tag_info` |  |
| `get_all_gameplay_tags` | `query_gameplay_tags` | reply `tags` → `matching_tags`; Call with no query_tag. Each entry keeps tag_name and tag_string. |
| `get_gameplay_tag_source` | `get_gameplay_tag_info` | reply `valid` → `found`; The editor data is now read under the resolved tag's FName rather than the requested string (same tag). |
| `get_gameplay_tags_manager` | `query_gameplay_tags` | with `count_only=true`; reply `total_tags` → `count`, `is_valid` → `manager_valid`; manager_name is gone (it was always the singleton's name). count counts valid registered tags; total_tags counted every entry of the registry list. |
| `has_all_gameplay_tags` | `has_gameplay_tags` | with `match="all"`; result holds the same boolean. |
| `has_none_gameplay_tags` | `has_gameplay_tags` | with `match="none"`; result holds the same boolean. |
| `has_tag` | `has_gameplay_tags` | params `tag_name` → `tags`; reply `has_tag` → `has_any`; tags is an ARRAY: wrap the old tag_name string, tags: [tag_name]. has_gameplay_tags also resolves objects without IGameplayTagAssetInterface through their first FGameplayTagContainer property (has_tag errored there). tag_name echo -> requested.0. |
| `validate_gameplay_tag_table` | `get_gameplay_tag_table_rows` | missing_tags[].row_name is now the real row key (validate reported the row's DevComment there). table_path is echoed as the loaded path name. |
| `delete_triangle_from_mesh` | `delete_mesh_triangles` | triangle_id is accepted as the one-element triangle_ids; the reply keeps triangle_id, deleted, triangle_count and previous_triangle_count (plus num_deleted, requested_count). |
| `generate_mesh_uvs` | `set_mesh_uvs_from_projection` | with `projection="plane"`, `scale=[1, 1, 1]`; reply `uvs_generated` → `applied`; projection 'plane' with scale [1,1,1] and the default rotation is the same planar projection down mesh-local Z at 1 UV unit per cm; a missing channel is added the same way (uv_channel 0-7). An empty mesh is no longer an error: it succeeds with applied false and a note, unchanged. |
| `get_is_dense_mesh` | `get_dynamic_mesh_info` | reply `actor_name` → `component_name`; The removed tool replied the dynamic mesh component's name as actor_name; get_dynamic_mesh_info replies the actor label as actor_name and the component name as component_name. |
| `get_mesh_bounding_box` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. The bounding box keeps min_*, max_*, extent_* and center_* (center_* is the box centre; the centroid is center_of_mass_*). |
| `get_mesh_connected_components` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_mesh_has_attribute_set` | `get_dynamic_mesh_info` | reply `actor_name` → `component_name`; The removed tool replied the dynamic mesh component's name as actor_name; get_dynamic_mesh_info replies the actor label as actor_name and the component name as component_name. |
| `get_mesh_is_closed` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_mesh_open_border_loops` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_mesh_selection_bounds` | `select_mesh_triangles` | Same region params; the reply keeps region, triangles_selected, is_empty and bounds{...}. |
| `get_mesh_triangle_count` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_mesh_uv_area` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. uv_channel keeps its name (default 0); a negative one is still an error. |
| `get_mesh_uv_channel_count` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_mesh_vertex_count` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_mesh_volume_area` | `get_static_mesh_geometry_stats` | Same params (static_mesh_path, lod_index) and the same reply field names, among many more. |
| `get_num_connected_islands` | `get_dynamic_mesh_info` | reply `num_connected_islands` → `num_connected_components`, `actor_name` → `component_name`; The removed tool replied the dynamic mesh component's name as actor_name; get_dynamic_mesh_info replies the actor label as actor_name and the component name as component_name. |
| `get_num_extended_polygroup_layers` | `get_dynamic_mesh_info` | reply `actor_name` → `component_name`; The removed tool replied the dynamic mesh component's name as actor_name; get_dynamic_mesh_info replies the actor label as actor_name and the component name as component_name. |
| `get_num_open_border_edges` | `get_dynamic_mesh_info` | reply `actor_name` → `component_name`; The removed tool replied the dynamic mesh component's name as actor_name; get_dynamic_mesh_info replies the actor label as actor_name and the component name as component_name. |
| `get_num_uv_islands` | `get_dynamic_mesh_info` | reply `uv_channel` → `uv_channels.0.uv_channel`, `num_uv_islands` → `uv_channels.0.num_uv_islands`, `is_valid_uv_channel` → `uv_channels.0.is_valid_uv_channel`, `actor_name` → `component_name`; uv_channel keeps its name and filters uv_channels to that one channel (without it, every channel is listed). A channel the mesh does not have is answered as before, with is_valid_uv_channel false (a negative one is an error, as before). The removed tool replied the dynamic mesh component's name as actor_name; get_dynamic_mesh_info replies the actor label as actor_name and the component name as component_name. |
| `is_bvh_valid_for_mesh` | `build_mesh_bvh` | bvh_valid is in the reply. build_mesh_bvh FAILS (success false, data.bvh_valid false) when no valid BVH can be built, where the removed tool succeeded with bvh_valid false. |
| `rebuild_mesh_bvh` | `build_mesh_bvh` | Same params (actor_name, b_only_if_invalid); bvh_rebuilt and bvh_valid are in the reply. |
| `rotate_dynamic_mesh` | `transform_dynamic_mesh` | Same rotation param ([pitch, yaw, roll] or {pitch, yaw, roll}) and rotation_* reply fields. A missing rotation is no longer an error: it is the identity (applied false, note). |
| `scale_dynamic_mesh` | `transform_dynamic_mesh` | Same scale param and scale_* reply fields. A missing scale is no longer an error: it is the identity (applied false, note). |
| `set_mesh_material_id_in_region` | `set_mesh_material_id` | with `scope="region"`; Same region params and reply fields (region, triangles_selected, max_material_id_after, distinct_material_ids_after, applied). scope may be omitted when region is given. Differences: the painted triangles are read back and the call fails (reverted) if any did not take the ID; an empty mesh is an error; a region selecting nothing succeeds with applied false and a note. |
| `set_polygroup_material_id` | `set_mesh_material_id` | with `scope="region"`, `region="polygroup"`; reply `polygroup_id_valid` → `triangles_selected`; polygroup_id, polygroup_layer and material_id keep their names. polygroup_id_valid is triangles_selected > 0 (a polygroup with no triangle succeeds with applied false and a note). A polygroup layer the mesh lacks still fails the call. polygroup_id is not echoed in the reply. |
| `translate_dynamic_mesh` | `transform_dynamic_mesh` | params `translation` → `location`; reply `translation_x` → `location_x`, `translation_y` → `location_y`, `translation_z` → `location_z`; translation is also accepted as an alias of location, so the rename is optional. A missing translation is no longer an error: it is the identity (applied false, note). |
| `set_decal_destroy_owner_after_fade` | `set_decal_fade` | with `destroy_owner_after_fade=true`; Add destroy_owner_after_fade true ONLY when the old call omitted it (the old default); a given value passes through. |
| `set_decal_fade_in` | `set_decal_fade` | with `fade_in_start_delay=0`, `fade_in_duration=0`; params `start_delay` → `fade_in_start_delay`, `duration` → `fade_in_duration`; set_decal_fade_in always wrote BOTH fields, defaulting an omitted one to 0: add fade_in_start_delay 0 / fade_in_duration 0 only for the one the old call omitted. Fields are now written directly (as the details panel does) instead of via SetFadeIn: same values. |
| `set_decal_fade_screen_size` | `set_decal_fade` | fade_screen_size is now validated before any write: a non-number, NaN or negative value is an error (the old tool wrote it as given). |
| `get_action_mappings` | `get_input_settings` | reply `count` → `action_mapping_count` |
| `get_available_keys` | `find_free_input_key` | with `bindable_only=false`; reply `count` → `key_count`, `keys` → `free_keys + bound_keys`, `keys[].name` → `free_keys[].display_name / bound_keys[].display_name`, `keys[].display_name` → `free_keys[].display_name / bound_keys[].display_name`; The key list is split in free_keys and bound_keys (concatenate them for get_available_keys' keys). get_available_keys put the DISPLAY name in name (the bug the fix removes): in find_free_input_key name and key are the FKey name binding calls take, display_name is the display name. is_valid is gone: only valid keys are listed. filter also matches the FKey name, not only the display name. |
| `get_axis_mappings` | `get_input_settings` | reply `count` → `axis_mapping_count` |
| `list_input_modifier_types` | `list_input_types` | reply `count` → `modifier_count`, `types` → `modifier_types` |
| `list_input_trigger_types` | `list_input_types` | reply `count` → `trigger_count`, `types` → `trigger_types` |
| `list_input_triggers` | `get_input_mapping_context` | params `ctx_path` → `context_path`; reply `count` → `mappings.0.trigger_count`, `triggers` → `mappings.0.triggers`; Pass mapping_index as before: the reply then holds that one row in mappings[0]. ctx_path is also accepted as an alias, so the rename is optional. context_path may also be a short asset name. |
| `map_input_action` | `add_mapping_to_context` | context_path, action_path, input_action_path, key_name and key are all accepted by add_mapping_to_context as aliases, so a call is renamed only. The reply keeps context_path, action_path (now the resolved object path /Game/X/IA.IA), key_name, index, mapping_count, added, already_mapped, saved. add_mapping_to_context also resolves short asset names. |
| `interchange_get_node_container` | `get_interchange_asset_import_data` | with `include_node_container=true`; Old present was true only with a node container: it is now present AND has_node_container (present alone means the import data exists). |
| `interchange_get_pipelines` | `get_interchange_asset_import_data` | reply `pipeline_count` → `pipelines_count`; pipelines_count is GetNumberOfPipelines(); pipeline_count counted the non-null pipelines listed, i.e. the length of pipelines. |
| `interchange_module_status` | `get_interchange_supported_formats` | reply `asset_formats` → `formats`, `translator_type_queried` → `translator_type`; With no translator_type the listed formats are the Assets ones, as asset_formats were. |
| `build_minimap` | `get_world_partition_info` | reply `is_world_partition` → `is_world_partition_enabled`, `already_has_minimap` → `has_minimap`, `world` → `world_name`; built (always false), success (always true) and message are gone and note has different text; has_minimap is now present (false) on a non-partitioned world too |
| `configure_gravity` | `set_world_gravity` | params `gravity_settings` → `preset`; reply `gravity_settings` → `preset`, `gravity_configured` → `success`, `old_gravity_z` → `previous_gravity_z`, `old_gravity_enabled` → `previous_override`, `new_gravity_z` → `gravity_z`, `new_gravity_enabled` → `enable_override`, `requested_gravity_z` → `applied_gravity_z`; an unknown preset is now an error (success false, nothing written) instead of success with gravity_configured false; the always-zero _x/_y triplet members, gravity_message, requested_gravity_enabled and the world_physics_* timing fields are dropped; gravity_impact/recommendations now rate the effective gravity, so 'Zero' rates as gravity off. The write is now in an undo transaction on the editor world. |
| `get_actor_references` | `get_actor_components` | reply `references` → `components`, `references_count` → `components_count`; each row's constant type:'Component' is gone (get_actor_components rows carry name, class, is_active, is_editable and an optional type of their own) |
| `get_data_layer_asset_info` | `get_data_layer_assets` | reply `asset_path` → `data_layer_assets.0.path`, `name` → `data_layer_assets.0.name`, `data_layer_type` → `data_layer_assets.0.type`, `is_runtime` → `data_layer_assets.0.is_runtime`, `is_private` → `data_layer_assets.0.is_private`, `supports_actor_filters` → `data_layer_assets.0.supports_actor_filters`, `debug_color` → `data_layer_assets.0.debug_color` |
| `get_world_partition_runtime_hash` | `get_world_partition_info` | reply `is_world_partition` → `is_world_partition_enabled`; the payload's constant success:true is gone and note has different text; num_streaming_grids and runtime_hash_type are now present on a non-partitioned world too (0 / the none name) |
| `get_world_settings_detail` | `get_world_settings` |  |
| `has_foliage_instances` | `get_foliage_instance_count` |  |
| `remove_foliage_in_radius` | `remove_foliage_instances` | reply `instances_removed` → `removed_count`; the constant applied:true is gone; the sphere now covers every InstancedFoliageActor of the world, not only the persistent level's |
| `get_post_process_volume_settings` | `get_post_process_settings` | params `volume_name` → `actor_name`; reply `volume_name` → `actor_name`, `white_temp` → `white_balance.white_temp.value`; get_post_process_volume_settings matched the editor label only; get_post_process_settings resolves its actor_name with its own lookup (label or name). |
| `create_editable_skeletal_mesh` | `duplicate_asset` | with `asset_path="/Engine/EngineMeshes/SkeletalCube"`, `if_exists="return"`, `expected_class="SkeletalMesh"`; params `package_path` → `destination_path`, `skeletal_mesh_name` → `new_asset_name`, `source_mesh` → `asset_path`; reply `asset_path` → `new_asset_path`, `skeletal_mesh_name` → `new_asset_name`, `source_mesh` → `original_asset_path`; new_asset_path is the object path (/Game/X/Name.Name) where asset_path was the package path (/Game/X/Name), and original_asset_path is the source as an object path. The copy is now always saved (it was saved only with source control enabled). |
| `create_editable_static_mesh` | `duplicate_asset` | with `asset_path="/Engine/BasicShapes/Cube"`, `if_exists="return"`, `expected_class="StaticMesh"`; params `package_path` → `destination_path`, `static_mesh_name` → `new_asset_name`, `source_mesh` → `asset_path`; reply `asset_path` → `new_asset_path`, `static_mesh_name` → `new_asset_name`, `source_mesh` → `original_asset_path`; new_asset_path is the object path (/Game/X/Name.Name) where asset_path was the package path (/Game/X/Name), and original_asset_path is the source as an object path. |
| `delete_level_sequence` | `delete_asset` | with `expected_class="LevelSequence"`; params `sequence_name` → `asset_path`; reply `actually_deleted` → `deleted`, `delete_attempted` → `delete_success`, `sequence_name` → `asset_path`; sequence_name was an asset NAME searched under /Game (case-insensitive); delete_asset needs the sequence's asset PATH, so the renamed value must become the path. asset_still_exists is the negation of asset_verified_deleted. A name matching nothing was an error; with expected_class a missing path is a no-op. |
| `delete_material_function` | `delete_asset` | with `expected_class="MaterialFunctionInterface"`; params `material_function_path` → `asset_path`; reply `material_function_path` → `asset_path`, `asset_class` → `asset_class_name`; A missing asset was an error; with expected_class it is a no-op success (deleted false, already_existed false). |
| `delete_meta_sound` | `delete_asset` | with `expected_class=["MetaSoundSource", "MetaSoundPatch"]`; params `metasound_path` → `asset_path`; drop `confirm`; reply `metasound_path` → `asset_path`, `asset_class` → `asset_class_name`; confirm is gone: a call that relied on a missing or false confirm being refused now deletes. A MetaSound asset class other than Source or Patch needs its class added to expected_class. |
| `delete_state_tree` | `delete_asset` | with `expected_class="StateTree"`; params `tree_path` → `asset_path`; drop `confirm`; reply `tree_path` → `asset_path`, `asset_class` → `asset_class_name`; confirm is gone: a call that relied on a missing or false confirm being refused now deletes. |
| `duplicate_anim_sequence` | `duplicate_asset` | with `destination_path="/Game/MCP"`, `expected_class="AnimSequence"`; params `source_path` → `asset_path`, `new_name` → `new_asset_name`, `package_path` → `destination_path`; reply `source_path` → `original_asset_path`, `duplicate_path` → `new_asset_path`, `duplicate_class` → `new_asset_class`; new_asset_class is the class path (/Script/Engine.AnimSequence) where duplicate_class was the short name. |
| `get_primary_asset_cook_rules` | `get_primary_asset_types` | reply `cook_rule` → `types.0.cook_rule`, `priority` → `types.0.priority`, `chunk_id` → `types.0.chunk_id`, `apply_recursively` → `types.0.apply_recursively` |
| `get_primary_asset_id_from_path` | `get_primary_asset_id` | params `object_path` → `asset_path`; reply `object_path` → `asset_path` |
| `get_primary_asset_type_info` | `get_all_primary_asset_types` | reply `asset_class` → `types.0.asset_class`, `has_blueprint_classes` → `types.0.has_blueprint_classes`, `is_editor_only` → `types.0.is_editor_only`, `num_assets` → `types.0.num_assets`, `directories` → `types.0.directories` |
| `remove_anim_blueprint` | `delete_asset` | with `expected_class="AnimBlueprint"`; reply `asset_class` → `asset_class_name` |
| `remove_anim_composite` | `delete_asset` | with `expected_class="AnimComposite"`; reply `asset_class` → `asset_class_name` |
| `remove_anim_montage` | `delete_asset` | with `expected_class="AnimMontage"`; reply `asset_class` → `asset_class_name` |
| `remove_anim_sequence` | `delete_asset` | with `expected_class="AnimSequence"`; reply `asset_class` → `asset_class_name` |
| `remove_blend_space` | `delete_asset` | with `expected_class="BlendSpace"`; reply `asset_class` → `asset_class_name` |
| `remove_blendspace_1d` | `delete_asset` | with `expected_class="BlendSpace1D"`; reply `asset_class` → `asset_class_name` |
| `remove_hlod_layer` | `delete_asset` | with `expected_class="HLODLayer"`; reply `asset_class` → `asset_class_name`; layer_type is no longer reported: read it with get_hlod_layer_info before deleting. |
| `remove_pose_asset` | `delete_asset` | with `expected_class="PoseAsset"`; reply `asset_class` → `asset_class_name` |
| `remove_pose_search_database` | `delete_asset` | with `expected_class="PoseSearchDatabase"`; params `database_path` → `asset_path`; reply `database_path` → `asset_path`, `asset_class` → `asset_class_name` |
| `remove_pose_search_schema` | `delete_asset` | with `expected_class="PoseSearchSchema"`; params `schema_path` → `asset_path`; reply `schema_path` → `asset_path`, `asset_class` → `asset_class_name` |
| `remove_render_target_2d` | `delete_asset` | with `expected_class="TextureRenderTarget2D"`; reply `asset_class` → `asset_class_name` |
| `remove_render_target_2d_array` | `delete_asset` | with `expected_class="TextureRenderTarget2DArray"`; reply `asset_class` → `asset_class_name` |
| `remove_render_target_cube` | `delete_asset` | with `expected_class="TextureRenderTargetCube"`; reply `asset_class` → `asset_class_name` |
| `remove_rvt_asset` | `delete_asset` | with `expected_class="RuntimeVirtualTexture"`; reply `asset_class` → `asset_class_name` |
| `remove_static_texture_from_render_target` | `delete_asset` | with `expected_class="Texture2D"`; reply `asset_class` → `asset_class_name` |
| `remove_subsystem_class_bp` | `delete_asset` | with `expected_class="Subsystem"`; reply `asset_class` → `asset_class_name`, `is_subsystem_class` → `expected_class_matched` |
| `remove_subtitles_asset` | `delete_asset` | with `expected_class="SubtitleAssetUserData"`; reply `asset_class` → `asset_class_name` |
| `remove_texture_2d_array` | `delete_asset` | with `expected_class="Texture2DArray"`; reply `asset_class` → `asset_class_name` |
| `count_mass_entities` | `list_mass_archetypes` | reply `per_archetype` → `archetypes`, `per_archetype[].count` → `archetypes[].entity_count`, `per_archetype[].archetype_id` → `archetypes[].archetype_id`; archetype_count is the manager's archetype count (count_mass_entities counted the query's matching collections). |
| `destroy_all_mass_entities` | `destroy_mass_entity` | with `all=true` |
| `get_mass_spawner_count` | `get_mass_spawner_info` |  |
| `get_mass_spawner_entity_types` | `get_mass_spawner_info` |  |
| `add_collection_vector_param` | `add_collection_parameter` | with `param_type="vector"`; default_value [r,g,b,a] is unchanged (an array also infers vector). Reply unchanged: vector_count_before, vector_count, default, default_rgba, id, param_added, already_existed, saved. |
| `clear_collection_params` | `remove_collection_parameter` | with `all=true`; param_type scalar\|vector\|all passes through (all is the default with all=true). Reply keeps param_type, scalar_count_before, vector_count_before, scalar_count, vector_count, cleared, saved. An unknown param_type is now refused (it cleared nothing before). |
| `clear_material_parameter_overrides` | `set_material_instance_parameter_override` | with `all=true`, `override=false`; Reply keeps material_instance_path and cleared, plus saved. |
| `create_material_function_expression` | `create_material_expression` | material_function_path is accepted by create_material_expression (alias of material_path that addresses a function); expression_class_name, node_pos_x, node_pos_y and the reply fields (material_function_path, expression_name, expression_class_name, node_pos_*, success) are unchanged. |
| `create_material_instance_constant` | `create_material_instance` | params `name` → `instance_name`, `package_path` → `instance_path`; name/package_path are also accepted as aliases. Same defaults (folder /Game/MCP, parent optional, idempotent with already_existed). The reply keeps material_instance_path, material_instance_name, parent_material_path, created, already_existed, saved, save_error. |
| `create_material_layer` | `create_material_function` | with `usage="MaterialLayer"`; Reply keeps asset_path, name, class, usage, created, already_existed, saved, save_error (plus function_path). A failed save now makes success false (it was success with saved=false); package_path no longer needs a leading slash check of its own (an invalid path is refused by the package-name check). |
| `create_material_layer_blend` | `create_material_function` | with `usage="MaterialLayerBlend"`; As create_material_layer. |
| `create_material_parameter_collection_instance` | `reset_material_parameter_collection_instance` | Reply keeps is_valid, already_existed, created (plus the reset counts). is_valid is now the instance's IsCollectionValid() instead of 'an instance exists'. |
| `create_material_variant` | `create_material_instance` | params `material_name` → `parent_material_path`; drop `variant_name`; reply `variant_path` → `material_instance_path`, `variant_asset_name` → `material_instance_name`, `parent_material` → `parent_material_name`, `variant_saved` → `saved`; Not mechanical: the old asset was named '<material_name>_<variant_name>' and placed in the parent's folder, so set instance_name='<material_name>_<variant_name>' and instance_path=<the parent's folder> (the default is /Game/MCP). parent_material_path accepts the bare material name. parameter_overrides passes through unchanged. An existing asset is now reported (already_existed=true, overrides NOT applied, note) instead of refused. The parent census and surface fields (material_class, parameters_count, *_parameters_count, material_is_*, blend_mode) are no longer returned: read them with get_material_parameters and get_material_referencers. |
| `get_collection_param_info` | `get_collection_info` | with `param_type="scalar"`; reply `id` → `scalar_params.0.id \| vector_params.0.id`, `default_value` → `scalar_params.0.default`, `default` → `vector_params.0.default`, `index` → `scalar_params.0.index \| vector_params.0.index`; Add param_type scalar only when the old call had none (its default). param_name passes through as a filter. A name that is not there gives found=false and empty lists instead of an error. The old param_type fell back to scalar on an unknown value; get_collection_info refuses it. |
| `get_material_default_parameter` | `get_material_parameters` | with `association="Global"`; params `material_path` → `material_instance_path`, `param_name` → `parameter_name`, `param_type` → `parameter_type`; reply `material_path` → `material_instance_path`; On a base UMaterial the effective value IS the graph default, so value is the same; texture values are 'None' (was '') when the default texture is empty. param_type static_switch maps to parameter_type static_switch. |
| `get_material_expressions` | `get_material_graph_info` | reply `expressions` → `nodes`; nodes[] carry name, class, class_path and description as expressions[] did, plus inputs. expression_count still counts every expression the material owns; ones its expression collection does not list are in orphan_nodes (the old list walked them too). The reply no longer carries success=true in the payload. |
| `get_material_function_graph` | `get_material_graph_info` | material_function_path is accepted by get_material_graph_info and the reply shape is the same (material_function_path, node_count, nodes, connection_count, connections) plus expression_count/orphan_nodes. |
| `get_material_instance_base_overrides` | `get_material_instance_info` | Same param; override_blend_mode, blend_mode, override_two_sided, two_sided, override_shading_model, shading_model, override_opacity_mask_clip_value, opacity_mask_clip_value are in the info reply. |
| `get_material_instance_params` | `get_material_parameters` | with `overridden_only=true`; reply `parent_material` → `parent_material_name`, `scalar_params` → `parameters[type=scalar] {name, value}`, `vector_params` → `parameters[type=vector] {name, value:{r,g,b,a}}`, `texture_params` → `parameters[type=texture] {name, value}`, `scalar_count` → `count of parameters[type=scalar]`, `vector_count` → `count of parameters[type=vector]`, `texture_count` → `count of parameters[type=texture]`; The three per-kind arrays became one parameters[] list with a type field; vector values are an {r,g,b,a} object under value (not flat r/g/b/a) and a texture is value (was texture_path). Overrides the parent no longer exposes, which the old list included, are in orphaned_overrides. parent_material is now the parent's object path; the old name is parent_material_name. |
| `get_material_instance_parent` | `get_material_instance_info` | Same param; parent_material_path, parent_material_name and parent_material_class are in the info reply, which now loads any UMaterialInstance. |
| `get_material_instance_static_component_mask` | `get_material_parameters` | with `parameter_type="static_component_mask"`, `association="Global"`; reply `association` → `parameters.0.association`; As get_material_instance_static_switch: add association Global only when the old call had none. value is {r,g,b,a} of bools as before. |
| `get_material_instance_static_switch` | `get_material_parameters` | with `parameter_type="static_switch"`, `association="Global"`; reply `association` → `parameters.0.association`; Add association Global only when the old call had none (the old default); a call that passed association and layer_index keeps them (layer_index is a filter now). A switch the material does not have gives found=false instead of value=false. |
| `get_material_layer_info` | `get_material_function_info` | params `layer_path` → `material_function_path`; layer_path is also accepted as an alias. Reply keeps asset_path, class, usage, num_inputs, num_outputs, num_expressions, is_layer, is_blend, plus the input/output node lists. |
| `get_material_layer_parameter` | `get_material_parameters` | with `association="Layer"`; reply `layer_index` → `parameters.0.layer_index`; parameter_name, parameter_type and layer_index pass through unchanged (layer_index is now a filter). |
| `get_material_param_info` | `get_material_parameters` | params `param_name` → `parameter_name`, `param_type` → `parameter_type`; reply `param_name` → `parameters.0.name`, `param_type` → `parameters.0.type`; param_name/param_type are also accepted as aliases. FIXED semantics: value is the EFFECTIVE value and found is true for a parameter the instance inherits (the old tool only looked at overrides and said found=false); overridden tells the two apart. value/overridden are copied to the top level when exactly one entry matches (a name used by Global and Layer parameters matches several: add association Global). |
| `get_material_static_switch_parameters` | `get_material_parameters` | with `parameter_type="static_switch"`; reply `static_switch_parameters` → `parameters[].name`, `static_switch_count` → `parameter_count`; Entries now also carry each switch's value, association and overridden flag. |
| `get_mid_all_params` | `get_material_parameters` | params `mid_object_path` → `material_instance_path`; reply `mid_object_path` → `object_path`, `scalar_params` → `parameters[type=scalar]`, `vector_params` → `parameters[type=vector] (value {r,g,b,a})`, `texture_params` → `parameters[type=texture] (value = texture path)`, `scalar_count` → `count of parameters[type=scalar]`, `vector_count` → `count of parameters[type=vector]`, `texture_count` → `count of parameters[type=texture]`; A MID resolves by object path or by name as before. texture_name is no longer returned (it is the last segment of value); a MID with no texture reads 'None' (was ''). |
| `set_mid_texture_param` | `set_material_instance_dynamic_parameter` | with `param_type="texture"`; params `mid_path` → `mid_object_path`, `parameter_name` → `param_name`, `texture_path` → `value`; reply `mid_path` → `mid_object_path`, `parameter_name` → `param_name`; mid_path, parameter_name and texture_path are also accepted as aliases. old_texture_path, new_texture_path, requested_texture_path and set are unchanged. |
| `set_mid_vector_param` | `set_material_instance_dynamic_parameter` | with `param_type="vector"`; mid_object_path, param_name and value {r,g,b,a} are unchanged; param_type can also be left out (an object value is a vector). Reply unchanged (old_value/new_value objects, set) plus param_type. |
| `set_mpci_parent_collection` | `reset_material_parameter_collection_instance` | reply `already_attached` → `already_existed`, `attached` → `is_valid`; previous_parent_path (always the collection itself) and already_attached (always true) are gone. The old tool rebuilt the instance with AddParameterCollectionInstance; reset gets-or-creates it and returns every parameter to its default -- the same observable result (no overrides left). |
| `set_mpci_vector_parameter_value` | `set_material_parameter_collection_instance_value` | rgba / r,g,b,a pass through unchanged; reply keeps value_rgba, readback_rgba, readback_ok plus param_type 'vector'. |
| `get_procedural_mesh_section_visible` | `get_procedural_mesh_info` | drop `section_index`; reply `visible` → `sections.<section_index>.visible`, `section_index` → `sections.<section_index>.index`; Read sections[section_index] (each entry carries its index). An out-of-range section_index was an error; now check num_sections. |
| `get_navigation_info` | `diagnose_navmesh` | diagnose_navmesh defaults to the editor world, like get_navigation_info, and reports much more (bounds volumes, recast, diagnosis). |
| `get_network_settings` | `get_network_info` | reply `net_driver_class` → `net_driver_info.class`, `net_driver_settings` → `net_driver_info`, `game_state_class` → `game_state_info.class`, `player_count` → `game_state_info.player_count`, `players` → `game_state_info.players`; has_replication_driver and replication_driver_class were constants (false, 'Not Available in UE 5.6') and are gone. |
| `get_world_net_mode` | `get_network_info` |  |
| `list_replicated_actors` | `get_network_info` | reply `actors` → `replicated_actors`, `total` → `replicated_actor_count`; Entries are { name, class, local_role, remote_role, is_replicated, net_dormancy }: actor_name -> name, actor_class -> class, and the roles are spelled Authority/SimulatedProxy/... instead of ROLE_Authority/...; role_filter in the reply is that canonical spelling. |
| `set_actor_replicate_movement` | `configure_replication` | params `actor_label` → `actor_name`; reply `actor_label` → `actor_name`, `replicate_movement` → `actual_settings.replicate_movement` |
| `set_actor_replicates` | `configure_replication` | params `actor_label` → `actor_name`, `replicates` → `replicate_lifecycle`; reply `actor_label` → `actor_name`, `replicates` → `actual_settings.replicate_lifecycle` |
| `get_engine_settings` | `get_fps_info` | reply `max_fps` → `performance_settings.max_fps` |
| `get_engine_stats` | `get_fps_info` | reply `max_fps` → `performance_settings.max_fps` |
| `clear_ism_instances` | `remove_instanced_static_mesh_instance` | with `all=true`; cleared is now measured (no instance left) instead of always true; removed and removed_count are added. |
| `list_geometry_collection_bones` | `get_geometry_collection_hierarchy` | reply `bones` → `bone_names / parents / simulation_types (parallel arrays)`, `bones[i].name` → `bone_names[i]`, `bones[i].parent` → `parents[i]`, `bones[i].sim_type` → `simulation_types[i]`; bones[] of objects becomes three parallel arrays indexed by transform. Unlike list_geometry_collection_bones, the canonical generates the Level attribute in memory (not saved) when missing. |
| `set_physics_body_advanced_properties` | `set_physics_body_properties` | Same param and reply field names (linear_damping, angular_damping, mass_scale, inertia_tensor_scale, com_nudge, enable_gravity, update_mass_when_scale_changes, applied_count, updated, saved). Stricter: mass_scale < 0.001 and a vector that is not 3 numbers are now refused before any write (the old tool silently skipped a short vector and wrote any mass_scale). |
| `set_static_mesh_kdop_collision` | `add_static_mesh_simple_collision` | with `shape="NDOP18"`; add_params.shape applies only when the call had no shape (the old default was NDOP18; the canonical defaults to Box). Reply fields kept: static_mesh_path, shape (now normalized to upper case), added_index, simple_collision_count, element_count, collision_added, duplicated_to_game, duplicated, saved, save_error, collision_changed (failure). |
| `call_component_function` | `call_function` | params `actor` → `object_path`, `function` → `function_name`, `params` → `parameters`; reply `returns_json.return_value` → `return_value`, `returns_json.out_params` → `out_params`, `function` → `function_name` |
| `call_library_function` | `call_function` | reply `returns_json.return_value` → `return_value`, `returns_json.out_params` → `out_params` |
| `call_object_method` | `call_function` | params `args_json` → `parameters`; reply `returns_json.return_value` → `return_value`, `returns_json.out_params` → `out_params` |
| `find_blueprint_assets_by_class` | `list_derived_classes` | with `include_native=false`, `include_abstract=true`, `package_path="/Game"`; params `class_name` → `base_class`; reply `assets` → `classes`, `total` → `count`, `target_class` → `base_class_path`, `class_name` → `base_class`; Per asset: asset_name, asset_path and native_parent unchanged, generated_class -> path. Nothing is loaded any more: Blueprints not in memory come back with loaded false (native_parent from the registry tag). |
| `get_blueprint_children` | `list_derived_classes` | with `include_native=false`, `include_abstract=true`, `package_path="/Game"`, `max_results=1000`; params `blueprint_name` → `base_class`; reply `blueprint_name` → `base_class`, `blueprint_path` → `base_class_path`, `children` → `classes`, `children_count` → `count`; base_class_path is the base's generated CLASS path (.._C), not its asset path. Per child: name -> asset_name, path -> asset_path, class_path -> path, package unchanged. parent_blueprint_valid is gone (an unresolvable base is an error). |
| `get_object_class` | `inspect_object` | with `include_properties=false` |
| `get_property_value` | `get_properties` | values and property_types are maps keyed by property name: the removed tool's value is values.<property_name> and property_type is property_types.<property_name>. A name the class lacks is listed in not_found instead of failing. |
| `get_subsystem_class_info` | `get_class_hierarchy` | reply `class_path` → `hierarchy.0.path`, `is_native` → `hierarchy.0.is_native`, `is_abstract` → `hierarchy.0.is_abstract`, `parent_class` → `hierarchy.1.name`, `parent_chain` → `hierarchy`; lifetime is at the top as before. parent_chain was the ancestors below USubsystem with class_name/class_path/lifetime; hierarchy starts with the class itself and goes up to UObject, each level with name, path and (for subsystem classes) lifetime. |
| `list_object_properties` | `inspect_object` | with `include_property_metadata=true` |
| `list_subsystem_classes` | `list_derived_classes` | with `base_class="Subsystem"`, `loaded_only=true`; drop `lifetime`; reply `total` → `count`, `native` → `native_count`, `blueprint` → `blueprint_count`; lifetime X becomes base_class XSubsystem (EngineSubsystem, EditorSubsystem, GameInstanceSubsystem, WorldSubsystem, LocalPlayerSubsystem) instead of base_class Subsystem. Per class: class_path -> path, is_native -> !is_blueprint; class_name, parent_class and lifetime unchanged (lifetime only on loaded classes). |
| `set_property_value` | `set_properties` | Reply: set[] names the property written and failed[{property,error}] the reason it was not; the removed tool only had success/error/message. |
| `get_actor_net_cull_distance` | `get_actor_replication_info` |  |
| `get_actor_net_update_frequency` | `get_actor_replication_info` |  |
| `get_net_priority` | `get_actor_replication_info` | reply `actor_name` → `actor_label`, `priority` → `net_priority` |
| `get_net_role` | `get_actor_replication_info` | reply `actor_name` → `actor_label` |
| `get_replication_info` | `get_actor_replication_info` | reply `actor_class` → `class_name`, `local_role` → `local_role_name`, `remote_role` → `remote_role_name`; net_dormancy now spells the engine enum name (DORM_Awake) where get_replication_info said Awake; net_dormancy_int is the same number. actor_name in the reply is still the object name; the param now matches a label too. |
| `does_save_game_exist` | `get_save_game_slot_info` |  |
| `get_save_game_info` | `get_save_game_slot_info` |  |
| `load_game_async` | `load_game` | mode ('sync_fallback') and note were constants and are gone. |
| `save_game_async` | `save_game` | mode ('sync_fallback') and note were constants and are gone; message reads 'Game saved successfully'. |
| `get_smart_object_info` | `get_smart_object_definition_summary` | reply `activity_tags` → `slot_activity_tags`; The summary's own activity_tags are the OBJECT-level tags; the old activity_tags (union of the slots' tags) is slot_activity_tags. Readback check CheckSmartObjectTags (Mcp/Readback) was registered for get_smart_object_info. |
| `list_states` | `get_state_tree_info` | reply `count` → `state_count`, `states[].child_count` → `states[].children`, `states[].transition_count` → `states[].transitions`; has_children is states[].children > 0. Order differs: list_states was breadth-first, get_state_tree_info is depth-first (last top-level state first); compare by name, not position. With root_state the walk root keeps its real parent name (list_states gave ''). |
| `list_transitions` | `get_state_tree_info` | params `state_name` → `root_state`; reply `transitions[].target` → `transitions[].to_state`, `transitions[].required_event_tag` → `transitions[].event_tag`, `transitions[].delay_enabled` → `transitions[].delay`, `count` → `transition_count`; Keep only transitions[] whose from_state == the old state_name: root_state also includes the descendants' transitions, and then transition_count counts them too. priority is now the enum NAME (was the integer); event_tag is '' when unset (required_event_tag was the tag text, 'None' when unset). |
| `list_tree_parameters` | `get_state_tree_info` | reply `count` → `parameter_count`; parameters[].type is the short kind (Float) except for object/class/struct/enum parameters, where it is the class, struct or enum name; value_type always holds the qualified kind (EPropertyBagPropertyType::Float). |
| `set_state_linked_asset` | `set_state_type` | with `state_type="LinkedAsset"`; The reply adds changed (always true with linked_asset_path). |
| `set_transition_properties` | `update_transition` | Same params and no-op rule (changed false, saved false). The reply adds transition_type, target_state, event_tag, payload_struct, delay_variance, applied_fields and errors. |
| `get_subsystem_instance` | `inspect_subsystem_instance` | with `include_properties=false`; params `class_name` → `subsystem_class`; reply `class_name` → `subsystem_class` |
| `is_subsystem_class_active` | `list_subsystem_instances` | active, class_name and lifetime keep their names. instances[] entries carry class_name, class_path, lifetime, instance_name and instance_path; the removed tool's per-instance collection is gone. |
| `profile_blueprint_execution` | `get_blueprint_debug_info` | drop `compile_for_timing`; reply `cpu_usage` → `complexity_score`, `memory_usage` → `file_size_bytes`, `memory_score_mb` → `file_size_mb`; Nothing is compiled any more (get_blueprint_debug_info stays read-only and PIE-safe): execution_time / compilation_score_ms (the compile time), profile_status and under_readonly_root have no equivalent. The constants is_static_analysis, runtime_profiled, execution_time_is_compile_time_ms, cpu_usage_is_complexity_score and memory_usage_is_disk_file_size_bytes are gone; recommendations is now always present (empty list instead of absent). |
| `test_blueprint_function` | `get_blueprint_debug_info` | drop `compile_for_timing`; reply `test_success` → `function_exists`, `node_count` → `function_node_count`, `connection_count` → `function_pin_count`, `performance_rating` → `function_rating`, `issues` → `function_issues`, `memory_usage` → `file_size_bytes`; Nothing is compiled any more: execution_time (the compile time) and under_readonly_root have no equivalent, and the slow-compile finding is gone from function_issues. test_result and the constant flags is_static_analysis, function_was_executed, execution_time_is_compile_time_ms, memory_usage_is_disk_file_size_bytes, connection_count_is_pin_count are gone; function_issues is always present when the function exists (empty list instead of absent). |
| `trace_blueprint_execution` | `get_blueprint_debug_info` | with `include_graph_listing=true`; reply `total_execution_paths` → `graphs_analyzed`; trace_status, trace_level, trace_message and the constants is_static_analysis, traced_runtime, total_execution_paths_is_graph_count are gone; function_count/event_count are the Blueprint's own graph counts (composites not added: count function_calls/event_calls instead); limitations no longer has the total_execution_paths caveat. |
| `validate_blueprint` | `get_blueprint_debug_info` | validation_status Valid/Invalid is is_blueprint true/false; validation_message and the constant is_static_analysis are gone. A non-Blueprint answers only { blueprint_path, resolved_object_path, blueprint_class, is_blueprint: false, debug_message }. |
| `add_camera_cut_section` | `add_camera_cut` | sequence_path is an alias of sequence_name; start_frame (default 0) and end_frame (default start_frame + 1) and every reply field keep their names. A camera_binding_guid the sequence has no binding for is now an error. |
| `add_camera_focus_track` | `add_property_track` | with `property_type="Float"`, `property_name="ManualFocusDistance"`; drop `track_label` |
| `add_visibility_track` | `add_property_track` | with `property_type="Visibility"`, `property_name="bVisible"`; drop `track_label` |
| `add_weighted_value_keyframe` | `add_keyframe_to_property_track` | with `property_type="Float"`, `interp_mode="cubic"` |
| `add_weighted_value_track` | `add_property_track` | with `property_type="Float"`, `property_name="Weight"`; drop `track_label` |
| `bind_actor_property_to_sequence` | `bind_actor_to_sequence` | drop `track_label`; Every reply field keeps its name. An actor the sequence already possesses now reuses that binding (binding_created false) instead of adding a second one. |
| `bind_component_property_to_sequence` | `bind_component_to_sequence` | drop `track_label` |
| `clear_marked_frames` | `remove_marked_frame` | with `all=true` |
| `get_binding_tracks` | `get_sequence_bindings` | reply `tracks` → `bindings.0.tracks`, `track_count` → `bindings.0.track_count` |
| `get_sequence_duration` | `get_timeline_info` |  |
| `get_sequence_frame_rate` | `get_timeline_info` | reply `frame_rate` → `tick_resolution`, `numerator` → `tick_resolution_numerator`, `denominator` → `tick_resolution_denominator` |
| `get_sequence_master_tracks` | `get_sequence_tracks` | params `sequence_path` → `sequence_name`; reply `binding_tracks` → `bindings`; master_tracks entries: name -> track_name (display_name is new), binding_guid gone (it was empty on root tracks). Binding tracks are nested per binding: bindings[].tracks, with the guid on the binding. total_track_count, master_track_count and binding_track_count keep their names. |
| `get_sequence_possessables` | `get_sequence_bindings` | with `kind="possessable"`; reply `possessables` → `bindings`; Entries keep guid, name, class and parent_guid. kind=possessable excludes bindings with a spawn track (the removed tool listed them too); possessable_count is still the movie scene's own count. |
| `get_sequence_spawnables` | `get_sequence_bindings` | with `kind="spawnable"`; reply `spawnables` → `bindings`, `spawnable_count` → `binding_count`; Entries: template_class -> class; guid, name, spawnable and spawn_ownership unchanged. |
| `get_sequence_track_count` | `get_sequence_tracks` | params `sequence_path` → `sequence_name` |
| `set_audio_section_start_offset` | `set_audio_section_properties` | reply `applied` → `set` |
| `set_audio_track_pitch` | `set_audio_section_properties` | params `sequence_name` → `sequence_path`; reply `key_count` → `pitch_key_count`, `keys_written` → `pitch_keys_written` |
| `set_audio_track_volume` | `set_audio_section_properties` | params `sequence_name` → `sequence_path`; reply `key_count` → `volume_key_count`, `keys_written` → `volume_keys_written` |
| `set_sequence_duration` | `set_sequence_playback_range` |  |
| `set_transform_track_interp_mode` | `set_section_blend_type` | params `interp_mode` → `blend_type`; reply `interp_mode` → `blend_type`, `applied` → `set` |
| `create_volume` | `spawn_box_volume` | params `volume_type` → `volume_class`, `volume_name` → `actor_label`; drop `size`; reply `volume_name` → `requested.actor_label`, `volume_type` → `volume_class`; size S (the FULL edge) becomes extent [S/2, S/2, S/2]; size omitted or <= 0: omit extent (default [100,100,100], the same 200 cm cube). volume_type omitted -> omit volume_class (default BlockingVolume). An unknown volume_type used to spawn a BlockingVolume silently; spawn_box_volume refuses a class it cannot resolve. spawn_box_volume also normalizes the actor scale and derives success from the re-read bounds; the spatial-loading flag is touched only when is_spatially_loaded is passed. |
| `get_trigger_overlap_count` | `get_actors_in_trigger` | params `actor_name` → `trigger_name`; reply `overlap_count` → `actor_count`; Same lookup (object name or editor label, first match). The reply also lists actors[] and the trigger's actor_class. |
| `remove_trigger_sphere` | `remove_trigger` | remove_trigger removes any ATriggerBase (box, sphere, capsule); the class check refuses other actors as before. |
| `add_tile_view` | `add_list_view` | with `view_type="Tile"`; Reply gains view_type. |
| `add_tree_view` | `add_list_view` | with `view_type="Tree"`; params `widget_name` → `list_view_name`; reply `widget_name` → `list_view_name`, `tree_view_class` → `list_view_class`; widget_name is also accepted as an alias of list_view_name, so the param rename is optional. parent_widget_name and the authoring note are kept. |
| `add_widget_extended` | `add_widget_to_blueprint` | Identical params and reply; add_widget_to_blueprint now takes the extended palette. |
| `get_widget_hierarchy` | `get_level_widget_properties` | reply `hierarchy.children` → `children`, `hierarchy.class` → `widget_class`, `hierarchy.valid` → `widget_valid`, `hierarchy.enabled` → `is_enabled`, `hierarchy.visibility` → `visibility`, `hierarchy.size` → `size`, `hierarchy.position` → `position`, `hierarchy.child_count` → `children.length`; children[] entries keep name, class, depth (1), valid, enabled, visibility, but no longer carry the always-zero child_count / empty children. The canonical's own child_count counts every root-panel child (invalid ones too); children.length counts the valid ones, as get_widget_hierarchy's hierarchy.child_count did. hierarchy.name (the widget object name) has no own field: widget_name echoes the requested name. |
| `get_usd_layer_info` | `list_usd_stage_layers` | reply `identifier` → `layers.0.identifier`, `real_path` → `layers.0.real_path`, `dirty` → `layers.0.dirty`, `is_muted` → `layers.0.is_muted`, `sub_layer_paths` → `layers.0.sub_layer_paths`, `sub_layer_count` → `layers.0.sub_layer_count`; layer_index passes through as the filter. get_usd_layer_info defaulted layer_index to 0: add layer_index 0 when the old call omitted it. Out of range is still an error. |
| `get_usd_variant_set_info` | `list_usd_variant_sets` | reply `variants` → `variants.<variant_set_name>`; variant_set_name passes through as the filter; a set the prim does not author is still an error. variants is now an object keyed by set name. |
| `set_usd_prim_active` | `set_usd_prim_metadata` | with `key="active"`; params `active` → `value`; reply `active` → `value.value`; active defaulted to true when omitted: pass value true in that case. On a refused write the old reply reported the prim's current IsActive(); now set=false with the USD errors. |
| `set_usd_prim_display_name` | `set_usd_prim_metadata` | with `key="displayName"`; params `display_name` → `value`; reply `display_name` → `value.value`; An empty string is still written as an empty displayName. |
| `set_usd_prim_instanceable` | `set_usd_prim_metadata` | with `key="instanceable"`; params `instanceable` → `value`; reply `instanceable` → `value.value`; instanceable defaulted to true when omitted: pass value true in that case. |
| `set_usd_prim_type` | `set_usd_prim_metadata` | with `key="typeName"`; params `type_name` → `value`; reply `type_name` → `value.value`; Written as a token, unchecked against the schema registry, as before. |
| `usd_module_status` | `list_usd_file_formats` |  |
| `get_water_body_wave_info` | `get_water_body_info` | reply `water_body_label` → `actor_label` |

## Renamed

Same tool, same params and reply, new name.

| Old name | New name |
|---|---|
| `add_blendspace_sample` | `add_blend_space_sample` |
| `add_bt_composite_child` | `add_behavior_tree_child` |
| `add_bt_decorator` | `add_behavior_tree_decorator` |
| `add_bt_service` | `add_behavior_tree_service` |
| `add_chooser_column_to_table` | `add_chooser_column` |
| `add_collection_scalar_param` | `add_collection_parameter` |
| `add_ism_instance` | `add_instanced_static_mesh_instance` |
| `add_mass_trait_to_entity` | `add_mass_tag_to_entity` |
| `add_rig_bone` | `add_rig_element` |
| `add_simple_collision_to_static_mesh` | `add_static_mesh_simple_collision` |
| `add_socket_to_skeleton_asset` | `add_skeleton_socket` |
| `add_state_evaluator` | `add_evaluator` |
| `add_state_tree_global_task_property` | `set_state_tree_global_task_property` |
| `add_tree_parameter` | `add_state_tree_parameter` |
| `add_warp_target_to_component` | `add_motion_warping_target` |
| `apply_component_property_overrides` | `set_blueprint_component_properties` |
| `assign_rvt_to_landscape` | `assign_runtime_virtual_texture_to_landscape` |
| `auto_generate_mesh_uvs` | `unwrap_mesh_uvs` |
| `batch` | `execute_batch` |
| `batch_delete_actors` | `delete_actors` |
| `batch_place_actors_on_terrain` | `place_actors_on_terrain` |
| `batch_set_actor_transform` | `set_actor_transforms` |
| `bookmark` | `add_trace_bookmark` |
| `build_animation_state_machine` | `build_anim_state_machine` |
| `bulk_delete_assets` | `delete_assets` |
| `bulk_rename_assets` | `rename_assets` |
| `clear_mpci_parameter_overrides` | `reset_material_parameter_collection_instance` |
| `compute_gc_proximity` | `compute_geometry_collection_proximity` |
| `configure_ability_tags` | `set_ability_asset_tags` |
| `configure_animation_events` | `set_skeletal_mesh_component_tick` |
| `connect_metasound_nodes` | `connect_metasound_graph_input_to_output` |
| `copy_material_instance_params` | `copy_material_instance_parameters` |
| `copy_mid_params_from_source` | `copy_material_instance_dynamic_parameters` |
| `create_attribute_set_asset` | `create_attribute_set_blueprint` |
| `create_gameplay_cue_notify_static` | `create_gameplay_cue_notify` |
| `create_gameplay_effect_container` | `create_gameplay_effect_asset` |
| `create_rvt_asset` | `create_runtime_virtual_texture` |
| `create_rvt_volume` | `create_runtime_virtual_texture_volume` |
| `delete_bp_variable` | `delete_blueprint_variable` |
| `delete_montage_section` | `remove_montage_section` |
| `delete_selected_triangles_from_mesh` | `delete_mesh_triangles` |
| `disconnect_metasound_nodes` | `disconnect_metasound_graph_input_from_output` |
| `discover_input_types` | `list_input_types` |
| `duplicate_mpc` | `duplicate_material_parameter_collection` |
| `edit_blendspace_sample` | `edit_blend_space_sample` |
| `editor_status` | `get_editor_status` |
| `enable_plugin` | `set_plugin_enabled` |
| `foliage_type_exists` | `does_foliage_type_exist` |
| `frame_timing` | `get_frame_timing` |
| `get_ability_activation_info` | `get_ability_asset_tags` |
| `get_asset_file_size` | `get_asset_file_info` |
| `get_blendspace_samples` | `get_blend_space_samples` |
| `get_blueprint_scs_tree` | `get_blueprint_components` |
| `get_gc_hierarchy_info` | `get_geometry_collection_hierarchy` |
| `get_ism_instance_info` | `get_instanced_static_mesh_instance_info` |
| `get_material_all_parameters` | `get_material_parameters` |
| `get_material_function_inputs` | `get_material_function_info` |
| `get_material_instance_param_overridden` | `get_material_instance_parameter_overridden` |
| `get_material_usage` | `get_material_referencers` |
| `get_mesh_volume_area_center` | `get_static_mesh_geometry_stats` |
| `get_meta_sound_node_info` | `get_metasound_node_info` |
| `get_meta_sound_node_property` | `get_metasound_node_property` |
| `get_modulation_info` | `get_control_bus_info` |
| `get_mpci_info` | `get_material_parameter_collection_instance_info` |
| `get_river_spline_points` | `get_water_body_spline_points` |
| `get_rvt_info` | `get_runtime_virtual_texture_info` |
| `get_state_theme_colors` | `get_state_tree_theme_colors` |
| `get_widget_properties` | `get_level_widget_properties` |
| `has_any_gameplay_tags` | `has_gameplay_tags` |
| `high_res_screenshot` | `capture_high_res_screenshot` |
| `inspect_material_instance_constant` | `get_material_instance_info` |
| `interchange_can_reset_actor` | `can_reset_interchange_actor` |
| `interchange_can_reset_world` | `can_reset_interchange_world` |
| `interchange_can_translate_source_data` | `can_translate_interchange_source_data` |
| `interchange_create_pipeline_asset` | `create_interchange_pipeline_asset` |
| `interchange_get_asset_import_data` | `get_interchange_asset_import_data` |
| `interchange_get_pipeline_properties` | `get_interchange_pipeline_properties` |
| `interchange_get_project_settings` | `get_interchange_project_settings` |
| `interchange_get_supported_formats` | `get_interchange_supported_formats` |
| `interchange_import_asset` | `import_interchange_asset` |
| `interchange_register_factory` | `register_interchange_factory` |
| `interchange_register_translator` | `register_interchange_translator` |
| `interchange_reimport_asset` | `reimport_interchange_asset` |
| `interchange_reset_actors` | `reset_interchange_actors` |
| `interchange_reset_level_asset` | `reset_interchange_level_asset` |
| `interchange_reset_scene_import_asset` | `reset_interchange_scene_import_asset` |
| `interchange_save_pipeline` | `save_interchange_pipeline` |
| `interchange_set_pipeline_properties` | `set_interchange_pipeline_properties` |
| `interchange_set_pipeline_stack_default` | `set_interchange_pipeline_stack_default` |
| `interchange_set_pipelines` | `set_interchange_pipelines` |
| `list_active_regions` | `list_timing_regions` |
| `list_global_tasks` | `list_state_tree_global_tasks` |
| `list_mass_trait_types` | `list_mass_tag_types` |
| `list_meta_sound_node_types` | `list_metasound_node_types` |
| `list_meta_sound_nodes` | `list_metasound_nodes` |
| `list_tree_bindings` | `list_state_tree_bindings` |
| `metasound_add_interface` | `add_metasound_interface` |
| `metasound_add_node_by_classname` | `add_metasound_node` |
| `metasound_connect_node_to_graph_output` | `connect_metasound_node_to_graph_output` |
| `metasound_set_input_default` | `set_metasound_graph_input_default` |
| `metasound_set_node_input_default` | `set_metasound_node_input_default` |
| `overlap_scene` | `query_scene_overlaps` |
| `plane_cut_fracture` | `cut_geometry_collection_with_planes` |
| `read_datatable_from_package_file` | `read_data_table_from_package_file` |
| `regenerate_collection_param_id` | `regenerate_collection_parameter_id` |
| `region_end` | `end_timing_region` |
| `region_start` | `start_timing_region` |
| `remove_all_foliage_for_type` | `remove_foliage_instances` |
| `remove_blendspace_sample` | `remove_blend_space_sample` |
| `remove_bt_composite_child` | `remove_behavior_tree_child` |
| `remove_bt_decorator` | `remove_behavior_tree_decorator` |
| `remove_bt_service` | `remove_behavior_tree_service` |
| `remove_collection_param` | `remove_collection_parameter` |
| `remove_ism_instance` | `remove_instanced_static_mesh_instance` |
| `remove_mass_trait_from_entity` | `remove_mass_tag_from_entity` |
| `remove_socket_from_skeleton_asset` | `remove_skeleton_socket` |
| `remove_tree_binding` | `remove_state_tree_binding` |
| `remove_tree_parameter` | `remove_state_tree_parameter` |
| `remove_trigger_box` | `remove_trigger` |
| `remove_warp_target` | `remove_motion_warping_target` |
| `rename_bp_variable` | `rename_blueprint_variable` |
| `rename_collection_param` | `rename_collection_parameter` |
| `reparent_blueprint_safe` | `reparent_blueprint` |
| `scan_uasset_refs` | `scan_uasset_references` |
| `set_anim_curve_interp_mode` | `set_anim_curve_flags` |
| `set_binding_channel_keyframe_interp` | `set_binding_channel_keyframe_interpolation` |
| `set_blendspace_interpolation` | `set_blend_space_interpolation` |
| `set_blendspace_properties` | `set_blend_space_properties` |
| `set_bp_class_replication` | `set_blueprint_class_replication` |
| `set_bp_function_rpc` | `set_blueprint_function_rpc` |
| `set_bp_variable_default` | `set_blueprint_variable_default` |
| `set_bp_variable_metadata` | `set_blueprint_variable_metadata` |
| `set_bp_variable_replication` | `set_blueprint_variable_replication` |
| `set_collection_param_default` | `set_collection_parameter_default` |
| `set_component_definition` | `set_smart_object_component_definition` |
| `set_component_parameter_override` | `set_state_tree_component_parameter_override` |
| `set_constraint_limits` | `set_physics_constraint_limits` |
| `set_game_mode_default_pawn` | `set_game_mode_classes` |
| `set_gc_connection_settings` | `set_geometry_collection_connection_settings` |
| `set_material_instance_param` | `set_material_instance_parameter` |
| `set_material_instance_param_override` | `set_material_instance_parameter_override` |
| `set_material_node_param` | `set_material_expression_property` |
| `set_meta_sound_node_property` | `set_metasound_node_property` |
| `set_mid_scalar_param` | `set_material_instance_dynamic_parameter` |
| `set_mpci_scalar_parameter_value` | `set_material_parameter_collection_instance_value` |
| `set_net_priority` | `set_actor_net_priority` |
| `set_niagara_system_fixed_bounds` | `set_niagara_component_fixed_bounds` |
| `set_river_spline_points` | `set_water_body_spline_points` |
| `set_rvt_volume_bounds` | `set_runtime_virtual_texture_volume_bounds` |
| `set_skeletal_animation_section_params` | `set_skeletal_animation_section_parameters` |
| `set_skeletal_mesh_section_cast_shadow` | `set_skeletal_mesh_section_flags` |
| `set_state_theme_color` | `set_state_tree_theme_color` |
| `set_subtitle_cue_timing` | `set_subtitle_cue` |
| `set_transform_section_quaternion_interp` | `set_transform_section_quaternion_interpolation` |
| `set_tree_parameter` | `set_state_tree_parameter` |
| `set_widget_visibility` | `set_level_widget_visibility` |
| `slice_fracture` | `slice_geometry_collection` |
| `snapshot_object` | `capture_object_snapshot` |
| `unassign_rvt_from_landscape` | `unassign_runtime_virtual_texture_from_landscape` |
| `update_ism_instance_transform` | `update_instanced_static_mesh_instance_transform` |

## Removed

These could not do what their name said; nothing replaces them.

| Removed | Why |
|---|---|
| `add_ability_task` | It ignores task_class (only validated) and always builds the abstract base UAbilityTask via NewAbilityTask<UAbilityTask>, which logs an ensure, is never activated, and is outered to the transient package so get_ability_tasks cannot even list it: the call can never produce a working task. |
| `add_component_event` | By its own code it never adds anything: success and binding_created are hardcoded false on every path ("Always false: this command never creates a binding"). The event catalogue it echoes is get_component_events's, and the real authoring path is add_component_bound_event. |
| `build_hlod` | never starts a build in either branch: WorldPartitionGateReply sets build_started=false and both paths return with only a note; is_world_partition is all it measures. |
| `convert_map_to_world_partition` | Never converts (commandlet-only; map_path ignored): converted is always false; its only fact, is_world_partition, is get_world_partition_info's is_world_partition_enabled. |
| `count_gameplay_tag_references` | reference_count is hard-coded to 0 by its own code; the only real output (valid) is what get_gameplay_tag_info already answers. |
| `create_save_game` | Creates a transient USaveGame that no other tool accepts and nothing persists (save_game builds its own fresh object); the class-resolves check it offers is done by save_game and save_game_to_memory as well. |
| `flush_net_dormancy` | AActor::FlushNetDormancy on an editor-world actor: no net driver, nothing is sent and PIE is never reached; flushed is hardcoded true. Its only side effect (DORM_Initial -> DORM_DormantAll on the placed instance) is an accident, and set_actor_net_dormancy sets dormancy deliberately. |
| `force_net_update` | AActor::ForceNetUpdate on an editor-world actor: the editor world has no net driver, so nothing is sent and no state changes; forced is hardcoded true (its own description says so). |
| `list_mass_signals` | always returns count 0 and an empty signals array (NamedSignals has no public enumeration); nothing it reports depends on the world. |
| `set_preserve_scale_ratio` | Writes nothing and always answers applied:false (the preference is not a public UPROPERTY); it only echoes its input. |
| `set_soundfield_effect_settings` | By its own code it can never write anything: after validating the slot it always returns success=false, applied_count 0, saved=false (USoundfieldEffectBase::GetSettingsClass is protected). |
| `set_water_body_gerstner_waves_spectrum` | in UE 5.8 UGerstnerWaterWaveGeneratorSpectrum::GenerateGerstnerWaves is an engine stub, so by its own note the call can only replace the body's waves with an empty set (num_waves 0); set_water_body_gerstner_waves is the working path. |
| `set_world_partition_runtime_hash` | Accepts cell_size/loading_range/grid_name and never applies them (no public setter): always applied:false; the runtime_hash_type it echoes is already in get_world_partition_info. |
