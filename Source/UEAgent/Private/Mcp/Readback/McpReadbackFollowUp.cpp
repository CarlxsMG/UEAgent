// FOLLOW_UP_CUSTOM (one or two tailored re-reads) and ON_FAILURE (why the stated reason for a
// failure is not the real one). Every re-read goes through ProbeOrEmpty(): it fails OPEN.

#include "McpReadbackCheckUtil.h"
#include "../McpReadbackChecks.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpReadbackFollowUp
{
	using namespace McpPy;
	using namespace McpReadback;
	using namespace McpReadbackText;
	using namespace McpReadbackUtil;

	// ------------------------------------------------------------------ FOLLOW_UP_CUSTOM
	/** compile_material reports that it recompiled, not whether the material compiles: read the stats. */
	static FO CheckCompileMaterial(const FO& Params, const FSend& Send)
	{
		const FV MaterialPath = Get(Params, TEXT("material_path"));
		if (!Truthy(MaterialPath))
		{
			return nullptr;
		}
		const FO Stats = ProbeOrEmpty(Send, TEXT("get_material_stats"), Dict({ { TEXT("material_path"), MaterialPath } }));
		if (Stats->Values.Num() == 0)
		{
			return nullptr;
		}
		const FV Errors = Get(Stats, TEXT("compile_errors"));
		const FV StatsValid = Get(Stats, TEXT("stats_valid"));
		if (!IsList(Errors) && IsNone(StatsValid))
		{
			return nullptr;
		}
		const bool bHasErrors = IsList(Errors) && Len(Errors) > 0;
		if (!bHasErrors && !IsFalse(StatsValid))
		{
			return nullptr;
		}
		return DegradingFinding(bHasErrors ? TEXT("compile_failed") : TEXT("stats_invalid"), S(Percent(TEXT("compile %s"), { MaterialPath })),
			bHasErrors ? L(Head(AsList(Errors), 3)) : S(Percent(TEXT("stats_valid=%r"), { StatsValid })), TEXT("reread:get_material_stats"), RB_check_compile_material_0);
	}

	/** Connecting to an input pin that is already linked ADDS a second link instead of replacing it. */
	static FO CheckConnectPins(const FO& Params, const FSend& Send)
	{
		const FV Bp = Get(Params, TEXT("blueprint_name"));
		const FV NodeId = Get(Params, TEXT("target_node_id"));
		const FV Pin = Get(Params, TEXT("target_pin"));
		if (!Truthy(Bp) || !Truthy(NodeId) || !Truthy(Pin))
		{
			return nullptr;
		}
		FO Query = Dict({ { TEXT("blueprint_name"), Bp }, { TEXT("node_id"), NodeId } });
		if (Truthy(Get(Params, TEXT("graph_name"))))
		{
			Query->SetField(TEXT("graph_name"), Get(Params, TEXT("graph_name")));
		}
		// get_graph_nodes with node_id answers that one node, at full fidelity, as nodes[0].
		const TArray<FV> Nodes = AsList(Get(ProbeOrEmpty(Send, TEXT("get_graph_nodes"), Query), TEXT("nodes")));
		const FV Pins = Nodes.Num() > 0 ? Get(Nodes[0], TEXT("pins")) : None();
		if (!IsList(Pins) || Len(Pins) == 0)
		{
			return nullptr;
		}
		for (const FV& P : AsList(Pins))
		{
			if (!IsDict(P) || !Equals(Get(P, TEXT("name")), Pin))
			{
				continue;
			}
			if (!IsInputPin(P))
			{
				return nullptr;
			}
			const FV Links = Get(P, TEXT("linked_to"));
			if (IsList(Links) && Len(Links) > 1)
			{
				return DegradingFinding(TEXT("extra_link"), S(Percent(TEXT("1 link on '%s'"), { Pin })), S(Percent(TEXT("%d links"), { N(Len(Links)) })),
					TEXT("reread:get_graph_nodes"), RB_check_connect_pins_0);
			}
			return nullptr;
		}
		return nullptr;
	}

	/** The write did not mark the owning package dirty (no Modify()): the value lives ONLY in memory. */
	static FO CheckDirtyPackage(const FO& Params, const FV& Entry, const FV& Component, const FSend& Send)
	{
		const FString OwnerClass = FirstNonEmptyText(Get(Entry, TEXT("resolved_from_class")), Get(Component, TEXT("resolved_from_class")));
		if (!StartsWith(OwnerClass, TEXT("/Game/")))
		{
			return nullptr;
		}
		const FString Package = Split(OwnerClass, TEXT("."), 1)[0];
		const FO DirtyList = ProbeOrEmpty(Send, TEXT("list_dirty_packages"), Dict({ { TEXT("path_prefix"), S(Package) } }));
		const FV Packages = Get(DirtyList, TEXT("packages"));
		if (DirtyList->Values.Num() == 0 || !IsList(Packages))
		{
			return nullptr;
		}
		for (const FV& P : AsList(Packages))
		{
			if (IsDict(P) && StrEq(Lower(TextOf(Get(P, TEXT("package_name")))), Lower(Package)))
			{
				return nullptr;
			}
		}
		return WarningFinding(TEXT("not_dirty"), Get(Params, TEXT("property_value")), Get(Entry, TEXT("value")), TEXT("reread:list_dirty_packages"),
			TEXT("The package ") + Package + RB_check_dirty_package_0);
	}

	/** The array came back shorter, or with empty slots where a value was requested. */
	static FO CheckIncompleteArray(const FO& Params, const FV& Requested, const FV& Stored)
	{
		if (!IsList(Requested) || !IsList(Stored))
		{
			return nullptr;
		}
		const TArray<FV> RequestedItems = AsList(Requested);
		const TArray<FV> StoredItems = AsList(Stored);
		TArray<FString> Lost;
		if (StoredItems.Num() < RequestedItems.Num())
		{
			Lost.Add(Percent(TEXT("%d of %d items missing"), { N(RequestedItems.Num() - StoredItems.Num()), N(RequestedItems.Num()) }));
		}
		for (int32 Index = 0; Index < RequestedItems.Num(); ++Index)
		{
			if (IsEmpty(RequestedItems[Index]))
			{
				continue;
			}
			if (Index >= StoredItems.Num() || IsEmpty(StoredItems[Index]))
			{
				Lost.Add(Percent(TEXT("position %d was requested with a value and came back empty"), { N(Index) }));
			}
		}
		if (Lost.Num() == 0)
		{
			return nullptr;
		}
		const TArray<FString> FirstFew = Head(Lost, 6);
		return Dict({
			{ TEXT("verdict"), S(TEXT("array_elements_lost")) },
			{ TEXT("requested"), Requested },
			{ TEXT("stored"), Stored },
			{ TEXT("lost"), L(FirstFew) },
			{ TEXT("how"), S(TEXT("reread:get_blueprint_component_overrides")) },
			{ TEXT("note"), S(FString(RB_check_incomplete_array_0) + Join(FirstFew, TEXT("; ")) + RB_check_incomplete_array_1) },
		});
	}

	/** Re-reads the written component property: empty means the write did not land; otherwise checks the array and the
	 *  dirty flag. */
	static FO CheckBlueprintComponentProperty(const FO& Params, const FSend& Send)
	{
		const FV Requested = Get(Params, TEXT("property_value"));
		if (IsEmpty(Requested))
		{
			return nullptr;
		}
		const FV Bp = Get(Params, TEXT("blueprint_name"));
		const FV Comp = Get(Params, TEXT("component_name"));
		const FV Prop = Get(Params, TEXT("property_name"));
		if (!Truthy(Bp) || !Truthy(Comp) || !Truthy(Prop))
		{
			return nullptr;
		}
		const FO Info = ProbeOrEmpty(Send, TEXT("get_blueprint_component_overrides"), Dict({
			{ TEXT("blueprint_name"), Bp },
			{ TEXT("component_name"), Comp },
			{ TEXT("property_name"), Prop },
			{ TEXT("include_identical"), B(true) },
		}));
		const FV Components = Get(Info, TEXT("components"));
		if (!IsList(Components) || Len(Components) == 0)
		{
			return nullptr;
		}
		FV Entry;
		for (const FV& C : AsList(Components))
		{
			if (!IsDict(C))
			{
				continue;
			}
			for (const FV& P : AsList(Get(C, TEXT("properties"))))
			{
				if (IsDict(P) && (Equals(Prop, Get(P, TEXT("property"))) || Equals(Prop, Get(P, TEXT("property_display")))))
				{
					Entry = P;
					break;
				}
			}
			if (Entry.IsValid())
			{
				break;
			}
		}
		if (!Entry.IsValid())
		{
			return nullptr;
		}
		const FV Stored = Get(Entry, TEXT("value"));
		if (!IsEmpty(Stored))
		{
			if (const FO ArrayFinding = CheckIncompleteArray(Params, Requested, Stored))
			{
				return ArrayFinding;
			}
			return CheckDirtyPackage(Params, Entry, AsList(Components)[0], Send);
		}
		return DegradingFinding(TEXT("wrote_nothing"), Requested, Stored, TEXT("reread:get_blueprint_component_overrides"), RB_check_blueprint_component_property_0);
	}

	/** Re-reads is_play_in_editor_active: the state if PIE is still running, otherwise null (also when unknown). */
	static FO LivePieState(const FSend& Send)
	{
		const FO PieState = ProbeOrEmpty(Send, TEXT("is_play_in_editor_active"), MakeShared<FJsonObject>());
		return PieState->Values.Num() > 0 && IsTrue(Get(PieState, TEXT("is_pie_active"))) ? PieState : nullptr;
	}

	/**
	 * Stopping PIE is not instantaneous: PIE can still be running right after the call.
	 * KNOWN LIMIT: this tool can also crash the editor (cause not diagnosed), which no re-read can
	 * detect -- a dead process returns nothing to verify. Whether the editor is alive is answered by
	 * the PROCESS, not by a response.
	 */
	static FO CheckStopPie(const FO& Params, const FSend& Send)
	{
		const FO PieState = LivePieState(Send);
		if (!PieState.IsValid())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("pie_teardown_pending"), S(TEXT("stop")), Get(PieState, TEXT("status")), TEXT("reread:is_play_in_editor_active"), RB_check_stop_pie_0);
	}

	/** `path.rstrip('/').split('/')[-1]` */
	static FString LastPathSegment(const FString& Path)
	{
		FString Trimmed = Path;
		while (Trimmed.EndsWith(TEXT("/")))
		{
			Trimmed.LeftChopInline(1);
		}
		return Split(Trimmed, TEXT("/")).Last();
	}

	/** An edited graph lives only in memory until saved; a package that is NOT dirty after an edit is worse. */
	static FO CheckGraphInMemory(const FO& Params, const FSend& Send)
	{
		const FString Name = TextOf(Get(Params, TEXT("blueprint_name")));
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		const FString ShortName = Lower(Split(LastPathSegment(Name), TEXT("."))[0]);
		if (ShortName.IsEmpty())
		{
			return nullptr;
		}
		const FO DirtyList = ProbeOrEmpty(Send, TEXT("list_dirty_packages"), MakeShared<FJsonObject>());
		if (DirtyList->Values.Num() == 0)
		{
			return nullptr;
		}
		const FV Packages = Get(DirtyList, TEXT("packages"));
		if (!IsList(Packages))
		{
			return nullptr;
		}
		for (const FV& P : AsList(Packages))
		{
			if (!IsDict(P))
			{
				continue;
			}
			const FString Path = TextOf(Get(P, TEXT("package_name")));
			if (!Path.IsEmpty() && StrEq(Lower(LastPathSegment(Path)), ShortName))
			{
				return WarningFinding(TEXT("in_memory_only"), S(Name),
					O(Dict({ { TEXT("package_name"), Get(P, TEXT("package_name")) }, { TEXT("exists_on_disk"), Get(P, TEXT("exists_on_disk")) } })),
					TEXT("reread:list_dirty_packages"), RB_check_graph_in_memory_0);
			}
		}
		return WarningFinding(TEXT("not_dirty"), S(Name), O(Dict({ { TEXT("dirty_count"), Get(DirtyList, TEXT("dirty_count")) } })), TEXT("reread:list_dirty_packages"),
			Percent(RB_check_graph_in_memory_1, { S(Name) }));
	}

	/** A short Blueprint name that matches SEVERAL assets: the read picked one without saying which. */
	static FO CheckAmbiguousName(const FO& Params, const FSend& Send)
	{
		const FString Name = TextOf(Get(Params, TEXT("blueprint_name")));
		if (Name.IsEmpty() || Contains(Name, TEXT("/")))
		{
			return nullptr;
		}
		const FO Resolved = ProbeOrEmpty(Send, TEXT("resolve_asset_by_name"), Dict({ { TEXT("asset_name"), S(Name) } }));
		if (Resolved->Values.Num() == 0 || !IsFalse(Get(Resolved, TEXT("unambiguous"))))
		{
			return nullptr;
		}
		TArray<FString> Paths;
		for (const FV& M : AsList(Get(Resolved, TEXT("matches"))))
		{
			if (IsDict(M))
			{
				const FString Path = FirstNonEmptyText(Get(M, TEXT("path")), Get(M, TEXT("package")));
				if (!Path.IsEmpty())
				{
					Paths.Add(Path);
				}
			}
		}
		const FString ListText = Join(Head(Paths, 4), TEXT(", "));
		return WarningFinding(TEXT("ambiguous_name"), S(Name), L(Paths), TEXT("reread:resolve_asset_by_name"),
			Percent(RB_check_ambiguous_name_0, { S(Name), Get(Resolved, TEXT("match_count")), S(ListText.IsEmpty() ? FString(TEXT("no paths in the response")) : ListText) }));
	}

	/** Whether the requested type is a class: class / softclass / class'...' */
	static bool RequestsAClass(const FV& TypeValue)
	{
		const FString Text = Lower(McpPy::TextOf(TypeValue));
		return StrEq(Text, TEXT("class")) || StrEq(Text, TEXT("softclass")) || StartsWith(Text, TEXT("class'")) || StartsWith(Text, TEXT("softclass'"));
	}

	/** A CLASS type was requested and an object reference was created instead (the response only echoes the request). */
	static FO WarnClassDegraded(const TCHAR* ReaderTool, const FString& Name, const FString& Requested, const FString& Observed)
	{
		return DegradingFindingWithError(TEXT("class_degraded_to_object"), S(Requested), S(Observed), FString(TEXT("reread:")) + ReaderTool,
			Percent(RB_warn_class_degraded_0, { S(Requested), S(Name), S(Observed.IsEmpty() ? FString(TEXT("no category")) : Observed) }), RB_warn_class_degraded_1);
	}

	/** A struct member requested with a class type: check it was not created as an object reference. */
	static FO CheckStructMember(const FO& Params, const FSend& Send)
	{
		if (!RequestsAClass(Get(Params, TEXT("member_type"))))
		{
			return nullptr;
		}
		const FString StructPath = TextOf(Get(Params, TEXT("struct_path")));
		const FString Name = TextOf(Get(Params, TEXT("member_name")));
		if (StructPath.IsEmpty() || Name.IsEmpty())
		{
			return nullptr;
		}
		const FV Members = Get(ProbeOrEmpty(Send, TEXT("get_struct_members"), Dict({ { TEXT("struct_path"), S(StructPath) } })), TEXT("members"));
		if (!IsList(Members) || Len(Members) == 0)
		{
			return nullptr;
		}
		for (const FV& M : AsList(Members))
		{
			if (!IsDict(M))
			{
				continue;
			}
			if (!StrEq(TextOf(Get(M, TEXT("name"))), Name) && !StrEq(TextOf(Get(M, TEXT("var_name"))), Name))
			{
				continue;
			}
			const FString Category = PinCategory(M);
			if (StrEq(Category, TEXT("object")))
			{
				return WarnClassDegraded(TEXT("get_struct_members"), Name, TextOf(Get(Params, TEXT("member_type"))), Category);
			}
			return nullptr;
		}
		return nullptr;
	}

	/** A Blueprint variable requested with a class type: check it was not created as an object reference. */
	static FO CheckClassVariable(const FO& Params, const FSend& Send)
	{
		if (!RequestsAClass(Get(Params, TEXT("variable_type"))))
		{
			return nullptr;
		}
		const FString Bp = TextOf(Get(Params, TEXT("blueprint_name")));
		const FString Name = TextOf(Get(Params, TEXT("variable_name")));
		if (Bp.IsEmpty() || Name.IsEmpty())
		{
			return nullptr;
		}
		const FV Variables = Get(ProbeOrEmpty(Send, TEXT("get_blueprint_variables"), Dict({ { TEXT("blueprint_name"), S(Bp) } })), TEXT("variables"));
		if (!IsList(Variables) || Len(Variables) == 0)
		{
			return nullptr;
		}
		for (const FV& V : AsList(Variables))
		{
			if (!IsDict(V) || !StrEq(TextOf(Get(V, TEXT("name"))), Name))
			{
				continue;
			}
			const FString Category = PinCategory(V);
			if (StrEq(Category, TEXT("object")))
			{
				return WarnClassDegraded(TEXT("get_blueprint_variables"), Name, TextOf(Get(Params, TEXT("variable_type"))), Category);
			}
			return nullptr;
		}
		return nullptr;
	}

	// ------------------------------------------------------------------ ON_FAILURE
	/** The error text: the top-level `error`, otherwise the payload's. */
	static FString ErrorOf(const FO& Result)
	{
		return FirstNonEmptyText(Get(Result, TEXT("error")), Get(Payload(Result), TEXT("error")));
	}

	/** A one-line failure reason: `{verdict, degrade: False, how, note}`. */
	static FO FailureReason(const TCHAR* Verdict, const TCHAR* How, const FString& Note)
	{
		return Dict({
			{ TEXT("verdict"), S(Verdict) },
			{ TEXT("degrade"), B(false) },
			{ TEXT("how"), S(How) },
			{ TEXT("note"), S(Note) },
		});
	}

	/** "owned by another session": PIE started inside a `execute_batch` has an unknown owner, so the bridge refuses to stop it. */
	static FO CheckUnownedPie(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("owned by another session")))
		{
			return nullptr;
		}
		return FailureReason(TEXT("pie_unowned_via_batch"), TEXT("in-response:error"), RB_check_unowned_pie_0);
	}

	/** "not declared on this blueprint": the tool only reaches the Blueprint's OWN variables, not inherited ones. */
	static FO CheckInheritedVariable(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("not declared on this blueprint")))
		{
			return nullptr;
		}
		return FailureReason(TEXT("inherited_variable_out_of_scope"), TEXT("in-response:error"), RB_check_inherited_variable_0);
	}

	/** "not found" while PIE is running is not "does not exist": editor asset loads return null during PIE. */
	static FO CheckWidgetTreeDuringPie(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("not found")) || !LivePieState(Send).IsValid())
		{
			return nullptr;
		}
		return FailureReason(TEXT("false_not_found"), TEXT("reread:is_play_in_editor_active"), RB_check_widget_tree_during_pie_0);
	}

	/** "not found / not yet initialized" for a world-scoped subsystem read on the EDITOR world while PIE runs: the
	 *  instance may live in the PIE world instead. The editor world is what lifetime=world reads without `world`,
	 *  and what any world-scoped lifetime reads with world='editor' (gameinstance and localplayer default to PIE). */
	static FO CheckSubsystemInEditorWorld(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("not yet initialized")))
		{
			return nullptr;
		}
		const FV Lifetime = Get(Params, TEXT("lifetime"));
		const FString Lt = LowerStrip(Lifetime);
		if (!StrEq(Lt, TEXT("world")) && !StrEq(Lt, TEXT("gameinstance")) && !StrEq(Lt, TEXT("localplayer"))) // world-scoped lifetimes
		{
			return nullptr;
		}
		const FString World = LowerStrip(Get(Params, TEXT("world")));
		const bool bReadEditorWorld = StrEq(World, TEXT("editor")) || (World.IsEmpty() && StrEq(Lt, TEXT("world")));
		if (!bReadEditorWorld)
		{
			return nullptr; // it already read a live world: "not found" there means what it says
		}
		if (!LivePieState(Send).IsValid())
		{
			return nullptr;
		}
		return FailureReason(TEXT("editor_world_only"), TEXT("reread:is_play_in_editor_active"), Percent(RB_check_subsystem_in_editor_world_0, { Lifetime }));
	}

	/** "no local PlayerController" while PIE runs: in multi-client PIE the client's controller lives in another world. */
	static FO CheckInputWithoutController(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("no local playercontroller found")) || !LivePieState(Send).IsValid())
		{
			return nullptr;
		}
		const FV PlayerIndex = Get(Params, TEXT("player_index"));
		return FailureReason(TEXT("false_not_found"), TEXT("reread:is_play_in_editor_active"),
			Percent(RB_check_input_without_controller_0, { !IsNone(PlayerIndex) ? PlayerIndex : S(TEXT("0 (default)")) }));
	}

	/** The delete reported failure, but the expression is gone from the material. */
	static FO CheckExpressionDelete(const FO& Params, const FO& Result, const FSend& Send)
	{
		const FString Name = FirstNonEmptyText(Get(Params, TEXT("expression_name")), Get(Params, TEXT("expression")));
		const FString Material = TextOf(Get(Params, TEXT("material_path")));
		if (Name.IsEmpty() || Material.IsEmpty())
		{
			return nullptr;
		}
		const FO Graph = ProbeOrEmpty(Send, TEXT("get_material_graph_info"), Dict({ { TEXT("material_path"), S(Material) } }));
		if (Graph->Values.Num() == 0)
		{
			return nullptr;
		}
		const FV Expressions = Get(Graph, TEXT("nodes"));
		if (!IsList(Expressions))
		{
			return nullptr;
		}
		// Every expression the material owns: the listed nodes, plus the orphans its expression
		// collection does not list.
		TArray<FString> LiveNames;
		for (const FV& Listing : { Expressions, Get(Graph, TEXT("orphan_nodes")) })
		{
			for (const FV& E : AsList(Listing))
			{
				if (IsDict(E))
				{
					SetAdd(LiveNames, TextOf(Get(E, TEXT("name"))));
				}
			}
		}
		if (InList(LiveNames, Name))
		{
			return nullptr;
		}
		return FailureReason(TEXT("failed_but_did_it"), TEXT("reread:get_material_graph_info"),
			Percent(RB_check_expression_delete_0, { S(Name), N(LiveNames.Num()) }));
	}

	/** "not found" for a function: the lookup only searches a few libraries, and some names carry a trailing space. */
	static FO CheckFunctionNotFound(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("not found")))
		{
			return nullptr;
		}
		const FString Name = TextOf(Get(Params, TEXT("function_name")));
		const bool bHasClass = !TextOf(Get(Params, TEXT("target_class"))).IsEmpty();
		TArray<FString> Hints;
		if (!bHasClass)
		{
			Hints.Add(RB_check_function_not_found_0);
		}
		Hints.Add(RB_check_function_not_found_1);
		return FailureReason(TEXT("misleading_function_not_found"), TEXT("in-response:error"),
			Percent(RB_check_function_not_found_2, {
				S(Name.IsEmpty() ? FString() : Percent(TEXT("For %r, "), { S(Name) })),
				S(Hints[0]),
				S(Hints.Num() > 1 ? Hints.Last() : Hints[0]) }));
	}

	/** "unknown output_property": only root pins are valid; displacement is a material attribute, not a root pin. */
	static FO CheckUnknownMaterialOutput(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("unknown output_property")))
		{
			return nullptr;
		}
		const bool bDisplacement = Contains(Lower(TextOf(Get(Params, TEXT("output_property")))), TEXT("displacement"));
		return FailureReason(TEXT("material_output_not_root_pin"), TEXT("in-response:error"),
			bDisplacement ? RB_check_unknown_material_output_0 : RB_check_unknown_material_output_1);
	}

	/** "function graph not found": the tool only reaches function graphs, not custom events. */
	static FO CheckRpcOnCustomEvent(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("function graph not found")))
		{
			return nullptr;
		}
		return FailureReason(TEXT("rpc_not_reaching_custom_event"), TEXT("in-response:error"), RB_check_rpc_on_custom_event_0);
	}

	/** "no asset named": the lookup is anchored to /Game, so plugin mounts are invisible to it. */
	static FO CheckSkeletonOutsideGame(const FO& Params, const FO& Result, const FSend& Send)
	{
		if (!Contains(Lower(ErrorOf(Result)), TEXT("no asset named")))
		{
			return nullptr;
		}
		return FailureReason(TEXT("resolution_anchored_to_game"), TEXT("in-response:error"), RB_check_skeleton_outside_game_0);
	}

	/** "does not exist" can be false while PIE is running. Here the payload error is checked FIRST. */
	static FO CheckMissingAssetDuringPie(const FO& Params, const FO& Result, const FSend& Send)
	{
		const FString Text = FirstNonEmptyText(Get(Payload(Result), TEXT("error")), Get(Result, TEXT("error")));
		if (!Contains(Lower(Text), TEXT("does not exist")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("missing_may_be_pie"), TextField(Params, TEXT("path")), S(Text), TEXT("on_failure:error"), RB_check_missing_asset_during_pie_0);
	}
}

void McpReadbackChecks::RegisterFollowUp(McpReadback::FTables& Tables)
{
	using namespace McpReadbackFollowUp;
	TMap<FString, McpReadback::FFollowUpCustom>& F = Tables.FollowUpCustom;
	F.Add(TEXT("build_blueprint_graph"), &CheckGraphInMemory);
	F.Add(TEXT("delete_node"), &CheckGraphInMemory);
	F.Add(TEXT("get_graph_nodes"), &CheckAmbiguousName);
	F.Add(TEXT("get_blueprint_functions"), &CheckAmbiguousName);
	F.Add(TEXT("set_blueprint_component_properties"), &CheckBlueprintComponentProperty);
	F.Add(TEXT("connect_pins"), &CheckConnectPins);
	F.Add(TEXT("compile_material"), &CheckCompileMaterial);
	F.Add(TEXT("add_struct_member"), &CheckStructMember);
	F.Add(TEXT("add_blueprint_variable"), &CheckClassVariable);
	F.Add(TEXT("stop_play_in_editor"), &CheckStopPie);

	TMap<FString, McpReadback::FOnFailure>& O = Tables.OnFailure;
	O.Add(TEXT("get_widget_blueprint_tree"), &CheckWidgetTreeDuringPie);
	O.Add(TEXT("inspect_subsystem_instance"), &CheckSubsystemInEditorWorld);
	O.Add(TEXT("trigger_input_action"), &CheckInputWithoutController);
	O.Add(TEXT("delete_material_expression"), &CheckExpressionDelete);
	O.Add(TEXT("set_blueprint_variable_default"), &CheckInheritedVariable);
	O.Add(TEXT("stop_play_in_editor"), &CheckUnownedPie);
	O.Add(TEXT("save_asset"), &CheckMissingAssetDuringPie);
	O.Add(TEXT("get_function_pins"), &CheckFunctionNotFound);
	O.Add(TEXT("set_blueprint_function_rpc"), &CheckRpcOnCustomEvent);
	O.Add(TEXT("get_skeleton_info"), &CheckSkeletonOutsideGame);
	O.Add(TEXT("connect_to_material_output"), &CheckUnknownMaterialOutput);
	O.Add(TEXT("disconnect_to_material_output"), &CheckUnknownMaterialOutput);
}
