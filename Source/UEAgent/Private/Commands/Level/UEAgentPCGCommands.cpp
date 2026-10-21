// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"

// PCG runtime (module: PCG)
#include "PCGGraph.h"          // UPCGGraph: AddNodeOfType / GetNodes / GetInputNode / GetOutputNode / AddEdge / RemoveEdge / GetAllEdges
#include "PCGNode.h"           // UPCGNode: GetNodeTitle / GetInputPins / GetOutputPins / GetInputPin / GetOutputPin
#include "PCGPin.h"            // UPCGPin: Properties.Label (FPCGPinProperties)
#include "PCGEdge.h"           // UPCGEdge: GetInputNode / GetOutputNode / GetInputPinLabel / GetOutputPinLabel
#include "PCGSettings.h"       // UPCGSettings (base for node-type classes)
#include "PCGCommon.h"         // EPCGNodeTitleType + PCGPinConstants::DefaultInputLabel/DefaultOutputLabel
#include "PCGVolume.h"         // APCGVolume + APCGVolume::PCGComponent
#include "PCGComponent.h"      // UPCGComponent::SetGraph / GetGraph

// Engine / editor
#include "Editor.h"                                         // GEditor
#include "GameFramework/Actor.h"
#include "Engine/World.h"
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Level.h"                 // ULevel must be complete -- GetCurrentLevel() feeds StaticFindObjectFast's Outer
#include "UObject/Class.h"
#include "UObject/UnrealType.h"      // FProperty (set_pcg_component_properties read-back)
#include "JsonObjectConverter.h"     // FJsonObjectConverter::JsonValueToUProperty (set_pcg_node_properties stages its keys)
#include "UObject/EnumProperty.h"    // FEnumProperty (the JSON kind a staged key of an enum must hold)
#include "Commands/Camera/ClassReflectionUtils.h"  // property writes by reflection
#include "StructUtils/PropertyBag.h" // FInstancedPropertyBag / FPropertyBagPropertyDesc (graph params)
#include "PCGData.h"                 // FPCGDataCollection / FPCGTaggedData (get_pcg_generated_data)
#include "PCGSubgraph.h"             // UPCGBaseSubgraphSettings::SetSubgraph (add_pcg_subgraph_node)
#include "Elements/PCGUserParameterGet.h" // UPCGUserParameterGetSettings (remove_pcg_graph_parameter: the getter nodes)
#include "Misc/PackageName.h"
// spawn_pcg_volume: an APCGVolume spawned without a brush has extent (0,0,0) and a
// UPCGComponent with null bounds generates 0 instances in total silence. These headers
// read back the cube brush mcp_common::BuildCubeBrushForVolume builds.
#include "GameFramework/Volume.h"          // AVolume (APCGVolume's base) + GetBrushComponent
#include "Model.h"                         // UModel (AVolume::Brush)
#include "Engine/Polys.h"                  // UPolys (the real oracle of "there is geometry")

/**
 * PCG (Procedural Content Generation) commands: authoring UPCGGraph assets, and the editor-world actors
 * (an APCGVolume, or any actor given a UPCGComponent) that generate from them.
 *
 * Requires the PCG plugin (runtime module PCG). Node-type classes (e.g. UPCGSurfaceSamplerSettings,
 * UPCGStaticMeshSpawnerSettings) are resolved by name at runtime through UClass::TryFindTypeSlow /
 * FindFirstObject, so their MinimalAPI headers need not be included and the PCGEditor module is not
 * pulled in.
 */
namespace UEAgentPCGCommands
{

// MCP-DOMAIN: Procedural

namespace
{
    // Resolve a short or fully-qualified UPCGSettings subclass by name.
    // Accepts e.g. "PCGSurfaceSamplerSettings", "UPCGSurfaceSamplerSettings",
    // or "/Script/PCG.PCGSurfaceSamplerSettings". Returns nullptr if it is not a
    // concrete (non-abstract) UPCGSettings subclass.
    UClass* ResolvePCGSettingsClass(const FString& InTypeName)
    {
        if (InTypeName.IsEmpty())
        {
            return nullptr;
        }

        UClass* Found = nullptr;

        // Fully-qualified /Script path.
        if (InTypeName.StartsWith(TEXT("/Script/")))
        {
            Found = LoadObject<UClass>(nullptr, *InTypeName);
        }

        if (!Found)
        {
            // Try the given name, then the U-prefixed variant.
            Found = UClass::TryFindTypeSlow<UClass>(InTypeName);
            if (!Found && !InTypeName.StartsWith(TEXT("U")))
            {
                Found = UClass::TryFindTypeSlow<UClass>(FString(TEXT("U")) + InTypeName);
            }
        }

        if (!Found)
        {
            Found = FindFirstObject<UClass>(*InTypeName, EFindFirstObjectOptions::None);
        }

        if (!Found)
        {
            // Last resort: assume it lives in the PCG module.
            const FString ScriptPath = FString::Printf(TEXT("/Script/PCG.%s"), *InTypeName);
            Found = LoadObject<UClass>(nullptr, *ScriptPath);
        }

        if (Found && Found->IsChildOf(UPCGSettings::StaticClass()) &&
            !Found->HasAnyClassFlags(CLASS_Abstract))
        {
            return Found;
        }
        return nullptr;
    }

    // Resolve a node within a graph by its UObject name (as reported by
    // get_pcg_graph_info), or by the tokens "Input"/"Output" for the implicit
    // graph endpoint nodes (which are not part of GetNodes()). Case-insensitive.
    UPCGNode* ResolvePCGNode(UPCGGraph* Graph, const FString& InName)
    {
        if (!Graph || InName.IsEmpty())
        {
            return nullptr;
        }
        if (InName.Equals(TEXT("Input"), ESearchCase::IgnoreCase) ||
            InName.Equals(TEXT("InputNode"), ESearchCase::IgnoreCase) ||
            InName.Equals(TEXT("Source"), ESearchCase::IgnoreCase))
        {
            return Graph->GetInputNode();
        }
        if (InName.Equals(TEXT("Output"), ESearchCase::IgnoreCase) ||
            InName.Equals(TEXT("OutputNode"), ESearchCase::IgnoreCase) ||
            InName.Equals(TEXT("Sink"), ESearchCase::IgnoreCase))
        {
            return Graph->GetOutputNode();
        }
        for (UPCGNode* Node : Graph->GetNodes())
        {
            if (Node && Node->GetName().Equals(InName, ESearchCase::IgnoreCase))
            {
                return Node;
            }
        }
        // Allow addressing the implicit endpoint nodes by their real object name too.
        if (UPCGNode* In = Graph->GetInputNode())
        {
            if (In->GetName().Equals(InName, ESearchCase::IgnoreCase)) { return In; }
        }
        if (UPCGNode* Out = Graph->GetOutputNode())
        {
            if (Out->GetName().Equals(InName, ESearchCase::IgnoreCase)) { return Out; }
        }
        return nullptr;
    }

    // Comma-separated list of pin labels, for honest "available pins" error text.
    FString JoinPinLabels(const TArray<TObjectPtr<UPCGPin>>& Pins)
    {
        TArray<FString> Labels;
        for (const UPCGPin* P : Pins)
        {
            if (P)
            {
                Labels.Add(P->Properties.Label.ToString());
            }
        }
        return FString::Join(Labels, TEXT(", "));
    }

    // Comma-separated node names of a graph (user nodes only), for error text.
    FString JoinNodeNames(const UPCGGraph* Graph)
    {
        TArray<FString> Names;
        for (const UPCGNode* Node : Graph->GetNodes())
        {
            if (Node)
            {
                Names.Add(Node->GetName());
            }
        }
        return FString::Join(Names, TEXT(", "));
    }

    // Resolve which output-pin label to use on From. An explicit RequestedLabel must
    // exist; otherwise prefer the default "Out" pin, else the node's first output pin.
    // The resolved label is echoed back to the caller so the choice is transparent.
    bool ResolveOutputPinLabel(UPCGNode* Node, const FString& RequestedLabel, FName& OutLabel, FString& OutError)
    {
        const TArray<TObjectPtr<UPCGPin>>& Pins = Node->GetOutputPins();
        if (Pins.Num() == 0)
        {
            OutError = FString::Printf(TEXT("Node '%s' has no output pins to connect from"), *Node->GetName());
            return false;
        }
        if (!RequestedLabel.IsEmpty())
        {
            if (UPCGPin* P = Node->GetOutputPin(FName(*RequestedLabel)))
            {
                OutLabel = P->Properties.Label;
                return true;
            }
            OutError = FString::Printf(TEXT("Node '%s' has no output pin '%s' (available: %s)"),
                *Node->GetName(), *RequestedLabel, *JoinPinLabels(Pins));
            return false;
        }
        if (UPCGPin* P = Node->GetOutputPin(PCGPinConstants::DefaultOutputLabel))
        {
            OutLabel = P->Properties.Label;
            return true;
        }
        OutLabel = Pins[0]->Properties.Label;
        return true;
    }

    // Symmetric to ResolveOutputPinLabel for the input (downstream) side: explicit
    // label must exist; else prefer the default "In" pin, else the first input pin.
    bool ResolveInputPinLabel(UPCGNode* Node, const FString& RequestedLabel, FName& OutLabel, FString& OutError)
    {
        const TArray<TObjectPtr<UPCGPin>>& Pins = Node->GetInputPins();
        if (Pins.Num() == 0)
        {
            OutError = FString::Printf(TEXT("Node '%s' has no input pins to connect to"), *Node->GetName());
            return false;
        }
        if (!RequestedLabel.IsEmpty())
        {
            if (UPCGPin* P = Node->GetInputPin(FName(*RequestedLabel)))
            {
                OutLabel = P->Properties.Label;
                return true;
            }
            OutError = FString::Printf(TEXT("Node '%s' has no input pin '%s' (available: %s)"),
                *Node->GetName(), *RequestedLabel, *JoinPinLabels(Pins));
            return false;
        }
        if (UPCGPin* P = Node->GetInputPin(PCGPinConstants::DefaultInputLabel))
        {
            OutLabel = P->Properties.Label;
            return true;
        }
        OutLabel = Pins[0]->Properties.Label;
        return true;
    }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // 5.7 has no UPCGGraph::GetAllEdges(). The same walk as 5.8's: every edge once, from its downstream (input) pin,
    // over the output node and the other nodes (the input node has no inbound edge).
    TArray<UPCGEdge*> PCG_GetAllEdges(const UPCGGraph* Graph)
    {
        TArray<UPCGEdge*> AllEdges;
        auto GatherInputEdgesFromNode = [&AllEdges](const UPCGNode* Node)
        {
            for (const UPCGPin* InPin : Node->GetInputPins())
            {
                AllEdges.Append(InPin->Edges);
            }
        };
        GatherInputEdgesFromNode(Graph->GetOutputNode());
        for (const UPCGNode* Node : Graph->GetNodes())
        {
            GatherInputEdgesFromNode(Node);
        }
        return AllEdges;
    }
#endif

    // True if a directed edge From.FromLabel (output) -> To.ToLabel (input) already
    // exists. UPCGEdge names its ends from the data-flow side: GetInputNode()/
    // GetInputPinLabel() are the upstream (From) side; GetOutputNode()/
    // GetOutputPinLabel() are the downstream (To) side.
    bool PCGEdgeExists(const UPCGGraph* Graph, const UPCGNode* From, const FName& FromLabel,
                       const UPCGNode* To, const FName& ToLabel)
    {
        if (!Graph)
        {
            return false;
        }
#if UE_VERSION_OLDER_THAN(5, 8, 0)
        // 5.7 has no UPCGEdge::GetInputNode() family: they read a valid edge's InputPin (upstream, From) and
        // OutputPin (downstream, To), so read those.
        for (const UPCGEdge* Edge : PCG_GetAllEdges(Graph))
        {
            if (!Edge || !Edge->IsValid())
            {
                continue;
            }
            const UPCGNode* EdgeFrom = Edge->InputPin->Node;
            const UPCGNode* EdgeTo = Edge->OutputPin->Node;
            if (EdgeFrom == From && EdgeTo == To &&
                Edge->InputPin->Properties.Label == FromLabel && Edge->OutputPin->Properties.Label == ToLabel)
            {
                return true;
            }
        }
#else
        for (const UPCGEdge* Edge : Graph->GetAllEdges())
        {
            if (!Edge || !Edge->IsValid())
            {
                continue;
            }
            if (Edge->GetInputNode() == From && Edge->GetOutputNode() == To &&
                Edge->GetInputPinLabel() == FromLabel && Edge->GetOutputPinLabel() == ToLabel)
            {
                return true;
            }
        }
#endif
        return false;
    }

    // A reply payload naming Graph: graph_name and asset_path.
    TSharedPtr<FJsonObject> PCG_NewGraphData(const UPCGGraph* Graph)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("graph_name"), Graph->GetName());
        Data->SetStringField(TEXT("asset_path"), Graph->GetPathName());
        return Data;
    }

    // Dirties and saves Graph's package; false when the graph has no package or the save fails.
    bool PCG_SaveGraph(UPCGGraph* Graph, FString& OutError)
    {
        Graph->MarkPackageDirty();
        if (UPackage* Package = Graph->GetPackage())
        {
            return mcp_common::SaveAssetPackage(Package, Graph, OutError);
        }
        return false;
    }

    // ResolvePCGNode, or null with OutError naming Role and the graph's nodes.
    UPCGNode* PCG_ResolveNodeOrError(UPCGGraph* Graph, const FString& Name, const TCHAR* Role, TSharedPtr<FJsonObject>& OutError)
    {
        UPCGNode* Node = ResolvePCGNode(Graph, Name);
        if (!Node)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s '%s' not found (use \"Input\"/\"Output\" or one of: %s)"), Role, *Name, *JoinNodeNames(Graph)));
        }
        return Node;
    }

    // The required "pcg_graph_path" and "node_name" params; null on success, else the error reply.
    TSharedPtr<FJsonObject> PCG_ReadNodeParams(const TSharedPtr<FJsonObject>& Params, FString& OutGraphPath, FString& OutNodeName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), OutGraphPath)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_name"), OutNodeName)) { return Missing; }
        return nullptr;
    }

    // Graph, node and node settings; null on success, else the error reply (Purpose ends its no-settings text).
    TSharedPtr<FJsonObject> PCG_ResolveNodeSettings(const FString& GraphPath, const FString& NodeName, const TCHAR* Purpose,
        UPCGGraph*& OutGraph, UPCGNode*& OutNode, UPCGSettings*& OutSettings)
    {
        OutGraph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
        if (!OutGraph)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
        }
        TSharedPtr<FJsonObject> Error;
        OutNode = PCG_ResolveNodeOrError(OutGraph, NodeName, TEXT("node"), Error);
        if (!OutNode)
        {
            return Error;
        }
        OutSettings = OutNode->GetSettings();
        if (!OutSettings)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Node '%s' has no settings object to %s"), *OutNode->GetName(), Purpose));
        }
        return nullptr;
    }

    // Graph, end nodes and pin labels an edge command names; null on success, else the error reply.
    TSharedPtr<FJsonObject> PCG_ResolveEdgeEnds(const TSharedPtr<FJsonObject>& Params, UPCGGraph*& OutGraph,
        UPCGNode*& OutFrom, FName& OutFromLabel, UPCGNode*& OutTo, FName& OutToLabel)
    {
        FString GraphPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }
        FString FromName;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_node"), FromName)) { return Missing; }
        FString ToName;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_node"), ToName)) { return Missing; }
        FString FromPinReq;
        Params->TryGetStringField(TEXT("from_pin"), FromPinReq);
        FString ToPinReq;
        Params->TryGetStringField(TEXT("to_pin"), ToPinReq);

        OutGraph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
        if (!OutGraph)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
        }

        TSharedPtr<FJsonObject> Error;
        OutFrom = PCG_ResolveNodeOrError(OutGraph, FromName, TEXT("from_node"), Error);
        if (!OutFrom)
        {
            return Error;
        }
        OutTo = PCG_ResolveNodeOrError(OutGraph, ToName, TEXT("to_node"), Error);
        if (!OutTo)
        {
            return Error;
        }

        FString PinError;
        if (!ResolveOutputPinLabel(OutFrom, FromPinReq, OutFromLabel, PinError))
        {
            return mcp_common::MakeErrorReply(PinError);
        }
        if (!ResolveInputPinLabel(OutTo, ToPinReq, OutToLabel, PinError))
        {
            return mcp_common::MakeErrorReply(PinError);
        }
        return nullptr;
    }

    // The values of one properties object staged for a write into an object: each converted into a copy of the current
    // value of the property its key names. The copies are freed with it.
    struct FPCGStagedProperties
    {
        TArray<TPair<FProperty*, void*>> Values;

        FPCGStagedProperties() = default;
        FPCGStagedProperties(const FPCGStagedProperties&) = delete;
        FPCGStagedProperties& operator=(const FPCGStagedProperties&) = delete;
        ~FPCGStagedProperties()
        {
            for (const TPair<FProperty*, void*>& Value : Values)
            {
                Value.Key->DestroyAndFreeValue(Value.Value);
            }
        }
    };

    // Value, a value of Property, clamped to Property's ClampMin/ClampMax meta, which FJsonObjectConverter ignores:
    // what mcp_common::ClampNumericByMeta does to each property of a container.
    void PCG_ClampToMeta(const FProperty& Property, void* Value)
    {
        const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
        const bool bHasMin = Property.HasMetaData(TEXT("ClampMin"));
        const bool bHasMax = Property.HasMetaData(TEXT("ClampMax"));
        if (!Numeric || (!bHasMin && !bHasMax))
        {
            return;
        }
        if (Numeric->IsFloatingPoint())
        {
            double Number = Numeric->GetFloatingPointPropertyValue(Value);
            if (bHasMin) { Number = FMath::Max(Number, FCString::Atod(*Property.GetMetaData(TEXT("ClampMin")))); }
            if (bHasMax) { Number = FMath::Min(Number, FCString::Atod(*Property.GetMetaData(TEXT("ClampMax")))); }
            Numeric->SetFloatingPointPropertyValue(Value, Number);
        }
        else if (Numeric->IsInteger())
        {
            int64 Number = Numeric->GetSignedIntPropertyValue(Value);
            if (bHasMin) { Number = FMath::Max<int64>(Number, FCString::Atoi64(*Property.GetMetaData(TEXT("ClampMin")))); }
            if (bHasMax) { Number = FMath::Min<int64>(Number, FCString::Atoi64(*Property.GetMetaData(TEXT("ClampMax")))); }
            Numeric->SetIntPropertyValue(Value, Number);
        }
    }

    // What Value must be for Property when it is not that, else empty. FJsonObjectConverter reads a number or a bool
    // out of any JSON value without an error -- a string that is not a number becomes 0, any string a bool -- so
    // those kinds are checked here; the converter refuses a wrong kind for the others itself.
    FString PCG_ExpectedJsonKind(const FProperty& Property, const FJsonValue& Value)
    {
        if (Property.ArrayDim != 1)
        {
            return FString();
        }
        const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
        if (CastField<FEnumProperty>(&Property) || (Numeric && Numeric->IsEnum()))
        {
            return Value.Type == EJson::String || Value.Type == EJson::Number ? FString() : FString(TEXT("an enum name"));
        }
        if (Numeric)
        {
            const bool bNumber = Value.Type == EJson::Number || (Value.Type == EJson::String && Value.AsString().IsNumeric());
            return bNumber ? FString() : FString(TEXT("a number"));
        }
        if (CastField<FBoolProperty>(&Property))
        {
            return Value.Type == EJson::Boolean ? FString() : FString(TEXT("true or false"));
        }
        return FString();
    }

    // Stages every key of Json for a write into Object, before anything is written (Param names the param in the
    // errors). Null when each key names a property of Object's class -- matched case-insensitively, as
    // FJsonObjectConverter matches them -- that is neither deprecated nor holds instanced subobjects (the staging
    // conversion would create those outside Object), holds a value of its kind (PCG_ExpectedJsonKind), and the
    // value converts; it is then clamped (PCG_ClampToMeta). Else the error reply naming the key, with nothing
    // changed: merging into Object directly would skip a key that names no property without a word, and stop at
    // the first value that does not convert with the keys before it already written.
    TSharedPtr<FJsonObject> PCG_StageProperties(const FJsonObject& Json, UObject& Object, const TCHAR* Param,
        FPCGStagedProperties& OutStaged)
    {
        const UClass* Class = Object.GetClass();
        auto IsWritable = [](const FProperty& Property)
        {
            return !Property.HasAnyPropertyFlags(
                CPF_Deprecated | CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
        };
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Json.Values)
        {
            FProperty* Property = nullptr;
            for (TFieldIterator<FProperty> It(Class); It && !Property; ++It)
            {
                if (Class->GetAuthoredNameForField(*It).Equals(Pair.Key, ESearchCase::IgnoreCase))
                {
                    Property = *It;
                }
            }
            if (!Property || !IsWritable(*Property))
            {
                TArray<FString> Names;
                for (TFieldIterator<FProperty> It(Class); It; ++It)
                {
                    if (IsWritable(**It))
                    {
                        Names.Add(Class->GetAuthoredNameForField(*It));
                    }
                }
                const FString Problem = Property
                    ? FString::Printf(TEXT("names %s, which is deprecated or holds instanced subobjects and is not written here"), *Property->GetName())
                    : FString::Printf(TEXT("names no property of %s"), *Class->GetName());
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s key '%s' %s; nothing was changed. Writable properties (keys match them case-insensitively): %s"),
                    Param, *Pair.Key, *Problem, *FString::Join(Names, TEXT(", "))));
            }
            if (!Pair.Value.IsValid() || Pair.Value->IsNull())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s.%s is null: give it a value or leave the key out; nothing was changed."), Param, *Pair.Key));
            }
            const FString Expected = PCG_ExpectedJsonKind(*Property, *Pair.Value);
            if (!Expected.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s.%s must be %s for %s (%s); nothing was changed."),
                    Param, *Pair.Key, *Expected, *Property->GetName(), *Property->GetCPPType()));
            }
            void* Value = Property->AllocateAndInitializeValue();
            OutStaged.Values.Emplace(Property, Value);
            Property->CopyCompleteValue(Value, Property->ContainerPtrToValuePtr<void>(&Object));
            FText Reason;
            if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Property, Value, 0, 0, false, &Reason))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s.%s does not convert to %s (%s): %s; nothing was changed."), Param, *Pair.Key,
                    *Property->GetName(), *Property->GetCPPType(), *Reason.ToString().Replace(TEXT("\n"), TEXT(" "))));
            }
            PCG_ClampToMeta(*Property, Value);
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// create_pcg_graph
// Creates (or loads, idempotently) a UPCGGraph asset under a /Game path and
// saves it. Uses CreatePackage + NewObject<UPCGGraph> (the PCGGraphFactory lives
// in the PCGEditor module and is not needed here; UPCGGraph has no modal setup).
// ---------------------------------------------------------------------------
// MCP:create_pcg_graph - Create an empty UPCGGraph asset at package_path/name and save it, or return the UPCGGraph already there unchanged (created=false, already_exists=true, nothing saved), so repeat calls are idempotent. A fresh graph holds only its implicit Input and Output nodes, which node_count leaves out (0). Another asset class at that path, or an invalid path, is an error; a failed save, as during PIE, makes success false with save_error. No undo step is recorded. Requires the PCG plugin. Next: add nodes with add_pcg_node (types from list_pcg_node_types), wire them with connect_pcg_nodes, expose tunables with add_pcg_graph_parameter, read back with get_pcg_graph_info, and use it in the level through spawn_box_volume or add_pcg_component.
//   @param name - Name of the PCG Graph asset to create; the object path becomes package_path/name.name.
//   @param package_path - Destination /Game folder (default /Game/PCG); a trailing slash is ignored.
//   @returns { graph_name: string, asset_path: string, created: bool, already_exists?: bool, node_count: int, saved?: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreatePCGGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/PCG");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);

    // Idempotent: if it already exists, load and return a stable payload shape.
    if (UPCGGraph* Existing = LoadObject<UPCGGraph>(nullptr, *ObjectPath))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("graph_name"), Existing->GetName());
        Data->SetStringField(TEXT("asset_path"), ObjectPath);
        Data->SetBoolField(TEXT("created"), false);
        Data->SetBoolField(TEXT("already_exists"), true);
        Data->SetNumberField(TEXT("node_count"), Existing->GetNodes().Num());
        return mcp_common::MakeOkReply(Data);
    }

    FString CreateError;
    UPCGGraph* NewGraph = mcp_common::NewAssetInPackage<UPCGGraph>(PackageName, Name, TEXT("Failed to create UPCGGraph object"), CreateError);
    if (!NewGraph)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewGraph);
    NewGraph->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewGraph->GetPackage(), NewGraph, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("graph_name"), NewGraph->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    // A fresh UPCGGraph already has the implicit Input/Output nodes; GetNodes()
    // returns only the user-added nodes (initially 0).
    Data->SetNumberField(TEXT("node_count"), NewGraph->GetNodes().Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_pcg_graph"), SaveError);
}

// ---------------------------------------------------------------------------
// add_pcg_node
// Adds a node of the given UPCGSettings subclass to a PCG graph via
// UPCGGraph::AddNodeOfType (PCG_API, headless-safe: it creates the settings in
// the transient package, builds the node, and reparents it into the graph).
// Saves the graph after mutating. Returns the node title + new node count.
// ---------------------------------------------------------------------------
// MCP:add_pcg_node - Adds a node of the given UPCGSettings subclass to a PCG graph.  Uses UPCGGraph::AddNodeOfType, which creates the node's settings in the transient package and reparents the node into the graph (headless-safe). The graph is re-saved after the mutation.
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example" or fully-qualified "/Game/PCG/PCG_Example.PCG_Example").
//   @param node_type - A concrete UPCGSettings subclass name, e.g. "PCGSurfaceSamplerSettings", "PCGStaticMeshSpawnerSettings", or a fully-qualified "/Script/PCG.PCGSurfaceSamplerSettings".
//   @returns { graph_name: string, asset_path: string, node_title: string, node_name: string, settings_class: string, node_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddPCGNode(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }

    FString NodeType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_type"), NodeType)) { return Missing; }

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    UClass* SettingsClass = ResolvePCGSettingsClass(NodeType);
    if (!SettingsClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not resolve '%s' to a concrete UPCGSettings subclass (e.g. 'PCGSurfaceSamplerSettings', 'PCGStaticMeshSpawnerSettings')"), *NodeType));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddPCGNode", "Add PCG Node"));
    Graph->Modify();

    UPCGSettings* OutSettings = nullptr;
    UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, OutSettings);
    if (!NewNode)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("AddNodeOfType returned null for settings class '%s'"), *SettingsClass->GetName()));
    }

    FString SaveError;
    const bool bSaved = PCG_SaveGraph(Graph, SaveError);

    const FString NodeTitle = NewNode->GetNodeTitle(EPCGNodeTitleType::ListView).ToString();

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("node_title"), NodeTitle);
    Data->SetStringField(TEXT("node_name"), NewNode->GetName());
    Data->SetStringField(TEXT("settings_class"), SettingsClass->GetName());
    Data->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
    if (!bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_pcg_node"));
}

// ---------------------------------------------------------------------------
// get_pcg_graph_info
// Loads a UPCGGraph and reports its node count and the title of every node.
// ---------------------------------------------------------------------------
// MCP:get_pcg_graph_info - Reads the structure of a UPCGGraph: node count, per-node titles and pin labels, and every directed edge.  Pin labels and the input_node_name/output_node_name feed connect_pcg_nodes / disconnect_pcg_nodes; the implicit graph endpoints are also addressable by the tokens "Input"/"Output".
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example" or fully-qualified "/Game/PCG/PCG_Example.PCG_Example").
//   @returns { graph_name: string, asset_path: string, node_count: int, nodes: [{node_name, title, settings_class, input_pins: [string], output_pins: [string]}], has_input_node: bool, has_output_node: bool, input_node_name: string, output_node_name: string, edges: [{from_node, from_pin, to_node, to_pin}], edge_count: int }

TSharedPtr<FJsonObject> HandleGetPCGGraphInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    const TArray<UPCGNode*>& Nodes = Graph->GetNodes();

    TArray<TSharedPtr<FJsonValue>> NodeArray;
    for (const UPCGNode* Node : Nodes)
    {
        if (!Node)
        {
            continue;
        }
        TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
        N->SetStringField(TEXT("node_name"), Node->GetName());
        N->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
        if (const UPCGSettings* Settings = Node->GetSettings())
        {
            N->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
        }
        // Pin labels so callers know what connect_pcg_nodes can target.
        TArray<TSharedPtr<FJsonValue>> InPins;
        for (const UPCGPin* Pin : Node->GetInputPins())
        {
            if (Pin)
            {
                InPins.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
            }
        }
        TArray<TSharedPtr<FJsonValue>> OutPins;
        for (const UPCGPin* Pin : Node->GetOutputPins())
        {
            if (Pin)
            {
                OutPins.Add(MakeShared<FJsonValueString>(Pin->Properties.Label.ToString()));
            }
        }
        N->SetArrayField(TEXT("input_pins"), InPins);
        N->SetArrayField(TEXT("output_pins"), OutPins);
        NodeArray.Add(MakeShared<FJsonValueObject>(N));
    }

    const UPCGNode* InputNode = Graph->GetInputNode();
    const UPCGNode* OutputNode = Graph->GetOutputNode();

    // Enumerate the graph's directed edges so callers can see what is already
    // wired. UPCGEdge names its ends from the data-flow side: GetInputNode() is
    // the upstream (from) node, GetOutputNode() the downstream (to) node.
    TArray<TSharedPtr<FJsonValue>> EdgeArray;
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // 5.7 has no UPCGEdge::GetInputNode() family: they read a valid edge's InputPin (upstream) and OutputPin
    // (downstream), so read those.
    for (const UPCGEdge* Edge : PCG_GetAllEdges(Graph))
    {
        if (!Edge || !Edge->IsValid())
        {
            continue;
        }
        const UPCGNode* EdgeFrom = Edge->InputPin->Node;
        const UPCGNode* EdgeTo = Edge->OutputPin->Node;
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("from_node"), EdgeFrom ? EdgeFrom->GetName() : FString());
        E->SetStringField(TEXT("from_pin"), Edge->InputPin->Properties.Label.ToString());
        E->SetStringField(TEXT("to_node"), EdgeTo ? EdgeTo->GetName() : FString());
        E->SetStringField(TEXT("to_pin"), Edge->OutputPin->Properties.Label.ToString());
        EdgeArray.Add(MakeShared<FJsonValueObject>(E));
    }
#else
    for (const UPCGEdge* Edge : Graph->GetAllEdges())
    {
        if (!Edge || !Edge->IsValid())
        {
            continue;
        }
        const UPCGNode* EdgeFrom = Edge->GetInputNode();
        const UPCGNode* EdgeTo = Edge->GetOutputNode();
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("from_node"), EdgeFrom ? EdgeFrom->GetName() : FString());
        E->SetStringField(TEXT("from_pin"), Edge->GetInputPinLabel().ToString());
        E->SetStringField(TEXT("to_node"), EdgeTo ? EdgeTo->GetName() : FString());
        E->SetStringField(TEXT("to_pin"), Edge->GetOutputPinLabel().ToString());
        EdgeArray.Add(MakeShared<FJsonValueObject>(E));
    }
#endif

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetNumberField(TEXT("node_count"), Nodes.Num());
    Data->SetArrayField(TEXT("nodes"), NodeArray);
    Data->SetBoolField(TEXT("has_input_node"), InputNode != nullptr);
    Data->SetBoolField(TEXT("has_output_node"), OutputNode != nullptr);
    Data->SetStringField(TEXT("input_node_name"), InputNode ? InputNode->GetName() : FString());
    Data->SetStringField(TEXT("output_node_name"), OutputNode ? OutputNode->GetName() : FString());
    Data->SetArrayField(TEXT("edges"), EdgeArray);
    Data->SetNumberField(TEXT("edge_count"), EdgeArray.Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// spawn_pcg_volume -- A VOLUME NEEDS A BRUSH
//
// An APCGVolume spawned WITHOUT A BRUSH has `BrushBuilder` null and get_actor_bounds
// extent (0,0,0), and a UPCGComponent whose bounds are null generates ZERO instances
// without a single error or warning. So `extent` (HALF-size, same convention as
// spawn_box_volume and spawn_navmesh_bounds_volume) builds a real cube brush
// (UCubeBuilder -> UActorFactory::CreateBrushForVolumeActor -> FBSPOps::csgPrepMovingBrush),
// and -- always, with or without extent -- bounds/scale/geometry are RE-READ FROM THE
// ACTOR, never echoed. Without `extent` the volume has no brush and the reply says so
// (has_brush_geometry:false + a warning naming the silent 0-instance failure) instead of
// inventing a size nobody asked for.
//
// The scale is emitted too: a spawn path can leave a residual actor scale that silently
// multiplies the volume (spawn_box_volume goes through UEditorActorSubsystem::SpawnActorFromClass,
// this one does not), so it is read and, when an extent was requested, normalized so the
// half-extent means what it says.
// ---------------------------------------------------------------------------
// MCP:spawn_pcg_volume - Spawn an APCGVolume in the editor world with a REAL cube brush of half-extent extent and optionally assign a PCG graph, in one undo step. Over spawn_box_volume(volume_class='PCGVolume') it adds the graph (UPCGComponent::SetGraph), a rotation and a name (used only when free); add_pcg_component sets a graph on an existing actor. Bounds, scale and brush are RE-READ, never echoed. Without extent the volume is brush-less (kept for compatibility) and the reply warns: null bounds generate 0 instances in total silence. Not saved (save_level).
//   @retired spawn_box_volume - volume_class="PCGVolume", location, extent; then add_pcg_component(graph) on it, which reuses the volume's PCG component
//   @param location [string!] - World location [X, Y, Z] for the volume.
//   @param extent - Cube HALF-size [x, y, z] in unreal units, each > 0 (same convention as spawn_box_volume / spawn_navmesh_bounds_volume). STRONGLY RECOMMENDED: omitting it leaves the volume with extent (0,0,0) and the PCG component generates 0 instances with no error and no warning.
//   @param rotation - Optional [pitch, yaw, roll] rotation for the volume (default 0,0,0).
//   @param pcg_graph_path - Optional content path of a UPCGGraph to assign to the spawned volume's PCG component.
//   @param name - Optional explicit name (skipped if already used in the current level to avoid Rename-onto-occupied crash).
//   @returns { actor_name: string, actor_label: string, actor_class: string, has_pcg_component: bool, pcg_graph_path: string, graph_assigned: bool, location: [float], bounds_extent_actual: [float], bounds_origin_actual: [float], actor_scale: [float], has_brush_geometry: bool, brush_poly_count: int, brush_requested: bool, scale_normalized: bool, warning?: string, requested: {...} }

TSharedPtr<FJsonObject> HandleSpawnPCGVolume(const TSharedPtr<FJsonObject>& Params)
{
    // location is required, as its @param declares: a typed read, so the catalog publishes it as the array it is.
    const TArray<TSharedPtr<FJsonValue>>* LocationJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("location"), LocationJson))
    {
        return mcp_common::MissingParameterReply(TEXT("location"));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    // Optional half-extent. Validated up front so a malformed size is an ERROR and
    // never a silently brush-less volume.
    const bool bExtentRequested = Params->HasField(TEXT("extent"));
    FVector Extent = FVector::ZeroVector;
    if (bExtentRequested)
    {
        Extent = mcp_common::ReadJsonVector(Params, TEXT("extent"));
        if (Extent.X <= 0.0 || Extent.Y <= 0.0 || Extent.Z <= 0.0)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("extent components must all be > 0 (cube HALF-size [x,y,z]; got %f, %f, %f)"),
                Extent.X, Extent.Y, Extent.Z));
        }
    }

    // Optional graph to assign.
    FString GraphPath;
    UPCGGraph* Graph = nullptr;
    if (Params->TryGetStringField(TEXT("pcg_graph_path"), GraphPath) && !GraphPath.IsEmpty())
    {
        Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
        if (!Graph)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
        }
    }

    // Optional explicit name; otherwise let the engine assign a unique one.
    FString RequestedName;
    Params->TryGetStringField(TEXT("name"), RequestedName);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SpawnPCGVolume", "Spawn PCG Volume"));

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    if (!RequestedName.IsEmpty())
    {
        // Avoid the Rename-onto-occupied-name crash: only set Name if it is free.
        // The class filter must be nullptr, NOT AActor::StaticClass(): the engine's own check is
        // StaticFindObjectFast(nullptr, LevelToSpawnIn, Name) (LevelActor.cpp), so a name held
        // by a NON-actor UObject passes this guard and still reaches the UE_LOG(Fatal) at
        // LevelActor.cpp. Neither call excludes garbage (UObjectGlobals.cpp), which is the
        // point: a deleted actor kept alive by the undo buffer is exactly the case that crashes.
        const FName DesiredName(*RequestedName);
        if (StaticFindObjectFast(nullptr, World->GetCurrentLevel(), DesiredName) == nullptr)
        {
            SpawnParams.Name = DesiredName;
        }
    }
    // Belt and braces: Error + null instead of a Fatal that kills the editor.
    SpawnParams.NameMode = FActorSpawnParameters::ESpawnActorNameMode::Required_ErrorAndReturnNull;

    APCGVolume* Volume = World->SpawnActor<APCGVolume>(APCGVolume::StaticClass(), Location, Rotation, SpawnParams);
    if (!Volume)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn APCGVolume in the editor world"));
    }

    if (!RequestedName.IsEmpty())
    {
        Volume->SetActorLabel(RequestedName);
    }

    // -- Build the REAL cube brush BEFORE the graph is assigned: the brush build
    // runs PreEditChange/PostEditChange on the actor, and the PCG component should
    // not receive a graph while the actor it samples still has null bounds.
    bool bScaleNormalized = false;
    if (bExtentRequested)
    {
        // A non-identity actor scale multiplies the brush: (25,25,10) turns a
        // 600 m volume into 15 km. Normalize so `extent` means
        // exactly what the parameter says it means, and report that we did.
        if (!Volume->GetActorScale3D().Equals(FVector::OneVector))
        {
            Volume->SetActorScale3D(FVector::OneVector);
            bScaleNormalized = true;
        }
        mcp_common::BuildCubeBrushForVolume(Volume, Extent);
        // Re-assert the requested location (the brush build runs PostEditChange).
        Volume->SetActorLocation(Location);
    }

    bool bGraphAssigned = false;
    if (Graph)
    {
        if (UPCGComponent* Comp = Volume->PCGComponent)
        {
            // NOTE: UPCGComponent::Graph is protected -- SetGraph/GetGraph are the
            // only accessors (no get_editor_property / set_editor_property route).
            Comp->SetGraph(Graph);
            bGraphAssigned = (Comp->GetGraph() == Graph);
        }
    }

    // ---- RE-READ everything that decides whether this volume can generate. ----
    FVector BoundsOrigin(0, 0, 0), BoundsExtent(0, 0, 0);
    Volume->GetActorBounds(/*bOnlyCollidingComponents*/ false, BoundsOrigin, BoundsExtent);
    const FVector ActorScale = Volume->GetActorScale3D();

    const int32 BrushPolyCount =
        (Volume->Brush != nullptr && Volume->Brush->Polys != nullptr)
            ? Volume->Brush->Polys->Element.Num()
            : 0;
    const bool bHasBrushGeometry = BrushPolyCount > 0;
    const bool bBoundsNonNull = BoundsExtent.X > 0.0 && BoundsExtent.Y > 0.0 && BoundsExtent.Z > 0.0;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Volume->GetName());
    Data->SetStringField(TEXT("actor_label"), Volume->GetActorLabel());
    Data->SetStringField(TEXT("actor_class"), Volume->GetClass()->GetName());
    Data->SetBoolField(TEXT("has_pcg_component"), Volume->PCGComponent != nullptr);
    if (Graph)
    {
        Data->SetStringField(TEXT("pcg_graph_path"), Graph->GetPathName());
        Data->SetBoolField(TEXT("graph_assigned"), bGraphAssigned);
    }
    // This field IS the requested location (kept for compatibility). The
    // measured one is bounds_origin_actual.
    Data->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Location));

    auto AddVectorField = [&Data](const TCHAR* FieldName, const FVector& V)
    {
        Data->SetArrayField(FieldName, mcp_common::VectorToJsonArray(V));
    };
    AddVectorField(TEXT("bounds_extent_actual"), BoundsExtent);
    AddVectorField(TEXT("bounds_origin_actual"), BoundsOrigin);
    AddVectorField(TEXT("actor_scale"), ActorScale);
    Data->SetBoolField(TEXT("has_brush_geometry"), bHasBrushGeometry);
    Data->SetNumberField(TEXT("brush_poly_count"), BrushPolyCount);
    Data->SetBoolField(TEXT("brush_requested"), bExtentRequested);
    Data->SetBoolField(TEXT("scale_normalized"), bScaleNormalized);

    // Honest outcome: an extent that was asked for must be measurable in the actor,
    // and a graph that was asked for must be readable back off the component.
    FString Error;
    if (bExtentRequested && !(bHasBrushGeometry && bBoundsNonNull))
    {
        const FString BrushState = bHasBrushGeometry ? TEXT("true") : TEXT("false");
        Error = FString::Printf(
            TEXT("extent was requested but the volume has no usable geometry (has_brush_geometry=%s, bounds_extent=[%f, %f, %f]). A UPCGComponent with null bounds generates 0 instances with no error and no warning."),
            *BrushState, BoundsExtent.X, BoundsExtent.Y, BoundsExtent.Z);
    }
    else if (Graph && !bGraphAssigned)
    {
        Error = FString::Printf(TEXT("graph %s did not read back off the volume's UPCGComponent"), *Graph->GetPathName());
    }

    if (!bExtentRequested)
    {
        Data->SetStringField(TEXT("warning"),
            TEXT("No 'extent' was supplied: the APCGVolume was spawned WITHOUT a brush (bounds extent (0,0,0)). "
                 "A UPCGComponent with null bounds generates 0 instances without a single error or warning. "
                 "Pass 'extent' (cube HALF-size [x,y,z]) to build a real volume."));
    }
    else if (!Error.IsEmpty())
    {
        Data->SetStringField(TEXT("warning"), Error);
    }

    mcp_common::AddRequestedEcho(Data, Params,
        { TEXT("location"), TEXT("extent"), TEXT("rotation"), TEXT("pcg_graph_path"), TEXT("name") });

    return mcp_common::MakeOutcomeReply(Data, Error.IsEmpty(), Error);
}

// ============================================================================
// The UPCGComponent as generation engine on an actor
// ============================================================================

namespace
{
    // The editor-world actor named ActorName and its UPCGComponent (an added one or an APCGVolume's own; null
    // when it has none). Null, with OutError set, when there is no editor world or no such actor.
    AActor* PCG_FindComponentActor(const FString& ActorName, UPCGComponent*& OutComp, TSharedPtr<FJsonObject>& OutError)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
        {
            OutError = NoWorld;
            return nullptr;
        }
        AActor* Actor = mcp_common::FindActorByName(World, ActorName);
        OutComp = Actor ? Actor->FindComponentByClass<UPCGComponent>() : nullptr;
        if (!Actor)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));
        }
        return Actor;
    }

    // PCG_FindComponentActor for the required "actor_name" param.
    AActor* PCG_ResolveComponentActor(const TSharedPtr<FJsonObject>& Params, FString& OutActorName, UPCGComponent*& OutComp,
        TSharedPtr<FJsonObject>& OutError)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
        {
            OutError = mcp_common::MakeErrorReply(TEXT("Missing 'actor_name'"));
            return nullptr;
        }
        return PCG_FindComponentActor(OutActorName, OutComp, OutError);
    }
}

// MCP:add_pcg_component - Attach a PCG component (the procedural generation engine) to an editor-world actor, found by name or label, and optionally point it at a PCG graph -- what turns a plain actor or volume into something that scatters meshes and points by rule. Over add_component_to_actor it assigns the graph via UPCGComponent::SetGraph, which set_properties cannot (the graph lives in a read-only GraphInstance). Idempotent: an actor that already has one (an APCGVolume does) gets it back, graph re-assigned. It generates within the actor's BOUNDS, so a volume with no brush geometry yields zero instances with no error -- check get_actor_bounds first. Then generate_pcg_component and get_pcg_component_info. Not saved.
//   @param actor_name - Display label or internal name of the target editor-world actor (required)
//   @param pcg_graph_path - Optional /Game/... path of a UPCGGraph to assign to the component
//   @returns { actor_name, component_name, already_existed, graph_path, graph_assigned }
TSharedPtr<FJsonObject> HandleAddPCGComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    UPCGComponent* Comp = nullptr;
    TSharedPtr<FJsonObject> Error;
    AActor* Actor = PCG_ResolveComponentActor(Params, ActorName, Comp, Error);
    if (!Actor)
        return Error;

    FString GraphPath;
    UPCGGraph* Graph = nullptr;
    if (Params->TryGetStringField(TEXT("pcg_graph_path"), GraphPath) && !GraphPath.IsEmpty())
    {
        Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
        if (!Graph)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    const bool bAlreadyExisted = (Comp != nullptr);
    if (!Comp)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddPCGComponent", "Add PCG Component"));
        Comp = NewObject<UPCGComponent>(Actor, UPCGComponent::StaticClass(), NAME_None, RF_Transactional);
        Actor->AddInstanceComponent(Comp);
        Comp->RegisterComponent();
        Actor->Modify();
    }

    bool bGraphAssigned = false;
    if (Graph && Comp)
    {
        Comp->SetGraph(Graph);
        bGraphAssigned = (Comp->GetGraph() == Graph);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetStringField(TEXT("component_name"), Comp ? Comp->GetName() : TEXT(""));
    Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    if (Graph)
    {
        Data->SetStringField(TEXT("graph_path"), Graph->GetPathName());
        Data->SetBoolField(TEXT("graph_assigned"), bGraphAssigned);
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:generate_pcg_component - Triggers PCG generation on an actor's UPCGComponent. Generation is ASYNCHRONOUS (scheduled on the PCG subsystem, resolved over subsequent editor ticks) — this returns the scheduled task id, NOT a finished result. Poll get_pcg_component_info / get_pcg_generated_data for completion (honest).
//   @param actor_name - Actor holding the UPCGComponent (required)
//   @param force - Optional; force regeneration even if already generated (default true)
//   @returns { actor_name, scheduled, task_id, is_generating, generated, note }
TSharedPtr<FJsonObject> HandleGeneratePCGComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    UPCGComponent* Comp = nullptr;
    TSharedPtr<FJsonObject> Error;
    AActor* Actor = PCG_ResolveComponentActor(Params, ActorName, Comp, Error);
    if (!Actor)
        return Error;
    if (!Comp)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no UPCGComponent (call add_pcg_component first)"), *ActorName));
    if (!Comp->GetGraph())
        return mcp_common::MakeErrorReply(TEXT("The UPCGComponent has no graph assigned (add_pcg_component with pcg_graph_path)"));

    bool bForce = true;
    Params->TryGetBoolField(TEXT("force"), bForce);

    const FPCGTaskId TaskId = Comp->GenerateLocalGetTaskId(bForce);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetBoolField(TEXT("scheduled"), TaskId != InvalidPCGTaskId);
    Data->SetNumberField(TEXT("task_id"), static_cast<double>(TaskId));
    Data->SetBoolField(TEXT("is_generating"), Comp->IsGenerating());
    Data->SetBoolField(TEXT("generated"), Comp->bGenerated);
    Data->SetStringField(TEXT("note"), TEXT("PCG generation is asynchronous; poll get_pcg_component_info / get_pcg_generated_data for completion."));
    return mcp_common::MakeOkReply(Data);
}

// MCP:cleanup_pcg_component - Wipe a PCG component's generated output synchronously -- and the step you must not skip after editing a graph: a plain generate_pcg_component with force reuses the CACHED result, so the world keeps the STALE meshes and settings while every flag reads generated:true. Call this with remove_components=true FIRST, then generate, then verify by re-reading the actor's spawned components rather than the generated flag. Also the way to clear procedural content before moving or resizing the volume.
//   @param actor_name - Actor holding the UPCGComponent (required)
//   @param remove_components - Optional; also remove spawned managed components (default false)
//   @returns { actor_name, cleaned, generated }
TSharedPtr<FJsonObject> HandleCleanupPCGComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    UPCGComponent* Comp = nullptr;
    TSharedPtr<FJsonObject> Error;
    AActor* Actor = PCG_ResolveComponentActor(Params, ActorName, Comp, Error);
    if (!Actor)
        return Error;
    if (!Comp)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no UPCGComponent"), *ActorName));

    bool bRemoveComponents = false;
    Params->TryGetBoolField(TEXT("remove_components"), bRemoveComponents);
    Comp->CleanupLocalImmediate(bRemoveComponents);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetBoolField(TEXT("cleaned"), true);
    Data->SetBoolField(TEXT("generated"), Comp->bGenerated);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_pcg_component_properties - Write UPROPERTYs (Seed, bActivated, GenerationTrigger, ...) on the PCG component of an editor-world actor found by label or name, reading each stored value back. Over set_properties: no component path and no edit gate, but no PostEditChange is sent, so the component is not refreshed -- run generate_pcg_component. Nothing is saved.
//   @retired set_properties - object_path="<actor>.PCGComponent", properties with the engine names
//   @param actor_name - Actor holding the UPCGComponent (required)
//   @param properties - JSON object { PropertyName: value, ... } applied to the component
//   @returns { actor_name, set:[name], failed:[{name, error}], applied:{name: stored_value_text} }
TSharedPtr<FJsonObject> HandleSetPCGComponentProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
        return mcp_common::MakeErrorReply(TEXT("Missing 'properties' object"));

    UPCGComponent* Comp = nullptr;
    TSharedPtr<FJsonObject> Error;
    AActor* Actor = PCG_FindComponentActor(ActorName, Comp, Error);
    if (!Actor)
        return Error;
    if (!Comp)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor '%s' has no UPCGComponent"), *ActorName));

    // Same undo-correctness pattern as the sibling HandleSetPCGNodeProperties
    // below: Modify() before the mutation loop, inside an open transaction.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPCGComponentProperties", "Set PCG Component Properties (MCP)"));
    Comp->Modify();

    const ClassReflectionUtils::FPropertyWrites Writes = ClassReflectionUtils::WriteProperties(Comp, **PropsObj, &mcp_common::SetObjectProperty);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetArrayField(TEXT("set"), Writes.Set);
    Data->SetArrayField(TEXT("failed"), Writes.Failed);
    Data->SetObjectField(TEXT("applied"), Writes.Applied);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_pcg_component_info - Read an actor's PCG component: which graph it runs, its seed, whether it is generating or already generated, activated, partitioned, and its generation trigger. Read-only. Use it to find which graph is driving an actor and to confirm a seed for deterministic scatter -- but NOT as proof the world matches the graph: generated:true is satisfied by a cached result, so after editing a graph the flag says done while the spawned meshes are stale. The real check is the actor's spawned components (get_pcg_generated_data / get_components_by_class), after cleanup_pcg_component + generate_pcg_component.
//   @param actor_name - Actor holding the UPCGComponent (required)
//   @returns { actor_name, has_component, graph_path, is_generating, generated, seed, activated, is_partitioned, generation_trigger }
TSharedPtr<FJsonObject> HandleGetPCGComponentInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    UPCGComponent* Comp = nullptr;
    TSharedPtr<FJsonObject> Error;
    AActor* Actor = PCG_ResolveComponentActor(Params, ActorName, Comp, Error);
    if (!Actor)
        return Error;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetBoolField(TEXT("has_component"), Comp != nullptr);
    if (Comp)
    {
        UPCGGraph* Graph = Comp->GetGraph();
        Data->SetStringField(TEXT("graph_path"), Graph ? Graph->GetPathName() : TEXT(""));
        Data->SetBoolField(TEXT("is_generating"), Comp->IsGenerating());
        Data->SetBoolField(TEXT("generated"), Comp->bGenerated);
        Data->SetNumberField(TEXT("seed"), Comp->Seed);
        Data->SetBoolField(TEXT("activated"), Comp->bActivated);
        Data->SetBoolField(TEXT("is_partitioned"), Comp->bIsComponentPartitioned);
        Data->SetStringField(TEXT("generation_trigger"),
            StaticEnum<EPCGComponentGenerationTrigger>()->GetNameStringByValue(static_cast<int64>(Comp->GenerationTrigger)));
    }
    return mcp_common::MakeOkReply(Data);
}

// ============================================================================
// Graph parameters, node-type discovery, generated-data inspection
// ============================================================================

namespace
{
    const TCHAR* PBTypeToString(EPropertyBagPropertyType T)
    {
        switch (T)
        {
        case EPropertyBagPropertyType::Bool:   return TEXT("bool");
        case EPropertyBagPropertyType::Int32:  return TEXT("int32");
        case EPropertyBagPropertyType::Int64:  return TEXT("int64");
        case EPropertyBagPropertyType::Float:  return TEXT("float");
        case EPropertyBagPropertyType::Double: return TEXT("double");
        case EPropertyBagPropertyType::Name:   return TEXT("name");
        case EPropertyBagPropertyType::String: return TEXT("string");
        default:                               return TEXT("other");
        }
    }

    bool PBTypeFromString(const FString& S, EPropertyBagPropertyType& Out)
    {
        const FString L = S.ToLower();
        if (L == TEXT("bool"))                                             { Out = EPropertyBagPropertyType::Bool;   return true; }
        if (L == TEXT("int") || L == TEXT("int32") || L == TEXT("integer")) { Out = EPropertyBagPropertyType::Int32;  return true; }
        if (L == TEXT("float"))                                            { Out = EPropertyBagPropertyType::Float;  return true; }
        if (L == TEXT("double"))                                           { Out = EPropertyBagPropertyType::Double; return true; }
        if (L == TEXT("string"))                                          { Out = EPropertyBagPropertyType::String; return true; }
        return false;
    }

    TSharedPtr<FJsonValue> PBReadValue(const FInstancedPropertyBag& Bag, const FName Name, EPropertyBagPropertyType T)
    {
        switch (T)
        {
        case EPropertyBagPropertyType::Bool:   { auto V = Bag.GetValueBool(Name);   if (V.IsValid()) return MakeShared<FJsonValueBoolean>(V.GetValue()); break; }
        case EPropertyBagPropertyType::Int32:  { auto V = Bag.GetValueInt32(Name);  if (V.IsValid()) return MakeShared<FJsonValueNumber>(V.GetValue()); break; }
        case EPropertyBagPropertyType::Int64:  { auto V = Bag.GetValueInt64(Name);  if (V.IsValid()) return MakeShared<FJsonValueNumber>(static_cast<double>(V.GetValue())); break; }
        case EPropertyBagPropertyType::Float:  { auto V = Bag.GetValueFloat(Name);  if (V.IsValid()) return MakeShared<FJsonValueNumber>(V.GetValue()); break; }
        case EPropertyBagPropertyType::Double: { auto V = Bag.GetValueDouble(Name); if (V.IsValid()) return MakeShared<FJsonValueNumber>(V.GetValue()); break; }
        case EPropertyBagPropertyType::String: { auto V = Bag.GetValueString(Name); if (V.IsValid()) return MakeShared<FJsonValueString>(V.GetValue()); break; }
        case EPropertyBagPropertyType::Name:   { auto V = Bag.GetValueName(Name);   if (V.IsValid()) return MakeShared<FJsonValueString>(V.GetValue().ToString()); break; }
        default: break;
        }
        return MakeShared<FJsonValueNull>();
    }
}

// Graph parameters, node-type discovery, generated-data inspection.

// MCP:list_pcg_node_types - Enumerate every PCG node type available in this build (all concrete PCGSettings subclasses, including plugin- and project-added ones), with class names and paths. Run it before add_pcg_node so you pass a class that exists instead of guessing a name from documentation that may not match this engine version. Read-only reflection.
//   @returns { base_class, count, types:[{name, path}] }
TSharedPtr<FJsonObject> HandleListPCGNodeTypes(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>> Types = mcp_common::ConcreteSubclassesToJson(UPCGSettings::StaticClass());

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("base_class"), TEXT("PCGSettings"));
    Data->SetNumberField(TEXT("count"), Types.Num());
    Data->SetArrayField(TEXT("types"), Types);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_pcg_graph_parameters - List a PCG graph's user parameters with name, type and current value -- and the read that decides whether a THIRD-PARTY graph can be reused without editing it. An empty list means nothing is exposed: meshes, densities and exclusions are welded inside, so the only way to change them is to edit the asset, which is not an option for a read-only library graph. Read-only. Note it accepts a PCGGraph asset, not a PCGGraphInstance -- overriding a parameter on an instance has no headless route and must be ticked in the details panel.
//   @param pcg_graph_path - /Game/... path to the UPCGGraph (required)
//   @returns { pcg_graph_path, parameter_count, parameters:[{name, type, value}] }
TSharedPtr<FJsonObject> HandleGetPCGGraphParameters(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }
    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));

    const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
    const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
    TArray<TSharedPtr<FJsonValue>> ParamArr;
    if (Bag && BagStruct)
    {
        for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
        {
            TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
            P->SetStringField(TEXT("name"), Desc.Name.ToString());
            P->SetStringField(TEXT("type"), PBTypeToString(Desc.ValueType));
            P->SetField(TEXT("value"), PBReadValue(*Bag, Desc.Name, Desc.ValueType));
            ParamArr.Add(MakeShared<FJsonValueObject>(P));
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pcg_graph_path"), GraphPath);
    Data->SetNumberField(TEXT("parameter_count"), ParamArr.Num());
    Data->SetArrayField(TEXT("parameters"), ParamArr);
    return mcp_common::MakeOkReply(Data);
}

// MCP:add_pcg_graph_parameter - Expose a tunable on a PCG graph (bool / int / float / double / string) so density, radius or a toggle can be driven from outside instead of being welded into a node. Idempotent (already_existed when the name is taken). Only worth doing on graphs you own -- adding one to a library graph means editing that asset. Set a value with set_pcg_graph_parameter and list them with get_pcg_graph_parameters; inverse: remove_pcg_graph_parameter.
//   @param pcg_graph_path - /Game/... path to the UPCGGraph (required)
//   @param name - Parameter name (required)
//   @param type - "bool" | "int" | "float" | "double" | "string" (required)
//   @returns { pcg_graph_path, name, type, added, already_existed, parameter_count }
TSharedPtr<FJsonObject> HandleAddPCGGraphParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath, Name, TypeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("type"), TypeStr)) { return Missing; }

    EPropertyBagPropertyType Type;
    if (!PBTypeFromString(TypeStr, Type))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unsupported type '%s' (bool|int|float|double|string)"), *TypeStr));

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));

    const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
    const bool bExisted = Bag && Bag->FindPropertyDescByName(FName(*Name)) != nullptr;
    if (!bExisted)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddPCGGraphParameter", "Add PCG Graph Parameter"));
        FPropertyBagPropertyDesc Desc(FName(*Name), Type);
        Graph->AddUserParameters({ Desc });
        Graph->Modify();
        Graph->MarkPackageDirty();
    }

    const FInstancedPropertyBag* NewBag = Graph->GetUserParametersStruct();
    const UPropertyBag* NewStruct = NewBag ? NewBag->GetPropertyBagStruct() : nullptr;
    const int32 Count = NewStruct ? NewStruct->GetPropertyDescs().Num() : 0;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pcg_graph_path"), GraphPath);
    Data->SetStringField(TEXT("name"), Name);
    Data->SetStringField(TEXT("type"), PBTypeToString(Type));
    Data->SetBoolField(TEXT("added"), !bExisted);
    Data->SetBoolField(TEXT("already_existed"), bExisted);
    Data->SetNumberField(TEXT("parameter_count"), Count);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_pcg_graph_parameter - Write the value of an existing PCG graph user parameter (bool / int / float / double) and read the stored value back, so the reply is its own oracle -- how you tune scatter density or a rule threshold without opening the graph. The parameter must already exist (add_pcg_graph_parameter) and the value type must match. IMPORTANT: it accepts a PCGGraph ASSET only and answers "PCG graph not found" for a PCGGraphInstance -- per-instance overrides have no headless route and must be ticked in the details panel. Changing a parameter does not update the world: cleanup_pcg_component then generate_pcg_component, or the cached output is reused.
//   @param pcg_graph_path - /Game/... path to the UPCGGraph (required)
//   @param name - Parameter name (must already exist; add with add_pcg_graph_parameter) (required)
//   @param value - New value; its type must match the parameter's declared type (required)
//   @returns { pcg_graph_path, name, type, set, value }
TSharedPtr<FJsonObject> HandleSetPCGGraphParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }
    const TSharedPtr<FJsonValue> Val = Params->Values.FindRef(TEXT("value"));
    if (!Val.IsValid())
        return mcp_common::MakeErrorReply(TEXT("Missing 'value'"));

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));

    const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
    const FPropertyBagPropertyDesc* Desc = Bag ? Bag->FindPropertyDescByName(FName(*Name)) : nullptr;
    if (!Desc)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parameter '%s' not found (add it with add_pcg_graph_parameter)"), *Name));
    const EPropertyBagPropertyType Type = Desc->ValueType;
    const FName ParamName(*Name);

    bool bSet = false;
    Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& MBag)
    {
        switch (Type)
        {
        case EPropertyBagPropertyType::Bool:   bSet = MBag.SetValueBool(ParamName, Val->AsBool()) == EPropertyBagResult::Success; break;
        case EPropertyBagPropertyType::Int32:  bSet = MBag.SetValueInt32(ParamName, static_cast<int32>(Val->AsNumber())) == EPropertyBagResult::Success; break;
        case EPropertyBagPropertyType::Int64:  bSet = MBag.SetValueInt64(ParamName, static_cast<int64>(Val->AsNumber())) == EPropertyBagResult::Success; break;
        case EPropertyBagPropertyType::Float:  bSet = MBag.SetValueFloat(ParamName, static_cast<float>(Val->AsNumber())) == EPropertyBagResult::Success; break;
        case EPropertyBagPropertyType::Double: bSet = MBag.SetValueDouble(ParamName, Val->AsNumber()) == EPropertyBagResult::Success; break;
        default: break;
        }
    });
    Graph->Modify();
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pcg_graph_path"), GraphPath);
    Data->SetStringField(TEXT("name"), Name);
    Data->SetStringField(TEXT("type"), PBTypeToString(Type));
    Data->SetBoolField(TEXT("set"), bSet);
    Data->SetField(TEXT("value"), PBReadValue(*Graph->GetUserParametersStruct(), ParamName, Type));
    return mcp_common::MakeOkReply(Data);
}

// MCP:remove_pcg_graph_parameter - Remove a user parameter from a PCG graph asset, in one undo step, and delete the Get Parameter nodes that read it, as the PCG editor does when a parameter goes (getter_nodes_removed); the graph is marked dirty, not saved, as add_pcg_graph_parameter leaves it. Inverse of add_pcg_graph_parameter. A name the graph has no parameter for is an honest no-op (removed=false); a graph that does not load is an error (a PCGGraphInstance is not accepted, as for set_pcg_graph_parameter). The reply re-reads the parameters (parameters). Generated output is not refreshed: cleanup_pcg_component then generate_pcg_component. Read: get_pcg_graph_parameters.
//   @param pcg_graph_path - /Game/... path to the UPCGGraph (required)
//   @param name - Parameter name to remove (required)
//   @returns { pcg_graph_path, name, removed, getter_nodes_removed, parameter_count, parameters: [string], note? }
TSharedPtr<FJsonObject> HandleRemovePCGGraphParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));

    const FName ParamName(*Name);
    const auto ParameterNames = [Graph]()
    {
        TArray<FString> Names;
        const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
        const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
        if (BagStruct)
        {
            for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
            {
                Names.Add(Desc.Name.ToString());
            }
        }
        return Names;
    };
    const auto CountGetters = [Graph, &ParamName]()
    {
        int32 Count = 0;
        for (const UPCGNode* Node : Graph->GetNodes())
        {
            const UPCGUserParameterGetSettings* Getter = Node ? Cast<UPCGUserParameterGetSettings>(Node->GetSettings()) : nullptr;
            if (Getter && Getter->PropertyName == ParamName)
            {
                ++Count;
            }
        }
        return Count;
    };

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pcg_graph_path"), GraphPath);
    Data->SetStringField(TEXT("name"), Name);
    const FInstancedPropertyBag* Bag = Graph->GetUserParametersStruct();
    if (!Bag || !Bag->FindPropertyDescByName(ParamName))
    {
        const TArray<FString> Names = ParameterNames();
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetNumberField(TEXT("getter_nodes_removed"), 0);
        Data->SetNumberField(TEXT("parameter_count"), Names.Num());
        Data->SetArrayField(TEXT("parameters"), mcp_common::StringsToJson(Names));
        Data->SetStringField(TEXT("note"), TEXT("the graph has no parameter of that name; nothing changed"));
        return mcp_common::MakeOkReply(Data);
    }

    const int32 GettersBefore = CountGetters();
    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemovePCGGraphParameter", "Remove PCG Graph Parameter"));
    Graph->Modify();
    Graph->UpdateUserParametersStruct([&ParamName](FInstancedPropertyBag& MutableBag)
    {
        MutableBag.RemovePropertyByName(ParamName);
    });
    // The event the PCG editor raises when a parameter goes: it deletes the Get Parameter nodes left without one.
    Graph->OnGraphParametersChanged(GettersBefore > 0 ? EPCGGraphParameterEvent::RemovedUsed : EPCGGraphParameterEvent::RemovedUnused, ParamName);
    Graph->MarkPackageDirty();

    const FInstancedPropertyBag* After = Graph->GetUserParametersStruct();
    if (After && After->FindPropertyDescByName(ParamName))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parameter '%s' is still on the graph after the removal"), *Name));
    }
    const TArray<FString> Names = ParameterNames();
    Data->SetBoolField(TEXT("removed"), true);
    Data->SetNumberField(TEXT("getter_nodes_removed"), GettersBefore - CountGetters());
    Data->SetNumberField(TEXT("parameter_count"), Names.Num());
    Data->SetArrayField(TEXT("parameters"), mcp_common::StringsToJson(Names));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_pcg_generated_data - Reports the generated output of an actor's UPCGComponent: per output item, its pin, tags and data class. Read-only; empty until a generation completes (generation is asynchronous).
//   @param actor_name - Actor holding the UPCGComponent (required)
//   @returns { actor_name, has_component, output_data_count, data:[{index, pin, data_class, tags:[...]}], note? }
TSharedPtr<FJsonObject> HandleGetPCGGeneratedData(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    UPCGComponent* Comp = nullptr;
    TSharedPtr<FJsonObject> Error;
    AActor* Actor = PCG_ResolveComponentActor(Params, ActorName, Comp, Error);
    if (!Actor)
        return Error;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Actor->GetName());
    Data->SetBoolField(TEXT("has_component"), Comp != nullptr);

    TArray<TSharedPtr<FJsonValue>> Arr;
    if (Comp)
    {
        const TArray<FPCGTaggedData>& Inputs = Comp->GetGeneratedGraphOutput().GetAllInputs();
        for (int32 i = 0; i < Inputs.Num(); ++i)
        {
            const FPCGTaggedData& TD = Inputs[i];
            TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
            D->SetNumberField(TEXT("index"), i);
            D->SetStringField(TEXT("pin"), TD.Pin.ToString());
            const UPCGData* PData = TD.Data.Get();
            D->SetStringField(TEXT("data_class"), PData ? PData->GetClass()->GetName() : TEXT("None"));
            TArray<TSharedPtr<FJsonValue>> Tags;
            for (const FString& Tag : TD.Tags) { Tags.Add(MakeShared<FJsonValueString>(Tag)); }
            D->SetArrayField(TEXT("tags"), Tags);
            Arr.Add(MakeShared<FJsonValueObject>(D));
        }
    }
    Data->SetNumberField(TEXT("output_data_count"), Arr.Num());
    Data->SetArrayField(TEXT("data"), Arr);
    if (Comp && Arr.Num() == 0)
        Data->SetStringField(TEXT("note"), TEXT("No generated output yet -- PCG generation is asynchronous; call generate_pcg_component and poll after it completes."));
    return mcp_common::MakeOkReply(Data);
}

// MCP:add_pcg_subgraph_node - Embed one PCG graph inside another as a Subgraph node -- how you reuse a scatter or filter recipe across biomes instead of duplicating nodes, and how you consume a library graph without editing it. Check first with get_pcg_graph_info that the target graph actually USES its input node (a graph with no edges from its default input cannot be fed filtered data and will ignore whatever you wire in), and with get_pcg_graph_parameters whether it exposes anything tunable.
//   @param pcg_graph_path - /Game/... path to the host UPCGGraph (required)
//   @param subgraph_path - /Game/... path to the UPCGGraph to embed as a subgraph (required)
//   @returns { graph_name, asset_path, node_name, node_title, subgraph_path, subgraph_set, node_count, saved }
TSharedPtr<FJsonObject> HandleAddPCGSubgraphNode(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath, SubgraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pcg_graph_path"), GraphPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("subgraph_path"), SubgraphPath)) { return Missing; }

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    UPCGGraph* Subgraph = mcp_common::ResolveAsset<UPCGGraph>(SubgraphPath);
    if (!Subgraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Subgraph not found: %s"), *SubgraphPath));
    if (Subgraph == Graph)
        return mcp_common::MakeErrorReply(TEXT("A graph cannot embed itself as a subgraph"));

    UClass* SettingsClass = ResolvePCGSettingsClass(TEXT("PCGSubgraphSettings"));
    if (!SettingsClass)
        return mcp_common::MakeErrorReply(TEXT("Could not resolve UPCGSubgraphSettings"));

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddPCGSubgraphNode", "Add PCG Subgraph Node"));
    Graph->Modify();

    UPCGSettings* OutSettings = nullptr;
    UPCGNode* NewNode = Graph->AddNodeOfType(SettingsClass, OutSettings);
    if (!NewNode || !OutSettings)
        return mcp_common::MakeErrorReply(TEXT("AddNodeOfType returned null for PCGSubgraphSettings"));

    bool bSubgraphSet = false;
    if (UPCGBaseSubgraphSettings* SubSettings = Cast<UPCGBaseSubgraphSettings>(OutSettings))
    {
        SubSettings->SetSubgraph(Subgraph);
        bSubgraphSet = (SubSettings->GetSubgraph() == Subgraph);
    }

    FString SaveError;
    const bool bSaved = PCG_SaveGraph(Graph, SaveError);

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("node_name"), NewNode->GetName());
    Data->SetStringField(TEXT("node_title"), NewNode->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
    Data->SetStringField(TEXT("subgraph_path"), Subgraph->GetPathName());
    Data->SetBoolField(TEXT("subgraph_set"), bSubgraphSet);
    Data->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
    if (!bSaved && !SaveError.IsEmpty())
        Data->SetStringField(TEXT("save_error"), SaveError);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_pcg_subgraph_node"));
}

// ---------------------------------------------------------------------------
// connect_pcg_nodes
// Adds a directed edge from an upstream node's output pin to a downstream node's
// input pin via UPCGGraph::AddEdge (PCG_API; the underlying UPCGPin::AddEdgeTo is
// idempotent so it never duplicates an edge). AddEdge ignores its own failure path
// and always returns the "To" node, so success is verified independently by
// scanning GetAllEdges(). Node names come from get_pcg_graph_info; the implicit
// endpoints accept the tokens "Input"/"Output". Pin labels default to the node's
// "Out"/"In" pin (else its first pin) and the resolved labels are echoed back.
// ---------------------------------------------------------------------------
// MCP:connect_pcg_nodes - Connects two PCG nodes by adding a directed edge from an output pin to an input pin.  Node names come from get_pcg_graph_info; the implicit graph endpoints are addressable as "Input"/"Output". from_pin/to_pin default to the node's "Out"/"In" pin (else its first pin); the resolved labels are echoed in the response. Idempotent: an existing edge reports already_connected without error. The graph is re-saved after a new edge is added.
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example" or fully-qualified "/Game/PCG/PCG_Example.PCG_Example").
//   @param from_node - Upstream node: a node_name from get_pcg_graph_info, or the token "Input" for the graph's implicit input node.
//   @param to_node - Downstream node: a node_name, or the token "Output" for the graph's implicit output node.
//   @param from_pin - Optional output-pin label on from_node (default the node's "Out" pin, else its first output pin).
//   @param to_pin - Optional input-pin label on to_node (default the node's "In" pin, else its first input pin).
//   @returns { graph_name: string, asset_path: string, from_node: string, from_pin: string, to_node: string, to_pin: string, already_connected: bool, connected: bool, edge_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleConnectPCGNodes(const TSharedPtr<FJsonObject>& Params)
{
    UPCGGraph* Graph = nullptr;
    UPCGNode* From = nullptr;
    UPCGNode* To = nullptr;
    FName FromLabel = NAME_None;
    FName ToLabel = NAME_None;
    if (TSharedPtr<FJsonObject> Error = PCG_ResolveEdgeEnds(Params, Graph, From, FromLabel, To, ToLabel))
    {
        return Error;
    }

    const bool bAlready = PCGEdgeExists(Graph, From, FromLabel, To, ToLabel);
    bool bSaved = false;
    FString SaveError;
    if (!bAlready)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ConnectPCGNodes", "Connect PCG Nodes"));
        Graph->Modify();
        Graph->AddEdge(From, FromLabel, To, ToLabel);

        // AddEdge always returns the "To" node regardless of success; verify the
        // edge actually exists (it will not if the pins are type-incompatible).
        if (!PCGEdgeExists(Graph, From, FromLabel, To, ToLabel))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to connect %s.%s -> %s.%s (the pins may be type-incompatible)"),
                    *From->GetName(), *FromLabel.ToString(), *To->GetName(), *ToLabel.ToString()));
        }

        bSaved = PCG_SaveGraph(Graph, SaveError);
    }

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("from_node"), From->GetName());
    Data->SetStringField(TEXT("from_pin"), FromLabel.ToString());
    Data->SetStringField(TEXT("to_node"), To->GetName());
    Data->SetStringField(TEXT("to_pin"), ToLabel.ToString());
    Data->SetBoolField(TEXT("already_connected"), bAlready);
    Data->SetBoolField(TEXT("connected"), true);
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    Data->SetNumberField(TEXT("edge_count"), PCG_GetAllEdges(Graph).Num());
#else
    Data->SetNumberField(TEXT("edge_count"), Graph->GetAllEdges().Num());
#endif
    Data->SetBoolField(TEXT("saved"), bSaved);
    if (!bAlready && !bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// disconnect_pcg_nodes
// Removes the directed edge From.from_pin -> To.to_pin via UPCGGraph::RemoveEdge.
// Idempotent: if no such edge exists it reports already_disconnected and makes no
// change. Pin resolution mirrors connect_pcg_nodes.
// ---------------------------------------------------------------------------
// MCP:disconnect_pcg_nodes - Removes the directed edge between two PCG nodes' pins.  Pin labels default to the node's "Out"/"In" pin (else its first pin) and the resolved labels are echoed. Idempotent: if no such edge exists it reports already_disconnected without error. The graph is re-saved after an edge is removed.
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example").
//   @param from_node - Upstream node name, or the token "Input".
//   @param to_node - Downstream node name, or the token "Output".
//   @param from_pin - Optional output-pin label on from_node (default the node's "Out" pin, else its first output pin).
//   @param to_pin - Optional input-pin label on to_node (default the node's "In" pin, else its first input pin).
//   @returns { graph_name: string, asset_path: string, from_node: string, from_pin: string, to_node: string, to_pin: string, already_disconnected: bool, removed: bool, disconnected: bool, edge_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleDisconnectPCGNodes(const TSharedPtr<FJsonObject>& Params)
{
    UPCGGraph* Graph = nullptr;
    UPCGNode* From = nullptr;
    UPCGNode* To = nullptr;
    FName FromLabel = NAME_None;
    FName ToLabel = NAME_None;
    if (TSharedPtr<FJsonObject> Error = PCG_ResolveEdgeEnds(Params, Graph, From, FromLabel, To, ToLabel))
    {
        return Error;
    }

    const bool bExisted = PCGEdgeExists(Graph, From, FromLabel, To, ToLabel);
    bool bRemoved = false;
    bool bSaved = false;
    FString SaveError;
    if (bExisted)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "DisconnectPCGNodes", "Disconnect PCG Nodes"));
        Graph->Modify();
        bRemoved = Graph->RemoveEdge(From, FromLabel, To, ToLabel);
        bSaved = PCG_SaveGraph(Graph, SaveError);
    }

    // Honest end-state: the edge is gone (whether we removed it now or it never existed).
    const bool bDisconnected = !PCGEdgeExists(Graph, From, FromLabel, To, ToLabel);

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("from_node"), From->GetName());
    Data->SetStringField(TEXT("from_pin"), FromLabel.ToString());
    Data->SetStringField(TEXT("to_node"), To->GetName());
    Data->SetStringField(TEXT("to_pin"), ToLabel.ToString());
    Data->SetBoolField(TEXT("already_disconnected"), !bExisted);
    Data->SetBoolField(TEXT("removed"), bRemoved);
    Data->SetBoolField(TEXT("disconnected"), bDisconnected);
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    Data->SetNumberField(TEXT("edge_count"), PCG_GetAllEdges(Graph).Num());
#else
    Data->SetNumberField(TEXT("edge_count"), Graph->GetAllEdges().Num());
#endif
    Data->SetBoolField(TEXT("saved"), bSaved);
    if (bExisted && !bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_pcg_node
// Removes a user-added node from a PCG graph via UPCGGraph::RemoveNode (PCG_API),
// which auto-breaks every edge touching the node and notifies its neighbours
// (Structural change), then reparents the node to the transient package. Saves
// the graph after the mutation. Idempotent: an unresolvable node reports
// already_removed without error. CRITICAL GUARD: RemoveNode has no engine-side
// protection for the implicit Input/Output endpoint nodes — removing one renames
// the endpoint to the transient package and leaves UPCGGraph::InputNode/OutputNode
// dangling (silent corruption, not an exception). We reject that up front.
// ---------------------------------------------------------------------------
// MCP:remove_pcg_node - Removes a user-added node from a PCG graph by name.  Uses UPCGGraph::RemoveNode, which auto-breaks the node's edges and notifies its neighbours; the graph is re-saved after the mutation. The implicit Input/Output endpoint nodes cannot be removed (doing so would silently corrupt the graph) and are rejected with an honest error. Idempotent: a node that is not present reports already_removed without error.
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example" or fully-qualified "/Game/PCG/PCG_Example.PCG_Example").
//   @param node_name - The node_name from get_pcg_graph_info of the node to remove. The tokens "Input"/"Output" (and the endpoints' real names) are rejected: the implicit graph endpoints cannot be removed.
//   @returns { graph_name: string, asset_path: string, node_name: string, removed: bool, already_removed: bool, node_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleRemovePCGNode(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    FString NodeName;
    if (TSharedPtr<FJsonObject> Error = PCG_ReadNodeParams(Params, GraphPath, NodeName))
    {
        return Error;
    }

    UPCGGraph* Graph = mcp_common::ResolveAsset<UPCGGraph>(GraphPath);
    if (!Graph)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("PCG graph not found: %s"), *GraphPath));
    }

    UPCGNode* Node = ResolvePCGNode(Graph, NodeName);

    // Idempotent: an unresolvable node is reported as already-removed, not an error.
    if (!Node)
    {
        TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
        Data->SetStringField(TEXT("node_name"), NodeName);
        Data->SetBoolField(TEXT("removed"), false);
        Data->SetBoolField(TEXT("already_removed"), true);
        Data->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
        Data->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Data);
    }

    // CRITICAL GUARD: RemoveNode would silently corrupt the graph if handed an
    // endpoint (it renames the endpoint to the transient package and leaves
    // InputNode/OutputNode dangling — no exception). Reject it with an honest error.
    if (Node == Graph->GetInputNode() || Node == Graph->GetOutputNode())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot remove the implicit Input/Output node of a PCG graph ('%s')"), *Node->GetName()));
    }

    const FString RemovedName = Node->GetName();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemovePCGNode", "Remove PCG Node"));
    Graph->Modify();
    Graph->RemoveNode(Node);   // PCG_API: auto-breaks edges + notifies neighbours.

    FString SaveError;
    const bool bSaved = PCG_SaveGraph(Graph, SaveError);

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("node_name"), RemovedName);
    Data->SetBoolField(TEXT("removed"), true);
    Data->SetBoolField(TEXT("already_removed"), false);
    Data->SetNumberField(TEXT("node_count"), Graph->GetNodes().Num());
    if (!bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_pcg_node"));
}

// ---------------------------------------------------------------------------
// get_pcg_node_properties
// Reads every editable property of a node's UPCGSettings via reflection
// (mcp_common::StructToJson over Settings->GetClass()). Read-only.
// ---------------------------------------------------------------------------
// MCP:get_pcg_node_properties - Read the reflected properties of one PCG node's settings object (UPCGSettings), serialized to JSON, the node found by its node_name from get_pcg_graph_info or the "Input"/"Output" tokens. Use it instead of get_properties, which would need the settings subobject's path. Read-only; write with set_pcg_node_properties.
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example" or fully-qualified "/Game/PCG/PCG_Example.PCG_Example").
//   @param node_name - The node_name from get_pcg_graph_info of the node to read (the tokens "Input"/"Output" address the implicit endpoints).
//   @returns { graph_name: string, asset_path: string, node_name: string, settings_class: string, properties: object }

TSharedPtr<FJsonObject> HandleGetPCGNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    FString NodeName;
    if (TSharedPtr<FJsonObject> Error = PCG_ReadNodeParams(Params, GraphPath, NodeName))
    {
        return Error;
    }

    UPCGGraph* Graph = nullptr;
    UPCGNode* Node = nullptr;
    UPCGSettings* Settings = nullptr;
    if (TSharedPtr<FJsonObject> Error = PCG_ResolveNodeSettings(GraphPath, NodeName, TEXT("read"), Graph, Node, Settings))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Properties = mcp_common::StructToJson(
        Settings->GetClass(), Settings, 0, 0);
    if (!Properties.IsValid())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to serialize settings of node '%s' (class %s)"),
                *Node->GetName(), *Settings->GetClass()->GetName()));
    }

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("node_name"), Node->GetName());
    Data->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
    Data->SetObjectField(TEXT("properties"), Properties);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_pcg_node_properties
// Authors a node's UPCGSettings via reflection: every key of properties is staged
// first (converted into a copy of its property's value, clamped to ClampMin/ClampMax
// meta), so an unknown key or a value that does not convert fails the call with
// nothing written; then the staged values are written and UPCGSettings::PostEditChange()
// invalidates the cached CRC / regens pins (the no-arg form forwards an empty
// FPropertyChangedEvent, which UPCGSettings treats as the deepest change type).
// Saves the graph after the mutation and reads the persisted state back.
// ---------------------------------------------------------------------------
// MCP:set_pcg_node_properties - Write fields of one PCG node's settings object, picked by node_name in a PCG Graph asset, then save the graph and read the settings back. Use it instead of set_properties, which needs the settings object's path: this also clamps numbers to ClampMin/ClampMax and runs PostEditChange, invalidating the node's cached CRC and rebuilding its pins. Only keys in properties are written, in one undo step; keys match property names case-insensitively. An empty object, a key that names no property (the error lists them), a deprecated or instanced one, a null value, or a value of the wrong kind or that does not convert is an error and nothing changes. A call that changes no value still runs PostEditChange and re-saves. "Input"/"Output" address the endpoints. Read: get_pcg_node_properties.
//   @param pcg_graph_path - Asset path of the PCG Graph (e.g. "/Game/PCG/PCG_Example" or fully-qualified "/Game/PCG/PCG_Example.PCG_Example").
//   @param node_name - The node_name from get_pcg_graph_info of the node to author (the tokens "Input"/"Output" address the implicit endpoints).
//   @param properties [object!] - Object of UPCGSettings fields to merge, keyed by property name (matched case-insensitively) (e.g. for a SurfaceSampler: bApplyDensityToPoints(bool), PointsPerSquaredMeter(float>=0), Looseness(float)); absent keys untouched. An empty object, a key that names no property of the settings class (or a deprecated one, or one holding instanced subobjects), a null value or a value that does not convert is an error and nothing is changed.
//   @returns { graph_name: string, asset_path: string, node_name: string, settings_class: string, applied_keys: [string], properties: object, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetPCGNodeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    FString NodeName;
    if (TSharedPtr<FJsonObject> Error = PCG_ReadNodeParams(Params, GraphPath, NodeName))
    {
        return Error;
    }
    const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsPtr) || !PropsPtr || !PropsPtr->IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'properties' parameter (object)"));
    }
    const TSharedPtr<FJsonObject>& PropsJson = *PropsPtr;
    if (PropsJson->Values.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("properties is empty: give at least one property to set; nothing was changed."));
    }

    UPCGGraph* Graph = nullptr;
    UPCGNode* Node = nullptr;
    UPCGSettings* Settings = nullptr;
    if (TSharedPtr<FJsonObject> Error = PCG_ResolveNodeSettings(GraphPath, NodeName, TEXT("author"), Graph, Node, Settings))
    {
        return Error;
    }

    // Every key is staged before the first write: the converter would skip a key that names no field without a
    // word and stop at the first value that does not convert, with the keys before it already written.
    FPCGStagedProperties Staged;
    if (TSharedPtr<FJsonObject> Refused = PCG_StageProperties(*PropsJson, *Settings, TEXT("properties"), Staged))
    {
        return Refused;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetPCGNodeProperties", "Set PCG Node Properties"));
    Settings->Modify();

    // Only the keys present in PropsJson are written; the rest of the settings object is left untouched.
    for (const TPair<FProperty*, void*>& Value : Staged.Values)
    {
        Value.Key->CopyCompleteValue(Value.Key->ContainerPtrToValuePtr<void>(Settings), Value.Value);
    }

    // Invalidate the node's cached CRC and regenerate its pins. The no-arg
    // PostEditChange() forwards an empty FPropertyChangedEvent, which UPCGSettings
    // treats as the deepest (Structural) change — sufficient to dirty the node.
    Settings->PostEditChange();

    FString SaveError;
    const bool bSaved = PCG_SaveGraph(Graph, SaveError);

    // Read the persisted settings back so the caller sees the effective state.
    TSharedPtr<FJsonObject> PropertiesOut = mcp_common::StructToJson(
        Settings->GetClass(), Settings, 0, 0);

    TArray<TSharedPtr<FJsonValue>> AppliedKeys;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : PropsJson->Values)
    {
        AppliedKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
    }

    TSharedPtr<FJsonObject> Data = PCG_NewGraphData(Graph);
    Data->SetStringField(TEXT("node_name"), Node->GetName());
    Data->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetName());
    Data->SetArrayField(TEXT("applied_keys"), AppliedKeys);
    if (PropertiesOut.IsValid())
    {
        Data->SetObjectField(TEXT("properties"), PropertiesOut);
    }
    if (!bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_pcg_node_properties"));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_pcg_component"), &HandleAddPCGComponent },
    { TEXT("add_pcg_graph_parameter"), &HandleAddPCGGraphParameter },
    { TEXT("add_pcg_node"), &HandleAddPCGNode },
    { TEXT("add_pcg_subgraph_node"), &HandleAddPCGSubgraphNode },
    { TEXT("cleanup_pcg_component"), &HandleCleanupPCGComponent },
    { TEXT("connect_pcg_nodes"), &HandleConnectPCGNodes },
    { TEXT("create_pcg_graph"), &HandleCreatePCGGraph },
    { TEXT("disconnect_pcg_nodes"), &HandleDisconnectPCGNodes },
    { TEXT("generate_pcg_component"), &HandleGeneratePCGComponent },
    { TEXT("get_pcg_component_info"), &HandleGetPCGComponentInfo },
    { TEXT("get_pcg_generated_data"), &HandleGetPCGGeneratedData },
    { TEXT("get_pcg_graph_info"), &HandleGetPCGGraphInfo },
    { TEXT("get_pcg_graph_parameters"), &HandleGetPCGGraphParameters },
    { TEXT("remove_pcg_graph_parameter"), &HandleRemovePCGGraphParameter },
    { TEXT("get_pcg_node_properties"), &HandleGetPCGNodeProperties },
    { TEXT("list_pcg_node_types"), &HandleListPCGNodeTypes },
    { TEXT("remove_pcg_node"), &HandleRemovePCGNode },
    { TEXT("set_pcg_component_properties"), &HandleSetPCGComponentProperties },
    { TEXT("set_pcg_graph_parameter"), &HandleSetPCGGraphParameter },
    { TEXT("set_pcg_node_properties"), &HandleSetPCGNodeProperties },
    { TEXT("spawn_pcg_volume"), &HandleSpawnPCGVolume },
};
} // namespace UEAgentPCGCommands
