// IN_RESPONSE (2/3): DataTables, pins and functions, session probes, asset reads, world settings.

#include "McpReadbackCheckUtil.h"
#include "../McpReadbackChecks.h"
#include "Commands/Common/McpCommonUtils.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpReadbackInResponseB
{
	using namespace McpPy;
	using namespace McpReadback;
	using namespace McpReadbackText;
	using namespace McpReadbackUtil;

	/** `p.get('default_value', p.get('default'))` */
	static FV PinDefault(const FV& Pin)
	{
		return Has(AsDict(Pin), TEXT("default_value")) ? Get(Pin, TEXT("default_value")) : Get(Pin, TEXT("default"));
	}

	/** Whether the pin has any links. */
	static bool HasLinks(const FV& Pin)
	{
		const FV Links = Get(Pin, TEXT("linked_to"));
		return IsList(Links) && Len(Links) > 0;
	}

	// ------------------------------------------------------------------ DataTables
	/** The row write reported problems, ignored or unknown columns, or a requested cell that exported empty. */
	static FO CheckTableRow(const FO& Params, const FO& Result)
	{
		const FV RequestedFields = Get(Params, TEXT("fields"));
		if (!IsDict(RequestedFields) || Len(RequestedFields) == 0)
		{
			return nullptr;
		}
		const FO Data = Payload(Result);
		const TArray<FV> Problems = AsList(Get(Data, TEXT("problems")));
		const TArray<FV> IgnoredColumns = AsList(Get(Data, TEXT("unchanged_despite_request")));
		const TArray<FV> UnknownColumns = AsList(Get(Data, TEXT("unknown_columns")));
		TArray<FString> HardProblems;
		if (Problems.Num() > 0)
		{
			HardProblems.Add(TEXT("problems: ") + JoinStr(Head(Problems, 3), TEXT("; ")));
		}
		if (IgnoredColumns.Num() > 0)
		{
			HardProblems.Add(TEXT("unchanged_despite_request: ") + JoinStr(Head(IgnoredColumns, 5), TEXT(", ")));
		}
		if (UnknownColumns.Num() > 0)
		{
			HardProblems.Add(TEXT("unknown_columns: ") + JoinStr(Head(UnknownColumns, 5), TEXT(", ")));
		}
		if (HardProblems.Num() > 0)
		{
			TArray<FString> Columns;
			for (const auto& Field : RequestedFields->AsObject()->Values)
			{
				Columns.Add(FString(Field.Key));
			}
			return DegradingFindingWithError(TEXT("row_write_partial"), L(Sorted(Columns)), Get(Data, TEXT("changed_columns")), RB_check_table_row_0,
				Percent(RB_check_table_row_1, { S(Join(HardProblems, TEXT(" | "))) }), RB_check_table_row_2);
		}
		const FV RowEcho = Get(Data, TEXT("row_after_exporttext"));
		TArray<FString> EmptyCells;
		if (IsDict(RowEcho))
		{
			static const TCHAR* const EmptyCellTexts[] = { TEXT(""), TEXT("()"), TEXT("none"), TEXT("null"),
				TEXT("(tagname=\"\")"), TEXT("(gameplaytags=)"), TEXT("(gameplaytags=())") }; // empty ExportText cells
			for (const auto& Field : RequestedFields->AsObject()->Values)
			{
				if (IsEmpty(Field.Value))
				{
					continue;
				}
				const FV Cell = Get(RowEcho, FString(Field.Key));
				if (!IsStr(Cell))
				{
					continue;
				}
				const FString Text = Lower(Strip(AsStr(Cell)));
				for (const TCHAR* EmptyText : EmptyCellTexts)
				{
					if (StrEq(Text, EmptyText))
					{
						EmptyCells.Add(FString(Field.Key));
						break;
					}
				}
			}
		}
		if (EmptyCells.Num() == 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("cell_empty_after_write"), L(Sorted(EmptyCells)), None(), TEXT("in-response:row_after_exporttext"),
			Percent(RB_check_table_row_3, { S(Join(EmptyCells, TEXT(", "))) }));
	}

	/** The same check with row_json as the fields; a partial write gets the JSON importer's causes instead. */
	static FO CheckTableRowJson(const FO& Params, const FO& Result)
	{
		const FV Cells = Get(Params, TEXT("row_json"));
		if (!IsDict(Cells) || Len(Cells) == 0)
		{
			return nullptr;
		}
		FO WithFields = MakeShared<FJsonObject>(); // a copy of params with fields=row_json
		if (Params.IsValid())
		{
			WithFields->Values = Params->Values;
		}
		WithFields->SetField(TEXT("fields"), Cells);
		const FO Finding = CheckTableRow(WithFields, Result);
		if (!Finding.IsValid() || !StrEq(Str(Get(Finding, TEXT("verdict"))), TEXT("row_write_partial")))
		{
			return Finding;
		}
		const FString Error = AsStr(Get(Finding, TEXT("error")));
		// Keep the table-row error up to its list of causes (this marker must match the text of
		// RB_check_table_row_1) and append the JSON importer's causes.
		Finding->SetStringField(TEXT("error"), Split(Error, TEXT(" Measured causes:"))[0] + RB_check_table_row_json_0);
		return Finding;
	}

	/** add_data_table_row returned `saved:false`: the row lives only in memory. `saved` is the proof, not `success`. */
	static FO CheckRowInMemory(const FO& Params, const FO& Result)
	{
		if (!IsFalse(Get(Payload(Result), TEXT("saved"))))
		{
			return nullptr;
		}
		return DegradingFindingWithError(TEXT("row_only_in_memory"), Get(Params, TEXT("row_name")), B(false), TEXT("in-response:saved"), RB_check_row_in_memory_0,
			TEXT("Read `saved`, not `success`."));
	}

	// ------------------------------------------------------------------ pins and functions
	/** A node disabled by the editor, input pins left bare (no link, no default), or a non-standard exec pin name. */
	static FO CheckPinInfo(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Comment = Get(Data, TEXT("comment"));
		if (IsStr(Comment) && Contains(Lower(AsStr(Comment)), TEXT("is disabled")))
		{
			return WarningFinding(TEXT("node_disabled_by_editor"), Or(Get(Data, TEXT("title")), Get(Data, TEXT("node_id"))),
				S(Strip(AsStr(Comment)).Left(120)), TEXT("in-response:comment"), RB_check_pin_info_0);
		}
		const FV Pins = Get(Data, TEXT("pins"));
		if (!IsList(Pins) || Len(Pins) == 0)
		{
			return nullptr;
		}
		TArray<FString> BarePins;
		TOptional<FString> OddExecPin;
		for (const FV& Pin : AsList(Pins))
		{
			if (!IsDict(Pin) || !IsInputPin(Pin))
			{
				continue;
			}
			const FV Name = Get(Pin, TEXT("name"));
			if (!IsStr(Name) || AsStr(Name).IsEmpty())
			{
				continue;
			}
			const FString NormalizedName = Lower(Strip(AsStr(Name))).Replace(TEXT(" "), TEXT(""), ESearchCase::CaseSensitive);
			if (StrEq(PinCategory(Pin), TEXT("exec")) && !StrEq(NormalizedName, TEXT("execute")))
			{
				OddExecPin = AsStr(Name);
			}
			if (!StartsWith(NormalizedName, TEXT("self")) && !StartsWith(NormalizedName, TEXT("worldcontext"))) // pins that are created bare
			{
				continue;
			}
			if (HasLinks(Pin) || !IsEmpty(PinDefault(Pin)))
			{
				continue;
			}
			BarePins.Add(AsStr(Name));
		}
		if (BarePins.Num() == 0 && !OddExecPin.IsSet())
		{
			return nullptr;
		}
		TArray<FString> Warnings;
		if (BarePins.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_pin_info_1, { S(Join(BarePins, TEXT(", "))) }));
		}
		if (OddExecPin.IsSet())
		{
			Warnings.Add(Percent(RB_check_pin_info_2, { S(*OddExecPin) }));
		}
		return JoinedWarningFinding(BarePins.Num() > 0 ? TEXT("pin_created_bare") : TEXT("nonstandard_exec_pin"),
			BarePins.Num() > 0 ? L(BarePins) : L(TArray<FString>{ *OddExecPin }), None(), TEXT("in-response:pins"), Warnings);
	}

	/** Short lowercase class name: /Script/Engine.Actor -> actor */
	static FString ShortClassName(const FV& Value)
	{
		FString ShortName = IsStr(Value) ? Strip(AsStr(Value)) : FString();
		if (Contains(ShortName, TEXT("/")))
		{
			ShortName = RSplit(ShortName, TEXT("/"), 1).Last();
		}
		if (Contains(ShortName, TEXT(".")))
		{
			ShortName = RSplit(ShortName, TEXT("."), 1).Last();
		}
		return Lower(ShortName);
	}

	/** The signature returned belongs to a different class than the requested target_class. */
	static FO CheckFunctionPins(const FO& Params, const FO& Result)
	{
		const FV RequestedClass = Get(Params, TEXT("target_class"));
		if (!IsStr(RequestedClass) || Strip(AsStr(RequestedClass)).IsEmpty())
		{
			return nullptr;
		}
		const FV OwnerClass = Get(Payload(Result), TEXT("owner_class"));
		if (!IsStr(OwnerClass) || Strip(AsStr(OwnerClass)).IsEmpty() || StrEq(ShortClassName(OwnerClass), ShortClassName(RequestedClass)))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("different_real_function"), RequestedClass, OwnerClass, TEXT("in-response:owner_class"),
			Percent(RB_check_function_pins_0, { RequestedClass, OwnerClass }));
	}

	// ------------------------------------------------------------------ session and batches
	/** The first terrain probe dirties a package; the eighth distinct probe is a grid survey. */
	static FO CheckTerrainProbe(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Height = Get(Data, TEXT("height"));
		if (!IsNum(Height) && !IsBool(Height))
		{
			return nullptr;
		}
		TArray<FString>& ProbedVertices = State().ProbedTerrain;
		const FString Key = Str(Get(Data, TEXT("vertex"))) + TEXT("|") + Str(Get(Data, TEXT("landscape_actor_name")));
		const bool bFirst = ProbedVertices.Num() == 0;
		SetAdd(ProbedVertices, Key);
		if (bFirst)
		{
			return WarningFinding(TEXT("read_dirties_disk"), None(), None(), TEXT("session:first_terrain_probe"), RB_check_terrain_probe_0);
		}
		constexpr int32 GridProbeCount = 8; // distinct probes that make a grid
		if (ProbedVertices.Num() == GridProbeCount)
		{
			return WarningFinding(TEXT("probe_grid"), N(ProbedVertices.Num()), None(),
				FString::Printf(TEXT("session:%d_distinct_probes"), GridProbeCount), Percent(RB_check_terrain_probe_1, { N(ProbedVertices.Num()) }));
		}
		return nullptr;
	}

	/** A non-negative integer, or None. */
	static FV NonNegativeInt(const FV& Value)
	{
		return IsInt(Value) && AsNum(Value) >= 0 ? Value : None();
	}

	/** Redirector OBJECT counts and redirector FILE counts differ: objects are not files. */
	static FO CheckRedirectors(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV ObjF = NonNegativeInt(Get(Data, TEXT("redirectors_found")));
		const FV PkgF = NonNegativeInt(Get(Data, TEXT("redirector_packages_found")));
		const FV ObjD = NonNegativeInt(Get(Data, TEXT("redirectors_deleted")));
		const FV PkgD = NonNegativeInt(Get(Data, TEXT("redirector_packages_deleted")));
		TArray<FString> Parts;
		if (!IsNone(ObjF) && !IsNone(PkgF) && AsNum(ObjF) != AsNum(PkgF))
		{
			Parts.Add(Percent(RB_check_redirectors_0, { ObjF, PkgF }));
		}
		if (!IsNone(ObjD) && !IsNone(PkgD) && AsNum(ObjD) != AsNum(PkgD))
		{
			Parts.Add(Percent(TEXT("DELETED: %d objects in %d file(s)"), { ObjD, PkgD }));
		}
		if (Parts.Num() == 0)
		{
			return nullptr;
		}
		const FV FailedDeletes = Get(Data, TEXT("redirectors_delete_failed"));
		const FString Suffix = IsList(FailedDeletes) && Len(FailedDeletes) > 0 ? Percent(RB_check_redirectors_1, { N(Len(FailedDeletes)) }) : FString();
		return WarningFinding(TEXT("objects_are_not_files"), None(),
			O(Dict({ { TEXT("objects"), L(TArray<FV>{ ObjF, ObjD }) }, { TEXT("files"), L(TArray<FV>{ PkgF, PkgD }) } })),
			TEXT("in-response:redirectors_vs_redirector_packages"), Percent(RB_check_redirectors_2, { S(Join(Parts, TEXT("; "))), S(Suffix) }));
	}

	/** Struct arguments and object paths that do not marshal through call_component_function. */
	static FO CheckBridgeArguments(const FO& Params, const FO& Result)
	{
		const FV Args = Get(Params, TEXT("params"));
		if (!IsDict(Args) || Len(Args) == 0)
		{
			return nullptr;
		}
		TArray<FString> Structs;
		TArray<FString> Paths;
		for (const auto& Field : Args->AsObject()->Values)
		{
			const FString Key(Field.Key);
			if (IsDict(Field.Value))
			{
				// A struct whose only key is TagName is a GameplayTag, and that one does cross.
				const FO Value = Field.Value->AsObject();
				bool bOnlyTag = Value->Values.Num() > 0;
				for (const auto& Sub : Value->Values)
				{
					bOnlyTag &= StrEq(Lower(Strip(FString(Sub.Key))), TEXT("tagname"));
				}
				if (!bOnlyTag)
				{
					Structs.Add(Key);
				}
			}
			else if (IsStr(Field.Value))
			{
				const FString Value = Lower(Strip(Field.Value->AsString()));
				if (StartsWith(Value, TEXT("/game/")) || StartsWith(Value, TEXT("/script/")) || StartsWith(Value, TEXT("/engine/"))) // object paths
				{
					Paths.Add(Key);
				}
			}
		}
		if (Structs.Num() == 0 && Paths.Num() == 0)
		{
			return nullptr;
		}
		TArray<FString> Warnings;
		if (Structs.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_bridge_arguments_0, { S(Join(Sorted(Structs), TEXT(", "))) }));
		}
		if (Paths.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_bridge_arguments_1, { S(Join(Sorted(Paths), TEXT(", "))) }));
		}
		TArray<FString> AllArgs = Structs;
		AllArgs.Append(Paths);
		return JoinedWarningFinding(TEXT("argument_not_marshaled"), L(Sorted(AllArgs)), None(), TEXT("params:types_that_do_not_marshal"), Warnings);
	}

	/** Reloading is not reverting: big batches stall the editor, on_dirty=discard does not revert the disk, OFPA
	 *  packages are skipped. */
	static FO CheckPackageReload(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV RequestedPackages = Get(Data, TEXT("requested"));
		const int32 RequestedCount = IsList(RequestedPackages) ? Len(RequestedPackages) : 0;
		const FV SkippedOfpa = Get(Data, TEXT("skipped_ofpa"));
		const FString OnDirtyMode = LowerStrip(Get(Params, TEXT("on_dirty")));
		TArray<FString> Warnings;
		constexpr int32 BatchReloadThreshold = 10; // packages that make a costly batch
		if (RequestedCount >= BatchReloadThreshold)
		{
			Warnings.Add(Percent(RB_check_package_reload_0, { N(RequestedCount) }));
		}
		if (StrEq(OnDirtyMode, TEXT("discard")))
		{
			Warnings.Add(RB_check_package_reload_1);
		}
		if (IsList(SkippedOfpa) && Len(SkippedOfpa) > 0)
		{
			Warnings.Add(Percent(RB_check_package_reload_2, { N(Len(SkippedOfpa)) }));
		}
		return JoinedWarningFinding(TEXT("reload_is_not_revert"), RequestedCount > 0 ? N(RequestedCount) : None(), None(),
			TEXT("in-response:requested+skipped_ofpa"), Warnings);
	}

	// ------------------------------------------------------------------ asset reads
	/** The internal enumerator names do not match the values: NewEnumerator3 is not necessarily 3. */
	static FO CheckEnumInternalNames(const FO& Params, const FO& Result)
	{
		const FV Values = Get(Payload(Result), TEXT("values"));
		if (!IsList(Values) || Len(Values) == 0)
		{
			return nullptr;
		}
		TArray<FString> Mismatched;
		for (const FV& Value : AsList(Values))
		{
			if (!IsDict(Value))
			{
				continue;
			}
			const FV InternalName = Get(Value, TEXT("internal_name"));
			const FV ActualValue = Get(Value, TEXT("value"));
			if (!IsStr(InternalName) || !IsInt(ActualValue))
			{
				continue;
			}
			const FString Name = AsStr(InternalName);
			int32 DigitStart = Name.Len();
			while (DigitStart > 0 && FChar::IsDigit(Name[DigitStart - 1]))
			{
				--DigitStart;
			}
			const FString DigitSuffix = Name.Mid(DigitStart);
			if (DigitSuffix.IsEmpty())
			{
				continue;
			}
			double SuffixValue = 0;
			for (const TCHAR C : DigitSuffix)
			{
				SuffixValue = SuffixValue * 10 + (C - TEXT('0'));
			}
			if (SuffixValue != AsNum(ActualValue))
			{
				Mismatched.Add(Percent(TEXT("%s is %d (not %s)"), { InternalName, ActualValue, S(DigitSuffix) }));
			}
		}
		if (Mismatched.Num() == 0)
		{
			return nullptr;
		}
		const TArray<FString> FirstFew = Head(Mismatched, 6);
		return WarningFinding(TEXT("internal_name_not_value"), None(), L(FirstFew), TEXT("in-response:values[].internal_name_vs_value"),
			Percent(RB_check_enum_internal_names_0, { S(Join(FirstFew, TEXT("; "))) }));
	}

	/** width/height are the resident mip, not the asset's source size. */
	static FO CheckTextureSize(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV W = Get(Data, TEXT("width"));
		const FV H = Get(Data, TEXT("height"));
		const FV SW = Get(Data, TEXT("source_width"));
		const FV SH = Get(Data, TEXT("source_height"));
		auto IsPositiveInt = [](const FV& Value) { return IsInt(Value) && AsNum(Value) > 0; };
		if (!(IsPositiveInt(W) && IsPositiveInt(H) && IsPositiveInt(SW) && IsPositiveInt(SH)))
		{
			return nullptr;
		}
		if (AsNum(W) == AsNum(SW) && AsNum(H) == AsNum(SH))
		{
			return nullptr;
		}
		const FV MaxTextureSize = Get(Data, TEXT("max_texture_size"));
		const FString Suffix = IsPositiveInt(MaxTextureSize) ? Percent(RB_check_texture_size_0, { MaxTextureSize }) : FString();
		return WarningFinding(TEXT("resident_mip_not_asset"), S(Str(W) + TEXT("x") + Str(H)), S(Str(SW) + TEXT("x") + Str(SH)),
			TEXT("in-response:width_vs_source_width"), Percent(RB_check_texture_size_1, { W, H, SW, SH, S(Suffix) }));
	}

	/** The imported font face exists, but its companion UFont package was not saved. */
	static FO CheckImportedFont(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV ImportedPath = Get(Data, TEXT("imported_object_path"));
		if (!IsStr(ImportedPath) || Strip(AsStr(ImportedPath)).IsEmpty())
		{
			return nullptr;
		}
		const FV SavedFlag = Get(Data, TEXT("font_asset_saved"));
		if (IsTrue(SavedFlag))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("font_not_confirmed_on_disk"), ImportedPath, SavedFlag, TEXT("in-response:font_asset_saved"),
			Percent(RB_check_imported_font_0, { S(IsFalse(SavedFlag) ? TEXT("it is False") : TEXT("it is MISSING")) }));
	}

	/** The component diff compares presence, class and origin -- not property values. */
	static FO CheckComponentDiff(const FO& Params, const FO& Result)
	{
		if (!IsList(Get(Payload(Result), TEXT("changed"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("structural_diff_not_values"), None(), None(), TEXT("in-response:changed"), RB_check_component_diff_0);
	}

	/** Setting a default map rewrites the WHOLE config section, not just the requested key. */
	static FO CheckDefaultMap(const FO& Params, const FO& Result)
	{
		TArray<FString> RequestedKeys;
		for (const TCHAR* Key : { TEXT("editor_startup_map"), TEXT("game_default_map") })
		{
			const FV Value = Get(Params, Key);
			if (IsStr(Value) && !Strip(AsStr(Value)).IsEmpty())
			{
				RequestedKeys.Add(Key);
			}
		}
		if (RequestedKeys.Num() == 0)
		{
			return nullptr;
		}
		return WarningFinding(TEXT("ini_fully_rewritten"), L(RequestedKeys), None(), TEXT("params:config_write"), RB_check_default_map_0);
	}

	/** Lint caveats: an empty result is not a clean graph, and two finding kinds are mostly false positives. */
	static FO CheckGraphLint(const FO& Params, const FO& Result)
	{
		const FV Findings = Get(Payload(Result), TEXT("findings"));
		if (!IsList(Findings))
		{
			return nullptr;
		}
		TArray<FV> Kinds;
		for (const FV& Finding : AsList(Findings))
		{
			if (IsDict(Finding))
			{
				Kinds.Add(Get(Finding, TEXT("kind")));
			}
		}
		auto CountKind = [&Kinds](const TCHAR* Kind)
		{
			int32 Total = 0;
			for (const FV& Value : Kinds)
			{
				Total += IsStr(Value) && StrEq(AsStr(Value), Kind) ? 1 : 0;
			}
			return Total;
		};
		const int32 Shadowing = CountKind(TEXT("custom_event_shadowing_override"));
		const int32 Unreachable = CountKind(TEXT("unreachable_exec_node"));
		TArray<FString> Warnings;
		if (Len(Findings) == 0)
		{
			Warnings.Add(RB_check_graph_lint_0);
		}
		if (Shadowing > 0)
		{
			Warnings.Add(Percent(RB_check_graph_lint_1, { N(Shadowing) }));
		}
		if (Unreachable > 0)
		{
			Warnings.Add(Percent(RB_check_graph_lint_2, { N(Unreachable) }));
		}
		if (Warnings.Num() == 0)
		{
			return nullptr;
		}
		TArray<FString> DistinctKinds;
		for (const FV& Value : Kinds)
		{
			if (IsStr(Value) && !AsStr(Value).IsEmpty())
			{
				SetAdd(DistinctKinds, AsStr(Value));
			}
		}
		return WarningFinding(TEXT("noisy_lint"), N(Len(Findings)), L(Sorted(DistinctKinds)), TEXT("in-response:findings[].kind"),
			Join(Warnings, TEXT(" | ")) + RB_check_graph_lint_3);
	}

	/** The material walker omits some nodes' input names and the constants of unlinked arithmetic inputs. */
	static FO CheckMaterialWalker(const FO& Params, const FO& Result)
	{
		const FV Nodes = Get(Payload(Result), TEXT("nodes"));
		if (!IsList(Nodes) || Len(Nodes) == 0)
		{
			return nullptr;
		}
		auto IsUnnamedInput = [](const FV& Input)
		{
			const FV Value = IsDict(Input) ? Get(Input, TEXT("input_name")) : None();
			return !IsStr(Value) || Strip(AsStr(Value)).IsEmpty() || StrEq(Lower(Strip(AsStr(Value))), TEXT("none"));
		};
		auto NodeName = [](const FV& Node) { return Str(Or(Get(Node, TEXT("name")), Get(Node, TEXT("class")), S(TEXT("?")))); };
		TArray<FString> Warnings;
		TArray<FString> BlindNodes;
		for (const FV& Node : AsList(Nodes))
		{
			if (!IsDict(Node))
			{
				continue;
			}
			const FV Inputs = Get(Node, TEXT("inputs"));
			if (!IsList(Inputs) || Len(Inputs) < 2)
			{
				continue;
			}
			bool bAllUnnamed = true;
			for (const FV& Input : AsList(Inputs))
			{
				bAllUnnamed &= IsUnnamedInput(Input);
			}
			if (bAllUnnamed)
			{
				BlindNodes.Add(Percent(TEXT("%s (%d inputs)"), { S(NodeName(Node)), N(Len(Inputs)) }));
			}
		}
		if (BlindNodes.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_material_walker_0, { S(Join(Head(BlindNodes, 4), TEXT("; "))) }));
		}
		static const TCHAR* const ArithmeticClasses[] = { TEXT("add"), TEXT("subtract"), TEXT("multiply"), TEXT("divide"), TEXT("power"),
			TEXT("min"), TEXT("max"), TEXT("lerp"), TEXT("linearinterpolate"), TEXT("clamp") };
		TArray<FString> UnlinkedInputs;
		for (const FV& Node : AsList(Nodes))
		{
			if (!IsDict(Node))
			{
				continue;
			}
			const FString NodeClass = Lower(TextOf(Get(Node, TEXT("class"))));
			bool bArithmetic = false;
			for (const TCHAR* ArithmeticName : ArithmeticClasses)
			{
				bArithmetic |= Contains(NodeClass, ArithmeticName);
			}
			if (!bArithmetic)
			{
				continue;
			}
			for (const FV& Input : AsList(Get(Node, TEXT("inputs"))))
			{
				if (!IsDict(Input) || !TextOf(Get(Input, TEXT("source"))).IsEmpty())
				{
					continue;
				}
				const FString Pin = TextOf(Get(Input, TEXT("input_name")));
				UnlinkedInputs.Add(NodeName(Node) + TEXT(".") + (Pin.IsEmpty() ? TEXT("input ") + Str(Get(Input, TEXT("index"))) : Pin));
			}
		}
		if (UnlinkedInputs.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_material_walker_1, { S(Join(Head(UnlinkedInputs, 4), TEXT("; "))) }));
		}
		return JoinedWarningFinding(TEXT("walker_omits_names"), L(Head(BlindNodes.Num() > 0 ? BlindNodes : UnlinkedInputs, 4)), None(),
			TEXT("in-response:nodes[].inputs"), Warnings);
	}

	/** The widget preview PNG is reliable for structure, but sRGB is applied twice: colors come out lighter. */
	static FO CheckWidgetPreview(const FO& Params, const FO& Result)
	{
		if (!IsTrue(Get(Payload(Result), TEXT("rendered"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("png_lightens_colors"), Get(Params, TEXT("widget_bp_path")), None(), TEXT("in-response:rendered"),
			RB_check_widget_preview_0);
	}

	/** Always speaks: these setters change the CDO without recompiling the Blueprint. */
	static FO CheckGameModeClass(const FO& Params, const FO& Result)
	{
		return WarningFinding(TEXT("cdo_changed_without_recompile"), None(), None(), TEXT("policy:gamemode"), RB_check_game_mode_class_0);
	}

	// ------------------------------------------------------------------ graph reads
	/** An empty pin default: not a string, or "", "()", "none", "null". */
	static bool IsEmptyPinValue(const FV& Value)
	{
		if (!IsStr(Value))
		{
			return true;
		}
		const FString Text = Lower(Strip(AsStr(Value)));
		return Text.IsEmpty() || StrEq(Text, TEXT("()")) || StrEq(Text, TEXT("none")) || StrEq(Text, TEXT("null"));
	}

	/** An empty or all-zero pin default: (X=0.000000,Y=0.000000,Z=0.000000) is a zero. */
	static bool IsZeroPinValue(const FV& Value)
	{
		if (IsEmptyPinValue(Value))
		{
			return true;
		}
		FString Cleaned = Lower(TextOf(Value));
		for (const TCHAR* Junk : { TEXT("x="), TEXT("y="), TEXT("z="), TEXT("w="), TEXT("r="), TEXT("g="), TEXT("b="), TEXT("a="),
			TEXT("("), TEXT(")"), TEXT(","), TEXT("."), TEXT(" "), TEXT("-") })
		{
			Cleaned.ReplaceInline(Junk, TEXT(""), ESearchCase::CaseSensitive);
		}
		if (Cleaned.IsEmpty())
		{
			return false;
		}
		for (const TCHAR C : Cleaned)
		{
			if (C != TEXT('0'))
			{
				return false;
			}
		}
		return true;
	}

	/** Caveats of a graph read: library graphs get dirtied, compact mode hides pins, disabled nodes, unwired return
	 *  defaults, delays, zero Make pins, AnimGraphs. */
	static FO CheckGraphRead(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Nodes = Get(Data, TEXT("nodes"));
		if (!IsList(Nodes))
		{
			return nullptr;
		}
		TArray<FString> Warnings;
		const FV Bp = Or(Get(Params, TEXT("blueprint_name")), Get(Params, TEXT("blueprint_path")));
		// The project's vendored-library roots (Project Settings > Plugins > UEAgent); the
		// match is case-insensitive.
		if (mcp_common::IsUnderReadOnlyRoot(LowerStrip(Bp)))
		{
			Warnings.Add(RB_check_graph_read_0);
		}
		if (IsTrue(Get(Data, TEXT("compact"))))
		{
			const FV Excluded = Get(Data, TEXT("excluded_count"));
			Warnings.Add(Percent(RB_check_graph_read_1, { S(IsIntOrBool(Excluded) ? Percent(TEXT(" (excluded_count: %d)"), { Excluded }) : FString()) }));
		}
		TArray<FString> Disabled;
		TArray<FV> ResultNodes;
		bool bHasWait = false;
		for (const FV& Node : AsList(Nodes))
		{
			if (!IsDict(Node))
			{
				continue;
			}
			const FV Comment = Get(Node, TEXT("comment"));
			const FString Title = FirstNonEmptyText(Get(Node, TEXT("title")), Get(Node, TEXT("node_id")));
			if (IsStr(Comment) && Contains(Lower(AsStr(Comment)), TEXT("is disabled")))
			{
				Disabled.Add(Title);
			}
			const FString NodeClass = TextOf(Get(Node, TEXT("class")));
			if (Contains(NodeClass, TEXT("FunctionResult")))
			{
				ResultNodes.Add(Node);
			}
			const FString Label = Lower(Title + TEXT(" ") + NodeClass);
			if (Contains(Label, TEXT("delay")) || Contains(Label, TEXT("settimer")) || Contains(Label, TEXT("set timer")))
			{
				bHasWait = true;
			}
		}
		if (Disabled.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_graph_read_2, { N(Disabled.Num()), S(Join(Head(Disabled, 3), TEXT(", "))) }));
		}
		TArray<FString> LyingDefaults;
		for (const FV& Node : ResultNodes)
		{
			for (const FV& Pin : AsList(Get(Node, TEXT("pins"))))
			{
				if (!IsDict(Pin) || !IsInputPin(Pin) || HasLinks(Pin))
				{
					continue;
				}
				const FV Value = PinDefault(Pin);
				if (!IsEmptyPinValue(Value))
				{
					const FString Name = TextOf(Get(Pin, TEXT("name")));
					LyingDefaults.Add((Name.IsEmpty() ? FString(TEXT("?")) : Name) + TEXT("=") + Str(Value));
				}
			}
		}
		if (LyingDefaults.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_graph_read_3, { S(Join(Head(LyingDefaults, 4), TEXT("; "))) }));
		}
		if (bHasWait)
		{
			Warnings.Add(RB_check_graph_read_4);
		}
		TArray<FString> ZeroMakePins;
		for (const FV& Node : AsList(Nodes))
		{
			if (!IsDict(Node) || !Contains(TextOf(Get(Node, TEXT("class"))), TEXT("MakeStruct")))
			{
				continue;
			}
			for (const FV& Pin : AsList(Get(Node, TEXT("pins"))))
			{
				if (!IsDict(Pin) || !IsInputPin(Pin) || HasLinks(Pin))
				{
					continue;
				}
				if (IsZeroPinValue(PinDefault(Pin)))
				{
					const FString Title = TextOf(Get(Node, TEXT("title")));
					const FString Name = TextOf(Get(Pin, TEXT("name")));
					ZeroMakePins.Add((Title.IsEmpty() ? FString(TEXT("Make")) : Title) + TEXT(".") + (Name.IsEmpty() ? FString(TEXT("?")) : Name));
				}
			}
		}
		if (ZeroMakePins.Num() > 0)
		{
			Warnings.Add(Percent(RB_check_graph_read_5, { S(Join(Head(ZeroMakePins, 4), TEXT("; "))) }));
		}
		for (const FV& Node : AsList(Nodes))
		{
			if (IsDict(Node) && Contains(TextOf(Get(Node, TEXT("class"))), TEXT("AnimGraphNode")))
			{
				Warnings.Add(RB_check_graph_read_6);
				break;
			}
		}
		return JoinedWarningFinding(TEXT("graph_read_with_caveats"), Bp, Get(Data, TEXT("node_count")), TEXT("in-response:nodes+compact"), Warnings);
	}

	/** Writing a property on a live PIE actor reruns its construction script. */
	static FO CheckWriteToPieActor(const FO& Params, const FO& Result)
	{
		const FV ObjectPath = Get(Params, TEXT("object_path"));
		if (!IsStr(ObjectPath) || !Contains(Lower(AsStr(ObjectPath)), TEXT("uedpie_")))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("write_to_pie_actor"), Get(Params, TEXT("property_name")), ObjectPath, TEXT("params:object_path"),
			RB_check_write_to_pie_actor_0);
	}

	/** The function list omits collapsed subgraphs unless include_subgraphs is set. */
	static FO CheckBlueprintFunctions(const FO& Params, const FO& Result)
	{
		if (Truthy(Get(Params, TEXT("include_subgraphs"))))
		{
			return nullptr;
		}
		const FO Data = Payload(Result);
		const FV HiddenSubgraphs = Get(Data, TEXT("subgraph_count"));
		if (!IsIntOrBool(HiddenSubgraphs) || AsNum(HiddenSubgraphs) <= 0)
		{
			return nullptr;
		}
		const FV OwnGraphs = Get(Data, TEXT("own_function_graph_count"));
		const FV Total = Get(Data, TEXT("function_count"));
		const FString Suffix = !IsIntOrBool(OwnGraphs) || !IsIntOrBool(Total) ? FString() : Percent(RB_check_blueprint_functions_1, { Total, OwnGraphs });
		return WarningFinding(TEXT("incomplete_function_list"), Get(Params, TEXT("blueprint_name")), HiddenSubgraphs, TEXT("response:subgraph_count"),
			Percent(RB_check_blueprint_functions_0, { HiddenSubgraphs, S(Suffix) }));
	}

	/** The variable list can be incomplete, and its instance_editable / expose_on_spawn flags are unreliable. */
	static FO CheckInstanceEditableVariables(const FO& Params, const FO& Result)
	{
		const FV Variables = Get(Payload(Result), TEXT("variables"));
		if (!IsList(Variables) || Len(Variables) == 0)
		{
			return nullptr;
		}
		TArray<FString> Warnings = { RB_check_instance_editable_variables_0 };
		TArray<FString> ClaimEditable;
		bool bNotExposedOnSpawn = false;
		for (const FV& Variable : AsList(Variables))
		{
			if (!IsDict(Variable))
			{
				continue;
			}
			if (IsTrue(Get(Variable, TEXT("instance_editable"))))
			{
				ClaimEditable.Add(TextOf(Get(Variable, TEXT("name"))));
			}
			bNotExposedOnSpawn |= IsFalse(Get(Variable, TEXT("expose_on_spawn")));
		}
		if (ClaimEditable.Num() > 0)
		{
			TArray<FString> Readable;
			for (const FString& Name : Head(ClaimEditable, 6))
			{
				if (!Name.IsEmpty())
				{
					Readable.Add(Name);
				}
			}
			const FString ListText = Join(Readable, TEXT(", "));
			Warnings.Add(Percent(RB_check_instance_editable_variables_1, { N(ClaimEditable.Num()), S(ListText.IsEmpty() ? FString(TEXT("no readable name")) : ListText) }));
		}
		if (bNotExposedOnSpawn)
		{
			Warnings.Add(RB_check_instance_editable_variables_2);
		}
		return JoinedWarningFinding(TEXT("unreliable_variable_read"), Get(Params, TEXT("blueprint_name")), N(Len(Variables)),
			TEXT("response:variables[]"), Warnings);
	}

	/** Batch nodes failed with 'Function not found': the Blueprint was not compiled after adding the function. */
	static FO CheckBatchWithoutCompile(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Nodes = Get(Data, TEXT("nodes"));
		if (!IsList(Nodes))
		{
			return nullptr;
		}
		TArray<FString> FailingNodes;
		for (const FV& Node : AsList(Nodes))
		{
			if (IsDict(Node) && Contains(TextOf(Get(Node, TEXT("error"))), TEXT("Function not found")))
			{
				FailingNodes.Add(FirstNonEmptyText(Get(Node, TEXT("id")), Get(Node, TEXT("index"))));
			}
		}
		if (FailingNodes.Num() == 0)
		{
			return nullptr;
		}
		const bool bRolledBack = IsTrue(Get(Data, TEXT("rolled_back")));
		const bool bPartial = IsTrue(Get(Data, TEXT("rollback_required"))) && !bRolledBack;
		TArray<FString> Ids;
		for (const FString& Id : Head(FailingNodes, 6))
		{
			if (!Id.IsEmpty())
			{
				Ids.Add(Id);
			}
		}
		const FString ListText = Join(Ids, TEXT(", "));
		return WarningFinding(TEXT("batch_called_uncompiled_function"), Get(Params, TEXT("blueprint_name")), N(FailingNodes.Num()),
			TEXT("response:nodes[].error"),
			Percent(RB_check_batch_without_compile_0, { N(FailingNodes.Num()), S(ListText.IsEmpty() ? FString(TEXT("no id")) : ListText),
				S(bPartial ? RB_check_batch_without_compile_1 : (bRolledBack ? RB_check_batch_without_compile_2 : RB_check_batch_without_compile_3)) }));
	}

	/** Renaming an enum entry regenerates the enum and leaves its Select nodes pointing at the old one. */
	static FO CheckRenamedEnumEntry(const FO& Params, const FO& Result)
	{
		if (!IsTrue(Get(Payload(Result), TEXT("renamed"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("renamed_enum_mutes_selects"), TextField(Params, TEXT("new_name")), TextField(Params, TEXT("enum_path")),
			TEXT("response:renamed"), RB_check_renamed_enum_entry_0);
	}

	/** The delete failed (two causes share one message), or it succeeded while the object is still loaded. */
	static FO CheckAssetDelete(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV DeleteSuccess = Get(Data, TEXT("delete_success"));
		const FString AssetPath = TextOf(Get(Params, TEXT("asset_path")));
		if (IsFalse(DeleteSuccess))
		{
			return WarningFinding(TEXT("delete_failed_two_causes"), S(AssetPath), TextField(Data, TEXT("delete_message")),
				TEXT("response:delete_success"), RB_check_asset_delete_0);
		}
		if (IsTrue(DeleteSuccess) && IsFalse(Get(Data, TEXT("asset_verified_deleted"))))
		{
			return WarningFinding(TEXT("partial_delete_object_still_loaded"), S(AssetPath), Get(Data, TEXT("file_still_exists")),
				TEXT("response:asset_verified_deleted"), RB_check_asset_delete_1);
		}
		return nullptr;
	}

	/** applied:false reads as "no headless way", which only holds for the legacy spatial hash; a runtime hash set
	 *  keeps its numbers elsewhere. */
	static FO CheckWorldPartitionHash(const FO& Params, const FO& Result)
	{
		if (!IsFalse(Get(Payload(Result), TEXT("applied"))))
		{
			return nullptr;
		}
		const FO RequestedGrid = Dict({
			{ TEXT("cell_size"), Get(Params, TEXT("cell_size")) },
			{ TEXT("loading_range"), Get(Params, TEXT("loading_range")) },
			{ TEXT("grid_name"), Get(Params, TEXT("grid_name")) },
		});
		return WarningFinding(TEXT("applied_false_not_impossible"), O(RequestedGrid), TextField(Payload(Result), TEXT("runtime_hash_type")),
			TEXT("response:applied"), RB_check_world_partition_hash_0);
	}

	/** The material instance parameter list is incomplete (static switches, among others, are not listed). */
	static FO CheckMaterialInstanceParams(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsList(Get(Data, TEXT("scalar_params"))) && !IsList(Get(Data, TEXT("vector_params"))) && !IsList(Get(Data, TEXT("texture_params"))))
		{
			return nullptr;
		}
		const FV ScalarCount = Get(Data, TEXT("scalar_count"));
		const FString Suffix = IsIntOrBool(ScalarCount) ? Percent(TEXT(" (it returned %d scalars)"), { ScalarCount }) : FString();
		return WarningFinding(TEXT("parameter_list_incomplete_by_default"), TextField(Params, TEXT("material_instance_path")),
			TextField(Data, TEXT("parent_material")), TEXT("response:scalar_params"), Percent(RB_check_material_instance_params_0, { S(Suffix) }));
	}

	/** pie_active is not a PIE oracle, and dirty library packages may have been dirtied by the engine itself. */
	static FO CheckDirtyPackages(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		TArray<FString> Warnings;
		if (IsFalse(Get(Data, TEXT("pie_active"))))
		{
			Warnings.Add(RB_check_dirty_packages_0);
		}
		const FV ReadonlyDirty = Get(Data, TEXT("readonly_dirty_count"));
		if (IsIntOrBool(ReadonlyDirty) && AsNum(ReadonlyDirty) > 0)
		{
			Warnings.Add(Percent(RB_check_dirty_packages_1, { ReadonlyDirty }));
		}
		return JoinedWarningFinding(TEXT("dirty_packages_fine_print"), Get(Data, TEXT("dirty_count")), ReadonlyDirty,
			TEXT("response:pie_active+readonly_dirty_count"), Warnings);
	}

	/** save_level reported success but the file was not touched (file_mtime_changed:false). */
	static FO CheckLevelSave(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsFalse(Get(Data, TEXT("file_mtime_changed"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("save_level_wrote_nothing"), TextField(Params, TEXT("filename")), Get(Data, TEXT("file_size_bytes")),
			TEXT("response:file_mtime_changed"), RB_check_level_save_0);
	}

	/** A new material comes with a trap: it cannot be used on skeletal meshes until its usage flag is set. */
	static FO CheckNewMaterial(const FO& Params, const FO& Result)
	{
		const FString MaterialPath = TextOf(Get(Payload(Result), TEXT("material_path")));
		if (MaterialPath.IsEmpty())
		{
			return nullptr;
		}
		return WarningFinding(TEXT("new_material_usage_trap"), TextField(Params, TEXT("material_name")), S(MaterialPath),
			TEXT("response:material_path"), RB_check_new_material_0);
	}

	/** Texts the localization gather cannot see, and texts with automatic keys. */
	static FO CheckWidgetTexts(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV NotGathered = Get(Data, TEXT("not_gathered_count"));
		const FV ByIdentity = Get(Data, TEXT("counts_by_identity"));
		const FV NoIdentity = IsDict(ByIdentity) ? Get(ByIdentity, TEXT("none")) : None();
		const FV Auto = IsDict(ByIdentity) ? Get(ByIdentity, TEXT("auto_key")) : None();
		TArray<FString> Warnings;
		if ((IsIntOrBool(NotGathered) && AsNum(NotGathered) > 0) || (IsIntOrBool(NoIdentity) && AsNum(NoIdentity) > 0))
		{
			Warnings.Add(Percent(RB_check_widget_texts_0, { S(IsIntOrBool(NotGathered) ? Percent(TEXT(" (%d)"), { NotGathered }) : FString()) }));
		}
		if (IsIntOrBool(Auto) && AsNum(Auto) > 0)
		{
			Warnings.Add(Percent(RB_check_widget_texts_1, { Auto }));
		}
		return JoinedWarningFinding(TEXT("texts_without_identity"), TextField(Params, TEXT("folder_path")), NotGathered,
			TEXT("response:not_gathered_count+counts_by_identity"), Warnings);
	}

	/** resolved_from_class says where the template lives, not who authored the value; a None value proves nothing. */
	static FO CheckComponentPropertyRead(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const bool bHasValue = Has(Data, TEXT("value"));
		const FString ResolvedFrom = TextOf(Get(Data, TEXT("resolved_from_class")));
		if (!bHasValue && ResolvedFrom.IsEmpty())
		{
			return nullptr;
		}
		TArray<FString> Warnings;
		Warnings.Add(Percent(RB_check_component_property_read_0, { S(ResolvedFrom.IsEmpty() ? FString() : Percent(TEXT(" (it says %r)"), { S(ResolvedFrom) })) }));
		Warnings.Add(RB_check_component_property_read_1);
		const FV Value = Get(Data, TEXT("value"));
		const FString ValueText = Lower(TextOf(Value));
		if (IsNone(Value) || ValueText.IsEmpty() || StrEq(ValueText, TEXT("none")))
		{
			Warnings.Add(RB_check_component_property_read_2);
		}
		return JoinedWarningFinding(TEXT("component_property_three_caveats"), TextField(Params, TEXT("property_name")), S(ResolvedFrom),
			TEXT("response:value+resolved_from_class"), Warnings);
	}

	// ------------------------------------------------------------------ world: RVT, data layers, water
	/** RVT tile_count/tile_size are exponents: texel values were passed and got clamped silently. */
	static FO CheckRvtExponents(const FO& Params, const FO& Result)
	{
		const FV TileCount = Get(Params, TEXT("tile_count"));
		const FV TileSize = Get(Params, TEXT("tile_size"));
		TArray<FString> OutOfRange;
		if (IsNum(TileCount) && AsNum(TileCount) > 12)
		{
			OutOfRange.Add(Percent(TEXT("tile_count=%s (max 12)"), { TileCount }));
		}
		if (IsNum(TileSize) && AsNum(TileSize) > 4)
		{
			OutOfRange.Add(Percent(TEXT("tile_size=%s (max 4)"), { TileSize }));
		}
		if (OutOfRange.Num() == 0)
		{
			return nullptr;
		}
		const FV Size = Get(Payload(Result), TEXT("size"));
		const FString Suffix = IsNone(Size) ? FString() : Percent(TEXT(" (it says %s)"), { Size });
		return WarningFinding(TEXT("rvt_requested_in_texels_not_exponents"), L(OutOfRange), Size, TEXT("params:tile_count+tile_size"),
			Percent(RB_check_rvt_exponents_0, { S(Join(OutOfRange, TEXT("; "))), S(Suffix) }));
	}

	/** The requested data layer color was not applied; the returned color is an echo. */
	static FO CheckDataLayerColor(const FO& Params, const FO& Result)
	{
		const FV Requested = Get(Params, TEXT("debug_color"));
		if (!IsList(Requested) || Len(Requested) == 0)
		{
			return nullptr;
		}
		const FV ReturnedColor = Get(Payload(Result), TEXT("debug_color"));
		if (!IsList(ReturnedColor) || Len(ReturnedColor) == 0)
		{
			return nullptr;
		}
		if (Equals(L(Head(AsList(Requested), 3)), L(Head(AsList(ReturnedColor), 3))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("color_echo_not_applied"), Requested, ReturnedColor, TEXT("params:debug_color vs response"),
			Percent(RB_check_data_layer_color_0, { Requested, ReturnedColor }));
	}

	/** total_steepness > 1: the Gerstner waves self-intersect. */
	static FO CheckGerstnerSteepness(const FO& Params, const FO& Result)
	{
		const FV Total = Get(Payload(Result), TEXT("total_steepness"));
		if (!IsNum(Total) || AsNum(Total) <= 1)
		{
			return nullptr;
		}
		const FV WaveCount = Get(Payload(Result), TEXT("num_waves"));
		const FString Suffix = !IsIntOrBool(WaveCount) || AsNum(WaveCount) <= 0 ? FString()
			: Percent(RB_check_gerstner_steepness_1, { WaveCount, N(1.0 / AsNum(WaveCount)) });
		return WarningFinding(TEXT("self_intersecting_waves"), Get(Params, TEXT("water_body_label")), Total, TEXT("response:total_steepness"),
			Percent(RB_check_gerstner_steepness_0, { Total, S(Suffix) }));
	}

	/** How much ground each water info texel covers (depth color and shore foam come from that texture). */
	static FO CheckWaterInfoDensity(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV Texel = Get(Data, TEXT("water_info_texel_size_uu"));
		if (!IsNum(Texel) || AsNum(Texel) <= 0)
		{
			return nullptr;
		}
		const FString Suffix = !IsFalse(Get(Data, TEXT("render_target_resolution_verified"))) ? FString() : FString(TEXT(" -- AND HERE IT CAME BACK false"));
		return WarningFinding(TEXT("water_info_texture_density"), Get(Params, TEXT("render_target_resolution")), Texel,
			TEXT("response:water_info_texel_size_uu"), Percent(RB_check_water_info_density_0, { N(AsNum(Texel) / 100.0), Texel, S(Suffix) }));
	}

	/** linked_body_count is a runtime registration, not what persists. */
	static FO CheckZoneLinkedBodies(const FO& Params, const FO& Result)
	{
		const FV BodyCount = Get(Payload(Result), TEXT("linked_body_count"));
		if (!IsNum(BodyCount))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("linked_body_count_is_runtime"), Get(Params, TEXT("water_zone_label")), BodyCount,
			TEXT("response:linked_body_count"), Percent(RB_check_zone_linked_bodies_0, { BodyCount }));
	}

	/** set_water_body_material sets only one of the water body's five material slots. */
	static FO CheckWaterMaterial(const FO& Params, const FO& Result)
	{
		if (!IsTrue(Get(Payload(Result), TEXT("set"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("only_one_of_five_slots"), TextField(Params, TEXT("material_path")), TextField(Params, TEXT("water_body_label")),
			TEXT("response:set"), RB_check_water_material_0);
	}

	/** The polygroup does not exist (ids start at 1), or the paint was dropped (no MaterialID attribute). */
	static FO CheckPolygroupPaint(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		TArray<FString> Warnings;
		if (IsFalse(Get(Data, TEXT("polygroup_id_valid"))))
		{
			Warnings.Add(Percent(RB_check_polygroup_paint_0, { Get(Params, TEXT("polygroup_id")) }));
		}
		const FV DistinctIds = Get(Data, TEXT("distinct_material_ids_after"));
		if (IsTrue(Get(Data, TEXT("applied"))) && Equals(DistinctIds, N(1)))
		{
			Warnings.Add(RB_check_polygroup_paint_1);
		}
		return JoinedWarningFinding(TEXT("material_id_paint_no_effect"), Get(Params, TEXT("material_id")), DistinctIds, RB_check_polygroup_paint_2,
			Warnings);
	}

	/** The bake kept the static mesh's existing material slots (replaced_materials:false). */
	static FO CheckBakeWithoutMaterials(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		if (!IsTrue(Get(Data, TEXT("wrote"))) && !IsTrue(Get(Data, TEXT("outcome_success"))))
		{
			return nullptr;
		}
		if (!IsFalse(Get(Data, TEXT("replaced_materials"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("baked_with_single_slot"), TextField(Params, TEXT("target_static_mesh_path")), Get(Data, TEXT("material_count")),
			TEXT("response:replaced_materials"), Percent(RB_check_bake_without_materials_0, { Get(Data, TEXT("material_count")) }));
	}

	/** "slot 0 only" describes this tool, not the engine: multi-material lives in the MaterialIDs. */
	static FO CheckDynamicMeshMaterial(const FO& Params, const FO& Result)
	{
		if (!IsTrue(Get(Payload(Result), TEXT("applied"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("slot_0_not_engine_limit"), TextField(Params, TEXT("material_path")), Get(Payload(Result), TEXT("slot_index")),
			TEXT("response:applied"), RB_check_dynamic_mesh_material_0);
	}

	/** num_pixel_texture_samples is an aggregate over every compiled output, not the per-pixel cost. */
	static FO CheckMaterialStats(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV TextureSamples = Get(Data, TEXT("num_pixel_texture_samples"));
		if (!IsNum(TextureSamples))
		{
			return nullptr;
		}
		const FV Instr = Get(Data, TEXT("num_pixel_shader_instructions"));
		return WarningFinding(TEXT("texture_samples_aggregated"), TextField(Params, TEXT("material_path")), TextureSamples,
			TEXT("response:num_pixel_texture_samples"),
			Percent(RB_check_material_stats_0, { TextureSamples, S(IsNum(Instr) ? Percent(TEXT(" (here: %s)"), { Instr }) : FString()) }));
	}

	/** The landscape Nanite flags contradict each other: they cannot tell "not built" from "built and up to date". */
	static FO CheckLandscapeNanite(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		const FV NaniteBuilt = Get(Data, TEXT("nanite_built"));
		const FV UpToDate = Get(Data, TEXT("nanite_up_to_date"));
		const FV OutOfDate = Get(Data, TEXT("nanite_out_of_date"));
		if (!IsFalse(NaniteBuilt))
		{
			return nullptr;
		}
		if (!IsTrue(UpToDate) && !IsFalse(OutOfDate))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("contradictory_nanite_flags"), TextField(Params, TEXT("landscape_actor_name")),
			O(Dict({ { TEXT("nanite_built"), NaniteBuilt }, { TEXT("nanite_up_to_date"), UpToDate }, { TEXT("nanite_out_of_date"), OutOfDate } })),
			TEXT("response:nanite_built+nanite_up_to_date"), RB_check_landscape_nanite_0);
	}

	/** The HLOD layer's cell_size/loading_range belong to the legacy hash; with a hash set they do not govern the world. */
	static FO CheckHlodGrid(const FO& Params, const FO& Result)
	{
		const FO Data = Payload(Result);
		FO Reported = MakeShared<FJsonObject>();
		for (const TCHAR* Key : { TEXT("cell_size"), TEXT("loading_range") })
		{
			if (Has(Data, Key))
			{
				Reported->SetField(Key, Get(Data, Key));
			}
		}
		if (Reported->Values.Num() == 0)
		{
			return nullptr;
		}
		TArray<FString> ChangedFields;
		for (const TCHAR* Key : { TEXT("changed_cell_size"), TEXT("changed_loading_range") })
		{
			if (IsTrue(Get(Data, Key)))
			{
				ChangedFields.Add(Key);
			}
		}
		const FString Suffix = ChangedFields.Num() == 0 ? FString() : Percent(RB_check_hlod_grid_1, { S(Join(ChangedFields, TEXT(", "))) });
		return WarningFinding(TEXT("hlod_grid_not_governing"), TextField(Params, TEXT("hlod_layer_path")), O(Reported),
			TEXT("response:cell_size+loading_range"), Percent(RB_check_hlod_grid_0, { S(Suffix) }));
	}
}

namespace McpReadbackUtil
{
	/** return_physical_material with a simple trace: per-texel physical materials need trace_complex. raycast_scene
	 *  composes it with the ambiguous-world check. */
	FO CheckPhysicalMaterialWithoutComplex(const FO& Params, const FO& Result)
	{
		using namespace McpPy;
		if (!Truthy(Get(Params, TEXT("return_physical_material"))) || IsTrue(Get(Params, TEXT("trace_complex"))))
		{
			return nullptr;
		}
		return WarningFinding(TEXT("physical_material_needs_complex"), S(TEXT("return_physical_material without trace_complex")), None(),
			TEXT("params:return_physical_material+trace_complex"), McpReadbackText::RB_check_physical_material_without_complex_0);
	}
}

void McpReadbackChecks::RegisterInResponseB(McpReadback::FTables& Tables)
{
	using namespace McpReadbackInResponseB;
	TMap<FString, McpReadback::FInResponse>& T = Tables.InResponse;
	T.Add(TEXT("set_data_table_row_fields"), &CheckTableRow);
	T.Add(TEXT("set_data_table_row_from_json"), &CheckTableRowJson);
	T.Add(TEXT("add_data_table_row"), &CheckRowInMemory);
	T.Add(TEXT("get_node_pin_info"), &CheckPinInfo);
	T.Add(TEXT("get_function_pins"), &CheckFunctionPins);
	T.Add(TEXT("get_landscape_height_at"), &CheckTerrainProbe);
	T.Add(TEXT("fixup_redirectors"), &CheckRedirectors);
	T.Add(TEXT("call_component_function"), &CheckBridgeArguments);
	T.Add(TEXT("reload_asset_package"), &CheckPackageReload);
	T.Add(TEXT("get_enum_info"), &CheckEnumInternalNames);
	T.Add(TEXT("get_texture_info"), &CheckTextureSize);
	T.Add(TEXT("import_font_file"), &CheckImportedFont);
	T.Add(TEXT("diff_blueprint_components"), &CheckComponentDiff);
	T.Add(TEXT("set_default_game_map"), &CheckDefaultMap);
	T.Add(TEXT("lint_blueprint_graph"), &CheckGraphLint);
	T.Add(TEXT("get_material_graph_info"), &CheckMaterialWalker);
	T.Add(TEXT("get_material_function_graph"), &CheckMaterialWalker);
	T.Add(TEXT("render_widget_preview"), &CheckWidgetPreview);
	for (const TCHAR* Command : { TEXT("set_game_mode_default_pawn"), TEXT("set_game_mode_game_state_class"), TEXT("set_game_mode_hud_class"),
		TEXT("set_game_mode_player_state_class"), TEXT("set_player_controller_class") })
	{
		T.Add(Command, &CheckGameModeClass);
	}
	T.Add(TEXT("get_graph_nodes"), &CheckGraphRead);
	T.Add(TEXT("set_property_value"), &CheckWriteToPieActor);
	T.Add(TEXT("get_blueprint_functions"), &CheckBlueprintFunctions);
	T.Add(TEXT("get_blueprint_variables"), &CheckInstanceEditableVariables);
	T.Add(TEXT("build_blueprint_graph"), &CheckBatchWithoutCompile);
	T.Add(TEXT("rename_enum_entry"), &CheckRenamedEnumEntry);
	T.Add(TEXT("delete_asset"), &CheckAssetDelete);
	T.Add(TEXT("set_world_partition_runtime_hash"), &CheckWorldPartitionHash);
	T.Add(TEXT("get_material_instance_params"), &CheckMaterialInstanceParams);
	T.Add(TEXT("list_dirty_packages"), &CheckDirtyPackages);
	T.Add(TEXT("save_level"), &CheckLevelSave);
	T.Add(TEXT("create_material"), &CheckNewMaterial);
	T.Add(TEXT("audit_widget_text"), &CheckWidgetTexts);
	T.Add(TEXT("get_blueprint_component_property"), &CheckComponentPropertyRead);
	T.Add(TEXT("create_rvt_asset"), &CheckRvtExponents);
	T.Add(TEXT("create_data_layer_asset"), &CheckDataLayerColor);
	T.Add(TEXT("set_water_body_gerstner_waves"), &CheckGerstnerSteepness);
	T.Add(TEXT("configure_water_zone"), &CheckWaterInfoDensity);
	T.Add(TEXT("get_water_zone_info"), &CheckZoneLinkedBodies);
	T.Add(TEXT("set_water_body_material"), &CheckWaterMaterial);
	T.Add(TEXT("set_polygroup_material_id"), &CheckPolygroupPaint);
	T.Add(TEXT("copy_dynamic_mesh_to_static_mesh"), &CheckBakeWithoutMaterials);
	T.Add(TEXT("set_dynamic_mesh_material"), &CheckDynamicMeshMaterial);
	T.Add(TEXT("get_material_stats"), &CheckMaterialStats);
	T.Add(TEXT("get_landscape_nanite_status"), &CheckLandscapeNanite);
	T.Add(TEXT("get_hlod_layer_info"), &CheckHlodGrid);
	T.Add(TEXT("set_hlod_layer_settings"), &CheckHlodGrid);
}
