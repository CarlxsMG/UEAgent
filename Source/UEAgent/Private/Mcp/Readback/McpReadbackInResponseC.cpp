// IN_RESPONSE (3/3). Almost everything here is a WARNING: degrade=False, it informs without
// touching `success`.

#include "McpReadbackCheckUtil.h"
#include "../McpReadbackChecks.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpReadbackInResponseC
{
	using namespace McpPy;
	using namespace McpReadback;
	using namespace McpReadbackText;
	using namespace McpReadbackUtil;

	/** Whether the payload says success:false. */
	static bool PayloadFailed(const FO& Result)
	{
		return IsFalse(Get(Payload(Result), TEXT("success")));
	}

	// ------------------------------------------------------------------ reads
	/** Reported differences whose two values are equal: over-reporting, not overrides. */
	static FO CheckObjectComparison(const FO& Params, const FO& Result)
	{
		const FV Diffs = Get(Payload(Result), TEXT("differences"));
		if (!IsList(Diffs) || Len(Diffs) == 0)
		{
			return nullptr;
		}
		TArray<FString> FalseDiffs;
		for (const FV& Diff : AsList(Diffs))
		{
			if (!IsDict(Diff))
			{
				continue;
			}
			const FO D = AsDict(Diff);
			for (const TPair<const TCHAR*, const TCHAR*>& FieldPair : { TPair<const TCHAR*, const TCHAR*>(TEXT("value_1"), TEXT("value_2")),
				TPair<const TCHAR*, const TCHAR*>(TEXT("export_text_1"), TEXT("export_text_2")) })
			{
				if (Has(D, FieldPair.Key) && Has(D, FieldPair.Value) && Equals(Get(D, FieldPair.Key), Get(D, FieldPair.Value)))
				{
					const FString Property = TextOf(Get(D, TEXT("property")));
					FalseDiffs.Add((Property.IsEmpty() ? FString(TEXT("?")) : Property) + TEXT("=") + Repr(Get(D, FieldPair.Key)));
					break;
				}
			}
		}
		if (FalseDiffs.Num() == 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("differences_that_are_not"), N(Len(Diffs)), L(Head(FalseDiffs, 4)), TEXT("response:differences[].value_1==value_2"),
			Percent(RB_check_object_comparison_0, { N(FalseDiffs.Num()), N(Len(Diffs)), S(Join(Head(FalseDiffs, 4), TEXT("; "))) }));
	}

	/** The reparent rollback was partial: SCS components were left renamed. */
	static FO CheckReparentRollback(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Unrestored = Get(Data, TEXT("rollback_unrestored_components"));
		const bool bIncomplete = IsFalse(Get(Data, TEXT("rollback_complete")));
		if (!(IsList(Unrestored) && Len(Unrestored) > 0) && !bIncomplete)
		{
			return nullptr;
		}
		TArray<FString> Names;
		for (const FV& Component : AsList(Unrestored))
		{
			if (IsDict(Component))
			{
				const FString Expected = TextOf(Get(Component, TEXT("expected_name")));
				const FString ActualName = TextOf(Get(Component, TEXT("actual_name")));
				Names.Add((Expected.IsEmpty() ? FString(TEXT("?")) : Expected) + TEXT("->") + (ActualName.IsEmpty() ? FString(TEXT("?")) : ActualName));
			}
		}
		const TArray<FString> FirstFew = Head(Names, 4);
		return WarningFinding(TEXT("partial_reparent_rollback"), TextField(Params, TEXT("new_parent_class")),
			FirstFew.Num() > 0 ? L(FirstFew) : Get(Data, TEXT("rollback_complete")), TEXT("response:rollback_unrestored_components"),
			Percent(RB_check_reparent_rollback_0, { S(Names.Num() == 0 ? FString() : TEXT(" (") + Join(FirstFew, TEXT("; ")) + TEXT(")")) }));
	}

	/** set_data_table_row returns no evidence of what was written. */
	static FO CheckRowWithoutEvidence(const FO& Params, const FO& Result)
	{
		if (PayloadFailed(Result) || IsFalse(Get(Result, TEXT("success"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("row_write_without_evidence"), TextField(Params, TEXT("row_name")), TextField(Params, TEXT("table_path")),
			TEXT("response:no verification fields"), RB_check_row_without_evidence_0);
	}

	/** success:true with a graph_name does not prove the function graph exists. */
	static FO CheckNewFunction(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (IsFalse(Get(Data, TEXT("success"))) || IsFalse(Get(Result, TEXT("success"))))
		{
			return nullptr;
		}
		const FString GraphName = TextOf(Get(Data, TEXT("graph_name")));
		if (GraphName.IsEmpty() || IsTrue(Get(Data, TEXT("already_existed"))) || IsTrue(Get(Data, TEXT("already_exists"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("function_graph_unverified"), TextField(Params, TEXT("function_name")), S(GraphName), TEXT("response:success+graph_name"),
			Percent(RB_check_new_function_0, { S(Percent(TEXT(" (you are looking for %r)"), { S(GraphName) })) }));
	}

	/** For an interface EVENT, already_existed:false is unreliable. */
	static FO CheckInterfaceOverride(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsFalse(Get(Data, TEXT("already_existed"))) || !StrEq(Lower(TextOf(Get(Data, TEXT("kind")))), TEXT("event")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("already_existed_unreliable_for_events"), TextField(Params, TEXT("function_name")), TextField(Data, TEXT("kind")),
			TEXT("response:kind+already_existed"), RB_check_interface_override_0);
	}

	/** Select option pins keep generic names, so the option-to-value mapping cannot be read from the graph. */
	static FO CheckSelectPolarity(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV OptionPins = Get(Data, TEXT("option_pins"));
		if (!IsList(OptionPins) || Len(OptionPins) == 0 || !TextOf(Get(Data, TEXT("index_enum"))).IsEmpty())
		{
			return nullptr;
		}
		TArray<FString> GenericPins;
		for (const FV& Pin : AsList(OptionPins))
		{
			if (IsStr(Pin) && StartsWith(Lower(Strip(AsStr(Pin))), TEXT("option")))
			{
				GenericPins.Add(AsStr(Pin));
			}
		}
		if (GenericPins.Num() == 0)
		{
			return nullptr;
		}
		TArray<FString> Quoted;
		for (const FString& Pin : Head(GenericPins, 3))
		{
			Quoted.Add(ReprStr(Pin));
		}
		return WarningFinding(TEXT("select_polarity_unreadable"), TextField(Params, TEXT("index_type")), L(Head(GenericPins, 4)), TEXT("response:option_pins"),
			Percent(RB_check_select_polarity_0, { S(Join(Quoted, TEXT(", "))) }));
	}

	/** activity_tags:[] does not prove the definition has no tags. */
	static FO CheckSmartObjectTags(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Tags = Get(Data, TEXT("activity_tags"));
		if (!IsList(Tags) || Len(Tags) > 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("empty_activity_tags_not_absent"), TextField(Params, TEXT("definition_path")), Get(Data, TEXT("slot_count")),
			TEXT("response:activity_tags"), RB_check_smart_object_tags_0);
	}

	/** Zero matches when searching labels: auto-named actors do not carry the label you expect. */
	static FO CheckNameSearch(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV MatchCount = Get(Data, TEXT("matching_actors_count"));
		if (!IsIntOrBool(MatchCount) || AsNum(MatchCount) > 0)
		{
			return nullptr;
		}
		FString MatchMode = FirstNonEmptyText(Get(Data, TEXT("match")), Get(Params, TEXT("match")));
		MatchMode = Lower(MatchMode.IsEmpty() ? FString(TEXT("both")) : MatchMode);
		if (!StrEq(MatchMode, TEXT("label")) && !StrEq(MatchMode, TEXT("both")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("zero_from_label_search"), TextField(Params, TEXT("pattern")), S(MatchMode), TEXT("response:matching_actors_count+match"),
			Percent(RB_check_name_search_0, { S(MatchMode) }));
	}

	/** An empty execution trace (or an unarmed debug object) does not mean the code did not run. */
	static FO CheckExecutionTrace(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV TracePath = Get(Data, TEXT("path"));
		const bool bEmpty = IsList(TracePath) && Len(TracePath) == 0;
		const bool bNoDebugObject = IsFalse(Get(Data, TEXT("debug_object_set")));
		if (!bEmpty && !bNoDebugObject)
		{
			return nullptr;
		}
		TArray<FString> Warnings;
		if (bEmpty)
		{
			Warnings.Add(RB_check_execution_trace_0);
		}
		if (bNoDebugObject)
		{
			Warnings.Add(RB_check_execution_trace_1);
		}
		Warnings.Add(RB_check_execution_trace_2);
		return WarningFinding(TEXT("empty_trace_not_absence"), TextField(Params, TEXT("blueprint_name")), Get(Data, TEXT("trace_stack_size")),
			TEXT("response:path+debug_object_set"), Join(Warnings, TEXT(" | ")));
	}

	/** Snapping uses the actor's aggregate bounds, not its capsule: large spheres leave it floating. */
	static FO CheckGroundPlacement(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("landed"))) && !IsTrue(Get(Data, TEXT("hit"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("placement_with_aggregate_bounds"), TextField(Params, TEXT("actor_name")), Get(Data, TEXT("hit_distance")),
			TEXT("response:landed"), RB_check_ground_placement_0);
	}

	/** A time dilation of exactly 20 is the clamp CEILING, not necessarily what was requested. */
	static FO CheckTimeDilation(const FO& Params, const FO& Result)
	{
		const FV Value = Get(Payload(Result), TEXT("time_dilation"));
		if (!IsNum(Value) || FMath::Abs(AsNum(Value) - 20.0) > 1e-09)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("dilation_at_clamp_ceiling"), TextField(Params, TEXT("world")), N(AsNum(Value)),
			TEXT("response:time_dilation"), RB_check_time_dilation_0);
	}

	/** material_assigned only covers the Landscape actor (the tool also propagates to streaming proxies). */
	static FO CheckLandscapeMaterial(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("material_assigned"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("landscape_material_propagated"), TextField(Params, TEXT("material_path")), Get(Data, TEXT("landscape_name")),
			TEXT("response:material_assigned"), RB_check_landscape_material_0);
	}

	/** These are the RESOLVED RVT values, not the raw ones that were written. */
	static FO CheckRvtInfo(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsNum(Get(Data, TEXT("tile_count"))) && !IsNum(Get(Data, TEXT("tile_size"))) && !IsNum(Get(Data, TEXT("size"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("rvt_resolved_values"), TextField(Params, TEXT("asset_path")), Get(Data, TEXT("size")),
			TEXT("response:tile_count+tile_size+size"), RB_check_rvt_info_0);
	}

	/** The notify list carries each notify's CLASS name, not its NotifyName. */
	static FO CheckMontageNotifies(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Notifies = Get(Data, TEXT("notifies"));
		if (!IsList(Notifies) || Len(Notifies) == 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("notifies_named_by_class"), TextField(Params, TEXT("montage_path")), Get(Data, TEXT("notify_count")),
			TEXT("response:notifies"), RB_check_montage_notifies_0);
	}

	/** is_valid:true with transition_count:0 is the signature of a broken AnimBP. */
	static FO CheckStateMachine(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("is_valid"))))
		{
			return nullptr;
		}
		const FV TransitionCount = Get(Data, TEXT("transition_count"));
		if (!IsInt(TransitionCount) || AsNum(TransitionCount) != 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("valid_machine_without_transitions"), TextField(Params, TEXT("anim_blueprint_path")), Get(Data, TEXT("state_count")),
			TEXT("response:is_valid+transition_count"), RB_check_state_machine_0);
	}

	/** The bone list is truncated (flagged at 100, or silently at 20) and cannot be paged. */
	static FO CheckTruncatedBones(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const bool bFlagged = IsTrue(Get(Data, TEXT("bones_truncated")));
		const FV Names = Get(Data, TEXT("bone_names"));
		const FV Total = Get(Data, TEXT("num_bones"));
		const bool bSilent = IsList(Names) && IsInt(Total) && AsNum(Total) > Len(Names);
		if (!bFlagged && !bSilent)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("truncated_bone_list"), S(FirstNonEmptyText(Get(Params, TEXT("skeleton_name")), Get(Params, TEXT("skeletal_mesh_path")))),
			bFlagged ? Get(Data, TEXT("bone_count")) : Total,
			bFlagged ? TEXT("response:bones_truncated") : TEXT("response:num_bones>len(bone_names)"),
			FString(bFlagged ? RB_check_truncated_bones_0 : RB_check_truncated_bones_1) + RB_check_truncated_bones_2);
	}

	/** Creating a gameplay tag rewrites the whole tags .ini (reordered, comments dropped). */
	static FO CheckGameplayTag(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FString Tag = FirstNonEmptyText(Get(Data, TEXT("tag_name")), Get(Data, TEXT("tag_string")));
		if (Tag.IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("tag_created_rewriting_ini"), TextField(Params, TEXT("tag_name")), S(Tag), TEXT("response:tag_name"),
			RB_check_gameplay_tag_0);
	}

	/** Only the +Profiles= entry is read: the +EditProfiles= entries applied on top of it are not. */
	static FO CheckCollisionProfileRead(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (TextOf(Get(Data, TEXT("profile_name"))).IsEmpty())
		{
			return nullptr;
		}
		const FV Responses = Get(Data, TEXT("custom_responses"));
		return WarningFinding(TEXT("profile_read_without_editprofiles"), TextField(Params, TEXT("profile_name")), IsList(Responses) ? N(Len(Responses)) : None(),
			TEXT("response:custom_responses"), RB_check_collision_profile_read_0);
	}

	/** The log ring buffer is not anchored to this run (and may be full). */
	static FO CheckLogLines(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsList(Get(Data, TEXT("lines"))))
		{
			return nullptr;
		}
		const FV HeldTotal = Get(Data, TEXT("held_total"));
		const bool bRingFull = IsInt(HeldTotal) && AsNum(HeldTotal) >= 4096;
		FString NoteText = RB_check_log_lines_0;
		if (bRingFull)
		{
			NoteText += Percent(RB_check_log_lines_1, { HeldTotal });
		}
		NoteText += RB_check_log_lines_2;
		return WarningFinding(bRingFull ? TEXT("log_not_time_anchored_ring_full") : TEXT("log_not_time_anchored"), Get(Data, TEXT("count")), HeldTotal,
			TEXT("response:lines+held_total"), NoteText);
	}

	/** This is a static listing, not a trace (and the profiling variant recompiles the Blueprint). */
	static FO CheckStaticAnalysis(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (TextOf(Get(Data, TEXT("blueprint_path"))).IsEmpty())
		{
			return nullptr;
		}
		const bool bProfiles = Has(Data, TEXT("execution_time")) || Has(Data, TEXT("performance_rating")) || Has(Data, TEXT("cpu_usage"));
		FString NoteText = RB_check_static_analysis_0;
		if (bProfiles)
		{
			NoteText += RB_check_static_analysis_1;
		}
		return WarningFinding(TEXT("static_analysis_not_a_trace"), TextField(Params, TEXT("blueprint_path")), Get(Data, TEXT("total_execution_paths")),
			TEXT("response:blueprint_path"), NoteText);
	}

	/** A root canvas created on a child widget eclipses the parent's widget tree. */
	static FO CheckChildWidgetWithRoot(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("root_created"))))
		{
			return nullptr;
		}
		const FString ParentClass = FirstNonEmptyText(Get(Params, TEXT("parent_class")), Get(Data, TEXT("parent_class")));
		if (ParentClass.IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("child_root_shadows_parent"), S(ParentClass), S(FirstNonEmptyText(Get(Data, TEXT("root_widget_class")), Get(Data, TEXT("root_widget")))),
			TEXT("response:root_created+parent_class"), RB_check_child_widget_with_root_0);
	}

	/** The CoreRedirect remaps callers but not the implementers' override graphs. */
	static FO CheckParameterRename(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("renamed"))) && TextOf(Get(Data, TEXT("redirect_registered"))).IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("redirect_skips_overrides"), TextField(Params, TEXT("old_name")), TextField(Data, TEXT("redirect_registered")),
			TEXT("response:renamed+redirect_registered"), RB_check_parameter_rename_0);
	}

	/** row_count may be a composite table's merged count. */
	static FO CheckCompositeTable(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV RowCount = Get(Data, TEXT("row_count"));
		if (!IsInt(RowCount) || Has(Data, TEXT("parent_tables")) || Has(Data, TEXT("merged_row_count")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("row_count_may_be_merged"), TextField(Params, TEXT("table_path")), RowCount,
			TEXT("response:row_count without include_row_sources"), RB_check_composite_table_0);
	}

	/** Player-visible text written without a localization identity. */
	static FO CheckVisibleTextWithoutIdentity(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const bool bWrote = !TextOf(Get(Data, TEXT("text"))).IsEmpty() || IsTrue(Get(Data, TEXT("added"))) || IsIntOrBool(Get(Data, TEXT("mapping_count")));
		if (!bWrote)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("visible_text_without_identity"), S(FirstNonEmptyText(Get(Params, TEXT("text")), Get(Params, TEXT("entry_name")))),
			S(FirstNonEmptyText(Get(Data, TEXT("text")), Get(Data, TEXT("entry_name")))), TEXT("response:text/added/mapping_count"),
			RB_check_visible_text_without_identity_0);
	}

	/** FGrassVariety fields with misleading units (grass_density is per 10 m2, among others). */
	static FO CheckGrassVariety(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsInt(Get(Data, TEXT("variety_index"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("grass_variety_misleading_units"), TextField(Params, TEXT("grass_path")), Get(Data, TEXT("variety_count")),
			TEXT("response:variety_index"), RB_check_grass_variety_0);
	}

	/** The fork was created without its origin tag. */
	static FO CheckForkWithoutTag(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsFalse(Get(Data, TEXT("fork_origin_set"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("fork_missing_origin_tag"), TextField(Params, TEXT("fork_name")), TextField(Data, TEXT("fork_path")),
			TEXT("response:fork_origin_set"), RB_check_fork_without_tag_0);
	}

	/** create_struct does not save to disk: the struct only exists in memory. */
	static FO CheckNewStruct(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (FirstNonEmptyText(Get(Data, TEXT("struct_path")), Get(Data, TEXT("path"))).IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("struct_only_in_memory"), TextField(Params, TEXT("name")), Get(Data, TEXT("member_count")),
			TEXT("response:struct_path"), RB_check_new_struct_0);
	}

	/** PCG regeneration can reuse the cached output instead of the new values. */
	static FO CheckPcgRegenerated(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("scheduled"))) && !IsTrue(Get(Data, TEXT("generated"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("pcg_regenerated_from_cache"), TextField(Params, TEXT("actor_name")), Get(Data, TEXT("task_id")),
			TEXT("response:scheduled+generated"), RB_check_pcg_regenerated_0);
	}

	/** An RPC called from Python does not cross the network: an empty returns_json proves nothing. */
	static FO WarnLocalRpc(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!Has(Data, TEXT("returns_json")))
		{
			return nullptr;
		}
		const FV Returned = Get(Data, TEXT("returns_json"));
		const bool bEmpty = (IsDict(Returned) && Len(Returned) == 0)
			|| (IsStr(Returned) && (StrEq(AsStr(Returned), TEXT("{}")) || AsStr(Returned).IsEmpty())); // in ({}, '{}', '')
		if (!bEmpty || IsFalse(Get(Data, TEXT("success"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("rpc_not_crossing_wire"), TextField(Params, TEXT("function_name")), TextField(Params, TEXT("object_path")),
			TEXT("in-response:returns_json"), RB_warn_local_rpc_0);
	}

	/** A GameplayTag argument passed as a string arrives as None; otherwise falls back to the local-RPC warning. */
	static FO CheckCallWithTag(const FO& Params, const FO& Result)
	{
		const FString Args = TextOf(Get(Params, TEXT("args_json")));
		if (Args.IsEmpty() || !Contains(Lower(Args), TEXT("tag")))
		{
			return WarnLocalRpc(Params, Result);
		}
		return WarningFinding(TEXT("tag_as_string_in_call"), TextField(Params, TEXT("function_name")), TextField(Params, TEXT("object_path")),
			TEXT("params:args_json"), RB_check_call_with_tag_0);
	}

	/** add_function_parameter adds a parameter but nothing can retype it afterwards. */
	static FO CheckFunctionParameter(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (TextOf(Get(Data, TEXT("param_name"))).IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("parameter_added_not_retypable"), TextField(Params, TEXT("param_type")), TextField(Data, TEXT("param_type")),
			TEXT("response:param_name"), RB_check_function_parameter_0);
	}

	/** The nav modifier volume's flags are echoes: they do not distinguish the requested area class. */
	static FO CheckNavModifierVolume(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (IsNone(Get(Data, TEXT("brush_built"))) && IsNone(Get(Data, TEXT("area_class_applied"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("echo_flags_on_compliant_volume"), TextField(Params, TEXT("area_class")), TextField(Data, TEXT("area_class")),
			TEXT("response:brush_built+area_class_applied"), RB_check_nav_modifier_volume_0);
	}

	/** The raw and effective navmesh tile sizes differ: reason with the effective one. */
	static FO CheckNavmeshSettings(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV EffectiveSize = Get(Data, TEXT("tile_size_uu"));
		const FV RawSize = Get(Data, TEXT("tile_size_uu_raw"));
		if (!IsNum(EffectiveSize) || !IsNum(RawSize) || AsNum(EffectiveSize) == AsNum(RawSize))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("navmesh_raw_differs_from_effective"), RawSize, EffectiveSize, TEXT("response:tile_size_uu vs tile_size_uu_raw"),
			Percent(RB_check_navmesh_settings_0, { RawSize, EffectiveSize }));
	}

	/** The new ocean's footprint is not limited by its spline. */
	static FO CheckOceanCreated(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (TextOf(Get(Data, TEXT("water_body_label"))).IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("ocean_created_huge_footprint"), TextField(Params, TEXT("actor_label")), TextField(Data, TEXT("water_zone_label")),
			TEXT("response:water_body_label"), RB_check_ocean_created_0);
	}

	/** The heightmap import works, but `success` is still not the proof: re-read the landscape. */
	static FO CheckHeightmapImport(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("applied"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("heightmap_import_unverified"), TextField(Params, TEXT("file_path")), Get(Data, TEXT("vertices_modified")),
			TEXT("response:applied"), RB_check_heightmap_import_0);
	}

	/** The RVT volume was reused or created; with bounds_align_actor, Z is fitted exactly to the heightmap. */
	static FO CheckRvtVolume(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (TextOf(Get(Data, TEXT("volume_name"))).IsEmpty())
		{
			return nullptr;
		}
		const bool bAlreadyExisted = IsTrue(Get(Data, TEXT("already_existed")));
		const bool bAligned = !TextOf(Get(Params, TEXT("bounds_align_actor"))).IsEmpty();
		return WarningFinding(bAlreadyExisted ? TEXT("rvt_volume_reused") : TEXT("rvt_volume_created"), TextField(Params, TEXT("rvt_asset_path")), TextField(Data, TEXT("volume_name")),
			TEXT("response:volume_name+already_existed"),
			FString(bAlreadyExisted ? RB_check_rvt_volume_0 : RB_check_rvt_volume_1) + RB_check_rvt_volume_2 + (bAligned ? RB_check_rvt_volume_3 : TEXT("")));
	}

	/** The unused list measures referencers == 0, not reachability: islands of assets escape it. */
	static FO CheckUnusedAssets(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsList(Get(Data, TEXT("deletion_candidates"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("unused_measured_by_referencers"), TextField(Params, TEXT("folder_path")), Get(Data, TEXT("unused_count")),
			TEXT("response:deletion_candidates"), RB_check_unused_assets_0);
	}

	/** config_written:true on every call: the tool regenerates its .ini, dropping manual edits. */
	static FO CheckTextGather(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("config_written"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("localization_ini_regenerated"), TextField(Params, TEXT("target_name")), Get(Data, TEXT("exit_code")),
			TEXT("response:config_written"), RB_check_text_gather_0);
	}

	/** A Blueprint-class component type, or one given a transform, can report success without persisting in the SCS. */
	static FO CheckComponentMayNotPersist(const FO& Params, const FO& Result)
	{
		const FString ComponentType = TextOf(Get(Params, TEXT("component_type")));
		const bool bIsBlueprintClass = StartsWith(ComponentType, TEXT("/Game/")) || EndsWith(ComponentType, TEXT("_C"));
		const bool bHasTransform = !IsNone(Get(Params, TEXT("location"))) || !IsNone(Get(Params, TEXT("rotation"))) || !IsNone(Get(Params, TEXT("scale")))
			|| IsDict(Get(Params, TEXT("transform")));
		if (!bIsBlueprintClass && !bHasTransform)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("add_may_not_persist"), Get(Params, TEXT("component_name")), None(), TEXT("in_response:add_component_to_blueprint"),
			Percent(RB_check_component_may_not_persist_0, { S(bIsBlueprintClass ? TEXT("a Blueprint class") : TEXT("a transform")) }));
	}

	/** Python's `float(x)`: a number, bool or numeric string; false where Python would raise. */
	static bool PyFloat(const FV& Value, double& Out)
	{
		if (IsNum(Value) || IsBool(Value))
		{
			Out = AsNum(Value);
			return true;
		}
		if (!IsStr(Value))
		{
			return false;
		}
		const FString Text = Lower(Strip(AsStr(Value)));
		int32 Index = (Text.StartsWith(TEXT("+")) || Text.StartsWith(TEXT("-"))) ? 1 : 0;
		const FString Body = Text.Mid(Index);
		const double Sign = Text.StartsWith(TEXT("-")) ? -1.0 : 1.0;
		if (StrEq(Body, TEXT("inf")) || StrEq(Body, TEXT("infinity")))
		{
			Out = Sign * TNumericLimits<double>::Max() * 2.0;
			return true;
		}
		if (StrEq(Body, TEXT("nan")))
		{
			Out = FMath::Sqrt(-1.0);
			return true;
		}
		bool bDigits = false;
		bool bDot = false;
		bool bExponent = false;
		for (int32 I = 0; I < Body.Len(); ++I)
		{
			const TCHAR C = Body[I];
			if (FChar::IsDigit(C))
			{
				bDigits = true;
			}
			else if (C == TEXT('.') && !bDot && !bExponent)
			{
				bDot = true;
			}
			else if (C == TEXT('e') && bDigits && !bExponent)
			{
				bExponent = true;
				bDigits = false;
				if (I + 1 < Body.Len() && (Body[I + 1] == TEXT('+') || Body[I + 1] == TEXT('-')))
				{
					++I;
				}
			}
			else
			{
				return false;
			}
		}
		if (!bDigits)
		{
			return false;
		}
		Out = FCString::Atod(*Text);
		return true;
	}

	/** The smoothing iterations were clamped silently. */
	static FO CheckClampedSmoothing(const FO& Params, const FO& Result)
	{
		const FV RequestedIterations = Get(Params, TEXT("iterations"));
		const FV AppliedIterations = Get(Payload(Result), TEXT("iterations"));
		if (IsNone(RequestedIterations) || IsNone(AppliedIterations))
		{
			return nullptr;
		}
		double A = 0;
		double B2 = 0;
		if (!PyFloat(RequestedIterations, A) || !PyFloat(AppliedIterations, B2) || A == B2)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("clamped_silently"), RequestedIterations, AppliedIterations, TEXT("in_response:smooth_landscape_in_box"),
			Percent(RB_check_clamped_smoothing_0, { RequestedIterations, AppliedIterations }));
	}

	/** The replication report cannot disconfirm the write: reader and writer look at the same flags. */
	static FO CheckUnverifiableReplication(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("is_replicated"))) && !IsTrue(Get(Data, TEXT("is_rep_notify"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("report_cannot_disconfirm"), Get(Params, TEXT("variable_name")), Get(Data, TEXT("property_flags")),
			TEXT("in_response:set_bp_variable_replication"), RB_check_unverifiable_replication_0);
	}

	// ------------------------------------------------------------------ standing warnings
	/** A warning on every successful call (`how: in-response:warning`), or null when the payload says success:false. */
	static FO StandingWarning(const FO& Result, const TCHAR* Verdict, const FV& Requested, const FV& Stored, const FString& Note)
	{
		return PayloadFailed(Result) ? nullptr : WarningFinding(Verdict, Requested, Stored, TEXT("in-response:warning"), Note);
	}

	/** Relevancy is not dormancy: these calls do not fix a client that does not mirror a change. */
	static FO WarnNetRelevancy(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("net_relevancy_not_dormancy"), TextField(Params, TEXT("actor_name")), S(TEXT("")),
			RB_warn_net_relevancy_0);
	}

	/** Possessing a pawn contaminates later reads of the remote proxy. */
	static FO WarnTamperedProxy(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("remote_proxy_contaminated"), TextField(Params, TEXT("pawn_actor_name")), S(TEXT("")),
			RB_warn_tampered_proxy_0);
	}

	/** Enabling replicated movement gives no headless oracle of the movement mirror. */
	static FO WarnMovementMirror(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("movement_mirror_not_measurable_headless"), TextField(Params, TEXT("actor_label")),
			TextField(Params, TEXT("replicate_movement")), RB_warn_movement_mirror_0);
	}

	/** This water body provides no swimming volume, and nothing reports it. */
	static FO WarnWaterWithoutSwimVolume(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("water_without_swim_volume"), TextField(Params, TEXT("actor_label")),
			TextField(Params, TEXT("water_zone_label")), RB_warn_water_without_swim_volume_0);
	}

	/** The largest of extent[] and size, or unset. */
	static TOptional<double> LongestVolumeSide(const FO& Params)
	{
		TOptional<double> Longest;
		auto Consider = [&Longest](const FV& Value)
		{
			if (IsNum(Value) && (!Longest.IsSet() || AsNum(Value) > *Longest))
			{
				Longest = AsNum(Value);
			}
		};
		for (const FV& Value : AsList(Get(Params, TEXT("extent"))))
		{
			Consider(Value);
		}
		Consider(Get(Params, TEXT("size")));
		return Longest;
	}

	/** Volume caveats: collision does not reach the corners of very large volumes, and water immersion uses the bounds. */
	static FO WarnVolumeCollision(const FO& Params, const FO& Result)
	{
		const TOptional<double> LongestSide = LongestVolumeSide(Params);
		const FString Lead = LongestSide.IsSet() && *LongestSide > 600000.0
			? Percent(RB_warn_volume_collision_0, { N(FMath::TruncToDouble(*LongestSide)) })
			: FString(RB_warn_volume_collision_1);
		return StandingWarning(Result, TEXT("volume_collision_not_transform"),
			S(TextOf(Or(Get(Params, TEXT("volume_class")), Get(Params, TEXT("volume_name"))))),
			S(LongestSide.IsSet() ? Str(N(FMath::TruncToDouble(*LongestSide))) : FString()), Lead + RB_warn_volume_collision_2);
	}

	/** A SplineMeshComponent's render deforms but its collision does not. */
	static FO WarnUndeformedCollision(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("spline_mesh_collision_undeformed"), TextField(Params, TEXT("static_mesh_path")),
			TextField(Params, TEXT("actor_name")), RB_warn_undeformed_collision_0);
	}

	/** Rerunning construction scripts can reset spline actors to their Blueprint default. Only on the FIRST rerun of
	 *  the session. */
	static FO WarnConstructionScriptErases(const FO& Params, const FO& Result)
	{
		if (PayloadFailed(Result))
		{
			return nullptr;
		}
		TArray<FString>& RerunActors = State().UcsRerun;
		const bool bFirst = RerunActors.Num() == 0;
		const FString Actor = TextOf(Get(Params, TEXT("actor_name")));
		SetAdd(RerunActors, Actor.IsEmpty() ? FString(TEXT("-")) : Actor);
		if (!bFirst)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("rebuild_may_erase_layout"), S(Actor), TextField(Params, TEXT("world")),
			TEXT("session:first_ucs_rerun"), RB_warn_construction_script_erases_0);
	}

	/** After setting spline points, any property write reruns the construction script and resets the spline. */
	static FO WarnFragileSplinePoints(const FO& Params, const FO& Result)
	{
		const FV Points = Get(Params, TEXT("points"));
		return StandingWarning(Result, TEXT("spline_points_erased_by_postedit"), TextField(Params, TEXT("actor_name")),
			S(IsList(Points) ? FString::FromInt(Len(Points)) : FString()), RB_warn_fragile_spline_points_0);
	}

	/** Zero HierarchicalInstancedStaticMesh objects does not prove the landscape grass is missing. */
	static FO WarnUncountedGrass(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FString ClassName = Lower(TextOf(Or(Get(Params, TEXT("class_name")), Get(Data, TEXT("class_name")))));
		if (!Contains(ClassName, TEXT("hierarchicalinstancedstaticmesh")))
		{
			return nullptr;
		}
		FV InstanceCount = Get(Data, TEXT("count"));
		if (IsNone(InstanceCount))
		{
			const FV Objects = Get(Data, TEXT("objects"));
			InstanceCount = IsList(Objects) ? N(Len(Objects)) : None();
		}
		if (!Equals(InstanceCount, N(0)))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("grass_zero_proves_nothing"), TextField(Params, TEXT("class_name")), N(0), TEXT("in-response:count"),
			RB_warn_uncounted_grass_0);
	}

	/** The new collision channel is written to the .ini, but the running engine does not know it yet. */
	static FO WarnChannelUnknownToEngine(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("channel_written_not_live"), TextField(Params, TEXT("channel_name")),
			TextField(Params, TEXT("default_response")), RB_warn_channel_unknown_to_engine_0);
	}

	/** Nesting another Widget Blueprint inherits its look but not its lifecycle. Only for a /Game widget_type. */
	static FO WarnWidgetWithoutLifecycle(const FO& Params, const FO& Result)
	{
		if (PayloadFailed(Result))
		{
			return nullptr;
		}
		const FString WidgetType = TextOf(Get(Params, TEXT("widget_type")));
		if (!StartsWith(WidgetType, TEXT("/Game")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("nested_widget_without_destruct"), S(WidgetType), TextField(Params, TEXT("widget_bp_path")), TEXT("params:widget_type"),
			RB_warn_widget_without_lifecycle_0);
	}

	/** Injected input does not drive the client copy. Only for a CLIENT (index >= 1). */
	static FO WarnClientKeyInput(const FO& Params, const FO& Result)
	{
		if (PayloadFailed(Result))
		{
			return nullptr;
		}
		const FV ClientIndex = Get(Params, TEXT("pie_client_index"));
		if (!IsNum(ClientIndex) || AsNum(ClientIndex) < 1)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("client_input_not_driving"), TextField(Params, TEXT("key")), PyInt(ClientIndex), TEXT("params:pie_client_index"),
			RB_warn_client_key_input_0);
	}

	/** Injected aim does not feed interaction targeting. */
	static FO WarnInjectedAim(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("aim_not_feeding_interact"), TextField(Params, TEXT("target_name")),
			O(Dict({ { TEXT("yaw"), Get(Params, TEXT("yaw")) }, { TEXT("pitch"), Get(Params, TEXT("pitch")) } })), RB_warn_injected_aim_0);
	}

	/** Checking the ends of a painted range does not validate the middle. */
	static FO WarnUnlocatedRegion(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("painted_region_unlocated"), TextField(Params, TEXT("actor_name")), Get(Params, TEXT("material_id")),
			RB_warn_unlocated_region_0);
	}

	/** Always speaks: without window focus the editor is throttled, so frame_ms measures the idle editor. */
	static FO WarnThrottledFrame(const FO& Params, const FO& Result)
	{
		const FV Ms = Get(Payload(Result), TEXT("frame_ms"));
		const FString Measured = IsNum(Ms) && AsNum(Ms) > 300 ? Percent(RB_warn_throttled_frame_0, { Ms }) : FString();
		return WarningFinding(TEXT("frame_timing_throttled"), None(), Ms, TEXT("in-response:frame_ms"),
			Percent(RB_warn_throttled_frame_1, { S(Measured) }));
	}

	/** is_pie_active:false does not tell "stopped" from "still loading". */
	static FO WarnPieStillLoading(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsFalse(Get(Data, TEXT("is_pie_active"))))
		{
			return nullptr;
		}
		FString Status = TextOf(Get(Data, TEXT("status")));
		Status = Status.IsEmpty() ? FString(TEXT("not reported")) : Status;
		FString LevelName = TextOf(Get(Data, TEXT("current_level")));
		LevelName = LevelName.IsEmpty() ? FString(TEXT("not reported")) : LevelName;
		return WarningFinding(TEXT("pie_inactive_reason_missing"), None(),
			O(Dict({ { TEXT("status"), S(Status) }, { TEXT("current_level"), S(LevelName) } })), TEXT("in-response:is_pie_active"),
			Percent(RB_warn_pie_still_loading_0, { S(Status), S(LevelName) }));
	}

	/** The PIE world may not start clean (an auto-save can be reloaded). Only on the FIRST PIE start of the session. */
	static FO WarnPieNotStartingClean(const FO& Params, const FO& Result)
	{
		if (PayloadFailed(Result))
		{
			return nullptr;
		}
		TArray<FString>& Launched = State().PieLaunched;
		const bool bFirst = Launched.Num() == 0;
		const FString LevelName = TextOf(Or(Get(Params, TEXT("level_name")), Get(Params, TEXT("map_name"))));
		SetAdd(Launched, LevelName.IsEmpty() ? FString(TEXT("-")) : LevelName);
		if (!bFirst)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("pie_not_starting_clean"), S(LevelName), S(TEXT("")), TEXT("session:first_pie_launch"),
			RB_warn_pie_not_starting_clean_0);
	}

	/** Reading is not harmless here: this tool loads Blueprints from disk, changing what the editor has in memory. */
	static FO WarnSweepLoadsBlueprints(const FO& Params, const FO& Result)
	{
		return StandingWarning(Result, TEXT("sweep_loads_blueprints"),
			S(TextOf(Or(Get(Params, TEXT("class_name")), Get(Params, TEXT("blueprint_name")), Get(Params, TEXT("base_class"))))), S(TEXT("")),
			RB_warn_sweep_loads_blueprints_0);
	}
}

// NOTE: no namespace-level `using namespace` here: in a unity build it would leak into the following files.
namespace McpReadbackUtil
{
	/** Adding a collision profile re-serializes the whole DefaultEngine.ini. Fallback of add_collision_profile. */
	FO WarnCollisionIni(const FO& Params, const FO& Result)
	{
		using namespace McpPy;
		const FO Data = Payload(Result);
		if (TextOf(Get(Data, TEXT("profile_name"))).IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("profile_write_reserializes_ini"), TextField(Params, TEXT("profile_name")), Get(Data, TEXT("already_existed")),
			TEXT("response:profile_name"), McpReadbackText::RB_warn_collision_ini_0);
	}

	/** In an array rotation the third component is ROLL, not yaw. Fallback of spawn_blueprint_actor. */
	FO WarnArrayRotation(const FO& Params, const FO& Result)
	{
		using namespace McpPy;
		const FV Rot = Get(Params, TEXT("rotation"));
		if (!IsList(Rot) || Len(Rot) < 3)
		{
			return nullptr;
		}
		const FV ThirdValue = AsList(Rot)[2];
		if (!IsNum(ThirdValue) || AsNum(ThirdValue) == 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("rotation_roll_third"), Rot, ThirdValue, TEXT("params:rotation"),
			Percent(McpReadbackText::RB_warn_array_rotation_0, { ThirdValue }));
	}

	/** A list, or text that looks like one ("[...]" / "(...)"). */
	static bool LooksLikeContainer(const FV& Value)
	{
		if (McpPy::IsList(Value))
		{
			return true;
		}
		const FString Text = McpPy::TextOf(Value);
		return McpPy::StartsWith(Text, TEXT("[")) || McpPy::StartsWith(Text, TEXT("("));
	}

	/** A container default reaches the CDO but not the SCS template or placed actors. Fallback of
	 *  set_bp_variable_default. */
	FO WarnArrayNotPropagated(const FO& Params, const FO& Result)
	{
		using namespace McpPy;
		const FO Data = Payload(Result);
		if (IsFalse(Get(Data, TEXT("success"))))
		{
			return nullptr;
		}
		if (!LooksLikeContainer(Get(Params, TEXT("value"))) && !LooksLikeContainer(Get(Data, TEXT("default_value"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("array_stays_in_cdo"), Get(Params, TEXT("value")), Get(Data, TEXT("default_value")), TEXT("params:value"),
			McpReadbackText::RB_warn_array_not_propagated_0);
	}
}

void McpReadbackChecks::RegisterInResponseC(McpReadback::FTables& Tables)
{
	using namespace McpReadbackInResponseC;
	TMap<FString, McpReadback::FInResponse>& T = Tables.InResponse;
	auto AddForEach = [&T](std::initializer_list<const TCHAR*> Commands, McpReadback::FInResponse Check)
	{
		for (const TCHAR* Command : Commands)
		{
			T.Add(Command, Check);
		}
	};
	T.Add(TEXT("compare_objects"), &CheckObjectComparison);
	T.Add(TEXT("reparent_blueprint_safe"), &CheckReparentRollback);
	T.Add(TEXT("set_data_table_row"), &CheckRowWithoutEvidence);
	T.Add(TEXT("add_blueprint_function"), &CheckNewFunction);
	T.Add(TEXT("implement_interface_function"), &CheckInterfaceOverride);
	T.Add(TEXT("add_select_node"), &CheckSelectPolarity);
	T.Add(TEXT("get_smart_object_info"), &CheckSmartObjectTags);
	T.Add(TEXT("find_actors_by_name"), &CheckNameSearch);
	T.Add(TEXT("get_blueprint_exec_path"), &CheckExecutionTrace);
	T.Add(TEXT("snap_actor_to_ground"), &CheckGroundPlacement);
	AddForEach({ TEXT("get_world_settings"), TEXT("get_world_settings_detail") }, &CheckTimeDilation);
	T.Add(TEXT("set_landscape_material"), &CheckLandscapeMaterial);
	T.Add(TEXT("get_rvt_info"), &CheckRvtInfo);
	T.Add(TEXT("get_montage_info"), &CheckMontageNotifies);
	T.Add(TEXT("validate_anim_state_machine"), &CheckStateMachine);
	AddForEach({ TEXT("get_skeleton_info"), TEXT("get_skeletal_mesh_info") }, &CheckTruncatedBones);
	T.Add(TEXT("create_gameplay_tag"), &CheckGameplayTag);
	T.Add(TEXT("get_collision_profile"), &CheckCollisionProfileRead);
	T.Add(TEXT("get_output_log_lines"), &CheckLogLines);
	AddForEach({ TEXT("trace_blueprint_execution"), TEXT("profile_blueprint_execution") }, &CheckStaticAnalysis);
	T.Add(TEXT("create_widget_blueprint"), &CheckChildWidgetWithRoot);
	T.Add(TEXT("rename_function_parameter"), &CheckParameterRename);
	T.Add(TEXT("get_data_table_info"), &CheckCompositeTable);
	AddForEach({ TEXT("add_subtitle_cue"), TEXT("set_subtitle_cue_text"), TEXT("add_enum_entry"), TEXT("map_input_action") }, &CheckVisibleTextWithoutIdentity);
	AddForEach({ TEXT("get_landscape_grass_type_properties"), TEXT("set_landscape_grass_type_properties") }, &CheckGrassVariety);
	T.Add(TEXT("fork_class_chain"), &CheckForkWithoutTag);
	T.Add(TEXT("create_struct"), &CheckNewStruct);
	T.Add(TEXT("generate_pcg_component"), &CheckPcgRegenerated);
	T.Add(TEXT("call_object_method"), &CheckCallWithTag);
	T.Add(TEXT("add_function_parameter"), &CheckFunctionParameter);
	T.Add(TEXT("spawn_nav_modifier_volume"), &CheckNavModifierVolume);
	T.Add(TEXT("get_recast_navmesh_settings"), &CheckNavmeshSettings);
	T.Add(TEXT("create_ocean_water_body"), &CheckOceanCreated);
	T.Add(TEXT("import_landscape_heightmap"), &CheckHeightmapImport);
	T.Add(TEXT("create_rvt_volume"), &CheckRvtVolume);
	T.Add(TEXT("find_unused_assets"), &CheckUnusedAssets);
	T.Add(TEXT("gather_text_for_target"), &CheckTextGather);
	T.Add(TEXT("add_component_to_blueprint"), &CheckComponentMayNotPersist);
	T.Add(TEXT("smooth_landscape_in_box"), &CheckClampedSmoothing);
	T.Add(TEXT("set_bp_variable_replication"), &CheckUnverifiableReplication);
	AddForEach({ TEXT("force_net_update"), TEXT("flush_net_dormancy") }, &WarnNetRelevancy);
	T.Add(TEXT("possess_pawn"), &WarnTamperedProxy);
	T.Add(TEXT("set_actor_replicate_movement"), &WarnMovementMirror);
	AddForEach({ TEXT("create_lake_water_body"), TEXT("create_river_water_body") }, &WarnWaterWithoutSwimVolume);
	AddForEach({ TEXT("spawn_box_volume"), TEXT("create_volume") }, &WarnVolumeCollision);
	T.Add(TEXT("add_spline_mesh_component"), &WarnUndeformedCollision);
	T.Add(TEXT("rerun_construction_scripts"), &WarnConstructionScriptErases);
	T.Add(TEXT("set_spline_points"), &WarnFragileSplinePoints);
	T.Add(TEXT("list_objects_of_class"), &WarnUncountedGrass);
	T.Add(TEXT("add_collision_channel"), &WarnChannelUnknownToEngine);
	T.Add(TEXT("add_widget_extended"), &WarnWidgetWithoutLifecycle);
	T.Add(TEXT("inject_key_input"), &WarnClientKeyInput);
	T.Add(TEXT("set_control_rotation"), &WarnInjectedAim);
	T.Add(TEXT("set_mesh_material_id_in_region"), &WarnUnlocatedRegion);
	T.Add(TEXT("frame_timing"), &WarnThrottledFrame);
	T.Add(TEXT("is_play_in_editor_active"), &WarnPieStillLoading);
	T.Add(TEXT("start_play_in_editor"), &WarnPieNotStartingClean);
	AddForEach({ TEXT("list_derived_classes"), TEXT("get_blueprint_scs_tree"), TEXT("get_class_hierarchy") }, &WarnSweepLoadsBlueprints);
}
