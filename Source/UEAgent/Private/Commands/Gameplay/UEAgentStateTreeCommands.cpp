#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "MCPStateTreeSchema.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "StructUtils/PropertyBag.h"
#include "PropertyBindingTypes.h"   // UE::PropertyBinding::FPropertyCreationDescriptor for add_tree_parameter
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
// Task/condition node authoring + compile.
#include "StateTreeEditorNode.h"
#include "StateTreeTaskBase.h"
#include "StateTreeConditionBase.h"
// State completion aggregation enum + tree-scoped evaluators.
#include "StateTreeTasksStatus.h"     // EStateTreeTaskCompletionType { All, Any } for set_state_completion_type
#include "StateTreeEvaluatorBase.h"   // FStateTreeEvaluatorBase::StaticStruct() for add_state_evaluator
#include "StateTreeConsiderationBase.h"        // FStateTreeConsiderationBase::StaticStruct() for add_consideration
#include "Components/StateTreeComponent.h"     // UStateTreeComponent + StateTreeRef for set_component_parameter_override
#include "GameplayTagContainer.h"              // FGameplayTag parsing for update_transition event_tag
#include "StateTreeEditingSubsystem.h"
#include "StateTreeCompilerLog.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/UObjectGlobals.h"
// Property bindings (parameter -> task input).
#include "StateTreeEditorPropertyBindings.h"   // FStateTreeEditorPropertyBindings::AddStateTreeBinding/GetBindings
#include "StateTreePropertyBindings.h"          // FStateTreePropertyPathBinding
#include "PropertyBindingPath.h"                // FPropertyBindingPath (PropertyBindingUtils)
// Read/list/remove use EditorAssetLibrary for tree deletion + AssetRegistry for enumeration.
#include "EditorAssetLibrary.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
// Reflection-driven get/set on task/condition/evaluator instance
// structs; uses FJsonObjectConverter (engine standard) for struct<->json round-trip
// and TObjectIterator<UScriptStruct> to discover registered task/condition/evaluator
// types without engine-doc lookup.
#include "JsonObjectConverter.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"  // CLASS_Abstract (ListDerivedStructNames filter)
// Component-side state tree resolution (UStateTreeComponent lives in GameplayStateTreeModule plugin — not always linked)
// We discover the component class via TObjectIterator<UStateTreeComponent> reflection walk instead of including the runtime header.
#include "EngineUtils.h"

/**
 * State Tree commands.
 *
 * Wraps the engine's StateTree plugin (UStateTree + UStateTreeEditorData) so an LLM can author state tree
 * assets: create the tree, add states (top-level SubTrees), wire transitions between them, set the root by
 * name, add typed parameters to the root parameter bag, attach tasks, conditions, evaluators and
 * considerations, bind properties, and compile.
 */
namespace UEAgentStateTreeCommands
{

// MCP-DOMAIN: StateTree

namespace
{
	/**
	 * Recursive walk helper for UStateTreeState. Uses a member function via
	 * a static-local trampoline to avoid std::function (which requires
	 * <functional> and adds overhead for headless tooling).
	 */
	struct FStateWalker
	{
		UStateTreeState* Target = nullptr;
		FName WantName;

		/** Returns the matching state or nullptr. Mutates `this`. */
		UStateTreeState* FindRecursive(UStateTreeState* S)
		{
			if (!S) { return nullptr; }
			if (S->Name == WantName) { return S; }
			for (UStateTreeState* Child : S->Children)
			{
				if (UStateTreeState* Hit = FindRecursive(Child)) { return Hit; }
			}
			return nullptr;
		}
	};

	UStateTreeState* FindStateByName(UStateTreeEditorData* ED, FName Name)
	{
		if (!ED) { return nullptr; }
		FStateWalker Walker;
		Walker.WantName = Name;
		for (UStateTreeState* SubTree : ED->SubTrees)
		{
			if (UStateTreeState* Hit = Walker.FindRecursive(SubTree)) { return Hit; }
		}
		return nullptr;
	}

	int32 CountStatesRecursive(UStateTreeState* S)
	{
		if (!S) { return 0; }
		int32 N = 1;
		for (UStateTreeState* Child : S->Children) { N += CountStatesRecursive(Child); }
		return N;
	}

	/** Resolve a UScriptStruct by short reflected name or /Script path. */
	UScriptStruct* ResolveScriptStructByName(const FString& Name)
	{
		if (Name.IsEmpty()) { return nullptr; }
		if (Name.Contains(TEXT(".")) || Name.StartsWith(TEXT("/Script")))
		{
			if (UScriptStruct* S = FindObject<UScriptStruct>(nullptr, *Name)) { return S; }
		}
		if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst))
		{
			return S;
		}
		// UHT strips the leading 'F' from struct reflection names; retry without it.
		if (Name.StartsWith(TEXT("F")) && Name.Len() > 1)
		{
			if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*Name.RightChop(1), EFindFirstObjectOptions::NativeFirst))
			{
				return S;
			}
		}
		return nullptr;
	}

	/** Map an enum value name (short or qualified) to its value. */
	template<typename TEnum>
	bool ResolveEnumByName(const FString& Name, TEnum& OutValue)
	{
		const UEnum* E = StaticEnum<TEnum>();
		if (!E) { return false; }
		int64 V = E->GetValueByNameString(Name);
		if (V == INDEX_NONE)
		{
			V = E->GetValueByNameString(E->GetName() + TEXT("::") + Name);
		}
		if (V == INDEX_NONE) { return false; }
		OutValue = static_cast<TEnum>(V);
		return true;
	}

	/** Short display name for an enum value. */
	template<typename TEnum>
	FString EnumToName(TEnum Value)
	{
		const UEnum* E = StaticEnum<TEnum>();
		return E ? E->GetNameStringByValue(static_cast<int64>(Value)) : FString(TEXT("Unknown"));
	}

	/** Load the UStateTree at TreePath and return its EditorData; on failure returns nullptr and fills OutError. */
	UStateTreeEditorData* StateTree_LoadEditorData(const FString& TreePath, const TCHAR* NoEditorDataError,
		UStateTree*& OutTree, TSharedPtr<FJsonObject>& OutError)
	{
		OutTree = LoadObject<UStateTree>(nullptr, *TreePath);
		if (!OutTree)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Cannot load state tree: %s"), *TreePath));
			return nullptr;
		}
		UStateTreeEditorData* ED = Cast<UStateTreeEditorData>(OutTree->EditorData);
		if (!ED)
		{
			OutError = mcp_common::MakeErrorReply(NoEditorDataError);
		}
		return ED;
	}

	/** StateTree_LoadEditorData for the tree named by the required tree_path param. */
	UStateTreeEditorData* StateTree_ResolveEditorData(const TSharedPtr<FJsonObject>& Params, const TCHAR* NoEditorDataError,
		UStateTree*& OutTree, TSharedPtr<FJsonObject>& OutError)
	{
		OutTree = nullptr;
		FString TreePath;
		if (!Params->TryGetStringField(TEXT("tree_path"), TreePath) || TreePath.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("tree_path is required"));
			return nullptr;
		}
		return StateTree_LoadEditorData(TreePath, NoEditorDataError, OutTree, OutError);
	}

	/** Load tree + EditorData + find a named state; on failure returns nullptr and fills OutError. */
	UStateTreeState* ResolveTreeAndState(
		const TSharedPtr<FJsonObject>& Params, const TCHAR* StateField,
		UStateTree*& OutTree, UStateTreeEditorData*& OutED, TSharedPtr<FJsonObject>& OutError)
	{
		OutTree = nullptr; OutED = nullptr; OutError.Reset();
		FString TreePath, StateName;
		if (!Params->TryGetStringField(TEXT("tree_path"), TreePath) || TreePath.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(TEXT("tree_path is required"));
			return nullptr;
		}
		if (!Params->TryGetStringField(StateField, StateName) || StateName.IsEmpty())
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s is required"), StateField));
			return nullptr;
		}
		OutED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), OutTree, OutError);
		if (!OutED) { return nullptr; }
		UStateTreeState* State = FindStateByName(OutED, FName(*StateName));
		if (!State)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("State not found: %s"), *StateName));
		}
		return State;
	}

	int32 StateTree_CountAllStates(UStateTreeEditorData* ED)
	{
		int32 Count = 0;
		for (UStateTreeState* Sub : ED->SubTrees) { Count += CountStatesRecursive(Sub); }
		return Count;
	}

	/** A reply payload that starts with Tree's tree_path. */
	TSharedPtr<FJsonObject> StateTree_NewTreeData(const UStateTree* Tree)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("tree_path"), Tree->GetPathName());
		return Data;
	}

	/** StateTree_NewTreeData followed by State's state_name. */
	TSharedPtr<FJsonObject> StateTree_NewStateData(const UStateTree* Tree, const UStateTreeState* State)
	{
		TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
		Data->SetStringField(TEXT("state_name"), State->Name.ToString());
		return Data;
	}

	/** Resolve a node struct by name and require it to derive from Base; on failure returns nullptr and fills OutError. */
	UScriptStruct* StateTree_ResolveNodeStruct(const FString& StructName, const UScriptStruct* Base,
		const FString& NotFoundError, TSharedPtr<FJsonObject>& OutError)
	{
		UScriptStruct* Struct = ResolveScriptStructByName(StructName);
		if (!Struct)
		{
			OutError = mcp_common::MakeErrorReply(NotFoundError);
			return nullptr;
		}
		if (!Struct->IsChildOf(Base))
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s does not derive from %s"), *Struct->GetName(), *Base->GetName()));
			return nullptr;
		}
		return Struct;
	}

	/** The tree's root parameter named Name, or nullptr. */
	const FPropertyBagPropertyDesc* StateTree_FindRootParameter(const UStateTreeEditorData* ED, FName Name)
	{
		if (const UPropertyBag* Bag = ED->GetRootParametersPropertyBag().GetPropertyBagStruct())
		{
			for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyDescs())
			{
				if (Desc.Name == Name) { return &Desc; }
			}
		}
		return nullptr;
	}

	int32 StateTree_RootParameterCount(const UStateTreeEditorData* ED)
	{
		const UPropertyBag* Bag = ED->GetRootParametersPropertyBag().GetPropertyBagStruct();
		return Bag ? Bag->GetPropertyDescs().Num() : 0;
	}

	/** [{ index, class, instance_id }] of a node list. */
	TArray<TSharedPtr<FJsonValue>> StateTree_NodeListToJson(const TArray<FStateTreeEditorNode>& Nodes)
	{
		TArray<TSharedPtr<FJsonValue>> NodesJson;
		NodesJson.Reserve(Nodes.Num());
		for (int32 i = 0; i < Nodes.Num(); ++i)
		{
			const FStateTreeEditorNode& N = Nodes[i];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"),       i);
			Entry->SetStringField(TEXT("class"),       N.Node.GetScriptStruct() ? N.Node.GetScriptStruct()->GetName() : TEXT(""));
			Entry->SetStringField(TEXT("instance_id"), N.ID.ToString());
			NodesJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return NodesJson;
	}

	/**
	 * Remove Items[Index] (Owner->Modify() first, the tree saved after) and add removed, remaining and saved to Data.
	 * An out-of-range Index removes nothing and adds OutOfRangeNote as note.
	 */
	template <typename ItemType>
	void StateTree_RemoveAtIndex(const TSharedPtr<FJsonObject>& Data, UStateTree* Tree, UObject* Owner,
		TArray<ItemType>& Items, int32 Index, const TCHAR* OutOfRangeNote)
	{
		if (!Items.IsValidIndex(Index))
		{
			Data->SetBoolField  (TEXT("removed"),   false);
			Data->SetNumberField(TEXT("remaining"), Items.Num());
			Data->SetBoolField  (TEXT("saved"),     false);
			Data->SetStringField(TEXT("note"),      OutOfRangeNote);
			return;
		}
		Owner->Modify();
		Items.RemoveAt(Index, 1, EAllowShrinking::No);
		const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);
		Data->SetBoolField  (TEXT("removed"),   true);
		Data->SetNumberField(TEXT("remaining"), Items.Num());
		Data->SetBoolField  (TEXT("saved"),     bSaved);
	}

	/** Map a friendly type string to EPropertyBagPropertyType. */
	EPropertyBagPropertyType MapFriendlyType(const FString& Friendly)
	{
		const FString T = Friendly.ToLower();
		if (T == TEXT("bool"))   { return EPropertyBagPropertyType::Bool; }
		if (T == TEXT("byte"))   { return EPropertyBagPropertyType::Byte; }
		if (T == TEXT("int") || T == TEXT("int32"))  { return EPropertyBagPropertyType::Int32; }
		if (T == TEXT("int64"))  { return EPropertyBagPropertyType::Int64; }
		if (T == TEXT("float"))  { return EPropertyBagPropertyType::Float; }
		if (T == TEXT("double")) { return EPropertyBagPropertyType::Double; }
		if (T == TEXT("name"))   { return EPropertyBagPropertyType::Name; }
		if (T == TEXT("string") || T == TEXT("fstring")) { return EPropertyBagPropertyType::String; }
		if (T == TEXT("text"))   { return EPropertyBagPropertyType::Text; }
		if (T == TEXT("object")) { return EPropertyBagPropertyType::Object; }
		if (T == TEXT("class"))  { return EPropertyBagPropertyType::Class; }
		if (T == TEXT("struct")) { return EPropertyBagPropertyType::Struct; }
		return EPropertyBagPropertyType::String;
	}
}

// ---------------------------------------------------------------------------
// create_state_tree
// ---------------------------------------------------------------------------
// MCP:create_state_tree - Create a StateTree asset at package_path/tree_name (or load the one already there), giving it any missing editor data, the plugin UMCPStateTreeSchema when it has no schema and a Root state when it has no states, then compile and save it. Returns tree_path, state_count, created and saved.
//   @param tree_name - Asset name without path, e.g. "ST_Guard"; an asset of another class with that name is an error.
//   @param package_path - Content folder for the asset, e.g. "/Game/AI" (default /Game/MCP).
//   @returns { tree_path, tree_name, state_count, created, saved }
TSharedPtr<FJsonObject> HandleCreateStateTree(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreeName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_name"), TreeName)) { return Missing; }
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);

	const FString FullPath = PackagePath / TreeName;
	FString CreateError;
	bool bCreated = false;
	UStateTree* Tree = mcp_common::CreateOrLoadAsset<UStateTree>(FullPath, CreateError, &bCreated);
	if (!Tree)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create or load state tree: %s"), *CreateError));
	}

	UStateTreeEditorData* ED = Cast<UStateTreeEditorData>(Tree->EditorData);
	if (!ED)
	{
		ED = NewObject<UStateTreeEditorData>(Tree, FName(), RF_Transactional);
		Tree->EditorData = ED;
	}
	if (!ED->Schema)
	{
		// Concrete plugin schema: the abstract UStateTreeSchema base trips the abstract-class
		// ensure and gets nulled out on save, leaving the saved asset schema-less.
		ED->Schema = NewObject<UMCPStateTreeSchema>(ED, UMCPStateTreeSchema::StaticClass(),
			FName(), RF_Transactional);
	}

	int32 StateCount = 0;
	if (ED->SubTrees.Num() == 0)
	{
		UStateTreeState& Root = ED->AddRootState();
		Root.ID = FGuid::NewGuid();
		StateCount = 1;
	}
	else
	{
		StateCount = StateTree_CountAllStates(ED);
	}

	Tree->CompileIfChanged();
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("tree_name"), Tree->GetName());
	Data->SetNumberField(TEXT("state_count"), StateCount);
	Data->SetBoolField(TEXT("created"), bCreated);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_state_tree"));
}

// ---------------------------------------------------------------------------
// add_state
// ---------------------------------------------------------------------------
// MCP:add_state - Add a top-level state (a new SubTree, never a child of another state) to a StateTree asset, then compile and save it. Idempotent: if any state already has that name nothing changes and already_existed is true. Returns state_count, added and saved.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name - Name of the new state, e.g. "Patrol"; the other state commands find states by this name.
//   @returns { tree_path, state_name, state_count, already_existed, added, saved }
TSharedPtr<FJsonObject> HandleAddState(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, StateName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_name"), StateName)) { return Missing; }

	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
	if (!ED) { return Err; }

	if (UStateTreeState* Existing = FindStateByName(ED, FName(*StateName)))
	{
		TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
		Data->SetStringField(TEXT("state_name"), StateName);
		Data->SetNumberField(TEXT("state_count"), StateTree_CountAllStates(ED));
		Data->SetBoolField(TEXT("already_existed"), true);
		return mcp_common::MakeOkReply(Data);
	}

	UStateTreeState& NewState = ED->AddSubTree(FName(*StateName));
	NewState.ID = FGuid::NewGuid();
	Tree->CompileIfChanged();
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("state_name"), StateName);
	Data->SetNumberField(TEXT("state_count"), StateTree_CountAllStates(ED));
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state"));
}

// ---------------------------------------------------------------------------
// add_state_transition
// ---------------------------------------------------------------------------
// MCP:add_state_transition - Append a transition from from_state to to_state on a StateTree asset (trigger OnStateCompleted, type GotoState), then compile and save it; each call adds another one. event_name only fills RequiredEvent, which matters with an OnEvent trigger: set that with set_transition_properties. Returns transition_count and saved.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param from_state - Name of the source state, which owns the new transition.
//   @param to_state - Name of the target state the transition goes to.
//   @param event_name - Gameplay tag stored as the transition RequiredEvent, e.g. "AI.Event.Alert"; a tag that is not registered is ignored without error.
//   @returns { tree_path, from_state, to_state, event_name, transition_count, added, saved }
TSharedPtr<FJsonObject> HandleAddStateTransition(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, FromState, ToState;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_state"), FromState)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_state"), ToState)) { return Missing; }
	FString EventName;
	Params->TryGetStringField(TEXT("event_name"), EventName);

	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
	if (!ED) { return Err; }

	UStateTreeState* From = FindStateByName(ED, FName(*FromState));
	UStateTreeState* To   = FindStateByName(ED, FName(*ToState));
	if (!From)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Source state not found: %s"), *FromState));
	}
	if (!To)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Target state not found: %s"), *ToState));
	}

	FStateTreeTransition& T = From->AddTransition(
		EStateTreeTransitionTrigger::OnStateCompleted,
		EStateTreeTransitionType::GotoState,
		To);
	T.ID = FGuid::NewGuid();
	T.State = To->GetLinkToState();
	if (!EventName.IsEmpty())
	{
		FGameplayTag EventTag = FGameplayTag::RequestGameplayTag(FName(*EventName), false);
		if (EventTag.IsValid())
		{
			T.RequiredEvent.Tag = EventTag;
		}
	}

	Tree->CompileIfChanged();
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("from_state"), FromState);
	Data->SetStringField(TEXT("to_state"), ToState);
	Data->SetStringField(TEXT("event_name"), EventName);
	Data->SetNumberField(TEXT("transition_count"), From->Transitions.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state_transition"));
}

// ---------------------------------------------------------------------------
// set_state_tree_root
// ---------------------------------------------------------------------------
// MCP:set_state_tree_root - Make a top-level state the root of a StateTree asset by moving it to the first SubTree slot, then compile and save it. A nested state is refused. Returns root_state with set and saved, or already_root when it was already first (nothing saved).
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param root_state_name - Name of a top-level state, such as one made by add_state; a state nested under another is refused.
//   @returns { tree_path, root_state, already_root, set, saved }
TSharedPtr<FJsonObject> HandleSetStateTreeRoot(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, RootStateName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("root_state_name"), RootStateName)) { return Missing; }

	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
	if (!ED) { return Err; }

	UStateTreeState* Target = FindStateByName(ED, FName(*RootStateName));
	if (!Target)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("State not found: %s"), *RootStateName));
	}
	bool bIsTopLevel = false;
	for (UStateTreeState* Sub : ED->SubTrees)
	{
		if (Sub == Target) { bIsTopLevel = true; break; }
	}
	if (!bIsTopLevel)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("State '%s' is nested under another state; only top-level SubTrees can be root"),
				*RootStateName));
	}
	if (ED->SubTrees[0] == Target)
	{
		TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
		Data->SetStringField(TEXT("root_state"), RootStateName);
		Data->SetBoolField(TEXT("already_root"), true);
		return mcp_common::MakeOkReply(Data);
	}
	ED->SubTrees.Remove(Target);
	ED->SubTrees.Insert(Target, 0);

	Tree->CompileIfChanged();
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("root_state"), RootStateName);
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_state_tree_root"));
}

// ---------------------------------------------------------------------------
// add_tree_parameter  — real implementation via CreateRootProperties
//   RootParameterPropertyBag is a private UPROPERTY; the public mutation path is
//   UStateTreeEditorData::CreateRootProperties(TArrayView<FPropertyCreationDescriptor>)
//   (FPropertyCreationDescriptor lives in PropertyBindingUtils).
//   Build one descriptor (name + scalar type) and read back via the public
//   GetRootParametersPropertyBag() getter.
// ---------------------------------------------------------------------------
// MCP:add_tree_parameter - Add a typed parameter to a UStateTree's root parameter bag (usable for bindings).
//   @param tree_path - Asset path to the UStateTree
//   @param parameter_name - Name of the parameter to add
//   @param parameter_type - Scalar type: bool, int (default), int64, float, double, name, string, text, byte
//   @returns { tree_path, parameter_name, parameter_type, parameter_count, added, saved }
TSharedPtr<FJsonObject> HandleAddTreeParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName)) { return Missing; }
	FString TypeStr = TEXT("int");
	Params->TryGetStringField(TEXT("parameter_type"), TypeStr);

	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
	if (!ED) { return Err; }

	UE::PropertyBinding::FPropertyCreationDescriptor Desc;
	Desc.PropertyDesc = FPropertyBagPropertyDesc(FName(*ParamName), MapFriendlyType(TypeStr));
	ED->CreateRootProperties(MakeArrayView(&Desc, 1));
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("parameter_name"), ParamName);
	Data->SetStringField(TEXT("parameter_type"), TypeStr);
	Data->SetNumberField(TEXT("parameter_count"), StateTree_RootParameterCount(ED));
	Data->SetBoolField(TEXT("added"), StateTree_FindRootParameter(ED, FName(*ParamName)) != nullptr);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_tree_parameter"));
}

// ---------------------------------------------------------------------------
// get_state_tree_info
// ---------------------------------------------------------------------------
// MCP:get_state_tree_info - Read a StateTree asset without changing it: every state with its type, selection behavior and task, enter-condition, child and transition counts, every transition with trigger, priority, event tag and delay, the root parameters, root_state and evaluator, global task and binding counts.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @returns { name, transitions, children, from_state, to_state, trigger, event_tag, type, tree_path, state_count, states, transition_count, parameter_count, parameters, root_state }
TSharedPtr<FJsonObject> HandleGetStateTreeInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	// One iterative depth-first walk over every state: a states entry per state, a transitions entry per transition.
	TArray<TSharedPtr<FJsonValue>> StatesJson;
	TArray<TSharedPtr<FJsonValue>> TransJson;
	int32 TotalStates = 0;
	int32 TotalTransitions = 0;
	{
		TArray<UStateTreeState*> Stack;
		for (UStateTreeState* Sub : ED->SubTrees) { Stack.Add(Sub); }
		while (Stack.Num() > 0)
		{
			UStateTreeState* S = Stack.Pop(EAllowShrinking::No);
			if (!S) { continue; }
			++TotalStates;
			TotalTransitions += S->Transitions.Num();
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), S->Name.ToString());
			Entry->SetNumberField(TEXT("transitions"), S->Transitions.Num());
			Entry->SetNumberField(TEXT("children"), S->Children.Num());
			// Task / enter-condition counts + state type / selection behavior.
			Entry->SetNumberField(TEXT("task_count"), S->Tasks.Num());
			Entry->SetNumberField(TEXT("enter_condition_count"), S->EnterConditions.Num());
			Entry->SetStringField(TEXT("type"), EnumToName(S->Type));
			Entry->SetStringField(TEXT("selection_behavior"), EnumToName(S->SelectionBehavior));
			StatesJson.Add(MakeShared<FJsonValueObject>(Entry));
			for (int32 ti = 0; ti < S->Transitions.Num(); ++ti)
			{
				const FStateTreeTransition& T = S->Transitions[ti];
				TSharedPtr<FJsonObject> TransEntry = MakeShared<FJsonObject>();
				TransEntry->SetStringField(TEXT("from_state"), S->Name.ToString());
				TransEntry->SetNumberField(TEXT("index"), ti);
				TransEntry->SetStringField(TEXT("to_state"), T.State.Name.ToString());
				TransEntry->SetStringField(TEXT("trigger"), EnumToName(T.Trigger));
				TransEntry->SetStringField(TEXT("priority"), EnumToName(T.Priority));
				TransEntry->SetStringField(TEXT("event_tag"),
					T.RequiredEvent.Tag.IsValid() ? T.RequiredEvent.Tag.GetTagName().ToString() : TEXT(""));
				TransEntry->SetNumberField(TEXT("condition_count"), T.Conditions.Num());
				TransEntry->SetBoolField(TEXT("delay"), T.bDelayTransition);
				TransEntry->SetNumberField(TEXT("delay_duration"), T.DelayDuration);
				TransEntry->SetBoolField(TEXT("enabled"), T.bTransitionEnabled);
				TransJson.Add(MakeShared<FJsonValueObject>(TransEntry));
			}
			for (int32 i = S->Children.Num() - 1; i >= 0; --i)
			{
				Stack.Add(S->Children[i]);
			}
		}
	}

	// Parameters — public const getter returns FInstancedPropertyBag&. To
	// enumerate the descriptors, we need to fetch the underlying UPropertyBag
	// (the USTRUCT that holds the property layout) and call GetPropertyDescs
	// on THAT. Each FPropertyBagPropertyDesc has Name + ValueType +
	// ValueTypeObject (the optional class for object/class/struct types).
	TArray<TSharedPtr<FJsonValue>> ParamsJson;
	if (const UPropertyBag* BagStruct = ED->GetRootParametersPropertyBag().GetPropertyBagStruct())
	{
		for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Desc.Name.ToString());
			// Best-effort type name: prefer the value-type enum's name, then
			// fall back to the ValueTypeObject's class name for object types.
			FString TypeName = StaticEnum<EPropertyBagPropertyType>() ?
				StaticEnum<EPropertyBagPropertyType>()->GetNameStringByValue((int64)Desc.ValueType) :
				TEXT("Unknown");
			if (Desc.ValueTypeObject)
			{
				TypeName = Desc.ValueTypeObject->GetName();
			}
			Entry->SetStringField(TEXT("type"), TypeName);
			ParamsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("state_count"), TotalStates);
	Data->SetArrayField(TEXT("states"), StatesJson);
	Data->SetArrayField(TEXT("transitions"), TransJson);
	Data->SetNumberField(TEXT("transition_count"), TotalTransitions);
	Data->SetNumberField(TEXT("evaluator_count"), ED->Evaluators.Num());
	Data->SetNumberField(TEXT("global_task_count"), ED->GlobalTasks.Num());
	Data->SetNumberField(TEXT("binding_count"),
		ED->GetPropertyEditorBindings() ? ED->GetPropertyEditorBindings()->GetBindings().Num() : 0);
	Data->SetNumberField(TEXT("parameter_count"), ParamsJson.Num());
	Data->SetArrayField(TEXT("parameters"), ParamsJson);
	Data->SetStringField(TEXT("root_state"),
		ED->SubTrees.Num() > 0 ? ED->SubTrees[0]->Name.ToString() : TEXT(""));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Task / condition node authoring + state type/selection + compile
// ---------------------------------------------------------------------------

// MCP:add_state_task - Attach a task node (by struct name, resolved via reflection) to a state's Tasks list on a UStateTree.
//   @param tree_path - Asset path to the UStateTree
//   @param state_name [string!] - Name of the state to add the task to
//   @param task_struct - Task struct name or /Script path; must derive from StateTreeTaskBase (e.g. "StateTreeDebugTextTask")
//   @returns { tree_path, state_name, task_struct, task_count, added, saved }
TSharedPtr<FJsonObject> HandleAddStateTask(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TaskStruct;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("task_struct"), TaskStruct)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	UScriptStruct* TaskType = StateTree_ResolveNodeStruct(TaskStruct, FStateTreeTaskBase::StaticStruct(),
		FString::Printf(TEXT("Task struct not found: %s"), *TaskStruct), Err);
	if (!TaskType) { return Err; }

	// AddDefaulted then initialize as the resolved struct (the editor's add-node path).
	FStateTreeEditorNode& Node = State->Tasks.AddDefaulted_GetRef();
	Node.InitializeAs(ED, TaskType);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("task_struct"), TaskType->GetName());
	Data->SetNumberField(TEXT("task_count"), State->Tasks.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state_task"));
}

// MCP:add_state_enter_condition - Attach an enter-condition node (by struct name, resolved via reflection) to a state's EnterConditions on a UStateTree.
//   @param tree_path - Asset path to the UStateTree
//   @param state_name [string!] - Name of the state to add the enter condition to
//   @param condition_struct - Condition struct name or /Script path; must derive from StateTreeConditionBase (e.g. "StateTreeRandomCondition")
//   @returns { tree_path, state_name, condition_struct, enter_condition_count, added, saved }
TSharedPtr<FJsonObject> HandleAddStateEnterCondition(
	const TSharedPtr<FJsonObject>& Params)
{
	FString CondStruct;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("condition_struct"), CondStruct)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	UScriptStruct* CondType = StateTree_ResolveNodeStruct(CondStruct, FStateTreeConditionBase::StaticStruct(),
		FString::Printf(TEXT("Condition struct not found: %s"), *CondStruct), Err);
	if (!CondType) { return Err; }

	FStateTreeEditorNode& Node = State->EnterConditions.AddDefaulted_GetRef();
	Node.InitializeAs(ED, CondType);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("condition_struct"), CondType->GetName());
	Data->SetNumberField(TEXT("enter_condition_count"), State->EnterConditions.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state_enter_condition"));
}

// MCP:set_state_type - Set the type of a StateTree state (EStateTreeStateType: State, Group, Linked, LinkedAsset or Subtree) and save the asset without recompiling it. An unknown name is an error. To point a state at another tree prefer set_state_linked_asset, which also sets the linked asset.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state to change; the command fails without it.
//   @param state_type - Enum value name: State, Group, Linked, LinkedAsset or Subtree (an EStateTreeStateType:: prefix is accepted).
//   @returns { tree_path, state_name, state_type, set, saved }
TSharedPtr<FJsonObject> HandleSetStateType(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TypeStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_type"), TypeStr)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	EStateTreeStateType NewType = EStateTreeStateType::State;
	if (!ResolveEnumByName(TypeStr, NewType))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown state_type: %s (expected State|Group|Linked|LinkedAsset|Subtree)"), *TypeStr));
	}
	State->Type = NewType;
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("state_type"), EnumToName(State->Type));
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_state_type"));
}

// MCP:set_state_selection_behavior - Set a state's selection behavior (EStateTreeStateSelectionBehavior) on a UStateTree.
//   @param tree_path - Asset path to the UStateTree
//   @param state_name [string!] - Name of the state to modify
//   @param selection_behavior - One of None | TryEnterState | TrySelectChildrenInOrder | TrySelectChildrenAtRandom | ...
//   @returns { tree_path, state_name, selection_behavior, set, saved }
TSharedPtr<FJsonObject> HandleSetStateSelectionBehavior(
	const TSharedPtr<FJsonObject>& Params)
{
	FString BehaviorStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("selection_behavior"), BehaviorStr)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	EStateTreeStateSelectionBehavior NewBehavior = EStateTreeStateSelectionBehavior::TrySelectChildrenInOrder;
	if (!ResolveEnumByName(BehaviorStr, NewBehavior))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown selection_behavior: %s"), *BehaviorStr));
	}
	State->SelectionBehavior = NewBehavior;
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("selection_behavior"), EnumToName(State->SelectionBehavior));
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_state_selection_behavior"));
}

// MCP:compile_state_tree - Compile a UStateTree via the editor subsystem (validate + compile); reports success and error/warning counts.
//   @param tree_path - Asset path to the UStateTree
//   @returns { tree_path, compiled, error_count, warning_count, message_count, saved }
TSharedPtr<FJsonObject> HandleCompileStateTree(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	if (!StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err))
	{
		return Err;
	}

	// Validate (schema fix-up safety net) then compile, mirroring the editor flow.
	UStateTreeEditingSubsystem::ValidateStateTree(Tree);
	FStateTreeCompilerLog Log;
	const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(Tree, Log);

	int32 Errors = 0, Warnings = 0;
	const TArray<TSharedRef<FTokenizedMessage>> Msgs = Log.ToTokenizedMessages();
	for (const TSharedRef<FTokenizedMessage>& M : Msgs)
	{
		switch (M->GetSeverity())
		{
			case EMessageSeverity::Error:              ++Errors; break;
			case EMessageSeverity::PerformanceWarning:
			case EMessageSeverity::Warning:            ++Warnings; break;
			default: break;
		}
	}
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetBoolField(TEXT("compiled"), bCompiled);
	Data->SetNumberField(TEXT("error_count"), Errors);
	Data->SetNumberField(TEXT("warning_count"), Warnings);
	Data->SetNumberField(TEXT("message_count"), Msgs.Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("compile_state_tree"));
}

// ---------------------------------------------------------------------------
// Transition conditions + global tasks + transition properties
// ---------------------------------------------------------------------------

// MCP:add_transition_condition - Attach a condition node (by struct name, via reflection) to a transition on a state of a UStateTree.
//   @param tree_path - Asset path to the UStateTree
//   @param state_name [string!] - Name of the state owning the transition
//   @param transition_index - Index of the transition within the state (from get_state_tree_info)
//   @param condition_struct - Condition struct name or /Script path; must derive from StateTreeConditionBase (e.g. "StateTreeRandomCondition")
//   @returns { tree_path, state_name, transition_index, condition_struct, condition_count, added, saved }
TSharedPtr<FJsonObject> HandleAddTransitionCondition(
	const TSharedPtr<FJsonObject>& Params)
{
	FString CondStruct;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("condition_struct"), CondStruct)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }
	int32 Ti = -1;
	{ double D = -1.0; if (Params->TryGetNumberField(TEXT("transition_index"), D)) { Ti = static_cast<int32>(D); } }
	if (!State->Transitions.IsValidIndex(Ti))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Invalid transition_index %d (state has %d transitions)"), Ti, State->Transitions.Num()));
	}
	UScriptStruct* CondType = StateTree_ResolveNodeStruct(CondStruct, FStateTreeConditionBase::StaticStruct(),
		FString::Printf(TEXT("Condition struct not found: %s"), *CondStruct), Err);
	if (!CondType) { return Err; }

	FStateTreeTransition& T = State->Transitions[Ti];
	FStateTreeEditorNode& Node = T.Conditions.AddDefaulted_GetRef();
	Node.InitializeAs(ED, CondType);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("transition_index"), Ti);
	Data->SetStringField(TEXT("condition_struct"), CondType->GetName());
	Data->SetNumberField(TEXT("condition_count"), T.Conditions.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_transition_condition"));
}

// MCP:add_state_tree_global_task - Attach a global task node (by struct name, via reflection) to a UStateTree; global tasks run for the whole tree's lifetime.
//   @param tree_path - Asset path to the UStateTree
//   @param task_struct - Task struct name or /Script path; must derive from StateTreeTaskBase (e.g. "StateTreeDebugTextTask")
//   @returns { tree_path, task_struct, global_task_count, added, saved }
TSharedPtr<FJsonObject> HandleAddStateTreeGlobalTask(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, TaskStruct;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("task_struct"), TaskStruct)) { return Missing; }
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
	if (!ED) { return Err; }
	UScriptStruct* TaskType = StateTree_ResolveNodeStruct(TaskStruct, FStateTreeTaskBase::StaticStruct(),
		FString::Printf(TEXT("Task struct not found: %s"), *TaskStruct), Err);
	if (!TaskType) { return Err; }

	FStateTreeEditorNode& Node = ED->GlobalTasks.AddDefaulted_GetRef();
	Node.InitializeAs(ED, TaskType);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("task_struct"), TaskType->GetName());
	Data->SetNumberField(TEXT("global_task_count"), ED->GlobalTasks.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state_tree_global_task"));
}

// MCP:set_transition_properties - Set properties (trigger, priority, delay, enabled) on a transition of a state in a UStateTree.
//   @param tree_path - Asset path to the UStateTree
//   @param state_name [string!] - Name of the state owning the transition
//   @param transition_index - Index of the transition within the state
//   @param trigger - Optional EStateTreeTransitionTrigger name (e.g. OnStateCompleted, OnTick)
//   @param priority - Optional EStateTreeTransitionPriority name (e.g. Low, Normal, High)
//   @param enabled - Optional whether the transition is enabled
//   @param delay - Optional whether to delay the transition
//   @param delay_duration - Optional delay duration in seconds (requires delay=true to take effect)
//   @returns { tree_path, state_name, transition_index, trigger, priority, enabled, delay, delay_duration, saved }
TSharedPtr<FJsonObject> HandleSetTransitionProperties(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }
	int32 Ti = -1;
	{ double D = -1.0; if (Params->TryGetNumberField(TEXT("transition_index"), D)) { Ti = static_cast<int32>(D); } }
	if (!State->Transitions.IsValidIndex(Ti))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Invalid transition_index %d (state has %d transitions)"), Ti, State->Transitions.Num()));
	}
	FStateTreeTransition& T = State->Transitions[Ti];

	FString TriggerStr;
	if (Params->TryGetStringField(TEXT("trigger"), TriggerStr) && !TriggerStr.IsEmpty())
	{
		EStateTreeTransitionTrigger Trig = EStateTreeTransitionTrigger::OnStateCompleted;
		if (!ResolveEnumByName(TriggerStr, Trig))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Unknown trigger: %s"), *TriggerStr));
		}
		T.Trigger = Trig;
	}
	FString PriorityStr;
	if (Params->TryGetStringField(TEXT("priority"), PriorityStr) && !PriorityStr.IsEmpty())
	{
		EStateTreeTransitionPriority Prio = EStateTreeTransitionPriority::Normal;
		if (!ResolveEnumByName(PriorityStr, Prio))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Unknown priority: %s"), *PriorityStr));
		}
		T.Priority = Prio;
	}
	bool bEnabled = true;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled)) { T.bTransitionEnabled = bEnabled; }
	bool bDelay = false;
	if (Params->TryGetBoolField(TEXT("delay"), bDelay)) { T.bDelayTransition = bDelay; }
	double DelayDur = 0.0;
	if (Params->TryGetNumberField(TEXT("delay_duration"), DelayDur)) { T.DelayDuration = static_cast<float>(DelayDur); }
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("transition_index"), Ti);
	Data->SetStringField(TEXT("trigger"), EnumToName(T.Trigger));
	Data->SetStringField(TEXT("priority"), EnumToName(T.Priority));
	Data->SetBoolField(TEXT("enabled"), T.bTransitionEnabled);
	Data->SetBoolField(TEXT("delay"), T.bDelayTransition);
	Data->SetNumberField(TEXT("delay_duration"), T.DelayDuration);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_transition_properties"));
}

// ---------------------------------------------------------------------------
// add_state_tree_binding
// ---------------------------------------------------------------------------
// MCP:add_state_tree_binding - Bind a UStateTree root parameter (source) to a target node's input property (target), so the node reads the parameter at runtime.  The target defaults to a state Task, but target_kind selects any FStateTreeEditorNode list: enter_condition/consideration (on the state) or global_task/evaluator (on the tree) — the binding anchor (FStateTreeEditorNode::ID) is identical for every kind. Authors an FStateTreePropertyPathBinding in the editor data's property bindings (idempotent per target path — re-binding the same target replaces it); validate it resolves with compile_state_tree.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name - Name of the state owning the target node (required for target_kind task|enter_condition|consideration; ignored for global_task|evaluator, which live on the tree).
//   @param target_kind - Which node list holds the target: "task" (default) | "enter_condition" | "consideration" | "global_task" | "evaluator".
//   @param target_index - Optional index into the selected node list (default 0).
//   @param target_property - Name of the target node's input property to bind (the binding target), e.g. "BindableText".
//   @param source_parameter - Name of the root parameter to bind from (the binding source; add it with add_tree_parameter first).
//   @returns { tree_path: string, state_name: string?, target_kind: string, target_index: int, target_property: string, source_parameter: string, source_struct_id: string, target_struct_id: string, bound: bool, binding_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleAddStateTreeBinding(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TargetProperty, SourceParameter;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_property"), TargetProperty)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_parameter"), SourceParameter)) { return Missing; }

	// The target defaults to a state Task, but target_kind selects any
	// FStateTreeEditorNode list — state-scoped (enter_condition/consideration) or tree-scoped
	// (global_task/evaluator). The binding anchor (FStateTreeEditorNode::ID) is identical for all.
	FString TargetKind = TEXT("task");
	Params->TryGetStringField(TEXT("target_kind"), TargetKind);
	TargetKind = TargetKind.TrimStartAndEnd().ToLower();

	int32 TargetIndex = 0;
	{ double D = 0.0; if (Params->TryGetNumberField(TEXT("target_index"), D)) { TargetIndex = static_cast<int32>(D); } }

	const bool bStateScoped =
		(TargetKind == TEXT("task") || TargetKind == TEXT("enter_condition") || TargetKind == TEXT("consideration"));
	const bool bTreeScoped =
		(TargetKind == TEXT("global_task") || TargetKind == TEXT("evaluator"));

	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; UStateTreeState* State = nullptr;
	TSharedPtr<FJsonObject> Err;
	if (bStateScoped)
	{
		State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
		if (!State) { return Err; }
	}
	else if (bTreeScoped)
	{
		// Tree-scoped: no owning state — load tree + editor data directly.
		ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
		if (!ED) { return Err; }
	}
	else
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Unsupported target_kind '%s' (use task|enter_condition|consideration|global_task|evaluator)"), *TargetKind));
	}

	// Select the FStateTreeEditorNode list the target lives in.
	TArray<FStateTreeEditorNode>* Nodes = nullptr;
	const TCHAR* NodeListLabel = TEXT("");
	if (TargetKind == TEXT("task"))                 { Nodes = &State->Tasks;           NodeListLabel = TEXT("Tasks"); }
	else if (TargetKind == TEXT("enter_condition")) { Nodes = &State->EnterConditions; NodeListLabel = TEXT("EnterConditions"); }
	else if (TargetKind == TEXT("consideration"))   { Nodes = &State->Considerations;  NodeListLabel = TEXT("Considerations"); }
	else if (TargetKind == TEXT("global_task"))     { Nodes = &ED->GlobalTasks;        NodeListLabel = TEXT("GlobalTasks"); }
	else /* evaluator */                            { Nodes = &ED->Evaluators;         NodeListLabel = TEXT("Evaluators"); }

	if (!Nodes->IsValidIndex(TargetIndex))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Invalid target_index %d (%s list has %d nodes)"),
			TargetIndex, NodeListLabel, Nodes->Num()));
	}

	const FStateTreeEditorNode& TargetNode = (*Nodes)[TargetIndex];
	const FGuid TargetID = TargetNode.ID;
	if (!TargetID.IsValid())
	{
		return mcp_common::MakeErrorReply(TEXT("Target node has no valid binding ID"));
	}

	// Source = the tree root parameters; validate the named parameter exists (else the binding won't resolve).
	const FGuid SourceID = ED->GetRootParametersGuid();
	if (!StateTree_FindRootParameter(ED, FName(*SourceParameter)))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Root parameter '%s' not found; add it with add_tree_parameter first"), *SourceParameter));
	}

	FStateTreeEditorPropertyBindings* Bindings = ED->GetPropertyEditorBindings();
	if (!Bindings)
	{
		return mcp_common::MakeErrorReply(TEXT("State tree editor data has no property bindings"));
	}

	// Build the source/target property paths (same construction the engine uses internally).
	FPropertyBindingPath SourcePath;
	SourcePath.FromString(SourceParameter);
	SourcePath.SetStructID(SourceID);
	FPropertyBindingPath TargetPath;
	TargetPath.FromString(TargetProperty);
	TargetPath.SetStructID(TargetID);

	// AddStateTreeBinding removes any existing binding on the target path first -> idempotent.
	Bindings->AddStateTreeBinding(FStateTreePropertyPathBinding(SourcePath, TargetPath, /*bInIsOutputBinding=*/false));

	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	// Read back: confirm a binding for this target struct + property exists in the collection.
	bool bBound = false;
	for (const FStateTreePropertyPathBinding& B : Bindings->GetBindings())
	{
		if (B.GetTargetPath().GetStructID() == TargetID && B.GetTargetPath().ToString() == TargetProperty)
		{
			bBound = true;
			break;
		}
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	if (State) { Data->SetStringField(TEXT("state_name"), State->Name.ToString()); }
	Data->SetStringField(TEXT("target_kind"), TargetKind);
	Data->SetNumberField(TEXT("target_index"), TargetIndex);
	Data->SetStringField(TEXT("target_property"), TargetProperty);
	Data->SetStringField(TEXT("source_parameter"), SourceParameter);
	Data->SetStringField(TEXT("source_struct_id"), SourceID.ToString());
	Data->SetStringField(TEXT("target_struct_id"), TargetID.ToString());
	Data->SetBoolField(TEXT("bound"), bBound);
	Data->SetNumberField(TEXT("binding_count"), Bindings->GetBindings().Num());
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state_tree_binding"));
}

// ---------------------------------------------------------------------------
// State completion aggregation + linked-asset states + evaluators
// ---------------------------------------------------------------------------

// MCP:set_state_completion_type - Set how a state aggregates its tasks' completion (UStateTreeState::TasksCompletion).  NOTE: this is the tasks-aggregation policy EStateTreeTaskCompletionType {All, Any} (All = the state completes when ALL its tasks complete; Any = when ANY single task completes) — NOT a success/failed result.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state to modify.
//   @param completion_type - Aggregation policy: "All" (default in engine: Any) | "Any" (case-insensitive).
//   @returns { tree_path, state_name, completion_type, set, saved }
TSharedPtr<FJsonObject> HandleSetStateCompletionType(
	const TSharedPtr<FJsonObject>& Params)
{
	FString CompletionStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("completion_type"), CompletionStr)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	// EStateTreeTaskCompletionType is a reflected UENUM {All, Any}; resolve by name
	// (case-insensitive) so the caller can pass "all"/"any" or "All"/"Any".
	EStateTreeTaskCompletionType NewValue = EStateTreeTaskCompletionType::Any;
	if (!ResolveEnumByName(CompletionStr, NewValue))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Unknown completion_type: %s (expected All|Any)"), *CompletionStr));
	}

	State->Modify();
	State->TasksCompletion = NewValue;
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("completion_type"), EnumToName(State->TasksCompletion));
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_state_completion_type"));
}

// MCP:set_state_linked_asset - Turn a state into a LinkedAsset state pointing at another UStateTree (UStateTreeState::SetLinkedStateAsset).  This forces the state's Type to EStateTreeStateType::LinkedAsset (the engine setter asserts on Type) and the setter clears Tasks, fixes the parameter layout from the linked subtree, and sets SelectionBehavior=TryEnterState — so a linked-asset state runs the target tree instead of local tasks.
//   @param tree_path - Asset path to the owning UStateTree.
//   @param state_name [string!] - Name of the state to convert into a LinkedAsset state.
//   @param linked_asset_path - Asset path to the target UStateTree to link (must exist).
//   @returns { tree_path, state_name, state_type, linked_asset, set, saved }
TSharedPtr<FJsonObject> HandleSetStateLinkedAsset(
	const TSharedPtr<FJsonObject>& Params)
{
	FString LinkedAssetPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("linked_asset_path"), LinkedAssetPath)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	// Resolve the link target first — an honest error beats a half-applied state.
	UStateTree* Target = LoadObject<UStateTree>(nullptr, *LinkedAssetPath);
	if (!Target)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Cannot load linked target state tree: %s"), *LinkedAssetPath));
	}

	State->Modify();
	// SetLinkedStateAsset begins with check(Type == EStateTreeStateType::LinkedAsset),
	// so the type MUST be set first or the engine asserts (crash). The setter does the
	// rest of the work (assign LinkedAsset, reset Tasks, fix parameter layout, mark dirty).
	State->Type = EStateTreeStateType::LinkedAsset;
	State->SetLinkedStateAsset(Target);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("state_type"), EnumToName(State->Type));
	Data->SetStringField(TEXT("linked_asset"),
		State->LinkedAsset ? State->LinkedAsset->GetPathName() : TEXT(""));
	Data->SetBoolField(TEXT("set"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_state_linked_asset"));
}

// MCP:add_state_evaluator - Attach an evaluator node (by struct name, via reflection) to a UStateTree; evaluators are TREE-scoped (they live on UStateTreeEditorData::Evaluators, not on a state) and tick for the whole tree, so this command takes no state_name.  Idempotent: an evaluator of the same struct already present is not appended again.
//   @param tree_path - Asset path to the UStateTree.
//   @param evaluator_class - Evaluator struct name or /Script path; must derive from StateTreeEvaluatorBase (e.g. "TestEval_A" from the StateTree test suite, or any project/plugin evaluator struct).
//   @returns { tree_path, evaluator_class, evaluator_count, already_existed, added, saved }
TSharedPtr<FJsonObject> HandleAddStateEvaluator(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, EvaluatorClass;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("evaluator_class"), EvaluatorClass)) { return Missing; }
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData; call create_state_tree first"), Tree, Err);
	if (!ED) { return Err; }
	UScriptStruct* EvalType = StateTree_ResolveNodeStruct(EvaluatorClass, FStateTreeEvaluatorBase::StaticStruct(),
		FString::Printf(TEXT("Evaluator struct not found: %s"), *EvaluatorClass), Err);
	if (!EvalType) { return Err; }

	// Idempotency: evaluators are appended (no dedup in the engine), so scan the
	// existing tree-scoped list for the same struct before adding another.
	for (const FStateTreeEditorNode& Existing : ED->Evaluators)
	{
		if (Existing.Node.GetScriptStruct() == EvalType)
		{
			TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
			Data->SetStringField(TEXT("evaluator_class"), EvalType->GetName());
			Data->SetNumberField(TEXT("evaluator_count"), ED->Evaluators.Num());
			Data->SetBoolField(TEXT("already_existed"), true);
			Data->SetBoolField(TEXT("added"), false);
			return mcp_common::MakeOkReply(Data);
		}
	}

	// Tree-scoped append, mirroring add_state_tree_global_task but on Evaluators.
	// Outer is the editor data (ED), not a state.
	FStateTreeEditorNode& Node = ED->Evaluators.AddDefaulted_GetRef();
	Node.InitializeAs(ED, EvalType);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("evaluator_class"), EvalType->GetName());
	Data->SetNumberField(TEXT("evaluator_count"), ED->Evaluators.Num());
	Data->SetBoolField(TEXT("already_existed"), false);
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_state_evaluator"));
}
// =========================================================================
// StateTree read / list / remove
//
// The read-side and destructive-side counterparts of the authoring commands above.
// Idempotent, honest about headless limits, one canonical path per operation.
//
// Engine source:
//   - UStateTreeEditorData: Evaluators, GlobalTasks, SubTrees
//     (StateTreeEditorData.h)
//   - UStateTreeState: EnterConditions, Tasks, Considerations, Transitions,
//     Children (StateTreeState.h)
//   - FStateTreeTransition.Conditions (StateTreeState.h)
//   - delete via UEditorAssetLibrary::DeleteAsset (pattern used across
//     TimelineCommands.cpp, MaterialExtraCommands.cpp, etc.)
//
// All list_* are pure reads (idempotent, headless-clean). All remove_*
// mutate EditorData (via Modify() + RemoveAt) + SaveAssetPackage, and are
// idempotent at the index-out-of-range boundary (return removed:false,
// index:X, was_present:false) so re-runs are no-ops.

// ---------------------------------------------------------------------------
// list_state_trees — enumerate UStateTree assets under a content path
// ---------------------------------------------------------------------------
// MCP:list_state_trees - List all UStateTree assets under a content path (default /Game).
//   @param package_path - Content path to filter on (default "/Game").
//   @returns { package_path, count, trees: [{ path, name, class }] }
TSharedPtr<FJsonObject> HandleListStateTrees(
	const TSharedPtr<FJsonObject>& Params)
{
	FString Root = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), Root);

	// Every UStateTree asset (subclasses included) anywhere below Root.
	FARFilter TreeQuery;
	TreeQuery.PackagePaths.Add(FName(*Root));
	TreeQuery.ClassPaths.Add(UStateTree::StaticClass()->GetClassPathName());
	TreeQuery.bRecursivePaths = true;
	TreeQuery.bRecursiveClasses = true;

	IAssetRegistry& Registry = mcp_common::AssetRegistry();
	TArray<FAssetData> Found;
	Registry.GetAssets(TreeQuery, Found);

	TArray<TSharedPtr<FJsonValue>> Trees;
	Trees.Reserve(Found.Num());
	for (const FAssetData& Asset : Found)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), Asset.GetObjectPathString());
		Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
		Entry->SetStringField(TEXT("class"), Asset.AssetClassPath.ToString());
		Trees.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("package_path"), Root);
	Data->SetNumberField(TEXT("count"), Found.Num());
	Data->SetArrayField(TEXT("trees"), Trees);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_states — recursive walk of the state tree hierarchy
// ---------------------------------------------------------------------------
// MCP:list_states - Walk the state hierarchy of a UStateTree, returning a flat list with parent/child/sibling info plus a recursive child_count.
//   @param tree_path - Asset path to the UStateTree.
//   @param root_state - Optional. If set, walk from this state as root (top-level SubTrees are skipped).
//   @returns { tree_path, count, states: [{ name, type, completion, has_children, child_count, depth, parent }] }
TSharedPtr<FJsonObject> HandleListStates(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* StartFrom = ResolveTreeAndState(Params, TEXT("root_state"), Tree, ED, Err);

	// ResolveTreeAndState fails on missing root_state — fall back to walking all SubTrees if so.
	const bool bStartFromSupplied = Params->HasField(TEXT("root_state"))
		&& !Params->GetStringField(TEXT("root_state")).IsEmpty();
	if (bStartFromSupplied && !StartFrom)
	{
		return Err;
	}
	if (!Tree)
	{
		// No root_state param either — try to load the tree alone.
		ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
		if (!ED) { return Err; }
	}

	// Build the list iteratively to keep the walk bounded and depth-tracked.
	TArray<TSharedPtr<FJsonValue>> StatesJson;
	int32 TotalCount = 0;

	struct FQueueItem { UStateTreeState* State; FString ParentName; int32 Depth; };
	TArray<FQueueItem> Queue;

	if (StartFrom)
	{
		Queue.Add({StartFrom, FString(), 0});
	}
	else if (ED)
	{
		for (UStateTreeState* SubTree : ED->SubTrees)
		{
			if (SubTree) { Queue.Add({SubTree, FString(), 0}); }
		}
	}

	while (Queue.Num() > 0)
	{
		FQueueItem Item = Queue[0];
		Queue.RemoveAt(0, 1, EAllowShrinking::No);

		if (!Item.State) { continue; }

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"),         Item.State->Name.ToString());
		Entry->SetStringField(TEXT("type"),         EnumToName(Item.State->Type));
		Entry->SetStringField(TEXT("completion"),   EnumToName(Item.State->TasksCompletion));
		Entry->SetBoolField  (TEXT("has_children"), Item.State->Children.Num() > 0);
		Entry->SetNumberField(TEXT("child_count"),  Item.State->Children.Num());
		Entry->SetNumberField(TEXT("task_count"),   Item.State->Tasks.Num());
		Entry->SetNumberField(TEXT("enter_condition_count"), Item.State->EnterConditions.Num());
		Entry->SetNumberField(TEXT("transition_count"),      Item.State->Transitions.Num());
		Entry->SetNumberField(TEXT("depth"),        Item.Depth);
		Entry->SetStringField(TEXT("parent"),       Item.ParentName);
		StatesJson.Add(MakeShared<FJsonValueObject>(Entry));
		TotalCount++;

		for (UStateTreeState* Child : Item.State->Children)
		{
			if (Child) { Queue.Add({Child, Item.State->Name.ToString(), Item.Depth + 1}); }
		}
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("count"),     TotalCount);
	Data->SetArrayField (TEXT("states"),    StatesJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_transitions — transitions of a single state
// ---------------------------------------------------------------------------
// MCP:list_transitions - List all transitions of a state, with their target name, trigger event, conditions count, delay, and priority.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @returns { tree_path, state_name, count, transitions: [{ index, target, trigger, condition_count, delay, priority }] }
TSharedPtr<FJsonObject> HandleListTransitions(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

TArray<TSharedPtr<FJsonValue>> TransitionsJson;
	TransitionsJson.Reserve(State->Transitions.Num());
	for (int32 i = 0; i < State->Transitions.Num(); ++i)
	{
		const FStateTreeTransition& T = State->Transitions[i];
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("index"),           i);
		// FStateTreeStateLink is a struct (not a pointer): use `.Name` not `->Name`.
		// The Name field is the state name at link time; resolves via ID at compile.
		Entry->SetStringField(TEXT("target"),          T.State.Name.ToString());
		// FStateTreeTransition has RequiredEvent (FStateTreeEventDesc) — not Event directly.
		// FStateTreeEventDesc's identifying member is FGameplayTag Tag; use Tag.ToString().
		Entry->SetStringField(TEXT("required_event_tag"), T.RequiredEvent.Tag.ToString());
		Entry->SetNumberField(TEXT("condition_count"), T.Conditions.Num());
		// Delay is `DelayDuration` + the `bDelayTransition` gate; expose both so the
		// caller can distinguish "delay disabled" from "delay=0".
		Entry->SetBoolField  (TEXT("delay_enabled"),   T.bDelayTransition);
		Entry->SetNumberField(TEXT("delay_duration"),  T.DelayDuration);
		Entry->SetNumberField(TEXT("priority"),        static_cast<int32>(T.Priority));
		TransitionsJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("count"),       State->Transitions.Num());
	Data->SetArrayField (TEXT("transitions"), TransitionsJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_tasks — tasks of a single state
// ---------------------------------------------------------------------------
// MCP:list_tasks - List all tasks of a state, with their struct class name, instance ID, and per-task condition count.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @returns { tree_path, state_name, count, tasks: [{ index, class, instance_id, condition_count }] }
TSharedPtr<FJsonObject> HandleListTasks(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("count"),      State->Tasks.Num());
	Data->SetArrayField (TEXT("tasks"),      StateTree_NodeListToJson(State->Tasks));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_evaluators — tree-scoped evaluators
// ---------------------------------------------------------------------------
// MCP:list_evaluators - List all evaluators on the tree (UStateTreeEditorData::Evaluators — tree-scoped).
//   @param tree_path - Asset path to the UStateTree.
//   @returns { tree_path, count, evaluators: [{ index, class, instance_id }] }
TSharedPtr<FJsonObject> HandleListEvaluators(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("count"),      ED->Evaluators.Num());
	Data->SetArrayField (TEXT("evaluators"), StateTree_NodeListToJson(ED->Evaluators));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_conditions — enter conditions + transition conditions
// ---------------------------------------------------------------------------
// MCP:list_conditions - List the enter conditions of a state. (Transition-level conditions are exposed via list_transitions' condition_count; a full per-transition dump is not available yet.)
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @returns { tree_path, state_name, count, conditions: [{ index, class, instance_id }] }
TSharedPtr<FJsonObject> HandleListConditions(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("count"),      State->EnterConditions.Num());
	Data->SetArrayField (TEXT("conditions"), StateTree_NodeListToJson(State->EnterConditions));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_global_tasks — tree-scoped global tasks
// ---------------------------------------------------------------------------
// MCP:list_global_tasks - List the global tasks of a StateTree asset (tasks that run for the whole tree, not one state) without changing it. Returns count and global_tasks [{index, class, instance_id}]; index is what add_state_tree_global_task_property and remove_state_tree_global_task take.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @returns { tree_path, count, global_tasks: [{ index, class, instance_id }] }
TSharedPtr<FJsonObject> HandleListGlobalTasks(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("count"),        ED->GlobalTasks.Num());
	Data->SetArrayField (TEXT("global_tasks"), StateTree_NodeListToJson(ED->GlobalTasks));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_tree_bindings — parameter bindings on this tree
// ---------------------------------------------------------------------------
// MCP:list_tree_bindings - List the property bindings stored in the editor data of a StateTree asset, without changing it. Returns count and bindings [{index, source_path, target_path, source_id, target_id}], where source_id and target_id are the struct GUIDs of the two ends. Add bindings with add_state_tree_binding and remove them with remove_tree_binding.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @returns { tree_path, count, bindings: [{ index, source_path, target_path, source_id, target_id }] }
TSharedPtr<FJsonObject> HandleListTreeBindings(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	FStateTreeEditorPropertyBindings* Bindings = ED->GetPropertyEditorBindings();

	TArray<TSharedPtr<FJsonValue>> BindingsJson;
	int32 BindingCount = 0;
	if (Bindings)
	{
		// GetBindings() returns TArrayView, not TArray — iterate directly.
		const TArrayView<const FStateTreePropertyPathBinding> All = Bindings->GetBindings();
		BindingCount = All.Num();
		BindingsJson.Reserve(All.Num());
		for (int32 i = 0; i < All.Num(); ++i)
		{
			const FStateTreePropertyPathBinding& B = All[i];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"),       i);
			Entry->SetStringField(TEXT("source_path"), B.GetSourcePath().ToString());
			Entry->SetStringField(TEXT("target_path"), B.GetTargetPath().ToString());
			Entry->SetStringField(TEXT("source_id"),   B.GetSourcePath().GetStructID().ToString());
			Entry->SetStringField(TEXT("target_id"),   B.GetTargetPath().GetStructID().ToString());
			BindingsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("count"),     BindingCount);
	Data->SetArrayField (TEXT("bindings"),  BindingsJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_tree_parameters — root parameter bag of the tree
// ---------------------------------------------------------------------------
// MCP:list_tree_parameters - List the root parameter bag (typed parameters declared on the tree).
//   @param tree_path - Asset path to the UStateTree.
//   @returns { tree_path, count, parameters: [{ name, type, value_type }] }
TSharedPtr<FJsonObject> HandleListTreeParameters(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	TArray<TSharedPtr<FJsonValue>> ParamsJson;
	const UPropertyBag* Bag = ED->GetRootParametersPropertyBag().GetPropertyBagStruct();
	if (Bag)
	{
		for (const FPropertyBagPropertyDesc& D : Bag->GetPropertyDescs())
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"),       D.Name.ToString());
			Entry->SetStringField(TEXT("type"),       EnumToName(D.ValueType));
			Entry->SetStringField(TEXT("value_type"), UEnum::GetValueAsString(D.ValueType));
			ParamsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("count"),      ParamsJson.Num());
	Data->SetArrayField (TEXT("parameters"), ParamsJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// delete_state_tree — full asset deletion via UEditorAssetLibrary
// ---------------------------------------------------------------------------
// MCP:delete_state_tree - Delete a UStateTree asset from the project. Idempotent — deleting a non-existent asset is a no-op (returns deleted:false, already_existed:false) so re-runs don't fail.
//   @param tree_path - Asset path to the UStateTree.
//   @param confirm - Must be true to actually delete. Honest-headless safety against accidental wipe.
//   @returns { tree_path, deleted, already_existed, note }
TSharedPtr<FJsonObject> HandleDeleteStateTree(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	if (!bConfirm)
	{
		return mcp_common::MakeErrorReply(
			TEXT("delete_state_tree is destructive; pass confirm=true to proceed"));
	}

	const bool bExisted = UEditorAssetLibrary::DoesAssetExist(TreePath);
	if (!bExisted)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("tree_path"),        TreePath);
		Data->SetBoolField  (TEXT("deleted"),          false);
		Data->SetBoolField  (TEXT("already_existed"),  false);
		Data->SetStringField(TEXT("note"),             TEXT("No asset at the given path; nothing to delete."));
		return mcp_common::MakeOkReply(Data);
	}

	const bool bDeleted = UEditorAssetLibrary::DeleteAsset(TreePath);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("tree_path"),        TreePath);
	Data->SetBoolField  (TEXT("deleted"),          bDeleted);
	Data->SetBoolField  (TEXT("already_existed"),  true);
	Data->SetStringField(TEXT("note"),             bDeleted ? TEXT("") : TEXT("UEditorAssetLibrary::DeleteAsset returned false."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_state — remove a named state (optionally with descendants)
// ---------------------------------------------------------------------------
// MCP:remove_state - Remove a state from the tree by name.  Idempotent: removing a non-existent state returns removed:false with a note.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name - Name of the state to remove.
//   @param with_descendants - When true (default), also removes all child states recursively.
//   @returns { tree_path, state_name, removed, with_descendants, remaining_top_level, note }
TSharedPtr<FJsonObject> HandleRemoveState(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, StateName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_name"), StateName)) { return Missing; }

	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	// Idempotency: if the named state isn't present, return removed:false
	// instead of an error so re-runs are no-ops. ResolveTreeAndState would surface
	// "State not found" as a hard error, which is the wrong shape for a destructive
	// idempotent command.
	UStateTreeState* State = FindStateByName(ED, FName(*StateName));
	if (!State)
	{
		TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
		Data->SetStringField(TEXT("state_name"),           StateName);
		Data->SetBoolField  (TEXT("removed"),              false);
		Data->SetBoolField  (TEXT("with_descendants"),     true); // default
		Data->SetNumberField(TEXT("remaining_top_level"),  ED->SubTrees.Num());
		Data->SetBoolField  (TEXT("saved"),                false);
		Data->SetStringField(TEXT("note"),                 TEXT("State was not present -- nothing to remove (idempotent no-op)."));
		return mcp_common::MakeOkReply(Data);
	}

	bool bWithDescendants = true;
	Params->TryGetBoolField(TEXT("with_descendants"), bWithDescendants);

	ED->Modify();

	// Find which parent owns this state (root SubTrees or a Children array).
	int32 RemovedCount = 0;
	auto RemoveFromChildren = [&](TArray<TObjectPtr<UStateTreeState>>& Arr) -> bool
	{
		const int32 idx = Arr.IndexOfByKey(State);
		if (idx == INDEX_NONE) { return false; }
		Arr.RemoveAt(idx, 1, EAllowShrinking::No);
		RemovedCount++;
		return true;
	};

	if (RemoveFromChildren(ED->SubTrees))
	{
		// Top-level — done.
	}
	else
	{
		// Recursive walk over the entire SubTrees tree to find the parent.
		TArray<TObjectPtr<UStateTreeState>>* Found = nullptr;
		int32 FoundIndex = INDEX_NONE;
		TFunction<void(TArray<TObjectPtr<UStateTreeState>>&)> Walk = [&](TArray<TObjectPtr<UStateTreeState>>& Arr)
		{
			for (int32 i = 0; i < Arr.Num() && !Found; ++i)
			{
				UStateTreeState* S = Arr[i];
				if (!S) { continue; }
				if (S == State) { Found = &Arr; FoundIndex = i; return; }
				Walk(S->Children);
			}
		};
		Walk(ED->SubTrees);
		if (Found && FoundIndex != INDEX_NONE)
		{
			Found->RemoveAt(FoundIndex, 1, EAllowShrinking::No);
			RemovedCount++;
		}
	}

	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetBoolField  (TEXT("removed"),           RemovedCount > 0);
	Data->SetBoolField  (TEXT("with_descendants"),  bWithDescendants);
	Data->SetNumberField(TEXT("remaining_top_level"), ED->SubTrees.Num());
	Data->SetStringField(TEXT("note"),               RemovedCount > 0 ? TEXT("") : TEXT("State was not present in any parent's Children array -- could not remove."));
	Data->SetBoolField  (TEXT("saved"),             bSaved);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_transition — remove a transition by index from a state's Transitions
// ---------------------------------------------------------------------------
// MCP:remove_transition - Remove a transition (by index) from a state. Idempotent at the index-out-of-range boundary.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param transition_index - 0-based index into the state's Transitions array.
//   @returns { tree_path, state_name, transition_index, removed, remaining, saved }
TSharedPtr<FJsonObject> HandleRemoveTransition(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("transition_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("transition_index is required (int)"));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("transition_index"), Index);
	StateTree_RemoveAtIndex(Data, Tree, State, State->Transitions, Index,
		TEXT("Index out of range -- nothing removed (idempotent no-op)."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_task — remove a task (by index) from a state's Tasks
// ---------------------------------------------------------------------------
// MCP:remove_task - Remove a task (by index) from a state. Idempotent at the index-out-of-range boundary.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param task_index - 0-based index into the state's Tasks array.
//   @returns { tree_path, state_name, task_index, removed, remaining, saved }
TSharedPtr<FJsonObject> HandleRemoveTask(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("task_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("task_index is required (int)"));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("task_index"),  Index);
	StateTree_RemoveAtIndex(Data, Tree, State, State->Tasks, Index,
		TEXT("Index out of range -- nothing removed (idempotent no-op)."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_evaluator — remove a tree-scoped evaluator (by index)
// ---------------------------------------------------------------------------
// MCP:remove_evaluator - Remove a tree-scoped evaluator (by index). Idempotent at the index-out-of-range boundary.
//   @param tree_path - Asset path to the UStateTree.
//   @param evaluator_index - 0-based index into the tree's Evaluators array.
//   @returns { tree_path, evaluator_index, removed, remaining, saved }
TSharedPtr<FJsonObject> HandleRemoveEvaluator(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("evaluator_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("evaluator_index is required (int)"));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("evaluator_index"), Index);
	StateTree_RemoveAtIndex(Data, Tree, ED, ED->Evaluators, Index,
		TEXT("Index out of range -- nothing removed (idempotent no-op)."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_state_tree_global_task — remove a tree-scoped global task (by index)
//   Inverse of add_state_tree_global_task. ED->GlobalTasks is a
//   TArray<FStateTreeEditorNode> (StateTreeEditorData.h), structurally
//   identical to ED->Evaluators — so this is an exact mirror of
//   remove_evaluator: RemoveAt is the symmetric inverse of the twin's
//   GlobalTasks.AddDefaulted_GetRef().
// ---------------------------------------------------------------------------
// MCP:remove_state_tree_global_task - Remove a tree-scoped global task (by index). Idempotent at the index-out-of-range boundary.
//   @param tree_path - Asset path to the UStateTree.
//   @param global_task_index - 0-based index into the tree's GlobalTasks array.
//   @returns { tree_path, global_task_index, removed, remaining, saved }
TSharedPtr<FJsonObject> HandleRemoveStateTreeGlobalTask(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("global_task_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("global_task_index is required (int)"));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("global_task_index"), Index);
	StateTree_RemoveAtIndex(Data, Tree, ED, ED->GlobalTasks, Index,
		TEXT("Index out of range -- nothing removed (idempotent no-op)."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_tree_parameter — remove a root parameter from the tree's parameter bag
//   Inverse of add_tree_parameter. The root parameters live in the
//   FInstancedPropertyBag RootParameterPropertyBag (StateTreeEditorData.h).
//   FInstancedPropertyBag::RemovePropertyByName (PropertyBag.h, UE_API)
//   rebuilds the desc list + MigrateToNewBagStruct itself (PropertyBag.cpp
//   -> 1947), so the caller only calls it and saves.
//   NOTE: RemovePropertiesByName always returns Success even for an absent name
//   (it no-ops), so we probe existence via GetPropertyBagStruct() first — the
//   same descriptor-resolution idiom as set_tree_parameter — to report an honest
//   `removed` flag and stay idempotent when the parameter is already absent.
//   Addressing is by name (mirrors add_tree_parameter's `parameter_name`).
// ---------------------------------------------------------------------------
// MCP:remove_tree_parameter - Remove a named parameter from a UStateTree's root parameter bag. Idempotent — removing an absent parameter is a no-op (removed:false).
//   @param tree_path - Asset path to the UStateTree.
//   @param parameter_name - Name of the root parameter to remove.
//   @returns { tree_path, parameter_name, removed, parameter_count, saved }
TSharedPtr<FJsonObject> HandleRemoveTreeParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName)) { return Missing; }
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	// Probe existence via the underlying UPropertyBag layout (same resolution
	// path as set_tree_parameter) — RemovePropertyByName returns Success even
	// for an absent name, so we must check first for an honest `removed` flag.
	const FName WantName(*ParamName);
	if (!StateTree_FindRootParameter(ED, WantName))
	{
		TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
		Data->SetStringField(TEXT("parameter_name"),  ParamName);
		Data->SetBoolField  (TEXT("removed"),         false);
		Data->SetNumberField(TEXT("parameter_count"), StateTree_RootParameterCount(ED));
		Data->SetBoolField  (TEXT("saved"),           false);
		Data->SetStringField(TEXT("note"),            TEXT("Parameter not found -- nothing removed (idempotent no-op)."));
		return mcp_common::MakeOkReply(Data);
	}

	// GetRootParametersPropertyBag() returns const& but the underlying bag is
	// mutable; the editor reflection pipeline was designed to mutate through
	// const access for editor-data convenience (same idiom as set_tree_parameter).
	ED->Modify();
	FInstancedPropertyBag& MutableBag = const_cast<FInstancedPropertyBag&>(ED->GetRootParametersPropertyBag());
	const EPropertyBagAlterationResult RemoveResult = MutableBag.RemovePropertyByName(WantName);
	if (RemoveResult != EPropertyBagAlterationResult::Success)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to remove parameter '%s' (EPropertyBagAlterationResult=%d)"),
				*ParamName, (int32)RemoveResult));
	}
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("parameter_name"),  ParamName);
	Data->SetBoolField  (TEXT("removed"),         true);
	Data->SetNumberField(TEXT("parameter_count"), StateTree_RootParameterCount(ED));
	Data->SetBoolField  (TEXT("saved"),           bSaved);
	return mcp_common::MakeOkReply(Data);
}
// =========================================================================
// StateTree task & condition introspection + property get/set
//
// Reflection-driven: list_*_types enumerates registered StateTree task/condition
// structs via TObjectIterator<UScriptStruct> so an agent can discover what's
// available without engine docs. get/set_*_property round-trip a single struct
// field via FJsonObjectConverter (the standard struct<->json path).
//
// These commands are headless-safe: editor-only asset reads + reflection + property
// edits + SaveAssetPackage. No PIE, no runtime evaluation.

// ---------------------------------------------------------------------------
// Helpers (anonymous namespace) — reflection utilities shared by the introspection and evaluator commands.
// ---------------------------------------------------------------------------
namespace
{
	/** Walk every loaded UScriptStruct and collect names of those that derive from
	 *  `Base` (excluding RF_Abstract so the agent only sees instantiable structs). */
	TArray<FString> ListDerivedStructNames(const UScriptStruct* Base)
	{
		TArray<FString> Out;
		if (!Base) { return Out; }
		for (TObjectIterator<UScriptStruct> It; It; ++It)
		{
			UScriptStruct* S = *It;
			if (!S) { continue; }
			// UScriptStruct doesn't expose ClassFlags like UClass does. Abstract-ness is
			// exposed via the "Abstract" UHT metadata tag.
			if (S->HasMetaData(TEXT("Abstract"))) { continue; }
			if (!S->IsChildOf(Base)) { continue; }
			Out.AddUnique(S->GetName());
		}
		Out.Sort();
		return Out;
	}

	/** Serialize a single FProperty value from `Memory` (struct instance memory)
	 *  into a FJsonValue via FJsonObjectConverter. Returns nullptr on mismatch. */
	TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Prop, const void* Memory)
	{
		if (!Prop || !Memory) { return nullptr; }
		const UStruct* OwnerStruct = Prop->GetOwnerStruct();
		if (!OwnerStruct) { return nullptr; }
		TSharedRef<FJsonObject> Box = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(OwnerStruct, const_cast<void*>(Memory), Box, 0, 0))
		{
			return nullptr;
		}
		// UStructToJsonObject serializes the WHOLE struct — pull out the single property the caller asked for.
		return Box->TryGetField(Prop->GetName());
	}

	/** Serialize a FJsonValue to a JSON FString for cheap equality comparison. */
	FString JsonValueToSerializedString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid()) { return FString(); }
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Value.ToSharedRef(), FString(), Writer);
		return Out;
	}

	/** The root parameter ParamName as JSON (the bag dumped through its layout BagStruct), or null. */
	TSharedPtr<FJsonValue> StateTree_RootParameterValue(const UStateTreeEditorData* ED, const UPropertyBag* BagStruct,
		const FString& ParamName)
	{
		TSharedRef<FJsonObject> Dump = MakeShared<FJsonObject>();
		if (BagStruct && FJsonObjectConverter::UStructToJsonObject(BagStruct,
			ED->GetRootParametersPropertyBag().GetValue().GetMemory(), Dump, 0, 0))
		{
			return Dump->TryGetField(ParamName);
		}
		return nullptr;
	}

	/** Apply a JSON value to a single FProperty on a struct instance by dumping the
	 *  whole struct to a JsonObject, replacing the targeted field, and re-applying
	 *  via JsonObjectToUStruct. JsonObjectToUStruct only overwrites fields present
	 *  in the source object, so other fields are preserved. This is the most
	 *  portable path — JsonValueToFProperty is not always exported. */
	bool ApplyJsonValueToProperty(FProperty* Prop, void* Memory, const TSharedPtr<FJsonValue>& Value)
	{
		if (!Prop || !Memory || !Value.IsValid()) { return false; }
		const UStruct* OwnerStruct = Prop->GetOwnerStruct();
		if (!OwnerStruct) { return false; }
		TSharedRef<FJsonObject> Box = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(OwnerStruct, Memory, Box, 0, 0))
		{
			return false;
		}
		Box->SetField(Prop->GetName(), Value);
		return FJsonObjectConverter::JsonObjectToUStruct(Box, OwnerStruct, Memory, 0, 0);
	}

	/** Resolve the FStateTreeEditorNode at a state-level slot (Tasks | EnterConditions | Considerations)
	 *  by index. Returns nullptr on OOR. */
	FStateTreeEditorNode* GetStateNodeByIndex(UStateTreeState* State,
		const TCHAR* SlotName, int32 Index)
	{
		if (!State) { return nullptr; }
		TArray<FStateTreeEditorNode>* Arr = nullptr;
		if (FCString::Strcmp(SlotName, TEXT("Tasks")) == 0)               { Arr = &State->Tasks; }
		else if (FCString::Strcmp(SlotName, TEXT("EnterConditions")) == 0) { Arr = &State->EnterConditions; }
		else if (FCString::Strcmp(SlotName, TEXT("Considerations")) == 0)  { Arr = &State->Considerations; }
		if (!Arr || !Arr->IsValidIndex(Index)) { return nullptr; }
		return &(*Arr)[Index];
	}

	/** GetStateNodeByIndex; on failure returns nullptr and fills OutError with OutOfRangeError. */
	FStateTreeEditorNode* StateTree_FindStateNode(UStateTreeState* State, const TCHAR* SlotName, int32 Index,
		const FString& OutOfRangeError, TSharedPtr<FJsonObject>& OutError)
	{
		FStateTreeEditorNode* Node = GetStateNodeByIndex(State, SlotName, Index);
		if (!Node)
		{
			OutError = mcp_common::MakeErrorReply(OutOfRangeError);
		}
		return Node;
	}

	/** Read a FJsonValue field from a TSharedPtr<FJsonObject> Params (params is a JsonObject).
	 *  FJsonObject::TryGetField is 1-arg and returns `const TSharedPtr<FJsonValue>*`
	 *  (nullptr if missing). Wrap that into a clean TSharedPtr return for callers. */
	TSharedPtr<FJsonValue> GetOptionalJsonValue(
		const TSharedPtr<FJsonObject>& Params, const FString& FieldName)
	{
		if (!Params.IsValid()) { return TSharedPtr<FJsonValue>(); }
		return Params->TryGetField(FieldName);
	}

	/** Read the required property and value params into OutPropName and OutValue; the error reply when one is missing. */
	TSharedPtr<FJsonObject> StateTree_ReadPropertyValue(const TSharedPtr<FJsonObject>& Params, FString& OutPropName,
		TSharedPtr<FJsonValue>& OutValue)
	{
		if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), OutPropName))
		{
			return Missing;
		}
		OutValue = GetOptionalJsonValue(Params, TEXT("value"));
		if (!OutValue.IsValid())
		{
			return mcp_common::MakeErrorReply(TEXT("value is required"));
		}
		return nullptr;
	}

	/** { count, types: [{ name, struct_path }] } of the concrete structs deriving from Base. */
	TSharedPtr<FJsonObject> StateTree_DerivedTypesData(const UScriptStruct* Base)
	{
		const TArray<FString> Names = ListDerivedStructNames(Base);
		TArray<TSharedPtr<FJsonValue>> TypesJson;
		TypesJson.Reserve(Names.Num());
		for (const FString& N : Names)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"),        N);
			Entry->SetStringField(TEXT("struct_path"), TEXT("/Script/StateTreeModule.") + N);
			TypesJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("count"), Names.Num());
		Data->SetArrayField (TEXT("types"), TypesJson);
		return Data;
	}

	/** Add class, instance_id and properties (the whole instance) of Node to Data; a node without a struct returns the error reply. */
	TSharedPtr<FJsonObject> StateTree_AddNodeDump(const TSharedPtr<FJsonObject>& Data, const FStateTreeEditorNode& Node,
		const TCHAR* NoStructError)
	{
		const UScriptStruct* Struct = Node.Node.GetScriptStruct();
		if (!Struct)
		{
			return mcp_common::MakeErrorReply(NoStructError);
		}
		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		FJsonObjectConverter::UStructToJsonObject(Struct, Node.Node.GetMemory(), Props.ToSharedRef(), 0, 0);
		Data->SetStringField(TEXT("class"),       Struct->GetName());
		Data->SetStringField(TEXT("instance_id"), Node.ID.ToString());
		Data->SetObjectField(TEXT("properties"),  Props);
		return nullptr;
	}

	/** The property PropName of Node's struct; on failure returns nullptr and fills OutError. */
	FProperty* StateTree_FindNodeProperty(const FStateTreeEditorNode& Node, const FString& PropName,
		const TCHAR* NoStructError, TSharedPtr<FJsonObject>& OutError)
	{
		const UScriptStruct* Struct = Node.Node.GetScriptStruct();
		if (!Struct)
		{
			OutError = mcp_common::MakeErrorReply(NoStructError);
			return nullptr;
		}
		FProperty* Prop = Struct->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Property '%s' not found on struct %s"), *PropName, *Struct->GetName()));
		}
		return Prop;
	}

	/** Add property, type, has_value and value (the property PropName of Node's instance) to Data; on failure returns the error reply. */
	TSharedPtr<FJsonObject> StateTree_ReadNodeProperty(const TSharedPtr<FJsonObject>& Data, const FStateTreeEditorNode& Node,
		const FString& PropName, const TCHAR* NoStructError)
	{
		TSharedPtr<FJsonObject> Error;
		FProperty* Prop = StateTree_FindNodeProperty(Node, PropName, NoStructError, Error);
		if (!Prop) { return Error; }
		const TSharedPtr<FJsonValue> Value = PropertyToJsonValue(Prop, Node.Node.GetMemory());
		Data->SetStringField(TEXT("property"),  PropName);
		Data->SetStringField(TEXT("type"),      Prop->GetCPPType());
		Data->SetBoolField  (TEXT("has_value"), Value.IsValid());
		if (Value.IsValid()) { Data->SetField(TEXT("value"), Value); }
		return nullptr;
	}

	/**
	 * Write Value into the property PropName of Node's instance, then Owner->Modify() and save the tree; on failure
	 * returns the error reply, on success adds property, set, value_changed, saved and new_value to Data.
	 */
	TSharedPtr<FJsonObject> StateTree_WriteNodeProperty(const TSharedPtr<FJsonObject>& Data, FStateTreeEditorNode& Node,
		const FString& PropName, const TSharedPtr<FJsonValue>& Value, const TCHAR* NoStructError, UObject* Owner, UStateTree* Tree)
	{
		TSharedPtr<FJsonObject> Error;
		FProperty* Prop = StateTree_FindNodeProperty(Node, PropName, NoStructError, Error);
		if (!Prop) { return Error; }

		// Snapshot the old value to report value_changed (idempotent semantics).
		const TSharedPtr<FJsonValue> OldValue = PropertyToJsonValue(Prop, Node.Node.GetMemory());
		void* Mem = Node.Node.GetMutableMemory();
		if (!ApplyJsonValueToProperty(Prop, Mem, Value))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Failed to apply JSON value to property '%s' (type %s)"),
					*PropName, *Prop->GetCPPType()));
		}
		const TSharedPtr<FJsonValue> NewValue = PropertyToJsonValue(Prop, Mem);
		const bool bValueChanged = !(OldValue.IsValid() && NewValue.IsValid()
			&& JsonValueToSerializedString(OldValue) == JsonValueToSerializedString(NewValue));

		Owner->Modify();
		const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

		Data->SetStringField(TEXT("property"),      PropName);
		Data->SetBoolField  (TEXT("set"),           true);
		Data->SetBoolField  (TEXT("value_changed"), bValueChanged);
		Data->SetBoolField  (TEXT("saved"),         bSaved);
		if (NewValue.IsValid()) { Data->SetField(TEXT("new_value"), NewValue); }
		return nullptr;
	}

	/**
	 * The condition node set/get_condition_property address: Conditions[CondIndex] of transition TransitionIdx when
	 * TargetKind is "transition", else EnterConditions[CondIndex]; on failure returns nullptr and fills OutError.
	 */
	FStateTreeEditorNode* StateTree_FindConditionNode(UStateTreeState* State, const FString& TargetKind,
		int32 TransitionIdx, int32 CondIndex, TSharedPtr<FJsonObject>& OutError)
	{
		if (TargetKind == TEXT("transition"))
		{
			if (!State->Transitions.IsValidIndex(TransitionIdx))
			{
				OutError = mcp_common::MakeErrorReply(
					FString::Printf(TEXT("transition_index %d out of range"), TransitionIdx));
				return nullptr;
			}
			if (!State->Transitions[TransitionIdx].Conditions.IsValidIndex(CondIndex))
			{
				OutError = mcp_common::MakeErrorReply(
					FString::Printf(TEXT("condition_index %d out of range"), CondIndex));
				return nullptr;
			}
			return &State->Transitions[TransitionIdx].Conditions[CondIndex];
		}
		return StateTree_FindStateNode(State, TEXT("EnterConditions"), CondIndex,
			FString::Printf(TEXT("Condition index %d out of range"), CondIndex), OutError);
	}
}

// ---------------------------------------------------------------------------
// list_task_types — enumerate StateTreeTaskBase derivatives
// ---------------------------------------------------------------------------
// MCP:list_task_types - Enumerate all concrete UScriptStructs deriving from FStateTreeTaskBase (instantiable, RF_Abstract excluded), via TObjectIterator<UScriptStruct>.
//   @returns { count, types: [{ name, struct_path }] }
TSharedPtr<FJsonObject> HandleListTaskTypes(
	const TSharedPtr<FJsonObject>& Params)
{
	return mcp_common::MakeOkReply(StateTree_DerivedTypesData(FStateTreeTaskBase::StaticStruct()));
}

// ---------------------------------------------------------------------------
// get_task_info — full struct dump of a state's task instance
// ---------------------------------------------------------------------------
// MCP:get_task_info - Read one task of a named state in a StateTree asset without changing it: the task node's struct name (class), its instance_id and every property of the node struct as JSON (properties). The task's instance data is not included. Errors when the tree, its editor data or the state is missing, or task_index is out of range; state_name picks the first state with that name. List a state's tasks with list_tasks; read or write one field with get_task_property or set_task_property; global tasks: list_global_tasks.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param task_index - 0-based index into State->Tasks.
//   @returns { tree_path, state_name, task_index, class, instance_id, properties: {...} }
TSharedPtr<FJsonObject> HandleGetTaskInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("task_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("task_index is required (int)"));
	}
	FStateTreeEditorNode* Node = StateTree_FindStateNode(State, TEXT("Tasks"), Index,
		FString::Printf(TEXT("Task index %d out of range (state has %d tasks)"), Index, State->Tasks.Num()), Err);
	if (!Node) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("task_index"),   Index);
	if (TSharedPtr<FJsonObject> DumpError = StateTree_AddNodeDump(Data, *Node, TEXT("Task node has no script struct")))
	{
		return DumpError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_task_property — edit one field of a task instance via reflection
// ---------------------------------------------------------------------------
// MCP:set_task_property - Set a single field of a task instance by reflection (FJsonObjectConverter::JsonValueToFProperty). Idempotent — re-applying the same value is a no-op (returns set:true, value_changed:false).
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param task_index - 0-based index into State->Tasks.
//   @param property - FProperty name on the task struct.
//   @param value [any!] - JSON value to apply (any FJsonObjectConverter-supported type).
//   @returns { tree_path, state_name, task_index, property, set, value_changed, saved }
TSharedPtr<FJsonObject> HandleSetTaskProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("task_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("task_index is required (int)"));
	}
	FString PropName;
	TSharedPtr<FJsonValue> Value;
	if (TSharedPtr<FJsonObject> Missing = StateTree_ReadPropertyValue(Params, PropName, Value)) { return Missing; }

	FStateTreeEditorNode* Node = StateTree_FindStateNode(State, TEXT("Tasks"), Index,
		FString::Printf(TEXT("Task index %d out of range"), Index), Err);
	if (!Node) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("task_index"),   Index);
	if (TSharedPtr<FJsonObject> WriteError = StateTree_WriteNodeProperty(Data, *Node, PropName, Value,
		TEXT("Task node has no script struct"), State, Tree))
	{
		return WriteError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_task_property — read one field of a task instance
// ---------------------------------------------------------------------------
// MCP:get_task_property - Read one property of a task node on a StateTree state by reflection, without changing the asset. Returns the value as JSON with its C++ type and has_value; only node struct properties are reachable, not instance data. Use get_task_info for the whole node and set_task_property to write.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the task; the command fails without it.
//   @param task_index - 0-based index into the state Tasks, as listed by list_tasks.
//   @param property - Name of a property on the task node struct, e.g. "FontScale" on a debug text task; case-insensitive.
//   @returns { tree_path, state_name, task_index, property, type, value, has_value }
TSharedPtr<FJsonObject> HandleGetTaskProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("task_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("task_index is required (int)"));
	}
	FString PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropName)) { return Missing; }
	FStateTreeEditorNode* Node = StateTree_FindStateNode(State, TEXT("Tasks"), Index,
		FString::Printf(TEXT("Task index %d out of range"), Index), Err);
	if (!Node) { return Err; }
	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("task_index"), Index);
	if (TSharedPtr<FJsonObject> ReadError = StateTree_ReadNodeProperty(Data, *Node, PropName, TEXT("Task node has no script struct")))
	{
		return ReadError;
	}
	return mcp_common::MakeOkReply(Data);
}

// =========================================================================
// StateTree Consideration CRUD, on par with the Task commands.
//
// FStateTreeState::Considerations is TArray<FStateTreeEditorNode> — the SAME
// type as ::Tasks — so the consideration commands work exactly like their Task
// siblings (remove_task / list_tasks / get_task_info / get_task_property /
// set_task_property), differing only in the array they address
// (State->Considerations) and the param name (consideration_index). State
// addressing, persistence (State->Modify() + SaveAssetPackage), reflection
// path (GetStateNodeByIndex(..., TEXT("Considerations"), ...)) and idempotent
// semantics are identical; add_consideration is the add side. Node CRUD leaves
// recompile to the explicit compile_state_tree — matching the Task siblings
// (no CompileIfChanged here).
//
// All headless-safe: editor-only authoring of EditorData. No PIE, no runtime.

// ---------------------------------------------------------------------------
// remove_consideration — remove a consideration (by index) from a state's Considerations
// ---------------------------------------------------------------------------
// MCP:remove_consideration - Remove a consideration (by index) from a state. Idempotent at the index-out-of-range boundary.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param consideration_index - 0-based index into the state's Considerations array.
//   @returns { tree_path, state_name, consideration_index, removed, remaining, saved }
TSharedPtr<FJsonObject> HandleRemoveConsideration(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("consideration_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("consideration_index is required (int)"));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("consideration_index"), Index);
	StateTree_RemoveAtIndex(Data, Tree, State, State->Considerations, Index,
		TEXT("index out of range -- idempotent no-op"));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_considerations — considerations of a single state
// ---------------------------------------------------------------------------
// MCP:list_considerations - List all considerations of a state, with their struct class name and instance ID.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @returns { tree_path, state_name, count, considerations: [{ index, class, instance_id }] }
TSharedPtr<FJsonObject> HandleListConsiderations(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("count"),          State->Considerations.Num());
	Data->SetArrayField (TEXT("considerations"), StateTree_NodeListToJson(State->Considerations));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_consideration_info — full struct dump of a state's consideration instance
// ---------------------------------------------------------------------------
// MCP:get_consideration_info - Serialize a single consideration's instance data (FJsonObjectConverter over the consideration struct) for inspection.
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param consideration_index - 0-based index into State->Considerations.
//   @returns { tree_path, state_name, consideration_index, class, instance_id, properties: {...} }
TSharedPtr<FJsonObject> HandleGetConsiderationInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("consideration_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("consideration_index is required (int)"));
	}
	FStateTreeEditorNode* Node = StateTree_FindStateNode(State, TEXT("Considerations"), Index,
		FString::Printf(TEXT("Consideration index %d out of range (state has %d considerations)"), Index, State->Considerations.Num()), Err);
	if (!Node) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("consideration_index"), Index);
	if (TSharedPtr<FJsonObject> DumpError = StateTree_AddNodeDump(Data, *Node, TEXT("Consideration node has no script struct")))
	{
		return DumpError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_consideration_property — edit one field of a consideration instance via reflection
// ---------------------------------------------------------------------------
// MCP:set_consideration_property - Set a single field of a consideration instance by reflection (FJsonObjectConverter::JsonValueToFProperty). Idempotent — re-applying the same value is a no-op (returns set:true, value_changed:false).
//   @param tree_path - Asset path to the UStateTree.
//   @param state_name [string!] - Name of the state.
//   @param consideration_index - 0-based index into State->Considerations.
//   @param property - FProperty name on the consideration struct.
//   @param value [any!] - JSON value to apply (any FJsonObjectConverter-supported type).
//   @returns { tree_path, state_name, consideration_index, property, set, value_changed, saved }
TSharedPtr<FJsonObject> HandleSetConsiderationProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("consideration_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("consideration_index is required (int)"));
	}
	FString PropName;
	TSharedPtr<FJsonValue> Value;
	if (TSharedPtr<FJsonObject> Missing = StateTree_ReadPropertyValue(Params, PropName, Value)) { return Missing; }

	FStateTreeEditorNode* Node = StateTree_FindStateNode(State, TEXT("Considerations"), Index,
		FString::Printf(TEXT("Consideration index %d out of range"), Index), Err);
	if (!Node) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("consideration_index"), Index);
	if (TSharedPtr<FJsonObject> WriteError = StateTree_WriteNodeProperty(Data, *Node, PropName, Value,
		TEXT("Consideration node has no script struct"), State, Tree))
	{
		return WriteError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_consideration_property — read one field of a consideration instance
// ---------------------------------------------------------------------------
// MCP:get_consideration_property - Read one property of a consideration node on a StateTree state by reflection, without changing the asset. Returns the value as JSON with its C++ type and has_value; only node struct properties are reachable, not instance data. Find the index with list_considerations.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the consideration; the command fails without it.
//   @param consideration_index - 0-based index into the state Considerations, as listed by list_considerations.
//   @param property - Name of a property on the consideration node struct; case-insensitive.
//   @returns { tree_path, state_name, consideration_index, property, type, value, has_value }
TSharedPtr<FJsonObject> HandleGetConsiderationProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("consideration_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("consideration_index is required (int)"));
	}
	FString PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropName)) { return Missing; }
	FStateTreeEditorNode* Node = StateTree_FindStateNode(State, TEXT("Considerations"), Index,
		FString::Printf(TEXT("Consideration index %d out of range"), Index), Err);
	if (!Node) { return Err; }
	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("consideration_index"), Index);
	if (TSharedPtr<FJsonObject> ReadError = StateTree_ReadNodeProperty(Data, *Node, PropName, TEXT("Consideration node has no script struct")))
	{
		return ReadError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_condition_types — enumerate StateTreeConditionBase derivatives
// ---------------------------------------------------------------------------
// MCP:list_condition_types - List the concrete condition struct types (children of FStateTreeConditionBase, abstract ones skipped) of the loaded modules, sorted by name, for add_state_enter_condition and add_transition_condition. Returns count and types [{name, struct_path}]; struct_path always assumes /Script/StateTreeModule, so pass name for other modules.
//   @returns { count, types: [{ name, struct_path }] }
TSharedPtr<FJsonObject> HandleListConditionTypes(
	const TSharedPtr<FJsonObject>& Params)
{
	return mcp_common::MakeOkReply(StateTree_DerivedTypesData(FStateTreeConditionBase::StaticStruct()));
}

// ---------------------------------------------------------------------------
// get_condition_info — full dump of a condition (state-level or transition-level)
// ---------------------------------------------------------------------------
// MCP:get_condition_info - Dump one condition node of a StateTree state as JSON without changing the asset: an enter condition of the state or, with target_kind "transition", a condition of one of its transitions. Returns class, instance_id and properties, the node struct fields (instance data is not included).
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the condition; the command fails without it.
//   @param target_kind - Which list to read: "state" (default, the state EnterConditions) or "transition" (the Conditions of transition transition_index); any other value reads the enter conditions.
//   @param transition_index - 0-based index into the state transitions; required when target_kind is "transition" and ignored otherwise.
//   @param condition_index - 0-based index into the selected condition list, as counted by list_conditions or list_transitions.
//   @returns { tree_path, state_name, target_kind, transition_index, condition_index, class, instance_id, properties: {...} }
TSharedPtr<FJsonObject> HandleGetConditionInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	FString TargetKind = TEXT("state");
	Params->TryGetStringField(TEXT("target_kind"), TargetKind);
	TargetKind = TargetKind.ToLower();

	int32 CondIndex = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("condition_index"), CondIndex))
	{
		return mcp_common::MakeErrorReply(TEXT("condition_index is required (int)"));
	}

	FStateTreeEditorNode* Node = nullptr;
	int32 TransitionIdx = INDEX_NONE;
	// Read for every kind and used only by "transition" (a guarded read inside that branch would make
	// the catalog publish transition_index as required for the enter conditions too).
	int32 RequestedTransitionIdx = INDEX_NONE;
	const bool bHasTransitionIndex = Params->TryGetNumberField(TEXT("transition_index"), RequestedTransitionIdx);
	if (TargetKind == TEXT("transition"))
	{
		if (!bHasTransitionIndex)
		{
			return mcp_common::MakeErrorReply(
				TEXT("transition_index is required when target_kind=transition"));
		}
		TransitionIdx = RequestedTransitionIdx;
		if (!State->Transitions.IsValidIndex(TransitionIdx))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("transition_index %d out of range"), TransitionIdx));
		}
		if (!State->Transitions[TransitionIdx].Conditions.IsValidIndex(CondIndex))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("condition_index %d out of range (transition has %d conditions)"),
					CondIndex, State->Transitions[TransitionIdx].Conditions.Num()));
		}
		Node = &State->Transitions[TransitionIdx].Conditions[CondIndex];
	}
	else
	{
		Node = StateTree_FindStateNode(State, TEXT("EnterConditions"), CondIndex,
			FString::Printf(TEXT("Condition index %d out of range"), CondIndex), Err);
		if (!Node) { return Err; }
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("target_kind"),       TargetKind);
	if (TransitionIdx != INDEX_NONE) { Data->SetNumberField(TEXT("transition_index"), TransitionIdx); }
	Data->SetNumberField(TEXT("condition_index"),    CondIndex);
	if (TSharedPtr<FJsonObject> DumpError = StateTree_AddNodeDump(Data, *Node, TEXT("Condition node has no script struct")))
	{
		return DumpError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_condition_property / get_condition_property
//   Same shape as task property helpers but routed to either EnterConditions
//   or a transition's Conditions list by `target_kind`.
// ---------------------------------------------------------------------------
// MCP:set_condition_property - Write one property of a condition node on a StateTree state by reflection and save the asset: an enter condition, or a transition condition with target_kind "transition". Only node struct properties are reachable, not instance data. Returns value_changed, new_value and saved; the same value again gives value_changed false.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the condition; the command fails without it.
//   @param target_kind - Which list holds the condition: "state" (default, EnterConditions) or "transition" (Conditions of transition transition_index); any other value means "state".
//   @param transition_index - 0-based index into the state transitions; used only when target_kind is "transition", where omitting it fails.
//   @param condition_index - 0-based index into the selected condition list; omitting it fails as out of range.
//   @param property - Name of a property on the condition node struct, e.g. "bInvert" on a compare condition; case-insensitive.
//   @param value [any!] - New value as JSON: a number, bool or string, or an object for a struct property, applied through FJsonObjectConverter.
//   @returns { tree_path, state_name, target_kind, transition_index, condition_index, property, set, value_changed, saved }
TSharedPtr<FJsonObject> HandleSetConditionProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	FString TargetKind = TEXT("state");
	Params->TryGetStringField(TEXT("target_kind"), TargetKind);
	TargetKind = TargetKind.ToLower();

	int32 CondIndex = INDEX_NONE;
	Params->TryGetNumberField(TEXT("condition_index"), CondIndex);
	int32 TransitionIdx = INDEX_NONE;
	Params->TryGetNumberField(TEXT("transition_index"), TransitionIdx);

	FString PropName;
	TSharedPtr<FJsonValue> Value;
	if (TSharedPtr<FJsonObject> Missing = StateTree_ReadPropertyValue(Params, PropName, Value)) { return Missing; }

	FStateTreeEditorNode* Node = StateTree_FindConditionNode(State, TargetKind, TransitionIdx, CondIndex, Err);
	if (!Node) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("target_kind"),    TargetKind);
	if (TransitionIdx != INDEX_NONE) { Data->SetNumberField(TEXT("transition_index"), TransitionIdx); }
	Data->SetNumberField(TEXT("condition_index"), CondIndex);
	if (TSharedPtr<FJsonObject> WriteError = StateTree_WriteNodeProperty(Data, *Node, PropName, Value,
		TEXT("Condition node has no script struct"), State, Tree))
	{
		return WriteError;
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_condition_property - Read one property of a condition node on a StateTree state by reflection, without changing the asset: an enter condition, or a transition condition with target_kind "transition". Returns the value as JSON with its C++ type and has_value; only node struct properties are reachable, not instance data.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the condition; the command fails without it.
//   @param property - Name of a property on the condition node struct, e.g. "bInvert" on a compare condition; case-insensitive.
//   @param target_kind - Which list holds the condition: "state" (default, EnterConditions) or "transition" (Conditions of transition transition_index); any other value means "state".
//   @param condition_index - 0-based index into the selected condition list; omitting it fails as out of range.
//   @param transition_index - 0-based index into the state transitions; used only when target_kind is "transition", where omitting it fails.
//   @returns { tree_path, state_name, target_kind, transition_index, condition_index, property, type, value, has_value }
TSharedPtr<FJsonObject> HandleGetConditionProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	FString TargetKind = TEXT("state");
	Params->TryGetStringField(TEXT("target_kind"), TargetKind);
	TargetKind = TargetKind.ToLower();

	int32 CondIndex = INDEX_NONE;
	Params->TryGetNumberField(TEXT("condition_index"), CondIndex);
	int32 TransitionIdx = INDEX_NONE;
	Params->TryGetNumberField(TEXT("transition_index"), TransitionIdx);
	FString PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropName)) { return Missing; }

	FStateTreeEditorNode* Node = StateTree_FindConditionNode(State, TargetKind, TransitionIdx, CondIndex, Err);
	if (!Node) { return Err; }

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("target_kind"),    TargetKind);
	if (TransitionIdx != INDEX_NONE) { Data->SetNumberField(TEXT("transition_index"), TransitionIdx); }
	Data->SetNumberField(TEXT("condition_index"), CondIndex);
	if (TSharedPtr<FJsonObject> ReadError = StateTree_ReadNodeProperty(Data, *Node, PropName, TEXT("Condition node has no script struct")))
	{
		return ReadError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_state_enter_condition — remove an enter condition by index
// ---------------------------------------------------------------------------
// MCP:remove_state_enter_condition - Remove one enter condition, by index, from a state of a StateTree asset and save the asset. An out-of-range index removes nothing and still succeeds, with removed false and a note. Returns removed, remaining and saved; for transition conditions use remove_transition_condition.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the condition; the command fails without it.
//   @param condition_index - 0-based index into the state EnterConditions, as listed by list_conditions.
//   @returns { tree_path, state_name, condition_index, removed, remaining, saved, note }
TSharedPtr<FJsonObject> HandleRemoveStateEnterCondition(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("condition_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("condition_index is required (int)"));
	}
	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("condition_index"), Index);
	StateTree_RemoveAtIndex(Data, Tree, State, State->EnterConditions, Index,
		TEXT("Index out of range -- nothing removed (idempotent no-op)."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_transition_condition — remove a transition's condition by index
// ---------------------------------------------------------------------------
// MCP:remove_transition_condition - Remove one condition, by index, from a transition of a StateTree state and save the asset. An out-of-range transition_index is an error; an out-of-range condition_index removes nothing and still succeeds, with removed false. Returns removed, remaining and saved.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param state_name [string!] - Name of the state that owns the transition; the command fails without it.
//   @param transition_index - 0-based index into the state transitions, as listed by list_transitions.
//   @param condition_index - 0-based index into the Conditions of that transition.
//   @returns { tree_path, state_name, transition_index, condition_index, removed, remaining, saved, note }
TSharedPtr<FJsonObject> HandleRemoveTransitionCondition(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	int32 TIdx = INDEX_NONE, CIdx = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("transition_index"), TIdx))
	{
		return mcp_common::MakeErrorReply(TEXT("transition_index is required (int)"));
	}
	if (!Params->TryGetNumberField(TEXT("condition_index"), CIdx))
	{
		return mcp_common::MakeErrorReply(TEXT("condition_index is required (int)"));
	}
	if (!State->Transitions.IsValidIndex(TIdx))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("transition_index %d out of range"), TIdx));
	}
	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("transition_index"), TIdx);
	Data->SetNumberField(TEXT("condition_index"),  CIdx);
	StateTree_RemoveAtIndex(Data, Tree, State, State->Transitions[TIdx].Conditions, CIdx,
		TEXT("Condition index out of range -- nothing removed (idempotent no-op)."));
	return mcp_common::MakeOkReply(Data);
}// =========================================================================
// Evaluators + bindings + parameters + validation
//
//   - reflection over StateTreeEvaluatorBase derivatives (same helper as the task/condition reflection)
//   - reflection-based get/set on evaluator instances
//   - remove_tree_binding is the inverse of add_state_tree_binding
//   - set_tree_parameter updates an existing parameter value (add_tree_parameter
//     only adds)
//   - add_state_tree_global_task_property wraps the global-task setter path
//   - validate_state_tree runs the editor compile pipeline WITHOUT saving so an
//     agent can dry-run before committing; reports structured errors/warnings
//     (compile_state_tree both compiles and saves — that
//     is the wrong shape for "is this tree valid?")

// ---------------------------------------------------------------------------
// list_evaluator_types — enumerate StateTreeEvaluatorBase derivatives
// ---------------------------------------------------------------------------
// MCP:list_evaluator_types - List the concrete evaluator struct types (children of FStateTreeEvaluatorBase, abstract ones skipped) of the loaded modules, sorted by name, for add_state_evaluator. Returns count and types [{name, struct_path}]; struct_path always assumes /Script/StateTreeModule, so pass name for other modules.
//   @returns { count, types: [{ name, struct_path }] }
TSharedPtr<FJsonObject> HandleListEvaluatorTypes(
	const TSharedPtr<FJsonObject>& Params)
{
	return mcp_common::MakeOkReply(StateTree_DerivedTypesData(FStateTreeEvaluatorBase::StaticStruct()));
}

// ---------------------------------------------------------------------------
// get_evaluator_info / set_evaluator_property / get_evaluator_property
//   Tree-scoped evaluator instance dump + reflection-driven get/set. Identical
//   shape to the task helpers but routed at UStateTreeEditorData::Evaluators.
// ---------------------------------------------------------------------------
// MCP:get_evaluator_info - Dump one tree-scoped evaluator of a StateTree asset as JSON without changing it. Returns evaluator_index, class, instance_id and properties, the node struct fields (instance data is not included). Find the index with list_evaluators.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param evaluator_index - 0-based index into the tree Evaluators, as listed by list_evaluators; out of range is an error.
//   @returns { tree_path, evaluator_index, class, instance_id, properties: {...} }
TSharedPtr<FJsonObject> HandleGetEvaluatorInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("evaluator_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("evaluator_index is required (int)"));
	}
	if (!ED->Evaluators.IsValidIndex(Index))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("evaluator_index %d out of range"), Index));
	}
	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("evaluator_index"),  Index);
	if (TSharedPtr<FJsonObject> DumpError = StateTree_AddNodeDump(Data, ED->Evaluators[Index], TEXT("Evaluator node has no script struct")))
	{
		return DumpError;
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:set_evaluator_property - Write one property of a tree-scoped evaluator node of a StateTree asset by reflection and save the asset. Only node struct properties are reachable, not instance data. Returns value_changed, new_value and saved; the same value again gives value_changed false. Read it back with get_evaluator_property.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param evaluator_index - 0-based index into the tree Evaluators, as listed by list_evaluators; out of range is an error.
//   @param property - Name of a property on the evaluator node struct; case-insensitive.
//   @param value [any!] - New value as JSON: a number, bool or string, or an object for a struct property, applied through FJsonObjectConverter.
//   @returns { tree_path, evaluator_index, property, set, value_changed, saved }
TSharedPtr<FJsonObject> HandleSetEvaluatorProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("evaluator_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("evaluator_index is required (int)"));
	}
	FString PropName;
	TSharedPtr<FJsonValue> Value;
	if (TSharedPtr<FJsonObject> Missing = StateTree_ReadPropertyValue(Params, PropName, Value)) { return Missing; }
	if (!ED->Evaluators.IsValidIndex(Index))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("evaluator_index %d out of range"), Index));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("evaluator_index"), Index);
	if (TSharedPtr<FJsonObject> WriteError = StateTree_WriteNodeProperty(Data, ED->Evaluators[Index], PropName, Value,
		TEXT("Evaluator node has no script struct"), ED, Tree))
	{
		return WriteError;
	}
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_evaluator_property - Read one property of a tree-scoped evaluator node of a StateTree asset by reflection, without changing it. Returns the value as JSON with its C++ type and has_value; only node struct properties are reachable, not instance data. Write it with set_evaluator_property.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param evaluator_index - 0-based index into the tree Evaluators, as listed by list_evaluators; omitting it fails as out of range.
//   @param property - Name of a property on the evaluator node struct, e.g. "Name"; case-insensitive.
//   @returns { tree_path, evaluator_index, property, type, value, has_value }
TSharedPtr<FJsonObject> HandleGetEvaluatorProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	int32 Index = INDEX_NONE;
	Params->TryGetNumberField(TEXT("evaluator_index"), Index);
	FString PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropName)) { return Missing; }
	if (!ED->Evaluators.IsValidIndex(Index))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("evaluator_index %d out of range"), Index));
	}
	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("evaluator_index"), Index);
	if (TSharedPtr<FJsonObject> ReadError = StateTree_ReadNodeProperty(Data, ED->Evaluators[Index], PropName, TEXT("Evaluator node has no script struct")))
	{
		return ReadError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_tree_binding — drop the bindings whose target is one node property
// ---------------------------------------------------------------------------
// MCP:remove_tree_binding - Remove the property binding whose target is target_property on one node of a StateTree asset (and any binding to a sub-property of it), the way the StateTree editor clears a binding, then save the asset. Nothing bound there is not an error: removed is false and the asset is not saved. Returns removed, removed_count and remaining (bindings left in the tree); list them with list_tree_bindings.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param target_kind - Node list of the binding target: "task", "condition" (a state enter condition; "enter_condition" also works), "consideration", "transition" (a transition condition), "global_task" or "evaluator".
//   @param state_name - Name of the state that owns the target node; needed for the task, condition, consideration and transition kinds.
//   @param transition_index - 0-based index into the state transitions; used only by the transition kind.
//   @param target_index - 0-based index of the target node in its list, default 0; for the transition kind, the index into the Conditions of that transition.
//   @param target_property - Property path of the binding target on that node, as list_tree_bindings shows it in target_path, e.g. "BindableText".
//   @returns { tree_path, target_kind, state_name?, target_index, target_property, target_struct_id, removed, removed_count, remaining, saved }
TSharedPtr<FJsonObject> HandleRemoveTreeBinding(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	FString TargetKind, TargetProperty;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_kind"), TargetKind)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target_property"), TargetProperty)) { return Missing; }
	TargetKind = TargetKind.TrimStartAndEnd().ToLower();
	if (TargetKind == TEXT("enter_condition"))
	{
		TargetKind = TEXT("condition");  // add_state_tree_binding's name for the same list
	}

	// Mirror the AddStateTreeBinding resolution: find the target FStateTreeEditorNode, whose ID
	// anchors the binding target path.
	FStateTreeEditorNode* TargetNode = nullptr;
	FString StateName;
	int32 TargetIndex = 0;
	int32 TransitionIdx = INDEX_NONE;

	Params->TryGetNumberField(TEXT("target_index"), TargetIndex);
	Params->TryGetNumberField(TEXT("transition_index"), TransitionIdx);
	Params->TryGetStringField(TEXT("state_name"), StateName);

	auto FindStateNode = [&]() -> FStateTreeEditorNode*
	{
		if (StateName.IsEmpty()) { return nullptr; }
		UStateTreeState* S = FindStateByName(ED, FName(*StateName));
		if (!S) { return nullptr; }
		if (TargetKind == TEXT("task"))
		{
			return S->Tasks.IsValidIndex(TargetIndex) ? &S->Tasks[TargetIndex] : nullptr;
		}
		if (TargetKind == TEXT("condition"))
		{
			return S->EnterConditions.IsValidIndex(TargetIndex) ? &S->EnterConditions[TargetIndex] : nullptr;
		}
		if (TargetKind == TEXT("consideration"))
		{
			return S->Considerations.IsValidIndex(TargetIndex) ? &S->Considerations[TargetIndex] : nullptr;
		}
		if (TargetKind == TEXT("transition"))
		{
			if (!S->Transitions.IsValidIndex(TransitionIdx)) { return nullptr; }
			return S->Transitions[TransitionIdx].Conditions.IsValidIndex(TargetIndex)
				? &S->Transitions[TransitionIdx].Conditions[TargetIndex] : nullptr;
		}
		return nullptr;
	};

	if (TargetKind == TEXT("global_task"))
	{
		if (!ED->GlobalTasks.IsValidIndex(TargetIndex))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("target_index %d out of range for global_task"), TargetIndex));
		}
		TargetNode = &ED->GlobalTasks[TargetIndex];
	}
	else if (TargetKind == TEXT("evaluator"))
	{
		if (!ED->Evaluators.IsValidIndex(TargetIndex))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("target_index %d out of range for evaluator"), TargetIndex));
		}
		TargetNode = &ED->Evaluators[TargetIndex];
	}
	else if (TargetKind == TEXT("task") || TargetKind == TEXT("condition") || TargetKind == TEXT("consideration")
		|| TargetKind == TEXT("transition"))
	{
		TargetNode = FindStateNode();
		if (!TargetNode)
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Could not resolve target node (kind=%s, state=%s, index=%d)"),
					*TargetKind, *StateName, TargetIndex));
		}
	}
	else
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Unsupported target_kind '%s' (use task|condition|consideration|transition|global_task|evaluator)"), *TargetKind));
	}
	const FGuid TargetID = TargetNode->ID;

	FStateTreeEditorPropertyBindings* Bindings = ED->GetPropertyEditorBindings();
	if (!Bindings)
	{
		return mcp_common::MakeErrorReply(TEXT("Tree has no property bindings"));
	}

	// The target path is built as add_state_tree_binding builds it. RemoveBindings comes from
	// FPropertyBindingBindingCollection; Includes also drops the bindings to sub-properties of the
	// target, as UStateTreeEditorData::RemovePropertyBinding (the editor's own removal) does.
	FPropertyBindingPath TargetPath;
	if (!TargetPath.FromString(TargetProperty))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("target_property '%s' is not a valid property path"), *TargetProperty));
	}
	TargetPath.SetStructID(TargetID);

	const int32 Before = Bindings->GetBindings().Num();
	ED->Modify();
	Bindings->RemoveBindings(TargetPath, FPropertyBindingBindingCollection::ESearchMode::Includes);
	const int32 Remaining = Bindings->GetBindings().Num();
	const int32 RemovedCount = Before - Remaining;

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("target_kind"), TargetKind);
	if (!StateName.IsEmpty()) { Data->SetStringField(TEXT("state_name"), StateName); }
	Data->SetNumberField(TEXT("target_index"), TargetIndex);
	Data->SetStringField(TEXT("target_property"), TargetProperty);
	Data->SetStringField(TEXT("target_struct_id"), TargetID.ToString());
	Data->SetBoolField(TEXT("removed"), RemovedCount > 0);
	Data->SetNumberField(TEXT("removed_count"), RemovedCount);
	Data->SetNumberField(TEXT("remaining"), Remaining);
	if (RemovedCount == 0)
	{
		// Nothing was bound to that target: no change, nothing to save.
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_tree_binding"));
}

// ---------------------------------------------------------------------------
// set_tree_parameter — update the value of an existing root parameter
// ---------------------------------------------------------------------------
// MCP:set_tree_parameter - Set the value of an existing root parameter of a StateTree asset and save it. Works for bool, byte, int32, int64, float, double, name, string and text parameters; array and other types return an error. Add the parameter first with add_tree_parameter. Returns value_changed, new_value and saved.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param parameter - Name of a root parameter that already exists, as listed by list_tree_parameters; an unknown name is an error.
//   @param value [any!] - New value as JSON matching the parameter type: true or false, a number, or a string for name, string and text parameters.
//   @returns { tree_path, parameter, set, value_changed, saved, new_value? }
TSharedPtr<FJsonObject> HandleSetTreeParameter(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, ParamName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter"), ParamName)) { return Missing; }
	const TSharedPtr<FJsonValue> ValuePtr = GetOptionalJsonValue(Params, TEXT("value"));
	if (!ValuePtr.IsValid())
	{
		return mcp_common::MakeErrorReply(TEXT("value is required"));
	}
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	// Resolve descriptor via the underlying UPropertyBag (the typed layout),
	// not the FInstancedPropertyBag instance (which doesn't expose GetPropertyDescs).
	const UPropertyBag* BagStruct = ED->GetRootParametersPropertyBag().GetPropertyBagStruct();
	const FPropertyBagPropertyDesc* Desc = StateTree_FindRootParameter(ED, FName(*ParamName));
	if (!Desc)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Root parameter '%s' not found -- add it with add_tree_parameter first"),
				*ParamName));
	}

	// Snapshot the old value via FJsonObjectConverter round-trip over the bag struct.
	const TSharedPtr<FJsonValue> OldValue = StateTree_RootParameterValue(ED, BagStruct, ParamName);

	// Apply the JSON value via the typed setter matching EPropertyBagPropertyType.
	// GetRootParametersPropertyBag() returns const& but the underlying bag is mutable;
	// the editor reflection pipeline (CreateRootProperties + SetValueX) was designed
	// to mutate through const access for editor-data convenience.
	EPropertyBagResult SetResult = EPropertyBagResult::PropertyNotFound;
	FInstancedPropertyBag& MutableBag = const_cast<FInstancedPropertyBag&>(ED->GetRootParametersPropertyBag());
	switch (Desc->ValueType)
	{
		case EPropertyBagPropertyType::Bool:
			SetResult = MutableBag.SetValueBool(Desc->Name, ValuePtr->AsBool()); break;
		case EPropertyBagPropertyType::Byte:
			SetResult = MutableBag.SetValueByte(Desc->Name, (uint8)FMath::Max(0, (int32)ValuePtr->AsNumber())); break;
		case EPropertyBagPropertyType::Int32:
			SetResult = MutableBag.SetValueInt32(Desc->Name, (int32)ValuePtr->AsNumber()); break;
		case EPropertyBagPropertyType::Int64:
			SetResult = MutableBag.SetValueInt64(Desc->Name, (int64)ValuePtr->AsNumber()); break;
		case EPropertyBagPropertyType::Float:
			SetResult = MutableBag.SetValueFloat(Desc->Name, (float)ValuePtr->AsNumber()); break;
		case EPropertyBagPropertyType::Double:
			SetResult = MutableBag.SetValueDouble(Desc->Name, ValuePtr->AsNumber()); break;
		case EPropertyBagPropertyType::Name:
		{
			FString Tmp; ValuePtr->TryGetString(Tmp);
			SetResult = MutableBag.SetValueName(Desc->Name, FName(*Tmp));
			break;
		}
		case EPropertyBagPropertyType::String:
		{
			FString Tmp; ValuePtr->TryGetString(Tmp);
			SetResult = MutableBag.SetValueString(Desc->Name, Tmp);
			break;
		}
		case EPropertyBagPropertyType::Text:
		{
			// SetValueString only writes an FStrProperty: a text parameter needs SetValueText.
			FString Tmp; ValuePtr->TryGetString(Tmp);
			SetResult = MutableBag.SetValueText(Desc->Name, FText::FromString(Tmp));
			break;
		}
		default:
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Parameter type %s does not support value update via this command"),
					*UEnum::GetValueAsString(Desc->ValueType)));
	}
	if (SetResult != EPropertyBagResult::Success)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to set parameter '%s' (EPropertyBagResult=%d, type=%s)"),
				*ParamName, (int32)SetResult, *UEnum::GetValueAsString(Desc->ValueType)));
	}

	// Read back new value via the same dump path.
	const TSharedPtr<FJsonValue> NewValue = StateTree_RootParameterValue(ED, BagStruct, ParamName);
	const bool bValueChanged = !(OldValue.IsValid() && NewValue.IsValid()
		&& JsonValueToSerializedString(OldValue) == JsonValueToSerializedString(NewValue));

	ED->Modify();
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetStringField(TEXT("parameter"),     ParamName);
	Data->SetBoolField  (TEXT("set"),           true);
	Data->SetBoolField  (TEXT("value_changed"), bValueChanged);
	Data->SetBoolField  (TEXT("saved"),         bSaved);
	if (NewValue.IsValid()) { Data->SetField(TEXT("new_value"), NewValue); }
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_state_tree_global_task_property — set a property on a tree-scoped global task
// ---------------------------------------------------------------------------
// MCP:add_state_tree_global_task_property - Write one property of an existing global task of a StateTree asset by reflection and save the asset; despite the name it adds nothing (create the task with add_state_tree_global_task). Only node struct properties are reachable, not instance data. Returns value_changed, new_value and saved.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @param global_task_index - 0-based index into the tree GlobalTasks, as listed by list_global_tasks; out of range is an error.
//   @param property - Name of a property on the global task node struct, e.g. "FontScale" on a debug text task; case-insensitive.
//   @param value [any!] - New value as JSON: a number, bool or string, or an object for a struct property, applied through FJsonObjectConverter.
//   @returns { tree_path, global_task_index, property, set, value_changed, saved }
TSharedPtr<FJsonObject> HandleAddStateTreeGlobalTaskProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_path"), TreePath)) { return Missing; }
	int32 Index = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("global_task_index"), Index))
	{
		return mcp_common::MakeErrorReply(TEXT("global_task_index is required (int)"));
	}
	FString PropName;
	TSharedPtr<FJsonValue> Value;
	if (TSharedPtr<FJsonObject> Missing = StateTree_ReadPropertyValue(Params, PropName, Value)) { return Missing; }
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("Tree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }
	if (!ED->GlobalTasks.IsValidIndex(Index))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("global_task_index %d out of range"), Index));
	}

	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetNumberField(TEXT("global_task_index"), Index);
	if (TSharedPtr<FJsonObject> WriteError = StateTree_WriteNodeProperty(Data, ED->GlobalTasks[Index], PropName, Value,
		TEXT("Global task node has no script struct"), ED, Tree))
	{
		return WriteError;
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// validate_state_tree — dry-run compile (no save) with structured error report
// ---------------------------------------------------------------------------
// MCP:validate_state_tree - Compile a StateTree asset in memory and report its errors and warnings without saving: valid, compiled, error_count, warning_count and the messages. It still marks the package dirty and refreshes the compiled data in memory; compile_state_tree compiles and saves.
//   @param tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard".
//   @returns { tree_path, valid, compiled, error_count, warning_count, message_count, saved, errors: [{severity, message}], warnings: [{severity, message}], note }
TSharedPtr<FJsonObject> HandleValidateStateTree(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	if (!StateTree_ResolveEditorData(Params, TEXT("Tree has no EditorData"), Tree, Err))
	{
		return Err;
	}

	// Mark dirty — CompileStateTree expects a dirty tree to compile it.
	Tree->MarkPackageDirty();
	Tree->CompileIfChanged();

	// UStateTreeEditingSubsystem::CompileStateTree is a static helper
	// that takes (TNotNull<UStateTree*>, FStateTreeCompilerLog& InOutLog) and returns bool.
	// FStateTreeCompilerLog.Messages is protected, so we round-trip through the
	// public ToTokenizedMessages() helper to get severity-tagged tokens.
	FStateTreeCompilerLog Log;
	const bool bCompiled = UStateTreeEditingSubsystem::CompileStateTree(
		TNotNull<UStateTree*>(Tree), Log);

	const TArray<TSharedRef<FTokenizedMessage>> Tokens = Log.ToTokenizedMessages();
	TArray<TSharedPtr<FJsonValue>> ErrorsJson, WarningsJson;
	int32 ErrorCount = 0, WarnCount = 0;
	for (const TSharedRef<FTokenizedMessage>& Tok : Tokens)
	{
		const EMessageSeverity::Type Sev = Tok->GetSeverity();
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("message"), Tok->ToText().ToString());
		if (Sev == EMessageSeverity::Error)
		{
			E->SetStringField(TEXT("severity"), TEXT("error"));
			ErrorsJson.Add(MakeShared<FJsonValueObject>(E));
			++ErrorCount;
		}
		else if (Sev == EMessageSeverity::Warning || Sev == EMessageSeverity::PerformanceWarning)
		{
			E->SetStringField(TEXT("severity"), TEXT("warning"));
			WarningsJson.Add(MakeShared<FJsonValueObject>(E));
			++WarnCount;
		}
	}

	const bool bValid = bCompiled && ErrorCount == 0;
	TSharedPtr<FJsonObject> Data = StateTree_NewTreeData(Tree);
	Data->SetBoolField  (TEXT("valid"),         bValid);
	Data->SetBoolField  (TEXT("compiled"),      bCompiled);
	Data->SetNumberField(TEXT("error_count"),   ErrorCount);
	Data->SetNumberField(TEXT("warning_count"), WarnCount);
	Data->SetNumberField(TEXT("message_count"), Tokens.Num());
	Data->SetBoolField  (TEXT("saved"),         false); // explicit: dry-run never saves
	Data->SetArrayField (TEXT("errors"),        ErrorsJson);
	Data->SetArrayField (TEXT("warnings"),      WarningsJson);
	Data->SetStringField(TEXT("note"),
		bValid
			? TEXT("Tree compiled cleanly. Use compile_state_tree to commit.")
			: TEXT("Tree has compile errors. Fix the listed issues and re-run validate_state_tree."));
	return mcp_common::MakeOkReply(Data);
}
// ===========================================================================
// Theme colors + component state tree path
// ===========================================================================

// MCP:get_state_theme_colors - List the theme color palette of a StateTree asset (UStateTreeEditorData Colors) without changing it. Returns count and colors [{display_name, color_ref, r, g, b, a}], color_ref being the GUID that states point to. Takes state_tree_path, not tree_path; set colors with set_state_theme_color.
//   @param state_tree_path - Asset path of the StateTree, e.g. "/Game/AI/ST_Guard"; named state_tree_path here, unlike its sibling tools.
//   @returns { count, colors: [{display_name, color_ref, r, g, b, a}] }
TSharedPtr<FJsonObject> HandleGetStateThemeColors(const TSharedPtr<FJsonObject>& Params)
{
    FString TreePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_tree_path"), TreePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UStateTree* Tree = Cast<UStateTree>(StaticLoadObject(UStateTree::StaticClass(), nullptr, *TreePath));
    if (!Tree)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("StateTree not found: %s"), *TreePath));
    }
    UStateTreeEditorData* ED = Cast<UStateTreeEditorData>(Tree->EditorData);
    if (!ED)
    {
        return mcp_common::MakeErrorReply(TEXT("StateTree has no EditorData"));
    }

    TArray<TSharedPtr<FJsonValue>> ColorsJson;
    for (const FStateTreeEditorColor& C : ED->Colors)
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("display_name"), C.DisplayName);
        O->SetStringField(TEXT("color_ref"), C.ColorRef.ID.ToString(EGuidFormats::DigitsWithHyphens));
        O->SetNumberField(TEXT("r"), C.Color.R);
        O->SetNumberField(TEXT("g"), C.Color.G);
        O->SetNumberField(TEXT("b"), C.Color.B);
        O->SetNumberField(TEXT("a"), C.Color.A);
        ColorsJson.Add(MakeShared<FJsonValueObject>(O));
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), ColorsJson.Num());
    Data->SetArrayField(TEXT("colors"), ColorsJson);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_component_state_tree_path - Resolve a UStateTreeComponent on a placed actor and return the bound asset path (read-only).
//   @param actor_label - Actor label/name to inspect
//   @returns { actor_label, has_state_tree_component, state_tree_path }
TSharedPtr<FJsonObject> HandleGetComponentStateTreePath(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    FString ActorLabel;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    AActor* Actor = mcp_common::FindActorByName(World, ActorLabel);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorLabel));
    }

    // Resolve UStateTreeComponent class via FindObject (avoid hard dep on GameplayStateTreeModule plugin)
    UClass* STCompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayStateTreeModule.StateTreeComponent"));
    UActorComponent* STComp = STCompClass ? Actor->FindComponentByClass(STCompClass) : nullptr;
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_label"), ActorLabel);
    Data->SetBoolField(TEXT("has_state_tree_component"), STComp != nullptr);
    if (STComp)
    {
        // Find the state tree asset path via reflection: walk every FProperty on the component
        // class, find the first FSoftObjectProperty (the bound UStateTree asset reference), and read it.
        FString Path = TEXT("");
        for (TFieldIterator<FProperty> It(STComp->GetClass()); It; ++It)
        {
            if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(*It))
            {
                void* Addr = It->ContainerPtrToValuePtr<void>(STComp);
                FSoftObjectPtr* SoftPtr = (FSoftObjectPtr*)Addr;
                if (SoftPtr && !SoftPtr->IsNull()) { Path = SoftPtr->ToString(); break; }
            }
        }
        Data->SetStringField(TEXT("state_tree_path"), Path);
    }
    else
    {
        Data->SetStringField(TEXT("state_tree_path"), TEXT(""));
    }
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// add_consideration (utility AI on a state)
// ===========================================================================

// MCP:add_consideration - Attach a utility-AI consideration (any struct deriving from FStateTreeConsiderationBase) to a state's Considerations array on a UStateTree. Considerations rank selectable children by score (0..1) when the state's selection behavior is utility-driven (e.g. TrySelectChildrenWithHighestUtility). Friendlier aliases are accepted in addition to the full struct name: "Constant" -> StateTreeConstantConsideration, "FloatInput"/"Float Input" -> StateTreeFloatInputConsideration, "EnumInput"/"Enum Input" -> StateTreeEnumInputConsideration. Use set_consideration_property to set initial values like Constant.Constant or FloatInput.Input. Idempotent: re-adding the same struct-type appends another consideration (engine semantics; considerations of the same type are valid).
//   @param tree_path - Asset path of the UStateTree
//   @param state_name [string!] - Name of the state to attach the consideration to
//   @param consideration_class - Struct name (FStateTreeConstantConsideration / StateTreeConstantConsideration) or alias (Constant|FloatInput|Enum Input|...)
//   @returns { tree_path, state_name, consideration_class, consideration_count, added, saved }
TSharedPtr<FJsonObject> HandleAddConsideration(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ClassStr;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("consideration_class"), ClassStr)) { return Missing; }
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }

	// Friendlier aliases for the 3 built-in common considerations (state engine ships them as
	// FStateTreeConstant/FloatInput/EnumInputConsideration under UENUM(DisplayName="Constant"/etc.).
	// UHT strips the leading 'F' from struct reflection names, so ResolveScriptStructByName already
	// maps "FStateTreeConstantConsideration" and "StateTreeConstantConsideration" — aliases below cover
	// the user-facing DisplayNames "Constant" / "Float Input" / "Enum Input" which don't match any
	// reflection name directly.
	static const TMap<FString, FString> AliasMap = {
		{TEXT("Constant"),     TEXT("StateTreeConstantConsideration")},
		{TEXT("FloatInput"),   TEXT("StateTreeFloatInputConsideration")},
		{TEXT("Float Input"),  TEXT("StateTreeFloatInputConsideration")},
		{TEXT("EnumInput"),    TEXT("StateTreeEnumInputConsideration")},
		{TEXT("Enum Input"),   TEXT("StateTreeEnumInputConsideration")},
	};
	const FString* Resolved = AliasMap.Find(ClassStr);
	UScriptStruct* ConsType = StateTree_ResolveNodeStruct(Resolved ? *Resolved : ClassStr, FStateTreeConsiderationBase::StaticStruct(),
		FString::Printf(TEXT("Consideration struct not found: %s (alias or /Script path also tried)"), *ClassStr), Err);
	if (!ConsType) { return Err; }

	// Outer is the state (Considerations are state-scoped, unlike Evaluators/GlobalTasks which are tree-scoped).
	FStateTreeEditorNode& Node = State->Considerations.AddDefaulted_GetRef();
	Node.InitializeAs(ED, ConsType);
	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetStringField(TEXT("consideration_class"), ConsType->GetName());
	Data->SetNumberField(TEXT("consideration_count"), State->Considerations.Num());
	Data->SetBoolField(TEXT("added"), true);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_consideration"));
}

// ===========================================================================
// set_state_theme_color (preserves ColorRef UUID across updates)
// ===========================================================================

// MCP:set_state_theme_color - Upsert a theme color in UStateTreeEditorData::Colors. CRITICAL: if a color with the same DisplayName (case-insensitive) already exists, its ColorRef UUID is preserved — state-level ColorRef references keep resolving, and the editor retains the original color identity in the asset's change history. Newly-created colors get a fresh UUID; the caller is responsible for re-binding any state ColorRefs that referenced an orphaned previous UUID. r/g/b default to 1.0; a defaults to 1.0. The engine stores Colors as a TSet<FStateTreeEditorColor> keyed on ColorRef (display name uniqueness is the caller's responsibility — duplicate names are allowed by the engine but produce ambiguous lookups downstream).
//   @param state_tree_path - Asset path of the UStateTree
//   @param color_name - Display name (required, case-insensitive lookup)
//   @param r - red 0..1 (default 1.0)
//   @param g - green 0..1 (default 1.0)
//   @param b - blue 0..1 (default 1.0)
//   @param a - alpha 0..1 (default 1.0)
//   @returns { state_tree_path, color_name, color_ref, r, g, b, a, was_new, saved }
TSharedPtr<FJsonObject> HandleSetStateThemeColor(
	const TSharedPtr<FJsonObject>& Params)
{
	FString TreePath, ColorName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("state_tree_path"), TreePath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("color_name"), ColorName)) { return Missing; }
	UStateTree* Tree = nullptr;
	TSharedPtr<FJsonObject> Err;
	UStateTreeEditorData* ED = StateTree_LoadEditorData(TreePath, TEXT("StateTree has no EditorData"), Tree, Err);
	if (!ED) { return Err; }

	FLinearColor NewColor = FLinearColor::White;
	double R = 1.0, G = 1.0, B = 1.0, A = 1.0;
	if (Params->TryGetNumberField(TEXT("r"), R)) { NewColor.R = static_cast<float>(R); }
	if (Params->TryGetNumberField(TEXT("g"), G)) { NewColor.G = static_cast<float>(G); }
	if (Params->TryGetNumberField(TEXT("b"), B)) { NewColor.B = static_cast<float>(B); }
	if (Params->TryGetNumberField(TEXT("a"), A)) { NewColor.A = static_cast<float>(A); }

	// Look for an existing entry with the same DisplayName (case-insensitive). When found,
	// PRESERVE its ColorRef UUID across the update — states that reference this color keep
	// resolving, and the asset's change history retains the original color identity.
	FStateTreeEditorColor Updated;
	bool bFoundExisting = false;
	for (const FStateTreeEditorColor& Existing : ED->Colors)
	{
		if (Existing.DisplayName.Equals(ColorName, ESearchCase::IgnoreCase))
		{
			Updated = Existing;          // copies ColorRef + DisplayName + Color
			bFoundExisting = true;
			break;
		}
	}

	if (bFoundExisting)
	{
		// TSet<FStateTreeEditorColor> is keyed on ColorRef (operator== / GetTypeHash both
		// compare ColorRef only). Remove-then-re-add with the same ColorRef yields a
		// single deduped entry preserving the original identity.
		ED->Colors.Remove(Updated);
	}
	else
	{
		// Fresh entry — default constructor auto-assigns a new GUID via FGuid::NewGuid().
		Updated.ColorRef = FStateTreeEditorColorRef(FGuid::NewGuid());
	}
	Updated.DisplayName = ColorName;
	Updated.Color = NewColor;
	ED->Colors.Add(Updated);

	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("state_tree_path"), Tree->GetPathName());
	Data->SetStringField(TEXT("color_name"), ColorName);
	Data->SetStringField(TEXT("color_ref"), Updated.ColorRef.ID.ToString(EGuidFormats::DigitsWithHyphens));
	Data->SetNumberField(TEXT("r"), Updated.Color.R);
	Data->SetNumberField(TEXT("g"), Updated.Color.G);
	Data->SetNumberField(TEXT("b"), Updated.Color.B);
	Data->SetNumberField(TEXT("a"), Updated.Color.A);
	Data->SetBoolField(TEXT("was_new"), !bFoundExisting);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_state_theme_color"));
}

// ===========================================================================
// set_component_parameter_override (per-instance UStateTreeComponent)
// ===========================================================================

// MCP:set_component_parameter_override - Override a single UStateTree root parameter on a UStateTreeComponent living on a placed actor. CRITICAL: the implementation calls UStateTreeStateTreeReference::SetPropertyOverridden(ParamGuid, true) — without this, the runtime reads from the asset default and the per-instance value is silently dropped. The value is written via FInstancedPropertyBag::SetValueSerializedString so the engine parses the string into the parameter's actual type (Bool/Int/Float/Vector/Struct/...). The tree must already declare the parameter; add it with add_tree_parameter first. Use clear_overrides=true to remove the override (asset default takes over again) without writing a value. If the component has no bound tree, pass state_tree_path to auto-bind before overriding (cold-start convenience); the public UStateTreeComponent::SetStateTree(UStateTree*) handles the bind through reflection on the protected StateTreeRef member.
//   @param actor_label - Actor label/name that owns the UStateTreeComponent (resolved against the editor world)
//   @param parameter_name - Root parameter name to override (must exist on the bound tree)
//   @param value - Optional string-encoded value; type-driven by the parameter's FPropertyBagPropertyDesc
//   @param state_tree_path - Optional path to a UStateTree to bind the component to if no tree is already bound
//   @param clear_overrides - If true, drop all per-instance overrides on the component (asset defaults take over); value is ignored
//   @returns { actor_label, component_class, parameter_name, parameter_id, value, was_overridden, cleared, saved, bound_tree }
TSharedPtr<FJsonObject> HandleSetComponentParameterOverride(
	const TSharedPtr<FJsonObject>& Params)
{
	FString ActorLabel;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_label"), ActorLabel)) { return Missing; }
	bool bClearOverrides = false;
	Params->TryGetBoolField(TEXT("clear_overrides"), bClearOverrides);

	UWorld* World = nullptr;
	if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
	UStateTreeComponent* Comp = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor && Actor->GetActorLabel() == ActorLabel)
		{
			Comp = Actor->FindComponentByClass<UStateTreeComponent>();
			if (Comp) { break; }
		}
	}
	if (!Comp)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Actor '%s' not found or has no UStateTreeComponent"), *ActorLabel));
	}
	// UStateTreeComponent::StateTreeRef is protected; no public getter. Reach it
	// through FProperty reflection (FProperty::ContainerPtrToValuePtr is the canonical path
	// used by serialization / property accessors to bypass access control).
	FProperty* STRefProp = Comp->GetClass()->FindPropertyByName(TEXT("StateTreeRef"));
	FStructProperty* STRefStructProp = CastField<FStructProperty>(STRefProp);
	if (!STRefStructProp || STRefStructProp->Struct != FStateTreeReference::StaticStruct())
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Could not locate FStateTreeReference member on UStateTreeComponent (prop lookup failed)")));
	}
	FStateTreeReference* CompRefPtr = STRefStructProp->ContainerPtrToValuePtr<FStateTreeReference>(Comp);
	UStateTree* BoundTree = CompRefPtr ? CompRefPtr->GetMutableStateTree() : nullptr;
	FString StateTreePath;
	Params->TryGetStringField(TEXT("state_tree_path"), StateTreePath);

	// Cold-start path: if the component has no bound tree but the caller passed state_tree_path,
	// bind it via the PUBLIC UStateTreeComponent::SetStateTree(UStateTree*) API. This keeps the
	// reflection trickery scoped to read-side (we still need it for StateTreeRef access), and
	// avoids the user having to wire up another cmd just to bind.
	if (!BoundTree)
	{
		if (StateTreePath.IsEmpty())
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("StateTreeComponent on '%s' has no UStateTree asset bound; pass state_tree_path to auto-bind"), *ActorLabel));
		}
		UStateTree* TreeToBind = LoadObject<UStateTree>(nullptr, *StateTreePath);
		if (!TreeToBind)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("state_tree_path not found: %s"), *StateTreePath));
		}
		Comp->SetStateTree(TreeToBind);
		BoundTree = TreeToBind;
		// Refresh CompRefPtr: SetStateTree may have moved the underlying storage.
		CompRefPtr = STRefStructProp->ContainerPtrToValuePtr<FStateTreeReference>(Comp);
	}

	// Clear path: drop every per-instance override (asset defaults take over).
	// SetPropertyOverridden toggles a single GUID; the canonical reset is to re-bind the
	// same tree (SetStateTree calls SyncParameters which drops all overrides).
	if (bClearOverrides)
	{
		Comp->SetStateTree(BoundTree);
		Comp->Modify();
		if (AActor* Owner = Comp->GetOwner()) { Owner->Modify(); }
		const bool bSaved = mcp_common::SaveAssetPackage(Comp->GetPackage(), Comp->GetOwner());
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_label"), ActorLabel);
		Data->SetStringField(TEXT("component_class"), Comp->GetClass()->GetName());
		Data->SetBoolField(TEXT("cleared"), true);
		Data->SetStringField(TEXT("bound_tree"), BoundTree ? BoundTree->GetPathName() : TEXT(""));
		Data->SetBoolField(TEXT("saved"), bSaved);
		return mcp_common::MakeOkReply(Data);
	}

	// Add/update path: locate the parameter, set its value, mark as overridden.
	FString ParameterName, ValueStr;
	if (!Params->TryGetStringField(TEXT("parameter_name"), ParameterName) || ParameterName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("parameter_name is required when clear_overrides is false"));
	}
	if (!Params->TryGetStringField(TEXT("value"), ValueStr))
	{
		return mcp_common::MakeErrorReply(TEXT("value is required when clear_overrides is false"));
	}

	// Same reflection path for the parameter bag accessor.
	FInstancedPropertyBag& Bag = CompRefPtr->GetMutableParameters();
	const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(FName(*ParameterName));
	if (!Desc)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("Root parameter '%s' not found on the bound tree"), *ParameterName));
	}

	const EPropertyBagResult SetResult = Bag.SetValueSerializedString(FName(*ParameterName), ValueStr);
	if (SetResult != EPropertyBagResult::Success)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("SetValueSerializedString failed (%d) for parameter '%s' -- value '%s' is not parseable as the parameter type"),
			static_cast<int32>(SetResult), *ParameterName, *ValueStr));
	}

	// CRITICAL: without SetPropertyOverridden(true) the runtime reads the asset default
	// and the per-instance value is silently dropped. Source: StateTreeReference.h.
	CompRefPtr->SetPropertyOverridden(Desc->ID, true);

	Comp->Modify();
	if (AActor* Owner = Comp->GetOwner()) { Owner->Modify(); }
	const bool bSaved = mcp_common::SaveAssetPackage(Comp->GetPackage(), Comp->GetOwner());

	const bool bIsOverriddenNow = CompRefPtr->IsPropertyOverridden(Desc->ID);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_label"), ActorLabel);
	Data->SetStringField(TEXT("component_class"), Comp->GetClass()->GetName());
	Data->SetStringField(TEXT("parameter_name"), ParameterName);
	Data->SetStringField(TEXT("parameter_id"), Desc->ID.ToString(EGuidFormats::DigitsWithHyphens));
	Data->SetStringField(TEXT("value"), ValueStr);
	Data->SetBoolField(TEXT("was_overridden"), bIsOverriddenNow);
	Data->SetBoolField(TEXT("cleared"), false);
	Data->SetStringField(TEXT("bound_tree"), BoundTree ? BoundTree->GetPathName() : TEXT(""));
	Data->SetBoolField(TEXT("saved"), bSaved);
	return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// update_transition (comprehensive transition metadata update)
// ===========================================================================

// MCP:update_transition - Update any subset of a transition's metadata fields on a state. Only fields explicitly passed are written; everything else is preserved. Unknown enum strings are rejected (no silent no-ops on bad input). Superset of set_transition_properties: adds transition_type (None|Succeeded|Failed|GotoState|Parent|NextState|...), target_state (rebind by name), event_tag (FGameplayTag, when trigger=OnEvent), and payload_struct (event-payload struct, when trigger=OnEvent). Use this when set_transition_properties doesn't expose the field you need.
//   @param tree_path - Asset path of the UStateTree
//   @param state_name [string!] - State that owns the transition
//   @param transition_index - Index into the state's Transitions array (0-based)
//   @param trigger - Optional EStateTreeTransitionTrigger name (None|OnStateCompleted|OnStateTick|OnEvent|OnDelta|...)
//   @param priority - Optional EStateTreeTransitionPriority name (Lowest|Low|Normal|High|Highest)
//   @param transition_type - Optional EStateTreeTransitionType name (None|Succeeded|Failed|GotoState|Parent|NextState|NextSelectableState|NextParent|NextSelectableParent)
//   @param target_state - Optional re-target by state name (only when you want to redirect the edge)
//   @param event_tag - Optional event tag as a literal FGameplayTag string (e.g. "GameplayCue.Movement.Jumped")
//   @param payload_struct - Optional UScriptStruct name or /Script path for the event payload
//   @param enabled - Optional bool (defaults preserved when omitted)
//   @param delay - Optional bool (when true, delay_duration and delay_variance are honored by the editor)
//   @param delay_duration - Optional number (seconds)
//   @param delay_variance - Optional number (seconds of random variance)
//   @returns { tree_path, state_name, transition_index, applied_fields: [string], errors: [string], saved }
TSharedPtr<FJsonObject> HandleUpdateTransition(
	const TSharedPtr<FJsonObject>& Params)
{
	UStateTree* Tree = nullptr; UStateTreeEditorData* ED = nullptr; TSharedPtr<FJsonObject> Err;
	UStateTreeState* State = ResolveTreeAndState(Params, TEXT("state_name"), Tree, ED, Err);
	if (!State) { return Err; }
	int32 Ti = -1;
	{ double D = -1.0; if (Params->TryGetNumberField(TEXT("transition_index"), D)) { Ti = static_cast<int32>(D); } }
	if (!State->Transitions.IsValidIndex(Ti))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Invalid transition_index %d (state has %d transitions)"), Ti, State->Transitions.Num()));
	}
	FStateTreeTransition& T = State->Transitions[Ti];

	TArray<TSharedPtr<FJsonValue>> AppliedFieldsJson;
	TArray<TSharedPtr<FJsonValue>> ErrorsJson;

	// --- trigger (EStateTreeTransitionTrigger) ---
	FString TriggerStr;
	if (Params->TryGetStringField(TEXT("trigger"), TriggerStr) && !TriggerStr.IsEmpty())
	{
		EStateTreeTransitionTrigger Trig = EStateTreeTransitionTrigger::None;
		if (!ResolveEnumByName(TriggerStr, Trig))
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Unknown trigger: '%s'"), *TriggerStr)));
		}
		else
		{
			T.Trigger = Trig;
			AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("trigger")));
		}
	}

	// --- priority (EStateTreeTransitionPriority) ---
	FString PriorityStr;
	if (Params->TryGetStringField(TEXT("priority"), PriorityStr) && !PriorityStr.IsEmpty())
	{
		EStateTreeTransitionPriority Prio = EStateTreeTransitionPriority::Normal;
		if (!ResolveEnumByName(PriorityStr, Prio))
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Unknown priority: '%s'"), *PriorityStr)));
		}
		else
		{
			T.Priority = Prio;
			AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("priority")));
		}
	}

	// --- transition_type (EStateTreeTransitionType) — applied to T.State.LinkType ---
	FString TypeStr;
	if (Params->TryGetStringField(TEXT("transition_type"), TypeStr) && !TypeStr.IsEmpty())
	{
		EStateTreeTransitionType NewType = EStateTreeTransitionType::None;
		if (!ResolveEnumByName(TypeStr, NewType))
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("Unknown transition_type: '%s'"), *TypeStr)));
		}
		else
		{
			T.State.LinkType = NewType;
			AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("transition_type")));
		}
	}

	// --- target_state (rebind by name -> resolve to state ID) ---
	FString TargetStateName;
	if (Params->TryGetStringField(TEXT("target_state"), TargetStateName) && !TargetStateName.IsEmpty())
	{
		UStateTreeState* TargetState = FindStateByName(ED, FName(*TargetStateName));
		if (!TargetState)
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("target_state not found: '%s'"), *TargetStateName)));
		}
		else
		{
			T.State.Name = FName(*TargetStateName);
			T.State.ID = TargetState->ID;
			AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("target_state")));
		}
	}

	// --- event_tag (FGameplayTag string) — only meaningful when trigger=OnEvent ---
	FString EventTagStr;
	if (Params->TryGetStringField(TEXT("event_tag"), EventTagStr) && !EventTagStr.IsEmpty())
	{
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*EventTagStr), /*ErrorIfNotFound=*/false);
		if (!Tag.IsValid())
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("event_tag is not a registered FGameplayTag: '%s' (add it to a GameplayTag table first)"), *EventTagStr)));
		}
		else
		{
			T.RequiredEvent.Tag = Tag;
			AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("event_tag")));
		}
	}

	// --- payload_struct (UScriptStruct name) — only meaningful when trigger=OnEvent ---
	FString PayloadStructName;
	if (Params->TryGetStringField(TEXT("payload_struct"), PayloadStructName) && !PayloadStructName.IsEmpty())
	{
		UScriptStruct* PayloadType = ResolveScriptStructByName(PayloadStructName);
		if (!PayloadType)
		{
			ErrorsJson.Add(MakeShared<FJsonValueString>(FString::Printf(
				TEXT("payload_struct not found: '%s' (use full struct name or /Script path)"), *PayloadStructName)));
		}
		else
		{
			T.RequiredEvent.PayloadStruct = PayloadType;
			AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("payload_struct")));
		}
	}

	// --- enabled (bTransitionEnabled) ---
	bool bEnabled = true;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		T.bTransitionEnabled = bEnabled;
		AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("enabled")));
	}

	// --- delay (bDelayTransition) + delay_duration + delay_variance ---
	bool bDelay = false;
	const bool bHasDelay = Params->TryGetBoolField(TEXT("delay"), bDelay);
	if (bHasDelay)
	{
		T.bDelayTransition = bDelay;
		AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("delay")));
	}
	double DelayDur = 0.0;
	if (Params->TryGetNumberField(TEXT("delay_duration"), DelayDur))
	{
		T.DelayDuration = static_cast<float>(DelayDur);
		AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("delay_duration")));
	}
	double DelayVar = 0.0;
	if (Params->TryGetNumberField(TEXT("delay_variance"), DelayVar))
	{
		T.DelayRandomVariance = static_cast<float>(DelayVar);
		AppliedFieldsJson.Add(MakeShared<FJsonValueString>(TEXT("delay_variance")));
	}

	const bool bSaved = mcp_common::SaveAssetPackage(Tree->GetPackage(), Tree);

	TSharedPtr<FJsonObject> Data = StateTree_NewStateData(Tree, State);
	Data->SetNumberField(TEXT("transition_index"), Ti);
	Data->SetStringField(TEXT("trigger"), EnumToName(T.Trigger));
	Data->SetStringField(TEXT("priority"), EnumToName(T.Priority));
	Data->SetStringField(TEXT("transition_type"), EnumToName(T.State.LinkType));
	Data->SetStringField(TEXT("target_state"), T.State.Name.ToString());
	Data->SetStringField(TEXT("event_tag"), T.RequiredEvent.Tag.ToString());
	Data->SetStringField(TEXT("payload_struct"),
		T.RequiredEvent.PayloadStruct ? T.RequiredEvent.PayloadStruct->GetName() : TEXT(""));
	Data->SetBoolField(TEXT("enabled"), T.bTransitionEnabled);
	Data->SetBoolField(TEXT("delay"), T.bDelayTransition);
	Data->SetNumberField(TEXT("delay_duration"), T.DelayDuration);
	Data->SetNumberField(TEXT("delay_variance"), T.DelayRandomVariance);
	Data->SetArrayField(TEXT("applied_fields"), AppliedFieldsJson);
	Data->SetArrayField(TEXT("errors"), ErrorsJson);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("update_transition"));
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_consideration"), &HandleAddConsideration },
	{ TEXT("add_state"), &HandleAddState },
	{ TEXT("add_state_enter_condition"), &HandleAddStateEnterCondition },
	{ TEXT("add_state_evaluator"), &HandleAddStateEvaluator },
	{ TEXT("add_state_task"), &HandleAddStateTask },
	{ TEXT("add_state_transition"), &HandleAddStateTransition },
	{ TEXT("add_state_tree_binding"), &HandleAddStateTreeBinding },
	{ TEXT("add_state_tree_global_task"), &HandleAddStateTreeGlobalTask },
	{ TEXT("add_state_tree_global_task_property"), &HandleAddStateTreeGlobalTaskProperty },
	{ TEXT("add_transition_condition"), &HandleAddTransitionCondition },
	{ TEXT("add_tree_parameter"), &HandleAddTreeParameter },
	{ TEXT("compile_state_tree"), &HandleCompileStateTree },
	{ TEXT("create_state_tree"), &HandleCreateStateTree },
	{ TEXT("delete_state_tree"), &HandleDeleteStateTree },
	{ TEXT("get_component_state_tree_path"), &HandleGetComponentStateTreePath },
	{ TEXT("get_condition_info"), &HandleGetConditionInfo },
	{ TEXT("get_condition_property"), &HandleGetConditionProperty },
	{ TEXT("get_consideration_info"), &HandleGetConsiderationInfo },
	{ TEXT("get_consideration_property"), &HandleGetConsiderationProperty },
	{ TEXT("get_evaluator_info"), &HandleGetEvaluatorInfo },
	{ TEXT("get_evaluator_property"), &HandleGetEvaluatorProperty },
	{ TEXT("get_state_theme_colors"), &HandleGetStateThemeColors },
	{ TEXT("get_state_tree_info"), &HandleGetStateTreeInfo },
	{ TEXT("get_task_info"), &HandleGetTaskInfo },
	{ TEXT("get_task_property"), &HandleGetTaskProperty },
	{ TEXT("list_condition_types"), &HandleListConditionTypes },
	{ TEXT("list_conditions"), &HandleListConditions },
	{ TEXT("list_considerations"), &HandleListConsiderations },
	{ TEXT("list_evaluator_types"), &HandleListEvaluatorTypes },
	{ TEXT("list_evaluators"), &HandleListEvaluators },
	{ TEXT("list_global_tasks"), &HandleListGlobalTasks },
	{ TEXT("list_state_trees"), &HandleListStateTrees },
	{ TEXT("list_states"), &HandleListStates },
	{ TEXT("list_task_types"), &HandleListTaskTypes },
	{ TEXT("list_tasks"), &HandleListTasks },
	{ TEXT("list_transitions"), &HandleListTransitions },
	{ TEXT("list_tree_bindings"), &HandleListTreeBindings },
	{ TEXT("list_tree_parameters"), &HandleListTreeParameters },
	{ TEXT("remove_consideration"), &HandleRemoveConsideration },
	{ TEXT("remove_evaluator"), &HandleRemoveEvaluator },
	{ TEXT("remove_state"), &HandleRemoveState },
	{ TEXT("remove_state_enter_condition"), &HandleRemoveStateEnterCondition },
	{ TEXT("remove_state_tree_global_task"), &HandleRemoveStateTreeGlobalTask },
	{ TEXT("remove_task"), &HandleRemoveTask },
	{ TEXT("remove_transition"), &HandleRemoveTransition },
	{ TEXT("remove_transition_condition"), &HandleRemoveTransitionCondition },
	{ TEXT("remove_tree_binding"), &HandleRemoveTreeBinding },
	{ TEXT("remove_tree_parameter"), &HandleRemoveTreeParameter },
	{ TEXT("set_component_parameter_override"), &HandleSetComponentParameterOverride },
	{ TEXT("set_condition_property"), &HandleSetConditionProperty },
	{ TEXT("set_consideration_property"), &HandleSetConsiderationProperty },
	{ TEXT("set_evaluator_property"), &HandleSetEvaluatorProperty },
	{ TEXT("set_state_completion_type"), &HandleSetStateCompletionType },
	{ TEXT("set_state_linked_asset"), &HandleSetStateLinkedAsset },
	{ TEXT("set_state_selection_behavior"), &HandleSetStateSelectionBehavior },
	{ TEXT("set_state_theme_color"), &HandleSetStateThemeColor },
	{ TEXT("set_state_tree_root"), &HandleSetStateTreeRoot },
	{ TEXT("set_state_type"), &HandleSetStateType },
	{ TEXT("set_task_property"), &HandleSetTaskProperty },
	{ TEXT("set_transition_properties"), &HandleSetTransitionProperties },
	{ TEXT("set_tree_parameter"), &HandleSetTreeParameter },
	{ TEXT("update_transition"), &HandleUpdateTransition },
	{ TEXT("validate_state_tree"), &HandleValidateStateTree },
};
} // namespace UEAgentStateTreeCommands
