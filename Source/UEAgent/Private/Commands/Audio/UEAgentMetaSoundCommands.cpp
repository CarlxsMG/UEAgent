// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h" // UE_VERSION_OLDER_THAN: the UE 5.7 paths

// MetaSound (MetasoundEngine / MetasoundFrontend runtime modules)
#include "MetasoundBuilderSubsystem.h"                          // UMetaSoundBuilderSubsystem / UMetaSoundSourceBuilder
#include "MetasoundBuilderBase.h"                               // UMetaSoundBuilderBase / EMetaSoundBuilderResult / handles
#include "MetasoundSource.h"                                    // UMetaSoundSource
#include "MetasoundDocumentInterface.h"                         // IMetaSoundDocumentInterface
#include "MetasoundFrontendDocument.h"                          // FMetasoundFrontendDocument / class metadata + interface
#include "MetasoundFrontendLiteral.h"                           // FMetasoundFrontendLiteral
#include "MetasoundFrontendDataTypeRegistry.h"                  // IDataTypeRegistry::IsRegistered / IsLiteralTypeSupported
#include "Interfaces/MetasoundOutputFormatInterfaces.h"         // EMetaSoundOutputAudioFormat

// MetaSound editor (MetasoundEditor — UncookedOnly module, editor only)
#include "MetasoundEditorSubsystem.h"                           // UMetaSoundEditorSubsystem::BuildToAsset / FindOrBeginBuilding

// Engine / editor
#include "Editor.h"                                             // GEditor
#include "Engine/Engine.h"                                      // GEngine
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/ScriptInterface.h"
#include "Misc/PackageName.h"
// Reflection-driven node reads (ISearchEngine + reflection on TInstancedStruct<FMetaSoundFrontendNodeConfiguration>).
#include "MetasoundFrontendQuery.h"                             // FMetaSoundClassInfo (must precede SearchEngine)
#include "MetasoundFrontendSearchEngine.h"                      // Metasound::Frontend::ISearchEngine
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#include "MetasoundAssetSubsystem.h"                            // UE 5.7 list_metasound_node_types: FMetaSoundAssetManager::IterateAssetTagData
#include "MetasoundFrontendRegistryKey.h"                       // UE 5.7 list_metasound_node_types: FNodeClassRegistryKey
#endif
#include "JsonObjectConverter.h"                               // UStructToJsonObject / JsonObjectToUStruct
#include "UObject/UObjectIterator.h"                            // TObjectIterator for node-type reflection

/**
 * MetaSound Source authoring and introspection commands.
 *
 * Authors persistent UMetaSoundSource assets headless via the MetaSound builder subsystems:
 *   - UMetaSoundBuilderSubsystem::CreateSourceBuilder() returns a UMetaSoundSourceBuilder plus the
 *     OnPlay/OnFinished/AudioOut handles.
 *   - UMetaSoundEditorSubsystem::BuildToAsset() goes through IAssetTools::CreateAsset, so it creates AND
 *     saves a /Game asset headless-safe, with no modal ConfigureProperties.
 *   - UMetaSoundEditorSubsystem::FindOrBeginBuilding() is the reliable headless path to a builder for an
 *     EXISTING asset, which UMetaSoundBuilderBase then edits (AddGraphInputNode, SetGraphInputDefault,
 *     AddInterface, ...).
 *   - IMetaSoundDocumentInterface::GetConstDocument() exposes the FMetasoundFrontendDocument: the RootGraph
 *     class name and its default interface inputs/outputs.
 *
 * Modules: MetasoundEngine, MetasoundFrontend (runtime) and MetasoundEditor (UncookedOnly, editor-only),
 * all guarded by bBuildEditor. Plugin: Metasound (enabled by default).
 */
namespace UEAgentMetaSoundCommands
{

// MCP-DOMAIN: Audio

namespace
{
    TSharedPtr<FJsonObject> MetaSound_SourceNotFoundReply(const FString& MetaSoundPath)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("MetaSound source not found: %s"), *MetaSoundPath));
    }

    TSharedPtr<FJsonObject> MetaSound_CannotLoadSourceReply(const FString& MetaSoundPath)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot load MetaSound Source: %s"), *MetaSoundPath));
    }

    // A reply payload that starts with the source's asset_path.
    TSharedPtr<FJsonObject> MetaSound_NewPayload(const UMetaSoundSource* Source)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("asset_path"), Source->GetPathName());
        return Data;
    }

    // Marks the source's package dirty and saves it; false, with OutError, when the save fails.
    bool MetaSound_MarkDirtyAndSave(UMetaSoundSource* Source, FString& OutError)
    {
        Source->MarkPackageDirty();
        return mcp_common::SaveAssetPackage(Source->GetOutermost(), Source, OutError);
    }

    // Obtain a mutable builder for an EXISTING saved source via the editor subsystem's
    // FindOrBeginBuilding (the reliable headless path used by every builder-based mutator).
    // Returns nullptr and sets OutError to the reply on failure. The reply names MetaSoundPath; for a missing
    // editor subsystem it does so only when bNameSourceIfNoSubsystem.
    UMetaSoundBuilderBase* GetBuilderForSource(UMetaSoundSource* Source, const FString& MetaSoundPath, TSharedPtr<FJsonObject>& OutError,
        const bool bNameSourceIfNoSubsystem = true)
    {
        UMetaSoundEditorSubsystem* EditorSubsystem =
            GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>() : nullptr;
        if (!EditorSubsystem)
        {
            OutError = mcp_common::MakeErrorReply(bNameSourceIfNoSubsystem
                ? FString::Printf(TEXT("UMetaSoundEditorSubsystem unavailable (editor subsystem not initialized) for '%s'"), *MetaSoundPath)
                : FString(TEXT("UMetaSoundEditorSubsystem unavailable (editor subsystem not initialized)")));
            return nullptr;
        }

        TScriptInterface<IMetaSoundDocumentInterface> DocInterface = Source;
        EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
        UMetaSoundBuilderBase* Builder = EditorSubsystem->FindOrBeginBuilding(DocInterface, FindResult);
        if (!Builder || FindResult != EMetaSoundBuilderResult::Succeeded)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("FindOrBeginBuilding failed to obtain a builder for '%s'"), *MetaSoundPath));
            return nullptr;
        }
        return Builder;
    }

    // The MetaSound Source at MetaSoundPath and a builder for it (GetBuilderForSource); nullptr, with OutError
    // set to the reply, when the source is not found or no builder can be obtained.
    UMetaSoundBuilderBase* MetaSound_OpenSourceBuilder(const FString& MetaSoundPath, UMetaSoundSource*& OutSource, TSharedPtr<FJsonObject>& OutError,
        const bool bNameSourceIfNoSubsystem = true)
    {
        OutSource = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
        if (!OutSource)
        {
            OutError = MetaSound_SourceNotFoundReply(MetaSoundPath);
            return nullptr;
        }
        return GetBuilderForSource(OutSource, MetaSoundPath, OutError, bNameSourceIfNoSubsystem);
    }

    // Map a string ("mono"/"stereo"/"quad"/"5.1"/"7.1", or empty for the mono default) to
    // EMetaSoundOutputAudioFormat; false for any other string.
    bool ParseOutputFormat(const FString& InFormat, EMetaSoundOutputAudioFormat& OutFormat)
    {
        const FString F = InFormat.TrimStartAndEnd().ToLower();
        if (F.IsEmpty() || F == TEXT("mono")) { OutFormat = EMetaSoundOutputAudioFormat::Mono; return true; }
        if (F == TEXT("stereo")) { OutFormat = EMetaSoundOutputAudioFormat::Stereo; return true; }
        if (F == TEXT("quad")) { OutFormat = EMetaSoundOutputAudioFormat::Quad; return true; }
        if (F == TEXT("5.1") || F == TEXT("fivedotone")) { OutFormat = EMetaSoundOutputAudioFormat::FiveDotOne; return true; }
        if (F == TEXT("7.1") || F == TEXT("sevendotone")) { OutFormat = EMetaSoundOutputAudioFormat::SevenDotOne; return true; }
        return false;
    }

    // The output_format spelling of Format, read back from a source; "channel_agnostic" for the experimental format.
    const TCHAR* OutputFormatName(EMetaSoundOutputAudioFormat Format)
    {
        switch (Format)
        {
        case EMetaSoundOutputAudioFormat::Mono:        return TEXT("mono");
        case EMetaSoundOutputAudioFormat::Stereo:      return TEXT("stereo");
        case EMetaSoundOutputAudioFormat::Quad:        return TEXT("quad");
        case EMetaSoundOutputAudioFormat::FiveDotOne:  return TEXT("5.1");
        case EMetaSoundOutputAudioFormat::SevenDotOne: return TEXT("7.1");
        default:                                       return TEXT("channel_agnostic");
        }
    }

    // The literal kinds a typed value field of the default setters stands for.
    enum class EMetaSoundValueKind : uint8 { Bool, Int32, String, Float };

    // The name of the literal type of Kind, as the replies report it in value_type.
    const TCHAR* MetaSound_ValueKindName(EMetaSoundValueKind Kind)
    {
        switch (Kind)
        {
        case EMetaSoundValueKind::Bool:   return TEXT("Bool");
        case EMetaSoundValueKind::Int32:  return TEXT("Int32");
        case EMetaSoundValueKind::String: return TEXT("String");
        default:                          return TEXT("Float");
        }
    }

    // The JSON value of the optional param Field when the request carries it and it is not null, else null.
    TSharedPtr<FJsonValue> MetaSound_PresentField(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Field);
        return (Value.IsValid() && !Value->IsNull()) ? Value : nullptr;
    }

    // The typed value field a request carries: the first of bool_value, int_value, string_value and FloatField that is
    // present and not null, with the literal kind its name stands for; null when there is none.
    TSharedPtr<FJsonValue> MetaSound_PickValueField(const TSharedPtr<FJsonObject>& Params, const TCHAR* FloatField,
        const TCHAR*& OutField, EMetaSoundValueKind& OutKind)
    {
        struct FValueField
        {
            const TCHAR* Name;
            EMetaSoundValueKind Kind;
        };
        const FValueField Fields[] = {
            { TEXT("bool_value"), EMetaSoundValueKind::Bool },
            { TEXT("int_value"), EMetaSoundValueKind::Int32 },
            { TEXT("string_value"), EMetaSoundValueKind::String },
            { FloatField, EMetaSoundValueKind::Float },
        };
        for (const FValueField& Candidate : Fields)
        {
            if (TSharedPtr<FJsonValue> Value = MetaSound_PresentField(Params, Candidate.Name))
            {
                OutField = Candidate.Name;
                OutKind = Candidate.Kind;
                return Value;
            }
        }
        return nullptr;
    }

    // Why Value cannot be the literal of Kind as it is, or empty when it can: a JSON boolean for Bool, a whole number
    // within int32 for Int32, a string for String, a finite number within the float range for Float. FJsonObject's
    // TryGet*Field would convert any other value (a number to a bool, a non-numeric string to 0) instead of refusing it.
    FString MetaSound_ValueKindError(const FJsonValue& Value, EMetaSoundValueKind Kind)
    {
        switch (Kind)
        {
        case EMetaSoundValueKind::Bool:
            return Value.Type == EJson::Boolean ? FString() : FString(TEXT("must be a JSON boolean (true or false)"));
        case EMetaSoundValueKind::String:
            return Value.Type == EJson::String ? FString() : FString(TEXT("must be a JSON string"));
        default:
            break;
        }
        if (Value.Type != EJson::Number)
        {
            return TEXT("must be a JSON number");
        }
        const double Number = Value.AsNumber();
        if (Kind == EMetaSoundValueKind::Int32)
        {
            const bool bWholeInt32 = FMath::IsFinite(Number) && Number == FMath::FloorToDouble(Number)
                && Number >= static_cast<double>(MIN_int32) && Number <= static_cast<double>(MAX_int32);
            return bWholeInt32 ? FString() : FString(TEXT("must be a whole number within the int32 range"));
        }
        const bool bFloat = FMath::IsFinite(Number) && FMath::Abs(Number) <= static_cast<double>(TNumericLimits<float>::Max());
        return bFloat ? FString() : FString(TEXT("must be a finite number within the float range"));
    }

    // Build an FMetasoundFrontendLiteral from the request's value field: the first of bool_value, int_value and
    // string_value present and not null, else value (a float). Shared by the graph-input and node-input default
    // setters. Null on success; otherwise the error reply, with nothing to write: no value field at all, or one whose
    // JSON type is not the one its name says.
    TSharedPtr<FJsonObject> BuildLiteralFromParams(const TSharedPtr<FJsonObject>& Params, FMetasoundFrontendLiteral& Literal, FString& ValueType)
    {
        const TCHAR* Field = nullptr;
        EMetaSoundValueKind Kind = EMetaSoundValueKind::Float;
        const TSharedPtr<FJsonValue> Value = MetaSound_PickValueField(Params, TEXT("value"), Field, Kind);
        if (!Value.IsValid())
        {
            return mcp_common::MakeErrorReply(
                TEXT("Missing value: give one of value (a float), int_value, bool_value or string_value. Nothing was written."));
        }
        const FString KindError = MetaSound_ValueKindError(*Value, Kind);
        if (!KindError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s %s. Nothing was written."), Field, *KindError));
        }
        // The field is known to hold its JSON type, so each read below takes it as it is.
        switch (Kind)
        {
        case EMetaSoundValueKind::Bool:
        {
            bool B = false; Params->TryGetBoolField(TEXT("bool_value"), B);
            Literal.Set(B);
            break;
        }
        case EMetaSoundValueKind::Int32:
        {
            int32 I = 0; Params->TryGetNumberField(TEXT("int_value"), I);
            Literal.Set(I);
            break;
        }
        case EMetaSoundValueKind::String:
        {
            FString S; Params->TryGetStringField(TEXT("string_value"), S);
            Literal.Set(S);
            break;
        }
        default:
        {
            double V = 0.0; Params->TryGetNumberField(TEXT("value"), V);
            Literal.Set(static_cast<float>(V));
            break;
        }
        }
        ValueType = MetaSound_ValueKindName(Kind);
        return nullptr;
    }

    // Build an FMetasoundFrontendLiteral from `value` read as the JSON type ParameterType names: "float" (a number),
    // "int" (a whole number), "bool" (true or false) or "string" (a string). Null on success; otherwise the error
    // reply, with nothing to write: an unknown type, no value, or a value whose JSON type is not the one named.
    TSharedPtr<FJsonObject> BuildLiteralFromTypedValue(const TSharedPtr<FJsonObject>& Params, const FString& ParameterType,
        FMetasoundFrontendLiteral& Literal, FString& ValueType)
    {
        const FString TypeLower = ParameterType.TrimStartAndEnd().ToLower();
        EMetaSoundValueKind Kind = EMetaSoundValueKind::Float;
        if (TypeLower == TEXT("bool"))
        {
            Kind = EMetaSoundValueKind::Bool;
        }
        else if (TypeLower == TEXT("int"))
        {
            Kind = EMetaSoundValueKind::Int32;
        }
        else if (TypeLower == TEXT("string"))
        {
            Kind = EMetaSoundValueKind::String;
        }
        else if (TypeLower != TEXT("float"))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Unknown parameter_type '%s': use \"float\", \"bool\", \"int\" or \"string\". Nothing was written."),
                *ParameterType));
        }
        // A raw read: value is of whatever JSON type parameter_type names.
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
        if (!Value.IsValid() || Value->IsNull())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Missing value: with parameter_type \"%s\", value carries the default. Nothing was written."), *TypeLower));
        }
        const FString KindError = MetaSound_ValueKindError(*Value, Kind);
        if (!KindError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("value %s for parameter_type \"%s\". Nothing was written."), *KindError, *TypeLower));
        }
        // The value is known to hold its JSON type, so each read below takes it as it is.
        switch (Kind)
        {
        case EMetaSoundValueKind::Bool:   Literal.Set(Value->AsBool()); break;
        case EMetaSoundValueKind::Int32:  Literal.Set(static_cast<int32>(Value->AsNumber())); break;
        case EMetaSoundValueKind::String: Literal.Set(Value->AsString()); break;
        default:                          Literal.Set(static_cast<float>(Value->AsNumber())); break;
        }
        ValueType = MetaSound_ValueKindName(Kind);
        return nullptr;
    }

    // Parse a node_id (FGuid string from add_metasound_node) into a node handle.
    bool ParseMetaSoundNodeHandle(const FString& NodeId, FMetaSoundNodeHandle& Out)
    {
        FGuid Guid;
        if (!FGuid::Parse(NodeId, Guid) || !Guid.IsValid()) { return false; }
        Out = FMetaSoundNodeHandle(Guid);
        return true;
    }

    // Enumerate a node's input pins as JSON [{ name, data_type }] (discovered at runtime,
    // never hardcoded).
    TArray<TSharedPtr<FJsonValue>> EnumerateNodeInputs(UMetaSoundBuilderBase* Builder, const FMetaSoundNodeHandle& Node)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
        for (const FMetaSoundBuilderNodeInputHandle& H : Builder->FindNodeInputs(Node, R))
        {
            FName Name, DataType; EMetaSoundBuilderResult DR = EMetaSoundBuilderResult::Failed;
            Builder->GetNodeInputData(H, Name, DataType, DR);
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), Name.ToString());
            O->SetStringField(TEXT("data_type"), DataType.ToString());
            Out.Add(MakeShared<FJsonValueObject>(O));
        }
        return Out;
    }

    TArray<TSharedPtr<FJsonValue>> EnumerateNodeOutputs(UMetaSoundBuilderBase* Builder, const FMetaSoundNodeHandle& Node)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        EMetaSoundBuilderResult R = EMetaSoundBuilderResult::Failed;
        for (const FMetaSoundBuilderNodeOutputHandle& H : Builder->FindNodeOutputs(Node, R))
        {
            FName Name, DataType; EMetaSoundBuilderResult DR = EMetaSoundBuilderResult::Failed;
            Builder->GetNodeOutputData(H, Name, DataType, DR);
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), Name.ToString());
            O->SetStringField(TEXT("data_type"), DataType.ToString());
            Out.Add(MakeShared<FJsonValueObject>(O));
        }
        return Out;
    }

    // The edge connect_metasound_graph_input_to_output and disconnect_metasound_graph_input_from_output act on: a graph input node's
    // output handle and a graph output node's input handle, both resolved by name.
    struct FMetaSoundGraphIOEdge
    {
        FString MetaSoundPath;
        FString FromOutputName;
        FString ToInputName;
        UMetaSoundSource* Source = nullptr;
        UMetaSoundBuilderBase* Builder = nullptr;
        FName FromDataType;
        FMetaSoundBuilderNodeOutputHandle FromOutputHandle;
        FName ToDataType;
        FMetaSoundBuilderNodeInputHandle ToInputHandle;

        // The payload both handlers report: the asset, the two names and their data types.
        TSharedPtr<FJsonObject> MakePayload() const
        {
            TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
            Data->SetStringField(TEXT("from_node_output_name"), FromOutputName);
            Data->SetStringField(TEXT("to_node_input_name"), ToInputName);
            Data->SetStringField(TEXT("from_data_type"), FromDataType.ToString());
            Data->SetStringField(TEXT("to_data_type"), ToDataType.ToString());
            return Data;
        }
    };

    // Reads metasound_path, from_node_output_name and to_node_input_name, then resolves the source, its
    // builder and both handles into Edge. Null on success, otherwise the error reply.
    TSharedPtr<FJsonObject> MetaSound_ResolveGraphIOEdge(const TSharedPtr<FJsonObject>& Params, FMetaSoundGraphIOEdge& Edge)
    {
        if (!Params->TryGetStringField(TEXT("metasound_path"), Edge.MetaSoundPath) || Edge.MetaSoundPath.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'metasound_path' parameter"));
        }
        if (!Params->TryGetStringField(TEXT("from_node_output_name"), Edge.FromOutputName) || Edge.FromOutputName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'from_node_output_name' parameter"));
        }
        if (!Params->TryGetStringField(TEXT("to_node_input_name"), Edge.ToInputName) || Edge.ToInputName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'to_node_input_name' parameter"));
        }

        TSharedPtr<FJsonObject> Error;
        Edge.Builder = MetaSound_OpenSourceBuilder(Edge.MetaSoundPath, Edge.Source, Error);
        if (!Edge.Builder)
        {
            return Error;
        }

        // The source (graph input) node's OUTPUT handle, by name.
        EMetaSoundBuilderResult FromResult = EMetaSoundBuilderResult::Failed;
        Edge.Builder->FindGraphInputNode(FName(*Edge.FromOutputName), Edge.FromDataType, Edge.FromOutputHandle, FromResult);
        if (FromResult != EMetaSoundBuilderResult::Succeeded || !Edge.FromOutputHandle.IsSet())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not resolve graph input '%s' as a connectable output"), *Edge.FromOutputName));
        }

        // The destination (graph output) node's INPUT handle, by name.
        EMetaSoundBuilderResult ToResult = EMetaSoundBuilderResult::Failed;
        Edge.Builder->FindGraphOutputNode(FName(*Edge.ToInputName), Edge.ToDataType, Edge.ToInputHandle, ToResult);
        if (ToResult != EMetaSoundBuilderResult::Succeeded || !Edge.ToInputHandle.IsSet())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not resolve graph output '%s' as a connectable input"), *Edge.ToInputName));
        }
        return nullptr;
    }

    // The node NodeId names in the default graph of the MetaSound Source at MetaSoundPath; nullptr, with
    // OutError set to the reply, when NodeId is not a GUID, the source does not load or no node has that id.
    const FMetasoundFrontendNode* MetaSound_FindGraphNode(const FString& MetaSoundPath, const FString& NodeId,
        UMetaSoundSource*& OutSource, TSharedPtr<FJsonObject>& OutError)
    {
        FGuid TargetId;
        if (!FGuid::Parse(NodeId, TargetId) || !TargetId.IsValid())
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("node_id is not a valid GUID: %s"), *NodeId));
            return nullptr;
        }
        OutSource = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
        if (!OutSource)
        {
            OutError = MetaSound_CannotLoadSourceReply(MetaSoundPath);
            return nullptr;
        }
        const TArray<FMetasoundFrontendNode>& Nodes = OutSource->GetConstDocument().RootGraph.GetConstDefaultGraph().Nodes;
        const FMetasoundFrontendNode* FoundNode = Nodes.FindByPredicate(
            [&TargetId](const FMetasoundFrontendNode& N) { return N.GetID() == TargetId; });
        if (!FoundNode)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Node not found in graph: %s"), *NodeId));
        }
        return FoundNode;
    }

    // Node's Configuration struct and its property PropName: null, or the error reply when the node has no
    // Configuration struct or the struct has no such property.
    TSharedPtr<FJsonObject> MetaSound_FindConfigProperty(const FMetasoundFrontendNode& Node, const FString& PropName,
        const UScriptStruct*& OutStruct, const FProperty*& OutProperty)
    {
        OutStruct = Node.Configuration.GetScriptStruct();
        if (!OutStruct)
        {
            return mcp_common::MakeErrorReply(TEXT("Node has no Configuration script struct"));
        }
        OutProperty = OutStruct->FindPropertyByName(FName(*PropName));
        if (!OutProperty)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Property '%s' not found on Configuration struct %s"),
                    *PropName, *OutStruct->GetName()));
        }
        return nullptr;
    }

    // The interface vertices as [{ name, type }].
    template <typename VertexType>
    TArray<TSharedPtr<FJsonValue>> MetaSound_NameTypeArray(const TArray<VertexType>& Vertices)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const VertexType& Vertex : Vertices)
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), Vertex.Name.ToString());
            P->SetStringField(TEXT("type"), Vertex.TypeName.ToString());
            Out.Add(MakeShared<FJsonValueObject>(P));
        }
        return Out;
    }

    // Obj's "value": the literal as a bool, int, float or string, else null.
    void MetaSound_SetLiteralValueField(const TSharedPtr<FJsonObject>& Obj, const FMetasoundFrontendLiteral& Literal)
    {
        bool BoolValue = false;
        int32 IntValue = 0;
        float FloatValue = 0.f;
        FString StringValue;
        if      (Literal.TryGet(BoolValue))   { Obj->SetBoolField  (TEXT("value"), BoolValue); }
        else if (Literal.TryGet(IntValue))    { Obj->SetNumberField(TEXT("value"), IntValue); }
        else if (Literal.TryGet(FloatValue))  { Obj->SetNumberField(TEXT("value"), FloatValue); }
        else if (Literal.TryGet(StringValue)) { Obj->SetStringField(TEXT("value"), StringValue); }
        else                                  { Obj->SetField      (TEXT("value"), MakeShared<FJsonValueNull>()); }
    }
}

// ---------------------------------------------------------------------------
// create_metasound_source
// CreateSourceBuilder(...) -> a transient UMetaSoundSourceBuilder pre-populated
// with the source interface (OnPlay/OnFinished/AudioOut nodes), then
// UMetaSoundEditorSubsystem::BuildToAsset(...) -> persists it as a saved
// UMetaSoundSource asset under /Game (via IAssetTools::CreateAsset; headless-safe,
// no modal). Idempotent: if the asset already exists, loads + returns its info.
// ---------------------------------------------------------------------------
// MCP:create_metasound_source - Create a UMetaSoundSource asset at package_path/name (default folder /Game/MetaSounds): a one-shot source graph pre-wired with the source interface (OnPlay input, OnFinished output, one audio output per channel), built by the MetaSound builder subsystem and written with BuildToAsset, then saved; no dialog, no undo step. Idempotent: a MetaSound Source already there is returned unchanged with already_exists=true, created=false and saved=false, nothing saved. output_format is mono, stereo, quad, 5.1 or 7.1; anything else is an error and nothing is created. The reply's output_format is read from the source, also for an existing one, whose own format is reported and left as it is. An invalid package name or a failed build is an error; a failed save gives success false with save_error. Next: add_metasound_node, add_metasound_interface; inspect: get_metasound_info; inverse: delete_asset.
//   @param name - Name of the MetaSound Source asset to create.
//   @param package_path - Target /Game package path (optional, default "/Game/MetaSounds")
//   @param output_format - Output audio format: "mono", "stereo", "quad", "5.1", or "7.1" (optional, default "mono")
//   @returns { source_name: string, asset_path: string, created: bool, already_exists: bool, saved: bool, output_format: string, class_name: string, input_count: number, output_count: number, save_error: string?, message: string? }

TSharedPtr<FJsonObject> HandleCreateMetaSoundSource(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/MetaSounds");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString OutputFormatStr = TEXT("mono");
    Params->TryGetStringField(TEXT("output_format"), OutputFormatStr);
    EMetaSoundOutputAudioFormat OutputFormat = EMetaSoundOutputAudioFormat::Mono;
    if (!ParseOutputFormat(OutputFormatStr, OutputFormat))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown output_format '%s': use \"mono\" (default), \"stereo\", \"quad\", \"5.1\" or \"7.1\". Nothing was created."),
            *OutputFormatStr));
    }

    const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);

    // Idempotent: if it already exists, load and return its current shape.
    if (UMetaSoundSource* Existing = LoadObject<UMetaSoundSource>(nullptr, *ObjectPath))
    {
        const FMetasoundFrontendDocument& Doc = Existing->GetConstDocument();
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("source_name"), Existing->GetName());
        Data->SetStringField(TEXT("asset_path"), ObjectPath);
        Data->SetBoolField(TEXT("created"), false);
        Data->SetBoolField(TEXT("already_exists"), true);
        // Returned unchanged: nothing is applied to it, so nothing is saved. The same keys as the create path keep
        // the idempotent re-run shape.
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("message"), TEXT("MetaSound Source already exists; returned the existing asset unchanged, nothing saved"));
        // The source's own format, not the requested one: nothing is applied to an existing source.
        Data->SetStringField(TEXT("output_format"), OutputFormatName(Existing->OutputFormat));
        Data->SetStringField(TEXT("class_name"), Doc.RootGraph.Metadata.GetClassName().ToString());
        Data->SetNumberField(TEXT("input_count"), Doc.RootGraph.GetDefaultInterface().Inputs.Num());
        Data->SetNumberField(TEXT("output_count"), Doc.RootGraph.GetDefaultInterface().Outputs.Num());
        return mcp_common::MakeOkReply(Data);
    }

    UMetaSoundBuilderSubsystem* BuilderSubsystem =
        GEngine ? GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>() : nullptr;
    if (!BuilderSubsystem)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UMetaSoundBuilderSubsystem unavailable (engine subsystem not initialized)"));
    }

    // CreateSourceBuilder out-params (verified 5.7 signature).
    FMetaSoundBuilderNodeOutputHandle OnPlayNodeOutput;
    FMetaSoundBuilderNodeInputHandle OnFinishedNodeInput;
    TArray<FMetaSoundBuilderNodeInputHandle> AudioOutNodeInputs;
    EMetaSoundBuilderResult CreateResult = EMetaSoundBuilderResult::Failed;

    const FName BuilderName(*FString::Printf(TEXT("MCP_%s_Builder"), *Name));
    UMetaSoundSourceBuilder* Builder = BuilderSubsystem->CreateSourceBuilder(
        BuilderName,
        OnPlayNodeOutput,
        OnFinishedNodeInput,
        AudioOutNodeInputs,
        CreateResult,
        OutputFormat,
        /*bIsOneShot*/ true);

    if (!Builder || CreateResult != EMetaSoundBuilderResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(
            TEXT("CreateSourceBuilder failed to produce a valid MetaSound source builder"));
    }

    // BuildToAsset is editor-only (UncookedOnly module). It internally uses
    // IAssetTools::CreateAsset which both creates and saves the /Game asset.
    UMetaSoundEditorSubsystem* EditorSubsystem =
        GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>() : nullptr;
    if (!EditorSubsystem)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UMetaSoundEditorSubsystem unavailable (editor subsystem not initialized)"));
    }

    EMetaSoundBuilderResult BuildResult = EMetaSoundBuilderResult::Failed;
    TScriptInterface<IMetaSoundDocumentInterface> DocInterface = EditorSubsystem->BuildToAsset(
        Builder,
        /*Author*/ TEXT("UEAgent"),
        /*AssetName*/ Name,
        /*PackagePath*/ PackagePath,
        BuildResult,
        /*TemplateSoundWave*/ nullptr);

    if (BuildResult != EMetaSoundBuilderResult::Succeeded || !DocInterface.GetObject())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("BuildToAsset failed for '%s' at '%s'"), *Name, *PackagePath));
    }

    UMetaSoundSource* NewSource = Cast<UMetaSoundSource>(DocInterface.GetObject());
    if (!NewSource)
    {
        return mcp_common::MakeErrorReply(
            TEXT("BuildToAsset returned a non-UMetaSoundSource object"));
    }

    // Ensure the package is persisted (BuildToAsset already saves via CreateAsset,
    // but re-save defensively so the on-disk file reflects the final built document).
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewSource->GetOutermost(), NewSource, SaveError);

    const FMetasoundFrontendDocument& Doc = NewSource->GetConstDocument();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("source_name"), NewSource->GetName());
    Data->SetStringField(TEXT("asset_path"), NewSource->GetPathName());
    Data->SetBoolField(TEXT("created"), true);
    Data->SetStringField(TEXT("output_format"), OutputFormatName(NewSource->OutputFormat));
    Data->SetStringField(TEXT("class_name"), Doc.RootGraph.Metadata.GetClassName().ToString());
    Data->SetNumberField(TEXT("input_count"), Doc.RootGraph.GetDefaultInterface().Inputs.Num());
    Data->SetNumberField(TEXT("output_count"), Doc.RootGraph.GetDefaultInterface().Outputs.Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_metasound_source"), SaveError);
}

// ---------------------------------------------------------------------------
// add_metasound_interface
// Obtains a builder for an EXISTING saved asset via the editor subsystem's
// FindOrBeginBuilding (the reliable headless path), then adds a registered
// interface (validated first via IsInterfaceRegistered) to the graph, which
// adds the interface's input/output nodes. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:add_metasound_interface - Adds a registered interface to an existing MetaSound Source's graph.  Obtains a builder for the saved asset via the editor subsystem's FindOrBeginBuilding (the reliable headless path), validates the interface via IsInterfaceRegistered, then calls UMetaSoundBuilderBase::AddInterface (which adds the interface's input/output nodes). The asset is saved after the edit. Inverse: remove_metasound_interface.
//   @param metasound_path - Asset path of the MetaSound Source (e.g. "/Game/MetaSounds/MS_Example" or fully-qualified ".MS_Example").
//   @param interface_name - Name of a registered MetaSound interface to add (required)
//   @returns { asset_path: string, node_kind: string, interface_name: string, added: bool, saved: bool }

TSharedPtr<FJsonObject> HandleMetaSoundAddInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    FString InterfaceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("interface_name"), InterfaceName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error, /*bNameSourceIfNoSubsystem=*/false);
    if (!Builder)
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundAddInterface", "MetaSound Add Interface"));
    Source->Modify();

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);

    UMetaSoundBuilderSubsystem* BuilderSubsystem =
        GEngine ? GEngine->GetEngineSubsystem<UMetaSoundBuilderSubsystem>() : nullptr;
    const bool bRegistered = BuilderSubsystem ? BuilderSubsystem->IsInterfaceRegistered(FName(*InterfaceName)) : false;
    if (!bRegistered)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Interface not registered: %s"), *InterfaceName));
    }

    EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
    Builder->AddInterface(FName(*InterfaceName), AddResult);
    if (AddResult != EMetaSoundBuilderResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddInterface failed for '%s'"), *InterfaceName));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    Data->SetStringField(TEXT("node_kind"), TEXT("interface"));
    Data->SetStringField(TEXT("interface_name"), InterfaceName);
    Data->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_metasound_interface"));
}

// ---------------------------------------------------------------------------
// set_metasound_graph_input_default
// Obtains a builder for an existing asset (FindOrBeginBuilding) and sets the
// named graph input's default literal via SetGraphInputDefault, then reads it
// back to verify and saves. Supports float / int / bool / string values.
// ---------------------------------------------------------------------------
// MCP:set_metasound_graph_input_default - Set the default value of one graph input of a MetaSound Source asset (a preset included) through the MetaSound editor builder, in one undo step, then save the asset. Give one typed field: bool_value (true or false), int_value (a whole number), string_value (a string) or value (a number, read as a float); with several, the first in that order wins. Or give parameter_type (float, int, bool or string) and value in that JSON type, the typed fields then being ignored. A missing or mistyped value, an unknown parameter_type, an unknown source, or an input that is missing or does not take that type is an error, with nothing written. value_set means the default reads back as the value; an input already holding it gives changed=false and saved=false, nothing written. A failed save makes success false with save_error. Inputs and their defaults: get_metasound_info (include_defaults); a node pin: set_metasound_node_input_default.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param preset_path - Alias of metasound_path.
//   @param input_name - Name of the graph input whose default to set.
//   @param parameter_name - Alias of input_name.
//   @param parameter_type - Optional literal type of value: "float", "int", "bool" or "string". When given, value must be of that JSON type (a whole number for int) and int_value/bool_value/string_value are ignored; another type name is an error.
//   @param value - Float default value, a JSON number (optional, used if no other *_value is provided); with parameter_type, the value in the JSON type it names. Another JSON type is an error
//   @param int_value - Int32 default value, a whole JSON number (optional, mutually exclusive with value/bool_value/string_value)
//   @param bool_value - Bool default value, JSON true or false (optional, mutually exclusive with value/int_value/string_value)
//   @param string_value - String default value, a JSON string (optional, mutually exclusive with value/int_value/bool_value)
//   @returns { asset_path: string, input_name: string, value_type: string, value_set: bool, changed: bool, verified: bool, value_read_back: string, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleMetaSoundSetInputDefault(const TSharedPtr<FJsonObject>& Params)
{
    // metasound_path, else its alias preset_path.
    FString MetaSoundPath;
    if (!Params->TryGetStringField(TEXT("metasound_path"), MetaSoundPath) && !Params->TryGetStringField(TEXT("preset_path"), MetaSoundPath))
    {
        return mcp_common::MissingParameterReply(TEXT("metasound_path"));
    }
    if (MetaSoundPath.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("metasound_path"));
    }

    // input_name, else its alias parameter_name.
    FString InputName;
    if (!Params->TryGetStringField(TEXT("input_name"), InputName) && !Params->TryGetStringField(TEXT("parameter_name"), InputName))
    {
        return mcp_common::MissingParameterReply(TEXT("input_name"));
    }
    if (InputName.IsEmpty())
    {
        return mcp_common::MissingParameterReply(TEXT("input_name"));
    }

    // Optional: with parameter_type, value carries the literal in the JSON type it names.
    FString ParameterType;
    const bool bTypedValue = Params->TryGetStringField(TEXT("parameter_type"), ParameterType)
        && !ParameterType.TrimStartAndEnd().IsEmpty();

    // The value is read and checked before anything is opened or written: a missing value, or one of the wrong JSON
    // type, would otherwise be written as 0, false or an empty string.
    FMetasoundFrontendLiteral Literal;
    FString ValueType;
    if (bTypedValue)
    {
        if (TSharedPtr<FJsonObject> ValueError = BuildLiteralFromTypedValue(Params, ParameterType, Literal, ValueType)) { return ValueError; }
    }
    else if (TSharedPtr<FJsonObject> ValueError = BuildLiteralFromParams(Params, Literal, ValueType))
    {
        return ValueError;
    }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error, /*bNameSourceIfNoSubsystem=*/false);
    if (!Builder)
    {
        return Error;
    }

    // The checks FMetaSoundFrontendDocumentBuilder::SetGraphInputDefault makes, made before anything changes: the
    // input exists in the root graph's interface and its data type takes this literal type.
    const FName InputFName(*InputName);
    const TArray<FMetasoundFrontendClassInput>& Inputs = Source->GetConstDocument().RootGraph.GetDefaultInterface().Inputs;
    const FMetasoundFrontendClassInput* Input =
        Inputs.FindByPredicate([&InputFName](const FMetasoundFrontendClassInput& In) { return In.Name == InputFName; });
    if (!Input)
    {
        TArray<FString> InputNames;
        for (const FMetasoundFrontendClassInput& In : Inputs)
        {
            InputNames.Add(In.Name.ToString());
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No graph input named '%s' on '%s' (inputs: %s). Nothing was written."), *InputName, *MetaSoundPath,
            InputNames.Num() > 0 ? *FString::Join(InputNames, TEXT(", ")) : TEXT("none")));
    }
    if (!Metasound::Frontend::IDataTypeRegistry::Get().IsLiteralTypeSupported(Input->TypeName, Literal.GetType()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Graph input '%s' is of type %s, which takes no %s value. Nothing was written."),
            *InputName, *Input->TypeName.ToString(), *ValueType));
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("input_name"), InputName);
    Data->SetStringField(TEXT("value_type"), ValueType);

    // An input already holding this value is a no-op: nothing is written, dirtied or saved.
    EMetaSoundBuilderResult BeforeResult = EMetaSoundBuilderResult::Failed;
    const FMetasoundFrontendLiteral Before = Builder->GetGraphInputDefault(InputFName, BeforeResult);
    if (BeforeResult == EMetaSoundBuilderResult::Succeeded && Before.IsEqual(Literal))
    {
        Data->SetBoolField(TEXT("value_set"), true);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("verified"), true);
        Data->SetStringField(TEXT("value_read_back"), Before.ToString());
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The input already holds this value; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundSetInputDefault", "MetaSound Set Input Default"));
    Source->Modify();

    EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
    Builder->SetGraphInputDefault(InputFName, Literal, SetResult);
    if (SetResult != EMetaSoundBuilderResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetGraphInputDefault failed for input '%s' (input may not exist)"), *InputName));
    }

    // The write succeeded when the default read back is the literal sent.
    EMetaSoundBuilderResult GetResult = EMetaSoundBuilderResult::Failed;
    const FMetasoundFrontendLiteral ReadBack = Builder->GetGraphInputDefault(InputFName, GetResult);
    const bool bVerified = GetResult == EMetaSoundBuilderResult::Succeeded;
    const bool bWritten = bVerified && ReadBack.IsEqual(Literal);
    const bool bChanged = !(bVerified && BeforeResult == EMetaSoundBuilderResult::Succeeded && ReadBack.IsEqual(Before));

    // Only a value that reads back is saved.
    FString SaveErr;
    bool bSaved = false;
    if (bWritten)
    {
        bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);
    }
    else
    {
        SaveErr = TEXT("not saved: the default read back is not the value sent");
    }

    Data->SetBoolField(TEXT("value_set"), bWritten);
    Data->SetBoolField(TEXT("changed"), bChanged);
    Data->SetBoolField(TEXT("verified"), bVerified);
    Data->SetStringField(TEXT("value_read_back"), ReadBack.ToString());
    mcp_common::AddSaveOutcome(Data, bSaved, SaveErr);
    return mcp_common::MakeOutcomeReply(Data, bWritten && bSaved, !bWritten
        ? mcp_common::MeasuredOutcomeError(TEXT("set_metasound_graph_input_default"), TEXT("value_set"))
        : mcp_common::SavedOutcomeError(TEXT("set_metasound_graph_input_default")));
}

// ---------------------------------------------------------------------------
// get_metasound_info
// Loads the UMetaSoundSource and reports its UClass, root graph class name, the
// document's input/output counts (read from the root graph's default interface) and
// the node/edge/variable counts of the root graph's default page; optionally the
// default of each input, read through the editor builder.
// ---------------------------------------------------------------------------
// MCP:get_metasound_info - Read the top-level metadata of a MetaSound Source asset (a preset included), loading it if needed: its class, the root graph class name, the graph inputs and outputs (name and type) with their counts, interface_count (the number of interfaces the document declares) and the node, connection (edge) and variable counts of the root graph's default page; with include_defaults, each input's default value too. Read-only; a path that does not resolve to a UMetaSoundSource, a MetaSound Patch included, is an error. list_metasound_nodes lists the nodes; get_assets_by_type with asset_type MetaSoundSource finds sources. The edges themselves: list_metasound_connections.
//   @param metasound_path - Asset path of the MetaSound Source, as a package path like "/Game/MetaSounds/MS_Example" or an object path like "/Game/MetaSounds/MS_Example.MS_Example"
//   @param include_defaults - Optional bool (default false). When true, each inputs[] entry gains value: its default as the MetaSound editor builder reads it (a literal string), or "(inherited)" when the builder has none (a preset input taken from its referenced asset). Opens the editor builder for the asset; the asset is not changed.
//   @returns { source_name: string, asset_path: string, class_name: string, graph_class_name: string, input_count: number, output_count: number, interface_count: number, node_count: number, connection_count: number, variable_count: number, inputs: [{ name: string, type: string, value?: string }], outputs: [{ name: string, type: string }] }

TSharedPtr<FJsonObject> HandleGetMetaSoundInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    bool bIncludeDefaults = false;
    Params->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);

    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
    {
        return MetaSound_SourceNotFoundReply(MetaSoundPath);
    }

    const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();
    const FMetasoundFrontendClassInterface& Interface = Doc.RootGraph.GetDefaultInterface();
    const FMetasoundFrontendGraph& Graph = Doc.RootGraph.GetConstDefaultGraph();

    TArray<TSharedPtr<FJsonValue>> InputsJson = MetaSound_NameTypeArray(Interface.Inputs);
    if (bIncludeDefaults)
    {
        // The defaults come from the editor builder, as for a preset: an input whose own graph declares no default
        // (a preset inheriting it from the referenced asset) reads as "(inherited)".
        TSharedPtr<FJsonObject> BuilderError;
        UMetaSoundBuilderBase* Builder = GetBuilderForSource(Source, MetaSoundPath, BuilderError);
        if (!Builder)
        {
            return BuilderError;
        }
        for (int32 Index = 0; Index < Interface.Inputs.Num() && Index < InputsJson.Num(); ++Index)
        {
            EMetaSoundBuilderResult GetResult = EMetaSoundBuilderResult::Failed;
            const FMetasoundFrontendLiteral DefaultValue = Builder->GetGraphInputDefault(Interface.Inputs[Index].Name, GetResult);
            InputsJson[Index]->AsObject()->SetStringField(TEXT("value"),
                GetResult == EMetaSoundBuilderResult::Succeeded ? DefaultValue.ToString() : FString(TEXT("(inherited)")));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("source_name"), Source->GetName());
    Data->SetStringField(TEXT("asset_path"), Source->GetPathName());
    Data->SetStringField(TEXT("class_name"), Source->GetClass()->GetName());
    Data->SetStringField(TEXT("graph_class_name"), Doc.RootGraph.Metadata.GetClassName().ToString());
    Data->SetNumberField(TEXT("input_count"), Interface.Inputs.Num());
    Data->SetNumberField(TEXT("output_count"), Interface.Outputs.Num());
    Data->SetNumberField(TEXT("interface_count"), Doc.Interfaces.Num());
    Data->SetNumberField(TEXT("node_count"), Graph.Nodes.Num());
    Data->SetNumberField(TEXT("connection_count"), Graph.Edges.Num());
    Data->SetNumberField(TEXT("variable_count"), Graph.Variables.Num());
    Data->SetArrayField(TEXT("inputs"), InputsJson);
    Data->SetArrayField(TEXT("outputs"), MetaSound_NameTypeArray(Interface.Outputs));
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Graph connections + IO nodes

// ---------------------------------------------------------------------------
// add_metasound_graph_input
// FindOrBeginBuilding for an existing asset + AddGraphInputNode(name, dataType,
// default literal). Adds a real graph input node (and its backing graph input
// member). Saves the asset. Supports an optional default value (float/int/bool/
// string) baked into the literal.
// ---------------------------------------------------------------------------
// MCP:add_metasound_graph_input - Add a graph input of data_type (default Float) to an existing MetaSound Source through the MetaSound builder, inside an undo transaction, then save the asset. Optionally bakes a typed default into the new input; give at most one of default_value, int_value, bool_value or string_value (bool_value wins, then int_value, string_value, default_value). An unregistered data_type or a source that does not load is an error, and so is a default of the wrong JSON type or one data_type takes no literal of, with nothing written. A name already used by an input is left untouched: with the same data_type the call succeeds with added=false and already_exists=true, saves nothing and does not apply a supplied default (note says so; use set_metasound_graph_input_default); with another data_type it is an error. success is false, with save_error, when the save fails (during PIE, for one). Inverse: remove_metasound_graph_input.
//   @param metasound_path - Asset path of the MetaSound Source, as a package path like "/Game/MetaSounds/MS_Example" or an object path like "/Game/MetaSounds/MS_Example.MS_Example"
//   @param input_name - Name of the graph input node to add.
//   @param data_type - Frontend data type: "Float", "Int32", "Bool", "String", "Audio", "Trigger" (optional, default "Float")
//   @param default_value - Float default baked into the literal, a JSON number (optional)
//   @param int_value - Int32 default baked into the literal, a whole JSON number (optional, mutually exclusive with other defaults)
//   @param bool_value - Bool default baked into the literal, JSON true or false (optional, mutually exclusive with other defaults)
//   @param string_value - String default baked into the literal, a JSON string (optional, mutually exclusive with other defaults)
//   @returns { asset_path: string, input_name: string, data_type: string, added: bool, already_exists?: bool, has_default: bool, note?: string, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleAddMetaSoundGraphInput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    FString InputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName)) { return Missing; }

    FString DataType = TEXT("Float");
    Params->TryGetStringField(TEXT("data_type"), DataType);

    // Optional typed default value baked into the literal: the first of bool_value, int_value, string_value and
    // default_value present and not null. No default is valid; one of the wrong JSON type, or one the data type takes no
    // literal of, is refused here, before anything is opened or written, instead of being baked in as 0, false or "".
    FMetasoundFrontendLiteral DefaultLiteral;
    bool bHasDefault = false;
    {
        const TCHAR* Field = nullptr;
        EMetaSoundValueKind Kind = EMetaSoundValueKind::Float;
        if (const TSharedPtr<FJsonValue> Value = MetaSound_PickValueField(Params, TEXT("default_value"), Field, Kind))
        {
            const FString KindError = MetaSound_ValueKindError(*Value, Kind);
            if (!KindError.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s %s. Nothing was written."), Field, *KindError));
            }
            // The field is known to hold its JSON type, so each read below takes it as it is.
            switch (Kind)
            {
            case EMetaSoundValueKind::Bool:
            {
                bool B = false; Params->TryGetBoolField(TEXT("bool_value"), B);
                DefaultLiteral.Set(B);
                break;
            }
            case EMetaSoundValueKind::Int32:
            {
                int32 I = 0; Params->TryGetNumberField(TEXT("int_value"), I);
                DefaultLiteral.Set(I);
                break;
            }
            case EMetaSoundValueKind::String:
            {
                FString S; Params->TryGetStringField(TEXT("string_value"), S);
                DefaultLiteral.Set(S);
                break;
            }
            default:
            {
                double V = 0.0; Params->TryGetNumberField(TEXT("default_value"), V);
                DefaultLiteral.Set(static_cast<float>(V));
                break;
            }
            }
            bHasDefault = true;

            // An unregistered data type is left to AddGraphInputNode, which refuses it.
            const Metasound::Frontend::IDataTypeRegistry& Registry = Metasound::Frontend::IDataTypeRegistry::Get();
            const FName DataTypeName(*DataType);
            if (Registry.IsRegistered(DataTypeName) && !Registry.IsLiteralTypeSupported(DataTypeName, DefaultLiteral.GetType()))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Data type '%s' takes no %s default (%s). Nothing was written."),
                    *DataType, MetaSound_ValueKindName(Kind), Field));
            }
        }
    }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    // AddGraphInputNode hands back an input that already has this name, whatever its type, without touching it
    // (the default passed in is dropped), so a taken name is answered here without calling it: the same type is
    // the state asked for (nothing is written or saved), another type is an error.
    const FName InputFName(*InputName);
    if (Builder->GetConstBuilder().FindGraphInputNode(InputFName) != nullptr)
    {
        const FMetasoundFrontendClassInput* ExistingInput = Builder->GetConstBuilder().FindGraphInput(InputFName);
        const FName ExistingType = ExistingInput ? ExistingInput->TypeName : FName();
        if (!ExistingType.IsEqual(FName(*DataType), ENameCase::IgnoreCase))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Graph input '%s' already exists with data type '%s', not '%s'; remove it first (remove_metasound_graph_input) to change its type"),
                *InputName, *ExistingType.ToString(), *DataType));
        }
        TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
        Data->SetStringField(TEXT("input_name"), InputName);
        Data->SetStringField(TEXT("data_type"), ExistingType.ToString());
        Data->SetBoolField(TEXT("added"), false);
        Data->SetBoolField(TEXT("already_exists"), true);
        Data->SetBoolField(TEXT("has_default"), false);
        Data->SetBoolField(TEXT("saved"), false);
        if (bHasDefault)
        {
            Data->SetStringField(TEXT("note"),
                TEXT("The input already exists; its default was left unchanged (set it with set_metasound_graph_input_default)."));
        }
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundAddGraphInput", "MetaSound Add Graph Input"));
    Source->Modify();

    EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundBuilderNodeOutputHandle OutputHandle =
        Builder->AddGraphInputNode(FName(*InputName), FName(*DataType), DefaultLiteral, AddResult, /*bIsConstructorInput*/ false);

    if (AddResult != EMetaSoundBuilderResult::Succeeded || !OutputHandle.IsSet())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddGraphInputNode failed for '%s' (type '%s')"), *InputName, *DataType));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("input_name"), InputName);
    Data->SetStringField(TEXT("data_type"), DataType);
    Data->SetBoolField(TEXT("added"), true);
    Data->SetBoolField(TEXT("has_default"), bHasDefault);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_metasound_graph_input"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_metasound_graph_output
// FindOrBeginBuilding for an existing asset + AddGraphOutputNode(name, dataType,
// default literal). Adds a real graph output node. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:add_metasound_graph_output - Add a graph output of data_type (default Audio; others include Float, Int32, Bool, String and Trigger) to an existing MetaSound Source through the MetaSound builder, inside an undo transaction, then save the asset. An unregistered data_type or a source that does not load is an error. A name already used by an output is left untouched: with the same data_type the call succeeds with added=false and already_exists=true and saves nothing; with another data_type it is an error. success is false, with save_error, when the save fails (during PIE, for one). Inverse: remove_metasound_graph_output.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param output_name - Name of the graph output node to add.
//   @param data_type - Frontend data type for the output node (optional, default "Audio")
//   @returns { asset_path: string, output_name: string, data_type: string, added: bool, already_exists?: bool, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleAddMetaSoundGraphOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    FString OutputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_name"), OutputName)) { return Missing; }

    FString DataType = TEXT("Audio");
    Params->TryGetStringField(TEXT("data_type"), DataType);

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    // AddGraphOutputNode hands back an output that already has this name and resets its default literal, so a
    // taken name is answered here without calling it: the same type is the state asked for (nothing is written
    // or saved), another type is an error.
    const FName OutputFName(*OutputName);
    if (Builder->GetConstBuilder().FindGraphOutputNode(OutputFName) != nullptr)
    {
        const FMetasoundFrontendClassOutput* ExistingOutput = Builder->GetConstBuilder().FindGraphOutput(OutputFName);
        const FName ExistingType = ExistingOutput ? ExistingOutput->TypeName : FName();
        if (!ExistingType.IsEqual(FName(*DataType), ENameCase::IgnoreCase))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Graph output '%s' already exists with data type '%s', not '%s'; remove it first (remove_metasound_graph_output) to change its type"),
                *OutputName, *ExistingType.ToString(), *DataType));
        }
        TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
        Data->SetStringField(TEXT("output_name"), OutputName);
        Data->SetStringField(TEXT("data_type"), ExistingType.ToString());
        Data->SetBoolField(TEXT("added"), false);
        Data->SetBoolField(TEXT("already_exists"), true);
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundAddGraphOutput", "MetaSound Add Graph Output"));
    Source->Modify();

    EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
    FMetasoundFrontendLiteral DefaultLiteral;  // empty -> data-type default
    const FMetaSoundBuilderNodeInputHandle InputHandle =
        Builder->AddGraphOutputNode(FName(*OutputName), FName(*DataType), DefaultLiteral, AddResult, /*bIsConstructorOutput*/ false);

    if (AddResult != EMetaSoundBuilderResult::Succeeded || !InputHandle.IsSet())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddGraphOutputNode failed for '%s' (type '%s')"), *OutputName, *DataType));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("output_name"), OutputName);
    Data->SetStringField(TEXT("data_type"), DataType);
    Data->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_metasound_graph_output"), SaveErr);
}

// ---------------------------------------------------------------------------
// connect_metasound_graph_input_to_output
// Connects a graph INPUT node's output to a graph OUTPUT node's input.
// 'from_node_output_name' resolves a graph-input node via FindGraphInputNode
// (which yields its output handle); 'to_node_input_name' resolves a graph-output
// node via FindGraphOutputNode (which yields its input handle). The handles are
// passed to ConnectNodes; the connection is real where the two data types match
// (the cov fixture deliberately uses a guaranteed-compatible Float->Float pair).
// Verified via NodesAreConnected. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:connect_metasound_graph_input_to_output - Connects a graph input node's output to a graph output node's input.  Resolves the source handle from a graph INPUT node by name (FindGraphInputNode -> the input node's output handle) and the destination handle from a graph OUTPUT node by name (FindGraphOutputNode -> the output node's input handle), then calls UMetaSoundBuilderBase::ConnectNodes and verifies via NodesAreConnected. The connection is real where the two data types match (e.g. a Float graph input connected to a Float graph output). Saves the asset. Graph input to a node: connect_metasound_graph_input_to_node; edges: list_metasound_connections.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param from_node_output_name - Name of the graph INPUT to use as the connection source.
//   @param to_node_input_name - Name of the graph OUTPUT to use as the connection target.
//   @returns { asset_path: string, from_node_output_name: string, to_node_input_name: string, from_data_type: string, to_data_type: string, connected: bool, result: string, verified: bool, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleConnectMetaSoundNodes(const TSharedPtr<FJsonObject>& Params)
{
    FMetaSoundGraphIOEdge Edge;
    if (TSharedPtr<FJsonObject> Error = MetaSound_ResolveGraphIOEdge(Params, Edge))
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundConnectNodes", "MetaSound Connect Nodes"));
    Edge.Source->Modify();

    EMetaSoundBuilderResult ConnectResult = EMetaSoundBuilderResult::Failed;
    Edge.Builder->ConnectNodes(Edge.FromOutputHandle, Edge.ToInputHandle, ConnectResult);

    const bool bConnected = (ConnectResult == EMetaSoundBuilderResult::Succeeded);
    if (!bConnected)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("ConnectNodes failed connecting '%s' (%s) -> '%s' (%s) -- data types may be incompatible"),
                *Edge.FromOutputName, *Edge.FromDataType.ToString(), *Edge.ToInputName, *Edge.ToDataType.ToString()));
    }

    // Verify the connection now exists.
    const bool bVerified = Edge.Builder->NodesAreConnected(Edge.FromOutputHandle, Edge.ToInputHandle);

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Edge.Source, SaveErr);

    TSharedPtr<FJsonObject> Data = Edge.MakePayload();
    Data->SetBoolField(TEXT("connected"), bConnected);
    Data->SetStringField(TEXT("result"), bConnected ? TEXT("Succeeded") : TEXT("Failed"));
    Data->SetBoolField(TEXT("verified"), bVerified);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("connect_metasound_graph_input_to_output"), SaveErr);
}

// ===========================================================================
// Real DSP patch authoring (process nodes, not just graph I/O)

// ---------------------------------------------------------------------------
// add_metasound_node
// Adds a real DSP/process node (e.g. the UE Sine oscillator) to an existing source's
// graph by its registered class name (namespace/name/variant), via the non-deprecated
// AddNodeByClassName overload. Returns the new node's GUID + its input/output pins
// (discovered at runtime) so a follow-up call can set defaults / wire it.
// ---------------------------------------------------------------------------
// MCP:add_metasound_node - Add a real process node to a MetaSound Source graph by its registered class name (e.g. UE/Sine/Audio).  Unlike add_metasound_graph_input (which adds a graph input node), this instantiates a DSP node so you can author a real signal patch headless. Returns the node's GUID (node_id) and its enumerated input/output pins (name + data_type) for follow-up set/connect calls. The asset is saved. NOT idempotent: each call adds a new node instance (delete + recreate the source for a clean run). Wire it with connect_metasound_nodes, connect_metasound_graph_input_to_node or connect_metasound_node_to_graph_output; edges: list_metasound_connections.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param class_namespace - Registered node class namespace, e.g. "UE".
//   @param class_name - Registered node class name, e.g. "Sine".
//   @param class_variant - Optional class variant, e.g. "Audio" (omit for the default/no-variant class).
//   @param major_version - Optional major version (default 1).
//   @returns { asset_path: string, node_id: string, class_namespace: string, class_name: string, class_variant: string, inputs: [{name,data_type}], outputs: [{name,data_type}], added: bool, saved: bool }

TSharedPtr<FJsonObject> HandleMetaSoundAddNodeByClassName(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, Namespace, ClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("class_namespace"), Namespace) || Namespace.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Missing required 'class_namespace' parameter (e.g. 'UE')"));
    if (!Params->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
        return mcp_common::MakeErrorReply(TEXT("Missing required 'class_name' parameter (e.g. 'Sine')"));
    FString Variant;
    Params->TryGetStringField(TEXT("class_variant"), Variant);  // optional (e.g. "Audio")
    int32 MajorVersion = 1;
    Params->TryGetNumberField(TEXT("major_version"), MajorVersion);

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
        return Error;

    const FMetasoundFrontendClassName FrontendClassName = Variant.IsEmpty()
        ? FMetasoundFrontendClassName(FName(*Namespace), FName(*ClassName))
        : FMetasoundFrontendClassName(FName(*Namespace), FName(*ClassName), FName(*Variant));

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundAddNodeByClassName", "MetaSound Add Node By ClassName"));
    Source->Modify();

    EMetaSoundBuilderResult AddResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundNodeHandle NodeHandle = Builder->AddNodeByClassName(FrontendClassName, AddResult, MajorVersion);
    if (AddResult != EMetaSoundBuilderResult::Succeeded || !NodeHandle.IsSet())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddNodeByClassName failed for %s/%s/%s (v%d) -- class may not be registered"),
                *Namespace, *ClassName, *Variant, MajorVersion));

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("node_id"), NodeHandle.NodeID.ToString());
    Data->SetStringField(TEXT("class_namespace"), Namespace);
    Data->SetStringField(TEXT("class_name"), ClassName);
    Data->SetStringField(TEXT("class_variant"), Variant);
    Data->SetArrayField(TEXT("inputs"), EnumerateNodeInputs(Builder, NodeHandle));
    Data->SetArrayField(TEXT("outputs"), EnumerateNodeOutputs(Builder, NodeHandle));
    Data->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_metasound_node"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_metasound_node_input_default
// Sets the literal default of a NAMED input pin on a CONCRETE node (by node_id GUID),
// then reads it back to verify. Mirrors set_metasound_graph_input_default but for process
// nodes, not graph inputs.
// ---------------------------------------------------------------------------
// MCP:set_metasound_node_input_default - Set the default literal of a named input pin on a concrete node of a MetaSound Source (node by node_id from add_metasound_node) through the MetaSound editor builder, then read it back and save the asset. Give one typed field: bool_value (true or false), int_value (a whole number), string_value (a string) or value (a number, read as a float); with several, the first in that order wins. A missing or mistyped value, an unknown source, node_id or pin, or a literal the pin refuses is an error. A failed save makes success false. For a graph input use set_metasound_graph_input_default.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param node_id - GUID of the node (from add_metasound_node).
//   @param input_name - Name of the node input pin (discover via the node's enumerated inputs).
//   @param value - Float default, a JSON number (optional, used if no other *_value is provided).
//   @param int_value - Int32 default, a whole JSON number (optional, mutually exclusive).
//   @param bool_value - Bool default, JSON true or false (optional, mutually exclusive).
//   @param string_value - String default, a JSON string (optional, mutually exclusive).
//   @returns { asset_path: string, node_id: string, input_name: string, value_type: string, value_set: bool, verified: bool, value_read_back: string, saved: bool }

TSharedPtr<FJsonObject> HandleMetaSoundSetNodeInputDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, NodeId, InputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName)) { return Missing; }

    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
        return MetaSound_SourceNotFoundReply(MetaSoundPath);

    FMetaSoundNodeHandle NodeHandle;
    if (!ParseMetaSoundNodeHandle(NodeId, NodeHandle))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid node_id (expected a node GUID from add_metasound_node): %s"), *NodeId));

    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = GetBuilderForSource(Source, MetaSoundPath, Error);
    if (!Builder)
        return Error;

    EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundBuilderNodeInputHandle InputHandle = Builder->FindNodeInputByName(NodeHandle, FName(*InputName), FindResult);
    if (FindResult != EMetaSoundBuilderResult::Succeeded || !InputHandle.IsSet())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Node input '%s' not found on node %s"), *InputName, *NodeId));

    FMetasoundFrontendLiteral Literal;
    FString ValueType;
    if (TSharedPtr<FJsonObject> ValueError = BuildLiteralFromParams(Params, Literal, ValueType)) { return ValueError; }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundSetNodeInputDefault", "MetaSound Set Node Input Default"));
    Source->Modify();

    EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
    Builder->SetNodeInputDefault(InputHandle, Literal, SetResult);
    if (SetResult != EMetaSoundBuilderResult::Succeeded)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetNodeInputDefault failed for input '%s' on node %s (literal type may not match the pin)"), *InputName, *NodeId));

    EMetaSoundBuilderResult GetResult = EMetaSoundBuilderResult::Failed;
    const FMetasoundFrontendLiteral ReadBack = Builder->GetNodeInputDefault(InputHandle, GetResult);

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("node_id"), NodeId);
    Data->SetStringField(TEXT("input_name"), InputName);
    Data->SetStringField(TEXT("value_type"), ValueType);
    Data->SetBoolField(TEXT("value_set"), true);
    Data->SetBoolField(TEXT("verified"), GetResult == EMetaSoundBuilderResult::Succeeded);
    Data->SetStringField(TEXT("value_read_back"), ReadBack.ToString());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_metasound_node_input_default"));
}

// ---------------------------------------------------------------------------
// connect_metasound_node_to_graph_output
// Connects a concrete node's named output pin to a named graph output via the
// purpose-built ConnectNodeOutputToGraphOutput (idempotent: it clears any existing
// edge before adding). Single canonical path (no runtime fallback).
// ---------------------------------------------------------------------------
// MCP:connect_metasound_node_to_graph_output - Connect a concrete node's output pin to a named graph output, completing a signal patch (e.g. Sine.Audio -> the source's audio output).  Uses ConnectNodeOutputToGraphOutput (idempotent: replaces any existing edge). Reports the honest EMetaSoundBuilderResult. Node to node: connect_metasound_nodes; edges: list_metasound_connections.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param node_id - GUID of the source node (from add_metasound_node).
//   @param node_output_name - Name of the node's output pin (e.g. "Audio").
//   @param graph_output_name - Name of the target graph output (discover via get_metasound_info).
//   @returns { asset_path: string, node_id: string, node_output_name: string, graph_output_name: string, connected: bool, result: string, saved: bool }

TSharedPtr<FJsonObject> HandleMetaSoundConnectNodeToGraphOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, NodeId, NodeOutputName, GraphOutputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_output_name"), NodeOutputName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("graph_output_name"), GraphOutputName)) { return Missing; }

    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
        return MetaSound_SourceNotFoundReply(MetaSoundPath);

    FMetaSoundNodeHandle NodeHandle;
    if (!ParseMetaSoundNodeHandle(NodeId, NodeHandle))
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid node_id (expected a node GUID): %s"), *NodeId));

    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = GetBuilderForSource(Source, MetaSoundPath, Error);
    if (!Builder)
        return Error;

    EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundBuilderNodeOutputHandle OutputHandle = Builder->FindNodeOutputByName(NodeHandle, FName(*NodeOutputName), FindResult);
    if (FindResult != EMetaSoundBuilderResult::Succeeded || !OutputHandle.IsSet())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Node output '%s' not found on node %s"), *NodeOutputName, *NodeId));

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundConnectNodeToGraphOutput", "MetaSound Connect Node To Graph Output"));
    Source->Modify();

    EMetaSoundBuilderResult ConnectResult = EMetaSoundBuilderResult::Failed;
    Builder->ConnectNodeOutputToGraphOutput(FName(*GraphOutputName), OutputHandle, ConnectResult);
    const bool bConnected = (ConnectResult == EMetaSoundBuilderResult::Succeeded);
    if (!bConnected)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("ConnectNodeOutputToGraphOutput failed: node %s output '%s' -> graph output '%s' (name/type mismatch?)"),
                *NodeId, *NodeOutputName, *GraphOutputName));

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("node_id"), NodeId);
    Data->SetStringField(TEXT("node_output_name"), NodeOutputName);
    Data->SetStringField(TEXT("graph_output_name"), GraphOutputName);
    Data->SetBoolField(TEXT("connected"), bConnected);
    Data->SetStringField(TEXT("result"), TEXT("Succeeded"));
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("connect_metasound_node_to_graph_output"), SaveErr);
}

// ---------------------------------------------------------------------------
// disconnect_metasound_node_from_graph_output
// Inverse of connect_metasound_node_to_graph_output. Resolves the graph OUTPUT
// node's INPUT handle by name (FindGraphOutputNode), queries NodeInputIsConnected
// first for honest idempotency, then calls UMetaSoundBuilderBase::DisconnectNodeInput
// to clear whatever node output drives that graph output. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:disconnect_metasound_node_from_graph_output - Disconnects whatever node output currently drives a named graph output, clearing the edge (inverse of connect_metasound_node_to_graph_output).  Resolves the graph output node's input handle by name (FindGraphOutputNode) and queries NodeInputIsConnected first: if the input is already unconnected it returns already_disconnected without error (idempotent), otherwise it calls UMetaSoundBuilderBase::DisconnectNodeInput and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param graph_output_name - Name of the target graph output to disconnect (discover via get_metasound_info).
//   @returns { asset_path: string, graph_output_name: string, data_type: string, disconnected: bool, already_disconnected: bool, result: string, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleMetaSoundDisconnectNodeFromGraphOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, GraphOutputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("graph_output_name"), GraphOutputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
        return Error;

    FName OutDataType;
    FMetaSoundBuilderNodeInputHandle InputHandle;
    EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
    Builder->FindGraphOutputNode(FName(*GraphOutputName), OutDataType, InputHandle, FindResult);
    if (FindResult != EMetaSoundBuilderResult::Succeeded || !InputHandle.IsSet())
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("graph output not found: %s"), *GraphOutputName));

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("graph_output_name"), GraphOutputName);
    Data->SetStringField(TEXT("data_type"), OutDataType.ToString());

    // Honest idempotency: if the graph output's input is already unconnected,
    // report it as such instead of issuing a DisconnectNodeInput that would honestly fail.
    if (!Builder->NodeInputIsConnected(InputHandle))
    {
        Data->SetBoolField(TEXT("disconnected"), false);
        Data->SetBoolField(TEXT("already_disconnected"), true);
        Data->SetStringField(TEXT("result"), TEXT("AlreadyDisconnected"));
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundDisconnectNodeFromGraphOutput", "MetaSound Disconnect Node From Graph Output"));
    Source->Modify();

    EMetaSoundBuilderResult DisconnectResult = EMetaSoundBuilderResult::Failed;
    Builder->DisconnectNodeInput(InputHandle, DisconnectResult);
    const bool bDisconnected = (DisconnectResult == EMetaSoundBuilderResult::Succeeded);
    if (!bDisconnected)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("DisconnectNodeInput failed for graph output '%s' (%s)"),
                *GraphOutputName, *OutDataType.ToString()));

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    Data->SetBoolField(TEXT("disconnected"), bDisconnected);
    Data->SetBoolField(TEXT("already_disconnected"), false);
    Data->SetStringField(TEXT("result"), TEXT("Succeeded"));
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("disconnect_metasound_node_from_graph_output"), SaveErr);
}

// ===========================================================================
// Graph teardown (disconnect / remove node / remove graph input)

// ---------------------------------------------------------------------------
// disconnect_metasound_graph_input_from_output
// Inverse of connect_metasound_graph_input_to_output. Resolves the source handle from a graph
// INPUT node by name (FindGraphInputNode -> output handle) and the destination
// handle from a graph OUTPUT node by name (FindGraphOutputNode -> input handle),
// queries NodesAreConnected first for honest idempotency, then calls
// UMetaSoundBuilderBase::DisconnectNodes. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:disconnect_metasound_graph_input_from_output - Disconnects a graph input node's output from a graph output node's input (inverse of connect_metasound_graph_input_to_output).  Resolves the source handle from a graph INPUT node by name (FindGraphInputNode -> the input node's output handle) and the destination handle from a graph OUTPUT node by name (FindGraphOutputNode -> the output node's input handle), then queries NodesAreConnected first: if the edge is already absent it returns already_disconnected without error (idempotent), otherwise it calls UMetaSoundBuilderBase::DisconnectNodes and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param from_node_output_name - Name of the graph INPUT whose output is the connection source.
//   @param to_node_input_name - Name of the graph OUTPUT whose input is the connection target.
//   @returns { asset_path: string, from_node_output_name: string, to_node_input_name: string, from_data_type: string, to_data_type: string, disconnected: bool, already_disconnected: bool, result: string, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleDisconnectMetaSoundNodes(const TSharedPtr<FJsonObject>& Params)
{
    FMetaSoundGraphIOEdge Edge;
    if (TSharedPtr<FJsonObject> Error = MetaSound_ResolveGraphIOEdge(Params, Edge))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = Edge.MakePayload();

    // Honest idempotency: if the edge is already absent, report it as such
    // instead of issuing a DisconnectNodes that would honestly fail.
    if (!Edge.Builder->NodesAreConnected(Edge.FromOutputHandle, Edge.ToInputHandle))
    {
        Data->SetBoolField(TEXT("disconnected"), false);
        Data->SetBoolField(TEXT("already_disconnected"), true);
        Data->SetStringField(TEXT("result"), TEXT("Succeeded"));
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"),
            FString::Printf(TEXT("'%s' -> '%s' was not connected; nothing to disconnect"), *Edge.FromOutputName, *Edge.ToInputName));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundDisconnectNodes", "MetaSound Disconnect Nodes"));
    Edge.Source->Modify();

    EMetaSoundBuilderResult DisconnectResult = EMetaSoundBuilderResult::Failed;
    Edge.Builder->DisconnectNodes(Edge.FromOutputHandle, Edge.ToInputHandle, DisconnectResult);

    const bool bDisconnected = (DisconnectResult == EMetaSoundBuilderResult::Succeeded);
    if (!bDisconnected)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("DisconnectNodes failed disconnecting '%s' (%s) -> '%s' (%s)"),
                *Edge.FromOutputName, *Edge.FromDataType.ToString(), *Edge.ToInputName, *Edge.ToDataType.ToString()));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Edge.Source, SaveErr);

    Data->SetBoolField(TEXT("disconnected"), bDisconnected);
    Data->SetBoolField(TEXT("already_disconnected"), false);
    Data->SetStringField(TEXT("result"), TEXT("Succeeded"));
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("disconnect_metasound_graph_input_from_output"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_metasound_node
// Removes a node (by its GUID node_id, as returned by add_metasound_node)
// via UMetaSoundBuilderBase::RemoveNode(bRemoveUnusedDependencies=true). Queries
// ContainsNode first for honest idempotency (already_absent without error). Saves.
// ---------------------------------------------------------------------------
// MCP:remove_metasound_node - Removes a node from a MetaSound Source graph by its node_id GUID (inverse of add_metasound_node).  Parses the node_id into a node handle and queries ContainsNode first: if the node is already absent it returns already_absent without error (idempotent), otherwise it calls UMetaSoundBuilderBase::RemoveNode (pruning unused dependencies) and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param node_id - GUID of the node to remove (from add_metasound_node).
//   @returns { asset_path: string, node_id: string, removed: bool, already_absent: bool, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleRemoveMetaSoundNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, NodeId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }

    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
    {
        return MetaSound_SourceNotFoundReply(MetaSoundPath);
    }

    FMetaSoundNodeHandle NodeHandle;
    if (!ParseMetaSoundNodeHandle(NodeId, NodeHandle))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid node_id (expected a node GUID from add_metasound_node): %s"), *NodeId));
    }

    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = GetBuilderForSource(Source, MetaSoundPath, Error);
    if (!Builder)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("node_id"), NodeId);

    // Honest idempotency: if the node is already gone, say so without error.
    if (!Builder->ContainsNode(NodeHandle))
    {
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetBoolField(TEXT("already_absent"), true);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"),
            FString::Printf(TEXT("node %s not present in graph; nothing to remove"), *NodeId));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundRemoveNode", "MetaSound Remove Node"));
    Source->Modify();

    EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
    Builder->RemoveNode(NodeHandle, RemoveResult, /*bRemoveUnusedDependencies*/ true);

    const bool bRemoved = (RemoveResult == EMetaSoundBuilderResult::Succeeded);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveNode failed for node %s"), *NodeId));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    Data->SetBoolField(TEXT("removed"), bRemoved);
    Data->SetBoolField(TEXT("already_absent"), false);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_metasound_node"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_metasound_graph_input
// Removes a graph input member (and its node) by name via
// UMetaSoundBuilderBase::RemoveGraphInput. Inverse of add_metasound_graph_input.
// Queries GetGraphInputNames first for honest idempotency (already_absent). Saves.
// ---------------------------------------------------------------------------
// MCP:remove_metasound_graph_input - Removes a graph INPUT (and its node) from a MetaSound Source by name (inverse of add_metasound_graph_input).  Queries GetGraphInputNames first: if the input is already absent it returns already_absent without error (idempotent), otherwise it calls UMetaSoundBuilderBase::RemoveGraphInput and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param input_name - Name of the graph input to remove.
//   @returns { asset_path: string, input_name: string, removed: bool, already_absent: bool, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleRemoveMetaSoundGraphInput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, InputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("input_name"), InputName);

    // Honest idempotency: if the input isn't declared, report already_absent.
    const FName InputFName(*InputName);
    EMetaSoundBuilderResult NamesResult = EMetaSoundBuilderResult::Failed;
    const TArray<FName> InputNames = Builder->GetGraphInputNames(NamesResult);
    if (!InputNames.Contains(InputFName))
    {
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetBoolField(TEXT("already_absent"), true);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"),
            FString::Printf(TEXT("graph input '%s' not declared; nothing to remove"), *InputName));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundRemoveGraphInput", "MetaSound Remove Graph Input"));
    Source->Modify();

    EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
    Builder->RemoveGraphInput(InputFName, RemoveResult);

    const bool bRemoved = (RemoveResult == EMetaSoundBuilderResult::Succeeded);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveGraphInput failed for input '%s'"), *InputName));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    Data->SetBoolField(TEXT("removed"), bRemoved);
    Data->SetBoolField(TEXT("already_absent"), false);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_metasound_graph_input"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_metasound_graph_output
// Removes a graph output member (and its node) by name via
// UMetaSoundBuilderBase::RemoveGraphOutput. Inverse of add_metasound_graph_output.
// Queries GetGraphOutputNames first for honest idempotency (already_absent). Saves.
// ---------------------------------------------------------------------------
// MCP:remove_metasound_graph_output - Removes a graph OUTPUT (and its node) from a MetaSound Source by name (inverse of add_metasound_graph_output).  Queries GetGraphOutputNames first: if the output is already absent it returns already_absent without error (idempotent), otherwise it calls UMetaSoundBuilderBase::RemoveGraphOutput and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param output_name - Name of the graph output to remove.
//   @returns { asset_path: string, output_name: string, removed: bool, already_absent: bool, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleRemoveMetaSoundGraphOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, OutputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_name"), OutputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("output_name"), OutputName);

    // Honest idempotency: if the output isn't declared, report already_absent.
    const FName OutputFName(*OutputName);
    EMetaSoundBuilderResult NamesResult = EMetaSoundBuilderResult::Failed;
    const TArray<FName> OutputNames = Builder->GetGraphOutputNames(NamesResult);
    if (!OutputNames.Contains(OutputFName))
    {
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetBoolField(TEXT("already_absent"), true);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"),
            FString::Printf(TEXT("graph output '%s' not declared; nothing to remove"), *OutputName));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundRemoveGraphOutput", "MetaSound Remove Graph Output"));
    Source->Modify();

    EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
    Builder->RemoveGraphOutput(OutputFName, RemoveResult);

    const bool bRemoved = (RemoveResult == EMetaSoundBuilderResult::Succeeded);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveGraphOutput failed for output '%s'"), *OutputName));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    Data->SetBoolField(TEXT("removed"), bRemoved);
    Data->SetBoolField(TEXT("already_absent"), false);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_metasound_graph_output"), SaveErr);
}
// =========================================================================
// MetaSound node reads + reflection-driven introspection
//
// Engine facts this section relies on (UE 5.8, Engine/Plugins/Runtime/Metasound/):
//   - UMetaSoundSource extends USoundWaveProcedural + FMetasoundAssetBase + IMetaSoundDocumentInterface
//   - GetConstDocument() returns FMetasoundFrontendDocument; nodes are at
//     Doc.RootGraph.GetConstDefaultGraph().Nodes (no top-level GetNodes())
//   - FMetasoundFrontendVertex (NOT FMetasoundFrontendPin) is the pin model;
//     Node.Interface.{Inputs,Outputs} are TArray<FMetasoundFrontendVertex>
//   - Metasound::Frontend::ISearchEngine::Get().FindAllClasses() for node-type reflection
//
// Every command of this section is headless-safe: editor-only asset reads + reflection +
// SaveAssetPackage. No PIE, no runtime evaluation.
//
// Traps:
//   - There is no public "compile MetaSound" API: compilation is implicit in PreSave /
//     UpdateAndRegisterForExecution, so no compile command exists and saving through the
//     build-and-commit path compiles.
//   - The UMetaSoundSource::Document getter is private; only GetConstDocument() (via
//     IMetaSoundDocumentInterface) is public.

// ---------------------------------------------------------------------------
// list_metasound_node_types — ISearchEngine reflection of registered node classes
// ---------------------------------------------------------------------------
// MCP:list_metasound_node_types - Enumerate all MetaSound node classes registered with the engine (via Metasound::Frontend::ISearchEngine). Editor-only because ISearchEngine::FindAllClasses is gated by WITH_EDITORONLY_DATA.
//   @param include_unloaded - If true (default), include classes from assets not yet loaded into memory.
//   @returns { count, types: [{ class_name, class_path, type, version, is_native }] }
TSharedPtr<FJsonObject> HandleListMetaSoundNodeTypes(
	const TSharedPtr<FJsonObject>& Params)
{
	bool bIncludeUnloaded = true;
	Params->TryGetBoolField(TEXT("include_unloaded"), bIncludeUnloaded);

	TArray<TSharedPtr<FJsonValue>> TypesJson;
try
	{
#if UE_VERSION_OLDER_THAN(5, 8, 0)
		// UE 5.7's ISearchEngine::FindAllClasses(bool) returns the registered classes as FMetasoundFrontendClass,
		// without unloaded assets, and its FMetaSoundClassInfo has no ClassType (both 5.8). The same entries by
		// hand, as 5.8 builds them: the registered classes minus the deprecated ones (FindAllClasses(false) is the
		// query 5.8 runs for EResultVersion::Highest), then for include_unloaded the asset manager's latest tag
		// data of every MetaSound asset whose class is not registered, deprecated ones left out; an asset class
		// is External.
		using namespace Metasound::Frontend;
		auto AddEntry = [&TypesJson](const FMetasoundFrontendClassName& ClassName, EMetasoundFrontendClassType ClassType,
			const FMetasoundFrontendVersionNumber& Version, EMetasoundFrontendClassAccessFlags AccessFlags)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class_name"),
				ClassName.Name.ToString());
			Entry->SetStringField(TEXT("class_path"),
				ClassName.ToString());
			Entry->SetStringField(TEXT("type"),
				UEnum::GetValueAsString(ClassType));
			Entry->SetStringField(TEXT("version"),
				Version.ToString());
			Entry->SetStringField(TEXT("access_flags"),
				UEnum::GetValueAsString(AccessFlags));
			Entry->SetBoolField  (TEXT("is_native"), !ClassName.Namespace.IsNone());
			TypesJson.Add(MakeShared<FJsonValueObject>(Entry));
		};

		const TArray<FMetasoundFrontendClass> Classes = ISearchEngine::Get().FindAllClasses(/*bInIncludeAllVersions*/ false);
		TSet<FNodeClassRegistryKey> RegisteredKeys;
		TypesJson.Reserve(Classes.Num());
		for (const FMetasoundFrontendClass& Class : Classes)
		{
			RegisteredKeys.Add(FNodeClassRegistryKey(Class.Metadata));
			AddEntry(Class.Metadata.GetClassName(), Class.Metadata.GetType(), Class.Metadata.GetVersion(), Class.Metadata.GetAccessFlags());
		}
		if (bIncludeUnloaded)
		{
			if (const Metasound::Engine::FMetaSoundAssetManager* AssetManager = Metasound::Engine::FMetaSoundAssetManager::Get())
			{
				AssetManager->IterateAssetTagData([&RegisteredKeys, &AddEntry](FMetaSoundAssetClassInfo ClassInfo)
				{
					if (EnumHasAnyFlags(ClassInfo.AccessFlags, EMetasoundFrontendClassAccessFlags::Deprecated))
					{
						return;
					}
					if (!RegisteredKeys.Contains(FNodeClassRegistryKey(EMetasoundFrontendClassType::External, ClassInfo.ClassName, ClassInfo.Version)))
					{
						AddEntry(ClassInfo.ClassName, EMetasoundFrontendClassType::External, ClassInfo.Version, ClassInfo.AccessFlags);
					}
				});
			}
		}
#else
		auto& SE = Metasound::Frontend::ISearchEngine::Get();
		const TArray<Metasound::Frontend::FMetaSoundClassInfo> Infos = SE.FindAllClasses(
			Metasound::Frontend::ISearchEngine::EResultVersion::Highest, bIncludeUnloaded);

		TypesJson.Reserve(Infos.Num());
		for (const Metasound::Frontend::FMetaSoundClassInfo& Info : Infos)
		{
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("class_name"),
				Info.ClassName.Name.ToString());
			Entry->SetStringField(TEXT("class_path"),
				Info.ClassName.ToString());
			Entry->SetStringField(TEXT("type"),
				UEnum::GetValueAsString(Info.ClassType));
			Entry->SetStringField(TEXT("version"),
				Info.Version.ToString());
			Entry->SetStringField(TEXT("access_flags"),
				UEnum::GetValueAsString(Info.AccessFlags));
			Entry->SetBoolField  (TEXT("is_native"), !Info.ClassName.Namespace.IsNone());
			TypesJson.Add(MakeShared<FJsonValueObject>(Entry));
		}
#endif
	}
	catch (...)
	{
		return mcp_common::MakeErrorReply(
			TEXT("ISearchEngine::Get() failed (MetaSound search engine not initialized in this build)"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), TypesJson.Num());
	Data->SetArrayField (TEXT("types"), TypesJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_metasound_nodes — enumerate nodes in a MetaSound graph
// ---------------------------------------------------------------------------
// MCP:list_metasound_nodes - List every node in the default graph of a MetaSound Source asset with its GUID id, name, class name and input/output counts, plus count. Read-only; pass the id to get_metasound_node_info and the other node tools.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @returns { metasound_path, count, nodes: [{ id, name, class_name, input_count, output_count }] }
TSharedPtr<FJsonObject> HandleListMetaSoundNodes(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
	if (!Source)
	{
		return MetaSound_CannotLoadSourceReply(MetaSoundPath);
	}
	const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();
	const TArray<FMetasoundFrontendNode>& Nodes = Doc.RootGraph.GetConstDefaultGraph().Nodes;

	// Resolve node ClassID -> class name string via the document's dependency table.
	// UE 5.8: FMetasoundFrontendNode has no ClassName field; the class identifier is
	// the FGuid ClassID, and the name lives on the matching FMetasoundFrontendClass entry
	// (held on FMetasoundFrontendDocument::Dependencies, NOT on RootGraph).
	TMap<FGuid, FString> ClassIdToName;
	ClassIdToName.Reserve(Doc.Dependencies.Num());
	for (const FMetasoundFrontendClass& Dep : Doc.Dependencies)
	{
		ClassIdToName.Add(Dep.ID, Dep.Metadata.GetClassName().ToString());
	}

	TArray<TSharedPtr<FJsonValue>> NodesJson;
	NodesJson.Reserve(Nodes.Num());
	for (const FMetasoundFrontendNode& N : Nodes)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"),           N.GetID().ToString());
		Entry->SetStringField(TEXT("name"),         N.Name.ToString());
		const FString* Resolved = ClassIdToName.Find(N.ClassID);
		Entry->SetStringField(TEXT("class_name"),   Resolved ? *Resolved : FString());
		Entry->SetNumberField(TEXT("input_count"),  N.Interface.Inputs.Num());
		Entry->SetNumberField(TEXT("output_count"), N.Interface.Outputs.Num());
		NodesJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"), MetaSoundPath);
	Data->SetNumberField(TEXT("count"),          Nodes.Num());
	Data->SetArrayField (TEXT("nodes"),          NodesJson);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_metasound_node_info — full node detail (inputs/outputs/literals)
// ---------------------------------------------------------------------------
// MCP:get_metasound_node_info - Read one node of the default graph of a MetaSound Source by GUID: its name, class name, input/output/environment vertices (name, type_name, vertex_id), each input's stored default literal (has_value, literal_type, value; has_value false means no literal is set), the input literal defaults by vertex, and the Configuration struct class; input_name narrows inputs and input_defaults to one input, and an unknown input name fails listing the valid ones. Read-only; get node ids from list_metasound_nodes, write an input default with set_metasound_node_input_default.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_metasound_nodes returns it in id; a value that is not a GUID is refused.
//   @param input_name - Optional name of one input vertex (FMetasoundFrontendVertex::Name): inputs and input_defaults then hold only that input. A name the node has no input for is an error listing the inputs.
//   @returns { metasound_path, node_id, name, class_name, input_name?, inputs: [{name, type_name, vertex_id, has_value, literal_type?, value?}], outputs: [{name, type_name, vertex_id}], environment: [...], input_defaults: [{vertex_id, input_name, value, type}], configuration_class }
TSharedPtr<FJsonObject> HandleGetMetaSoundNodeInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath, NodeId;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
	// Optional filter: one input vertex by name.
	FString InputName;
	const bool bFilterInput = Params->TryGetStringField(TEXT("input_name"), InputName) && !InputName.IsEmpty();
	UMetaSoundSource* Source = nullptr;
	TSharedPtr<FJsonObject> Error;
	const FMetasoundFrontendNode* FoundNode = MetaSound_FindGraphNode(MetaSoundPath, NodeId, Source, Error);
	if (!FoundNode)
	{
		return Error;
	}
	const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();

	const FName InputFName(*InputName);
	if (bFilterInput && !FoundNode->Interface.Inputs.ContainsByPredicate(
		[&InputFName](const FMetasoundFrontendVertex& V) { return V.Name == InputFName; }))
	{
		FString AvailableInputs;
		for (const FMetasoundFrontendVertex& V : FoundNode->Interface.Inputs)
		{
			if (!AvailableInputs.IsEmpty()) { AvailableInputs += TEXT(","); }
			AvailableInputs += V.Name.ToString();
		}
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Input '%s' not found on node (available: %s)"),
				*InputName, *AvailableInputs));
	}
	// The vertex an input literal belongs to, by its VertexID.
	auto FindInputVertex = [FoundNode](const FGuid& VertexID)
	{
		return FoundNode->Interface.Inputs.FindByPredicate(
			[&VertexID](const FMetasoundFrontendVertex& V) { return V.VertexID == VertexID; });
	};

	auto SerializeVertices = [](const TArray<FMetasoundFrontendVertex>& Vs)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Vs.Num());
		for (const FMetasoundFrontendVertex& V : Vs)
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("name"),      V.Name.ToString());
			E->SetStringField(TEXT("type_name"), V.TypeName.ToString());
			E->SetStringField(TEXT("vertex_id"), V.VertexID.ToString());
			Out.Add(MakeShared<FJsonValueObject>(E));
		}
		return Out;
	};

	// Each input vertex with the default literal stored for it (has_value false when none is set).
	TArray<TSharedPtr<FJsonValue>> InputsJson;
	for (const FMetasoundFrontendVertex& V : FoundNode->Interface.Inputs)
	{
		if (bFilterInput && V.Name != InputFName)
		{
			continue;
		}
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("name"),      V.Name.ToString());
		E->SetStringField(TEXT("type_name"), V.TypeName.ToString());
		E->SetStringField(TEXT("vertex_id"), V.VertexID.ToString());
		const FMetasoundFrontendVertexLiteral* Lit = FoundNode->InputLiterals.FindByPredicate(
			[&V](const FMetasoundFrontendVertexLiteral& L) { return L.VertexID == V.VertexID; });
		E->SetBoolField(TEXT("has_value"), Lit != nullptr);
		if (Lit)
		{
			E->SetStringField(TEXT("literal_type"), UEnum::GetValueAsString(Lit->Value.GetType()));
			MetaSound_SetLiteralValueField(E, Lit->Value);
		}
		InputsJson.Add(MakeShared<FJsonValueObject>(E));
	}

	// Input literal defaults — captured as {vertex_id -> {input_name, type, value}}.
	TArray<TSharedPtr<FJsonValue>> InputDefaultsJson;
	for (const FMetasoundFrontendVertexLiteral& Lit : FoundNode->InputLiterals)
	{
		const FMetasoundFrontendVertex* LitVertex = FindInputVertex(Lit.VertexID);
		if (bFilterInput && (!LitVertex || LitVertex->Name != InputFName))
		{
			continue;
		}
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("vertex_id"), Lit.VertexID.ToString());
		E->SetStringField(TEXT("input_name"), LitVertex ? LitVertex->Name.ToString() : FString());
		const FString TypeName = UEnum::GetValueAsString(Lit.Value.GetType());
		E->SetStringField(TEXT("type"), TypeName);
		MetaSound_SetLiteralValueField(E, Lit.Value);
		InputDefaultsJson.Add(MakeShared<FJsonValueObject>(E));
	}

	const UScriptStruct* CfgStruct = FoundNode->Configuration.GetScriptStruct();

	// Resolve class name via Doc.Dependencies (UE 5.8: node has no ClassName field).
	const FMetasoundFrontendClass* Dep = Doc.Dependencies.FindByPredicate(
		[FoundNode](const FMetasoundFrontendClass& D) { return D.ID == FoundNode->ClassID; });
	const FString ResolvedClassName = Dep ? Dep->Metadata.GetClassName().ToString() : FString();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"),     MetaSoundPath);
	Data->SetStringField(TEXT("node_id"),            NodeId);
	Data->SetStringField(TEXT("name"),               FoundNode->Name.ToString());
	Data->SetStringField(TEXT("class_name"),         ResolvedClassName);
	if (bFilterInput)
	{
		Data->SetStringField(TEXT("input_name"),     InputName);
	}
	Data->SetArrayField (TEXT("inputs"),             InputsJson);
	Data->SetArrayField (TEXT("outputs"),            SerializeVertices(FoundNode->Interface.Outputs));
	Data->SetArrayField (TEXT("environment"),        SerializeVertices(FoundNode->Interface.Environment));
	Data->SetArrayField (TEXT("input_defaults"),     InputDefaultsJson);
	Data->SetStringField(TEXT("configuration_class"), CfgStruct ? CfgStruct->GetName() : TEXT(""));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_metasound_node_property / get_metasound_node_property
//   Reflection on the node's TInstancedStruct<FMetaSoundFrontendNodeConfiguration>.
//   The Configuration struct is per-subclass — for example ArrayGet's config has
//   NumOutputs, Index pin references, etc. We round-trip via FJsonObjectConverter.
// ---------------------------------------------------------------------------
// MCP:set_metasound_node_property - Write one field of a MetaSound Source node's Configuration struct, node found by GUID, by reflection directly in the frontend document (not through the MetaSound builder), then save the asset. set_properties cannot reach it: frontend nodes are document data, not UObjects. No undo step. Re-applying a value returns value_changed false but still saves. Succeeds only when the asset reaches disk; a failed save leaves the value in memory only (saved false, save_error). Read: get_metasound_node_property; GUIDs: list_metasound_nodes.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_metasound_nodes returns it in id; a value that is not a GUID is refused.
//   @param property - FProperty name on the Configuration struct.
//   @param value [any!] - New value for the field as JSON, converted by FJsonObjectConverter: a number, bool, string, array or object (for a struct field); numbers and booleans can also be passed as strings ("0.5", "true").
//   @returns { metasound_path, node_id, property, set, value_changed, new_value?, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetMetaSoundNodeProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath, NodeId, PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropName)) { return Missing; }
	const TSharedPtr<FJsonValue>* ValuePtr = Params->Values.Find(TEXT("value"));
	if (!ValuePtr || !ValuePtr->IsValid())
	{
		return mcp_common::MakeErrorReply(TEXT("value is required"));
	}
	UMetaSoundSource* Source = nullptr;
	TSharedPtr<FJsonObject> Error;
	FMetasoundFrontendNode* FoundNode = const_cast<FMetasoundFrontendNode*>(
		MetaSound_FindGraphNode(MetaSoundPath, NodeId, Source, Error));
	if (!FoundNode)
	{
		return Error;
	}
	const UScriptStruct* CfgStruct = nullptr;
	const FProperty* Prop = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MetaSound_FindConfigProperty(*FoundNode, PropName, CfgStruct, Prop))
	{
		return NotFound;
	}

	// The property's current value, via a JSON dump of the config struct (null when the dump fails).
	auto ReadPropertyValue = [&]() -> TSharedPtr<FJsonValue>
	{
		TSharedRef<FJsonObject> Dump = MakeShared<FJsonObject>();
		if (FJsonObjectConverter::UStructToJsonObject(CfgStruct,
			FoundNode->Configuration.GetMutableMemory(), Dump, 0, 0))
		{
			return Dump->TryGetField(PropName);
		}
		return nullptr;
	};

	// Capture old value via JSON round-trip on the config struct.
	const TSharedPtr<FJsonValue> OldValue = ReadPropertyValue();

	// Apply via dump+JsonObjectToUStruct (JsonValueToFProperty is not exported in 5.8).
	{
		TSharedRef<FJsonObject> Box = MakeShared<FJsonObject>();
		if (!FJsonObjectConverter::UStructToJsonObject(CfgStruct,
			FoundNode->Configuration.GetMutableMemory(), Box, 0, 0))
		{
			return mcp_common::MakeErrorReply(
				TEXT("Failed to dump Configuration struct before applying value"));
		}
		Box->SetField(PropName, *ValuePtr);
		if (!FJsonObjectConverter::JsonObjectToUStruct(Box, CfgStruct,
			FoundNode->Configuration.GetMutableMemory(), 0, 0))
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("Failed to apply JSON value to property '%s' (type %s)"),
					*PropName, *Prop->GetCPPType()));
		}
	}

	// Read back the new value.
	const TSharedPtr<FJsonValue> NewValue = ReadPropertyValue();
	// Cheap equality via JSON-serialized strings.
	auto JsonToStr = [](const TSharedPtr<FJsonValue>& V) -> FString
	{
		if (!V.IsValid()) { return FString(); }
		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(V.ToSharedRef(), FString(), Writer);
		return Out;
	};
	const bool bValueChanged = JsonToStr(OldValue) != JsonToStr(NewValue);

	// Persist: a real "commit" requires the builder subsystem. We mark dirty +
	// save the package so the in-memory mutation sticks; the next build pass will
	// pick it up. (UE 5.8 has no public "compile" API.)
	Source->MarkPackageDirty();
	FString SaveError;
	const bool bSaved = mcp_common::SaveAssetPackage(Source->GetPackage(), Source, SaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"), MetaSoundPath);
	Data->SetStringField(TEXT("node_id"),        NodeId);
	Data->SetStringField(TEXT("property"),      PropName);
	Data->SetBoolField  (TEXT("set"),           true);
	Data->SetBoolField  (TEXT("value_changed"), bValueChanged);
	if (NewValue.IsValid()) { Data->SetField(TEXT("new_value"), NewValue); }
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_metasound_node_property"), SaveError);
}

// MCP:get_metasound_node_property - Read one field of the Configuration struct of a MetaSound Source node, found by its node GUID (from list_metasound_nodes), returning its C++ type, has_value and value as JSON. Use it because MetaSound nodes are document structs, not UObjects, so get_properties cannot reach them; input defaults are set_metasound_node_input_default. Fails when the node has no Configuration struct or no such property; Patches are refused. Read-only; write with set_metasound_node_property.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_metasound_nodes returns it in id; a value that is not a GUID is refused.
//   @param property - FProperty name on the Configuration struct.
//   @returns { metasound_path, node_id, property, type, has_value, value? }
TSharedPtr<FJsonObject> HandleGetMetaSoundNodeProperty(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath, NodeId, PropName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property"), PropName)) { return Missing; }
	UMetaSoundSource* Source = nullptr;
	TSharedPtr<FJsonObject> Error;
	const FMetasoundFrontendNode* FoundNode = MetaSound_FindGraphNode(MetaSoundPath, NodeId, Source, Error);
	if (!FoundNode)
	{
		return Error;
	}
	const UScriptStruct* CfgStruct = nullptr;
	const FProperty* Prop = nullptr;
	if (TSharedPtr<FJsonObject> NotFound = MetaSound_FindConfigProperty(*FoundNode, PropName, CfgStruct, Prop))
	{
		return NotFound;
	}

	// Dump the full Configuration struct and pull out the requested field.
	TSharedRef<FJsonObject> Dump = MakeShared<FJsonObject>();
	FJsonObjectConverter::UStructToJsonObject(CfgStruct,
		const_cast<FMetasoundFrontendNode*>(FoundNode)->Configuration.GetMutableMemory(), Dump, 0, 0);
	const TSharedPtr<FJsonValue> Value = Dump->TryGetField(PropName);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"), MetaSoundPath);
	Data->SetStringField(TEXT("node_id"),        NodeId);
	Data->SetStringField(TEXT("property"),      PropName);
	Data->SetStringField(TEXT("type"),          Prop->GetCPPType());
	Data->SetBoolField  (TEXT("has_value"),     Value.IsValid());
	if (Value.IsValid()) { Data->SetField(TEXT("value"), Value); }
	return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Node-to-node wiring: connect_metasound_nodes, connect_metasound_graph_input_to_node, disconnect_metasound_nodes
// and their reader list_metasound_connections. The graph-output tools above only reach the graph's outputs; a real
// patch (Sine -> ADSR -> Out) needs node outputs wired to node inputs. A MetaSound input takes ONE edge, so an
// input is the address of a connection: disconnecting names the input and drops whatever feeds it.
// ===========================================================================
namespace
{
    // The input pin InputName of node NodeId in Builder's graph; the error reply when the id is not a GUID or the
    // node has no such input (the reply lists the inputs it has).
    TSharedPtr<FJsonObject> MetaSound_FindNodeInput(UMetaSoundBuilderBase* Builder, const FString& NodeId, const FString& InputName,
        FMetaSoundBuilderNodeInputHandle& OutHandle)
    {
        FMetaSoundNodeHandle NodeHandle;
        if (!ParseMetaSoundNodeHandle(NodeId, NodeHandle))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Invalid to_node_id (expected a node GUID from add_metasound_node or list_metasound_nodes): %s; nothing was changed"), *NodeId));
        }
        if (!Builder->ContainsNode(NodeHandle))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No node %s in the graph (list_metasound_nodes lists them); nothing was changed"), *NodeId));
        }
        EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
        OutHandle = Builder->FindNodeInputByName(NodeHandle, FName(*InputName), Result);
        if (Result != EMetaSoundBuilderResult::Succeeded || !OutHandle.IsSet())
        {
            TArray<FString> Names;
            for (const TSharedPtr<FJsonValue>& Input : EnumerateNodeInputs(Builder, NodeHandle))
            {
                Names.Add(Input->AsObject()->GetStringField(TEXT("name")));
            }
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Node %s has no input '%s' (its inputs: %s); nothing was changed"), *NodeId, *InputName, *FString::Join(Names, TEXT(", "))));
        }
        return nullptr;
    }

    // Wires From to To with ConnectNodes and re-reads the edge. Shared by the two connect tools: an edge already
    // there changes and saves nothing; an input fed from elsewhere is re-fed (replaced=true), since an input takes
    // one edge.
    TSharedPtr<FJsonObject> MetaSound_ConnectHandles(UMetaSoundSource* Source, UMetaSoundBuilderBase* Builder,
        const FMetaSoundBuilderNodeOutputHandle& From, const FMetaSoundBuilderNodeInputHandle& To,
        const TSharedPtr<FJsonObject>& Data, const TCHAR* Command)
    {
        FName FromName, FromType, ToName, ToType;
        EMetaSoundBuilderResult DataResult = EMetaSoundBuilderResult::Failed;
        Builder->GetNodeOutputData(From, FromName, FromType, DataResult);
        Builder->GetNodeInputData(To, ToName, ToType, DataResult);
        Data->SetStringField(TEXT("from_data_type"), FromType.ToString());
        Data->SetStringField(TEXT("to_data_type"), ToType.ToString());
        if (Builder->NodesAreConnected(From, To))
        {
            Data->SetBoolField(TEXT("connected"), true);
            Data->SetBoolField(TEXT("already_connected"), true);
            Data->SetBoolField(TEXT("replaced"), false);
            Data->SetBoolField(TEXT("saved"), false);
            Data->SetStringField(TEXT("note"), TEXT("the edge is already there; nothing changed and nothing was saved"));
            return mcp_common::MakeOkReply(Data);
        }
        if (FromType != ToType)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Cannot connect a %s output to a %s input; nothing was changed"), *FromType.ToString(), *ToType.ToString()));
        }
        const bool bWasFed = Builder->NodeInputIsConnected(To);

        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundConnectNodes", "MetaSound Connect Nodes"));
        Source->Modify();
        if (bWasFed)
        {
            EMetaSoundBuilderResult DisconnectResult = EMetaSoundBuilderResult::Failed;
            Builder->DisconnectNodeInput(To, DisconnectResult);
        }
        EMetaSoundBuilderResult ConnectResult = EMetaSoundBuilderResult::Failed;
        Builder->ConnectNodes(From, To, ConnectResult);
        const bool bConnected = Builder->NodesAreConnected(From, To);
        if (ConnectResult != EMetaSoundBuilderResult::Succeeded || !bConnected)
        {
            Transaction.Cancel();
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("ConnectNodes did not make the edge (result %s)%s"),
                ConnectResult == EMetaSoundBuilderResult::Succeeded ? TEXT("Succeeded") : TEXT("Failed"),
                bWasFed ? TEXT("; the input's previous edge was dropped, the asset is not saved") : TEXT("; nothing was changed")));
        }

        FString SaveErr;
        const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);
        Data->SetBoolField(TEXT("connected"), true);
        Data->SetBoolField(TEXT("already_connected"), false);
        Data->SetBoolField(TEXT("replaced"), bWasFed);
        return mcp_common::MakeSavedReply(Data, bSaved, Command, SaveErr);
    }
}

// MCP:connect_metasound_nodes - Wire the output from_output_name of node from_node_id to the input to_input_name of node to_node_id in a MetaSound Source graph, in one undo step, then save the asset -- the node-to-node edge a patch needs (Sine.Audio -> ADSR.In). Node ids come from add_metasound_node or list_metasound_nodes, pin names from their inputs/outputs. The two pins must carry the same data type. An input takes one edge, so an input already fed from elsewhere is re-fed (replaced=true); the same edge already there changes and saves nothing (already_connected=true). A bad id, an unknown pin or a type mismatch is an error and nothing changes. Read back: list_metasound_connections; inverse: disconnect_metasound_nodes. A graph input as the source: connect_metasound_graph_input_to_node; a graph output as the target: connect_metasound_node_to_graph_output.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param from_node_id - GUID of the node whose output feeds the edge.
//   @param from_output_name - Name of that node's output pin (e.g. "Audio").
//   @param to_node_id - GUID of the node whose input receives the edge.
//   @param to_input_name - Name of that node's input pin (e.g. "In").
//   @returns { asset_path: string, from_node_id: string, from_output_name: string, to_node_id: string, to_input_name: string, from_data_type: string, to_data_type: string, connected: bool, already_connected: bool, replaced: bool, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleConnectMetaSoundNodeToNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, FromNodeId, FromOutputName, ToNodeId, ToInputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_node_id"), FromNodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_output_name"), FromOutputName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_node_id"), ToNodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_input_name"), ToInputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    FMetaSoundNodeHandle FromNode;
    if (!ParseMetaSoundNodeHandle(FromNodeId, FromNode) || !Builder->ContainsNode(FromNode))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No node %s in the graph (from_node_id must be a node GUID from list_metasound_nodes); nothing was changed"), *FromNodeId));
    }
    EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
    const FMetaSoundBuilderNodeOutputHandle From = Builder->FindNodeOutputByName(FromNode, FName(*FromOutputName), FindResult);
    if (FindResult != EMetaSoundBuilderResult::Succeeded || !From.IsSet())
    {
        TArray<FString> Names;
        for (const TSharedPtr<FJsonValue>& Output : EnumerateNodeOutputs(Builder, FromNode))
        {
            Names.Add(Output->AsObject()->GetStringField(TEXT("name")));
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Node %s has no output '%s' (its outputs: %s); nothing was changed"), *FromNodeId, *FromOutputName, *FString::Join(Names, TEXT(", "))));
    }
    FMetaSoundBuilderNodeInputHandle To;
    if (TSharedPtr<FJsonObject> Failure = MetaSound_FindNodeInput(Builder, ToNodeId, ToInputName, To))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("from_node_id"), FromNodeId);
    Data->SetStringField(TEXT("from_output_name"), FromOutputName);
    Data->SetStringField(TEXT("to_node_id"), ToNodeId);
    Data->SetStringField(TEXT("to_input_name"), ToInputName);
    return MetaSound_ConnectHandles(Source, Builder, From, To, Data, TEXT("connect_metasound_nodes"));
}

// MCP:connect_metasound_graph_input_to_node - Wire the graph input graph_input_name of a MetaSound Source to the input to_input_name of node to_node_id, in one undo step, then save the asset -- how a parameter (a Float input "Frequency") drives a node pin. Same rules as connect_metasound_nodes: matching data types, an input already fed is re-fed (replaced=true), the same edge already there changes nothing (already_connected=true), and an unknown graph input, node or pin is an error with nothing changed. Read back: list_metasound_connections; inverse: disconnect_metasound_nodes on the node input. Graph input to graph output: connect_metasound_graph_input_to_output.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param graph_input_name - Name of the graph input (get_metasound_info lists them).
//   @param to_node_id - GUID of the node whose input receives the edge.
//   @param to_input_name - Name of that node's input pin.
//   @returns { asset_path: string, graph_input_name: string, to_node_id: string, to_input_name: string, from_data_type: string, to_data_type: string, connected: bool, already_connected: bool, replaced: bool, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleConnectMetaSoundGraphInputToNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, GraphInputName, ToNodeId, ToInputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("graph_input_name"), GraphInputName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_node_id"), ToNodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_input_name"), ToInputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    FName InputType;
    FMetaSoundBuilderNodeOutputHandle From;
    EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
    Builder->FindGraphInputNode(FName(*GraphInputName), InputType, From, FindResult);
    if (FindResult != EMetaSoundBuilderResult::Succeeded || !From.IsSet())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No graph input '%s' (get_metasound_info lists the inputs); nothing was changed"), *GraphInputName));
    }
    FMetaSoundBuilderNodeInputHandle To;
    if (TSharedPtr<FJsonObject> Failure = MetaSound_FindNodeInput(Builder, ToNodeId, ToInputName, To))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("graph_input_name"), GraphInputName);
    Data->SetStringField(TEXT("to_node_id"), ToNodeId);
    Data->SetStringField(TEXT("to_input_name"), ToInputName);
    return MetaSound_ConnectHandles(Source, Builder, From, To, Data, TEXT("connect_metasound_graph_input_to_node"));
}

// MCP:disconnect_metasound_nodes - Drop the edge that feeds the input to_input_name of node to_node_id in a MetaSound Source graph, whatever drives it -- another node's output or a graph input -- in one undo step, then save the asset. Inverse of connect_metasound_nodes and connect_metasound_graph_input_to_node: an input takes one edge, so the input names it. The reply says which output fed it (from_node_id, from_output_name) and re-reads the input. An input with no edge is an honest no-op (disconnected=false, already_disconnected=true, nothing saved); a bad node id or an unknown pin is an error. Read first: list_metasound_connections; a graph output's edge: disconnect_metasound_node_from_graph_output.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param to_node_id - GUID of the node whose input loses its edge.
//   @param to_input_name - Name of that node's input pin.
//   @returns { asset_path: string, to_node_id: string, to_input_name: string, from_node_id: string?, from_node_name: string?, from_output_name: string?, disconnected: bool, already_disconnected: bool, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleDisconnectMetaSoundNodeInput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, ToNodeId, ToInputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_node_id"), ToNodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_input_name"), ToInputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }
    FMetaSoundBuilderNodeInputHandle To;
    if (TSharedPtr<FJsonObject> Failure = MetaSound_FindNodeInput(Builder, ToNodeId, ToInputName, To))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("to_node_id"), ToNodeId);
    Data->SetStringField(TEXT("to_input_name"), ToInputName);
    if (!Builder->NodeInputIsConnected(To))
    {
        Data->SetBoolField(TEXT("disconnected"), false);
        Data->SetBoolField(TEXT("already_disconnected"), true);
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }

    // The output that feeds the input, read from the document's edges before they change.
    const FMetasoundFrontendGraph& Graph = Source->GetConstDocument().RootGraph.GetConstDefaultGraph();
    for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
    {
        if (Edge.ToNodeID == To.NodeID && Edge.ToVertexID == To.VertexID)
        {
            Data->SetStringField(TEXT("from_node_id"), Edge.FromNodeID.ToString());
            for (const FMetasoundFrontendNode& Node : Graph.Nodes)
            {
                if (Node.GetID() != Edge.FromNodeID)
                {
                    continue;
                }
                Data->SetStringField(TEXT("from_node_name"), Node.Name.ToString());
                for (const FMetasoundFrontendVertex& Output : Node.Interface.Outputs)
                {
                    if (Output.VertexID == Edge.FromVertexID)
                    {
                        Data->SetStringField(TEXT("from_output_name"), Output.Name.ToString());
                    }
                }
            }
            break;
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundDisconnectNodeInput", "MetaSound Disconnect Node Input"));
    Source->Modify();
    EMetaSoundBuilderResult DisconnectResult = EMetaSoundBuilderResult::Failed;
    Builder->DisconnectNodeInput(To, DisconnectResult);
    if (DisconnectResult != EMetaSoundBuilderResult::Succeeded || Builder->NodeInputIsConnected(To))
    {
        Transaction.Cancel();
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DisconnectNodeInput left input '%s' of node %s connected; nothing was saved"), *ToInputName, *ToNodeId));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);
    Data->SetBoolField(TEXT("disconnected"), true);
    Data->SetBoolField(TEXT("already_disconnected"), false);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("disconnect_metasound_nodes"), SaveErr);
}

// MCP:list_metasound_connections - List every edge of the default graph of a MetaSound Source: from which node and output pin to which node and input pin, with each node's name and class (a graph input or output node is named after the input or output), plus count. Read-only. It reads what connect_metasound_nodes, connect_metasound_graph_input_to_node, connect_metasound_graph_input_to_output and connect_metasound_node_to_graph_output write; node ids are those of list_metasound_nodes, and an input's edge is dropped with disconnect_metasound_nodes. Patches are refused.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain".
//   @returns { metasound_path: string, count: int, connections: [{ from_node_id, from_node_name, from_node_class, from_output_name, to_node_id, to_node_name, to_node_class, to_input_name, data_type }] }

TSharedPtr<FJsonObject> HandleListMetaSoundConnections(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
    {
        return MetaSound_CannotLoadSourceReply(MetaSoundPath);
    }
    const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();
    const FMetasoundFrontendGraph& Graph = Doc.RootGraph.GetConstDefaultGraph();

    TMap<FGuid, FString> ClassIdToName;
    for (const FMetasoundFrontendClass& Dep : Doc.Dependencies)
    {
        ClassIdToName.Add(Dep.ID, Dep.Metadata.GetClassName().ToString());
    }
    TMap<FGuid, const FMetasoundFrontendNode*> NodesById;
    for (const FMetasoundFrontendNode& Node : Graph.Nodes)
    {
        NodesById.Add(Node.GetID(), &Node);
    }
    const auto VertexName = [](const TArray<FMetasoundFrontendVertex>& Vertices, const FGuid& VertexId, FString& OutType) -> FString
    {
        for (const FMetasoundFrontendVertex& Vertex : Vertices)
        {
            if (Vertex.VertexID == VertexId)
            {
                OutType = Vertex.TypeName.ToString();
                return Vertex.Name.ToString();
            }
        }
        return FString();
    };

    TArray<TSharedPtr<FJsonValue>> EdgesJson;
    for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
    {
        const FMetasoundFrontendNode* const* FromNode = NodesById.Find(Edge.FromNodeID);
        const FMetasoundFrontendNode* const* ToNode = NodesById.Find(Edge.ToNodeID);
        FString DataType, ToType;
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("from_node_id"), Edge.FromNodeID.ToString());
        Entry->SetStringField(TEXT("from_node_name"), FromNode ? (*FromNode)->Name.ToString() : FString());
        const FString* FromClass = FromNode ? ClassIdToName.Find((*FromNode)->ClassID) : nullptr;
        Entry->SetStringField(TEXT("from_node_class"), FromClass ? *FromClass : FString());
        Entry->SetStringField(TEXT("from_output_name"), FromNode ? VertexName((*FromNode)->Interface.Outputs, Edge.FromVertexID, DataType) : FString());
        Entry->SetStringField(TEXT("to_node_id"), Edge.ToNodeID.ToString());
        Entry->SetStringField(TEXT("to_node_name"), ToNode ? (*ToNode)->Name.ToString() : FString());
        const FString* ToClass = ToNode ? ClassIdToName.Find((*ToNode)->ClassID) : nullptr;
        Entry->SetStringField(TEXT("to_node_class"), ToClass ? *ToClass : FString());
        Entry->SetStringField(TEXT("to_input_name"), ToNode ? VertexName((*ToNode)->Interface.Inputs, Edge.ToVertexID, ToType) : FString());
        Entry->SetStringField(TEXT("data_type"), DataType.IsEmpty() ? ToType : DataType);
        EdgesJson.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("metasound_path"), MetaSoundPath);
    Data->SetNumberField(TEXT("count"), EdgesJson.Num());
    Data->SetArrayField(TEXT("connections"), EdgesJson);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_metasound_interface: the inverse of add_metasound_interface. RemoveInterface takes the interface's input
// and output members (and their nodes) out of the graph, as the MetaSound editor's interface list does.
// ---------------------------------------------------------------------------
// MCP:remove_metasound_interface - Remove an interface from a MetaSound Source graph, with the graph inputs and outputs (and their nodes) it declared, in one undo step, then save the asset. Inverse of add_metasound_interface. The reply re-reads the document's interfaces (interface_count, still_declared). An interface the graph does not declare is an honest no-op (removed=false, nothing saved); a source that does not load is an error. Removing the source interface a one-shot MetaSound Source needs (UE.Source...) leaves a source the engine may refuse to play; get_metasound_info lists what is declared.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param interface_name - Name of the declared interface to remove, as add_metasound_interface took it.
//   @returns { asset_path: string, interface_name: string, removed: bool, still_declared: bool, interface_count: int, saved: bool, save_error: string?, note: string? }

TSharedPtr<FJsonObject> HandleMetaSoundRemoveInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath, InterfaceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("interface_name"), InterfaceName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("interface_name"), InterfaceName);
    const FName InterfaceFName(*InterfaceName);
    if (!Builder->InterfaceIsDeclared(InterfaceFName))
    {
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetBoolField(TEXT("still_declared"), false);
        Data->SetNumberField(TEXT("interface_count"), Source->GetConstDocument().Interfaces.Num());
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("the graph does not declare that interface; nothing changed"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundRemoveInterface", "MetaSound Remove Interface"));
    Source->Modify();
    EMetaSoundBuilderResult RemoveResult = EMetaSoundBuilderResult::Failed;
    Builder->RemoveInterface(InterfaceFName, RemoveResult);
    const bool bStillDeclared = Builder->InterfaceIsDeclared(InterfaceFName);
    if (RemoveResult != EMetaSoundBuilderResult::Succeeded || bStillDeclared)
    {
        Transaction.Cancel();
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("RemoveInterface failed for '%s' (the graph still declares it); nothing was saved"), *InterfaceName));
    }

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);
    Data->SetBoolField(TEXT("removed"), true);
    Data->SetBoolField(TEXT("still_declared"), false);
    Data->SetNumberField(TEXT("interface_count"), Source->GetConstDocument().Interfaces.Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_metasound_interface"), SaveErr);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("connect_metasound_nodes"), &HandleConnectMetaSoundNodeToNode },
    { TEXT("connect_metasound_graph_input_to_node"), &HandleConnectMetaSoundGraphInputToNode },
    { TEXT("disconnect_metasound_nodes"), &HandleDisconnectMetaSoundNodeInput },
    { TEXT("list_metasound_connections"), &HandleListMetaSoundConnections },
    { TEXT("remove_metasound_interface"), &HandleMetaSoundRemoveInterface },
    { TEXT("add_metasound_graph_input"), &HandleAddMetaSoundGraphInput },
    { TEXT("add_metasound_graph_output"), &HandleAddMetaSoundGraphOutput },
    { TEXT("connect_metasound_graph_input_to_output"), &HandleConnectMetaSoundNodes },
    { TEXT("create_metasound_source"), &HandleCreateMetaSoundSource },
    { TEXT("disconnect_metasound_node_from_graph_output"), &HandleMetaSoundDisconnectNodeFromGraphOutput },
    { TEXT("disconnect_metasound_graph_input_from_output"), &HandleDisconnectMetaSoundNodes },
    { TEXT("get_metasound_node_info"), &HandleGetMetaSoundNodeInfo },
    { TEXT("get_metasound_node_property"), &HandleGetMetaSoundNodeProperty },
    { TEXT("get_metasound_info"), &HandleGetMetaSoundInfo },
    { TEXT("list_metasound_node_types"), &HandleListMetaSoundNodeTypes },
    { TEXT("list_metasound_nodes"), &HandleListMetaSoundNodes },
    { TEXT("add_metasound_interface"), &HandleMetaSoundAddInterface },
    { TEXT("add_metasound_node"), &HandleMetaSoundAddNodeByClassName },
    { TEXT("connect_metasound_node_to_graph_output"), &HandleMetaSoundConnectNodeToGraphOutput },
    { TEXT("set_metasound_graph_input_default"), &HandleMetaSoundSetInputDefault },
    { TEXT("set_metasound_node_input_default"), &HandleMetaSoundSetNodeInputDefault },
    { TEXT("remove_metasound_graph_input"), &HandleRemoveMetaSoundGraphInput },
    { TEXT("remove_metasound_graph_output"), &HandleRemoveMetaSoundGraphOutput },
    { TEXT("remove_metasound_node"), &HandleRemoveMetaSoundNode },
    { TEXT("set_metasound_node_property"), &HandleSetMetaSoundNodeProperty },
};
} // namespace UEAgentMetaSoundCommands
