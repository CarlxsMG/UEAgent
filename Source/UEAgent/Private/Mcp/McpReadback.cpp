#include "Mcp/McpReadback.h"

#include "McpReadbackChecks.h"
#include "McpReadbackText.h"
#include "Readback/McpReadbackCheckUtil.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpReadbackCorePrivate
{
	using namespace McpPy;
	using namespace McpReadback;
	using namespace McpReadbackText;

	// ------------------------------------------------------------- pre-command snapshots (BEFORE)
	/** The SCS component names of a Blueprint, sorted; unset if they could not be read. Its OWN construction
	 *  script only: no native components, nothing inherited (the parent is read on its own). */
	static TOptional<TArray<FString>> ScsComponentNames(const FSend& Send, const FString& Blueprint)
	{
		const FO Info = ProbeOrEmpty(Send, TEXT("get_blueprint_components"), Dict({
			{ TEXT("blueprint_name"), S(Blueprint) },
			{ TEXT("include_native"), B(false) },
			{ TEXT("include_inherited"), B(false) },
		}));
		const FV Components = Get(Info, TEXT("components"));
		if (!IsList(Components))
		{
			return {};
		}
		TArray<FString> Names;
		for (const FV& Component : AsList(Components))
		{
			if (IsDict(Component) && !TextOf(Get(Component, TEXT("name"))).IsEmpty())
			{
				Names.Add(TextOf(Get(Component, TEXT("name"))));
			}
		}
		return Sorted(Names);
	}

	/**
	 * The parent Blueprint (package path) if it is a /Game asset; cached for the session.
	 * By PATH, not by short name, on purpose: a bare name can silently resolve to ANOTHER asset when a
	 * backup folder keeps copies with the same name.
	 */
	static FString ParentBlueprintAsset(const FSend& Send, const FString& Blueprint, bool& bOutIsNone)
	{
		FSessionState& Session = State();
		for (const TPair<FString, FV>& Entry : Session.ParentCache)
		{
			if (StrEq(Entry.Key, Blueprint))
			{
				bOutIsNone = IsNone(Entry.Value);
				return AsStr(Entry.Value);
			}
		}
		const FO Info = ProbeOrEmpty(Send, TEXT("get_blueprint_parent"), Dict({ { TEXT("blueprint_name"), S(Blueprint) } }));
		FV ParentPath = None();
		if (Truthy(Get(Info, TEXT("has_parent"))) && IsFalse(Get(Info, TEXT("parent_is_native"))))
		{
			const FString ParentClass = TextOf(Get(Info, TEXT("parent_class_path")));
			if (StartsWith(ParentClass, TEXT("/Game/")))
			{
				ParentPath = S(Split(ParentClass, TEXT("."), 1)[0]);
			}
		}
		if (Info->Values.Num() > 0)
		{
			Session.ParentCache.Emplace(Blueprint, ParentPath);
		}
		bOutIsNone = IsNone(ParentPath);
		return AsStr(ParentPath);
	}

	/** Snapshot of the Blueprint's SCS and its parent's, taken before the write. */
	static FV SnapshotScs(const FO& Params, const FSend& Send)
	{
		const FV Bp = Get(Params, TEXT("blueprint_name"));
		if (!IsStr(Bp) || Strip(AsStr(Bp)).IsEmpty())
		{
			return None();
		}
		const FString Blueprint = AsStr(Bp);
		FO Snapshot = MakeShared<FJsonObject>();
		const TOptional<TArray<FString>> OwnNames = ScsComponentNames(Send, Blueprint);
		if (OwnNames.IsSet() && OwnNames->Num() > 0)
		{
			Snapshot->SetField(Blueprint, L(*OwnNames));
		}
		bool bParentIsNone = true;
		const FString Parent = ParentBlueprintAsset(Send, Blueprint, bParentIsNone);
		if (!bParentIsNone && !Parent.IsEmpty())
		{
			const TOptional<TArray<FString>> InheritedNames = ScsComponentNames(Send, Parent);
			if (InheritedNames.IsSet() && InheritedNames->Num() > 0)
			{
				Snapshot->SetField(Parent, L(*InheritedNames));
			}
		}
		return Snapshot->Values.Num() > 0 ? O(Snapshot) : None();
	}

	/** Dirty packages under a read-only (vendored library) root, as a sorted set. */
	static TOptional<TArray<FString>> DirtyLibraryPackages(const FSend& Send)
	{
		const FO DirtyList = ProbeOrEmpty(Send, TEXT("list_dirty_packages"), MakeShared<FJsonObject>());
		if (DirtyList->Values.Num() == 0)
		{
			return {};
		}
		TArray<FString> Paths;
		const FV Packages = Get(DirtyList, TEXT("packages"));
		for (const FV& Package : AsList(Truthy(Packages) ? Packages : L(TArray<FV>())))
		{
			if (IsDict(Package) && IsTrue(Get(Package, TEXT("under_readonly_root"))))
			{
				const FString Path = TextOf(Get(Package, TEXT("package_name")));
				if (!Path.IsEmpty())
				{
					SetAdd(Paths, Path);
				}
			}
		}
		return Sorted(Paths);
	}

	/** Snapshot of the library packages that were ALREADY dirty before adding the component. */
	static FV SnapshotDirtyLibraryPackages(const FO& Params, const FSend& Send)
	{
		const TOptional<TArray<FString>> AlreadyDirty = DirtyLibraryPackages(Send);
		if (!AlreadyDirty.IsSet())
		{
			return None();
		}
		return O(Dict({ { TEXT("readonly"), L(*AlreadyDirty) } }));
	}

	/** The write left the Blueprint's (or its parent's) SCS missing a whole component in memory. */
	static FO CheckDroppedComponents(const FString& Name, const FO& Params, const FV& Before, const FSend& Send)
	{
		if (!IsDict(Before) || Len(Before) == 0)
		{
			return nullptr;
		}
		for (const auto& Field : Before->AsObject()->Values)
		{
			const FString Blueprint(Field.Key);
			const FV PreviousNames = Field.Value;
			if (!Truthy(PreviousNames))
			{
				continue;
			}
			const TOptional<TArray<FString>> Current = ScsComponentNames(Send, Blueprint);
			if (!Current.IsSet())
			{
				continue; // could not re-read: fail OPEN
			}
			TArray<FString> Dropped;
			for (const FV& ComponentName : AsList(PreviousNames))
			{
				if (!InList(*Current, Str(ComponentName)))
				{
					Dropped.Add(Str(ComponentName));
				}
			}
			if (Dropped.Num() == 0)
			{
				continue;
			}
			const FString Package = StartsWith(Blueprint, TEXT("/Game/")) ? Split(Blueprint, TEXT("."), 1)[0] : Blueprint;
			const FString ListText = Join(Dropped, TEXT(", "));
			return Dict({
				{ TEXT("verdict"), S(TEXT("component_dropped")) },
				{ TEXT("requested"), Get(Params, TEXT("property_value")) },
				{ TEXT("stored"), N(Current->Num()) },
				{ TEXT("blueprint"), S(Blueprint) },
				{ TEXT("dropped"), L(Dropped) },
				{ TEXT("before_count"), N(Len(PreviousNames)) },
				{ TEXT("after_count"), N(Current->Num()) },
				{ TEXT("how"), S(TEXT("snapshot:get_blueprint_components")) },
				{ TEXT("error"), S(Name + TEXT(": the write may have landed, but ") + Blueprint + TEXT(" has LOST ")
					+ FString::FromInt(Dropped.Num()) + TEXT(" SCS component(s) IN MEMORY (") + ListText + TEXT("): ")
					+ FString::FromInt(Len(PreviousNames)) + TEXT(" -> ") + FString::FromInt(Current->Num()) + RB_check_dropped_components_0) },
				{ TEXT("note"), S(FString(RB_check_dropped_components_1) + Package + RB_check_dropped_components_2) },
			});
		}
		return nullptr;
	}

	/** The new component was written INTO a vendored library package (the parent's SCS). */
	static FO CheckComponentInLibrary(const FString& Name, const FO& Params, const FV& Before, const FSend& Send)
	{
		if (!IsDict(Before))
		{
			return nullptr;
		}
		const FV PreviousDirty = Get(Before, TEXT("readonly"));
		if (!IsList(PreviousDirty))
		{
			return nullptr; // no usable snapshot
		}
		const TOptional<TArray<FString>> Current = DirtyLibraryPackages(Send);
		if (!Current.IsSet())
		{
			return nullptr;
		}
		TArray<FString> PreviousPaths;
		for (const FV& PreviousPath : AsList(PreviousDirty))
		{
			PreviousPaths.Add(Str(PreviousPath));
		}
		TArray<FString> NewlyDirty;
		for (const FString& Path : *Current)
		{
			if (!InList(PreviousPaths, Path))
			{
				NewlyDirty.Add(Path);
			}
		}
		NewlyDirty = Sorted(NewlyDirty);
		if (NewlyDirty.Num() == 0)
		{
			return nullptr;
		}
		return McpReadbackUtil::WarningFinding(TEXT("wrote_into_library"), Get(Params, TEXT("component_name")), L(NewlyDirty),
			RB_check_component_in_library_0, Percent(RB_check_component_in_library_1, { S(Join(NewlyDirty, TEXT(", "))) }));
	}

	// ------------------------------------------------------------- applying a finding
	static FO ApplyFinding(const FString& Name, const FO& Result, const FO& Finding)
	{
		Result->SetObjectField(TEXT("readback"), Checked(Finding));
		// degrade=False INFORMS without touching `success`.
		if (IsFalse(Get(Finding, TEXT("degrade"))))
		{
			return Result;
		}
		Result->SetBoolField(TEXT("success"), false);
		const FV Error = Get(Finding, TEXT("error"));
		Result->SetField(TEXT("error"), Truthy(Error) ? Error : S(F(RB_apply_finding_0, {
			Name, Repr(Get(Finding, TEXT("requested"))), Repr(Get(Finding, TEXT("stored"))) })));
		return Result;
	}

	// ------------------------------------------------------------- replaying test vectors
	/** Decodes the vector encodings: {"__set__": [...]} -> sorted array. */
	static FV FromRecorded(const FV& Value)
	{
		if (IsDict(Value))
		{
			const FO Object = Value->AsObject();
			const FV SetValues = Object->TryGetField(TEXT("__set__"));
			if (Object->Values.Num() == 1 && IsList(SetValues))
			{
				return Clone(SetValues);
			}
			FO Copy = MakeShared<FJsonObject>();
			for (const auto& Field : Object->Values)
			{
				Copy->SetField(FString(Field.Key), FromRecorded(Field.Value));
			}
			return O(Copy);
		}
		if (IsList(Value))
		{
			TArray<FV> Items;
			for (const FV& Item : AsList(Value))
			{
				Items.Add(FromRecorded(Item));
			}
			return L(Items);
		}
		return Clone(Value);
	}

	static FString FirstDifference(const FString& Want, const FString& Have)
	{
		int32 Index = 0;
		while (Index < Want.Len() && Index < Have.Len() && Want[Index] == Have[Index])
		{
			++Index;
		}
		const int32 From = FMath::Max(0, Index - 80);
		return FString::Printf(TEXT("@%d expected '%s' | got '%s'"), Index, *Want.Mid(From, 240), *Have.Mid(From, 240));
	}
}

namespace McpReadback
{
	using namespace McpPy;
	using namespace McpReadbackCorePrivate;

	FSessionState& State()
	{
		static FSessionState Session;
		return Session;
	}

	bool SetAdd(TArray<FString>& Set, const FString& Value)
	{
		if (InList(Set, Value))
		{
			return false;
		}
		Set.Add(Value);
		return true;
	}

	FO ProbeOrEmpty(const FSend& Send, const FString& Command, const FO& Params)
	{
		if (!Send)
		{
			return MakeShared<FJsonObject>();
		}
		const FO Reply = Send(Command, Params);
		if (!Reply.IsValid() || IsFalse(Get(Reply, TEXT("success"))))
		{
			return MakeShared<FJsonObject>();
		}
		const FO Inner = Payload(Reply);
		return IsFalse(Get(Inner, TEXT("success"))) ? MakeShared<FJsonObject>() : Inner;
	}

	FO Checked(const FO& Finding)
	{
		FO Out = Dict({ { TEXT("checked"), B(true) } });
		if (Finding.IsValid())
		{
			for (const auto& Field : Finding->Values)
			{
				Out->SetField(FString(Field.Key), Field.Value);
			}
		}
		return Out;
	}

	void RegisterCore(FTables& Tables)
	{
		// The single write (property_name + value) is re-read; properties{} alone requests no `value`, so it
		// spends no re-read (the generic follow-up skips a call that requested nothing).
		FFollowUp SetProperties;
		SetProperties.Reader = TEXT("get_properties");
		SetProperties.ParamMap = { { TEXT("object_path"), TEXT("object_path") }, { TEXT("property_name"), TEXT("property_name") } };
		SetProperties.ValueKey = TEXT("values");
		SetProperties.ValueKeyParam = TEXT("property_name");
		SetProperties.RequestKey = TEXT("value");
		Tables.FollowUp.Add(TEXT("set_properties"), SetProperties);

		Tables.Before.Add(TEXT("set_blueprint_component_properties"), &SnapshotScs);
		Tables.Before.Add(TEXT("add_component_to_blueprint"), &SnapshotDirtyLibraryPackages);
		Tables.BeforeCheck.Add(TEXT("set_blueprint_component_properties"), &CheckDroppedComponents);
		Tables.BeforeCheck.Add(TEXT("add_component_to_blueprint"), &CheckComponentInLibrary);
	}

	const FTables& Tables()
	{
		static const FTables Built = []()
		{
			FTables Out;
			RegisterCore(Out);
			McpReadbackChecks::RegisterAll(Out);
			return Out;
		}();
		return Built;
	}

	bool HasContract(const FString& Name)
	{
		const FTables& T = Tables();
		return T.InResponse.Contains(Name) || T.FollowUp.Contains(Name) || T.FollowUpCustom.Contains(Name)
			|| T.OnFailure.Contains(Name) || T.Before.Contains(Name);
	}

	FV TakeBefore(const FString& Name, const FO& Params, const FSend& Send)
	{
		const FBefore* TakeSnapshot = Tables().Before.Find(Name);
		return TakeSnapshot ? (*TakeSnapshot)(Params, Send) : None();
	}

	FO Verify(const FString& Name, const FO& Params, const FO& Result, const FSend& Send, const FV& Before)
	{
		if (!Result.IsValid())
		{
			return Result;
		}
		const FTables& T = Tables();

		// The call already failed: the original error STANDS. Only note why its stated reason misleads.
		if (IsFalse(Get(Result, TEXT("success"))) || IsFalse(Get(Payload(Result), TEXT("success"))))
		{
			if (const FOnFailure* Checker = T.OnFailure.Find(Name))
			{
				if (const FO Reason = (*Checker)(Params, Result, Send))
				{
					Result->SetObjectField(TEXT("readback"), Checked(Reason));
				}
			}
			return Result;
		}

		// The snapshot check runs FIRST and WINS over any other finding.
		if (Truthy(Before))
		{
			if (const FBeforeCheck* Checker = T.BeforeCheck.Find(Name))
			{
				if (const FO BeforeFinding = (*Checker)(Name, Params, Before, Send))
				{
					return ApplyFinding(Name, Result, BeforeFinding);
				}
			}
		}

		FO Finding;
		if (const FInResponse* Checker = T.InResponse.Find(Name))
		{
			Finding = (*Checker)(Params, Result);
		}
		// The families COMPOSE: the next one runs only if the one before it found nothing.
		if (!Finding.IsValid())
		{
			if (const FFollowUpCustom* Custom = T.FollowUpCustom.Find(Name))
			{
				Finding = (*Custom)(Params, Send);
			}
		}
		if (!Finding.IsValid())
		{
			if (const FFollowUp* Follow = T.FollowUp.Find(Name))
			{
				if (IsEmpty(Get(Params, Follow->RequestKey)))
				{
					return Result; // nothing was requested: do not spend a re-read
				}
				FO ReadParams = MakeShared<FJsonObject>();
				for (const TPair<FString, FString>& Map : Follow->ParamMap)
				{
					const FV Source = Get(Params, Map.Value);
					if (!IsNone(Source))
					{
						ReadParams->SetField(Map.Key, Source);
					}
				}
				if (ReadParams->Values.Num() != Follow->ParamMap.Num())
				{
					Result->SetObjectField(TEXT("readback"), Dict({ { TEXT("checked"), B(false) },
						{ TEXT("reason"), S(F(McpReadbackText::RB_verify_readback_0, { Follow->Reader })) } }));
					return Result;
				}
				const FO Echo = Send ? Send(Follow->Reader, ReadParams) : nullptr;
				if (!Echo.IsValid() || IsFalse(Get(Echo, TEXT("success"))) || IsFalse(Get(Payload(Echo), TEXT("success"))))
				{
					const FString Why = Echo.IsValid() ? Str(Get(Echo, TEXT("error"))) : FString(TEXT("None"));
					Result->SetObjectField(TEXT("readback"), Dict({ { TEXT("checked"), B(false) },
						{ TEXT("reason"), S(F(McpReadbackText::RB_verify_readback_1, { Follow->Reader, Why })) } }));
					return Result;
				}
				FV Stored = Get(Payload(Echo), Follow->ValueKey);
				if (!Follow->ValueKeyParam.IsEmpty())
				{
					Stored = Get(Stored, TextOf(Get(Params, Follow->ValueKeyParam)));
				}
				const FString Verdict = McpReadbackUtil::CheckNonEmptyStored(Params, Stored, Follow->RequestKey);
				if (!Verdict.IsEmpty())
				{
					Finding = Dict({
						{ TEXT("verdict"), S(Verdict) },
						{ TEXT("requested"), Get(Params, Follow->RequestKey) },
						{ TEXT("stored"), Stored },
						{ TEXT("how"), S(F(McpReadbackText::RB_verify_readback_2, { Follow->Reader })) },
						{ TEXT("note"), S(McpReadbackText::RB_verify_readback_3) },
					});
				}
			}
		}

		if (!Finding.IsValid())
		{
			Result->SetObjectField(TEXT("readback"), Dict({ { TEXT("checked"), B(true) }, { TEXT("verdict"), S(TEXT("ok")) } }));
			return Result;
		}
		return ApplyFinding(Name, Result, Finding);
	}

	FO AnnotateOffload(const FString& Name, const FO& Result)
	{
		if (!Result.IsValid())
		{
			return Result;
		}
		const FO Data = IsTrue(Get(Result, TEXT("response_offloaded"))) ? Result : Payload(Result);
		if (!IsTrue(Get(Data, TEXT("response_offloaded"))))
		{
			return Result;
		}

		TArray<FString> OmittedFields;
		const FV Offloaded = Get(Data, TEXT("offloaded_fields"));
		for (const FV& Entry : AsList(Truthy(Offloaded) ? Offloaded : L(TArray<FV>())))
		{
			if (IsDict(Entry))
			{
				const FString FieldName = TextOf(Get(Entry, TEXT("field")));
				const FV Length = Get(Entry, TEXT("length"));
				OmittedFields.Add(FieldName.IsEmpty() ? FString() : Percent(TEXT("%s (%s items)"), { S(FieldName), Length }));
			}
			else if (Truthy(Entry))
			{
				OmittedFields.Add(TextOf(Entry));
			}
		}
		OmittedFields.RemoveAll([](const FString& FieldText) { return FieldText.IsEmpty(); });
		const FString ReportPath = TextOf(Get(Data, TEXT("report_path")));

		const FString NextStep = !ReportPath.IsEmpty()
			? FString(McpReadbackText::RB_annotate_offload_0) + ReportPath + McpReadbackText::RB_annotate_offload_1
			: FString(McpReadbackText::RB_annotate_offload_2);
		const FString SilencedNote = Tables().InResponse.Contains(Name) ? FString(McpReadbackText::RB_annotate_offload_3) : FString();

		const FString Note = FString(McpReadbackText::RB_annotate_offload_4) + Str(Get(Data, TEXT("response_chars")))
			+ TEXT(" characters, threshold ") + Str(Get(Data, TEXT("offload_threshold_chars"))) + TEXT(") and the bridge removed the heavy field")
			+ (OmittedFields.Num() > 0 ? TEXT(": ") + Join(OmittedFields, TEXT(", ")) : FString())
			+ McpReadbackText::RB_annotate_offload_5 + NextStep + SilencedNote;

		const FO OffloadFinding = Dict({
			{ TEXT("checked"), B(true) },
			{ TEXT("verdict"), S(TEXT("payload_offloaded")) },
			{ TEXT("degrade"), B(false) },
			{ TEXT("how"), S(TEXT("in-response:response_offloaded")) },
			{ TEXT("omitted"), L(OmittedFields) },
			{ TEXT("note"), S(Note) },
		});

		const FV ExistingReadback = Get(Result, TEXT("readback"));
		if (IsDict(ExistingReadback))
		{
			if (!ExistingReadback->AsObject()->HasField(TEXT("offloaded")))
			{
				ExistingReadback->AsObject()->SetObjectField(TEXT("offloaded"), OffloadFinding); // an earlier finding is not overwritten
			}
		}
		else
		{
			Result->SetObjectField(TEXT("readback"), OffloadFinding);
		}
		return Result;
	}

	void NoteResult(const FString& Name, const FO& Params, const FO& Result)
	{
		FSessionState& Session = State();
		// Compiling does NOT rebuild actors that already exist: remember what was compiled.
		if (Name.Equals(TEXT("compile_blueprint"), ESearchCase::IgnoreCase))
		{
			const FO Data = Payload(Result);
			if (IsFalse(Get(Data, TEXT("succeeded"))))
			{
				return;
			}
			const FV Named = Get(Params, TEXT("blueprint_name"));
			const FString Bp = TextOf(Truthy(Named) ? Named : Get(Params, TEXT("blueprint_path")));
			if (!Bp.IsEmpty())
			{
				SetAdd(Session.CompiledBlueprints, RSplit(Bp, TEXT("/"), 1).Last());
			}
			return;
		}
		// A census taken in a PIE world does not count the MAP: remember which world it was.
		if (Name.Equals(TEXT("get_actors_by_class"), ESearchCase::IgnoreCase) || Name.Equals(TEXT("get_actor_properties_by_class"), ESearchCase::IgnoreCase))
		{
			const FO Data = Payload(Result);
			if (TextOf(Get(Data, TEXT("world_type"))).ToUpper() == TEXT("PIE"))
			{
				const FString Package = TextOf(Get(Data, TEXT("world_package")));
				if (!Package.IsEmpty())
				{
					SetAdd(Session.PieCensus, Package);
				}
			}
		}
	}

	FO Unwrap(const FO& Envelope, bool& bOutRefused)
	{
		bOutRefused = false;
		if (!Envelope.IsValid())
		{
			return Dict({ { TEXT("success"), B(false) }, { TEXT("error"), S(TEXT("Invalid response from the Unreal bridge")) } });
		}
		const FString Status = TextOf(Get(Envelope, TEXT("status")));
		if (Status == TEXT("success"))
		{
			const FV Result = Get(Envelope, TEXT("result"));
			return IsDict(Result) ? Result->AsObject() : MakeShared<FJsonObject>();
		}
		if (Status == TEXT("error"))
		{
			const FV Error = Envelope->TryGetField(TEXT("error"));
			FO Out = Dict({ { TEXT("success"), B(false) }, { TEXT("error"), Error.IsValid() ? Error : S(TEXT("Unknown error")) } });
			const FV Result = Get(Envelope, TEXT("result"));
			if (!IsNone(Result))
			{
				Out->SetField(TEXT("result"), Result);
				bOutRefused = IsTrue(Get(Result, TEXT("refused")));
			}
			return Out;
		}
		return Envelope;
	}

	FO StateToJson()
	{
		const FSessionState& Session = State();
		FO Cache = MakeShared<FJsonObject>();
		for (const TPair<FString, FV>& Entry : Session.ParentCache)
		{
			Cache->SetField(Entry.Key, Entry.Value);
		}
		return Dict({
			{ TEXT("compiled_blueprints"), L(Sorted(Session.CompiledBlueprints)) },
			{ TEXT("pie_census"), L(Sorted(Session.PieCensus)) },
			{ TEXT("probed_terrain"), L(Sorted(Session.ProbedTerrain)) },
			{ TEXT("pie_launched"), L(Sorted(Session.PieLaunched)) },
			{ TEXT("ucs_rerun"), L(Sorted(Session.UcsRerun)) },
			{ TEXT("parent_cache"), O(Cache) },
		});
	}

	void StateFromJson(const FO& Json)
	{
		FSessionState& Session = State();
		auto Strings = [&Json](const TCHAR* Key)
		{
			TArray<FString> Out;
			for (const FV& Item : AsList(Get(Json, Key)))
			{
				Out.Add(Str(Item));
			}
			return Out;
		};
		Session.CompiledBlueprints = Strings(TEXT("compiled_blueprints"));
		Session.PieCensus = Strings(TEXT("pie_census"));
		Session.ProbedTerrain = Strings(TEXT("probed_terrain"));
		Session.PieLaunched = Strings(TEXT("pie_launched"));
		Session.UcsRerun = Strings(TEXT("ucs_rerun"));
		Session.ParentCache.Reset();
		if (const FO Cache = AsDict(Get(Json, TEXT("parent_cache"))))
		{
			for (const auto& Field : Cache->Values)
			{
				Session.ParentCache.Emplace(FString(Field.Key), Field.Value);
			}
		}
	}

	FO TablesToJson()
	{
		const FTables& T = Tables();
		auto Keys = [](const auto& Map)
		{
			TArray<FString> Out;
			for (const auto& Pair : Map)
			{
				Out.Add(Pair.Key);
			}
			return L(Sorted(Out));
		};
		return Dict({
			{ TEXT("IN_RESPONSE"), Keys(T.InResponse) },
			{ TEXT("FOLLOW_UP"), Keys(T.FollowUp) },
			{ TEXT("FOLLOW_UP_CUSTOM"), Keys(T.FollowUpCustom) },
			{ TEXT("ON_FAILURE"), Keys(T.OnFailure) },
			{ TEXT("BEFORE"), Keys(T.Before) },
			{ TEXT("BEFORE_CHECK"), Keys(T.BeforeCheck) },
		});
	}

	/** Why a recorded vector cannot be replayed here ("" if it can): its optional `skip` field. */
	static FString WhyNotApplicable(const FO& Record)
	{
		const FV Skip = Get(Record, TEXT("skip"));
		return IsStr(Skip) ? AsStr(Skip) : FString();
	}

	void RunRecords(const TArray<FV>& Records, int32& OutChecks, int32& OutPassed, TArray<FV>& OutFailures, TArray<FV>& OutSkipped)
	{
		const FSessionState Saved = State();
		int32 RecordIndex = -1;
		for (const FV& RecordValue : Records)
		{
			++RecordIndex;
			const FO Record = AsDict(FromRecorded(RecordValue));
			if (!Record.IsValid())
			{
				continue;
			}
			const FString Op = AsStr(Get(Record, TEXT("op")));
			const FString Command = AsStr(Get(Record, TEXT("command")));
			const FO Params = AsDict(Get(Record, TEXT("params")));
			const FString Skip = WhyNotApplicable(Record);
			if (!Skip.IsEmpty())
			{
				OutSkipped.Add(O(Dict({ { TEXT("test"), Get(Record, TEXT("test")) }, { TEXT("command"), S(Command) }, { TEXT("why"), S(Skip) } })));
				continue;
			}
			if (const FO StateBefore = AsDict(Get(Record, TEXT("state_before"))))
			{
				StateFromJson(StateBefore);
			}

			// FAKE probes: the recorded replies, in order, checking command and params.
			const TArray<FV> Probes = AsList(Get(Record, TEXT("probes")));
			int32 NextProbe = 0;
			FString ProbeError;
			const FSend Fake = [&Probes, &NextProbe, &ProbeError](const FString& ProbeCommand, const FO& ProbeParams) -> FO
			{
				if (!Probes.IsValidIndex(NextProbe))
				{
					ProbeError += FString::Printf(TEXT(" extra probe: %s;"), *ProbeCommand);
					return nullptr;
				}
				const TArray<FV> Probe = AsList(Probes[NextProbe++]);
				const FString WantCommand = Probe.IsValidIndex(0) ? Str(Probe[0]) : FString();
				const FString WantParams = Probe.IsValidIndex(1) ? Canonical(Probe[1]) : FString();
				const FString HaveParams = Canonical(O(ProbeParams));
				if (!StrEq(WantCommand, ProbeCommand) || WantParams != HaveParams)
				{
					ProbeError += FString::Printf(TEXT(" probe %d: expected %s %s, got %s %s;"), NextProbe - 1, *WantCommand, *WantParams, *ProbeCommand, *HaveParams);
				}
				const FV Reply = Probe.IsValidIndex(2) ? Probe[2] : None();
				if (IsDict(Reply) && Reply->AsObject()->HasField(TEXT("__raises__")))
				{
					return nullptr; // the recorded probe threw: no reply
				}
				return IsDict(Reply) ? Clone(Reply->AsObject()) : nullptr;
			};

			FV Got;
			bool bCompare = true;
			if (Op == TEXT("readback"))
			{
				const FV ResultValue = Get(Record, TEXT("result"));
				Got = IsDict(ResultValue) ? O(Verify(Command, Params, Clone(ResultValue->AsObject()), Fake, Get(Record, TEXT("before")))) : ResultValue;
			}
			else if (Op == TEXT("take_before"))
			{
				Got = TakeBefore(Command, Params, Fake);
			}
			else if (Op == TEXT("annotate_offload"))
			{
				const FV ResultValue = Get(Record, TEXT("result"));
				Got = IsDict(ResultValue) ? O(AnnotateOffload(Command, Clone(ResultValue->AsObject()))) : ResultValue;
			}
			else if (Op == TEXT("note_result"))
			{
				const FV ResultValue = Get(Record, TEXT("result"));
				NoteResult(Command, Params, IsDict(ResultValue) ? ResultValue->AsObject() : MakeShared<FJsonObject>());
				bCompare = false;
			}
			else
			{
				continue;
			}

			FString Why;
			if (bCompare)
			{
				const FString Want = Canonical(Get(Record, TEXT("expect")));
				const FString Have = Canonical(Got);
				if (Want != Have)
				{
					Why += TEXT(" output ") + FirstDifference(Want, Have);
				}
			}
			const bool bProbeMismatch = NextProbe != Probes.Num() || !ProbeError.IsEmpty();
			if (NextProbe != Probes.Num())
			{
				Why += FString::Printf(TEXT(" probes: used %d of %d;"), NextProbe, Probes.Num());
			}
			Why += ProbeError;
			bool bStateMismatch = false;
			if (const FO StateAfter = AsDict(Get(Record, TEXT("state_after"))))
			{
				const FString WantState = Canonical(O(StateAfter));
				const FString HaveState = Canonical(O(StateToJson()));
				if (WantState != HaveState)
				{
					bStateMismatch = true;
					Why += TEXT(" state ") + FirstDifference(WantState, HaveState);
				}
			}

			++OutChecks;
			if (Why.IsEmpty())
			{
				++OutPassed;
			}
			else
			{
				// `index` and `have` let a script re-baseline a record whose output changed ONLY in
				// prose (the notes are text, not contract); any other difference stays a failure.
				OutFailures.Add(O(Dict({
					{ TEXT("test"), Get(Record, TEXT("test")) },
					{ TEXT("index"), N(RecordIndex) },
					{ TEXT("op"), S(Op) },
					{ TEXT("command"), S(Command) },
					{ TEXT("why"), S(Why.TrimStartAndEnd()) },
					{ TEXT("probe_mismatch"), B(bProbeMismatch) },
					{ TEXT("state_mismatch"), B(bStateMismatch) },
					{ TEXT("have"), bCompare ? Got : None() },
				})));
			}
		}
		State() = Saved;
	}
}
