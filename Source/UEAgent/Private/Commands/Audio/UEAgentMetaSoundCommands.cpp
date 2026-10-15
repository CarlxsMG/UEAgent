#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// MetaSound (MetasoundEngine / MetasoundFrontend runtime modules)
#include "MetasoundBuilderSubsystem.h"                          // UMetaSoundBuilderSubsystem / UMetaSoundSourceBuilder
#include "MetasoundBuilderBase.h"                               // UMetaSoundBuilderBase / EMetaSoundBuilderResult / handles
#include "MetasoundSource.h"                                    // UMetaSoundSource
#include "MetasoundDocumentInterface.h"                         // IMetaSoundDocumentInterface
#include "MetasoundFrontendDocument.h"                          // FMetasoundFrontendDocument / class metadata + interface
#include "MetasoundFrontendLiteral.h"                           // FMetasoundFrontendLiteral
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
// Reflection-driven read/list/delete (AssetRegistry + ISearchEngine +
// EditorAssetLibrary + reflection on TInstancedStruct<FMetaSoundFrontendNodeConfiguration>).
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "EditorAssetLibrary.h"
#include "MetasoundFrontendQuery.h"                             // FMetaSoundClassInfo (must precede SearchEngine)
#include "MetasoundFrontendSearchEngine.h"                      // Metasound::Frontend::ISearchEngine
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

    // Map a string ("mono"/"stereo"/"quad"/"5.1"/"7.1") to EMetaSoundOutputAudioFormat.
    EMetaSoundOutputAudioFormat ParseOutputFormat(const FString& InFormat)
    {
        const FString F = InFormat.ToLower();
        if (F == TEXT("stereo")) { return EMetaSoundOutputAudioFormat::Stereo; }
        if (F == TEXT("quad")) { return EMetaSoundOutputAudioFormat::Quad; }
        if (F == TEXT("5.1") || F == TEXT("fivedotone")) { return EMetaSoundOutputAudioFormat::FiveDotOne; }
        if (F == TEXT("7.1") || F == TEXT("sevendotone")) { return EMetaSoundOutputAudioFormat::SevenDotOne; }
        return EMetaSoundOutputAudioFormat::Mono;
    }

    // Build an FMetasoundFrontendLiteral from the request's value field (one of
    // bool_value / int_value / string_value / value-float). Shared by the graph-input
    // and node-input default setters.
    void BuildLiteralFromParams(const TSharedPtr<FJsonObject>& Params, FMetasoundFrontendLiteral& Literal, FString& ValueType)
    {
        if (Params->HasField(TEXT("bool_value")))
        {
            bool B = false; Params->TryGetBoolField(TEXT("bool_value"), B);
            Literal.Set(B); ValueType = TEXT("Bool");
        }
        else if (Params->HasField(TEXT("int_value")))
        {
            int32 I = 0; Params->TryGetNumberField(TEXT("int_value"), I);
            Literal.Set(I); ValueType = TEXT("Int32");
        }
        else if (Params->HasField(TEXT("string_value")))
        {
            FString S; Params->TryGetStringField(TEXT("string_value"), S);
            Literal.Set(S); ValueType = TEXT("String");
        }
        else
        {
            double V = 0.0; Params->TryGetNumberField(TEXT("value"), V);
            Literal.Set(static_cast<float>(V)); ValueType = TEXT("Float");
        }
    }

    // Parse a node_id (FGuid string from metasound_add_node_by_classname) into a node handle.
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

    // The edge connect_metasound_nodes and disconnect_metasound_nodes act on: a graph input node's
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
// MCP:create_metasound_source - Creates (or loads, idempotently) a UMetaSoundSource asset and saves it.  Uses UMetaSoundBuilderSubsystem::CreateSourceBuilder to build a source graph (pre-populated with the source interface: OnPlay input + audio output +, for one-shots, OnFinished output), then UMetaSoundEditorSubsystem::BuildToAsset to persist it as a real /Game asset (headless-safe; no modal picker).
//   @param name - Name of the MetaSound Source asset to create.
//   @param package_path - Target /Game package path (optional, default "/Game/MetaSounds")
//   @param output_format - Output audio format: "mono", "stereo", "quad", "5.1", or "7.1" (optional, default "mono")
//   @returns { source_name: string, asset_path: string, created: bool, already_exists: bool, saved: bool, output_format: string, class_name: string, input_count: number, output_count: number, save_error: string? }

TSharedPtr<FJsonObject> HandleCreateMetaSoundSource(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/MetaSounds");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString OutputFormatStr = TEXT("mono");
    Params->TryGetStringField(TEXT("output_format"), OutputFormatStr);
    const EMetaSoundOutputAudioFormat OutputFormat = ParseOutputFormat(OutputFormatStr);

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
        // Already on disk => persisted. Emit the same keys as the create path so
        // the idempotent re-run shape matches (was missing 'saved'/'output_format').
        Data->SetBoolField(TEXT("saved"), true);
        Data->SetStringField(TEXT("output_format"), OutputFormatStr.ToLower());
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
    Data->SetStringField(TEXT("output_format"), OutputFormatStr.ToLower());
    Data->SetStringField(TEXT("class_name"), Doc.RootGraph.Metadata.GetClassName().ToString());
    Data->SetNumberField(TEXT("input_count"), Doc.RootGraph.GetDefaultInterface().Inputs.Num());
    Data->SetNumberField(TEXT("output_count"), Doc.RootGraph.GetDefaultInterface().Outputs.Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_metasound_source"), SaveError);
}

// ---------------------------------------------------------------------------
// metasound_add_interface
// Obtains a builder for an EXISTING saved asset via the editor subsystem's
// FindOrBeginBuilding (the reliable headless path), then adds a registered
// interface (validated first via IsInterfaceRegistered) to the graph, which
// adds the interface's input/output nodes. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:metasound_add_interface - Adds a registered interface to an existing MetaSound Source's graph.  Obtains a builder for the saved asset via the editor subsystem's FindOrBeginBuilding (the reliable headless path), validates the interface via IsInterfaceRegistered, then calls UMetaSoundBuilderBase::AddInterface (which adds the interface's input/output nodes). The asset is saved after the edit.
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("metasound_add_interface"));
}

// ---------------------------------------------------------------------------
// metasound_set_input_default
// Obtains a builder for an existing asset (FindOrBeginBuilding) and sets the
// named graph input's default literal via SetGraphInputDefault, then reads it
// back to verify and saves. Supports float / int / bool / string values.
// ---------------------------------------------------------------------------
// MCP:metasound_set_input_default - Sets the default literal value of a MetaSound Source graph input.  Obtains a builder for the existing asset (FindOrBeginBuilding) and calls SetGraphInputDefault with an FMetasoundFrontendLiteral built from the supplied value, then reads the value back to verify and saves the asset. Provide exactly one of ``value`` (float, default), ``int_value``, ``bool_value`` or ``string_value``.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param input_name - Name of the graph input whose default to set.
//   @param value - Float default value (optional, used if no other *_value is provided)
//   @param int_value - Int32 default value (optional, mutually exclusive with value/bool_value/string_value)
//   @param bool_value - Bool default value (optional, mutually exclusive with value/int_value/string_value)
//   @param string_value - String default value (optional, mutually exclusive with value/int_value/bool_value)
//   @returns { asset_path: string, input_name: string, value_type: string, value_set: bool, verified: bool, value_read_back: string, saved: bool }

TSharedPtr<FJsonObject> HandleMetaSoundSetInputDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    FString InputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName)) { return Missing; }

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error, /*bNameSourceIfNoSubsystem=*/false);
    if (!Builder)
    {
        return Error;
    }

    // Build the literal from the supplied value (float default).
    FMetasoundFrontendLiteral Literal;
    FString ValueType;
    BuildLiteralFromParams(Params, Literal, ValueType);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MetaSoundSetInputDefault", "MetaSound Set Input Default"));
    Source->Modify();

    EMetaSoundBuilderResult SetResult = EMetaSoundBuilderResult::Failed;
    Builder->SetGraphInputDefault(FName(*InputName), Literal, SetResult);
    if (SetResult != EMetaSoundBuilderResult::Succeeded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("SetGraphInputDefault failed for input '%s' (input may not exist)"), *InputName));
    }

    // Read the value back to verify.
    EMetaSoundBuilderResult GetResult = EMetaSoundBuilderResult::Failed;
    const FMetasoundFrontendLiteral ReadBack = Builder->GetGraphInputDefault(FName(*InputName), GetResult);

    FString SaveErr;
    const bool bSaved = MetaSound_MarkDirtyAndSave(Source, SaveErr);

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("input_name"), InputName);
    Data->SetStringField(TEXT("value_type"), ValueType);
    Data->SetBoolField(TEXT("value_set"), true);
    Data->SetBoolField(TEXT("verified"), GetResult == EMetaSoundBuilderResult::Succeeded);
    Data->SetStringField(TEXT("value_read_back"), ReadBack.ToString());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("metasound_set_input_default"));
}

// ---------------------------------------------------------------------------
// get_metasound_info
// Loads the UMetaSoundSource and reports its UClass, root graph class name and the
// document's input/output counts (read from the root graph's default interface).
// ---------------------------------------------------------------------------
// MCP:get_metasound_info - Read the top-level metadata of a MetaSound Source asset, loading it if needed: its class, the root graph class name, the graph inputs and outputs (name and type) with their counts, and interface_count, the number of interfaces the document declares. Read-only; a path that does not resolve to a UMetaSoundSource is an error. For node and connection counts use get_metasound_graph_info; list_meta_sound_nodes lists the nodes and list_meta_sounds finds sources.
//   @param metasound_path - Asset path of the MetaSound Source, as a package path like "/Game/MetaSounds/MS_Example" or an object path like "/Game/MetaSounds/MS_Example.MS_Example"
//   @returns { source_name: string, asset_path: string, class_name: string, graph_class_name: string, input_count: number, output_count: number, interface_count: number, inputs: [{ name: string, type: string }], outputs: [{ name: string, type: string }] }

TSharedPtr<FJsonObject> HandleGetMetaSoundInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
    {
        return MetaSound_SourceNotFoundReply(MetaSoundPath);
    }

    const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();
    const FMetasoundFrontendClassInterface& Interface = Doc.RootGraph.GetDefaultInterface();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("source_name"), Source->GetName());
    Data->SetStringField(TEXT("asset_path"), Source->GetPathName());
    Data->SetStringField(TEXT("class_name"), Source->GetClass()->GetName());
    Data->SetStringField(TEXT("graph_class_name"), Doc.RootGraph.Metadata.GetClassName().ToString());
    Data->SetNumberField(TEXT("input_count"), Interface.Inputs.Num());
    Data->SetNumberField(TEXT("output_count"), Interface.Outputs.Num());
    Data->SetNumberField(TEXT("interface_count"), Doc.Interfaces.Num());
    Data->SetArrayField(TEXT("inputs"), MetaSound_NameTypeArray(Interface.Inputs));
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
// MCP:add_metasound_graph_input - Adds a graph INPUT node to an existing MetaSound Source and saves it.  Obtains a builder for the saved asset via FindOrBeginBuilding (the reliable headless path) and calls UMetaSoundBuilderBase::AddGraphInputNode, adding a real graph input node (and its backing graph input member) of the given data type. Optionally bakes a typed default into the input's literal — provide at most one of ``default_value`` (float), ``int_value``, ``bool_value`` or ``string_value``.          "/Game/MetaSounds/MS_Example" or fully-qualified ".MS_Example").         "Bool", "String", "Audio", "Trigger").
//   @param metasound_path - Asset path of the MetaSound Source (e.g.
//   @param input_name - Name of the graph input node to add.
//   @param data_type - Frontend data type: "Float", "Int32", "Bool", "String", "Audio", "Trigger" (optional, default "Float")
//   @param default_value - Float default baked into the literal (optional)
//   @param int_value - Int32 default baked into the literal (optional, mutually exclusive with other defaults)
//   @param bool_value - Bool default baked into the literal (optional, mutually exclusive with other defaults)
//   @param string_value - String default baked into the literal (optional, mutually exclusive with other defaults)
//   @returns { asset_path: string, input_name: string, data_type: string, added: bool, has_default: bool, saved: bool, save_error: string? }

TSharedPtr<FJsonObject> HandleAddMetaSoundGraphInput(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    FString InputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName)) { return Missing; }

    FString DataType = TEXT("Float");
    Params->TryGetStringField(TEXT("data_type"), DataType);

    UMetaSoundSource* Source = nullptr;
    TSharedPtr<FJsonObject> Error;
    UMetaSoundBuilderBase* Builder = MetaSound_OpenSourceBuilder(MetaSoundPath, Source, Error);
    if (!Builder)
    {
        return Error;
    }

    // Optional typed default value baked into the literal.
    FMetasoundFrontendLiteral DefaultLiteral;
    bool bHasDefault = false;
    if (Params->HasField(TEXT("bool_value")))
    {
        bool B = false; Params->TryGetBoolField(TEXT("bool_value"), B);
        DefaultLiteral.Set(B); bHasDefault = true;
    }
    else if (Params->HasField(TEXT("int_value")))
    {
        int32 I = 0; Params->TryGetNumberField(TEXT("int_value"), I);
        DefaultLiteral.Set(I); bHasDefault = true;
    }
    else if (Params->HasField(TEXT("string_value")))
    {
        FString S; Params->TryGetStringField(TEXT("string_value"), S);
        DefaultLiteral.Set(S); bHasDefault = true;
    }
    else if (Params->HasField(TEXT("default_value")))
    {
        double V = 0.0; Params->TryGetNumberField(TEXT("default_value"), V);
        DefaultLiteral.Set(static_cast<float>(V)); bHasDefault = true;
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
// MCP:add_metasound_graph_output - Add a graph output of data_type (default Audio; others include Float, Int32, Bool, String and Trigger) to an existing MetaSound Source through the MetaSound builder, inside an undo transaction, then save the asset. An unregistered data_type or a source that does not load is an error. A name already used by an output is not duplicated or retyped: the engine hands back that output and the reply still says added=true, so check get_metasound_info first. success is false, with save_error, when the save fails (during PIE, for one). Inverse: remove_metasound_graph_output.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param output_name - Name of the graph output node to add.
//   @param data_type - Frontend data type for the output node (optional, default "Audio")
//   @returns { asset_path: string, output_name: string, data_type: string, added: bool, saved: bool, save_error: string? }

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
// connect_metasound_nodes
// Connects a graph INPUT node's output to a graph OUTPUT node's input.
// 'from_node_output_name' resolves a graph-input node via FindGraphInputNode
// (which yields its output handle); 'to_node_input_name' resolves a graph-output
// node via FindGraphOutputNode (which yields its input handle). The handles are
// passed to ConnectNodes; the connection is real where the two data types match
// (the cov fixture deliberately uses a guaranteed-compatible Float->Float pair).
// Verified via NodesAreConnected. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:connect_metasound_nodes - Connects a graph input node's output to a graph output node's input.  Resolves the source handle from a graph INPUT node by name (FindGraphInputNode -> the input node's output handle) and the destination handle from a graph OUTPUT node by name (FindGraphOutputNode -> the output node's input handle), then calls UMetaSoundBuilderBase::ConnectNodes and verifies via NodesAreConnected. The connection is real where the two data types match (e.g. a Float graph input connected to a Float graph output). Saves the asset.
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("connect_metasound_nodes"), SaveErr);
}

// ===========================================================================
// Real DSP patch authoring (process nodes, not just graph I/O)

// ---------------------------------------------------------------------------
// metasound_add_node_by_classname
// Adds a real DSP/process node (e.g. the UE Sine oscillator) to an existing source's
// graph by its registered class name (namespace/name/variant), via the non-deprecated
// AddNodeByClassName overload. Returns the new node's GUID + its input/output pins
// (discovered at runtime) so a follow-up call can set defaults / wire it.
// ---------------------------------------------------------------------------
// MCP:metasound_add_node_by_classname - Add a real process node to a MetaSound Source graph by its registered class name (e.g. UE/Sine/Audio).  Unlike add_metasound_graph_input (which adds a graph input node), this instantiates a DSP node so you can author a real signal patch headless. Returns the node's GUID (node_id) and its enumerated input/output pins (name + data_type) for follow-up set/connect calls. The asset is saved. NOT idempotent: each call adds a new node instance (delete + recreate the source for a clean run).
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("metasound_add_node_by_classname"), SaveErr);
}

// ---------------------------------------------------------------------------
// metasound_set_node_input_default
// Sets the literal default of a NAMED input pin on a CONCRETE node (by node_id GUID),
// then reads it back to verify. Mirrors metasound_set_input_default but for process
// nodes, not graph inputs.
// ---------------------------------------------------------------------------
// MCP:metasound_set_node_input_default - Set the default literal of a named input pin on a concrete node (by node_id from metasound_add_node_by_classname).  Reads the value back to verify and saves. Provide exactly one of ``value`` (float, default), ``int_value``, ``bool_value`` or ``string_value``.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param node_id - GUID of the node (from metasound_add_node_by_classname).
//   @param input_name - Name of the node input pin (discover via the node's enumerated inputs).
//   @param value - Float default (optional, used if no other *_value is provided).
//   @param int_value - Int32 default (optional, mutually exclusive).
//   @param bool_value - Bool default (optional, mutually exclusive).
//   @param string_value - String default (optional, mutually exclusive).
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
            FString::Printf(TEXT("Invalid node_id (expected a node GUID from metasound_add_node_by_classname): %s"), *NodeId));

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
    BuildLiteralFromParams(Params, Literal, ValueType);

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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("metasound_set_node_input_default"));
}

// ---------------------------------------------------------------------------
// metasound_connect_node_to_graph_output
// Connects a concrete node's named output pin to a named graph output via the
// purpose-built ConnectNodeOutputToGraphOutput (idempotent: it clears any existing
// edge before adding). Single canonical path (no runtime fallback).
// ---------------------------------------------------------------------------
// MCP:metasound_connect_node_to_graph_output - Connect a concrete node's output pin to a named graph output, completing a signal patch (e.g. Sine.Audio -> the source's audio output).  Uses ConnectNodeOutputToGraphOutput (idempotent: replaces any existing edge). Reports the honest EMetaSoundBuilderResult.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param node_id - GUID of the source node (from metasound_add_node_by_classname).
//   @param node_output_name - Name of the node's output pin (e.g. "Audio").
//   @param graph_output_name - Name of the target graph output (discover via get_metasound_graph_info).
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("metasound_connect_node_to_graph_output"), SaveErr);
}

// ---------------------------------------------------------------------------
// disconnect_metasound_node_from_graph_output
// Inverse of metasound_connect_node_to_graph_output. Resolves the graph OUTPUT
// node's INPUT handle by name (FindGraphOutputNode), queries NodeInputIsConnected
// first for honest idempotency, then calls UMetaSoundBuilderBase::DisconnectNodeInput
// to clear whatever node output drives that graph output. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:disconnect_metasound_node_from_graph_output - Disconnects whatever node output currently drives a named graph output, clearing the edge (inverse of metasound_connect_node_to_graph_output).  Resolves the graph output node's input handle by name (FindGraphOutputNode) and queries NodeInputIsConnected first: if the input is already unconnected it returns already_disconnected without error (idempotent), otherwise it calls UMetaSoundBuilderBase::DisconnectNodeInput and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param graph_output_name - Name of the target graph output to disconnect (discover via get_metasound_graph_info).
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

// ---------------------------------------------------------------------------
// get_metasound_graph_info
// Reads the source's document RootGraph: node_count + connection_count (from the
// default page graph's Nodes/Edges arrays) plus the graph's declared interface
// input/output counts and names (from the root graph's default interface).
// ---------------------------------------------------------------------------
// MCP:get_metasound_graph_info - Reads the MetaSound Source document graph: node/connection counts + I/O.  Reads the document's RootGraph default page (node + connection/edge counts) and the graph's declared interface inputs/outputs (names + types).          "/Game/MetaSounds/MS_Example" or fully-qualified ".MS_Example").
//   @param metasound_path - Asset path of the MetaSound Source (e.g.
//   @returns { asset_path: string, graph_class_name: string, node_count: number, connection_count: number, variable_count: number, input_count: number, output_count: number, inputs: [{ name: string, type: string }], outputs: [{ name: string, type: string }] }

TSharedPtr<FJsonObject> HandleGetMetaSoundGraphInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString MetaSoundPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }

    UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
    if (!Source)
    {
        return MetaSound_SourceNotFoundReply(MetaSoundPath);
    }

    const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();
    const FMetasoundFrontendGraph& Graph = Doc.RootGraph.GetConstDefaultGraph();
    const FMetasoundFrontendClassInterface& Interface = Doc.RootGraph.GetDefaultInterface();

    TSharedPtr<FJsonObject> Data = MetaSound_NewPayload(Source);
    Data->SetStringField(TEXT("graph_class_name"), Doc.RootGraph.Metadata.GetClassName().ToString());
    Data->SetNumberField(TEXT("node_count"), Graph.Nodes.Num());
    Data->SetNumberField(TEXT("connection_count"), Graph.Edges.Num());
    Data->SetNumberField(TEXT("variable_count"), Graph.Variables.Num());
    Data->SetNumberField(TEXT("input_count"), Interface.Inputs.Num());
    Data->SetNumberField(TEXT("output_count"), Interface.Outputs.Num());
    Data->SetArrayField(TEXT("inputs"), MetaSound_NameTypeArray(Interface.Inputs));
    Data->SetArrayField(TEXT("outputs"), MetaSound_NameTypeArray(Interface.Outputs));
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Graph teardown (disconnect / remove node / remove graph input)

// ---------------------------------------------------------------------------
// disconnect_metasound_nodes
// Inverse of connect_metasound_nodes. Resolves the source handle from a graph
// INPUT node by name (FindGraphInputNode -> output handle) and the destination
// handle from a graph OUTPUT node by name (FindGraphOutputNode -> input handle),
// queries NodesAreConnected first for honest idempotency, then calls
// UMetaSoundBuilderBase::DisconnectNodes. Saves the asset.
// ---------------------------------------------------------------------------
// MCP:disconnect_metasound_nodes - Disconnects a graph input node's output from a graph output node's input (inverse of connect_metasound_nodes).  Resolves the source handle from a graph INPUT node by name (FindGraphInputNode -> the input node's output handle) and the destination handle from a graph OUTPUT node by name (FindGraphOutputNode -> the output node's input handle), then queries NodesAreConnected first: if the edge is already absent it returns already_disconnected without error (idempotent), otherwise it calls UMetaSoundBuilderBase::DisconnectNodes and reports the result. Saves the asset.
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("disconnect_metasound_nodes"), SaveErr);
}

// ---------------------------------------------------------------------------
// remove_metasound_node
// Removes a node (by its GUID node_id, as returned by metasound_add_node_by_classname)
// via UMetaSoundBuilderBase::RemoveNode(bRemoveUnusedDependencies=true). Queries
// ContainsNode first for honest idempotency (already_absent without error). Saves.
// ---------------------------------------------------------------------------
// MCP:remove_metasound_node - Removes a node from a MetaSound Source graph by its node_id GUID (inverse of metasound_add_node_by_classname).  Parses the node_id into a node handle and queries ContainsNode first: if the node is already absent it returns already_absent without error (idempotent), otherwise it calls UMetaSoundBuilderBase::RemoveNode (pruning unused dependencies) and reports the result. Saves the asset.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param node_id - GUID of the node to remove (from metasound_add_node_by_classname).
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
            FString::Printf(TEXT("Invalid node_id (expected a node GUID from metasound_add_node_by_classname): %s"), *NodeId));
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
// MetaSound read / list / delete + reflection-driven introspection
//
// Engine facts this section relies on (UE 5.8, Engine/Plugins/Runtime/Metasound/):
//   - UMetaSoundSource extends USoundWaveProcedural + FMetasoundAssetBase + IMetaSoundDocumentInterface
//   - GetConstDocument() returns FMetasoundFrontendDocument; nodes are at
//     Doc.RootGraph.GetConstDefaultGraph().Nodes (no top-level GetNodes())
//   - FMetasoundFrontendVertex (NOT FMetasoundFrontendPin) is the pin model;
//     Node.Interface.{Inputs,Outputs} are TArray<FMetasoundFrontendVertex>
//   - Metasound::Frontend::ISearchEngine::Get().FindAllClasses() for node-type reflection
//   - UEditorAssetLibrary::DeleteAsset + AssetRegistry::Get for delete / list / refs
//   - IAssetRegistry::GetReferencers for "what references this MetaSound"
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
// list_meta_sounds — AssetRegistry enumeration of UMetaSoundSource under a path
// ---------------------------------------------------------------------------
// MCP:list_meta_sounds - List all UMetaSoundSource assets under a content path (default /Game).
//   @param package_path - Content path to filter on (default "/Game").
//   @returns { package_path, count, sounds: [{ path, name, class }] }
TSharedPtr<FJsonObject> HandleListMetaSounds(
	const TSharedPtr<FJsonObject>& Params)
{
	FString RootPath = TEXT("/Game");
	Params->TryGetStringField(TEXT("package_path"), RootPath);

	// Every UMetaSoundSource, subclasses included, at or below RootPath.
	FARFilter MetaSoundsUnderRoot;
	MetaSoundsUnderRoot.PackagePaths.Add(FName(*RootPath));
	MetaSoundsUnderRoot.bRecursivePaths = true;
	MetaSoundsUnderRoot.ClassPaths.Add(UMetaSoundSource::StaticClass()->GetClassPathName());
	MetaSoundsUnderRoot.bRecursiveClasses = true;

	IAssetRegistry& Registry = mcp_common::AssetRegistry();
	TArray<FAssetData> Found;
	Registry.GetAssets(MetaSoundsUnderRoot, Found);

	TArray<TSharedPtr<FJsonValue>> Listed;
	Listed.Reserve(Found.Num());
	for (const FAssetData& SoundAsset : Found)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("path"), SoundAsset.GetObjectPathString());
		Entry->SetStringField(TEXT("name"), SoundAsset.AssetName.ToString());
		Entry->SetStringField(TEXT("class"), SoundAsset.AssetClassPath.ToString());
		Listed.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("package_path"), RootPath);
	Data->SetNumberField(TEXT("count"), Found.Num());
	Data->SetArrayField(TEXT("sounds"), Listed);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// delete_meta_sound — UEditorAssetLibrary::DeleteAsset with confirm gate
// ---------------------------------------------------------------------------
// MCP:delete_meta_sound - Delete a UMetaSoundSource asset from the project. Idempotent — deleting a non-existent asset is a no-op. Destructive — requires confirm=true.
//   @param metasound_path - Asset path of the MetaSound Source.
//   @param confirm - Must be true to actually delete.
//   @returns { metasound_path, deleted, already_existed, note }
TSharedPtr<FJsonObject> HandleDeleteMetaSound(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	if (!bConfirm)
	{
		return mcp_common::MakeErrorReply(
			TEXT("delete_meta_sound is destructive; pass confirm=true to proceed"));
	}

	const bool bExisted = UEditorAssetLibrary::DoesAssetExist(MetaSoundPath);
	if (!bExisted)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("metasound_path"),   MetaSoundPath);
		Data->SetBoolField  (TEXT("deleted"),         false);
		Data->SetBoolField  (TEXT("already_existed"), false);
		Data->SetStringField(TEXT("note"),            TEXT("No asset at the given path; nothing to delete."));
		return mcp_common::MakeOkReply(Data);
	}

	const bool bDeleted = UEditorAssetLibrary::DeleteAsset(MetaSoundPath);
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"),   MetaSoundPath);
	Data->SetBoolField  (TEXT("deleted"),         bDeleted);
	Data->SetBoolField  (TEXT("already_existed"), true);
	Data->SetStringField(TEXT("note"),            bDeleted ? TEXT("") : TEXT("UEditorAssetLibrary::DeleteAsset returned false."));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_meta_sound_node_types — ISearchEngine reflection of registered node classes
// ---------------------------------------------------------------------------
// MCP:list_meta_sound_node_types - Enumerate all MetaSound node classes registered with the engine (via Metasound::Frontend::ISearchEngine). Editor-only because ISearchEngine::FindAllClasses is gated by WITH_EDITORONLY_DATA.
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
// list_meta_sound_nodes — enumerate nodes in a MetaSound graph
// ---------------------------------------------------------------------------
// MCP:list_meta_sound_nodes - List every node in the default graph of a MetaSound Source asset with its GUID id, name, class name and input/output counts, plus count. Read-only; pass the id to get_meta_sound_node_info and the other node tools.
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
// get_meta_sound_node_info — full node detail (inputs/outputs/literals)
// ---------------------------------------------------------------------------
// MCP:get_meta_sound_node_info - Read one node of the default graph of a MetaSound Source by GUID: its name, class name, input/output/environment vertices (name, type_name, vertex_id), input literal defaults and Configuration struct class. Read-only; get node ids from list_meta_sound_nodes.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_meta_sound_nodes returns it in id; a value that is not a GUID is refused.
//   @returns { metasound_path, node_id, name, class_name, inputs: [{name, type_name}], outputs: [...], environment: [...], input_defaults: [{vertex_id, value, type}], configuration_class }
TSharedPtr<FJsonObject> HandleGetMetaSoundNodeInfo(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath, NodeId;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
	UMetaSoundSource* Source = nullptr;
	TSharedPtr<FJsonObject> Error;
	const FMetasoundFrontendNode* FoundNode = MetaSound_FindGraphNode(MetaSoundPath, NodeId, Source, Error);
	if (!FoundNode)
	{
		return Error;
	}
	const FMetasoundFrontendDocument& Doc = Source->GetConstDocument();

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

	// Input literal defaults — captured as {vertex_id -> {type, value}}.
	TArray<TSharedPtr<FJsonValue>> InputDefaultsJson;
	for (const FMetasoundFrontendVertexLiteral& Lit : FoundNode->InputLiterals)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("vertex_id"), Lit.VertexID.ToString());
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
	Data->SetArrayField (TEXT("inputs"),             SerializeVertices(FoundNode->Interface.Inputs));
	Data->SetArrayField (TEXT("outputs"),            SerializeVertices(FoundNode->Interface.Outputs));
	Data->SetArrayField (TEXT("environment"),        SerializeVertices(FoundNode->Interface.Environment));
	Data->SetArrayField (TEXT("input_defaults"),     InputDefaultsJson);
	Data->SetStringField(TEXT("configuration_class"), CfgStruct ? CfgStruct->GetName() : TEXT(""));
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_meta_sound_node_input_default — read one input literal by vertex name
// ---------------------------------------------------------------------------
// MCP:get_meta_sound_node_input_default - Read the default literal stored on one input of a MetaSound Source node: vertex type, has_value, literal_type and value; has_value false means no literal is set. An unknown input name fails and lists the valid ones. Write it with metasound_set_node_input_default.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_meta_sound_nodes returns it in id; a value that is not a GUID is refused.
//   @param input_name - Name of the input vertex (matches FMetasoundFrontendVertex::Name).
//   @returns { metasound_path, node_id, input_name, type, has_value, literal_type?, value? }
TSharedPtr<FJsonObject> HandleGetMetaSoundNodeInputDefault(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath, NodeId, InputName;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName)) { return Missing; }
	UMetaSoundSource* Source = nullptr;
	TSharedPtr<FJsonObject> Error;
	const FMetasoundFrontendNode* FoundNode = MetaSound_FindGraphNode(MetaSoundPath, NodeId, Source, Error);
	if (!FoundNode)
	{
		return Error;
	}

	const FMetasoundFrontendVertex* Vertex = FoundNode->Interface.Inputs.FindByPredicate(
		[&InputName](const FMetasoundFrontendVertex& V) { return V.Name == FName(*InputName); });
	if (!Vertex)
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

	const FMetasoundFrontendVertexLiteral* Literal = FoundNode->InputLiterals.FindByPredicate(
		[Vertex](const FMetasoundFrontendVertexLiteral& L) { return L.VertexID == Vertex->VertexID; });

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"), MetaSoundPath);
	Data->SetStringField(TEXT("node_id"),        NodeId);
	Data->SetStringField(TEXT("input_name"),     InputName);
	Data->SetStringField(TEXT("type"),           Vertex->TypeName.ToString());
	Data->SetBoolField  (TEXT("has_value"),     Literal != nullptr);
	if (Literal)
	{
		Data->SetStringField(TEXT("literal_type"), UEnum::GetValueAsString(Literal->Value.GetType()));
		MetaSound_SetLiteralValueField(Data, Literal->Value);
	}
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_meta_sound_node_property / get_meta_sound_node_property
//   Reflection on the node's TInstancedStruct<FMetaSoundFrontendNodeConfiguration>.
//   The Configuration struct is per-subclass — for example ArrayGet's config has
//   NumOutputs, Index pin references, etc. We round-trip via FJsonObjectConverter.
// ---------------------------------------------------------------------------
// MCP:set_meta_sound_node_property - Write one field of a MetaSound Source node's Configuration struct by reflection, directly in the document (not through the MetaSound builder), then save the asset. Re-applying a value returns value_changed false but still saves. Succeeds only when the asset reaches disk; a failed save leaves the value in memory only (saved false, save_error).
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_meta_sound_nodes returns it in id; a value that is not a GUID is refused.
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
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_meta_sound_node_property"), SaveError);
}

// MCP:get_meta_sound_node_property - Read one field of the Configuration struct of a MetaSound Source node by property name, returning its C++ type, has_value and value as JSON. Fails when the node has no Configuration struct or the struct has no such property. Read-only; write it with set_meta_sound_node_property.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @param node_id - Node GUID, as list_meta_sound_nodes returns it in id; a value that is not a GUID is refused.
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

// ---------------------------------------------------------------------------
// list_meta_sound_references — AssetRegistry "what assets reference this MS?"
// ---------------------------------------------------------------------------
// MCP:list_meta_sound_references - List the packages that reference a MetaSound Source asset, hard or soft, from the Asset Registry, with their count. Read-only; the source is loaded to resolve its package. For assets of any type use get_asset_referencers.
//   @param metasound_path - MetaSound Source asset path, e.g. "/Game/Audio/MS_Rain"; MetaSound Patches are not accepted.
//   @returns { metasound_path, count, references: [{ package_name }] }
TSharedPtr<FJsonObject> HandleListMetaSoundReferences(
	const TSharedPtr<FJsonObject>& Params)
{
	FString MetaSoundPath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("metasound_path"), MetaSoundPath)) { return Missing; }
	UMetaSoundSource* Source = mcp_common::ResolveAsset<UMetaSoundSource>(MetaSoundPath);
	if (!Source)
	{
		return MetaSound_CannotLoadSourceReply(MetaSoundPath);
	}
	const FName PackageName(*Source->GetPackage()->GetName());

	IAssetRegistry& AR = IAssetRegistry::GetChecked();
	TArray<FName> Referencers;
	AR.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package);

	TArray<TSharedPtr<FJsonValue>> RefsJson;
	RefsJson.Reserve(Referencers.Num());
	for (const FName& Ref : Referencers)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("package_name"), Ref.ToString());
		RefsJson.Add(MakeShared<FJsonValueObject>(E));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("metasound_path"), MetaSoundPath);
	Data->SetNumberField(TEXT("count"),         Referencers.Num());
	Data->SetArrayField (TEXT("references"),    RefsJson);
	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_metasound_graph_input"), &HandleAddMetaSoundGraphInput },
    { TEXT("add_metasound_graph_output"), &HandleAddMetaSoundGraphOutput },
    { TEXT("connect_metasound_nodes"), &HandleConnectMetaSoundNodes },
    { TEXT("create_metasound_source"), &HandleCreateMetaSoundSource },
    { TEXT("delete_meta_sound"), &HandleDeleteMetaSound },
    { TEXT("disconnect_metasound_node_from_graph_output"), &HandleMetaSoundDisconnectNodeFromGraphOutput },
    { TEXT("disconnect_metasound_nodes"), &HandleDisconnectMetaSoundNodes },
    { TEXT("get_meta_sound_node_info"), &HandleGetMetaSoundNodeInfo },
    { TEXT("get_meta_sound_node_input_default"), &HandleGetMetaSoundNodeInputDefault },
    { TEXT("get_meta_sound_node_property"), &HandleGetMetaSoundNodeProperty },
    { TEXT("get_metasound_graph_info"), &HandleGetMetaSoundGraphInfo },
    { TEXT("get_metasound_info"), &HandleGetMetaSoundInfo },
    { TEXT("list_meta_sound_node_types"), &HandleListMetaSoundNodeTypes },
    { TEXT("list_meta_sound_nodes"), &HandleListMetaSoundNodes },
    { TEXT("list_meta_sound_references"), &HandleListMetaSoundReferences },
    { TEXT("list_meta_sounds"), &HandleListMetaSounds },
    { TEXT("metasound_add_interface"), &HandleMetaSoundAddInterface },
    { TEXT("metasound_add_node_by_classname"), &HandleMetaSoundAddNodeByClassName },
    { TEXT("metasound_connect_node_to_graph_output"), &HandleMetaSoundConnectNodeToGraphOutput },
    { TEXT("metasound_set_input_default"), &HandleMetaSoundSetInputDefault },
    { TEXT("metasound_set_node_input_default"), &HandleMetaSoundSetNodeInputDefault },
    { TEXT("remove_metasound_graph_input"), &HandleRemoveMetaSoundGraphInput },
    { TEXT("remove_metasound_graph_output"), &HandleRemoveMetaSoundGraphOutput },
    { TEXT("remove_metasound_node"), &HandleRemoveMetaSoundNode },
    { TEXT("set_meta_sound_node_property"), &HandleSetMetaSoundNodeProperty },
};
} // namespace UEAgentMetaSoundCommands
