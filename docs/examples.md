# Examples

Prompts that work well, with the tools an agent usually reaches for. The agent picks the tools
itself: the lists only show what each task touches. Every tool is described in the
[tool reference](tools/README.md).

## Blueprints and UI

| Ask | Tools |
|---|---|
| "Create BP_Door: a door mesh and a box trigger, and open the door with a timeline when the player overlaps the trigger." | `create_blueprint`, `add_component_to_blueprint`, `build_blueprint_graph`, `compile_blueprint` |
| "Find every Blueprint under /Game that does not compile and tell me why." | `find_erroring_blueprints`, `get_graph_nodes`, `lint_blueprint_graph` |
| "Make a main menu with Play, Options and Quit buttons." | `scaffold_menu_screen` |

## Levels, lighting and terrain

| Ask | Tools |
|---|---|
| "Light this level as a sunny outdoor scene: sun, sky and a light fog." | `spawn_actor`, `set_light_intensity`, `spawn_sky_atmosphere`, `spawn_sky_light`, `spawn_exponential_height_fog` |
| "Make a terrain from D:/Heightmaps/island.png." | `create_landscape`, `import_landscape_heightmap` |
| "Place five BP_Crate in a row in front of the player start and take a high-resolution screenshot." | `spawn_blueprint_actor`, `high_res_screenshot` |

## Materials

| Ask | Tools |
|---|---|
| "Make a red variant of M_Base and put it on the crates in this level." | `create_material_instance`, `set_material_instance_param`, `get_actors_by_class`, `set_actor_material` |

## Animation and VFX

| Ask | Tools |
|---|---|
| "Create AM_Attack with the sections Windup, Strike and Recover." | `create_anim_montage`, `add_montage_section` |
| "Add a Footstep notify to AS_Run at 0.4 and 0.9 seconds." | `add_anim_notify` |
| "Create NS_Sparks from a template emitter and place it at the forge." | `create_niagara_system`, `add_niagara_emitter_from_template`, `spawn_niagara_component` |

## Gameplay and AI

| Ask | Tools |
|---|---|
| "Add a Jump action bound to Space and Gamepad A in IMC_Default." | `create_input_action`, `add_mapping_to_context` |
| "Create the tags Status.Stunned and Status.Burning." | `create_gameplay_tag` |
| "Make an enemy that patrols between points and chases the player when it sees them." | `create_blackboard_data`, `add_blackboard_key`, `create_behavior_tree`, `add_bt_composite_child`, `add_bt_task_child`, `create_ai_controller_blueprint`, `build_blueprint_graph` |
| "Create DT_Weapons from this CSV, with FWeaponRow as its row." | `create_data_table`, `import_data_table_csv`, `get_data_table_rows` |

## Cinematics and testing

| Ask | Tools |
|---|---|
| "Cut a shot: a camera cut, a fade in, and the intro sound at frame 0." | `create_level_sequence`, `add_camera_cut_track`, `add_fade_track_to_sequence`, `add_audio_section` |
| "Play the level, read the player's health, take a screenshot and stop." | `start_play_in_editor`, `get_properties`, `capture_pie_screenshot`, `stop_play_in_editor` |

## Project hygiene

| Ask | Tools |
|---|---|
| "Which assets under /Game/Old have no referencers, and which of them are safe to delete?" | `find_unused_assets`, `get_asset_referencers` |
| "Clean up the redirectors the last rename left in /Game/Characters." | `fixup_redirectors` |
| "Which of my assets still reference the vendor pack, and which classes inherit from it?" | `audit_asset_references`, `audit_class_purity`, `run_map_check` |

## When no tool fits

- `execute_python` runs Python in the editor, with Unreal's Python API.
- `batch` runs several calls in one pass on the game thread, without a round trip each.

## Tips

- Name things exactly: `/Game/Characters/BP_Hero` beats "the hero".
- After a graph edit, ask the agent to compile and fix what the compiler reports.
- For a client that cannot take every tool, use the [compact tool list](../README.md#compact-tool-list):
  the agent then finds these tools with `find_tools`.
