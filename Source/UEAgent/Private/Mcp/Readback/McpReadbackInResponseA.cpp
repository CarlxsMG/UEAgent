// IN_RESPONSE (1/3): the evidence is already in the command's response -- zero cost.

#include "McpReadbackCheckUtil.h"
#include "../McpReadbackChecks.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpReadbackInResponseA
{
	using namespace McpPy;
	using namespace McpReadback;
	using namespace McpReadbackText;
	using namespace McpReadbackUtil;

	static const TCHAR* const NetModeClient = TEXT("Client"); // net_mode of a client world

	// ------------------------------------------------------------------ writes that echo
	/** The echoed default_value is empty or unchanged: the write did not land (roundtrip_verified does not prove
	 *  it). Otherwise falls back to the container-default warning. */
	static FO CheckBpVariableDefault(const FO& Params, const FO& Result)
	{
		const FV Stored = Get(Payload(Result), TEXT("default_value"));
		const FString Verdict = CheckNonEmptyStored(Params, Stored, TEXT("value"));
		if (Verdict.IsEmpty())
		{
			return WarnArrayNotPropagated(Params, Result);
		}
		return DegradingFinding(Verdict, Get(Params, TEXT("value")), Stored, TEXT("in_response:default_value"), RB_check_bp_variable_default_0);
	}

	/** Fewer per-channel responses reached the profile than were requested (lowercase keys are dropped
	 *  silently). Otherwise falls back to the config-file warning. */
	static FO CheckCollisionProfile(const FO& Params, const FO& Result)
	{
		const FV Requested = Get(Params, TEXT("custom_responses"));
		if (!IsList(Requested) || Len(Requested) == 0)
		{
			return WarnCollisionIni(Params, Result);
		}
		const FV Count = Get(Payload(Result), TEXT("custom_response_count"));
		if (!IsInt(Count) || AsNum(Count) >= Len(Requested))
		{
			return WarnCollisionIni(Params, Result);
		}
		return DegradingFinding(AsNum(Count) == 0 ? TEXT("wrote_nothing") : TEXT("partial"), N(Len(Requested)), Count, TEXT("in_response:custom_response_count"),
			RB_check_collision_profile_0);
	}

	/** The Blueprint does not compile (succeeded:false or num_errors > 0). */
	static FO CheckCompileBlueprint(const FO& Params, const FO& Result)
	{
		const FO P = Payload(Result);
		const FV NumErrors = Get(P, TEXT("num_errors"));
		const bool bFailed = IsFalse(Get(P, TEXT("succeeded"))) || (IsInt(NumErrors) && AsNum(NumErrors) > 0);
		if (!bFailed)
		{
			return nullptr;
		}
		return DegradingFinding(TEXT("compile_failed"), S(TEXT("0 errors")), S(Percent(TEXT("%s errors"), { IsIntOrBool(NumErrors) ? NumErrors : S(TEXT("?")) })),
			TEXT("in_response:num_errors"), RB_check_compile_blueprint_0);
	}

	static const TCHAR* const LiveWorlds[] = { TEXT("pie"), TEXT("server"), TEXT("client"), TEXT("game") };

	template <int32 Count>
	static bool InTable(const FString& Value, const TCHAR* const (&Table)[Count])
	{
		for (const TCHAR* Item : Table)
		{
			if (StrEq(Value, Item))
			{
				return true;
			}
		}
		return false;
	}

	/** Parameters with a known side effect: verify_persistence, a live world, DefinitionAsset, user-struct properties. */
	static FO CheckComponentParameters(const FO& Params, const FO& Result)
	{
		TArray<FString> Warnings;
		if (IsTrue(Get(Params, TEXT("verify_persistence"))))
		{
			Warnings.Add(RB_check_component_parameters_0);
		}
		const FString WorldName = LowerStrip(Get(Params, TEXT("world")));
		if (!WorldName.IsEmpty() && (InTable(WorldName, LiveWorlds) || StartsWith(WorldName, TEXT("pie"))))
		{
			Warnings.Add(Percent(RB_check_component_parameters_1, { S(WorldName) }));
		}
		const FV Prop = Get(Params, TEXT("property_name"));
		if (IsStr(Prop) && StrEq(Strip(AsStr(Prop)), TEXT("DefinitionAsset")))
		{
			Warnings.Add(RB_check_component_parameters_2);
		}
		return JoinedWarningFinding(TEXT("parameter_with_measured_effect"), Prop, None(), TEXT("params:verify_persistence+world+property_name"), Warnings);
	}

	/** final_value is the re-read value: empty or unchanged means the write did not land. Otherwise checks the
	 *  parameters that have a known side effect. */
	static FO CheckComponentProperty(const FO& Params, const FO& Result)
	{
		const FV Stored = Get(Payload(Result), TEXT("final_value"));
		const FString Verdict = CheckNonEmptyStored(Params, Stored, TEXT("property_value"));
		if (Verdict.IsEmpty())
		{
			return CheckComponentParameters(Params, Result);
		}
		return DegradingFinding(Verdict, Get(Params, TEXT("property_value")), Stored, TEXT("in_response:final_value"), RB_check_component_property_0);
	}

	/** The comment box kept the literal 'Comment' instead of the requested text. */
	static FO CheckCommentNode(const FO& Params, const FO& Result)
	{
		const FV Requested = Get(Params, TEXT("text"));
		if (IsEmpty(Requested))
		{
			return nullptr;
		}
		const FV Stored = Get(Payload(Result), TEXT("comment"));
		if (!IsStr(Stored) || !StrEq(Strip(AsStr(Stored)), TEXT("Comment")) || StrEq(Strip(Str(Requested)), TEXT("Comment")))
		{
			return nullptr;
		}
		return DegradingFinding(TEXT("text_ignored"), Requested, Stored, TEXT("in-response:comment"), RB_check_comment_node_0);
	}

	// ------------------------------------------------------------------ reads that mislead
	/** The node scan is not exhaustive: assets that hit the prefilter without a graph match, or skipped foreign
	 *  subgraphs. */
	static FO CheckNodeReferences(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV MatchesFound = Get(Data, TEXT("matches_found"));
		if (!IsInt(MatchesFound) || AsNum(MatchesFound) < 0)
		{
			return nullptr;
		}
		const FV PrefilterHits = Get(Data, TEXT("assets_hit_prefilter_without_graph_match"));
		const FV SkippedSubgraphs = Get(Data, TEXT("foreign_subgraphs_skipped"));
		TArray<FString> Hints;
		if (IsList(PrefilterHits) && Len(PrefilterHits) > 0)
		{
			Hints.Add(Percent(RB_check_node_references_0, { N(Len(PrefilterHits)), S(JoinStr(Head(AsList(PrefilterHits), 3), TEXT(", "))) }));
		}
		if (IsNum(SkippedSubgraphs) && AsNum(SkippedSubgraphs) > 0)
		{
			Hints.Add(Percent(TEXT("%d foreign subgraphs were skipped"), { PyInt(SkippedSubgraphs) }));
		}
		if (Hints.Num() == 0)
		{
			return nullptr;
		}
		if (AsNum(MatchesFound) > 0)
		{
			return WarningFinding(TEXT("matches_not_exhaustive"), MatchesFound, MatchesFound, RB_check_node_references_1,
				Percent(RB_check_node_references_2, { MatchesFound, S(Join(Hints, TEXT("; "))) }));
		}
		return WarningFinding(TEXT("zero_not_proven"), N(0), N(0), RB_check_node_references_3,
			FString(RB_check_node_references_4) + Join(Hints, TEXT("; ")) + RB_check_node_references_5);
	}

	/** The actor's Blueprint was compiled this session: compiling does not rebuild existing actors, so the read can be
	 *  stale. */
	static FO CheckReadAfterCompile(const FO& Params, const FO& Result)
	{
		const TArray<FString>& Compiled = State().CompiledBlueprints;
		if (Compiled.Num() == 0)
		{
			return nullptr;
		}
		const FString Actor = TextOf(Get(Params, TEXT("actor_name")));
		if (Actor.IsEmpty())
		{
			return nullptr;
		}
		TArray<FString> Affected;
		for (const FString& Bp : Compiled)
		{
			if (!Bp.IsEmpty() && Contains(Lower(Actor), Lower(Bp)))
			{
				Affected.Add(Bp);
			}
		}
		if (Affected.Num() == 0)
		{
			return nullptr;
		}
		Affected = Sorted(Affected);
		return WarningFinding(TEXT("stale_after_compile"), S(Actor), L(Affected), TEXT("session:prior compile_blueprint"),
			Percent(RB_check_read_after_compile_0, { S(Join(Affected, TEXT(", "))) }));
	}

	/** A census was taken in a PIE world this session: its counts are not the map's, so do not decide deletions from it. */
	static FO CheckDeleteAfterPieCensus(const FO& Params, const FO& Result)
	{
		const TArray<FString>& Census = State().PieCensus;
		if (Census.Num() == 0)
		{
			return nullptr;
		}
		const TArray<FString> Packages = Sorted(Census);
		return WarningFinding(TEXT("count_from_other_world"), Or(Get(Params, TEXT("name")), Get(Params, TEXT("actor_names"))), L(Packages),
			TEXT("session:prior census in PIE world"), Percent(RB_check_delete_after_pie_census_0, { S(Join(Packages, TEXT(", "))) }));
	}

	/** reference_count is a hardcoded constant, not a count. */
	static FO CheckTagRefcount(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!Has(Data, TEXT("reference_count")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("count_is_constant"), Get(Params, TEXT("tag_name")), Get(Data, TEXT("reference_count")), TEXT("in-response:reference_count"),
			RB_check_tag_refcount_0);
	}

	/**
	 * A tiny count behind a huge sweep does not prove UNIQUENESS: the sweep only sees LOADED classes.
	 * It only speaks for that shape (count <= 2, 1000+ classes scanned); otherwise this tool is a
	 * reliable instrument.
	 */
	static FO CheckInterfaceSweep(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Count = Get(Data, TEXT("count"));
		const FV ScannedClasses = Get(Data, TEXT("scanned_class_count"));
		if (!IsInt(Count) || !IsNum(ScannedClasses))
		{
			return nullptr;
		}
		if (AsNum(Count) > 2 || AsNum(ScannedClasses) < 1000)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("sweep_not_exhaustive"), Get(Params, TEXT("interface")), Count, TEXT("in-response:count + scanned_class_count"),
			Percent(RB_check_interface_sweep_0, { Count, PyInt(ScannedClasses) }));
	}

	/** /Game/A/B and /Game/A/B.B are the same path: drops an object name that repeats the package's. */
	static FString StripObjectName(const FV& Value)
	{
		const FString T = TextOf(Value);
		int32 Dot = INDEX_NONE;
		if (!T.FindLastChar(TEXT('.'), Dot))
		{
			return T;
		}
		const FString PackagePath = T.Left(Dot);
		const FString ObjectName = T.Mid(Dot + 1);
		int32 Slash = INDEX_NONE;
		const FString LastSegment = PackagePath.FindLastChar(TEXT('/'), Slash) ? PackagePath.Mid(Slash + 1) : PackagePath;
		if (!ObjectName.IsEmpty() && StrEq(LastSegment, ObjectName))
		{
			return PackagePath;
		}
		return T;
	}

	static bool SamePathWithOrWithoutObject(const FV& Lhs, const FV& Rhs)
	{
		const FString A = StripObjectName(Lhs);
		const FString B = StripObjectName(Rhs);
		return !A.IsEmpty() && StrEq(A, B);
	}

	/** memory_matches_disk:false is an AND over several keys: report which keys DO match, and package-vs-object format
	 *  mismatches. */
	static FO CheckDefaultMapAggregate(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsFalse(Get(Data, TEXT("memory_matches_disk"))))
		{
			return nullptr;
		}
		const FV Checks = Get(Data, TEXT("disk_checks"));
		if (!IsList(Checks) || Len(Checks) == 0)
		{
			return nullptr;
		}
		TArray<FString> Matching;
		TArray<FString> Mismatching;
		TArray<FString> FormatOnly;
		for (const FV& Entry : AsList(Checks))
		{
			if (!IsDict(Entry))
			{
				continue;
			}
			const FString Key = Str(Or(Get(Entry, TEXT("property_name")), Get(Entry, TEXT("ini_key")), S(TEXT("?"))));
			bool bMatches = IsTrue(Get(Entry, TEXT("disk_matches_memory")));
			if (!bMatches && SamePathWithOrWithoutObject(Get(Entry, TEXT("memory_value")), Get(Entry, TEXT("disk_value"))))
			{
				bMatches = true;
				FormatOnly.Add(Key);
			}
			(bMatches ? Matching : Mismatching).Add(Key);
		}
		if (Matching.Num() == 0)
		{
			return nullptr;
		}
		const FString MismatchingText = Join(Mismatching, TEXT(", "));
		return WarningFinding(TEXT("aggregate_verdict"), None(),
			O(Dict({ { TEXT("matching"), L(Matching) }, { TEXT("not_matching"), L(Mismatching) }, { TEXT("format_only"), L(FormatOnly) } })),
			TEXT("in-response:disk_checks[]"),
			Percent(RB_check_default_map_aggregate_0, { S(Join(Matching, TEXT(", "))), S(MismatchingText.IsEmpty() ? FString(TEXT("another key")) : MismatchingText) })
				+ (FormatOnly.Num() > 0 ? Percent(RB_check_default_map_aggregate_1, { S(Join(FormatOnly, TEXT(", "))) }) : FString()));
	}

	/** small_touch_target:0 proves nothing: only canvas slots with an authored size are checked. */
	static FO CheckBlindTouchTarget(const FO& Params, const FO& Result)
	{
		const FV ByKind = Get(Payload(Result), TEXT("issues_by_kind"));
		if (!IsDict(ByKind))
		{
			return nullptr;
		}
		const FV SmallTargets = Get(ByKind, TEXT("small_touch_target"));
		if (!IsNum(SmallTargets) || AsNum(SmallTargets) != 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("zero_not_proven"), Get(Params, TEXT("folder_path")), N(0), TEXT("in-response:issues_by_kind.small_touch_target"),
			RB_check_blind_touch_target_0);
	}

	/** A zero from the World Partition actor listing does not prove the actor is not placed. */
	static FO CheckZeroWpActors(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("is_world_partition"))))
		{
			return nullptr;
		}
		const FV Count = Get(Data, TEXT("count"));
		const FV Total = Get(Data, TEXT("total_matched"));
		if (!IsNum(Count) || AsNum(Count) != 0)
		{
			return nullptr;
		}
		if (IsNum(Total) && AsNum(Total) != 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("zero_not_proven"), Or(Get(Params, TEXT("filter_class")), Get(Params, TEXT("name_contains")), Get(Params, TEXT("folder"))), N(0),
			TEXT("in-response:count"), RB_check_zero_wp_actors_0);
	}

	// ------------------------------------------------------------------ the wrong world
	/** The actor was spawned without authority (a client-only ghost); otherwise falls back to the array-rotation
	 *  warning. */
	static FO CheckSpawnWithoutAuthority(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Authority = Get(Data, TEXT("has_authority"));
		const FV NetMode = Get(Data, TEXT("net_mode"));
		const FV RequestedWorld = Get(Params, TEXT("world"));
		const bool bNoAuthority = IsFalse(Authority);
		const bool bClientMode = IsStr(NetMode) && StrEq(AsStr(NetMode), NetModeClient);
		const bool bLocalOnClient = bClientMode && !(IsStr(RequestedWorld) && StrEq(Lower(Strip(AsStr(RequestedWorld))), TEXT("client")));
		if (!bNoAuthority && !bLocalOnClient)
		{
			return WarnArrayRotation(Params, Result);
		}
		const FO Stored = Dict({
			{ TEXT("world"), Get(Data, TEXT("world")) },
			{ TEXT("world_package"), Get(Data, TEXT("world_package")) },
			{ TEXT("local_role"), Get(Data, TEXT("local_role")) },
			{ TEXT("net_mode"), NetMode },
		});
		return WarningFinding(TEXT("spawned_without_authority"), Or(Get(Params, TEXT("actor_name")), Get(Params, TEXT("blueprint_name"))), O(Stored),
			TEXT("in-response:has_authority/net_mode"), Percent(RB_check_spawn_without_authority_0, {
				Or(Get(Data, TEXT("local_role")), Get(Data, TEXT("world_package")), S(TEXT("role not reported"))),
				Or(NetMode, S(TEXT("not reported"))) }));
	}

	/** replicated_class_count:0 with replication signals declared: the column is blind to component Blueprints. */
	static FO CheckReplicationAudit(const FO& Params, const FO& Result)
	{
		const FV Summary = Get(Payload(Result), TEXT("summary"));
		if (!IsDict(Summary))
		{
			return nullptr;
		}
		auto SummaryInt = [&Summary](const TCHAR* Key) -> FV
		{
			const FV Value = Get(Summary, Key);
			return IsNum(Value) ? PyInt(Value) : None();
		};
		const FV ReplicatedClasses = SummaryInt(TEXT("replicated_class_count"));
		const FV Rpcs = SummaryInt(TEXT("rpc_count"));
		const FV Variables = SummaryInt(TEXT("replicated_variable_count"));
		if (IsNone(ReplicatedClasses) || AsNum(ReplicatedClasses) != 0)
		{
			return nullptr;
		}
		const double ReplicationSignals = AsNum(Rpcs) + AsNum(Variables);
		if (ReplicationSignals <= 0)
		{
			return nullptr;
		}
		const FO Stored = Dict({
			{ TEXT("replicated_class_count"), ReplicatedClasses },
			{ TEXT("rpc_count"), Rpcs },
			{ TEXT("replicated_variable_count"), Variables },
			{ TEXT("blueprint_count"), SummaryInt(TEXT("blueprint_count")) },
		});
		return WarningFinding(TEXT("column_blind_to_components"), Get(Params, TEXT("folder_path")), O(Stored), TEXT("in-response:summary.replicated_class_count"),
			Percent(RB_check_replication_audit_0, { N(ReplicationSignals) }));
	}

	/** An ambiguous world selector ('pie'/'game') resolved to a world without authority. Needs to know which command
	 *  called. */
	static FO CheckAmbiguousWorldRead(const FString& Name, const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV RequestedWorld = Get(Params, TEXT("world"));
		bool bAmbiguous = false;
		if (!IsStr(RequestedWorld))
		{
			bAmbiguous = StrEq(Name, TEXT("get_actor_properties_by_class")); // its default world is ambiguous
		}
		else
		{
			const FString RequestedWorldText = Lower(Strip(AsStr(RequestedWorld)));
			bAmbiguous = StrEq(RequestedWorldText, TEXT("pie")) || StrEq(RequestedWorldText, TEXT("game")); // ambiguous selectors
		}
		if (!bAmbiguous)
		{
			return nullptr;
		}
		const FV NetMode = Get(Data, TEXT("net_mode"));
		const FV Authority = Get(Data, TEXT("has_authority"));
		const bool bNoAuthority = (IsStr(NetMode) && StrEq(AsStr(NetMode), NetModeClient)) || IsFalse(Authority);
		if (!bNoAuthority)
		{
			return nullptr;
		}
		const FString Extra = StrEq(Name, TEXT("diagnose_navmesh")) ? FString(RB_check_ambiguous_world_read_0) : FString();
		const FO Stored = Dict({
			{ TEXT("net_mode"), NetMode },
			{ TEXT("pie_instance"), Get(Data, TEXT("pie_instance")) },
			{ TEXT("world_package"), Get(Data, TEXT("world_package")) },
			{ TEXT("has_authority"), Authority },
		});
		return WarningFinding(TEXT("ambiguous_world"), !IsNone(RequestedWorld) ? RequestedWorld : S(TEXT("(no selector: the default is 'pie')")), O(Stored),
			TEXT("in-response:net_mode/has_authority"), Percent(RB_check_ambiguous_world_read_1, {
				Or(NetMode, S(TEXT("not reported"))), Or(Get(Data, TEXT("world_package")), S(TEXT("not reported"))), S(Extra) }));
	}

	/** Every listed subsystem is native: the tool walks the editor world only, so per-world Blueprint subsystems are
	 *  missing. */
	static FO CheckSubsystemListEditor(const FO& Params, const FO& Result)
	{
		const FV Instances = Get(Payload(Result), TEXT("instances"));
		if (!IsList(Instances) || Len(Instances) == 0)
		{
			return nullptr;
		}
		TArray<FString> Paths;
		for (const FV& Instance : AsList(Instances))
		{
			const FV Path = IsDict(Instance) ? Get(Instance, TEXT("class_path")) : None();
			if (IsStr(Path) && !AsStr(Path).IsEmpty())
			{
				Paths.Add(AsStr(Path));
			}
		}
		if (Paths.Num() == 0)
		{
			return nullptr;
		}
		for (const FString& Path : Paths)
		{
			if (!StartsWith(Path, TEXT("/Script/")))
			{
				return nullptr;
			}
		}
		return WarningFinding(TEXT("editor_world_only"), Get(Params, TEXT("lifetime")), N(Paths.Num()), TEXT("in-response:instances[].class_path"),
			Percent(RB_check_subsystem_list_editor_0, { N(Paths.Num()) }));
	}

	/** Slots are the loose .sav files at the root: a slot kept as a folder is only in slot_folders. */
	static FO CheckFlatSaveFiles(const FO& Params, const FO& Result)
	{
		const FV Folders = Get(Payload(Result), TEXT("slot_folders"));
		if (!IsList(Folders))
		{
			return nullptr;
		}
		TArray<FString> Names;
		for (const FV& Folder : AsList(Folders))
		{
			const FV Name = IsDict(Folder) ? Get(Folder, TEXT("folder")) : None();
			if (IsStr(Name) && !Strip(AsStr(Name)).IsEmpty())
			{
				Names.Add(AsStr(Name));
			}
		}
		if (Names.Num() == 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("flat_files_only"), Get(Params, TEXT("user_index")), L(Names), TEXT("in-response:slot_folders[].folder"),
			Percent(RB_check_flat_save_files_0, { N(Names.Num()), S(Join(Names, TEXT(", "))) }));
	}

	/** already_exists comes from the asset registry: an orphan package from an earlier attempt looks like a real map. */
	static FO CheckGhostMap(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("already_exists"))) || IsTrue(Get(Data, TEXT("created"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("maybe_ghost_package"), Get(Params, TEXT("name")),
			O(Dict({ { TEXT("map_path"), Get(Data, TEXT("map_path")) }, { TEXT("saved"), Get(Data, TEXT("saved")) } })), TEXT("in-response:already_exists + created"),
			Percent(RB_check_ghost_map_0, { Or(Get(Data, TEXT("map_path")), S(TEXT("map_path not reported"))) }));
	}

	/** Empty cells in a complex row read do not mean "not written" (e.g. enum index 0 exports as empty). */
	static FO CheckComplexRow(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Fields = Get(Data, TEXT("fields"));
		if (!IsDict(Fields) || Len(Fields) == 0)
		{
			return nullptr;
		}
		TArray<FString> EmptyFields;
		for (const auto& Field : Fields->AsObject()->Values)
		{
			if (IsStr(Field.Value))
			{
				const FString Value = Strip(Field.Value->AsString());
				if (Value.IsEmpty() || StrEq(Value, TEXT("()"))) // empty ExportText
				{
					EmptyFields.Add(FString(Field.Key));
				}
			}
		}
		if (EmptyFields.Num() == 0)
		{
			return nullptr;
		}
		const FV RowStruct = Or(Get(Data, TEXT("row_struct")), S(TEXT("<row_struct>")));
		const FO Stored = Dict({
			{ TEXT("empty_fields"), L(EmptyFields) },
			{ TEXT("row_struct"), Get(Data, TEXT("row_struct")) },
			{ TEXT("returned_field_count"), N(Len(Fields)) },
		});
		return WarningFinding(TEXT("empty_is_not_absent"), Get(Params, TEXT("row_name")), O(Stored), TEXT("in-response:fields"),
			Percent(RB_check_complex_row_0, { N(EmptyFields.Num()), S(Join(Head(EmptyFields, 4), TEXT(", "))), RowStruct, RowStruct }));
	}

	/** This is what the subsystem has REGISTERED, not the property: it lags until the actor re-registers. */
	static FO CheckSmartObjectRegistry(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!Has(Data, TEXT("definition")) && !Has(Data, TEXT("registered")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("registry_not_property"), Get(Params, TEXT("actor_label")),
			O(Dict({ { TEXT("definition"), Get(Data, TEXT("definition")) }, { TEXT("registered"), Get(Data, TEXT("registered")) } })),
			TEXT("in-response:definition + registered"), RB_check_smart_object_registry_0);
	}
}

void McpReadbackChecks::RegisterInResponseA(McpReadback::FTables& Tables)
{
	using namespace McpReadbackInResponseA;
	TMap<FString, McpReadback::FInResponse>& T = Tables.InResponse;
	T.Add(TEXT("set_bp_variable_default"), &CheckBpVariableDefault);
	T.Add(TEXT("add_collision_profile"), &CheckCollisionProfile);
	T.Add(TEXT("compile_blueprint"), &CheckCompileBlueprint);
	T.Add(TEXT("set_component_property"), &CheckComponentProperty);
	T.Add(TEXT("add_comment_node"), &CheckCommentNode);
	T.Add(TEXT("find_blueprint_node_references"), &CheckNodeReferences);
	T.Add(TEXT("get_component_properties"), &CheckReadAfterCompile);
	T.Add(TEXT("delete_actor"), &CheckDeleteAfterPieCensus);
	T.Add(TEXT("batch_delete_actors"), &CheckDeleteAfterPieCensus);
	T.Add(TEXT("count_gameplay_tag_references"), &CheckTagRefcount);
	T.Add(TEXT("find_classes_implementing_interface"), &CheckInterfaceSweep);
	T.Add(TEXT("get_default_game_map"), &CheckDefaultMapAggregate);
	T.Add(TEXT("audit_widget_accessibility"), &CheckBlindTouchTarget);
	T.Add(TEXT("list_world_partition_actors"), &CheckZeroWpActors);
	T.Add(TEXT("spawn_blueprint_actor"), &CheckSpawnWithoutAuthority);
	T.Add(TEXT("audit_replication"), &CheckReplicationAudit);
	T.Add(TEXT("list_subsystem_instances"), &CheckSubsystemListEditor);
	T.Add(TEXT("list_save_games"), &CheckFlatSaveFiles);
	T.Add(TEXT("create_world_partition_map"), &CheckGhostMap);
	T.Add(TEXT("read_data_table_row_complex"), &CheckComplexRow);
	T.Add(TEXT("get_smart_object_component_definition"), &CheckSmartObjectRegistry);

	// These four readers share the ambiguous-world check, which needs to know WHICH one called. On
	// raycast_scene it COMPOSES with the physical-material check: the ambiguous-world finding wins
	// and the other only speaks if it found nothing (registering them separately would hide one).
	for (const TCHAR* Reader : { TEXT("raycast_scene"), TEXT("get_actors_by_class"), TEXT("get_actor_properties_by_class"), TEXT("diagnose_navmesh") })
	{
		const FString Name(Reader);
		T.Add(Name, [Name](const McpPy::FO& Params, const McpPy::FO& Result) -> McpPy::FO
		{
			if (const McpPy::FO Ambiguous = CheckAmbiguousWorldRead(Name, Params, Result))
			{
				return Ambiguous;
			}
			return McpPy::StrEq(Name, TEXT("raycast_scene")) ? McpReadbackUtil::CheckPhysicalMaterialWithoutComplex(Params, Result) : nullptr;
		});
	}
}
