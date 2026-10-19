#include "Mcp/McpPreconditions.h"

#include "Mcp/McpPythonScan.h"
#include "Mcp/McpPy.h"
#include "Mcp/McpReadback.h"
#include "Mcp/McpToolCatalog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/ScopeExit.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpPreconditionsPrivate
{
	using McpPreconditions::FProbe;
	using McpPreconditions::FRefusal;

	// -------------------------------------------------------------------------------------
	// Refusal messages. They are read by an AI agent: what was refused, why it would crash,
	// freeze or mislead, and the way that works instead. Each %s is a value from the call; the
	// argument order is noted above each message. The self-test compares them byte for byte.
	// -------------------------------------------------------------------------------------

	// MsgSetMaterialAttributes  (args: -)
	static const TCHAR* const MsgSetMaterialAttributes =
		TEXT("create_material_expression: creating a `SetMaterialAttributes` node CRASHES THE EDITOR (assertion in Array.h, not an ensure): the node is ")
		TEXT("created with an empty AttributeSetTypes array and GetInputName indexes Inputs[] out of range. Retrying does not help and it cannot be fixed ")
		TEXT("afterwards: writing attribute_set_types through reflection does not grow Inputs either. WHAT WORKS: use the Displacement pin of ")
		TEXT("MakeMaterialAttributes. If that node is really needed, place it once by hand in the Material Editor and wire it headless afterwards.");

	// MsgFoliage  (args: -)
	static const TCHAR* const MsgFoliage =
		TEXT("The foliage tools CRASH THE EDITOR PROCESS (EXCEPTION_ACCESS_VIOLATION in FoliageEdit.dll); splitting the work or lowering the count does ")
		TEXT("not help -- a single instance crashes it too. There is no workaround through these tools. WHAT WORKS: place the instances as an ")
		TEXT("InstancedStaticMeshComponent on a placed actor, with add_instanced_static_mesh_component + add_ism_instance.");

	// MsgLoadLevelTemp  (args: filename)
	static const TCHAR* const MsgLoadLevelTemp =
		TEXT("load_level on '%s': loading a map under /Temp/ or /Engine/ CRASHES THE EDITOR (fatal 'World Memory Leaks' in EditorServer.cpp). The ")
		TEXT("resident-world preflight excludes the CURRENT world by design, but a dirty Untitled map still counts as a leak -- and that is exactly the ")
		TEXT("state the editor reopens in after a crash. WHAT WORKS: ask the user to open that map from the editor. If you know what you are doing, ")
		TEXT("force=true skips this check (and will probably crash the editor).");

	// MsgLoadLevelWp  (args: filename)
	static const TCHAR* const MsgLoadLevelWp =
		TEXT("load_level on '%s': that map was CREATED in this session with create_world_partition_map, and its UWorld stays RESIDENT in memory for the ")
		TEXT("rest of the session (world_context_count goes up and it survives collect_garbage). Loading it now is FATAL -- the same 'World Memory ")
		TEXT("Leaks' -- even when the resident world IS the target and its .umap exists on disk: the leak check counts it anyway. WHAT WORKS: do not ")
		TEXT("open that map in this session. It takes a NEW editor session (a fresh session starts without the resident world), or ask the user to open it.");

	// MsgSaveLevelReuse  (args: filename)
	static const TCHAR* const MsgSaveLevelReuse =
		TEXT("save_level(filename='%s') was ALREADY used in this session, and a second save to the SAME name opens a MODAL DIALOG that freezes the ")
		TEXT("editor for good. The first call does not rename: it DUPLICATES the level and leaves that world resident, so the name stays taken for the ")
		TEXT("rest of the session. WHAT WORKS: use a different name, or call save_level WITHOUT filename to save the level in place (that branch has no ")
		TEXT("such trap).");

	// MsgDeleteWorld  (args: asset_path)
	static const TCHAR* const MsgDeleteWorld =
		TEXT("delete_asset on '%s': that logical path is a MAP (class: World), not an ordinary asset. This call starts deleting the MAP and everything ")
		TEXT("that hangs from it (for a World Partition map, every external actor package that references it). Cancelling the request on the client ")
		TEXT("does NOT stop it: the command keeps running inside the editor, and only killing the process stops it. THE TRAP: if what you meant to ")
		TEXT("delete was the stray `.uasset` the editor can write next to a read-only `.umap` (e.g. a .umap locked by Git LFS), both share the same ")
		TEXT("name and differ only by extension -- which this path does not carry. WHAT WORKS, as an exception to the usual rule: for that stray file, ")
		TEXT("close the editor and delete the file from disk. For the real map there is no way from here: it is not deleted.");

	// MsgDeleteMhUnknown  (args: asset_path, folder, folder)
	static const TCHAR* const MsgDeleteMhUnknown =
		TEXT("delete_asset on the MetaHumanCharacter '%s': could not verify whether baked meshes remain in '%s/Unpacked', and deleting the character ")
		TEXT("while they exist CRASHES THE EDITOR on the next frame. Without that check the call is not sent. WHAT WORKS: delete_directory on ")
		TEXT("'%s/Unpacked' first, then the character.");

	// MsgDeleteMh  (args: asset_path, folder, folder)
	static const TCHAR* const MsgDeleteMh =
		TEXT("delete_asset on the MetaHumanCharacter '%s': its baked meshes are still in '%s/Unpacked', and deleting the character first CRASHES THE ")
		TEXT("EDITOR on the next frame (EXCEPTION_ACCESS_VIOLATION; the log reports 'SkeletalMesh ... has no skeleton' milliseconds after the delete ")
		TEXT("returns True). That True is honest about the deletion and silent about what is left dangling. WHAT WORKS: delete the baked meshes FIRST ")
		TEXT("-- delete_directory on '%s/Unpacked' -- and the character AFTERWARDS. NOTE where they are: the build does NOT put them under ")
		TEXT("/Game/MetaHumans/ but under the character's own folder, and they keep the CHARACTER's name.");

	// MsgSparseDelegate  (args: delegate_name, class or object_path)
	static const TCHAR* const MsgSparseDelegate =
		TEXT("subscribe_to_delegate('%s' on %s): could NOT confirm that this delegate is declared by a Blueprint, and without that confirmation the ")
		TEXT("call is not sent. The engine's SPARSE delegates are not a Function but a SparseDelegateFunction; the tool duplicates them as if they were ")
		TEXT("a Function and the editor CRASHES IMMEDIATELY (assertion in GetPropertiesSize, UObjectGlobals.cpp). This guard fails CLOSED on purpose: ")
		TEXT("a fail-open version crashed the editor. WHAT WORKS: subscribe to a dispatcher declared in Blueprint, e.g. on a Blueprint COMPONENT that ")
		TEXT("fires right before the effect you want to observe; `get_event_dispatchers` on the Blueprint ASSET (without the _C suffix) lists the ones ")
		TEXT("that are safe. WARNING: do not chain two subscribe_to_delegate calls in the same batch: if the second one crashes the editor, the first ")
		TEXT("subscription is lost with the process.");

	// MsgPieRunning  (args: -)
	static const TCHAR* const MsgPieRunning =
		TEXT("start_play_in_editor: a PIE session is ALREADY running (is_pie_active: true). Starting now leaves PIE HALF-STARTED, and that does NOT ")
		TEXT("fail: it corrupts the data. Observed: the spawned PlayerController was the engine default instead of the project's class and the pawn ")
		TEXT("ended up without a controller; spawned content appeared at wrong transforms (kilometres away or far below the map), which is easy to ")
		TEXT("misreport as a game bug. WHAT WORKS: stop_play_in_editor, CONFIRM that is_play_in_editor_active returns false (the pie_stopped flag of ")
		TEXT("the stop call is hardcoded to true and proves nothing), and only then start.");

	// MsgCaptureUi  (args: -)
	static const TCHAR* const MsgCaptureUi =
		TEXT("capture_pie_screenshot with include_ui CRASHES THE EDITOR (access violation in D3D12/Slate). And without include_ui it does not serve ")
		TEXT("the usual purpose either: it does NOT render UMG -- the UI is not in the image. To inspect the UI, use read_pie_widgets; for an image of ")
		TEXT("the viewport, capture_viewport; to judge real gameplay, ask the user to play-test it in PIE.");

	// MsgDeleteThenRename  (args: -)
	static const TCHAR* const MsgDeleteThenRename =
		TEXT("execute_python: this script calls both delete_subobject AND rename_subobject. That sequence CRASHES THE EDITOR (fatal in Obj.cpp: ")
		TEXT("'Renaming on top of an existing object is not allowed') and cannot return an error. The trap is not renaming onto a taken name: the name ")
		TEXT("is FREE in the Blueprint but still TAKEN in the instance ALREADY PLACED in the level, because delete_subobject removes the component from ")
		TEXT("the SCS but does not reach what that instance serialized. RULE: do not rename an SCS component -- if the name matters, create it with the ")
		TEXT("right name, and if it already exists under another name, adapt whatever reads it. WHAT WORKS if it must be done anyway: delete -> ")
		TEXT("compile_blueprint -> save_level -> re-read the instances with list_world_partition_actors -> rename, in SEPARATE CALLS, never both steps ")
		TEXT("in the same script. WARNING: if the name ends in a digit you did not ask for (MyComponent1), Unreal made it unique because a namesake ")
		TEXT("ALREADY exists: find out what it is instead of renaming over it.");

	// MsgAttributeSetTypes  (args: sites)
	static const TCHAR* const MsgAttributeSetTypes =
		TEXT("execute_python: this script WRITES attribute_set_types (%s), and that CRASHES THE EDITOR inside the call: assertion in Array.h, 'Array ")
		TEXT("index out of bounds: 1 into an array of size 1'. UMaterialExpressionSetMaterialAttributes keeps TWO parallel arrays (Inputs and ")
		TEXT("AttributeSetTypes); writing AttributeSetTypes through reflection does NOT grow Inputs, and PostEditChangeProperty indexes Inputs[1]. ")
		TEXT("There is no return, so there is no success:false to rely on: the process goes down with every unsaved material edit. The two arrays are ")
		TEXT("only kept in sync by the node's '+' button in the Material Editor (manual work for the user). WHAT WORKS: use the Displacement pin of ")
		TEXT("MakeMaterialAttributes, reachable by name -- there is no need to touch this node. READING the property IS allowed (get_editor_property ")
		TEXT("returns [] without error); only writing it crashes.");

	// MsgMergeMeshes  (args: -)
	static const TCHAR* const MsgMergeMeshes =
		TEXT("execute_python: SkeletalMergingLibrary.merge_meshes CRASHES THE EDITOR with an assert, not an exception (Assertion failed: (Index >= 0) ")
		TEXT("& (Index < ArrayNum), 'Array index out of bounds: 1 into an array of size 1', not an ensure). It indexes the parallel arrays ")
		TEXT("mesh_section_mappings and uv_transforms_per_mesh by NUMBER OF MESHES without validating them, so with 2 meshes and those arrays empty it ")
		TEXT("crashes and takes everything unsaved with it. With a SINGLE mesh it is harmless: the crash starts at TWO, which is exactly the useful ")
		TEXT("case. IF YOU STILL NEED IT: fill EVERY parallel array to the size of meshes_to_merge before calling it. NOTE: re-skinning has no ")
		TEXT("headless path in UE 5.8: Skeleton is read-only through reflection, no MCP tool assigns a skeleton, SkeletalMeshEditorSubsystem only ")
		TEXT("exposes assign_physics_asset, and IKRetargetBatchOperation is for ANIMATIONS. The step that produces the mesh is done in the editor ")
		TEXT("(right-click the SkeletalMesh -> Skeleton -> Assign Skeleton).");

	// MsgPngExporter  (args: -)
	static const TCHAR* const MsgPngExporter =
		TEXT("execute_python: TextureExporterPNG CRASHES THE EDITOR if the texture is not 8 bits per channel (Assertion failed: ")
		TEXT("SupportsTexture(Texture), EditorFactories.cpp, not an ensure). With a texture from an RTF_RGBA16F render target, or TC_HDR, or ")
		TEXT("TC_EDITOR_ICON over float data, there is no return value: there is an assert. And the rule is not 'check the bit depth', it is DO NOT ")
		TEXT("EXPORT AN IMAGE TO LOOK AT IT: to verify a capture, COUNT PIXELS with unreal.RenderingLibrary.read_render_target_raw / ")
		TEXT("read_render_target_uv. IF YOU REALLY NEED A FILE: export_render_target is the supported path, or create the texture from an RTF_RGBA8 ")
		TEXT("render target. NOTE: this guard only sees code sent through execute_python; the same call inside execute_python_file does NOT go through here.");

	// MsgAutorig  (args: -)
	static const TCHAR* const MsgAutorig =
		TEXT("execute_python: request_auto_rigging CRASHES THE EDITOR IMMEDIATELY (Assertion failed: IsValid(), SharedPointer.h, not an ensure; script ")
		TEXT("stack MetaHumanCharacterEditorSubsystem.RequestAutoRigging), and it is not a local failure: METAHUMAN AUTO-RIGGING IS A CLOUD SERVICE. ")
		TEXT("The observed chain: no Epic session -> EOS_InvalidAuth -> no autorig -> 'Character is not rigged' in the UI -> can_build_meta_human:false ")
		TEXT("-> Assemble disabled. rig_type=JOINTS_ONLY makes no difference. It is not blocked because it is impossible, but because what decides it ")
		TEXT("is the ACCOUNT STATE, which cannot be read from the code: if you believe there is an Epic session, check it FIRST with ")
		TEXT("get_output_log_lines(category_filter='MetaHuman') WITHOUT a verbosity filter -- LogMetaHumanAuth logs at Display, not Error, so with a ")
		TEXT("filter it does NOT show. And save EVERYTHING before any attempt: this is an assert, not an exception.");

	// MsgPositionalRotator  (args: tool, calls)
	static const TCHAR* const MsgPositionalRotator =
		TEXT("%s: unreal.Rotator() called with POSITIONAL arguments -- its order is (roll, pitch, yaw), NOT the (pitch, yaw, roll) it prints in, so ")
		TEXT("Rotator(0, 90, 0) is PITCH 90 and points the actor at the sky. Name the axes: unreal.Rotator(roll=0.0, pitch=0.0, yaw=90.0). Offending ")
		TEXT("call(s): %s");

	// MsgDoubleSlashPath  (args: argument, value)
	static const TCHAR* const MsgDoubleSlashPath =
		TEXT("%s is '%s', a content path with '//' in it: the engine aborts the editor on a package name like that ")
		TEXT("(CreatePackage is Fatal on it). Write the path with single slashes.");

	// -------------------------------------------------------------------------------------
	// Session state. It belongs to the EDITOR process: it starts empty with each editor, which is
	// exactly what "in this session" means for a resident world or a used save name.
	// -------------------------------------------------------------------------------------
	static TSet<FString> GWpMapsCreated; // World Partition maps created this session (normalized, lowercase)
	static TSet<FString> GSaveAsUsed;    // save_level filenames already used this session

	// Delegates AActor declares SPARSE: the family that crashes subscribe_to_delegate.
	static const TCHAR* const ActorSparseDelegates[] = {
		TEXT("OnTakeAnyDamage"), TEXT("OnTakePointDamage"), TEXT("OnTakeRadialDamage"),
		TEXT("OnActorBeginOverlap"), TEXT("OnActorEndOverlap"), TEXT("OnActorHit"),
		TEXT("OnDestroyed"), TEXT("OnEndPlay"),
		TEXT("OnBeginCursorOver"), TEXT("OnEndCursorOver"), TEXT("OnClicked"), TEXT("OnReleased"),
		TEXT("OnInputTouchBegin"), TEXT("OnInputTouchEnd"), TEXT("OnInputTouchEnter"), TEXT("OnInputTouchLeave") };

	/** Replaces each %s with the next argument and %% with %. */
	static FString Fill(const TCHAR* Template, const TArray<FString>& Args)
	{
		FString Out;
		int32 ArgIndex = 0;
		for (const TCHAR* P = Template; *P; ++P)
		{
			if (P[0] == TEXT('%') && P[1] == TEXT('s'))
			{
				Out += Args.IsValidIndex(ArgIndex) ? Args[ArgIndex] : FString();
				++ArgIndex;
				++P;
			}
			else if (P[0] == TEXT('%') && P[1] == TEXT('%'))
			{
				Out.AppendChar(TEXT('%'));
				++P;
			}
			else
			{
				Out.AppendChar(*P);
			}
		}
		return Out;
	}

	static TOptional<FRefusal> Refuse(const FString& Message, const TCHAR* Check)
	{
		FRefusal Refusal;
		Refusal.Message = Message;
		Refusal.Check = Check;
		return Refusal;
	}

	/** The trimmed string field, or empty if it is not a string. */
	static FString TextOf(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		if (Object.IsValid())
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Key);
			if (Value.IsValid() && Value->Type == EJson::String)
			{
				return Value->AsString().TrimStartAndEnd();
			}
		}
		return FString();
	}

	/** A bool, the string "true"/"1"/"yes", or Python truthiness for anything else. */
	static bool Truthy(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		const TSharedPtr<FJsonValue> Value = Object.IsValid() ? Object->TryGetField(Key) : nullptr;
		if (!Value.IsValid())
		{
			return false;
		}
		switch (Value->Type)
		{
		case EJson::Boolean: return Value->AsBool();
		case EJson::String:
		{
			const FString Text = Value->AsString().TrimStartAndEnd().ToLower();
			return Text == TEXT("true") || Text == TEXT("1") || Text == TEXT("yes");
		}
		case EJson::Number: return Value->AsNumber() != 0.0;
		case EJson::Array:  return Value->AsArray().Num() > 0;
		case EJson::Object: return Value->AsObject().IsValid() && Value->AsObject()->Values.Num() > 0;
		default:            return false;
		}
	}

	/** Forward slashes, no trailing slash, lowercase. */
	static FString NormPkg(const FString& Path)
	{
		FString Result = Path.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
		while (Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1);
		}
		return Result.ToLower();
	}

	/** An integral number field (`isinstance(x, int)`). */
	static TOptional<int64> IntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		if (Object.IsValid())
		{
			const TSharedPtr<FJsonValue> Value = Object->TryGetField(Key);
			if (Value.IsValid() && Value->Type == EJson::Number)
			{
				const double Number = Value->AsNumber();
				if (FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number), 0.0))
				{
					return static_cast<int64>(Number);
				}
			}
		}
		return TOptional<int64>();
	}

	/** Runs a probe: its payload, or null for "could not tell". */
	static TSharedPtr<FJsonObject> Ask(const FProbe& Probe, const TCHAR* Command, const TSharedPtr<FJsonObject>& Params)
	{
		return Probe ? Probe(Command, Params) : nullptr;
	}

	static void SortCaseSensitive(TArray<FString>& Values)
	{
		Values.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
	}

	static TArray<FString> UniqueSorted(const TArray<FString>& Values)
	{
		TArray<FString> Unique;
		for (const FString& Value : Values)
		{
			if (!McpPythonScan::ContainsExact(Unique, Value))
			{
				Unique.Add(Value);
			}
		}
		SortCaseSensitive(Unique);
		return Unique;
	}

	// ======================================================================== (a) ARGUMENTS

	/**
	 * A content path with "//" in it: what CreatePackage, and every LoadObject that has to create
	 * the package it resolves, abort the editor on. Its first segment is a root name ("/Game/",
	 * "/MyPlugin/"); a path that exists on disk is a file-system path and passes.
	 */
	static bool IsDoubleSlashContentPath(const FString& Value)
	{
		if (!Value.StartsWith(TEXT("/")) || !Value.Contains(TEXT("//")))
		{
			return false;
		}
		int32 Index = 0;
		while (Index < Value.Len() && Value[Index] == TEXT('/'))
		{
			++Index;
		}
		const int32 RootStart = Index;
		while (Index < Value.Len() && (FChar::IsAlnum(Value[Index]) || Value[Index] == TEXT('_') || Value[Index] == TEXT('-')))
		{
			++Index;
		}
		if (Index == RootStart || Index == Value.Len() || Value[Index] != TEXT('/'))
		{
			return false;
		}
		return !IFileManager::Get().FileExists(*Value) && !IFileManager::Get().DirectoryExists(*Value);
	}

	/** The first string, at any depth, that IsDoubleSlashContentPath, and where it sits ("asset_path", "commands[0].params.path"). */
	static bool FindDoubleSlashPath(const TSharedPtr<FJsonValue>& Value, const FString& Where, int32 Depth, FString& OutWhere, FString& OutValue)
	{
		if (!Value.IsValid() || Depth > 16)
		{
			return false;
		}
		if (Value->Type == EJson::String)
		{
			if (IsDoubleSlashContentPath(Value->AsString()))
			{
				OutWhere = Where;
				OutValue = Value->AsString();
				return true;
			}
			return false;
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				if (FindDoubleSlashPath(Items[Index], FString::Printf(TEXT("%s[%d]"), *Where, Index), Depth + 1, OutWhere, OutValue))
				{
					return true;
				}
			}
			return false;
		}
		if (Value->Type == EJson::Object && Value->AsObject().IsValid())
		{
			for (const auto& Field : Value->AsObject()->Values)
			{
				const FString Key(Field.Key);
				if (FindDoubleSlashPath(Field.Value, Where.IsEmpty() ? Key : Where + TEXT(".") + Key, Depth + 1, OutWhere, OutValue))
				{
					return true;
				}
			}
		}
		return false;
	}

	/** Any argument that is a content path with "//": every command, before anything else. */
	static TOptional<FRefusal> RefuseDoubleSlashPath(const TSharedPtr<FJsonObject>& Params)
	{
		FString Where, Value;
		if (!Params.IsValid() || !FindDoubleSlashPath(MakeShared<FJsonValueObject>(Params), FString(), 0, Where, Value))
		{
			return {};
		}
		return Refuse(Fill(MsgDoubleSlashPath, { Where, Value }), TEXT("double_slash_path"));
	}

	/** A number outside the integer range its parameter is read into: the engine's read fails and
	 *  the handler would go on with its default (0, -1, the first slot) as if the call had asked for it. */
	static TOptional<FRefusal> RefuseIntegerOutOfRange(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
	{
		const FString Message = FMcpToolCatalog::CheckIntegerRanges(CommandType, Params);
		if (Message.IsEmpty())
		{
			return {};
		}
		return Refuse(Message, TEXT("integer_range"));
	}

	/** A name or path argument (`nameLike` in the catalog) of NAME_SIZE characters or more: the FName the tool
	 *  builds from it is a checkf in UnrealNames.cpp, which aborts the editor. */
	static TOptional<FRefusal> RefuseNameTooLong(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
	{
		const FString Message = FMcpToolCatalog::CheckNameLengths(CommandType, Params);
		if (Message.IsEmpty())
		{
			return {};
		}
		return Refuse(Message, TEXT("name_length"));
	}

	/** A batch item whose command_type has NAME_SIZE characters or more: the dispatch makes an FName of each
	 *  item's command type (its PIE-safety and parameter tables are keyed by FName) before that item's own
	 *  preconditions run, so the batch is refused before any item runs. */
	static TOptional<FRefusal> RefuseBatchCommandTypeTooLong(const TSharedPtr<FJsonObject>& Params)
	{
		const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
		if (!Params.IsValid() || !Params->TryGetArrayField(TEXT("commands"), Commands) || !Commands)
		{
			return {};
		}
		for (int32 Index = 0; Index < Commands->Num(); ++Index)
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			FString SubCommand;
			if ((*Commands)[Index].IsValid() && (*Commands)[Index]->TryGetObject(Item) && Item
				&& (*Item)->TryGetStringField(TEXT("command_type"), SubCommand) && SubCommand.Len() >= NAME_SIZE)
			{
				return Refuse(FString::Printf(
					TEXT("batch: commands[%d].command_type is %d characters long. An FName holds at most %d characters, and the engine ")
					TEXT("aborts the editor when it builds one from a longer string (UnrealNames.cpp, NAME_SIZE); every command type ")
					TEXT("becomes one before its item runs, and no tool has a name that long. Refused before any item ran: nothing was ")
					TEXT("changed. WHAT WORKS: the name of a tool, as tools/list gives it."),
					Index, SubCommand.Len(), NAME_SIZE - 1), TEXT("name_length"));
			}
		}
		return {};
	}

	/** Creating a SetMaterialAttributes node crashes the editor. */
	static TOptional<FRefusal> RefuseSetMaterialAttributes(const TSharedPtr<FJsonObject>& Params)
	{
		if (!TextOf(Params, TEXT("expression_class_name")).ToLower().Contains(TEXT("setmaterialattributes")))
		{
			return {};
		}
		return Refuse(MsgSetMaterialAttributes, TEXT("set_material_attributes"));
	}

	/** Foliage tools: access violation in FoliageEdit.dll, even with a single instance. */
	static TOptional<FRefusal> RefuseFoliage(const TSharedPtr<FJsonObject>&)
	{
		return Refuse(MsgFoliage, TEXT("foliage"));
	}

	/** 'World Memory Leaks': a /Temp/ or /Engine/ map, or a World Partition map created in this session. */
	static TOptional<FRefusal> RefuseLoadLevel(const TSharedPtr<FJsonObject>& Params)
	{
		if (Truthy(Params, TEXT("force")))
		{
			return {}; // the tool's own escape hatch, documented in its description
		}
		const FString Target = TextOf(Params, TEXT("filename")).Replace(TEXT("\\"), TEXT("/"));
		const FString Low = Target.ToLower();
		if (Low.StartsWith(TEXT("/temp/")) || Low.StartsWith(TEXT("/engine/")))
		{
			return Refuse(Fill(MsgLoadLevelTemp, { Target }), TEXT("load_level_temp"));
		}
		if (GWpMapsCreated.Contains(NormPkg(Target)))
		{
			return Refuse(Fill(MsgLoadLevelWp, { Target }), TEXT("load_level_wp_resident"));
		}
		return {};
	}

	/** save_level(filename=X) works ONCE per name and session: the second call opens a modal dialog. */
	static TOptional<FRefusal> RefuseSaveLevelReuse(const TSharedPtr<FJsonObject>& Params)
	{
		const FString Target = TextOf(Params, TEXT("filename"));
		if (Target.IsEmpty())
		{
			return {}; // no filename means "save in place": the branch that works
		}
		const FString Key = NormPkg(Target);
		if (!GSaveAsUsed.Contains(Key))
		{
			GSaveAsUsed.Add(Key);
			return {};
		}
		return Refuse(Fill(MsgSaveLevelReuse, { Target }), TEXT("save_level_reuse"));
	}

	/** Deleting a MAP path, or a MetaHumanCharacter whose baked meshes still exist. */
	static TOptional<FRefusal> RefuseDeleteAsset(const TSharedPtr<FJsonObject>& Params, const FProbe& Probe)
	{
		const FString Asset = TextOf(Params, TEXT("asset_path"));
		if (Asset.IsEmpty())
		{
			return {};
		}
		// NOTE: get_asset_metadata does not take a path: the handler prepends "/Game/" to whatever it gets.
		const FString ProbeName = Asset.ToLower().StartsWith(TEXT("/game/")) ? Asset.Mid(6) : Asset;
		TSharedPtr<FJsonObject> MetaParams = MakeShared<FJsonObject>();
		MetaParams->SetStringField(TEXT("asset_name"), ProbeName);
		const FString Class = TextOf(Ask(Probe, TEXT("get_asset_metadata"), MetaParams), TEXT("class")).ToLower();

		// The map first, the worst case: deleting the .umap's path deletes the MAP.
		if (Class == TEXT("world"))
		{
			return Refuse(Fill(MsgDeleteWorld, { Asset }), TEXT("delete_world"));
		}
		if (!Class.Contains(TEXT("metahumancharacter")))
		{
			return {}; // outside the crashing family, fail OPEN
		}

		int32 Slash = INDEX_NONE;
		const FString Folder = Asset.FindLastChar(TEXT('/'), Slash) ? Asset.Left(Slash) : Asset;
		TSharedPtr<FJsonObject> FolderParams = MakeShared<FJsonObject>();
		FolderParams->SetStringField(TEXT("folder_path"), Folder + TEXT("/Unpacked"));
		FolderParams->SetBoolField(TEXT("recursive"), true);
		FolderParams->SetNumberField(TEXT("max_results"), 1);
		const TSharedPtr<FJsonObject> Baked = Ask(Probe, TEXT("get_assets_in_folder"), FolderParams);
		TOptional<int64> Total = IntField(Baked, TEXT("total_found"));
		if (!Total.IsSet())
		{
			Total = IntField(Baked, TEXT("count"));
		}
		if (Total.IsSet() && Total.GetValue() <= 0)
		{
			return {}; // the meshes are gone: deleting the character now is the right order
		}
		if (!Total.IsSet())
		{
			// INSIDE the crashing family, "could not read" cannot mean "go ahead".
			return Refuse(Fill(MsgDeleteMhUnknown, { Asset, Folder, Folder }), TEXT("delete_metahuman_unverified"));
		}
		return Refuse(Fill(MsgDeleteMh, { Asset, Folder, Folder }), TEXT("delete_metahuman_baked"));
	}

	/** Subscribing to an engine SPARSE delegate crashes the editor. Fails CLOSED: only a Blueprint that
	 *  declares the delegate ITSELF clears the call (include_inherited=false; an inherited one does not). */
	static TOptional<FRefusal> RefuseSparseDelegate(const TSharedPtr<FJsonObject>& Params, const FProbe& Probe)
	{
		const FString Delegate = TextOf(Params, TEXT("delegate_name"));
		const FString Object = TextOf(Params, TEXT("object_path"));
		if (Delegate.IsEmpty() || Object.IsEmpty())
		{
			return {};
		}
		bool bFatalFamily = false;
		for (const TCHAR* Sparse : ActorSparseDelegates)
		{
			bFatalFamily |= Delegate.Equals(Sparse, ESearchCase::CaseSensitive);
		}
		if (!bFatalFamily)
		{
			return {}; // not in the crashing family: allowed, at no read cost
		}

		TSharedPtr<FJsonObject> ClassParams = MakeShared<FJsonObject>();
		ClassParams->SetStringField(TEXT("object_path"), Object);
		const TSharedPtr<FJsonObject> Info = Ask(Probe, TEXT("get_object_class"), ClassParams);
		const FString ClassPath = TextOf(Info, TEXT("class_path"));
		const FString ClassName = TextOf(Info, TEXT("class_name"));
		// get_event_dispatchers wants the ASSET, not the generated class:
		//   /Game/X/BP_Y.BP_Y_C -> /Game/X/BP_Y     |     BP_Y_C -> BP_Y
		FString Blueprint;
		if (ClassPath.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
		{
			int32 Dot = INDEX_NONE;
			Blueprint = ClassPath.FindChar(TEXT('.'), Dot) ? ClassPath.Left(Dot) : ClassPath;
		}
		else if (ClassName.EndsWith(TEXT("_C"), ESearchCase::CaseSensitive))
		{
			Blueprint = ClassName.LeftChop(2);
		}

		bool bDeclared = false;
		if (!Blueprint.IsEmpty())
		{
			// include_inherited=false: with true the list includes the SPARSE delegates the class INHERITS
			// from the native AActor, and "confirming" against it would clear exactly the fatal call.
			TSharedPtr<FJsonObject> DispatcherParams = MakeShared<FJsonObject>();
			DispatcherParams->SetStringField(TEXT("blueprint_name"), Blueprint);
			DispatcherParams->SetBoolField(TEXT("include_inherited"), false);
			const TSharedPtr<FJsonObject> Dispatchers = Ask(Probe, TEXT("get_event_dispatchers"), DispatcherParams);
			const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
			if (Dispatchers.IsValid() && Dispatchers->TryGetArrayField(TEXT("dispatchers"), List) && List)
			{
				for (const TSharedPtr<FJsonValue>& Entry : *List)
				{
					const TSharedPtr<FJsonObject>* EntryObject = nullptr;
					if (Entry.IsValid() && Entry->TryGetObject(EntryObject) && EntryObject
						&& TextOf(*EntryObject, TEXT("name")).Equals(Delegate, ESearchCase::CaseSensitive)
						&& !Truthy(*EntryObject, TEXT("is_inherited")))
					{
						bDeclared = true; // belt and braces: an inherited one does NOT clear it
					}
				}
			}
		}
		if (bDeclared)
		{
			return {}; // the Blueprint declares it: the SAFE case
		}
		return Refuse(Fill(MsgSparseDelegate, { Delegate, ClassName.IsEmpty() ? Object : ClassName }), TEXT("sparse_delegate"));
	}

	/** The code sent to `execute_python`: the positional Rotator check, then the five crash checks
	 *  in order. */
	static TOptional<FRefusal> RefusePythonCode(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
	{
		const TSharedPtr<FJsonValue> CodeValue = Params.IsValid() ? Params->TryGetField(TEXT("command")) : nullptr;
		if (!CodeValue.IsValid() || CodeValue->Type != EJson::String)
		{
			return {};
		}
		const FString Code = CodeValue->AsString();

		// Rotator(roll, pitch, yaw) versus the (pitch, yaw, roll) it prints in.
		const TArray<FString> Offenders = McpPythonScan::PositionalRotatorCalls(Code);
		if (Offenders.Num() > 0)
		{
			TArray<FString> Shown = UniqueSorted(Offenders);
			if (Shown.Num() > 3)
			{
				Shown.SetNum(3);
			}
			return Refuse(Fill(MsgPositionalRotator, { CommandType, FString::Join(Shown, TEXT(", ")) }), TEXT("positional_rotator"));
		}

		if (Code.TrimStartAndEnd().IsEmpty())
		{
			return {};
		}
		const TArray<FString> Calls = McpPythonScan::CalledNames(Code);
		// Deleting a subobject and renaming another in the SAME script crashes the editor: the name
		// was free in the Blueprint and TAKEN in the placed instance. The SEQUENCE is refused, not the
		// tool: `rename_subobject` on its own is the normal way to set up a component in a child
		// Blueprint, and refusing it outright would block normal work.
		if (McpPythonScan::ContainsExact(Calls, TEXT("delete_subobject")) && McpPythonScan::ContainsExact(Calls, TEXT("rename_subobject")))
		{
			return Refuse(MsgDeleteThenRename, TEXT("delete_then_rename"));
		}
		const TArray<FString> Sites = McpPythonScan::PropertyWrites(Code, TEXT("attribute_set_types"));
		if (Sites.Num() > 0)
		{
			return Refuse(Fill(MsgAttributeSetTypes, { FString::Join(UniqueSorted(Sites), TEXT(", ")) }), TEXT("attribute_set_types"));
		}
		// SkeletalMergingLibrary.merge_meshes crashes the editor with an assert, not an exception.
		if (McpPythonScan::ContainsExact(Calls, TEXT("merge_meshes")))
		{
			return Refuse(MsgMergeMeshes, TEXT("merge_meshes"));
		}
		if (McpPythonScan::ContainsExact(Calls, TEXT("TextureExporterPNG")))
		{
			return Refuse(MsgPngExporter, TEXT("png_exporter"));
		}
		// request_auto_rigging crashes the editor at once: auto-rigging is a cloud service.
		if (McpPythonScan::ContainsExact(Calls, TEXT("request_auto_rigging")))
		{
			return Refuse(MsgAutorig, TEXT("autorig"));
		}
		return {};
	}

	/** Starting PIE while a session is still alive corrupts the data. Fails OPEN. */
	static TOptional<FRefusal> RefusePieAlreadyRunning(const FProbe& Probe)
	{
		const TSharedPtr<FJsonObject> State = Ask(Probe, TEXT("is_play_in_editor_active"), MakeShared<FJsonObject>());
		if (!State.IsValid() || State->Values.Num() == 0)
		{
			return {}; // could not tell: LET IT THROUGH
		}
		const TSharedPtr<FJsonValue> Active = State->TryGetField(TEXT("is_pie_active"));
		if (!Active.IsValid() || Active->Type != EJson::Boolean || !Active->AsBool())
		{
			return {}; // clean: start
		}
		return Refuse(MsgPieRunning, TEXT("pie_already_running"));
	}

	/** include_ui crashes the editor (D3D12/Slate). The tool has no `force` parameter. */
	static TOptional<FRefusal> RefuseCaptureWithUi(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Truthy(Params, TEXT("include_ui")))
		{
			return {};
		}
		return Refuse(MsgCaptureUi, TEXT("capture_with_ui"));
	}

	/** Deep copy (so the test case's JSON is left untouched). */
	static TSharedPtr<FJsonObject> CloneJson(const TSharedPtr<FJsonObject>& Source)
	{
		if (!Source.IsValid())
		{
			return MakeShared<FJsonObject>();
		}
		FString Text;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Source.ToSharedRef(), Writer);
		TSharedPtr<FJsonObject> Copy;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Copy);
		return Copy.IsValid() ? Copy : MakeShared<FJsonObject>();
	}

	/** JSON with sorted keys, to compare regardless of insertion order. */
	static FString Canonical(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("null");
		}
		switch (Value->Type)
		{
		case EJson::Object:
		{
			// FJsonObject keys are FSharedString, not FString: copy them to FString to sort
			// them, then read back with TryGetField.
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			TArray<FString> Keys;
			for (const auto& Field : Object->Values)
			{
				Keys.Add(FString(Field.Key));
			}
			SortCaseSensitive(Keys);
			TArray<FString> Parts;
			for (const FString& Key : Keys)
			{
				Parts.Add(TEXT("\"") + Key + TEXT("\":") + Canonical(Object->TryGetField(Key)));
			}
			return TEXT("{") + FString::Join(Parts, TEXT(",")) + TEXT("}");
		}
		case EJson::Array:
		{
			TArray<FString> Parts;
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
			{
				Parts.Add(Canonical(Item));
			}
			return TEXT("[") + FString::Join(Parts, TEXT(",")) + TEXT("]");
		}
		case EJson::String:  return TEXT("\"") + Value->AsString() + TEXT("\"");
		case EJson::Number:  return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
		default:             return TEXT("null");
		}
	}
}

TOptional<McpPreconditions::FRefusal> McpPreconditions::Check(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FProbe& Probe)
{
	using namespace McpPreconditionsPrivate;

	// (a) by arguments
	if (TOptional<FRefusal> Refusal = RefuseDoubleSlashPath(Params)) { return Refusal; }
	if (TOptional<FRefusal> Refusal = RefuseIntegerOutOfRange(CommandType, Params)) { return Refusal; }
	if (TOptional<FRefusal> Refusal = RefuseNameTooLong(CommandType, Params)) { return Refusal; }
	if (CommandType == TEXT("batch")) { return RefuseBatchCommandTypeTooLong(Params); }
	if (CommandType == TEXT("create_material_expression")) { return RefuseSetMaterialAttributes(Params); }
	if (CommandType == TEXT("scatter_foliage_in_box") || CommandType == TEXT("add_foliage_instances")) { return RefuseFoliage(Params); }
	if (CommandType == TEXT("load_level")) { return RefuseLoadLevel(Params); }
	if (CommandType == TEXT("capture_pie_screenshot")) { return RefuseCaptureWithUi(Params); }
	if (CommandType == TEXT("save_level")) { return RefuseSaveLevelReuse(Params); }
	// (b) by a read-only probe
	if (CommandType == TEXT("delete_asset")) { return RefuseDeleteAsset(Params, Probe); }
	if (CommandType == TEXT("subscribe_to_delegate")) { return RefuseSparseDelegate(Params, Probe); }
	if (CommandType == TEXT("start_play_in_editor")) { return RefusePieAlreadyRunning(Probe); }
	// (c) by the code to run
	if (CommandType == TEXT("execute_python")) { return RefusePythonCode(CommandType, Params); }
	return {};
}

void McpPreconditions::NoteResult(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Envelope)
{
	using namespace McpPreconditionsPrivate;

	// A create_world_partition_map that WORKED leaves its world resident for the whole session.
	// Recorded AFTER the call, only on success and only when the reply says this call created the
	// map (created=true): recording it before would refuse loading a map that was never created, and
	// a map that already existed is only returned, not created here.
	if (!CommandType.Equals(TEXT("create_world_partition_map"), ESearchCase::IgnoreCase))
	{
		return;
	}
	const TSharedPtr<FJsonObject> Payload = PayloadFromEnvelope(Envelope);
	if (!Payload.IsValid() || !Truthy(Payload, TEXT("created")))
	{
		return;
	}
	// The map path the handler reports covers its default folder; the params are the fallback.
	FString MapPath = TextOf(Payload, TEXT("map_path"));
	if (MapPath.IsEmpty())
	{
		FString Folder = TextOf(Params, TEXT("package_path"));
		while (Folder.EndsWith(TEXT("/")))
		{
			Folder.LeftChopInline(1);
		}
		const FString Name = TextOf(Params, TEXT("name"));
		if (!Folder.IsEmpty() && !Name.IsEmpty())
		{
			MapPath = Folder + TEXT("/") + Name;
		}
	}
	if (!MapPath.IsEmpty())
	{
		GWpMapsCreated.Add(NormPkg(MapPath));
	}
}

void McpPreconditions::ResetSessionState()
{
	McpPreconditionsPrivate::GWpMapsCreated.Reset();
	McpPreconditionsPrivate::GSaveAsUsed.Reset();
}

TSharedPtr<FJsonObject> McpPreconditions::MakeRefusalEnvelope(const FString& CommandType, const FRefusal& Refusal)
{
	// Same envelope as the PIE guards: the diagnostics go INSIDE `result`, which is what the
	// unwrapping forwards along with `error`.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("refused"), true);
	Data->SetStringField(TEXT("refused_by"), TEXT("precondition"));
	Data->SetStringField(TEXT("command"), CommandType);
	Data->SetStringField(TEXT("check"), Refusal.Check);
	Data->SetStringField(TEXT("hint"), TEXT("Nothing was executed: the call never reached the editor. The message names the way that DOES work."));

	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("status"), TEXT("error"));
	Envelope->SetStringField(TEXT("error"), TEXT("REFUSED: ") + Refusal.Message);
	Envelope->SetObjectField(TEXT("result"), Data);
	return Envelope;
}

TSharedPtr<FJsonObject> McpPreconditions::PayloadFromEnvelope(const TSharedPtr<FJsonObject>& Envelope)
{
	if (!Envelope.IsValid())
	{
		return nullptr;
	}
	FString Status;
	Envelope->TryGetStringField(TEXT("status"), Status);
	if (Status != TEXT("success"))
	{
		return nullptr;
	}
	const TSharedPtr<FJsonObject>* Result = nullptr;
	if (!Envelope->TryGetObjectField(TEXT("result"), Result) || !Result || !Result->IsValid())
	{
		return MakeShared<FJsonObject>();
	}
	// The bridge wraps SOME responses in `data` and not others: accept both -- except a `data` that
	// is only the bridge's parameter report (McpPy::Payload).
	const TSharedPtr<FJsonObject> Payload = McpPy::Payload(*Result);
	bool bSuccess = true;
	if (Payload->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
	{
		return nullptr;
	}
	return Payload;
}

TSharedPtr<FJsonObject> McpPreconditions::AssetMetadataFromRegistry(const TSharedPtr<FJsonObject>& ProbeParams)
{
	FString AssetName;
	if (!ProbeParams.IsValid() || !ProbeParams->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		return nullptr;
	}
	// Same path as the handler: prepend "/Game/" (which is why the probe strips that prefix).
	FString PackageName = TEXT("/Game/") + AssetName;
	int32 Dot = INDEX_NONE;
	if (PackageName.FindChar(TEXT('.'), Dot))
	{
		PackageName.LeftInline(Dot);
	}
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		return nullptr;
	}

	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPackageName(FName(*PackageName), Assets, /*bIncludeOnlyOnDiskAssets*/ false);
	if (Assets.Num() == 0)
	{
		return nullptr; // unknown to the registry (or still scanning): let the handler answer
	}
	const FString ShortName = FPackageName::GetShortName(PackageName);
	const FAssetData* Primary = Assets.FindByPredicate([&ShortName](const FAssetData& Asset)
	{
		return Asset.AssetName.ToString().Equals(ShortName, ESearchCase::IgnoreCase);
	});
	if (!Primary)
	{
		Primary = &Assets[0];
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("name"), Primary->AssetName.ToString());
	// Same as Asset->GetClass()->GetName(): "World", "Blueprint", "MetaHumanCharacter"...
	Result->SetStringField(TEXT("class"), Primary->AssetClassPath.GetAssetName().ToString());
	Result->SetStringField(TEXT("path"), Primary->GetObjectPathString());
	Result->SetStringField(TEXT("package"), Primary->PackageName.ToString());
	Result->SetStringField(TEXT("asset_class_path"), Primary->AssetClassPath.ToString());
	Result->SetStringField(TEXT("source"), TEXT("asset_registry"));
	return Result;
}

FString McpPreconditions::RunSelfTest(const FString& VectorsPath, FString& OutReportPath)
{
	using namespace McpPreconditionsPrivate;

	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *VectorsPath) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return FString::Printf(TEXT("could not read '%s'"), *VectorsPath);
	}
	const TArray<TSharedPtr<FJsonValue>>* Cases = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Records = nullptr;
	const bool bHasCases = Root->TryGetArrayField(TEXT("cases"), Cases) && Cases;
	const bool bHasRecords = Root->TryGetArrayField(TEXT("records"), Records) && Records;
	if (!bHasCases && !bHasRecords)
	{
		return TEXT("the file has neither 'cases' nor 'records'");
	}
	static const TArray<TSharedPtr<FJsonValue>> NoCases;
	if (!bHasCases)
	{
		Cases = &NoCases;
	}

	// The self-test must not erase what the editor already knows about this session.
	const TSet<FString> SavedWp = GWpMapsCreated;
	const TSet<FString> SavedSaveAs = GSaveAsUsed;

	// The vectors declare the project settings they were written against (e.g. which content
	// roots are vendored libraries), so they mean the same thing in any project.
	bool bOverrodeRoots = false;
	const TSharedPtr<FJsonObject>* VectorSettings = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* VectorRoots = nullptr;
	if (Root->TryGetObjectField(TEXT("settings"), VectorSettings) && VectorSettings
		&& (*VectorSettings)->TryGetArrayField(TEXT("read_only_content_roots"), VectorRoots) && VectorRoots)
	{
		TArray<FString> Roots;
		for (const TSharedPtr<FJsonValue>& RootValue : *VectorRoots)
		{
			Roots.Add(RootValue->AsString());
		}
		mcp_common::SetDefaultReadonlyRoots(Roots);
		bOverrodeRoots = true;
	}
	ON_SCOPE_EXIT
	{
		if (bOverrodeRoots)
		{
			mcp_common::ClearDefaultReadonlyRootsOverride();
		}
	};

	int32 TotalChecks = 0;
	int32 Passed = 0;
	TArray<TSharedPtr<FJsonValue>> Failures;
	// Recorded READBACK contract vectors (readback / take_before / annotate_offload /
	// note_result): McpReadback replays them.
	TArray<TSharedPtr<FJsonValue>> Skipped;
	if (bHasRecords)
	{
		McpReadback::RunRecords(*Records, TotalChecks, Passed, Failures, Skipped);
	}
	for (const TSharedPtr<FJsonValue>& CaseValue : *Cases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		const FString CaseId = Case->GetStringField(TEXT("id"));
		ResetSessionState();
		const TArray<TSharedPtr<FJsonValue>>* Steps = nullptr;
		if (!Case->TryGetArrayField(TEXT("steps"), Steps) || !Steps)
		{
			continue;
		}
		int32 StepIndex = 0;
		for (const TSharedPtr<FJsonValue>& StepValue : *Steps)
		{
			const TSharedPtr<FJsonObject> Step = StepValue->AsObject();
			const FString Op = Step->GetStringField(TEXT("op"));
			const FString Command = Step->GetStringField(TEXT("command"));
			const TSharedPtr<FJsonObject>* StepParams = nullptr;
			Step->TryGetObjectField(TEXT("params"), StepParams);
			const TSharedPtr<FJsonObject> Params = CloneJson(StepParams ? *StepParams : nullptr);

			if (Op == TEXT("note"))
			{
				const TSharedPtr<FJsonObject>* Envelope = nullptr;
				Step->TryGetObjectField(TEXT("envelope"), Envelope);
				NoteResult(Command, Params, Envelope ? *Envelope : nullptr);
				++StepIndex;
				continue;
			}

			// The REAL get_asset_metadata probe (Asset Registry, nothing loaded): it can be checked
			// against a real map without risk, which delete_asset cannot.
			if (Op == TEXT("registry_probe"))
			{
				const double Start = FPlatformTime::Seconds();
				const TSharedPtr<FJsonObject> Found = AssetMetadataFromRegistry(Params);
				const double Millis = (FPlatformTime::Seconds() - Start) * 1000.0;
				FString ExpectClass;
				Step->TryGetStringField(TEXT("expect_class"), ExpectClass);
				const FString GotClass = Found.IsValid() ? Found->GetStringField(TEXT("class")) : FString(TEXT("<null>"));
				++TotalChecks;
				if (GotClass.Equals(ExpectClass, ESearchCase::CaseSensitive))
				{
					++Passed;
				}
				else
				{
					TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetStringField(TEXT("case"), CaseId);
					Failure->SetStringField(TEXT("expected_class"), ExpectClass);
					Failure->SetStringField(TEXT("got_class"), GotClass);
					Failures.Add(MakeShared<FJsonValueObject>(Failure));
				}
				UE_LOG(LogTemp, Display, TEXT("[MCP] registry_probe %s -> %s in %.1f ms"), *CaseId, *GotClass, Millis);
				++StepIndex;
				continue;
			}

			// The REAL asset-name guard of the create commands (mcp_common::AssetNameConflict): a read-only
			// lookup, checked against engine content every project has. An empty expected message means
			// the name is free for that class.
			if (Op == TEXT("name_conflict"))
			{
				FString PackageName, ObjectName, ClassPath, ExpectMessage;
				Params->TryGetStringField(TEXT("package_name"), PackageName);
				Params->TryGetStringField(TEXT("object_name"), ObjectName);
				Params->TryGetStringField(TEXT("class"), ClassPath);
				Step->TryGetStringField(TEXT("expect_message"), ExpectMessage);
				const UClass* ObjectClass = FindObject<UClass>(nullptr, *ClassPath);
				const FString Got = ObjectClass ? mcp_common::AssetNameConflict(PackageName, ObjectName, ObjectClass)
					: FString::Printf(TEXT("<class not found: %s>"), *ClassPath);
				++TotalChecks;
				if (Got.Equals(ExpectMessage, ESearchCase::CaseSensitive))
				{
					++Passed;
				}
				else
				{
					TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
					Failure->SetStringField(TEXT("case"), CaseId);
					Failure->SetStringField(TEXT("expected_message"), ExpectMessage);
					Failure->SetStringField(TEXT("got_message"), Got);
					Failures.Add(MakeShared<FJsonValueObject>(Failure));
				}
				++StepIndex;
				continue;
			}

			// FAKE probes: the replies recorded for each probe command.
			const TSharedPtr<FJsonObject>* Probes = nullptr;
			Step->TryGetObjectField(TEXT("probes"), Probes);
			TArray<TSharedPtr<FJsonValue>> ProbeCalls;
			const FProbe Fake = [&Probes, &ProbeCalls](const FString& ProbeCommand, const TSharedPtr<FJsonObject>& ProbeParams) -> TSharedPtr<FJsonObject>
			{
				TArray<TSharedPtr<FJsonValue>> Call;
				Call.Add(MakeShared<FJsonValueString>(ProbeCommand));
				Call.Add(MakeShared<FJsonValueObject>(CloneJson(ProbeParams)));
				ProbeCalls.Add(MakeShared<FJsonValueArray>(Call));
				const TSharedPtr<FJsonObject>* Reply = nullptr;
				if (!Probes || !(*Probes)->TryGetObjectField(ProbeCommand, Reply) || !Reply)
				{
					return MakeShared<FJsonObject>(); // an unrecorded probe answers {}
				}
				bool bSuccess = true;
				if ((*Reply)->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
				{
					return nullptr;
				}
				return CloneJson(*Reply);
			};

			const TOptional<FRefusal> Got = Check(Command, Params, Fake);
			const TSharedPtr<FJsonObject> Expect = Step->GetObjectField(TEXT("expect"));
			const bool bExpectRefused = Expect->GetBoolField(TEXT("refused"));
			FString ExpectMessage;
			Expect->TryGetStringField(TEXT("message"), ExpectMessage);
			// Where the expected message quotes reformatted code, the case carries `message_prefix` and
			// only that prefix is compared: what differs is the spacing of the quote.
			FString ExpectPrefix;
			const bool bPrefixOnly = Expect->TryGetStringField(TEXT("message_prefix"), ExpectPrefix);

			bool bOk = Got.IsSet() == bExpectRefused && (!bExpectRefused
				|| (bPrefixOnly ? Got->Message.StartsWith(ExpectPrefix, ESearchCase::CaseSensitive)
				                : Got->Message.Equals(ExpectMessage, ESearchCase::CaseSensitive)));
			FString ProbeDiff;
			const TArray<TSharedPtr<FJsonValue>>* ExpectProbes = nullptr;
			if (Step->TryGetArrayField(TEXT("expect_probe_calls"), ExpectProbes) && ExpectProbes)
			{
				const FString Want = Canonical(MakeShared<FJsonValueArray>(*ExpectProbes));
				const FString Have = Canonical(MakeShared<FJsonValueArray>(ProbeCalls));
				if (Want != Have)
				{
					bOk = false;
					ProbeDiff = FString::Printf(TEXT("probes: expected %s, got %s"), *Want, *Have);
				}
			}

			++TotalChecks;
			if (bOk)
			{
				++Passed;
			}
			else
			{
				TSharedPtr<FJsonObject> Failure = MakeShared<FJsonObject>();
				Failure->SetStringField(TEXT("case"), CaseId);
				Failure->SetNumberField(TEXT("step"), StepIndex);
				Failure->SetStringField(TEXT("command"), Command);
				Failure->SetBoolField(TEXT("expected_refused"), bExpectRefused);
				Failure->SetBoolField(TEXT("got_refused"), Got.IsSet());
				Failure->SetStringField(TEXT("expected_message"), ExpectMessage);
				Failure->SetStringField(TEXT("got_message"), Got.IsSet() ? Got->Message : FString());
				Failure->SetStringField(TEXT("probe_diff"), ProbeDiff);
				Failures.Add(MakeShared<FJsonValueObject>(Failure));
			}
			++StepIndex;
		}
	}

	GWpMapsCreated = SavedWp;
	GSaveAsUsed = SavedSaveAs;

	TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("checks"), TotalChecks);
	Report->SetNumberField(TEXT("passed"), Passed);
	Report->SetArrayField(TEXT("failures"), Failures);
	if (bHasRecords)
	{
		Report->SetArrayField(TEXT("skipped"), Skipped);
		Report->SetObjectField(TEXT("readback_tables"), McpReadback::TablesToJson());
	}
	FString ReportText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ReportText);
	FJsonSerializer::Serialize(Report.ToSharedRef(), Writer);
	OutReportPath = VectorsPath + TEXT(".result.json");
	FFileHelper::SaveStringToFile(ReportText, *OutReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	return FString::Printf(TEXT("%d/%d"), Passed, TotalChecks);
}

namespace McpPreconditionsConsole
{
	static FAutoConsoleCommand SelfTestCommand(
		TEXT("mcp.contract.selftest"),
		TEXT("Runs the contract test vectors against the precondition refusals and the READBACK checks, with fake probes: nothing reaches any handler. Usage: mcp.contract.selftest <path> [quit]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& InArgs)
		{
			// A trailing `quit` closes the editor when done, with exit code 0 if everything passed and 1
			// otherwise. That is what lets it run in a headless editor (UnrealEditor-Cmd -ExecCmds),
			// where the console `Quit` does not end the process.
			TArray<FString> Args = InArgs;
			const bool bQuit = Args.Num() > 0 && Args.Last().Equals(TEXT("quit"), ESearchCase::IgnoreCase);
			if (bQuit)
			{
				Args.Pop();
			}
			FString ReportPath;
			const FString Summary = McpPreconditions::RunSelfTest(FString::Join(Args, TEXT(" ")), ReportPath);
			UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.contract.selftest: %s (report: %s)"), *Summary, *ReportPath);
			if (bQuit)
			{
				FString Passed, Total;
				const bool bAll = Summary.Split(TEXT("/"), &Passed, &Total) && Passed == Total;
				// On Windows a NON-forced exit drops the exit code (PostQuitMessage) and always exits 0.
				// So a failure exits FORCED -- the report is already written to disk.
				FPlatformMisc::RequestExitWithStatus(!bAll, bAll ? 0 : 1);
			}
		}));
}
