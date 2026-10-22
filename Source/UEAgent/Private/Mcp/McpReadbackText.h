// Copyright (c) 2026 CarlxsMG.

#pragma once
// Readback contract notes. After a tool runs, the readback layer re-reads the editor state and,
// when the tool's answer is misleading (a false success, a zero that proves nothing, a write that
// landed in the wrong world...), attaches one of these notes: what is misleading and how to verify
// instead. Maintained by hand; the contract test vectors (Resources/ContractTests) embed them.
// {0}, {1}... are filled by McpPy::F (the comment above a constant says what goes in each slot);
// %s / %d / %r are filled by McpPy::Percent, where a literal percent sign must be written %%.

#include "CoreMinimal.h"

namespace McpReadbackText
{
inline constexpr const TCHAR* RB_check_bp_variable_default_0 =
	TEXT("roundtrip_verified is not the oracle: a None re-exports just as cleanly, so it confirms the failure instead of catching it. A path that ")
	TEXT("does write: set the value on the CDO from Python (get_default_object + set_editor_property), without recompiling afterwards.");

inline constexpr const TCHAR* RB_check_collision_profile_0 =
	TEXT("Some per-channel responses did not reach the profile, so for those channels it keeps the default response instead of the one asked, ")
	TEXT("with no error. An entry is skipped when its channel is not a channel of the project or its response is not Block, Ignore or Overlap: ")
	TEXT("take the channel names list_collision_channels shows (a custom channel by its friendly name), retry until custom_response_count ")
	TEXT("matches the entries sent, and confirm the profile line in Config/DefaultEngine.ini lists the channels inside CustomResponses=(...).");

inline constexpr const TCHAR* RB_check_compile_blueprint_0 =
	TEXT("The Blueprint does not compile. A common cause is an unresolved wildcard pin on a CustomThunk array node ('The type of Target Array is ")
	TEXT("undetermined'), which also blocks PIE. compile_blueprint(blueprint_name, return_messages=true) lists the messages; after editing graphs over MCP, always check ")
	TEXT("for compile errors.");

inline constexpr const TCHAR* RB_check_component_property_0 =
	TEXT("final_value is the value re-read from the component, so an empty value means the write did not land. For a struct, pass the complete ")
	TEXT("ExportText with the GUID-suffixed member names; for an enum, the internal name (NewEnumeratorN). A non-empty final_value is not conclusive ")
	TEXT("for properties the construction script rewrites (e.g. RelativeScale3D), because the re-read happens before the UCS runs: call ")
	TEXT("rerun_construction_scripts, resolve the actor and component again by name (the UCS recreates its components) and re-read.");

inline constexpr const TCHAR* RB_check_component_parameters_0 =
	TEXT("verify_persistence=true can cause the loss it reports: it runs RerunConstructionScripts, and if the template has this property at None the ")
	TEXT("rerun recreates the component from the template, wiping the value you just wrote (reported as survives_construction_script:false). Use it ")
	TEXT("to check whether an existing value is stable, never in the same call that writes it.");

inline constexpr const TCHAR* RB_check_component_parameters_1 =
	TEXT("Writing a property on a live actor (world=%r) triggers PostEditChangeProperty -> RerunConstructionScripts. Actors whose construction script ")
	TEXT("spawns helper actors (manager or proxy actors) break: SpawnActor is not allowed there, so those components end up ")
	TEXT("holding None while everything looks in place. In PIE, change state through a function instead (call_function with component_class runs a UFUNCTION ")
	TEXT("without re-running construction); if it already happened, restart PIE.");

inline constexpr const TCHAR* RB_check_component_parameters_2 =
	TEXT("DefinitionAsset is deprecated: it is written and re-reads fine, but the SmartObject keeps using the old definition, and neither this tool ")
	TEXT("nor get_properties warns. Write DefinitionRef instead (a StructProperty): {\"SmartObjectDefinition\": \"/Game/...\"}. ")
	TEXT("get_object_info with include_property_metadata=true reports deprecated:true/false per property.");

inline constexpr const TCHAR* RB_check_comment_node_0 =
	TEXT("The comment text was lost and the box shows the literal 'Comment': the tool sets NodeComment before the node is finalized and the ")
	TEXT("engine's PostPlacedNewNode overwrites it, while this response (which has no success field) still returns the node_id. To write the text, ")
	TEXT("use execute_python with set_comment_text() on the EdGraphNode; otherwise delete the empty box.");

inline constexpr const TCHAR* RB_check_node_references_0 =
	TEXT("the token is in the name map of %d asset(s) (%s)");

inline constexpr const TCHAR* RB_check_node_references_1 =
	TEXT("in-response:assets_hit_prefilter_without_graph_match");

inline constexpr const TCHAR* RB_check_node_references_2 =
	TEXT("There are %d match(es), but they may not be all: %s. A reference can also arrive through data -- e.g. a DataTable row fed into a Class pin, ")
	TEXT("which then has no default -- and a node scanner cannot see that. Cross-check with get_asset_referencers on the class (it sees DataTables, ")
	TEXT("which are package references) and, when looking for who spawns something, compare how many instances are placed with how many exist in PIE.");

inline constexpr const TCHAR* RB_check_node_references_3 =
	TEXT("in-response:assets_hit_prefilter_without_graph_match");

inline constexpr const TCHAR* RB_check_node_references_4 =
	TEXT("matches_found:0 here does not prove the symbol is unused: ");

inline constexpr const TCHAR* RB_check_node_references_5 =
	TEXT(". coverage_complete refers to the asset sweep, not the node sweep. Most likely the symbol is referenced in another form: the ")
	TEXT("assets_hit_prefilter_without_graph_match list says where to look (get_blueprint_functions on those assets, or a binary grep of the .uasset ")
	TEXT("for the real name). An honest zero comes with that list empty and no skipped subgraphs. The scan also cannot see Level Script Blueprints ")
	TEXT("inside a .umap, C++ callers, runtime-built names or DataRegistry data; to prove an absence, add a binary grep over Content/, Config/, ")
	TEXT("Source/ and Plugins/.");

inline constexpr const TCHAR* RB_check_read_after_compile_0 =
	TEXT("This session compiled %s, and compiling does not rebuild actors already instanced in the editor world (PIE duplicates that world, stale ")
	TEXT("state included), so this read may show the old value while the template has the new one. Before concluding that the actor has an instance ")
	TEXT("override, run list_instance_overrides: override_count:0 means the problem is stale state. To refresh without touching the map, use ")
	TEXT("set_component_property(..., verify_persistence=true), which re-runs construction from the template, or reload the level.");

inline constexpr const TCHAR* RB_check_delete_after_pie_census_0 =
	TEXT("This session counted actors in a PIE world (%s), and that count is not the map's: PIE only holds what World Partition has loaded around the ")
	TEXT("player. Do not decide what to delete from it. Count the map in the editor world anchored by package (find_object + ")
	TEXT("GameplayStatics.get_all_actors_of_class), and filter bulk deletes by exact label, never by class.");

inline constexpr const TCHAR* RB_check_interface_sweep_0 =
	TEXT("%d class(es) found with scanned_class_count:%d does not prove uniqueness: that counter measures cost, not coverage, and the sweep only sees ")
	TEXT("loaded classes, so a vendor class that is not in the open map is missed. Do a second pass that does not depend on loading ")
	TEXT("(find_blueprint_node_references on an interface function, get_asset_referencers on the interface, or a binary grep). Before declaring a ")
	TEXT("class missing, check whether the vendor has a child of the one you use (get_blueprint_parent).");

inline constexpr const TCHAR* RB_check_default_map_aggregate_0 =
	TEXT("memory_matches_disk:false is an AND over both keys: here %s matches disk, and what flips the verdict is %s. A project may deliberately keep ")
	TEXT("a key such as EditorStartupMap out of the .ini while memory holds the engine default, in which case this is the correct state. Read ")
	TEXT("disk_checks[] and use the disk_matches_memory of the key you care about.");

inline constexpr const TCHAR* RB_check_default_map_aggregate_1 =
	TEXT(" Note that %s does not match as text but is the same asset (package form versus package.object): a format difference, not a value ")
	TEXT("difference.");

inline constexpr const TCHAR* RB_check_blind_touch_target_0 =
	TEXT("small_touch_target:0 does not mean every button is large enough: sizes are only read from a UCanvasPanelSlot with an authored size. Buttons ")
	TEXT("in vertical or horizontal box slots are sized at runtime and skipped without any trace in the summary (unlike contrast_unknown). Positive ")
	TEXT("control: repeat with min_touch_size:9999, which every measurable button must fail -- buttons missing from that result are the blind spot.");

inline constexpr const TCHAR* RB_check_zero_wp_actors_0 =
	TEXT("A 0 here does not prove the actor is not placed: placed actors have been missed by filter_class, name_contains and even the full ")
	TEXT("untruncated listing, with nothing in the response to flag it. Use find_actors_by_name (it searches by label) to answer 'is it placed?'. Do ")
	TEXT("not count an area by outliner folder either; its content can be spread across folders.");

inline constexpr const TCHAR* RB_check_spawn_without_authority_0 =
	TEXT("The actor is a client-only ghost (%s, net_mode:%s): it does not exist on the server or for other players and has no AI controller, yet its ")
	TEXT("payload looks like a good one. `has_authority` can still be true, because an actor spawned locally in a client world has authority on that ")
	TEXT("machine. Destroy it and spawn with world='server'. There is no Python path for an authoritative spawn (World.spawn_actor is not exposed): ")
	TEXT("use a game spawner, place the actor in the level, or run the CheatManager `summon` command with the server's PlayerController.");

inline constexpr const TCHAR* RB_check_replication_audit_0 =
	TEXT("replicated_class_count:0 alongside %d declared replication signal(s) (RPCs or variables) points to a blind column, not broken networking: ")
	TEXT("class_replicates casts the CDO to AActor, which fails for every component Blueprint, so it always reports false for them. Trust the ")
	TEXT("variables and RPCs in this report instead. For one component's flag, read bReplicates on the instance inside the actor (get_properties ")
	TEXT("with object_path='<ActorInstance>.<Component>'), not on the component class CDO, which defaults to false; an SCS component routes RPCs by ")
	TEXT("its stable name even when bReplicates is false.");

inline constexpr const TCHAR* RB_check_ambiguous_world_read_0 =
	TEXT(" Here it is a read of a different thing: the navmesh is server-authoritative, so a client world can report \"No navigation system in this ")
	TEXT("world\" while the server has a live RecastNavMesh.");

inline constexpr const TCHAR* RB_check_ambiguous_world_read_1 =
	TEXT("An ambiguous world selector was used and the read landed in a world without authority (net_mode:%s, world_package:%s). With 2+ players, ")
	TEXT("'pie'/'game' may resolve to different worlds for different tools, so combining their reads produces contradictions and false zeros.%s ")
	TEXT("Repeat with world='server' (or an explicit 'pie:N') and label each networked read with its UEDPIE_N world.");

inline constexpr const TCHAR* RB_check_subsystem_list_editor_0 =
	TEXT("All %d instances are native (/Script/), which does not mean there are no Blueprint subsystems: this tool only walks the editor world, and a ")
	TEXT("Blueprint WorldSubsystem exists once per world, in PIE. To see them, use execute_python with ObjectIterator filtered by ")
	TEXT("get_class().get_name() over the anchored PIE world; to call them, load_object by path + call_method.");

inline constexpr const TCHAR* RB_check_flat_save_files_0 =
	TEXT("%d folder(s) under the save directory hold .sav files that save_games does not list as slots (%s): a save system can keep a slot as a ")
	TEXT("folder, so the slot list alone does not prove there is no saved game. If a save exists, persistent values you read in PIE are loaded ")
	TEXT("from it, not initial values -- find out where a value comes from before interpreting it.");

inline constexpr const TCHAR* RB_check_ghost_map_0 =
	TEXT("already_exists:true comes from the Asset Registry, not the disk: an orphan package from an earlier creation (or a duplicate left by ")
	TEXT("save_level(filename=...)) reads like a real map. Check the disk for the .umap (%s); if it is not there, the map does not exist.");

inline constexpr const TCHAR* RB_check_complex_row_0 =
	TEXT("%d cell(s) come back empty (%s), which does not necessarily mean 'not written'. Index 0 of a UserDefinedEnum exports as an empty string ")
	TEXT("(get_data_table_rows shows the display name, or write a non-zero enumerator to tell them apart), and empty fields of a nested struct are ")
	TEXT("not printed, so `fields` is not the schema. For the schema, ask the struct: get_struct_members does not accept the bare name '%s', so first ")
	TEXT("run resolve_asset_by_name('%s') and pass the full path; its var_name matches the hashed names shown here.");

inline constexpr const TCHAR* RB_check_smart_object_registry_0 =
	TEXT("This is what the subsystem has registered, not what the property says: the registration is fixed when the actor is created and only ")
	TEXT("refreshes on re-registration or level reload, so in the editor it can lag behind a DefinitionRef you just wrote. To know which definition ")
	TEXT("will be used on load, read the component's DefinitionRef with get_properties.");

inline constexpr const TCHAR* RBK_PYTHON_SAVE_WARNING_0 =
	TEXT("from Python, its return value is not the save oracle. On a read-only .uasset it returns False but the change stays in memory, so re-reading ")
	TEXT("the asset looks like success; it has also returned True without writing the file, and with only_if_is_dirty=False it can rewrite files you ")
	TEXT("did not change. Verify on disk (binary grep of the new value + git status), or use the MCP tool save_asset, which reports saved, ")
	TEXT("still_dirty_after_save and file_on_disk.");

inline constexpr const TCHAR* RBK_LYING_APIS_0 =
	TEXT("set_material_instance_static_switch_parameter_value");

inline constexpr const TCHAR* RBK_LYING_APIS_1 =
	TEXT("render_target_create_static_texture2d_editor_only");

inline constexpr const TCHAR* RBK_LYING_APIS_2 =
	TEXT("does not clear the module namespace: it runs the new file on top of the old one, so anything removed from the file is still there and a ")
	TEXT("damaged module keeps 'working' (ast.parse and the reload both stay green). Compare the symbols before and after with ast, or diff the `def` ")
	TEXT("lines against git, and cut code blocks by AST node ranges rather than with a regex.");

inline constexpr const TCHAR* RBK_LYING_APIS_3 =
	TEXT("returns success but, because import_whole_rig defaults to True, imports the archetype's whole rig and can silently corrupt the proportions; ")
	TEXT("can_build may stay false, so it is no shortcut around the autorig. If you need it, pass import_whole_rig=False and measure the mesh before ")
	TEXT("and after.");

inline constexpr const TCHAR* RBK_LYING_APIS_4 =
	TEXT("is a valid way to simulate a player hit from Python in PIE (it runs the whole Blueprint damage chain), but it fails silently if ")
	TEXT("event_instigator is not a Controller: logic that looks the attacker up from the instigator stops at an Is Valid. Signature: (actor, damage, ")
	TEXT("direction, HitResult(), PlayerController, pawn, DamageType). apply_damage enters a different chain, so one may work where the other does ")
	TEXT("not; the oracle is the hit actor's state, not the return value.");

inline constexpr const TCHAR* RBK_LYING_APIS_5 =
	TEXT("reports differences between two untouched MetaHumanCharacters from the same factory (tolerance=0.01), so 'did the body change?' always ")
	TEXT("reads as yes. Use list_dirty_packages with path_prefix instead: a mutation that does not dirty the package did not happen. Before trusting ")
	TEXT("any comparator, run it on two objects you know are equal.");

inline constexpr const TCHAR* RBK_LYING_APIS_6 =
	TEXT("(SkeletonModifier) is a good way to read the whole bone hierarchy in one call (get_all_bone_names + get_parent_name, without the 100/20 ")
	TEXT("caps of get_skeleton_info and get_skeletal_mesh_info), but it is a mutating API: use only its readers and never call commit. It reports the ")
	TEXT("mesh's bones, not the Skeleton asset's, so bone counts can legitimately differ between tools; virtual bones are not exposed by any path.");

inline constexpr const TCHAR* RBK_LYING_APIS_7 =
	TEXT("does not exist on GameplayTagLibrary in 5.8, and unreal.GameplayTag(tag_name=...) fails too. Build a tag with t = unreal.GameplayTag(); ")
	TEXT("t.import_text('(TagName=\"A.B\")') and verify it with t.get_editor_property('tag_name') -- print(t) shows {} whether or not it is set.");

inline constexpr const TCHAR* RBK_LYING_APIS_8 =
	TEXT("with a read-only .umap returns True and writes a duplicate world as a .uasset next to it (even when the map was not dirty); if committed, ")
	TEXT("the next session opens the wrong world. Check git status after every call and delete the stray file (the real changes live in the OFPA ")
	TEXT("packages). If the map itself must change, take its git lfs lock first. It also saves every dirty package, including vendor packages dirtied ")
	TEXT("as a side effect.");

inline constexpr const TCHAR* RBK_LYING_APIS_9 =
	TEXT("is the right synchronous call from Python, but its result is an InstancedStruct whose to_dict() returns [] even when assembly succeeded; ")
	TEXT("read it with export_text(). Also, is_assembled() is False right after touching a slot (normal), and get_existing_assembly_output() returns ")
	TEXT("an empty structure rather than None.");

inline constexpr const TCHAR* RBK_LYING_APIS_10 =
	TEXT("does not tell whether a point is in the water: it extrapolates the body's surface over the whole world and answers for any location ")
	TEXT("(positive depths far from any water), and its bool return does not reach Python. Use it only for 'what is the water surface height at this ")
	TEXT("XY?'. For containment, overlap the water body's real collision (SphereOverlapActors against the WaterBodyCollision profile) and, if several ")
	TEXT("bodies contain the point, take the one with the smallest actor bounds.");

inline constexpr const TCHAR* RBK_LYING_APIS_11 =
	TEXT("returns True no matter what. A TMap<Enum,SoftClass> exported with the export wrapper does not re-import (it becomes an empty ()) and still ")
	TEXT("returns True. Pass bare paths ({'Key': '/Game/....Class_C'}) and re-read with get_data_table_column or get_data_table_row ")
	TEXT("(include_export_text=true adds each cell's ExportText). File size is no proof of integrity.");

inline constexpr const TCHAR* RBK_LYING_APIS_12 =
	TEXT("returns None in three ways that do not mean 'no navmesh here': with an explicit nav_data it always returns None (use nav_data=None); ")
	TEXT("filter_class must be the Python type unreal.NavigationQueryFilter (None or .static_class() fail with 'Failed to convert parameter'); and ")
	TEXT("query_extent defaults to (0,0,0). Before concluding anything, run a positive control on a point known to have navmesh (e.g. the player's ")
	TEXT("position).");

inline constexpr const TCHAR* RBK_LYING_APIS_13 =
	TEXT("on a World Partition map can write a duplicate <Map>.uasset next to <Map>.umap and return True; check the folder after each call and delete ")
	TEXT("the stray file (the real changes are in the OFPA packages). With an OFPA landscape it can also block the game thread for minutes while ")
	TEXT("Nanite meshes build: MCP calls time out, but that does not mean the save failed -- do not retry; watch the files on disk, Saved/Logs and ")
	TEXT("the editor process instead.");

inline constexpr const TCHAR* RBK_LYING_APIS_14 =
	TEXT("wants a DataLayerInstance, not the UDataLayerAsset its signature suggests (\"Cannot nativize 'DataLayerAsset' as ... DataLayerInstance\"). ")
	TEXT("Get the instance first: add_actor_to_data_layer(actor, subsystem.get_data_layer_instance(asset)).");

inline constexpr const TCHAR* RBK_LYING_APIS_15 =
	TEXT("returning False does not mean the MetaHuman Creator UI is required: check the log for 'Texture Synthesis has not been initialized', a ")
	TEXT("missing plugin component that blocks the UI build too. build_meta_human returns nothing and may write nothing. The build oracle is new ")
	TEXT("assets appearing (asset registry scan plus the disk), not the return value or can_build.");

inline constexpr const TCHAR* RBK_LYING_APIS_16 =
	TEXT("only accepts MetaHuman rigs even though it takes a vertex cloud: other skeletons fail (INVALID_INPUT_BONES) because the joint count does ")
	TEXT("not match. Run the same call on the MetaHuman archetype as a positive control before calling the tool broken.");

inline constexpr const TCHAR* RBK_LYING_APIS_17 =
	TEXT("does not re-assemble; it only invalidates the output, so is_assembled() is False right after it (normal). Assemble explicitly after ")
	TEXT("changing slots.");

inline constexpr const TCHAR* RBK_LYING_APIS_18 =
	TEXT("registers the asset for editing and never releases it on its own; until you call remove_object_to_edit(asset), the editor cannot open it ")
	TEXT("and reports 'The asset may be corrupted' (it is not). Always release it when done (is_object_added_for_editing should go back to False).");

inline constexpr const TCHAR* RBK_LYING_APIS_19 =
	TEXT("reports the requested constraint values, not what the body measures, and keeps them after the body is rebuilt. Measure the mesh instead ")
	TEXT("(get_mesh_data_for_conforming, max(z) - min(z); for a MetaHuman add the head mesh to the body). Centimeter-scale changes in other ")
	TEXT("constraints indicate the solver actually ran.");

inline constexpr const TCHAR* RBK_LYING_APIS_20 =
	TEXT("returns the reference pose on the first evaluation after the asset loads, with no warning, so a loop that evaluates each asset once gets ")
	TEXT("the reference pose for all of them. Do a warm-up evaluation and discard it. It is reliable for orientation, not for fine offsets.");

inline constexpr const TCHAR* RBK_LYING_APIS_21 =
	TEXT("does not freeze an AI-driven NPC (nor does max_walk_speed = 0): movement resumes on the next AI request, while a quick re-read can still ")
	TEXT("look 'stopped'. Stop the AI itself if you need the NPC still, and never use 'it did not move much' as proof.");

inline constexpr const TCHAR* RBK_LYING_APIS_22 =
	TEXT("writes the profile name without applying its full response table, so the live responses can differ from the profile, and fixing the class ")
	TEXT("does not update already placed instances. Set collision explicitly ('Custom' + set_collision_response_to_all_channels + ")
	TEXT("set_collision_response_to_channel) and verify per channel with get_collision_response_to_channel.");

inline constexpr const TCHAR* RBK_LYING_APIS_23 =
	TEXT("with WorldStatic/WorldDynamic object types near the ground returns the ground (the sphere overlaps the floor from the start and a single ")
	TEXT("trace returns the closest hit), so damage logic silently does nothing. Exclude WorldStatic and WorldDynamic when looking for bodies, and ")
	TEXT("check the ImpactPoint.Z of a manual trace.");

inline constexpr const TCHAR* RBK_LYING_APIS_24 =
	TEXT("returns ConstraintInstanceAccessor handles that carry no bone data, and the physics asset's body and constraint setups are not readable ")
	TEXT("from Python. Use the MCP tool get_physics_asset_info (bodies, constraints with child/parent bones, skeleton bones in hierarchy order).");

inline constexpr const TCHAR* RBK_LYING_APIS_25 =
	TEXT("with an empty string produces the FName 'None', not an empty name; a consumer such as Open Level (by Name) then tries to load 'None' and ")
	TEXT("hangs without an error. Guard against empty strings before converting.");

inline constexpr const TCHAR* RBK_LYING_APIS_26 =
	TEXT("on a component returns 'Actor.Component', not the component name, so name comparisons never match. Use GetObjectName for the bare name, and ")
	TEXT("verify by calling the function and checking its output.");

inline constexpr const TCHAR* RBK_LYING_APIS_27 =
	TEXT("does not exist (AttributeError) even though the property does; use box.get_editor_property('hint_text'). In UMG, do not assume a getter ")
	TEXT("exists because the setter does.");

inline constexpr const TCHAR* RBK_LYING_APIS_28 =
	TEXT("is asymmetric: HasAnyTags(A, B, exact=False) matches only when a tag in A equals or descends from a tag in B, so swapping the arguments ")
	TEXT("changes the result. Check the argument order at each call site.");

inline constexpr const TCHAR* RBK_LYING_APIS_29 =
	TEXT("returns False even for mapping contexts that demonstrably work, so it proves nothing about the binding. Test it first on a context known to ")
	TEXT("work; key bindings need to be verified in play.");

inline constexpr const TCHAR* RBK_LYING_APIS_30 =
	TEXT("does not exist in 5.8 Python and is not needed: set_editor_property already sends the change event. Verify by re-reading the asset after ")
	TEXT("save_asset (for textures: srgb, compression_settings, lod_group).");

inline constexpr const TCHAR* RBK_LYING_APIS_31 =
	TEXT("is not exposed, and the HitResult returned by line_trace_single cannot be read from Python (its fields are missing, protected or not ")
	TEXT("found), so traces always look like misses. Use raycast_scene / find_scene_overlaps over MCP, which return real hit data and accept world='server'.");

inline constexpr const TCHAR* RBK_LYING_APIS_32 =
	TEXT("on an AIPerceptionComponent says nothing about whether perception works: perception is driven by the central AIPerceptionSystem. Use ")
	TEXT("get_currently_perceived_actors(sense) and the observed behavior.");

inline constexpr const TCHAR* RBK_LYING_APIS_33 =
	TEXT("can return False for a sense that is delivering stimuli in the same call. Use get_currently_perceived_actors(sense) and the observed ")
	TEXT("behavior.");

inline constexpr const TCHAR* RBK_LYING_APIS_34 =
	TEXT("cannot distinguish 'never ragdolled' from 'ragdolled and already frozen' (a ragdoll freeze can stop the simulation). Read the ragdoll state ")
	TEXT("variables of the instance's ragdoll component instead.");

inline constexpr const TCHAR* RBK_LYING_APIS_35 =
	TEXT("returns only the actor list in Python (no bool), so indexing hits[1] fails exactly when something was found. Treat the result as a list, ")
	TEXT("and if a probe returns an unexpected type, print the type and stop instead of substituting a default.");

inline constexpr const TCHAR* RBK_LYING_APIS_36 =
	TEXT("is clamped to AWorldSettings::MaxGlobalTimeDilation (default 20) without warning. Read the value back (get_global_time_dilation, or ")
	TEXT("time_dilation from get_world_settings) and raise MaxGlobalTimeDilation if needed. High dilation distorts fast timers, so use it to observe ")
	TEXT("the shape of a cycle, never to measure durations.");

inline constexpr const TCHAR* RBK_LYING_APIS_37 =
	TEXT("is always zero for crowd-following NPCs (CrowdFollowingComponent uses RequestDirectMove, not AddMovementInput). Use ")
	TEXT("controller.get_move_status() together with movement.get_last_update_requested_velocity(): MOVING with zero requested velocity is the real ")
	TEXT("'stuck' signature.");

inline constexpr const TCHAR* RBK_LYING_APIS_38 =
	TEXT("does not list every reflected property of an Unreal object, so a dir() sweep misses properties that exist (often inside structs). Use ")
	TEXT("get_properties / get_object_info (include_property_metadata=true) over MCP, or try the CamelCase name directly with get_editor_property.");

inline constexpr const TCHAR* RBK_LYING_APIS_40 =
	TEXT("argument passing fails silently in several ways: arrays of structs of the wrong type arrive empty; a GameplayTag passed where a Blueprint ")
	TEXT("struct is expected arrives as an empty struct; Blueprint user structs and enums have no Python type (an int index fails); and positional ")
	TEXT("arguments of the same type are not checked for order. Check the real order with get_function_info, and prefer the MCP tool call_function ")
	TEXT("(on an object_path, a class_name or a component_class), which marshals through the bridge and accepts enums by value name. Passing a UObject argument ")
	TEXT("has crashed the editor. Calls made this way do not mark packages dirty (a following save_level writes nothing), and RPCs called from Python ")
	TEXT("do not cross the network: they run locally, so verify state in both worlds and drive networked flows with real input.");

inline constexpr const TCHAR* RBK_LYING_APIS_42 =
	TEXT("returns True even when it writes nothing or garbage: unreal.Guid only accepts 32 hex digits without dashes; object references in a ")
	TEXT("HitResult are not imported; unregistered GameplayTags become None; nested struct arrays need the GUID-suffixed member names; and editing an ")
	TEXT("element of an array obtained from Python changes nothing (use set_editor_property with the visible names and reassign the array). Verify ")
	TEXT("with export_text(). It is still the only way to build a GameplayTag from Python.");

inline constexpr const TCHAR* RBK_LYING_APIS_43 =
	TEXT("on a Blueprint struct returns {} because its fields carry GUID suffixes, which does not mean nothing was written. Read with ")
	TEXT("get_object_info (include_property_metadata=true), get_editor_property using the GUID-suffixed name, or get_data_table_row with ")
	TEXT("include_export_text=true. Never compare structs with str() ")
	TEXT("(it includes the pointer); compare fields.");

inline constexpr const TCHAR* RBK_LYING_APIS_44 =
	TEXT("returns 0.0 for any name that does not exist, with no error. Confirm the name with list_cvars (filter), or use the ")
	TEXT("get_cvar tool, which fails honestly.");

inline constexpr const TCHAR* RBK_LYING_APIS_45 =
	TEXT("can leave expressions behind without an error, and the material still compiles. Count them with get_material_graph_info (expression_count) afterwards.");

inline constexpr const TCHAR* RBK_LYING_APIS_46 =
	TEXT("returns False even when the change was applied. Re-read with get_material_instance_static_switch_parameter_value.");

inline constexpr const TCHAR* RBK_LYING_APIS_47 =
	TEXT("returns [] for freshly assigned actors even when the assignment succeeded. Read actor.get_editor_property('data_layer_assets') instead.");

inline constexpr const TCHAR* RBK_LYING_APIS_48 =
	TEXT("does not count the world's OFPA packages, so 0 does not mean nothing needs saving. Use the list_dirty_packages tool and git status.");

inline constexpr const TCHAR* RBK_LYING_APIS_49 =
	TEXT("returns None while PIE is running (and get_all_level_actors() returns 0), which looks like a failure. Anchor by package instead: ")
	TEXT("unreal.find_object(None, '/Game/Maps/MainMap.MainMap').");

inline constexpr const TCHAR* RBK_LYING_APIS_50 =
	TEXT("creates the texture in memory only, even if its path is under /Game. Save it (save_asset) and verify the file on disk.");

inline constexpr const TCHAR* RBK_LYING_APIS_51 =
	TEXT("does not dirty the package: without col.modify(True) + save_loaded_asset nothing reaches disk, although re-reading shows the in-memory ")
	TEXT("state. There is no API to remove items from the collection, so check before adding. Adding a head does not fill the Body slot; add the same ")
	TEXT("head item to Body too, or Body silently stays empty.");

inline constexpr const TCHAR* RBK_LYING_APIS_52 =
	TEXT("returns (0,0,0) both for a missing bone or socket and for a mesh reinstanced after a compile (no evaluated pose); attaching to a missing ")
	TEXT("socket also fails silently and snaps to the component origin. Check get_reference_pose().get_bone_names() and compare with the asset pose; ")
	TEXT("reloading the level fixes a reinstanced mesh.");

inline constexpr const TCHAR* RBK_LYING_APIS_53 =
	TEXT("inside execute_python blocks the game thread, so the PIE world does not tick while the script sleeps and every 'spaced' sample in one ")
	TEXT("script is the same snapshot. Take each sample in a separate execute_python call and wait on the client side; an unfocused editor is also ")
	TEXT("throttled, so less game time passes than wall time.");

inline constexpr const TCHAR* RBK_LYING_APIS_54 =
	TEXT("on a Chaos wheeled vehicle destabilizes it violently instead of moving it. It is only useful to let speed-reading logic see a velocity ")
	TEXT("within the same call (reset it to 0 afterwards). To drive, possess the vehicle and hold an input key on the host; avoid physics ")
	TEXT("manipulation when measuring networked state.");

inline constexpr const TCHAR* RBK_LYING_APIS_55 =
	TEXT("valid=True can still be a partial path that stops where the navmesh ends -- the usual failure with navigation invokers. Always check ")
	TEXT("is_partial(), and compare get_path_length() with the straight-line distance.");

inline constexpr const TCHAR* RBK_LYING_APIS_56 =
	TEXT("valid=True can still be a partial path that stops where the navmesh ends -- the usual failure with navigation invokers. Always check ")
	TEXT("is_partial(), and compare get_path_length() with the straight-line distance.");

inline constexpr const TCHAR* RBK_LYING_APIS_57 =
	TEXT("is the wrong base for measuring a game clock's rate: when the editor is overloaded, game time falls behind wall time and the clock looks ")
	TEXT("frame-rate dependent. Measure rates against get_time_seconds (game time).");

inline constexpr const TCHAR* RBK_LYING_APIS_58 =
	TEXT("does not clear OverrideMaterials, which apply by slot index, so the previous mesh's material override stays on the new mesh. Set all ")
	TEXT("overrides to None before swapping meshes (while the old mesh, with all its slots, is still assigned).");

inline constexpr const TCHAR* RBK_LYING_APIS_59 =
	TEXT("spline points written from Python re-read correctly but are reset to the Blueprint template the next time the construction script runs (a ")
	TEXT("set_actor_location is enough to trigger it). Also, add_spline_point with update_spline=False can leave zero points after a final ")
	TEXT("update_spline(); pass True on every add. To change the geometry persistently, change the actor's scale or the Blueprint defaults, and ")
	TEXT("verify after a construction-script rerun.");

inline constexpr const TCHAR* RBK_LYING_APIS_60 =
	TEXT("spline points written from Python re-read correctly but are reset to the Blueprint template the next time the construction script runs (a ")
	TEXT("set_actor_location is enough to trigger it). Also, add_spline_point with update_spline=False can leave zero points after a final ")
	TEXT("update_spline(); pass True on every add. To change the geometry persistently, change the actor's scale or the Blueprint defaults, and ")
	TEXT("verify after a construction-script rerun.");

inline constexpr const TCHAR* RBK_LYING_APIS_61 =
	TEXT("its return value and get_move_status are no proof of movement: an NPC whose CharacterMovement was disabled (e.g. by a distance-based ")
	TEXT("optimization component) accepts the request and never moves. Read the position in separate calls.");

inline constexpr const TCHAR* RBK_LYING_APIS_62 =
	TEXT("returns True without unloading a package held by a WorldContext (find_package still finds it), and Python has no way to release it. Check ")
	TEXT("find_package(<path>) after the call; restarting the editor is the only known remedy.");

inline constexpr const TCHAR* RBK_LYING_APIS_63 =
	TEXT("on a live component in PIE does nothing and does not warn (other properties of the same component do write). Set character materials from ")
	TEXT("the Blueprint; use headless access only to read them.");

inline constexpr const TCHAR* RBK_LYING_APIS_64 =
	TEXT("reflects the movement mode computed on the CMC's next tick, so reading it right after a teleport is unreliable. Check GetPhysicsVolume ")
	TEXT("(immediate) or find_scene_overlaps instead, and read IsSwimming in a later call.");

inline constexpr const TCHAR* RBK_LYING_APIS_65 =
	TEXT("order is not possession order, so 'the first character' may be the wrong player; use PlayerController.get_controlled_pawn(). When comparing ")
	TEXT("server and client worlds, remember that execute_python blocks the game thread: nothing replicates within the same call, so mutate in one ")
	TEXT("call and read the client in the next. The same player's objects have different names in each world.");

inline constexpr const TCHAR* RBK_LYING_APIS_66 =
	TEXT("fails on an asset that is open in an editor (a freshly created asset is opened automatically), with a noisy ensure that looks like a crash, ")
	TEXT("and leaves it dirty in memory. Close it with AssetEditorSubsystem.close_all_editors_for_asset and delete it in a separate call; verify with ")
	TEXT("does_asset_exist and the file on disk, not the return value.");

inline constexpr const TCHAR* RBK_LYING_APIS_67 =
	TEXT("from Python changes the actor and re-reads fine, but does not mark its OFPA package dirty, so save_level reports success without writing it ")
	TEXT("and the change is lost on reload (list_dirty_packages shows 0 either way). Use the MCP tool set_actor_transform, which calls Modify(), and ")
	TEXT("check git status on the actor's external package. Long teleports of a pawn in PIE (hundreds of meters) can end PIE or crash the editor on ")
	TEXT("teardown; afterwards reads fail as if the actor had vanished, so check is_play_in_editor_active first.");

inline constexpr const TCHAR* RBK_LYING_APIS_68 =
	TEXT("from Python changes the actor and re-reads fine, but does not mark its OFPA package dirty, so save_level reports success without writing it ")
	TEXT("and the change is lost on reload (list_dirty_packages shows 0 either way). Use the MCP tool set_actor_transform, which calls Modify(), and ")
	TEXT("check git status on the actor's external package. Long teleports of a pawn in PIE (hundreds of meters) can end PIE or crash the editor on ")
	TEXT("teardown; afterwards reads fail as if the actor had vanished, so check is_play_in_editor_active first.");

inline constexpr const TCHAR* RBK_LYING_APIS_69 =
	TEXT("from Python changes the actor and re-reads fine, but does not mark its OFPA package dirty, so save_level reports success without writing it ")
	TEXT("and the change is lost on reload (list_dirty_packages shows 0 either way). Use the MCP tool set_actor_transform, which calls Modify(), and ")
	TEXT("check git status on the actor's external package. Long teleports of a pawn in PIE (hundreds of meters) can end PIE or crash the editor on ")
	TEXT("teardown; afterwards reads fail as if the actor had vanished, so check is_play_in_editor_active first.");

inline constexpr const TCHAR* RBK_LYING_APIS_70 =
	TEXT("from Python changes the actor and re-reads fine, but does not mark its OFPA package dirty, so save_level reports success without writing it ")
	TEXT("and the change is lost on reload (list_dirty_packages shows 0 either way). Use the MCP tool set_actor_transform, which calls Modify(), and ")
	TEXT("check git status on the actor's external package. Long teleports of a pawn in PIE (hundreds of meters) can end PIE or crash the editor on ")
	TEXT("teardown; afterwards reads fail as if the actor had vanished, so check is_play_in_editor_active first.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_0 =
	TEXT("setting bDynamicObstacle with an AreaClass on a primitive component re-reads correctly but does not carve the navmesh, and ")
	TEXT("bUseSystemDefaultObstacleAreaClass defaults to true, which replaces your AreaClass with the walkable NavArea_Obstacle. Use a ")
	TEXT("NavModifierComponent or a NavModifierVolume (spawn_box_volume volume_class=NavModifierVolume, then its AreaClass with set_properties) instead, and verify with the route ")
	TEXT("(find_path_to_location_synchronously before and after: path length and number of path points), not with the properties.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_1 =
	TEXT("setting bDynamicObstacle with an AreaClass on a primitive component re-reads correctly but does not carve the navmesh, and ")
	TEXT("bUseSystemDefaultObstacleAreaClass defaults to true, which replaces your AreaClass with the walkable NavArea_Obstacle. Use a ")
	TEXT("NavModifierComponent or a NavModifierVolume (spawn_box_volume volume_class=NavModifierVolume, then its AreaClass with set_properties) instead, and verify with the route ")
	TEXT("(find_path_to_location_synchronously before and after: path length and number of path points), not with the properties.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_5 =
	TEXT("that property does not exist, so its absence says nothing about the landscape. The real property is 'layers': get_editor_property('layers') ")
	TEXT("on the Landscape actor (an empty list means no edit layers). Without edit layers, landscape patch brushes deform nothing, with no warning.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_6 =
	TEXT("registering an HLOD layer has no headless path: no tool writes the array, Python cannot see runtime_partitions, and writing it as JSON can ")
	TEXT("null the instanced subobjects. Do it in the editor UI, and verify with run_map_check, which reports 'invalid HLOD layer' per actor until ")
	TEXT("the layer is registered.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_10 =
	TEXT("is protected and not reachable from Python, so a widget census through it silently prints 0 with success:true. Use ")
	TEXT("get_widget_blueprint_tree, audit_widget_style or audit_widget_accessibility. In any execute_python census, print one line per subject, not ")
	TEXT("only a total.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_15 =
	TEXT("an empty result means the PCG graph has no overridable parameters: its meshes, densities and exclusions can only change by editing the ")
	TEXT("graph asset itself. Check this before planning, and check whether the graph uses its input node at all (get_pcg_graph_info) before feeding ")
	TEXT("it data as a subgraph.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_16 =
	TEXT("is the cheap way to assign an HLOD layer, but the actors still read None (and get_hlod_layer_actors returns 0) because they resolve it ")
	TEXT("implicitly. Verify with run_map_check, and register the layer in the HLOD setup first (editor UI); otherwise every actor degrades to ")
	TEXT("ForcedNoHLODLayer with a warning.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_17 =
	TEXT("turning on tessellation does not rebuild the landscape's Nanite meshes (displacement is applied at runtime); only changing the heights ")
	TEXT("does. Count 'Built static mesh' lines in Saved/Logs instead of trusting get_landscape_nanite_status, and compare get_material_stats before ")
	TEXT("and after to see the base-pass cost.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_18 =
	TEXT("the ocean collision box has its top at the water surface and a Z half-extent equal to collision_extents.z, so with the default value the ")
	TEXT("water ends 2*z below the surface and any deeper seabed has no water. Read the box component (box_extent + location) and size it against the ")
	TEXT("lowest point of the heightmap.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_19 =
	TEXT("setting it to 0 does not freeze an AI-driven NPC; movement resumes on the next request. Do not treat 'it did not move much' as proof that ")
	TEXT("it is frozen.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_20 =
	TEXT("is always zero for crowd-following NPCs (they move with RequestDirectMove). Use controller.get_move_status() together with ")
	TEXT("movement.get_last_update_requested_velocity().");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_21 =
	TEXT("written on the RecastNavMesh actor, this succeeds but is overwritten from SupportedAgents when the world initializes (the engine only logs ")
	TEXT("a warning). Change the agent on the NavigationSystemV1 CDO (supported_agents) and restart PIE.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_22 =
	TEXT("written on the RecastNavMesh actor, this succeeds but is overwritten from SupportedAgents when the world initializes (the engine only logs ")
	TEXT("a warning). Change the agent on the NavigationSystemV1 CDO (supported_agents) and restart PIE.");

inline constexpr const TCHAR* RBK_LYING_ARGUMENTS_23 =
	TEXT("is not exposed to Python ('Failed to find property'); inside a try/except sweep it yields 0 components for every Blueprint, and the CDO ")
	TEXT("only lists native components. Use get_blueprint_components (native + SCS + inherited) or the live actor in PIE. From Python, ")
	TEXT("SubobjectDataSubsystem also returns inherited components, but their templates may live in a library package: write inherited defaults with ")
	TEXT("set_blueprint_component_properties (which creates the override in the child), and prefer get_associated_object over the deprecated get_object.");

inline constexpr const TCHAR* RBK_LYING_APIS_DURING_PIE_0 =
	TEXT("(EditorAssetLibrary) returns None while PIE is running, by design (the reason only goes to the engine log), and a None passed on turns into ")
	TEXT("a plausible zero (e.g. get_data_table_row_names(None) returns []). With PIE running, use the module-level unreal.load_asset or anchor by ")
	TEXT("package with unreal.find_object(None, '/Game/.../Map.Map').");

inline constexpr const TCHAR* RBK_LYING_APIS_DURING_PIE_1 =
	TEXT("returns None while PIE is running, and get_all_actors_of_class(world, None) then returns [], an empty census that is an artifact. Use ")
	TEXT("unreal.load_class(None, '/Game/....X_C') or unreal.load_object(None, '<path>.<Name>_C'), which work with PIE running.");

inline constexpr const TCHAR* RBK_LYING_APIS_DURING_PIE_2 =
	TEXT("returns [] while PIE is running, by design, which reads as an empty level. Count on the intended world with ")
	TEXT("GameplayStatics.get_all_actors_of_class, anchoring the UWorld by its package.");

inline constexpr const TCHAR* RB_run_output_text_0 =
	TEXT("\n");

inline constexpr const TCHAR* RB_check_python_code_0 =
	TEXT("listing components on the CDO only returns native components; SCS components are not there, so this cannot prove that a component is ")
	TEXT("missing. Use get_blueprint_components, or spawn the actor off camera, read it, and destroy it in a `finally` block.");

inline constexpr const TCHAR* RB_check_python_code_1 =
	TEXT("this error is usually harmless with navigation invokers: MaxTiles limits simultaneously loaded tiles, not the addressable grid, and ")
	TEXT("invokers keep few tiles loaded. Raising TileSizeUU / TileNumberHardLimit as the message suggests makes builds slower without fixing ")
	TEXT("anything. Measure the consequence instead (project_point_to_navigation, find_path_* with is_partial() == False, an agent that actually ")
	TEXT("moves); if tile-pool memory matters, use bFixedTilePoolSize + TilePoolSize.");

inline constexpr const TCHAR* RB_check_python_code_2 =
	TEXT("that is the repr of an unreal.Array (its memory address), not its content: full and empty arrays print the same. Iterate and export_text() ")
	TEXT("each element, use len() for the count, and include a known-full and a known-empty array as a control.");

inline constexpr const TCHAR* RB_check_python_code_3 =
	TEXT("an out-of-range Blueprint byte-enum value (e.g. written by code that increments an enum past its last entry) makes the read raise, which ")
	TEXT("can abort a whole census over one actor. Wrap such reads in try/except and report the value as data; use get_enum_info for the ")
	TEXT("authoritative index/name map.");

inline constexpr const TCHAR* RB_check_python_code_4 =
	TEXT("Blueprint enums are not Python types and an int is not accepted for them, which looks like 'this function cannot be called' when it can. ")
	TEXT("Pass a real enum value taken from data that already holds one (e.g. a key of a TMap typed by that enum), or use the MCP tool ")
	TEXT("call_function, which accepts an enum by its value name.");

inline constexpr const TCHAR* RB_check_python_code_5 =
	TEXT("Python cannot see the bindings of a multicast delegate (it shows a single unbound entry), so this does not prove the dispatcher is unbound. ")
	TEXT("Verify the effect instead: fire it through the game's own path and read the state the handler should change.");

inline constexpr const TCHAR* RB_check_python_code_6 =
	TEXT("the library has no Python binding, but the class exists and is callable: ")
	TEXT("find_object(...Default__WidgetBlueprintLibrary).call_method('Create', (world, cls, pc)) returns the widget. Resolve the class with ")
	TEXT("find_object (load_asset returns None in PIE) and verify the returned object, not success.");

inline constexpr const TCHAR* RB_check_python_code_7 =
	TEXT("do not compare enum values as strings: the repr includes delimiters and the value (<MovementMode.MOVE_SWIMMING: 4>), so str(v) == ")
	TEXT("'MovementMode.MOVE_SWIMMING' is always False. Compare against the enum member (unreal.MovementMode.MOVE_SWIMMING).");

inline constexpr const TCHAR* RB_check_python_code_8 =
	TEXT("the repr of a Python-UE struct does not show its fields: it prints {} whether or not it holds data, so it supports neither 'empty' nor ")
	TEXT("'filled' conclusions. Use export_text() (parse it carefully: values can contain nested quotes) plus get_editor_property for specific ")
	TEXT("fields; Blueprint struct field names carry a _<idx>_<GUID> suffix, so match them by prefix. Include a known-full and a known-empty case as ")
	TEXT("a control.");

inline constexpr const TCHAR* RB_check_python_code_9 =
	TEXT("a deprecated asset property can return an empty container without an error, so a loop over it silently does nothing (e.g. ")
	TEXT("InputMappingContext.mappings is deprecated and empty in 5.8; the data lives in default_key_mappings.mappings). Count before writing, ")
	TEXT("re-read from the asset afterwards, and use get_object_info with include_property_metadata=true to see which properties are deprecated.");

inline constexpr const TCHAR* RB_check_python_code_10 =
	TEXT("this usually does not mean the property is missing: Blueprint variable names may contain spaces and capitals (ask for the exact authored ")
	TEXT("name), Blueprint struct fields carry GUID suffixes (read them through export_text()), and some properties only exist on another class. ")
	TEXT("get_properties / set_properties and get_blueprint_component_property reach Blueprint properties through C++ reflection.");

inline constexpr const TCHAR* RB_check_python_code_11 =
	TEXT("This script calls %d Python API(s) with known silent failure modes: ");

inline constexpr const TCHAR* RB_check_python_code_13 =
	TEXT(" The script may have run fine; what cannot be trusted is reading its results at face value: a 0, [] or None from these APIs is not a ")
	TEXT("measurement.");

inline constexpr const TCHAR* RB_check_table_row_0 =
	TEXT("in-response:problems+unchanged_despite_request+unknown_columns");

inline constexpr const TCHAR* RB_check_table_row_1 =
	TEXT("set_data_table_row_fields: the row was only partially written, as the response itself reports -- %s. The top-level success:true is not the ")
	TEXT("oracle, and changed_columns may list a column that ended up empty. Measured causes: a GameplayTag with spaces must be passed as ExportText ")
	TEXT("(TagName=\"...\"); a TArray of structs passed as a string does not re-import (pass a JSON list keyed by the internal index+GUID names); a ")
	TEXT("sub-field without its GUID suffix lands in unknown_columns and is ignored. Compare row_after_exporttext with what you asked for, use ")
	TEXT("set_data_table_cell for arrays of structs, and restore a damaged row with reload_asset_package(on_dirty=\"discard\") (refused while PIE is ")
	TEXT("active).");

inline constexpr const TCHAR* RB_check_table_row_2 =
	TEXT("All three causes come from the writer/importer asymmetry, and all of them can be read from this response without a re-read.");

inline constexpr const TCHAR* RB_check_table_row_3 =
	TEXT("You asked to write %s, but row_after_exporttext comes back empty while problems is [] (and the column may appear in changed_columns). A ")
	TEXT("nested FGameplayTagContainer is silently stored as () (the accepted format is (GameplayTags=((TagName=\"...\")))), and an unregistered ")
	TEXT("GameplayTag becomes None. This is informational because an empty value is also how enum index 0 exports: check with get_data_table_rows ")
	TEXT("(display names) or by writing a non-zero enumerator.");

inline constexpr const TCHAR* RB_check_pin_info_0 =
	TEXT("This node is disabled (its comment says so) and never runs: new Blueprints come with disabled placeholder events, so a node sweep finds ")
	TEXT("many events that never fire. Treat an event as active only if its comment lacks 'is disabled' and its `then` pin has links.");

inline constexpr const TCHAR* RB_check_pin_info_1 =
	TEXT("Unconnected input pin(s) with no default: %s. The node will use default values at runtime ((0,0,0), false, null) and the compiler does not ")
	TEXT("complain -- e.g. actors spawned at the world origin, or a dynamic material never created. Wire them explicitly; for `self`, take the actor ")
	TEXT("from one of its components (GetOwner). This applies to any static function with a world context (SpawnActor, GetAllActorsOfClass, ")
	TEXT("GetGameMode...).");

inline constexpr const TCHAR* RB_check_pin_info_2 =
	TEXT("This node's execution pin is named %r, not 'execute': the name depends on the node class (e.g. a macro instance uses `exec`), so assuming ")
	TEXT("'execute' breaks one connection per batch with 'Target pin not found'. Use this name in connect_pins.");

inline constexpr const TCHAR* RB_check_function_pins_0 =
	TEXT("You asked for target_class=%r, but this signature belongs to %r: a function with the same name in KismetSystemLibrary, GameplayStatics or ")
	TEXT("KismetMathLibrary (always searched) won. The pins are valid but belong to a different function (e.g. GameplayStatics::GetPlayerState by ")
	TEXT("index instead of the pawn's PlayerState). For a property of a parent class, use variable_get with scope='member'.");

inline constexpr const TCHAR* RB_check_table_row_json_0 =
	TEXT(" Measured causes in the JSON importer: (1) a UserDefinedEnum whose display name has spaces is cut at the first space and lands on index 0, ")
	TEXT("while the returned row_data still shows the requested value -- write the internal name (NewEnumeratorN) instead and read it back with ")
	TEXT("get_enum_info; (2) a GameplayTagContainer passed as a JSON array is rejected and left empty -- pass the ExportText string ")
	TEXT("(GameplayTags=((TagName=\"...\"))); (3) 'missing an entry for X' means the sub-field's internal name differs from your key, so the field kept ")
	TEXT("its default. Copying a row and re-importing it loses multi-word enum values other than index 0.");

inline constexpr const TCHAR* RB_check_terrain_probe_0 =
	TEXT("This read is not read-only: each get_landscape_height_at dirties the LandscapeStreamingProxy that contains the sampled point, and saving ")
	TEXT("many such proxies is expensive. reload_asset_package cannot discard OFPA packages, so the only clean-up is closing the editor without ")
	TEXT("saving. Fine for occasional probes; for broad sampling use export_landscape_heightmap, and check list_dirty_packages before saving. This ")
	TEXT("note is shown once per session.");

inline constexpr const TCHAR* RB_check_terrain_probe_1 =
	TEXT("%d distinct probes so far: sampling a grid with probes misses features (a summit is where a probe is least likely to land) and can suggest ")
	TEXT("flips and offsets that do not exist. For whole-terrain questions use export_landscape_heightmap; for a height ceiling, use the bounds of ")
	TEXT("the landscape and its proxies.");

inline constexpr const TCHAR* RB_check_redirectors_0 =
	TEXT("FOUND: %d objects across %d file(s)");

inline constexpr const TCHAR* RB_check_redirectors_1 =
	TEXT(" Also, %d failed to delete (redirectors_delete_failed[]); they are listed there rather than counted as deleted.");

inline constexpr const TCHAR* RB_check_redirectors_2 =
	TEXT("The two counter pairs measure different things and differ here -- %s. redirectors_found/redirectors_deleted count objects; ")
	TEXT("redirector_packages_found/redirector_packages_deleted count files (one renamed Blueprint can leave several redirectors in one package). The ")
	TEXT("mismatch is expected, not a failure.%s");

inline constexpr const TCHAR* RB_check_bridge_arguments_0 =
	TEXT("Struct argument(s) %s: a JSON key that matches no field of the struct is ignored, so a struct can arrive at its defaults while the ")
	TEXT("function's fallback output still looks plausible. Use the fields' authored names; call the inner piece alone (call_function with ")
	TEXT("class_name) to separate the instrument from the system; to really verify a function with struct parameters, let the game call it.");

inline constexpr const TCHAR* RB_check_package_reload_0 =
	TEXT("%d packages in one call: reloading is synchronous on the game thread and can take many seconds per package, so large batches stall the ")
	TEXT("editor and every MCP call times out. If that happens, do not retry or kill anything; progress shows in Saved/Logs ('Reloading N ")
	TEXT("Package(s)'). Reload in batches of 10-15.");

inline constexpr const TCHAR* RB_check_package_reload_1 =
	TEXT("on_dirty=discard does not revert the file on disk: it loads a new in-memory package, and the editor may write over the file later. To ")
	TEXT("revert an asset to HEAD, run `git checkout -- <asset>` first and then this; verify by re-reading the property and with git status.");

inline constexpr const TCHAR* RB_check_package_reload_2 =
	TEXT("Refused %d OFPA package(s) (skipped_ofpa): this tool does not reload World Partition external actors by design, so they cannot be cleaned ")
	TEXT("from here; close the editor without saving to discard them.");

inline constexpr const TCHAR* RB_check_row_in_memory_0 =
	TEXT("set_data_table_row: `saved: false` inside the payload despite success:true -- the row exists only in memory and is lost when the editor ")
	TEXT("closes. Usual cause: the DataTable file is read-only (e.g. a lockable file under git-lfs without the lock). Take the lock (git lfs lock ")
	TEXT("<file>) and save again; verify with git status.");

inline constexpr const TCHAR* RB_check_enum_internal_names_0 =
	TEXT("In this enum the internal names do not follow the values: %s. NewEnumeratorN follows declaration order, not the ordinal, so reading ")
	TEXT("'NewEnumerator1' as value 1 can return a different entry. Map by display_name, and use the `value` field when you need the ordinal.");

inline constexpr const TCHAR* RB_check_texture_size_0 =
	TEXT(" The actual in-game cap is max_texture_size: %s.");

inline constexpr const TCHAR* RB_check_texture_size_1 =
	TEXT("width/height = %dx%d is the mip currently resident, not the asset size: the imported source is %dx%d (source_width/source_height).%s The ")
	TEXT("resident mip depends on streaming and recent use, so do not judge quality from it; blueprint_get_size_x/y in Python also returns the ")
	TEXT("resident mip.");

inline constexpr const TCHAR* RB_check_imported_font_0 =
	TEXT("imported_object_path confirms the font face, but the companion UFont is created in another package (<path>/<Name>_Font) that the factory ")
	TEXT("leaves dirty and unsaved, so the font disappears next session. font_asset_saved tells (here: %s). Check list_dirty_packages and save the ")
	TEXT("<Name>_Font package.");

inline constexpr const TCHAR* RB_check_component_diff_0 =
	TEXT("This diff compares presence, class and origin (plus attachment and transform), not property values: a component can be 'unchanged' with ")
	TEXT("different values. To compare values, use capture_object_snapshot + diff_object_snapshots.");

inline constexpr const TCHAR* RB_check_default_map_0 =
	TEXT("This call writes more than your key: TryUpdateDefaultConfigFile serializes the whole UGameMapsSettings CDO into Config/DefaultEngine.ini, ")
	TEXT("adding other keys and possibly dropping comments, while its own report only verifies your key. Check `git diff Config/DefaultEngine.ini`; ")
	TEXT("to change a single key, keep the CDO change but restore the file and edit that line by hand.");

inline constexpr const TCHAR* RB_check_graph_lint_0 =
	TEXT("findings: [] does not mean the graph is clean: this lint does not detect dangling execution chains (e.g. an Is Valid with both outputs ")
	TEXT("unconnected), and unreachable_exec_node only reports nodes that are never reached. Inspect the pins with get_graph_nodes(compact=false).");

inline constexpr const TCHAR* RB_check_graph_lint_1 =
	TEXT("%d custom_event_shadowing_override finding(s): typically a false positive -- a custom event used with Set Timer by Event / Bind Event ")
	TEXT("creates a UFunction with its own name, which the lint then reports. Check list_overridable_functions on the class: if the name is not ")
	TEXT("there, nothing is shadowed. Do not rename the event to silence it; that breaks callers that use it by name.");

inline constexpr const TCHAR* RB_check_graph_lint_2 =
	TEXT("%d unreachable_exec_node finding(s): useful for spotting half-wired logic, but verify each one -- the lint can attribute a parent class's ")
	TEXT("node to the child. Resolve the node_id with get_graph_nodes (node_id) first.");

inline constexpr const TCHAR* RB_check_graph_lint_3 =
	TEXT(" Most findings of this lint are noise: treat it as a search aid, not an audit.");

inline constexpr const TCHAR* RB_check_material_walker_0 =
	TEXT("%s shows all its inputs unnamed, but they can still be addressed: the walker emits FExpressionInput::InputName, which is empty on nodes ")
	TEXT("like MakeMaterialAttributes, while connections resolve through UMaterialExpression::GetInputName (the property name), so ")
	TEXT("connect_material_expressions(..., to_input_name='Displacement') works. Input names are per node (MakeMaterialAttributes uses BaseColor; ")
	TEXT("Substrate Slab BSDF uses names with spaces such as 'Diffuse Albedo'): get the exact list from ")
	TEXT("MaterialEditingLibrary.get_material_expression_input_names(expr) via execute_python, and verify connections by re-reading this graph by ")
	TEXT("index.");

inline constexpr const TCHAR* RB_check_material_walker_1 =
	TEXT("Arithmetic nodes have unconnected inputs (%s) whose constants (const_a / const_b) this walker does not emit, so the formula cannot be read ")
	TEXT("from here, and those constants can change its meaning entirely. Read them with get_editor_property('const_a') / ('const_b').");

inline constexpr const TCHAR* RB_check_physical_material_without_complex_0 =
	TEXT("physical_material was requested with a simple trace: on a landscape the physical material is resolved per texel and only exists in per-poly ")
	TEXT("(complex) collision, so an empty value here means 'not visible to this trace', not 'no physical material'. Repeat with trace_complex:true.");

inline constexpr const TCHAR* RB_check_widget_preview_0 =
	TEXT("This render is reliable for structure, not color: sRGB is applied twice, so dark colors come out much lighter, and pixel values will not ")
	TEXT("match the hex you set (colors are stored linear). Judge colors from the asset (get_widget_blueprint_tree, audit_widget_accessibility). It ")
	TEXT("is also a design-time render: anything that only exists at runtime (NativeConstruct text, bindings, view-model data) appears empty; for a ")
	TEXT("live HUD use list_pie_widgets or capture_pie_screenshot. Without an RHI it returns rendered:false with a reason.");

inline constexpr const TCHAR* RB_check_game_mode_class_0 =
	TEXT("This sets the class property on the CDO without recompiling, marking the package dirty or calling Modify, yet success is true, the re-read ")
	TEXT("shows the new value and save_asset persists it -- while PIE may keep using the old class. Follow with compile_blueprint(<the GameMode>) and ")
	TEXT("save_asset, and verify in PIE by class path (e.g. get_player_controller(world, 0).get_class().get_path_name()).");

inline constexpr const TCHAR* RB_check_graph_read_0 =
	TEXT("Reading this graph dirtied its package (a macro or a library Blueprint): the disk is unchanged, but a bulk save would now write into the ")
	TEXT("library. Before finishing, run list_dirty_packages and make sure readonly_dirty_count is 0; discard stray library packages with ")
	TEXT("reload_asset_package(on_dirty='discard').");

inline constexpr const TCHAR* RB_check_graph_read_1 =
	TEXT("Compact output hides knots, comment boxes and unlinked pins without defaults, and can omit the parameter pins of a function entry node, ")
	TEXT("making a parameter look like 'a variable nobody writes'%s. A missing pin here is not proof it does not exist: repeat with compact=false and ")
	TEXT("exclude_classes=[] (and expand_composites=true if there are collapsed graphs).");

inline constexpr const TCHAR* RB_check_graph_read_2 =
	TEXT("%d disabled node(s) created by the editor (%s): new Blueprints start with disabled Event BeginPlay / Event Tick placeholders. ")
	TEXT("add_graph_node does not reuse them -- it adds a duplicate -- so delete them with remove_graph_node before wiring.");

inline constexpr const TCHAR* RB_check_graph_read_3 =
	TEXT("An unwired return node returns its default values: %s. A low nodes_count does not mean an empty function -- a bool output that defaults to ")
	TEXT("true makes the function answer 'yes' without any logic, and nothing warns.");

inline constexpr const TCHAR* RB_check_graph_read_4 =
	TEXT("This graph has a Delay / Set Timer: if you call into it and read the effect in the same call you will see nothing, because the effect lands ")
	TEXT("one or more frames later. Read it in the next call (unless the state is short-lived).");

inline constexpr const TCHAR* RB_check_graph_read_5 =
	TEXT("MakeStruct node(s) with unconnected inputs at zero: %s. Adding a member to a struct adds the pin to every Make/Break node, but the Make's ")
	TEXT("new pin stays unconnected, so the field is silently built as zero. After adding a member, check every K2Node_MakeStruct that builds the ")
	TEXT("struct.");

inline constexpr const TCHAR* RB_check_graph_read_6 =
	TEXT("In an AnimGraph an exposed, unconnected pin overrides the node's own property: set_anim_graph_node_property writes the FAnimNode struct, ")
	TEXT("not the pin, and still reports applied:true while the node keeps using the pin value. Compare get_anim_graph_node_properties with the pin ")
	TEXT("in get_graph_nodes(compact=false), and if they differ also set the pin with set_pin_default_value (non-pin properties such as ")
	TEXT("TranslationMode still need set_anim_graph_node_property).");

inline constexpr const TCHAR* RB_check_write_to_pie_actor_0 =
	TEXT("You are writing a property on a live PIE actor: PostEditChange re-runs its construction script, where spawning actors is not allowed, so ")
	TEXT("components that create helper actors there (e.g. a manager actor) end up with None while the value itself re-reads fine. Change ")
	TEXT("state through the actor's own functions (call_function) instead; if it already happened, restart PIE ")
	TEXT("(nothing on disk is affected).");

inline constexpr const TCHAR* RB_check_blueprint_functions_0 =
	TEXT("This list is incomplete: %d collapsed graph(s) are not shown (include_subgraphs defaults to false), so a composite inside another function ")
	TEXT("does not appear here%s. Repeat with include_subgraphs=true before concluding that a function is missing. Interface graphs are included; ")
	TEXT("get_blueprint_info, by contrast, misses them.");

inline constexpr const TCHAR* RB_check_blueprint_functions_1 =
	TEXT(" (function_count: %d, own_function_graph_count: %d)");

inline constexpr const TCHAR* RB_check_instance_editable_variables_0 =
	TEXT("This list can omit variables that exist in the generated class (for reasons not yet diagnosed). If a specific variable is missing, check ")
	TEXT("the CDO: get_default_object + get_editor_property('<Author Name>') using the exact authored name.");

inline constexpr const TCHAR* RB_check_instance_editable_variables_1 =
	TEXT("instance_editable is unreliable: %d variable(s) report true (%s), but the field ignores CPF_DisableEditOnInstance, which new variables and ")
	TEXT("component-Blueprint variables get by default. The real test is writing the value on a placed instance and re-reading it. For your own ")
	TEXT("Blueprints, use BlueprintEditorLibrary.set_blueprint_variable_instance_editable + compile; for library classes, subclass and expose your ")
	TEXT("own variable instead of editing the library.");

inline constexpr const TCHAR* RB_check_instance_editable_variables_2 =
	TEXT("expose_on_spawn can report false for variables that are exposed on the spawn node. Check the pins of an actual SpawnActorFromClass node ")
	TEXT("(add_graph_node returns them) before resorting to a deferred spawn.");

inline constexpr const TCHAR* RB_check_batch_without_compile_0 =
	TEXT("Functions added with add_blueprint_function do not exist in the generated class until the Blueprint is compiled: %d node(s) (%s) failed ")
	TEXT("with 'Function not found'. %s Compile after creating functions or parameters and before the build_blueprint_graph that calls them, and pass ")
	TEXT("the full generated-class path as target_class (/Game/.../BP_X.BP_X_C).");

inline constexpr const TCHAR* RB_check_batch_without_compile_1 =
	TEXT("The rollback was requested but not performed, so the graph may be half-built: inspect it before editing again.");

inline constexpr const TCHAR* RB_check_batch_without_compile_2 =
	TEXT("With stop_on_error:true (the default) the whole batch was rolled back: nodes that reported ok:true are gone too.");

inline constexpr const TCHAR* RB_check_batch_without_compile_3 =
	TEXT("The batch was not fully rolled back: check node by node which ones remain.");

inline constexpr const TCHAR* RB_check_renamed_enum_entry_0 =
	TEXT("The rename succeeded, but every Select node indexed by this enum still points to the old enum object and now returns its default value -- ")
	TEXT("while compile, find_erroring_blueprints and get_graph_nodes all look clean. Reconnect the Index pin of each such Select with connect_pins ")
	TEXT("(this rebuilds the node against the new enum), and verify by calling the function. Renaming the entry does not update data mapped to it ")
	TEXT("either; check the Break/Make nodes that used it.");

inline constexpr const TCHAR* RB_check_asset_delete_0 =
	TEXT("The asset was not deleted; the same engine message covers two causes. (1) The file is read-only (lockable without a git lfs lock) and still ")
	TEXT("mapped by the editor: load another map, clear the read-only attribute (on Windows, Set-ItemProperty <f> -Name IsReadOnly -Value $false; ")
	TEXT("chmod from Git Bash is not enough), then run delete_asset again and confirm with git status. (2) The asset is open in an editor: deleting ")
	TEXT("can corrupt the in-memory package, and a later create_asset with the same name returns None. Call close_all_editors_for_asset before ")
	TEXT("deleting.");

inline constexpr const TCHAR* RB_check_asset_delete_1 =
	TEXT("The delete returned true but the object is still in memory (asset_verified_deleted is false): the file may be gone while the package stays ")
	TEXT("rooted, does_asset_exist stays true even after garbage collection, and creating or renaming onto that name fails. Recreate under another ")
	TEXT("name or restart the editor; verify with does_asset_exist and the file on disk.");

inline constexpr const TCHAR* RB_check_dirty_packages_0 =
	TEXT("pie_active:false is not a reliable PIE indicator (it has read false with PIE running), and saving with PIE active reports success without ")
	TEXT("writing. Use is_play_in_editor_active, and confirm saves with save_asset's still_dirty_after_save:false + file_on_disk:true, plus git ")
	TEXT("status.");

inline constexpr const TCHAR* RB_check_dirty_packages_1 =
	TEXT("%d library package(s) are dirty and this response does not say why. The engine may be repairing them (e.g. adding missing material usage ")
	TEXT("flags: 'needed to set usage flag' in the log), which a cooked build cannot do, so discarding them only hides the issue. Check Saved/Logs ")
	TEXT("for those lines, and use git log --diff-filter=M to separate your changes from the vendor's.");

inline constexpr const TCHAR* RB_check_level_save_0 =
	TEXT("success:true, but the file was not written (file_mtime_changed is false; file_exists_on_disk proves nothing). If get_dirty_map_packages() ")
	TEXT("is empty, nothing marked the map dirty (e.g. actors moved from Python): call actor.modify(True) on each touched actor before saving. If the ")
	TEXT("map is dirty, the .umap is probably read-only (lockable without a git lfs lock): take the lock and retry. Confirm with git status.");

inline constexpr const TCHAR* RB_check_missing_asset_during_pie_0 =
	TEXT("This \"does not exist\" can be false while PIE is running: loading or saving editor assets is blocked then and reports existing assets as ")
	TEXT("missing, while resolve_asset_by_name still finds them. Check existence with resolve_asset_by_name, and stop PIE before saving or compiling.");

inline constexpr const TCHAR* RB_check_new_material_0 =
	TEXT("The material was created, with one catch. It is born with bUsedWithSkeletalMesh = false, so assigning it to a SkeletalMeshComponent is ")
	TEXT("silently refused and the old material stays (it still compiles): check get_material_usage_flag(mat, SkeletalMesh) and enable it with ")
	TEXT("set_material_usage_flag if needed.");

inline constexpr const TCHAR* RB_check_widget_texts_0 =
	TEXT("Some texts are not gathered for localization%s, which is a real loss: in a saved asset not_gathered_count should be 0. A known cause: FText ")
	TEXT("written through set_data_table_row_from_json is stored as INVTEXT (invariant, not localizable). Fix it with a String Table (LOCTABLE) or by ")
	TEXT("editing the field in the editor; it is fixed when row_after_exporttext no longer shows INVTEXT.");

inline constexpr const TCHAR* RB_check_widget_texts_1 =
	TEXT("%d text(s) have an automatic key: they are still gathered and translated (Unreal assigns a stable key on save), but they lack meaningful ")
	TEXT("identity -- empty namespace and a GUID key -- so translators get no context, and rewriting the text renews the key and orphans its ")
	TEXT("translation. A maintenance cost, best fixed with a String Table.");

inline constexpr const TCHAR* RB_check_component_property_read_0 =
	TEXT("resolved_from_class does not say who authored the value%s, only where the template object lives: an inherited component template reflects ")
	TEXT("parent values it does not override, so it can name the child for a fully inherited value. To check authorship, grep the child's .uasset for ")
	TEXT("the value (an FName is only serialized where it is set). The template also freezes when its package loads and does not refresh on ")
	TEXT("recompile; restart the editor before trusting inherited defaults.");

inline constexpr const TCHAR* RB_check_component_property_read_1 =
	TEXT("Changing a component class's default does not reach Blueprints that already contain that component: their SCS template is an independent ")
	TEXT("copy made when the component was added. get_properties on the CDO shows the new value while existing Blueprints (and their actors) keep ")
	TEXT("the old one.");

inline constexpr const TCHAR* RB_check_component_property_read_2 =
	TEXT("A None here does not prove nothing is set: some properties were renamed and are only migrated on load (e.g. the serialized SkeletalMesh ")
	TEXT("became SkeletalMeshAsset), so asking a template for the new name can return None. Check get_asset_dependencies (a hard dependency means it ")
	TEXT("is set) or grep the .uasset for the path.");

inline constexpr const TCHAR* RB_check_rvt_exponents_0 =
	TEXT("Texel counts were passed where exponents are expected, and the values were silently clamped to the maximum: %s. The size is 1 << tile_count ")
	TEXT("tiles of 1 << (tile_size + 6) texels, so 1024 tiles of 256 texels is tile_count=10, tile_size=2. The resolved values this tool returns are ")
	TEXT("honest (check its size%s). Verify with URuntimeVirtualTexture.get_size() (get_tile_count / get_tile_size do not exist in 5.8).");

inline constexpr const TCHAR* RB_check_data_layer_color_0 =
	TEXT("The requested color was not applied: you asked for %s and the response echoes %s, which is the parameter's default rather than a read-back. ")
	TEXT("Re-read the asset's debug_color, or use set_data_layer_debug_color.");

inline constexpr const TCHAR* RB_check_gerstner_steepness_0 =
	TEXT("applied:true, but the waves self-intersect: total_steepness = %s, and above 1 Gerstner waves fold over (dark, doubled crests). A correct ")
	TEXT("max_wave_height does not prove the waves are fine. Keep it <= 1: with N waves and a linear ramp, total ~ N*(large+small)/2%s. Steepness ")
	TEXT("does not change wave height, only whether waves fold.");

inline constexpr const TCHAR* RB_check_gerstner_steepness_1 =
	TEXT(", so for your %d waves you need (large+small)/2 <= %.3f");

inline constexpr const TCHAR* RB_check_water_info_density_0 =
	TEXT("Each water info texture texel covers %.1f m (%s uu). Depth color and shore foam come from this texture, so large texels flatten the ")
	TEXT("shallow-to-deep transition and shore foam may never appear; use a smaller water zone (or several) rather than a higher resolution. If ")
	TEXT("render_target_resolution_verified is false, this number is unconfirmed%s. A large zone can also make the renderer bias the ocean tile size, ")
	TEXT("which only shows in the log (a LogWater warning); keep extent/2/tile_size below 256.");

inline constexpr const TCHAR* RB_check_zone_linked_bodies_0 =
	TEXT("linked_body_count (%s) is a runtime registry: it says what is linked now, not what was saved. What persists is OwningWaterZone in each ")
	TEXT("water body's own package, so linking does not modify the zone actor. Verify persistence in the water body's package, not the zone's.");

inline constexpr const TCHAR* RB_check_water_material_0 =
	TEXT("A water body has several material slots and this changed only one. WaterMaterial covers the tessellated area around the camera; with ")
	TEXT("local-only tessellation, farther water is drawn by WaterStaticMeshMaterial, so changes may not show where you are looking. Check which slot ")
	TEXT("you are seeing before recalibrating. set_water_body_material is the only setter here; the other slots are reachable from Python on ")
	TEXT("UWaterBodyComponent (readers: get_water_static_mesh_material_instance(), get_water_lod_material_instance()).");

inline constexpr const TCHAR* RB_check_polygroup_paint_0 =
	TEXT("Polygroup %s selected no triangle, so nothing was painted: with PolygroupMode = PerFace (the default of add_mesh_primitive) polygroups are ")
	TEXT("numbered from 1, not 0. Check first with select_mesh_triangles(region='polygroup'), which returns triangles_selected.");

inline constexpr const TCHAR* RB_check_polygroup_paint_1 =
	TEXT("applied:true, but the mesh still has a single MaterialID: the write was discarded because the mesh does not have the MaterialID attribute ")
	TEXT("enabled. Verify with get_mesh_material_ids (an unpainted mesh reports distinct_material_ids = 1). These tools enable the attribute first; ")
	TEXT("direct Python or Blueprint calls do not.");

inline constexpr const TCHAR* RB_check_polygroup_paint_2 =
	TEXT("response:triangles_selected+distinct_material_ids_after");

inline constexpr const TCHAR* RB_check_bake_without_materials_0 =
	TEXT("The MaterialIDs were baked but the asset kept its previous material slots (material_count: %s), because bReplaceMaterials defaults to ")
	TEXT("false. Pass materials (one path per ID, in order) and check material_count and replaced_materials; on the asset, check ")
	TEXT("get_static_mesh_sections. Close gaps in the IDs with remap_material_ids(compact:true). The asset is dirty but not saved.");

inline constexpr const TCHAR* RB_check_dynamic_mesh_material_0 =
	TEXT("The description's 'the dynamic mesh only uses slot 0' refers to this component's material slot, not an engine limit: multi-material dynamic ")
	TEXT("meshes use per-triangle MaterialIDs. Write them with set_mesh_material_id (scope 'all', 'triangles' or 'region', e.g. ")
	TEXT("region='polygroup') or remap_material_ids; copy_dynamic_mesh_to_static_mesh then emits one section per MaterialID.");

inline constexpr const TCHAR* RB_check_material_stats_0 =
	TEXT("num_pixel_texture_samples (%s) is summed over every output the material compiles (e.g. adding a RuntimeVirtualTextureOutput can triple it), ")
	TEXT("so it is not the base-pass cost per pixel. For per-pixel cost use num_pixel_shader_instructions%s, and num_samplers (a hardware limit).");

inline constexpr const TCHAR* RB_check_landscape_nanite_0 =
	TEXT("This response contradicts itself (nanite_built:false with nanite_up_to_date:true): these flags cannot distinguish 'not built' from 'built ")
	TEXT("and current'. Count 'Built static mesh ...LandscapeNaniteMesh' lines in Saved/Logs instead; only nanite_enabled is reliable here.");

inline constexpr const TCHAR* RB_check_hlod_grid_0 =
	TEXT("This asset's cell_size and loading_range belong to the old spatial hash and do not describe the world: on WorldPartitionRuntimeHashSet maps ")
	TEXT("the HLOD grid comes from the partition layer in the HLOD setup. %sThe effective values are CellSize / LoadingRange of the ")
	TEXT("RuntimePartitionLHGrid_* associated with this layer; only layer_type is reliable here.");

inline constexpr const TCHAR* RB_check_hlod_grid_1 =
	TEXT("You just wrote %s: changed_* says the asset changed, not that the world uses it. ");

inline constexpr const TCHAR* RB_check_object_comparison_0 =
	TEXT("%d of the %d reported differences have identical values on both sides (%s): diff_objects compares exported text without defaults while ")
	TEXT("the editor compares display strings, so FText fields and struct containers can be over-reported. Confirm with ")
	TEXT("get_blueprint_component_property on both sides before treating it as an override; noise_count in this response estimates the known noise.");

inline constexpr const TCHAR* RB_check_reparent_rollback_0 =
	TEXT("The rollback was partial: the parent class was restored but SCS components stay renamed%s, and the renamed components then look like ")
	TEXT("pre-existing state. Restore the .uasset with git checkout after every failed attempt and before measuring again; git status, not ")
	TEXT("rolled_back:true, tells whether it is clean.");

inline constexpr const TCHAR* RB_check_row_without_evidence_0 =
	TEXT("This response carries no evidence of what was written, and GameplayTagContainer and SoftObjectPath fields can stay empty silently. Re-read ")
	TEXT("the row, or use set_data_table_row_fields, which reports changed_columns, unchanged_despite_request and row_after_exporttext. If you use a ")
	TEXT("JSON round trip through execute_python, remember that fill replaces the whole table.");

inline constexpr const TCHAR* RB_check_new_function_0 =
	TEXT("success:true with graph_name does not prove the graph exists (the tool's own description says so). Confirm with get_blueprint_functions ")
	TEXT("before wiring%s. If it is missing, create it with unreal.BlueprintEditorLibrary.add_function_graph(bp, name) via execute_python.");

inline constexpr const TCHAR* RB_check_select_polarity_0 =
	TEXT("The option pins are named %s, not False/True: the tool does not rename them when a bool drives the Index, so the option-to-value mapping ")
	TEXT("cannot be read from the graph, and getting it wrong compiles fine and returns the other branch. Verify by calling the function with a ")
	TEXT("discriminating input (call_function); usually Option 1 = true and Option 0 = false.");

inline constexpr const TCHAR* RB_check_name_search_0 =
	TEXT("No results with match=%r does not mean the actor is absent: auto-named actors (spawned, or placed without renaming) have labels other than ")
	TEXT("the ones you expect. Search by class with get_actors_by_class, or use match='name' to search internal names.");

inline constexpr const TCHAR* RB_check_execution_trace_0 =
	TEXT("path: [] does not mean the code did not run: the trace has come back empty while the event was running (e.g. an event fired by a timer).");

inline constexpr const TCHAR* RB_check_execution_trace_1 =
	TEXT("debug_object_set:false is not reliable either: it was reported after the trace had been armed.");

inline constexpr const TCHAR* RB_check_execution_trace_2 =
	TEXT("A reliable alternative is a probe in the graph: a counter variable (and the suspect call's return value) read live.");

inline constexpr const TCHAR* RB_check_ground_placement_0 =
	TEXT("This rests the actor's aggregate bounds on the ground, not its capsule: large spheres (perception, proximity) inflate the bounds and leave ")
	TEXT("the actor floating. Restrict the bounds with get_actor_bounds(components=...), or for a pawn use set_actor_location with z = ground + ")
	TEXT("capsule half height.");

inline constexpr const TCHAR* RB_check_time_dilation_0 =
	TEXT("20.0 is exactly the default clamp (AWorldSettings::MaxGlobalTimeDilation), so this value does not prove 20 was requested: higher requests ")
	TEXT("are clamped silently. Raise MaxGlobalTimeDilation in the WorldSettings to go higher (set the dilation with ")
	TEXT("GameplayStatics.set_global_time_dilation via execute_python). Fast timers fall behind at high dilation, so measure durations at dilation 1.");

inline constexpr const TCHAR* RB_check_landscape_material_0 =
	TEXT("material_assigned:true only reports the Landscape actor. This tool also propagates the material to every LandscapeStreamingProxy, dirtying ")
	TEXT("all their packages, which then need saving and committing. Verify with get_landscape_material and each proxy's landscape_material.");

inline constexpr const TCHAR* RB_check_rvt_info_0 =
	TEXT("These are resolved values (GetTileCount / GetTileSize / GetSize), not the raw values written, so they will not match what you set even when ")
	TEXT("the asset is correct. Use them to verify the cache density, and cross-check with get_size() from Python.");

inline constexpr const TCHAR* RB_check_montage_notifies_0 =
	TEXT("This list shows notify class names (e.g. 'PlayMontageNotify'), not their NotifyName, so it cannot be matched against authored names. Read ")
	TEXT("notify.get_editor_property('notify_name') instead.");

inline constexpr const TCHAR* RB_check_state_machine_0 =
	TEXT("is_valid:true with transition_count:0 is the signature of a broken AnimBlueprint (one reported this way failed to compile with dozens of ")
	TEXT("errors). The only reliable check is compile_anim_blueprint (status, error_count), not this tool or the authoring tools' return values. A ")
	TEXT("state machine without transitions is not valid.");

inline constexpr const TCHAR* RB_check_truncated_bones_0 =
	TEXT("The bone list is truncated at 100, and the tool says so: ");

inline constexpr const TCHAR* RB_check_truncated_bones_1 =
	TEXT("The bone list is truncated at 20 without warning (num_bones is larger than the number of names returned): ");

inline constexpr const TCHAR* RB_check_truncated_bones_2 =
	TEXT("neither tool accepts an offset, so the rest cannot be fetched this way. Comparing two skeletons through these lists is misleading, since ")
	TEXT("the first bones (from the root) match even between incompatible skeletons. Compare bone counts first (bone_count / num_bones versus ")
	TEXT("len(bone_names)); for full lists, parse the asset or check bones individually with get_bone_transform.");

inline constexpr const TCHAR* RB_warn_collision_ini_0 =
	TEXT("This tool re-serializes the whole Config/DefaultEngine.ini instead of adding a line, and can drop comments. Check now with git diff ")
	TEXT("--numstat -- Config/DefaultEngine.ini: any deletions are the alarm. Keep important rationale in your own documentation rather than in .ini ")
	TEXT("comments.");

inline constexpr const TCHAR* RB_check_gameplay_tag_0 =
	TEXT("Creating a tag rewrites the whole Config/DefaultGameplayTags.ini: entries are reordered alphabetically, comments are removed and other ")
	TEXT("entries' DevComments are normalized. Check git diff --numstat -- Config/DefaultGameplayTags.ini: deletions mean collateral changes; if so, ")
	TEXT("restore the file and reinsert only your lines (the tag stays registered in the running editor). The editor can rewrite this file again ")
	TEXT("later, so check again before committing.");

inline constexpr const TCHAR* RB_check_collision_profile_read_0 =
	TEXT("This reads only the +Profiles= entry and ignores the +EditProfiles= entries that modify it, so a channel missing here may still be ")
	TEXT("configured. Verify the behavior instead: trace with raycast_scene on that channel and compare with the same trace on a channel known to hit.");

inline constexpr const TCHAR* RB_check_log_lines_0 =
	TEXT("These lines are not tied to your run: the buffer is circular (4096 entries) and survives across PIE sessions, so old errors can look ")
	TEXT("current. count returns the oldest retained lines first, and since_sequence_id alone does not exclude a previous run.");

inline constexpr const TCHAR* RB_check_log_lines_1 =
	TEXT(" The ring is full (held_total %s): a single error repeated every frame can push everything else out, so check the category breakdown before ")
	TEXT("concluding anything; filter_text has also been seen not to filter, so an absence of lines proves nothing.");

inline constexpr const TCHAR* RB_check_log_lines_2 =
	TEXT(" To scope a run: clear_message_log('PIE') before start_play_in_editor and read get_message_log_entries, or print_to_output_log('MARK-...') ")
	TEXT("and pass that line's sequence_id as since_sequence_id.");

inline constexpr const TCHAR* RB_warn_array_rotation_0 =
	TEXT("The array's third component is roll, not yaw (the order is [pitch, yaw, roll]), so this rotation has roll=%s and yaw=0. Pass the rotation ")
	TEXT("as an object instead ({'pitch': ..., 'yaw': ..., 'roll': ...}). This tool does not re-read the rotation: check get_actor_rotation() after ")
	TEXT("spawning.");

inline constexpr const TCHAR* RB_check_child_widget_with_root_0 =
	TEXT("A root canvas was added to a child Widget Blueprint: at runtime the first non-null RootWidget in the class chain wins, so this empty root ")
	TEXT("hides the parent's tree (blank widget, clean compile), and it cannot be removed afterwards (RootWidget is protected and not writable ")
	TEXT("through reflection). Create inheriting children with unreal.WidgetBlueprintFactory via execute_python, which leaves them without a root ")
	TEXT("(get_widget_blueprint_tree should show widget_count: 0).");

inline constexpr const TCHAR* RB_check_parameter_rename_0 =
	TEXT("The CoreRedirect remaps callers but not the implementers' override graphs: if the interface is renamed first, overrides keep the old pin ")
	TEXT("dangling and fail to compile. Rename in the implementers first and in the interface last, without compiling in between. To clean up an ")
	TEXT("existing orphan pin, add a dummy parameter with add_function_parameter (which rebuilds the entry/result nodes), move the wire with ")
	TEXT("connect_pins, then remove the dummy with reconcile_function_signature in remove_extra mode.");

inline constexpr const TCHAR* RB_check_composite_table_0 =
	TEXT("A CompositeDataTable loads as a UDataTable, so this row_count may already merge its parent tables and will not match the rows you wrote. ")
	TEXT("Repeat with include_row_sources=True to get parent_tables, merged_row_count versus sum_parent_row_counts, merged_row_count_matches and ")
	TEXT("shadowed_rows.");

inline constexpr const TCHAR* RB_check_visible_text_without_identity_0 =
	TEXT("This writes player-facing text without localization identity, so it will not be gathered or translated, and audit_widget_text will not ")
	TEXT("catch it (it only inspects Widget Blueprints). Write the value through set_properties / set_data_table_cell ")
	TEXT("as NSLOCTEXT(\"NS\",\"Key\",\"Source\") or LOCTABLE(\"/Game/ST\",\"key\"); bare text stays invariant. The same applies to create_enum and ")
	TEXT("rename_enum_entry.");

inline constexpr const TCHAR* RB_check_grass_variety_0 =
	TEXT("grass_density is in instances per 10 m2, not per m2 (see unreal.GrassVariety.grass_density.__doc__): a factor of 10 in instance budgets. ")
	TEXT("start_cull_distance only fades if the material uses PerInstanceFadeAmount; otherwise grass pops at end_cull_distance. FGrassVariety has no ")
	TEXT("collision settings: landscape grass never blocks characters.");

inline constexpr const TCHAR* RB_check_fork_without_tag_0 =
	TEXT("The fork was created without its origin tag (fork_origin_set is false), so nothing records where it came from and audit_class_purity will ")
	TEXT("flag it as missing_fork_tag. Write the fork-origin tag (Project Settings: Fork Origin Tag) with set_asset_metadata_tag while you still know the origin.");

inline constexpr const TCHAR* RB_check_new_struct_0 =
	TEXT("create_struct does not save the struct to disk, so a commit can miss it; save it explicitly and check git status. It is also created with a ")
	TEXT("default boolean MemberVar_0 besides the members you add; remove it with remove_struct_member('MemberVar_0').");

inline constexpr const TCHAR* RB_check_pcg_regenerated_0 =
	TEXT("scheduled/generated does not mean the world reflects your new settings: PCG reuses cached output when settings are edited through ")
	TEXT("reflection, because that does not invalidate the node's cached CRC (set_pcg_node_properties does). Run ")
	TEXT("cleanup_pcg_component(remove_components=True), then generate_pcg_component, and verify the generated components. Also check which graph you ")
	TEXT("are baking: a non-hierarchical graph bakes a large volume in one go, memory keeps growing, and garbage collection during the bake has ")
	TEXT("crashed the editor; prefer graphs with use_hierarchical_generation=True (a generation_grid_size of 2147483648 means hierarchical generation ")
	TEXT("is off).");

inline constexpr const TCHAR* RB_warn_local_rpc_0 =
	TEXT("success:true with nothing returned (no return_value, no out_params) does not mean the event ran: an RPC invoked from the editor does ")
	TEXT("not cross the network (the call runs locally) and may not run at all. Repeat the call on the object where the effect should happen locally (e.g. the ")
	TEXT("server's instance); if that is silent too, the harness is the problem. Drive networked effects with real input injected into the right ")
	TEXT("player.");

inline constexpr const TCHAR* RB_check_function_parameter_0 =
	TEXT("This tool adds parameters but cannot retype them or create arrays (an array parameter comes out scalar); rename_function_parameter does not ")
	TEXT("retype either, and there is no remove_function_parameter. To change a parameter's type, author the signature in a Blueprint interface ")
	TEXT("(add_function_parameter on the interface, compile_blueprint, add_blueprint_function with override=true).");

inline constexpr const TCHAR* RB_check_nav_modifier_volume_0 =
	TEXT("This tool works, but brush_built and area_class_applied are echoes: they are just as true with NavArea_Default. Verify the geometry with ")
	TEXT("get_actor_bounds, the area with get_editor_property('area_class'), and the effect with a path query after rebuild_navigation.");

inline constexpr const TCHAR* RB_check_navmesh_settings_0 =
	TEXT("The raw value (%s) and the effective value (%s) differ because the engine quantizes the tile size to multiples of CellSize. Use the ")
	TEXT("effective value for tile and coverage arithmetic. cell_size and cell_height are deprecated as actor properties in 5.8 (see ")
	TEXT("NavMeshResolutionParams).");

inline constexpr const TCHAR* RB_check_ocean_created_0 =
	TEXT("The ocean works and sets up its materials, extents and waves, but its spline does not limit its footprint (check get_local_bounds() of the ")
	TEXT("WaterInfoMeshComponent). It provides no swimming volume: pawns walk on the seabed. Swimming needs a separate APhysicsVolume with ")
	TEXT("water_volume=True; verify in PIE with CharacterMovement.get_physics_volume() and MOVE_SWIMMING.");

inline constexpr const TCHAR* RB_check_heightmap_import_0 =
	TEXT("This import is the right path for an existing landscape of matching resolution, but applied does not prove the heights are the ones you ")
	TEXT("wanted. Verify with export_landscape_heightmap compared against the source file, not with a few probes.");

inline constexpr const TCHAR* RB_check_rvt_volume_0 =
	TEXT("already_existed is true: no volume was created and the existing one was returned (by design). If you expected new bounds, you do not have ")
	TEXT("them. ");

inline constexpr const TCHAR* RB_check_rvt_volume_1 =
	TEXT("The volume was created with its RuntimeVirtualTextureComponent set and is_spatially_loaded false; calling again returns this same volume. ");

inline constexpr const TCHAR* RB_check_rvt_volume_2 =
	TEXT("Verify on the actor, not on this response: virtual_texture and is_spatially_loaded on the component, and get_actor_bounds for the covered ")
	TEXT("region.");

inline constexpr const TCHAR* RB_check_rvt_volume_3 =
	TEXT(" With bounds_align_actor, Z is fitted exactly to the heightmap's minimum and maximum with no margin, which can leave patchy coverage. ")
	TEXT("Location is the minimum corner and scale is the full size (not center and half extent); the top must leave room for the material's height ")
	TEXT("variation. Call again with bounds_align_actor=None after fixing Z, and check get_actor_bounds.");

inline constexpr const TCHAR* RB_check_unused_assets_0 =
	TEXT("This list measures referencers == 0, not reachability: assets that only reference each other (islands) never appear, and some listed assets ")
	TEXT("are unreferenced by design (e.g. a new String Table). It is not a deletion list. To find dead content, traverse references from your roots ")
	TEXT("(maps, game mode, startup classes).");

inline constexpr const TCHAR* RB_check_text_gather_0 =
	TEXT("config_written is true on every call because this tool regenerates its .ini from the localization target's configuration, discarding manual ")
	TEXT("edits. If the gather steps are missing, the commandlet has nothing to read and exits with -1: the localization pipeline is not configured. ")
	TEXT("Set it up in the Localization Dashboard (Window -> Localization Dashboard); this tool only converts that configuration to an .ini.");

inline constexpr const TCHAR* RB_check_component_may_not_persist_0 =
	TEXT("Returning a component_name does not prove the component is in the SCS: with %s (this case) the addition can succeed without persisting and ")
	TEXT("may appear later, so retrying creates duplicates. Check get_blueprint_components after every addition; ")
	TEXT("SubobjectDataSubsystem.add_new_subobject via execute_python is a reliable alternative. Remove duplicates with ")
	TEXT("remove_component_from_blueprint.");

inline constexpr const TCHAR* RB_check_clamped_smoothing_0 =
	TEXT("You asked for %r iterations and %r were applied: the tool silently clamps to its documented range (1-5). Smoothing only averages existing ")
	TEXT("heights; it cannot add relief. Library heightmap brushes require edit layers and do nothing without them (check ")
	TEXT("get_editor_property('layers') on the Landscape actor); set_landscape_height_in_box and flatten_landscape_in_box write heights directly. ")
	TEXT("Verify relief with a vertical trace.");

inline constexpr const TCHAR* RB_check_unverifiable_replication_0 =
	TEXT("This report cannot fail: get_blueprint_variable_detailed_info reads is_replicated from the same flags this tool writes, while actual ")
	TEXT("replication depends on the generated class's FProperty. Compile and run audit_replication on the folder (the full payload lands in ")
	TEXT("Saved/MCPAudits/), and verify in a networked PIE session.");

inline constexpr const TCHAR* RB_warn_tampered_proxy_0 =
	TEXT("Possessing a pawn and manipulating its physics or input can leave its client proxy inconsistent (wrong position, components not ")
	TEXT("enumerable), which produces false replication failures. To measure networked state, use a fresh session, mutate only on the server, and ")
	TEXT("read both worlds.");

inline constexpr const TCHAR* RB_warn_movement_mirror_0 =
	TEXT("bReplicateMovement does not give a headless oracle for movement replication: the client copy has been observed frozen even with a correct ")
	TEXT("setup, and there is no positive control that this path works in the harness. Measure host-authoritative movement on the server and verify ")
	TEXT("the client mirror in a real multiplayer session. Note that FloatingPawnMovement / UPawnMovementComponent do no client prediction: ")
	TEXT("client-driven pawns need their input sent through a server RPC.");

inline constexpr const TCHAR* RB_warn_water_without_swim_volume_0 =
	TEXT("This water body does not provide swimming and does not warn: pawns walk on the bed (MOVE_WALKING). Swimming needs a separate APhysicsVolume ")
	TEXT("with water_volume=True whose top matches the water surface; keep volumes under about 6 km per side. Verify in PIE with ")
	TEXT("CharacterMovement.get_physics_volume() and MOVE_SWIMMING.");

inline constexpr const TCHAR* RB_warn_volume_collision_0 =
	TEXT("(1) Size: this volume is %d uu on its longest side; above about 6 km, volume collision does not reach the corners, while get_actor_bounds ")
	TEXT("still reports full coverage. Cover large areas with a grid of smaller volumes. ");

inline constexpr const TCHAR* RB_warn_volume_collision_1 =
	TEXT("Three things to check with volumes. (1) Size: above about 6 km per side, collision does not reach the corners; use a grid of volumes. ");

inline constexpr const TCHAR* RB_warn_volume_collision_2 =
	TEXT("(2) For water, the volume's top must match the water surface: the CMC measures immersion against the volume bounds, so a sloped surface ")
	TEXT("makes pawns swim on dry land. (3) With the default Static mobility, moving the volume moves its transform but not its collision; set ")
	TEXT("BrushComponent0.Mobility = Movable if it must move (volumes cannot be created at runtime). Verify with find_scene_overlaps, never with the ")
	TEXT("transform.");

inline constexpr const TCHAR* RB_warn_undeformed_collision_0 =
	TEXT("A SplineMeshComponent's render deforms along the spline but its collision does not: traces can hit an undeformed slab above or below the ")
	TEXT("visible surface, while get_collision_enabled() and the placement look correct. Toggling collision, set_forward_axis or update_mesh do not ")
	TEXT("fix it. Verify with vertical traces along the path or a pawn standing on it; shorter sections reduce the error, or disable collision on the ")
	TEXT("sections and rely on other collision.");

inline constexpr const TCHAR* RB_warn_construction_script_erases_0 =
	TEXT("On a spline actor whose points were set from Python, rerunning the construction script resets the SplineComponent to the Blueprint default, ")
	TEXT("and if the construction script is guarded (e.g. by a Do Once) it rebuilds nothing. Call the actor's own functions in construction-script ")
	TEXT("order after writing the points, and verify get_number_of_spline_points(), get_spline_length() and the SplineMeshComponent count.");

inline constexpr const TCHAR* RB_warn_fragile_spline_points_0 =
	TEXT("Do not write any other property of this actor after setting the spline points: any PostEditChangeProperty reruns the construction script ")
	TEXT("and resets the spline to the Blueprint default (rerun_construction_scripts does the same). Set properties first and points last, then ")
	TEXT("rebuild. Verify with get_location_at_spline_point(i, WORLD) for every point; the point count alone does not reveal the reset.");

inline constexpr const TCHAR* RB_warn_uncounted_grass_0 =
	TEXT("A zero grass instance count from Python does not mean the grass is miswired: landscape grass has been visible on screen while the headless ")
	TEXT("count was 0. Check that the response fit (large sweeps can lose the data field; narrow with world='editor' or world='server'), and check ")
	TEXT("the wiring with get_grass_types_bp() on the landscape components; whether grass is actually visible needs a visual check.");

inline constexpr const TCHAR* RB_warn_channel_unknown_to_engine_0 =
	TEXT("The .ini is written, but the running engine does not know this channel until the editor restarts (profiles are not reloaded), and live ")
	TEXT("components respond to the unknown channel with Block. Restart the editor before testing anything that depends on it; until then, only the ")
	TEXT(".ini on disk can be checked.");

inline constexpr const TCHAR* RB_warn_widget_without_lifecycle_0 =
	TEXT("Nesting another Widget Blueprint inherits its appearance but not its lifecycle: when your wrapper is removed, the nested widget's Destruct ")
	TEXT("cleanup may not run (e.g. a post-process effect it enabled stays on), and nothing headless shows it. Before nesting, check its Destruct ")
	TEXT("(get_blueprint_functions, get_function_pins) and call any public cleanup from your wrapper's Destruct.");

inline constexpr const TCHAR* RB_warn_client_key_input_0 =
	TEXT("Injected input does not drive the client copy of a pawn: key injection with pie_client_index does not reach it, Enhanced Input injection ")
	TEXT("lasts one frame, and raw setters do not feed network input. Held keys do work on the host. Verify client-driven control in a real ")
	TEXT("multiplayer session; headless, test possession and state replication.");

inline constexpr const TCHAR* RB_warn_injected_aim_0 =
	TEXT("Injected aim does not make interaction systems pick a target, even though the camera rotation is set and a manual trace hits; a known-good ")
	TEXT("interactable fails the same way, so a failure here says nothing about the object. Call the interactable's entry point through authority ")
	TEXT("(e.g. call_method('Interact', pc) on the server) or use real input without aiming, and verify look-and-interact in play.");

inline constexpr const TCHAR* RB_warn_unlocated_region_0 =
	TEXT("If the triangle range you are painting was derived from append order, checking the first and last pieces does not validate the ones in ")
	TEXT("between. Locate a piece before painting it: paint half the space with a decoy ID using set_mesh_material_id (scope='region'), then check ")
	TEXT("get_mesh_material_ids -- if the target ID disappears, the piece is in that half.");

inline constexpr const TCHAR* RB_warn_array_not_propagated_0 =
	TEXT("A container variable's default reaches the CDO but not the SCS template or placed instances, even though roundtrip_verified is true and ")
	TEXT("scalars in the same component do propagate. Neither set_blueprint_component_properties nor the construction script fixes placed instances. ")
	TEXT("Verify by reading the placed instance.");

inline constexpr const TCHAR* RB_warn_throttled_frame_0 =
	TEXT(" This read shows it: frame_ms = %s is the editor sleeping, not your game's cost.");

inline constexpr const TCHAR* RB_warn_throttled_frame_1 =
	TEXT("When the editor window is not focused (always the case under MCP), the editor throttles: frame_ms sticks near 333 ms (3 FPS) and ")
	TEXT("game_thread_wait_ms inflates game_thread_ms.%s Slate.bAllowThrottling does not change this. Disable bThrottleCPUWhenNotForeground on ")
	TEXT("Default__EditorPerformanceSettings (find_object + set_editor_property with the native name) while measuring, and restore it afterwards. ")
	TEXT("Throttled numbers are only comparable as ratios.");

inline constexpr const TCHAR* RB_warn_pie_still_loading_0 =
	TEXT("A false here does not prove PIE cannot start, nor does it by itself mean PIE is still loading (status=%s, current_level=%s). Right after ")
	TEXT("start_play_in_editor, a large World Partition map can take well over a minute to reach 'playing' (the log shows 'Waiting on static mesh ... ")
	TEXT("before playing'), so poll before concluding. If you did not start PIE, or just stopped it, false simply means PIE is not running.");

inline constexpr const TCHAR* RB_warn_pie_not_starting_clean_0 =
	TEXT("If you are about to measure, check that the world starts clean: an auto-save (e.g. under Saved/SaveGames/) can be reloaded on start, ")
	TEXT("restoring the previous session's state for saveable components while other variables reset, which produces impossible readings. List ")
	TEXT("Saved/SaveGames/ first; if a save is present and must not apply, delete it (delete_save_game) and confirm with a control read before ")
	TEXT("measuring.");

inline constexpr const TCHAR* RB_warn_sweep_loads_blueprints_0 =
	TEXT("This tool loads Blueprints from disk, which changes what the editor holds in memory: loading a vendor asset that does not compile can make ")
	TEXT("the next PIE start open a modal dialog that blocks the game thread, so MCP calls hang. If get_editor_status times out while the editor process ")
	TEXT("still responds, suspect a modal. Do not edit or delete the vendor asset; the damage is in memory only, and restarting the editor clears it.");

inline constexpr const TCHAR* RB_check_compile_material_0 =
	TEXT("recompiled:true is an acknowledgment, not a result: a material that fails to compile renders as the default material without any error on ")
	TEXT("the actor, so the failure stays silent until someone looks at the screen.");

inline constexpr const TCHAR* RB_check_connect_pins_0 =
	TEXT("The input pin keeps both links and the compiler uses the first, so the graph still behaves as before and compiles cleanly. To rewire, use ")
	TEXT("disconnect_pin (it breaks all of the pin's links) and connect only the right one.");

inline constexpr const TCHAR* RB_check_dirty_package_0 =
	TEXT(" is not dirty, and without Modify() nothing will save it: the value is probably only in memory and will be lost when the editor closes. To ")
	TEXT("persist it: execute_python with bp.modify() + save_loaded_asset(only_if_is_dirty=False). The dirty count can also be 0 when the file was ")
	TEXT("already written, so the final check is git (git hash-object <f> versus git rev-parse HEAD:<f>); to revert, run git checkout -- <f> and then ")
	TEXT("reload_asset_package(on_dirty='discard').");

inline constexpr const TCHAR* RB_check_incomplete_array_0 =
	TEXT("This tool rewrites the whole array, so a partial failure silently drops elements, and the return value does not say so: ");

inline constexpr const TCHAR* RB_check_incomplete_array_1 =
	TEXT(". Only element counts and positions were compared, not values. set_blueprint_component_array_element edits a single element and compares ")
	TEXT("all the others before and after.");

inline constexpr const TCHAR* RB_check_blueprint_component_property_0 =
	TEXT("override_persisted does not re-read the value, so it proves nothing, and on an object reference this can erase the inherited default. To ")
	TEXT("set it, use execute_python with load_object on the component's _GEN_VARIABLE template + set_editor_property, then compile_blueprint and ")
	TEXT("save (check whether the property expects a DataAsset instance or a class). An object reference can re-read correctly on the template and ")
	TEXT("still not reach spawned instances, even when other properties in the same batch do; the definitive check is spawning the actor, reading the ")
	TEXT("instance, and destroying it.");

inline constexpr const TCHAR* RB_check_stop_pie_0 =
	TEXT("pie_stopped:true is a constant and measures nothing: teardown is asynchronous, so PIE is still active right after this call. Re-read ")
	TEXT("is_play_in_editor_active before continuing. Python handles to PIE worlds or packages left in globals keep the PIE world alive ('Object ... ")
	TEXT("from PIE level still referenced'); clear them and run gc.collect() before stopping. Stopping PIE can occasionally crash the editor, so ")
	TEXT("commit your work first.");

inline constexpr const TCHAR* RB_check_graph_in_memory_0 =
	TEXT("The graph change is only in memory: the package is dirty and the .uasset unchanged, while num_errors and a graph re-read both look fine and ")
	TEXT("PIE uses the in-memory version. Save now (save_asset(path=...)) and confirm the .uasset shows as modified in git status; unsaved edits can ")
	TEXT("be lost, or flushed later as someone else's change.");

inline constexpr const TCHAR* RB_check_graph_in_memory_1 =
	TEXT("The package of '%s' is not dirty after editing its graph, so neither save_asset nor 'Save All' will write it and the change is lost on ")
	TEXT("close. Either the edit did not apply, or the owning package has another name (an SCS template lives in its class's package): check ")
	TEXT("list_dirty_packages. If it really is not dirty, use execute_python -> bp.modify() + EditorAssetLibrary.save_loaded_asset(bp, ")
	TEXT("only_if_is_dirty=False), then confirm in git status.");

inline constexpr const TCHAR* RB_check_ambiguous_name_0 =
	TEXT("'%s' matches %s assets and this read picked one without saying which: %s. Same-named copies (e.g. backups) are common, and reading the ")
	TEXT("wrong one leads to false conclusions. Repeat with the full path, and use full paths for every further read of this asset.");

inline constexpr const TCHAR* RB_warn_class_degraded_0 =
	TEXT("You asked for %r for %r but got an object reference (%s), not a class. The response echoes the type you passed, so it does not reveal this: ")
	TEXT("the plugin's pin-type resolver cannot produce class references (PC_Class) from text. It fails later, when wiring ('Object Class Reference ")
	TEXT("is not compatible with ... Class Reference'). For a table row, use /Script/CoreUObject.SoftClassPath; for a pin, connect the consumer first ")
	TEXT("and let the type be inferred, then set the defaults.");

inline constexpr const TCHAR* RB_warn_class_degraded_1 =
	TEXT("The type in the response is your own parameter echoed back, not the stored state.");

inline constexpr const TCHAR* RB_check_component_in_library_0 =
	TEXT("before/after:list_dirty_packages.under_readonly_root");

inline constexpr const TCHAR* RB_check_component_in_library_1 =
	TEXT("This call dirtied a library package: %s. When the child Blueprint has no scene root of its own, a scene component can be written into the ")
	TEXT("parent's SCS, editing a vendor or library asset; saving your Blueprint does not write it, but a 'Save All' would. Discard it with ")
	TEXT("reload_asset_package(<parent package>, on_dirty='discard'). Alternatives that stay in the child: add the component at runtime ")
	TEXT("(AddComponentByClass in BeginPlay), or set the data through set_blueprint_component_properties. Confirm with get_blueprint_components ")
	TEXT("(owner_class and source of the new node).");

inline constexpr const TCHAR* RB_check_dropped_components_0 =
	TEXT(".");

inline constexpr const TCHAR* RB_check_dropped_components_1 =
	TEXT("Disk and git are unaffected (compile_blueprint is clean, find_erroring_blueprints sees nothing), but the editor's in-memory copy, which PIE ")
	TEXT("instances, lost those components. Fix: reload_asset_package(package_name='");

inline constexpr const TCHAR* RB_check_dropped_components_2 =
	TEXT("', on_dirty='discard'), after saving any change you want to keep (discard drops unsaved work in that package). Verify on the live actor ")
	TEXT("with get_actor_components(world='server') during PIE.");

// {0}=tool name; {1}=requested value (repr); {2}=stored value (repr)
inline constexpr const TCHAR* RB_apply_finding_0 =
	TEXT("{0}: the call reported success but the state did not change (requested {1}, stored {2}).");

inline constexpr const TCHAR* RB_annotate_offload_0 =
	TEXT("The full payload is on disk: read it with get_mcp_audit_report(path='");

inline constexpr const TCHAR* RB_annotate_offload_1 =
	TEXT("'), or call again with a narrower query (max_results or a filter) to get it inline. Report file names use UTC, not local time; compare them ")
	TEXT("with UTC timestamps.");

inline constexpr const TCHAR* RB_annotate_offload_2 =
	TEXT("Writing it to disk also failed, so the detail is lost: repeat the call with a narrower query (max_results or a filter).");

inline constexpr const TCHAR* RB_annotate_offload_3 =
	TEXT(" Also, this tool's own readback check could not run because it reads the missing field: the absence of a warning here does not mean there ")
	TEXT("is no trap, so call again with a narrower query before concluding.");

inline constexpr const TCHAR* RB_annotate_offload_4 =
	TEXT("This response is incomplete and its missing field is not an empty result: it did not fit (");

inline constexpr const TCHAR* RB_annotate_offload_5 =
	TEXT(". The remaining scalar fields are valid. Do not treat the missing field as an empty list (`x.get(field) or []` turns 'not delivered' into ")
	TEXT("'nothing there'). ");

inline constexpr const TCHAR* RB_check_unowned_pie_0 =
	TEXT("This PIE session is yours: starting PIE inside a batch leaves pie_owner=unknown, so the bridge refuses to stop it. Pass force=true to stop ")
	TEXT("it now, and next time start PIE with its own call, outside a batch.");

inline constexpr const TCHAR* RB_check_inherited_variable_0 =
	TEXT("This tool only reaches variables declared on the Blueprint itself, so it cannot change an inherited default. Set it on the CDO instead: ")
	TEXT("unreal.get_default_object(<Class>_C).set_editor_property('<Authored Name>', value) (the exact authored name, spaces included), then ")
	TEXT("compile_blueprint and save. Verify on disk (binary grep + git status).");

inline constexpr const TCHAR* RB_check_widget_tree_during_pie_0 =
	TEXT("PIE is active, and loading editor assets returns null while it runs, so this 'not found' may be false. Check the asset with ")
	TEXT("resolve_asset_by_name. To read the static tree, stop PIE and repeat; to inspect live widgets, use WidgetLibrary.get_all_widgets_of_class on ")
	TEXT("the anchored world.");

inline constexpr const TCHAR* RB_check_subsystem_in_editor_world_0 =
	TEXT("PIE is active and the '%s' lifetime was resolved on the editor world, so this 'not found' may mean 'not in this world': a Blueprint ")
	TEXT("WorldSubsystem exists once per world. Repeat the call with world='pie' (or 'server' / 'client') to read the PIE instance, and call it ")
	TEXT("with call_function on the instance_path that read returns.");

inline constexpr const TCHAR* RB_check_input_without_controller_0 =
	TEXT("PIE is active, so 'start PIE first' is misleading: with player_index=%s the tool searches the PlayerControllers of the resolved world, and ")
	TEXT("in multi-client PIE a client's controller lives in another world (UEDPIE_1), while the default 'pie' resolves to the first. Try ")
	TEXT("world='client' or an explicit 'pie:N'. Alternatively, use execute_python to pick the EnhancedInputLocalPlayerSubsystem whose ")
	TEXT("get_outer().get_world() is the target world and call inject_input_vector_for_action(IA, Vector, [], []).");

inline constexpr const TCHAR* RB_check_expression_delete_0 =
	TEXT("The deletion did happen: '%s' is no longer in the material (%d live expressions). This tool can return False or None after doing the work, ")
	TEXT("so do not repeat the call. The oracle is the material's expression list (get_material_graph_info); the error above is kept because it may ")
	TEXT("concern something else.");

inline constexpr const TCHAR* RB_check_function_not_found_0 =
	TEXT("no target_class was given, and the tool only searches KismetSystemLibrary, GameplayStatics and KismetMathLibrary besides the Blueprint's ")
	TEXT("own class, so functions from libraries such as KismetArrayLibrary or KismetStringLibrary look nonexistent. Repeat with target_class (e.g. ")
	TEXT("'KismetArrayLibrary' or /Script/Engine.<Library>)");

inline constexpr const TCHAR* RB_check_function_not_found_1 =
	TEXT("the name may have a trailing space (some vendor functions do, and it survives in the FName), so the documented name may not match. Copy the ")
	TEXT("exact name from get_blueprint_functions");

inline constexpr const TCHAR* RB_check_function_not_found_2 =
	TEXT("%s'not found' does not necessarily mean the function does not exist. Two known causes: (1) %s. (2) If the function belongs to a Blueprint ")
	TEXT("or a vendor library, %s. To tell them apart, call get_class_functions(<class>) for a native class or get_blueprint_functions(<Blueprint>); ")
	TEXT("if the function is listed there, the lookup is the problem.");

inline constexpr const TCHAR* RB_check_unknown_material_output_0 =
	TEXT("No spelling will work: in 5.8 displacement is not a root material pin but a material attribute (MP_Displacement), and ")
	TEXT("unreal.MaterialProperty has no MP_DISPLACEMENT, so connect_material_property cannot reach it. Enable use_material_attributes on the ")
	TEXT("material, connect the Displacement pin of a MakeMaterialAttributes node, and enable enable_tessellation + displacement_scaling on the ")
	TEXT("UMaterial (do not use SetMaterialAttributes for this).");

inline constexpr const TCHAR* RB_check_unknown_material_output_1 =
	TEXT("This tool only knows the material's root pins. For displacement, no spelling works: in 5.8 it is a material attribute, reachable only ")
	TEXT("through MakeMaterialAttributes with use_material_attributes=True, plus enable_tessellation and displacement_scaling on the UMaterial.");

inline constexpr const TCHAR* RB_check_rpc_on_custom_event_0 =
	TEXT("If you passed a custom event, the name is not misspelled: this tool only reaches function graphs, and custom events cannot be made RPCs or ")
	TEXT("renamed headless (FunctionFlags is not writable via reflection). Put the RPC on a function (add_blueprint_function + set_blueprint_function_rpc, ")
	TEXT("with rpc_type capitalized, e.g. 'Server') that calls the custom event. Verify with the returned function_flags and audit_replication; a ")
	TEXT("component RPC also needs replicates=True on its CDO.");

inline constexpr const TCHAR* RB_check_skeleton_outside_game_0 =
	TEXT("This does not prove the asset is missing: the lookup is anchored to /Game and has no search_root, so assets under plugin mounts (/Engine, ")
	TEXT("/MetaHuman...) are invisible. Pass the full object path, including the object name after the dot (e.g. '/Plugin/.../Skel.Skel'), and ")
	TEXT("confirm with get_assets_in_folder on the mount or resolve_asset_by_name (which accepts search_root).");

// {0}=reader tool
inline constexpr const TCHAR* RB_verify_readback_0 =
	TEXT("missing parameters to re-read with {0}");

// {0}=reader tool; {1}=the reader's error (or the repr of its echo)
inline constexpr const TCHAR* RB_verify_readback_1 =
	TEXT("{0} could not read: {1}");

// {0}=reader tool
inline constexpr const TCHAR* RB_verify_readback_2 =
	TEXT("reread:{0}");

inline constexpr const TCHAR* RB_verify_readback_3 =
	TEXT("The write did not land. Cheap positive control: write a trivial scalar and re-read it. For structs or tags on a placed actor, write with ")
	TEXT("set_component_property (MCP) using the complete ExportText.");

}
