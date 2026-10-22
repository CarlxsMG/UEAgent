// Copyright (c) 2026 CarlxsMG.

#include "Commands/Common/McpGraphRouter.h"

#include "Commands/Common/McpCommonUtils.h"

#include "Dom/JsonValue.h"
#include "UObject/Class.h"

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpGraphRouterPrivate
{
	enum class EKind : uint8
	{
		Blueprint,
		Material,
		MaterialFunction,
		SoundCue,
		MetaSound,
		PCG,
		ControlRig,
		MaterialInstance,
		Other,
	};

	static const TCHAR* const GraphInput = TEXT("@input");
	static const TCHAR* const GraphOutput = TEXT("@output");

	static bool IsA(const UClass* Class, const TCHAR* Name)
	{
		for (const UClass* C = Class; C; C = C->GetSuperClass())
		{
			if (C->GetName() == Name)
			{
				return true;
			}
		}
		return false;
	}

	/** The kind by class name, so this file needs none of the families' modules. A Control Rig is a Blueprint too: it goes first. */
	static EKind KindOf(const UObject* Asset)
	{
		const UClass* Class = Asset->GetClass();
		if (IsA(Class, TEXT("RigVMBlueprint")))
		{
			return EKind::ControlRig;
		}
		if (IsA(Class, TEXT("Blueprint")))
		{
			return EKind::Blueprint;
		}
		if (IsA(Class, TEXT("MaterialFunction")))
		{
			return EKind::MaterialFunction;
		}
		if (IsA(Class, TEXT("Material")))
		{
			return EKind::Material;
		}
		if (IsA(Class, TEXT("MaterialInstance")))
		{
			return EKind::MaterialInstance;
		}
		if (IsA(Class, TEXT("SoundCue")))
		{
			return EKind::SoundCue;
		}
		if (IsA(Class, TEXT("MetaSoundSource")) || IsA(Class, TEXT("MetaSoundPatch")))
		{
			return EKind::MetaSound;
		}
		if (IsA(Class, TEXT("PCGGraph")))
		{
			return EKind::PCG;
		}
		return EKind::Other;
	}

	static const TCHAR* KindName(EKind Kind)
	{
		switch (Kind)
		{
		case EKind::Material:         return TEXT("material");
		case EKind::MaterialFunction: return TEXT("material_function");
		case EKind::SoundCue:         return TEXT("sound_cue");
		case EKind::MetaSound:        return TEXT("metasound");
		case EKind::PCG:              return TEXT("pcg");
		case EKind::ControlRig:       return TEXT("control_rig");
		default:                      return TEXT("blueprint");
		}
	}

	/** A string argument, also when the model sent it as a number (a sound cue's node index). */
	static FString Arg(const TSharedPtr<FJsonObject>& Args, const TCHAR* Key)
	{
		TSharedPtr<FJsonValue> Value;
		if (Args.IsValid())
		{
			Value = Args->TryGetField(Key);
		}
		if (!Value.IsValid() || Value->IsNull())
		{
			return FString();
		}
		double Number = 0.0;
		if (Value->Type == EJson::Number && Value->TryGetNumber(Number))
		{
			return FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)) ? FString::Printf(TEXT("%lld"), static_cast<int64>(Number)) : Value->AsString();
		}
		FString Text;
		Value->TryGetString(Text);
		return Text.TrimStartAndEnd();
	}

	/** A sound cue node id or input slot: a whole number at least 0. */
	static bool ReadIndex(const FString& Text, int32& Out)
	{
		if (Text.IsEmpty() || !Text.IsNumeric() || Text.Contains(TEXT(".")) || Text.StartsWith(TEXT("-")))
		{
			return false;
		}
		Out = FCString::Atoi(*Text);
		return true;
	}

	/** Where a reply keeps its fields: under `data` when the tool wraps them (MakeOkReply), else at the top. */
	static TSharedPtr<FJsonObject> Payload(const TSharedPtr<FJsonObject>& Reply)
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Reply.IsValid() && Reply->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid())
		{
			return *Data;
		}
		return Reply;
	}

	static bool Succeeded(const TSharedPtr<FJsonObject>& Reply)
	{
		bool bSuccess = true;
		return Reply.IsValid() && (!Reply->TryGetBoolField(TEXT("success"), bSuccess) || bSuccess);
	}

	static TSharedPtr<FJsonObject> Refuse(const FString& Message)
	{
		return mcp_common::MakeErrorReply(Message);
	}

	/** The call Tool makes as Command; the reply gets `graph_kind`, and `node_id` from NodeIdField when it has one. */
	static TSharedPtr<FJsonObject> Forward(const TCHAR* Tool, EKind Kind, const TCHAR* Command, const TSharedRef<FJsonObject>& Args,
		const TCHAR* NodeIdField = nullptr)
	{
		const TSharedPtr<FJsonObject> Reply = mcp_common::ForwardCall(Tool, Command, Args);
		const TSharedPtr<FJsonObject> Fields = Payload(Reply);
		if (Fields.IsValid())
		{
			Fields->SetStringField(TEXT("graph_kind"), KindName(Kind));
			if (NodeIdField && Succeeded(Reply))
			{
				const FString NodeId = Arg(Fields, NodeIdField);
				if (!NodeId.IsEmpty())
				{
					Fields->SetStringField(TEXT("node_id"), NodeId);
				}
			}
		}
		return Reply;
	}

	/** Every entry of the reply's `nodes` gets `node_id` from IdField; with WantedId, only that node stays (an error when it is not there). */
	static TSharedPtr<FJsonObject> NameNodes(const TSharedPtr<FJsonObject>& Reply, const TCHAR* IdField, const FString& WantedId)
	{
		const TSharedPtr<FJsonObject> Fields = Payload(Reply);
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Succeeded(Reply) || !Fields.IsValid() || !Fields->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			return Reply;
		}
		TArray<TSharedPtr<FJsonValue>> Kept;
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject> Node = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : TSharedPtr<FJsonObject>();
			if (!Node.IsValid())
			{
				continue;
			}
			const FString Id = Arg(Node, IdField);
			Node->SetStringField(TEXT("node_id"), Id);
			if (WantedId.IsEmpty() || Id.Equals(WantedId, ESearchCase::IgnoreCase))
			{
				Kept.Add(Value);
			}
		}
		if (!WantedId.IsEmpty())
		{
			if (Kept.Num() == 0)
			{
				return Refuse(FString::Printf(TEXT("Node not found: %s (get_graph_nodes without node_id lists the graph's nodes)"), *WantedId));
			}
			Fields->SetArrayField(TEXT("nodes"), Kept);
		}
		return Reply;
	}

	/** One string argument of a forwarded call. */
	struct FArg
	{
		const TCHAR* Key;
		FString Value;
	};

	static TSharedRef<FJsonObject> Call(std::initializer_list<FArg> Strings)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FArg& Entry : Strings)
		{
			Out->SetStringField(Entry.Key, Entry.Value);
		}
		return Out;
	}

	/** position_x/position_y, else node_position [x, y]; false when the call gives neither. */
	static bool ReadNodePosition(const TSharedPtr<FJsonObject>& Args, double& OutX, double& OutY)
	{
		OutX = 0.0;
		OutY = 0.0;
		const bool bX = Args->TryGetNumberField(TEXT("position_x"), OutX);
		const bool bY = Args->TryGetNumberField(TEXT("position_y"), OutY);
		if (bX || bY)
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
		if (Args->TryGetArrayField(TEXT("node_position"), Position) && Position && Position->Num() >= 2)
		{
			(*Position)[0]->TryGetNumber(OutX);
			(*Position)[1]->TryGetNumber(OutY);
			return true;
		}
		return false;
	}

	/** A material's asset parameter: material_path, or material_function_path for a function. */
	static const TCHAR* MaterialKey(EKind Kind)
	{
		return Kind == EKind::MaterialFunction ? TEXT("material_function_path") : TEXT("material_path");
	}

	// ---- get_graph_nodes ----------------------------------------------------------------------------------------

	static TSharedPtr<FJsonObject> GetGraphNodes(const TCHAR* Tool, EKind Kind, const FString& Path, const TSharedPtr<FJsonObject>& Args)
	{
		const FString NodeId = Arg(Args, TEXT("node_id"));
		switch (Kind)
		{
		case EKind::Material:
		case EKind::MaterialFunction:
			return NameNodes(Forward(Tool, Kind, TEXT("get_material_graph_info"), Call({ { MaterialKey(Kind), Path } })), TEXT("name"), NodeId);
		case EKind::SoundCue:
			return NameNodes(Forward(Tool, Kind, TEXT("get_sound_cue_graph"), Call({ { TEXT("cue_path"), Path } })), TEXT("id"), NodeId);
		case EKind::PCG:
			return NameNodes(Forward(Tool, Kind, TEXT("get_pcg_graph_info"), Call({ { TEXT("pcg_graph_path"), Path } })), TEXT("node_name"), NodeId);
		case EKind::ControlRig:
			return NameNodes(Forward(Tool, Kind, TEXT("get_rigvm_graph"), Call({ { TEXT("control_rig_path"), Path } })), TEXT("name"), NodeId);
		case EKind::MetaSound:
		{
			// Two reads: the node (or every node) and the graph's connections. Both are forwarded, so no one tool's contract
			// applies to the merged reply.
			TSharedPtr<FJsonObject> Reply = NodeId.IsEmpty()
				? NameNodes(Forward(Tool, Kind, TEXT("list_metasound_nodes"), Call({ { TEXT("metasound_path"), Path } })), TEXT("id"), FString())
				: Forward(Tool, Kind, TEXT("get_metasound_node_info"), Call({ { TEXT("metasound_path"), Path }, { TEXT("node_id"), NodeId } }), TEXT("node_id"));
			if (!Succeeded(Reply))
			{
				return Reply;
			}
			const TSharedPtr<FJsonObject> Links = Payload(Forward(Tool, Kind, TEXT("list_metasound_connections"), Call({ { TEXT("metasound_path"), Path } })));
			const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
			TArray<TSharedPtr<FJsonValue>> Kept;
			if (Links.IsValid() && Links->TryGetArrayField(TEXT("connections"), Connections) && Connections)
			{
				for (const TSharedPtr<FJsonValue>& Link : *Connections)
				{
					const TSharedPtr<FJsonObject> Edge = Link.IsValid() && Link->Type == EJson::Object ? Link->AsObject() : TSharedPtr<FJsonObject>();
					if (Edge.IsValid() && (NodeId.IsEmpty() || Arg(Edge, TEXT("from_node_id")).Equals(NodeId, ESearchCase::IgnoreCase)
						|| Arg(Edge, TEXT("to_node_id")).Equals(NodeId, ESearchCase::IgnoreCase)))
					{
						Kept.Add(Link);
					}
				}
			}
			Payload(Reply)->SetArrayField(TEXT("connections"), Kept);
			Payload(Reply)->SetNumberField(TEXT("connection_count"), Kept.Num());
			return Reply;
		}
		default:
			return nullptr;
		}
	}

	// ---- add_graph_node -----------------------------------------------------------------------------------------

	static TSharedPtr<FJsonObject> AddGraphNode(const TCHAR* Tool, EKind Kind, const FString& Path, const TSharedPtr<FJsonObject>& Args)
	{
		const FString NodeType = Arg(Args, TEXT("node_type"));
		double X = 0.0, Y = 0.0;
		const bool bPosition = ReadNodePosition(Args, X, Y);
		TSharedPtr<FJsonObject> Reply;
		switch (Kind)
		{
		case EKind::Material:
		case EKind::MaterialFunction:
		{
			TSharedRef<FJsonObject> Call = MakeShared<FJsonObject>();
			Call->SetStringField(MaterialKey(Kind), Path);
			Call->SetStringField(TEXT("expression_class_name"), NodeType);
			if (bPosition)
			{
				Call->SetNumberField(TEXT("node_pos_x"), X);
				Call->SetNumberField(TEXT("node_pos_y"), Y);
			}
			return Forward(Tool, Kind, TEXT("create_material_expression"), Call, TEXT("expression_name"));
		}
		case EKind::SoundCue:
		{
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("cue_path"), Path }, { TEXT("node_type"), NodeType } });
			const FString Wave = Arg(Args, TEXT("sound_wave_path"));
			if (!Wave.IsEmpty())
			{
				Call->SetStringField(TEXT("sound_wave_path"), Wave);
			}
			Reply = Forward(Tool, Kind, TEXT("add_sound_cue_node"), Call, TEXT("node_id"));
			break;
		}
		case EKind::MetaSound:
		{
			// node_type is the class path list_metasound_node_types gives: Namespace.Name[.Variant].
			TArray<FString> Parts;
			NodeType.ParseIntoArray(Parts, TEXT("."), /*bCullEmpty*/ false);
			if (Parts.Num() < 2 || Parts[0].IsEmpty() || Parts[1].IsEmpty())
			{
				return Refuse(FString::Printf(TEXT("%s: for a MetaSound, node_type is a class path Namespace.Name.Variant, such as UE.Sine.Audio, as list_metasound_node_types lists it; got '%s'"), Tool, *NodeType));
			}
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("metasound_path"), Path }, { TEXT("class_namespace"), Parts[0] }, { TEXT("class_name"), Parts[1] } });
			if (Parts.Num() > 2)
			{
				TArray<FString> Variant(Parts);
				Variant.RemoveAt(0, 2);
				Call->SetStringField(TEXT("class_variant"), FString::Join(Variant, TEXT(".")));
			}
			double MajorVersion = 0.0;
			if (Args->TryGetNumberField(TEXT("major_version"), MajorVersion))
			{
				Call->SetNumberField(TEXT("major_version"), MajorVersion);
			}
			Reply = Forward(Tool, Kind, TEXT("add_metasound_node"), Call, TEXT("node_id"));
			break;
		}
		case EKind::PCG:
		{
			const FString Subgraph = Arg(Args, TEXT("subgraph_path"));
			Reply = Subgraph.IsEmpty()
				? Forward(Tool, Kind, TEXT("add_pcg_node"), Call({ { TEXT("pcg_graph_path"), Path }, { TEXT("node_type"), NodeType } }), TEXT("node_name"))
				: Forward(Tool, Kind, TEXT("add_pcg_subgraph_node"), Call({ { TEXT("pcg_graph_path"), Path }, { TEXT("subgraph_path"), Subgraph } }), TEXT("node_name"));
			break;
		}
		case EKind::ControlRig:
		{
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("control_rig_path"), Path }, { TEXT("node_type"), NodeType } });
			const FString NodeName = Arg(Args, TEXT("node_name"));
			if (!NodeName.IsEmpty())
			{
				Call->SetStringField(TEXT("node_name"), NodeName);
			}
			if (bPosition)
			{
				TArray<TSharedPtr<FJsonValue>> Position;
				Position.Add(MakeShared<FJsonValueNumber>(X));
				Position.Add(MakeShared<FJsonValueNumber>(Y));
				Call->SetArrayField(TEXT("position"), Position);
			}
			return Forward(Tool, Kind, TEXT("add_rigvm_node"), Call, TEXT("node_name"));
		}
		default:
			return nullptr;
		}
		// Sound cue, MetaSound and PCG nodes have no position these tools set.
		if (bPosition && Succeeded(Reply) && Payload(Reply).IsValid())
		{
			Payload(Reply)->SetBoolField(TEXT("position_ignored"), true);
		}
		return Reply;
	}

	// ---- remove_graph_node --------------------------------------------------------------------------------------

	static TSharedPtr<FJsonObject> RemoveGraphNode(const TCHAR* Tool, EKind Kind, const FString& Path, const TSharedPtr<FJsonObject>& Args)
	{
		const FString NodeId = Arg(Args, TEXT("node_id"));
		// The graph's own input and output are part of the graph, not nodes to take out: the family tools would read the
		// id as a name they do not know, and one of them answers that as already removed.
		if (NodeId == GraphInput || NodeId == GraphOutput)
		{
			return Refuse(FString::Printf(TEXT("%s: %s is the graph's own %s, which cannot be removed. Nothing was removed."), Tool, *NodeId,
				NodeId == GraphInput ? TEXT("input") : TEXT("output")));
		}
		switch (Kind)
		{
		case EKind::Material:
		case EKind::MaterialFunction:
			return Forward(Tool, Kind, TEXT("delete_material_expression"), Call({ { MaterialKey(Kind), Path }, { TEXT("expression_name"), NodeId } }));
		case EKind::SoundCue:
		{
			int32 Index = 0;
			if (!ReadIndex(NodeId, Index))
			{
				return Refuse(FString::Printf(TEXT("%s: a sound cue node's node_id is its index, a whole number; got '%s'"), Tool, *NodeId));
			}
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("cue_path"), Path } });
			Call->SetNumberField(TEXT("node_id"), Index);
			return Forward(Tool, Kind, TEXT("remove_sound_cue_node"), Call);
		}
		case EKind::MetaSound:
			return Forward(Tool, Kind, TEXT("remove_metasound_node"), Call({ { TEXT("metasound_path"), Path }, { TEXT("node_id"), NodeId } }));
		case EKind::PCG:
			return Forward(Tool, Kind, TEXT("remove_pcg_node"), Call({ { TEXT("pcg_graph_path"), Path }, { TEXT("node_name"), NodeId } }));
		case EKind::ControlRig:
			return Forward(Tool, Kind, TEXT("remove_rigvm_node"), Call({ { TEXT("control_rig_path"), Path }, { TEXT("node_name"), NodeId } }));
		default:
			return nullptr;
		}
	}

	// ---- connect_pins -------------------------------------------------------------------------------------------

	static TSharedPtr<FJsonObject> ConnectPins(const TCHAR* Tool, EKind Kind, const FString& Path, const TSharedPtr<FJsonObject>& Args)
	{
		const FString Source = Arg(Args, TEXT("source_node_id"));
		const FString SourcePin = Arg(Args, TEXT("source_pin"));
		const FString Target = Arg(Args, TEXT("target_node_id"));
		const FString TargetPin = Arg(Args, TEXT("target_pin"));
		const bool bFromInput = Source == GraphInput;
		const bool bToOutput = Target == GraphOutput;
		if (Source == GraphOutput || Target == GraphInput)
		{
			return Refuse(FString::Printf(TEXT("%s: a link goes from a node, or @input, to a node, or @output"), Tool));
		}
		switch (Kind)
		{
		case EKind::Material:
			if (bFromInput)
			{
				return Refuse(FString::Printf(TEXT("%s: a material has no graph input; @output names its main inputs (BaseColor, Roughness...)"), Tool));
			}
			return bToOutput
				? Forward(Tool, Kind, TEXT("connect_to_material_output"), Call({ { TEXT("material_path"), Path }, { TEXT("from_node_id"), Source }, { TEXT("output_property"), TargetPin }, { TEXT("from_pin"), SourcePin } }))
				: Forward(Tool, Kind, TEXT("connect_material_expressions"), Call({ { TEXT("material_path"), Path }, { TEXT("from_expression_name"), Source },
					{ TEXT("from_output_name"), SourcePin }, { TEXT("to_expression_name"), Target }, { TEXT("to_input_name"), TargetPin } }));
		case EKind::MaterialFunction:
			if (bFromInput || bToOutput)
			{
				return Refuse(FString::Printf(TEXT("%s: a material function's inputs and outputs are its FunctionInput and FunctionOutput expressions: name them by node_id"), Tool));
			}
			return Forward(Tool, Kind, TEXT("connect_material_function_pins"), Call({ { TEXT("material_function_path"), Path }, { TEXT("from_node_id"), Source },
				{ TEXT("to_node_id"), Target }, { TEXT("from_pin"), SourcePin }, { TEXT("to_input"), TargetPin } }));
		case EKind::SoundCue:
		{
			int32 Child = 0, Parent = 0, Slot = 0;
			if (bFromInput || !ReadIndex(Source, Child))
			{
				return Refuse(FString::Printf(TEXT("%s: a sound cue node's node_id is its index, a whole number; got source_node_id '%s'"), Tool, *Source));
			}
			if (bToOutput)
			{
				TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("cue_path"), Path } });
				Call->SetNumberField(TEXT("node_id"), Child);
				return Forward(Tool, Kind, TEXT("set_sound_cue_first_node"), Call);
			}
			if (!ReadIndex(Target, Parent))
			{
				return Refuse(FString::Printf(TEXT("%s: a sound cue node's node_id is its index, a whole number; got target_node_id '%s'"), Tool, *Target));
			}
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("cue_path"), Path } });
			Call->SetNumberField(TEXT("parent_node_id"), Parent);
			Call->SetNumberField(TEXT("child_node_id"), Child);
			if (!TargetPin.IsEmpty())
			{
				if (!ReadIndex(TargetPin, Slot))
				{
					return Refuse(FString::Printf(TEXT("%s: a sound cue node's input is a slot index, a whole number, or empty for the first free one; got target_pin '%s'"), Tool, *TargetPin));
				}
				Call->SetNumberField(TEXT("input_index"), Slot);
			}
			return Forward(Tool, Kind, TEXT("connect_sound_cue_nodes"), Call);
		}
		case EKind::MetaSound:
			if (bFromInput && bToOutput)
			{
				return Forward(Tool, Kind, TEXT("connect_metasound_graph_input_to_output"), Call({ { TEXT("metasound_path"), Path }, { TEXT("from_node_output_name"), SourcePin }, { TEXT("to_node_input_name"), TargetPin } }));
			}
			if (bFromInput)
			{
				return Forward(Tool, Kind, TEXT("connect_metasound_graph_input_to_node"), Call({ { TEXT("metasound_path"), Path }, { TEXT("graph_input_name"), SourcePin },
					{ TEXT("to_node_id"), Target }, { TEXT("to_input_name"), TargetPin } }));
			}
			if (bToOutput)
			{
				return Forward(Tool, Kind, TEXT("connect_metasound_node_to_graph_output"), Call({ { TEXT("metasound_path"), Path }, { TEXT("node_id"), Source },
					{ TEXT("node_output_name"), SourcePin }, { TEXT("graph_output_name"), TargetPin } }));
			}
			return Forward(Tool, Kind, TEXT("connect_metasound_nodes"), Call({ { TEXT("metasound_path"), Path }, { TEXT("from_node_id"), Source },
				{ TEXT("from_output_name"), SourcePin }, { TEXT("to_node_id"), Target }, { TEXT("to_input_name"), TargetPin } }));
		case EKind::PCG:
			return Forward(Tool, Kind, TEXT("connect_pcg_nodes"), Call({ { TEXT("pcg_graph_path"), Path }, { TEXT("from_node"), bFromInput ? FString(TEXT("Input")) : Source },
				{ TEXT("from_pin"), SourcePin }, { TEXT("to_node"), bToOutput ? FString(TEXT("Output")) : Target }, { TEXT("to_pin"), TargetPin } }));
		case EKind::ControlRig:
			if (bFromInput || bToOutput)
			{
				return Refuse(FString::Printf(TEXT("%s: a Control Rig's graph has no @input or @output; link its nodes' pins"), Tool));
			}
			return Forward(Tool, Kind, TEXT("add_rigvm_link"), Call({ { TEXT("control_rig_path"), Path }, { TEXT("source_pin"), Source + TEXT(".") + SourcePin },
				{ TEXT("target_pin"), Target + TEXT(".") + TargetPin } }));
		default:
			return nullptr;
		}
	}

	// ---- disconnect_pin -----------------------------------------------------------------------------------------

	static TSharedPtr<FJsonObject> DisconnectPin(const TCHAR* Tool, EKind Kind, const FString& Path, const TSharedPtr<FJsonObject>& Args)
	{
		const FString NodeId = Arg(Args, TEXT("node_id"));
		const FString PinName = Arg(Args, TEXT("pin_name"));
		const FString Linked = Arg(Args, TEXT("linked_node_id"));
		const FString LinkedPin = Arg(Args, TEXT("linked_pin"));
		switch (Kind)
		{
		case EKind::Material:
		case EKind::MaterialFunction:
			// An expression input holds one link: the input names it.
			if (NodeId == GraphOutput)
			{
				if (Kind == EKind::MaterialFunction)
				{
					return Refuse(FString::Printf(TEXT("%s: a material function's outputs are its FunctionOutput expressions: name the input of one by node_id"), Tool));
				}
				return Forward(Tool, Kind, TEXT("disconnect_to_material_output"), Call({ { TEXT("material_path"), Path }, { TEXT("output_property"), PinName } }));
			}
			return Forward(Tool, Kind, TEXT("disconnect_material_expressions"), Call({ { MaterialKey(Kind), Path }, { TEXT("to_expression_name"), NodeId }, { TEXT("to_input_name"), PinName } }));
		case EKind::SoundCue:
		{
			int32 Parent = 0, Slot = 0, Child = 0;
			if (NodeId == GraphOutput)
			{
				return Refuse(FString::Printf(TEXT("%s: a sound cue always plays one first node; connect another node to @output to change it"), Tool));
			}
			if (!ReadIndex(NodeId, Parent))
			{
				return Refuse(FString::Printf(TEXT("%s: a sound cue node's node_id is its index, a whole number; got '%s'"), Tool, *NodeId));
			}
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("cue_path"), Path } });
			Call->SetNumberField(TEXT("parent_node_id"), Parent);
			if (ReadIndex(PinName, Slot))
			{
				Call->SetNumberField(TEXT("input_index"), Slot);
			}
			else if (ReadIndex(Linked, Child))
			{
				Call->SetNumberField(TEXT("child_node_id"), Child);
			}
			else
			{
				return Refuse(FString::Printf(TEXT("%s: on a sound cue, node_id is the parent node and pin_name the input slot index; or name the child with linked_node_id"), Tool));
			}
			return Forward(Tool, Kind, TEXT("disconnect_sound_cue_nodes"), Call);
		}
		case EKind::MetaSound:
			if (NodeId == GraphOutput)
			{
				return Linked == GraphInput
					? Forward(Tool, Kind, TEXT("disconnect_metasound_graph_input_from_output"), Call({ { TEXT("metasound_path"), Path }, { TEXT("from_node_output_name"), LinkedPin }, { TEXT("to_node_input_name"), PinName } }))
					: Forward(Tool, Kind, TEXT("disconnect_metasound_node_from_graph_output"), Call({ { TEXT("metasound_path"), Path }, { TEXT("graph_output_name"), PinName } }));
			}
			// A MetaSound input takes one link: the input names it.
			return Forward(Tool, Kind, TEXT("disconnect_metasound_nodes"), Call({ { TEXT("metasound_path"), Path }, { TEXT("to_node_id"), NodeId }, { TEXT("to_input_name"), PinName } }));
		case EKind::PCG:
			// A PCG input can take several links: name the other end.
			if (Linked.IsEmpty())
			{
				return Refuse(FString::Printf(TEXT("%s: a PCG input can take several links, so name the one to break: linked_node_id and linked_pin are its other end"), Tool));
			}
			return Forward(Tool, Kind, TEXT("disconnect_pcg_nodes"), Call({ { TEXT("pcg_graph_path"), Path }, { TEXT("from_node"), Linked == GraphInput ? FString(TEXT("Input")) : Linked },
				{ TEXT("from_pin"), LinkedPin }, { TEXT("to_node"), NodeId == GraphOutput ? FString(TEXT("Output")) : NodeId }, { TEXT("to_pin"), PinName } }));
		case EKind::ControlRig:
		{
			TSharedRef<FJsonObject> Call = McpGraphRouterPrivate::Call({ { TEXT("control_rig_path"), Path }, { TEXT("target_pin"), NodeId + TEXT(".") + PinName } });
			if (!Linked.IsEmpty())
			{
				Call->SetStringField(TEXT("source_pin"), Linked + TEXT(".") + LinkedPin);
			}
			return Forward(Tool, Kind, TEXT("remove_rigvm_link"), Call);
		}
		default:
			return nullptr;
		}
	}

	// ---- set_pin_default_value ----------------------------------------------------------------------------------

	static TSharedPtr<FJsonObject> SetPinDefaultValue(const TCHAR* Tool, EKind Kind, const FString& Path, const TSharedPtr<FJsonObject>& Args)
	{
		const FString NodeId = Arg(Args, TEXT("node_id"));
		const FString PinName = Arg(Args, TEXT("pin_name"));
		// The value as the call gives it: `value` (text or a JSON value), else `value_json`.
		TSharedPtr<FJsonValue> Value = Args->TryGetField(TEXT("value"));
		if (!Value.IsValid() || Value->IsNull())
		{
			Value = Args->TryGetField(TEXT("value_json"));
		}
		switch (Kind)
		{
		case EKind::MetaSound:
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				return mcp_common::MissingParameterReply(TEXT("value"));
			}
			// The MetaSound tools read a text value as the input's own type.
			TSharedRef<FJsonObject> Call = NodeId == GraphInput
				? McpGraphRouterPrivate::Call({ { TEXT("metasound_path"), Path }, { TEXT("input_name"), PinName } })
				: McpGraphRouterPrivate::Call({ { TEXT("metasound_path"), Path }, { TEXT("node_id"), NodeId }, { TEXT("input_name"), PinName } });
			Call->SetField(TEXT("value"), Value);
			return Forward(Tool, Kind, NodeId == GraphInput ? TEXT("set_metasound_graph_input_default") : TEXT("set_metasound_node_input_default"), Call);
		}
		case EKind::ControlRig:
		{
			FString Text;
			if (Value.IsValid() && Value->Type == EJson::Object)
			{
				return Refuse(FString::Printf(TEXT("%s: a Control Rig pin default is export text, such as (X=1.0,Y=0.0,Z=0.0): give it as value"), Tool));
			}
			if (!Value.IsValid() || !Value->TryGetString(Text))
			{
				return mcp_common::MissingParameterReply(TEXT("value"));
			}
			return Forward(Tool, Kind, TEXT("set_rigvm_pin_default"), Call({ { TEXT("control_rig_path"), Path }, { TEXT("pin_path"), NodeId + TEXT(".") + PinName }, { TEXT("value"), Text } }));
		}
		case EKind::Material:
		case EKind::MaterialFunction:
			return Refuse(FString::Printf(TEXT("%s: a material expression's unconnected inputs are its properties (ConstA, R, Constant...): set them with set_properties on the expression's object path, which get_graph_nodes gives"), Tool));
		case EKind::SoundCue:
			return Refuse(FString::Printf(TEXT("%s: sound cue nodes have no pin defaults; their settings are properties: set them with set_properties on the node's object path, which get_graph_nodes gives"), Tool));
		case EKind::PCG:
			return Refuse(FString::Printf(TEXT("%s: a PCG node's values are its settings, not pin defaults: set them with set_properties on the node's settings object path, which get_graph_nodes gives"), Tool));
		default:
			return nullptr;
		}
	}
}

TSharedPtr<FJsonObject> McpGraphRouter::Route(const TCHAR* Tool, const TSharedPtr<FJsonObject>& Args)
{
	using namespace McpGraphRouterPrivate;

	if (!Args.IsValid())
	{
		return nullptr;
	}
	FString Path = Arg(Args, TEXT("asset_path"));
	if (Path.IsEmpty())
	{
		Path = Arg(Args, TEXT("blueprint_name"));
	}
	// A Blueprint's short name is the Blueprint code's to resolve; any graph asset is named by its path.
	if (!Path.StartsWith(TEXT("/")))
	{
		return nullptr;
	}
	UObject* Asset = mcp_common::ResolveObjectByPath(Path);
	if (!Asset)
	{
		return nullptr;
	}
	const EKind Kind = KindOf(Asset);
	if (Kind == EKind::Blueprint)
	{
		return nullptr;
	}
	if (Kind == EKind::MaterialInstance)
	{
		return Refuse(FString::Printf(TEXT("%s: %s is a material instance, which has no graph: edit its parent material's graph, or its parameters with the material instance tools"), Tool, *Path));
	}
	if (Kind == EKind::Other)
	{
		return Refuse(FString::Printf(TEXT("%s: %s is a %s, which has no graph these tools edit. They take a Blueprint (an Animation or Widget Blueprint included), a material, a material function, a sound cue, a MetaSound Source, a PCG graph or a Control Rig"),
			Tool, *Path, *Asset->GetClass()->GetName()));
	}
	if (!Arg(Args, TEXT("graph_name")).IsEmpty())
	{
		return Refuse(FString::Printf(TEXT("%s: a %s has one graph, so graph_name does not apply: leave it out"), Tool, KindName(Kind)));
	}

	const FString Name = Tool;
	if (Name == TEXT("get_graph_nodes"))       { return GetGraphNodes(Tool, Kind, Path, Args); }
	if (Name == TEXT("add_graph_node"))        { return AddGraphNode(Tool, Kind, Path, Args); }
	if (Name == TEXT("remove_graph_node"))     { return RemoveGraphNode(Tool, Kind, Path, Args); }
	if (Name == TEXT("connect_pins"))          { return ConnectPins(Tool, Kind, Path, Args); }
	if (Name == TEXT("disconnect_pin"))        { return DisconnectPin(Tool, Kind, Path, Args); }
	if (Name == TEXT("set_pin_default_value")) { return SetPinDefaultValue(Tool, Kind, Path, Args); }
	return nullptr;
}
