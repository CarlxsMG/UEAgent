// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h" // function-graph entry node (UK2Node_Event is only for the ubergraph)
#include "K2Node_Tunnel.h"        // macro/collapsed-graph input tunnel counts as an entry too
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/PostProcessVolume.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundWave.h"
#include "Sound/SoundCue.h"
#include "AudioDevice.h"
#include "Audio.h"
#include "Components/AudioComponent.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsThruster.h"
#include "PhysicsEngine/RadialForceComponent.h"
#include "CollisionQueryParams.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimationAsset.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Components/PrimitiveComponent.h"
#include "HAL/PlatformTime.h"
// Additional includes
#include "EdGraphNode_Comment.h"   // Comments have NO pins -> exclude them from "orphaned node"
#include "Editor.h"                // UnrealEd types (world resolution itself lives in mcp_common::ResolveWorldFromParams)
#include "Containers/Ticker.h"     // FTSTicker periodic sampling for watch_pie_property
#include "UObject/UObjectGlobals.h" // FindFirstObject<UClass> (class_name resolution in PIE)

namespace UEAgentTestingCommands
{

// MCP-DOMAIN: Testing

// Building blocks shared by the static-analysis reports below (check_blueprint_errors and
// get_blueprint_debug_info). They hold the exact wording and arithmetic the reports use.
namespace UEAgentTestingCommandsImpl
{

// The three identity fields most reports start with.
void WriteAssetIdentity(const TSharedPtr<FJsonObject>& Report, const FString& RequestedPath, const FAssetData& Asset)
{
    Report->SetStringField(TEXT("blueprint_path"), RequestedPath);
    Report->SetStringField(TEXT("resolved_object_path"), Asset.GetObjectPathString());
    Report->SetStringField(TEXT("blueprint_class"), Asset.AssetClassPath.ToString());
}

// Size on disk of the asset's package file (.uasset or .umap). False when the package has no file.
bool TryGetPackageFileSize(const FAssetData& Asset, int64& OutBytes)
{
    FString OnDiskName;
    if (!mcp_common::FindPackageFileOnDisk(Asset.PackageName.ToString(), OnDiskName))
    {
        return false;
    }
    OutBytes = FMath::Max<int64>(0, FPlatformFileManager::Get().GetPlatformFile().FileSize(*OnDiskName));
    return true;
}

double BytesToMegabytes(int64 Bytes)
{
    return static_cast<double>(Bytes) / (1024.0 * 1024.0);
}

// Cached compile state as reported by every report of this file (nothing is compiled here).
const TCHAR* CompileStatusLabel(const UBlueprint* Blueprint)
{
    switch (Blueprint->Status.GetValue())
    {
    case BS_UpToDate:             return TEXT("UpToDate");
    case BS_Error:                return TEXT("Error");
    case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
    default:                      return TEXT("Unknown");
    }
}

// Sizes of a Blueprint as the reports count them: function and event graphs (plus the nodes they
// hold), declared variables, and own construction-script nodes.
struct FBlueprintShape
{
    int32 Functions = 0;
    int32 Events = 0;
    int32 Variables = 0;
    int32 Components = 0;
    int32 Nodes = 0;
};

FBlueprintShape MeasureBlueprintShape(UBlueprint* Blueprint)
{
    FBlueprintShape Shape;
    Shape.Functions = Blueprint->FunctionGraphs.Num();
    Shape.Events = Blueprint->EventGraphs.Num();
    Shape.Variables = Blueprint->NewVariables.Num();
    for (const TArray<TObjectPtr<UEdGraph>>* Family : { &Blueprint->FunctionGraphs, &Blueprint->EventGraphs })
    {
        for (const UEdGraph* Graph : *Family)
        {
            Shape.Nodes += Graph ? Graph->Nodes.Num() : 0;
        }
    }
    if (USimpleConstructionScript* OwnScs = Blueprint->SimpleConstructionScript)
    {
        Shape.Components = OwnScs->GetAllNodes().Num();
    }
    return Shape;
}

// The heuristic weighting the reports call "complexity".
double HeuristicComplexity(const FBlueprintShape& Shape)
{
    return Shape.Nodes * 0.1 + Shape.Functions * 0.5 + Shape.Events * 0.3 + Shape.Variables * 0.2;
}

// First graph of the Blueprint's OWN function graphs with that name (compared like FString ==).
UEdGraph* FindOwnFunctionGraph(UBlueprint* Blueprint, const FString& GraphName)
{
    for (UEdGraph* Candidate : Blueprint->FunctionGraphs)
    {
        if (Candidate && Candidate->GetName().Equals(GraphName, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }
    return nullptr;
}
}

// =====================================================================
// SHARED FLEXIBLE RESOLVER
//
// The handlers in this file resolve their Blueprint through mcp_common::ResolveAssetFlexible,
// which accepts object path / package path / short name and NEVER picks silently when there is
// ambiguity. GetAssetByObjectPath(FSoftObjectPath(BlueprintPath)) only resolves a full OBJECT path
// ("/Game/X/BP_Foo.BP_Foo"): with the PACKAGE path everyone writes ("/Game/X/BP_Foo") it yields a
// valid FAssetData of the PACKAGE (class "Package"), whose GetAsset() is the UPackage -- and the
// Cast<UBlueprint> fails on a healthy Blueprint.
// =====================================================================

// Resolves NameOrPath to ONE single FAssetData. Ambiguity = failure with the full
// list of candidates (never a silent pick).
static bool TestingResolveAssetFlexibleSingle(
    const FString& NameOrPath,
    UClass* ExpectedClass,
    FAssetData& OutAssetData,
    FString& OutErrorText)
{
    TArray<FAssetData> Resolved;
    FString ResolveError;
    if (!mcp_common::ResolveAssetFlexible(NameOrPath, ExpectedClass, TEXT("/Game"), Resolved, ResolveError))
    {
        OutErrorText = ResolveError;
        return false;
    }
    if (Resolved.Num() > 1)
    {
        FString Candidates;
        for (int32 Index = 0; Index < Resolved.Num(); ++Index)
        {
            Candidates += (Index ? TEXT(", ") : TEXT("")) + Resolved[Index].GetObjectPathString();
        }
        OutErrorText = FString::Printf(
            TEXT("Ambiguous blueprint_path '%s': %d matches (%s). Pass a full object path to disambiguate."),
            *NameOrPath, Resolved.Num(), *Candidates);
        return false;
    }
    OutAssetData = Resolved[0];
    return true;
}

// Variant that also loads and casts to UBlueprint. Returns nullptr and leaves in
// OutError an error response ready to return. Distinguishes three different
// failures instead of "Failed to load blueprint" for all of them:
//   (a) does not exist / does not resolve, (b) exists but is NOT a Blueprint (says which
//   class it is), (c) resolves but loading returns something else.
static UBlueprint* TestingResolveBlueprintFlexible(
    const FString& NameOrPath,
    FAssetData& OutAssetData,
    TSharedPtr<FJsonObject>& OutError)
{
    FString ErrorText;
    if (!TestingResolveAssetFlexibleSingle(NameOrPath, UBlueprint::StaticClass(), OutAssetData, ErrorText))
    {
        // Diagnostic-only retry (no class filter): if the asset exists but
        // is not a Blueprint, saying so is far more useful than "not found".
        FAssetData AnyAsset;
        FString AnyError;
        if (TestingResolveAssetFlexibleSingle(NameOrPath, nullptr, AnyAsset, AnyError))
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Not a Blueprint: '%s' resolved to '%s' of class '%s'"),
                *NameOrPath, *AnyAsset.GetObjectPathString(), *AnyAsset.AssetClassPath.ToString()));
            return nullptr;
        }
        OutError = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Blueprint not found: %s (%s)"), *NameOrPath, *ErrorText));
        return nullptr;
    }

    UObject* Loaded = OutAssetData.GetAsset();
    UBlueprint* Blueprint = Cast<UBlueprint>(Loaded);
    if (!Blueprint)
    {
        OutError = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Failed to load blueprint: %s (registry resolved '%s'; GetAsset() returned %s)"),
            *NameOrPath,
            *OutAssetData.GetObjectPathString(),
            Loaded ? *Loaded->GetClass()->GetName() : TEXT("nullptr")));
        return nullptr;
    }
    return Blueprint;
}

// Reads the required blueprint_path, resolves it with TestingResolveBlueprintFlexible and starts the report with the
// asset's identity fields (WriteAssetIdentity). Null, with OutError holding the reply, when the param is missing or
// the Blueprint does not resolve.
static TSharedPtr<FJsonObject> TestingStartBlueprintReport(
    const TSharedPtr<FJsonObject>& Params,
    UBlueprint*& OutBlueprint,
    FAssetData& OutAssetData,
    TSharedPtr<FJsonObject>& OutError)
{
    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes))
    {
        OutError = Missing;
        return nullptr;
    }
    OutBlueprint = TestingResolveBlueprintFlexible(BlueprintPath, OutAssetData, OutError);
    if (!OutBlueprint)
    {
        return nullptr;
    }
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    UEAgentTestingCommandsImpl::WriteAssetIdentity(Report, BlueprintPath, OutAssetData);
    return Report;
}

// A node WITHOUT pins is not necessarily "orphaned". Comments
// (UEdGraphNode_Comment) have no pins BY DESIGN, so the heuristic
// `!Node->Pins.Num()` would flag EVERY comment in the graph as a warning -- pure noise
// that buries the real warnings. Events are excluded too.
static bool TestingIsPinlessByDesign(const UEdGraphNode* Node)
{
    if (!Node)
    {
        return true;
    }
    if (Node->IsA<UEdGraphNode_Comment>())
    {
        return true;   // comment: never has pins
    }
    if (Node->IsA<UK2Node_Event>())
    {
        return true;   // event
    }
    return false;
}

// MCP:check_blueprint_errors - Check a blueprint for compilation errors, warnings, structural issues, and orphaned nodes. Read-only: it does NOT recompile the Blueprint (it reports the cached Status).
//   @param blueprint_path - Object path, package path, or bare short name of the blueprint (resolved via mcp_common::ResolveAssetFlexible; ambiguity errors out listing candidates)
//   @returns { blueprint_path: string, resolved_object_path: string, blueprint_class: string, is_blueprint: bool, error_status: string, error_message: string, errors: [string], warnings: [string], error_count: int, warning_count: int, blueprint_compiled: bool, compilation_status: string, is_static_analysis: bool, recompiled_blueprint: bool, orphaned_node_heuristic: string }

TSharedPtr<FJsonObject> HandleCheckBlueprintErrors(const TSharedPtr<FJsonObject>& Params)
{
    UBlueprint* Blueprint = nullptr;
    FAssetData AssetData;
    TSharedPtr<FJsonObject> ResolveFailure;
    TSharedPtr<FJsonObject> Report = TestingStartBlueprintReport(Params, Blueprint, AssetData, ResolveFailure);
    if (!Report)
    {
        return ResolveFailure;
    }

    Report->SetBoolField(TEXT("is_blueprint"), true);
    // Honesty -- this command READS Blueprint->Status (the result of the last
    // compile) and walks the graph; it compiles NOTHING. A never-compiled BP comes out as
    // "Unknown", not as "clean".
    Report->SetBoolField(TEXT("is_static_analysis"), true);
    Report->SetBoolField(TEXT("recompiled_blueprint"), false);
    Report->SetStringField(TEXT("orphaned_node_heuristic"),
        TEXT("a node with zero pins that is neither a comment (UEdGraphNode_Comment, pin-less by design) nor an event"));

    // Findings are appended in a fixed order; each list keeps that order in the reply.
    TArray<FString> Problems;
    TArray<FString> Notices;

    // (1) The cached status of the last compile. Up to date or merely dirty adds nothing.
    switch (Blueprint->Status.GetValue())
    {
    case BS_Error:                Problems.Add(TEXT("Blueprint has compilation errors")); break;
    case BS_Unknown:              Notices.Add(TEXT("Blueprint compilation status is unknown")); break;
    case BS_BeingCreated:         Notices.Add(TEXT("Blueprint is currently being created")); break;
    case BS_UpToDateWithWarnings: Notices.Add(TEXT("Blueprint compiled with warnings")); break;
    default:                      break;
    }

    // (2) Pin-less nodes. GetAllPinConnections is not available, so a node with no pin at all is
    //     the signal -- except comments and events, which have no pins by design (TestingIsPinlessByDesign;
    //     counting comments would bury the real warnings under dozens of false ones).
    auto LacksPinsUnexpectedly = [](const UEdGraphNode* GraphNode)
    {
        return GraphNode && GraphNode->Pins.Num() == 0 && !TestingIsPinlessByDesign(GraphNode);
    };
    for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
    {
        if (!FunctionGraph)
        {
            continue;
        }
        for (const UEdGraphNode* GraphNode : FunctionGraph->Nodes)
        {
            if (LacksPinsUnexpectedly(GraphNode))
            {
                Notices.Add(FString::Printf(TEXT("Orphaned node found in function '%s': %s"),
                    *FunctionGraph->GetName(), *GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
            }
        }
    }
    for (UEdGraph* EventGraph : Blueprint->EventGraphs)
    {
        if (!EventGraph)
        {
            continue;
        }
        for (const UEdGraphNode* GraphNode : EventGraph->Nodes)
        {
            if (LacksPinsUnexpectedly(GraphNode))
            {
                Notices.Add(FString::Printf(TEXT("Disconnected node in event graph: %s"),
                    *GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString()));
            }
        }
    }

    // (3) A parent class is mandatory.
    if (!Blueprint->ParentClass)
    {
        Problems.Add(TEXT("Blueprint has no parent class assigned"));
    }

    // (4) Every own component template must still have a class.
    if (USimpleConstructionScript* OwnScs = Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* ScsNode : OwnScs->GetAllNodes())
        {
            if (ScsNode && ScsNode->ComponentTemplate && !ScsNode->ComponentTemplate->GetClass())
            {
                Problems.Add(FString::Printf(TEXT("Component '%s' has invalid class"), *ScsNode->GetVariableName().ToString()));
            }
        }
    }

    // (5) Every declared variable needs a name and a pin type.
    for (const FBPVariableDescription& Declared : Blueprint->NewVariables)
    {
        if (Declared.VarName.IsNone())
        {
            Problems.Add(TEXT("Blueprint has variable with empty name"));
        }
        if (Declared.VarType.PinCategory.IsNone())
        {
            Problems.Add(FString::Printf(TEXT("Variable '%s' has invalid type"), *Declared.VarName.ToString()));
        }
    }

    // (6) Function graphs: an empty one is reported as such; a non-empty one needs an entry node.
    //     Function graphs are entered through UK2Node_FunctionEntry (events only live in the
    //     ubergraph) and macro/collapsed graphs through UK2Node_Tunnel; an event is accepted too.
    //     Accepting only events would flag every well-formed user function as entry-less.
    for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
    {
        if (!FunctionGraph)
        {
            continue;
        }
        if (FunctionGraph->Nodes.Num() == 0)
        {
            Notices.Add(FString::Printf(TEXT("Function '%s' is empty"), *FunctionGraph->GetName()));
            continue;
        }
        bool bHasEntry = false;
        for (const UEdGraphNode* Candidate : FunctionGraph->Nodes)
        {
            if (Candidate && (Candidate->IsA<UK2Node_FunctionEntry>() || Candidate->IsA<UK2Node_Tunnel>() || Candidate->IsA<UK2Node_Event>()))
            {
                bHasEntry = true;
                break;
            }
        }
        if (!bHasEntry)
        {
            Notices.Add(FString::Printf(TEXT("Function '%s' has no entry point"), *FunctionGraph->GetName()));
        }
    }

    const int32 ProblemCount = Problems.Num();
    const int32 NoticeCount = Notices.Num();
    FString Verdict;
    FString Summary;
    if (ProblemCount > 0)
    {
        Verdict = TEXT("Errors Found");
        Summary = FString::Printf(TEXT("Found %d errors and %d warnings"), ProblemCount, NoticeCount);
    }
    else if (NoticeCount > 0)
    {
        Verdict = TEXT("Warnings Only");
        Summary = FString::Printf(TEXT("Found %d warnings, no errors"), NoticeCount);
    }
    else
    {
        Verdict = TEXT("Clean");
        Summary = TEXT("No errors or warnings found");
    }

    Report->SetStringField(TEXT("error_status"), Verdict);
    Report->SetStringField(TEXT("error_message"), Summary);
    Report->SetArrayField(TEXT("errors"), mcp_common::StringsToJson(Problems));
    Report->SetArrayField(TEXT("warnings"), mcp_common::StringsToJson(Notices));
    Report->SetNumberField(TEXT("error_count"), ProblemCount);
    Report->SetNumberField(TEXT("warning_count"), NoticeCount);
    Report->SetBoolField(TEXT("blueprint_compiled"), Blueprint->Status == BS_UpToDate);
    Report->SetStringField(TEXT("compilation_status"), UEAgentTestingCommandsImpl::CompileStatusLabel(Blueprint));
    return Report;
}

// Collects Graph and, if bIncludeComposites, its collapsed subgraphs (recursive,
// with a visited set in case a graph is referenced twice). ONLY UEdGraph::SubGraphs:
// following macro-instance graphs would drag in the engine's whole macro libraries.
static void TestingCollectGraphAndSubGraphs(UEdGraph* Graph, bool bIncludeComposites, TSet<UEdGraph*>& Visited, TArray<UEdGraph*>& Out)
{
    if (!Graph || Visited.Contains(Graph))
    {
        return;
    }
    Visited.Add(Graph);
    Out.Add(Graph);
    if (!bIncludeComposites)
    {
        return;
    }
    for (UEdGraph* Sub : Graph->SubGraphs)
    {
        TestingCollectGraphAndSubGraphs(Sub, bIncludeComposites, Visited, Out);
    }
}

// Classification of a node, IDENTICAL for function graphs and event graphs.
static void TestingAppendNodeClassification(UEdGraphNode* Node, const FString& NodeTitle, TArray<FString>& ExecutionFlow)
{
    if (!Node)
    {
        return;
    }
    // At most one indented "<kind>: <title>" line: the kinds are tried in table order and the
    // first class the node derives from names it. Any other node adds nothing.
    struct FListedNodeKind
    {
        UClass* BaseClass;
        const TCHAR* Label;
    };
    const FListedNodeKind ListedKinds[] =
    {
        { UK2Node_CallFunction::StaticClass(), TEXT("Function Call") },
        { UK2Node_Event::StaticClass(),        TEXT("Event") },
        { UK2Node_VariableGet::StaticClass(),  TEXT("Variable Get") },
        { UK2Node_VariableSet::StaticClass(),  TEXT("Variable Set") },
    };
    for (const FListedNodeKind& Kind : ListedKinds)
    {
        if (Node->IsA(Kind.BaseClass))
        {
            ExecutionFlow.Add(FString::Printf(TEXT("    %s: %s"), Kind.Label, *NodeTitle));
            return;
        }
    }
}

// The static graph listing of get_blueprint_debug_info (include_graph_listing): one family of graphs at a time
// (function graphs first, then event graphs), each with its own visited set. Every graph contributes a header
// line, then per node a "Node:" line and, for the kinds TestingAppendNodeClassification knows, one more line.
// Nothing is traced: nodes come in array order, not execution order.
static void TestingWriteGraphListing(UBlueprint* Blueprint, bool bIncludeComposites, const TSharedPtr<FJsonObject>& Report)
{
    TArray<FString> FlowLines;
    TArray<FString> FunctionGraphNames;
    TArray<FString> EventGraphNames;
    auto ListFamily = [&FlowLines, bIncludeComposites](const TArray<TObjectPtr<UEdGraph>>& Roots, const TCHAR* HeaderLabel, TArray<FString>& NamesOut)
    {
        TSet<UEdGraph*> Seen;
        TArray<UEdGraph*> InWalkOrder;
        for (UEdGraph* Root : Roots)
        {
            TestingCollectGraphAndSubGraphs(Root, bIncludeComposites, Seen, InWalkOrder);
        }
        for (UEdGraph* Walked : InWalkOrder)
        {
            if (!Walked)
            {
                continue;
            }
            const FString WalkedName = Walked->GetName();
            FlowLines.Add(FString::Printf(TEXT("%s: %s"), HeaderLabel, *WalkedName));
            NamesOut.Add(WalkedName);
            for (UEdGraphNode* GraphNode : Walked->Nodes)
            {
                if (!GraphNode)
                {
                    continue;
                }
                const FString Title = GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
                FlowLines.Add(FString::Printf(TEXT("  Node: %s"), *Title));
                TestingAppendNodeClassification(GraphNode, Title, FlowLines);
            }
        }
    };
    ListFamily(Blueprint->FunctionGraphs, TEXT("Function"), FunctionGraphNames);
    ListFamily(Blueprint->EventGraphs, TEXT("Event Graph"), EventGraphNames);

    TArray<FString> Caveats;
    Caveats.Add(TEXT("No runtime tracing: this is a static walk of the asset's graphs in the editor."));
    Caveats.Add(TEXT("Nodes are listed in array order, NOT in execution order: exec-pin wiring is not followed."));
    if (!bIncludeComposites)
    {
        Caveats.Add(TEXT("Collapsed composite subgraphs are NOT included; pass include_composites=true to descend into them."));
    }
    Caveats.Add(TEXT("Macro-instance graphs are never followed (they usually belong to another Blueprint or an engine macro library)."));
    Caveats.Add(TEXT("Only FunctionGraphs + EventGraphs are walked; MacroGraphs / DelegateSignatureGraphs / interface-override graphs are not."));

    Report->SetArrayField(TEXT("execution_flow"), mcp_common::StringsToJson(FlowLines));
    Report->SetArrayField(TEXT("function_calls"), mcp_common::StringsToJson(FunctionGraphNames));
    Report->SetArrayField(TEXT("event_calls"), mcp_common::StringsToJson(EventGraphNames));
    Report->SetNumberField(TEXT("graphs_analyzed"), FunctionGraphNames.Num() + EventGraphNames.Num());
    Report->SetBoolField(TEXT("includes_composite_subgraphs"), bIncludeComposites);
    Report->SetArrayField(TEXT("limitations"), mcp_common::StringsToJson(Caveats));
}

// The function_name block of get_blueprint_debug_info: whether the Blueprint has that function graph of its own and,
// when it does, its node and pin counts with their rating and findings. The function is never executed.
static void TestingWriteFunctionAnalysis(UBlueprint* Blueprint, const FString& FunctionName,
    const TSharedPtr<FJsonObject>& Report)
{
    Report->SetStringField(TEXT("function_name"), FunctionName);
    // Only the Blueprint's own function graphs are searched.
    const UEdGraph* FunctionGraph = UEAgentTestingCommandsImpl::FindOwnFunctionGraph(Blueprint, FunctionName);
    Report->SetBoolField(TEXT("function_exists"), FunctionGraph != nullptr);
    if (!FunctionGraph)
    {
        return;
    }

    const int32 NodeCount = FunctionGraph->Nodes.Num();
    int32 PinTotal = 0;
    for (const UEdGraphNode* GraphNode : FunctionGraph->Nodes)
    {
        PinTotal += GraphNode ? GraphNode->Pins.Num() : 0;
    }

    const TCHAR* Rating = TEXT("Good");
    if (NodeCount > 50)
    {
        Rating = TEXT("Complex");
    }
    else if (NodeCount > 20)
    {
        Rating = TEXT("Moderate");
    }

    TArray<FString> Findings;
    if (NodeCount > 100)
    {
        Findings.Add(TEXT("Function has high node count - consider breaking into smaller functions"));
    }
    if (PinTotal > 200)
    {
        Findings.Add(TEXT("Function has many pins - may impact performance"));
    }

    Report->SetNumberField(TEXT("function_node_count"), NodeCount);
    Report->SetNumberField(TEXT("function_pin_count"), PinTotal);
    Report->SetStringField(TEXT("function_rating"), Rating);
    Report->SetArrayField(TEXT("function_issues"), mcp_common::StringsToJson(Findings));
}

// MCP:get_blueprint_debug_info - Read a Blueprint's structure, metadata and complexity in one static report: cached compile status, file size, function/event/variable/component/node counts, function names, variables, components, a heuristic complexity score with its level, a performance rating and advice; with function_name, one function graph's node and pin counts; with include_graph_listing, every graph's nodes listed. An asset that resolves but is not a Blueprint is not an error: the reply is its identity with is_blueprint false. Read-only: nothing is compiled (compile_blueprint compiles), and nothing is executed or traced at runtime: observe runtime with set_blueprint_breakpoint (+ PIE) or watch_pie_property; the cached compile errors and structural problems are check_blueprint_errors.
//   @param blueprint_path - Object path, package path, or bare short name of the blueprint (resolved via mcp_common::ResolveAssetFlexible; ambiguity errors out listing candidates)
//   @param function_name - Optional name of one of the Blueprint's own function graphs (case-insensitive) to analyze statically: adds function_name and function_exists and, when it exists, function_node_count, function_pin_count, function_rating (Good, Moderate over 20 nodes, Complex over 50) and function_issues. The function is never executed; an unknown name is function_exists false, not an error.
//   @param include_graph_listing [bool] - Optional (default false): add a static listing of every function and event graph with its nodes, in array order rather than execution order: execution_flow, function_calls (function graph names), event_calls (event graph names), graphs_analyzed, includes_composite_subgraphs and limitations.
//   @param include_composites [bool] - Optional (default false), used with include_graph_listing: also descend into collapsed composite subgraphs (UEdGraph::SubGraphs). Macro-instance graphs are never followed.
//   @returns { blueprint_path: string, resolved_object_path: string, blueprint_class: string, is_blueprint: bool, debug_status: string, debug_level: string, debug_message: string, file_size_bytes: int, file_size_mb: number, function_count: int, event_count: int, variable_count: int, component_count: int, total_nodes: int, parent_class: string, blueprint_compiled: bool, compilation_status: string, function_names: [string], variable_info: [{ name, type, category, is_public, is_editable }], component_info: [{ name, class, is_root }], complexity_score: number, complexity_level: string, performance_rating: string, recommendations: [string], function_name?: string, function_exists?: bool, function_node_count?: int, function_pin_count?: int, function_rating?: string, function_issues?: [string], execution_flow?: [string], function_calls?: [string], event_calls?: [string], graphs_analyzed?: int, includes_composite_subgraphs?: bool, limitations?: [string] }

TSharedPtr<FJsonObject> HandleGetBlueprintDebugInfo(const TSharedPtr<FJsonObject>& Params)
{
    // The optional parts of the report, read up front.
    FString FunctionName;
    Params->TryGetStringField(TEXT("function_name"), FunctionName);
    bool bIncludeGraphListing = false;
    Params->TryGetBoolField(TEXT("include_graph_listing"), bIncludeGraphListing);
    bool bIncludeComposites = false;
    Params->TryGetBoolField(TEXT("include_composites"), bIncludeComposites);

    FString BlueprintPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_path"), BlueprintPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetData AssetData;
    TSharedPtr<FJsonObject> ResolveFailure;
    UBlueprint* Blueprint = TestingResolveBlueprintFlexible(BlueprintPath, AssetData, ResolveFailure);
    if (!Blueprint)
    {
        // An asset that resolves on its own but is not a Blueprint is an answer, not an error: its identity
        // with is_blueprint false. A path that resolves to nothing (or to several assets) stays the error.
        FAssetData AnyAsset;
        FString AnyError;
        if (TestingResolveAssetFlexibleSingle(BlueprintPath, nullptr, AnyAsset, AnyError)
            && !AnyAsset.IsInstanceOf(UBlueprint::StaticClass()))
        {
            TSharedPtr<FJsonObject> NotBlueprint = MakeShared<FJsonObject>();
            UEAgentTestingCommandsImpl::WriteAssetIdentity(NotBlueprint, BlueprintPath, AnyAsset);
            NotBlueprint->SetBoolField(TEXT("is_blueprint"), false);
            NotBlueprint->SetStringField(TEXT("debug_message"), FString::Printf(
                TEXT("Asset is not a Blueprint (class %s): nothing to analyze"), *AnyAsset.AssetClassPath.ToString()));
            return NotBlueprint;
        }
        return ResolveFailure;
    }

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    UEAgentTestingCommandsImpl::WriteAssetIdentity(Report, BlueprintPath, AssetData);
    Report->SetBoolField(TEXT("is_blueprint"), true);

    // Status / level pair derived from the cached compile state.
    const TCHAR* DebugStatus = TEXT("Available");
    const TCHAR* DebugLevel = TEXT("Basic");
    switch (Blueprint->Status.GetValue())
    {
    case BS_Error:                DebugStatus = TEXT("Error");    DebugLevel = TEXT("Critical"); break;
    case BS_UpToDateWithWarnings: DebugStatus = TEXT("Warnings"); DebugLevel = TEXT("Warning");  break;
    case BS_UpToDate:             DebugStatus = TEXT("Clean");    DebugLevel = TEXT("Normal");   break;
    default:                      break;
    }
    Report->SetStringField(TEXT("debug_status"), DebugStatus);
    Report->SetStringField(TEXT("debug_level"), DebugLevel);
    Report->SetStringField(TEXT("debug_message"), FString::Printf(TEXT("Debug info retrieved with real analysis - Status: %s"), DebugStatus));

    int64 DiskBytes = 0;
    UEAgentTestingCommandsImpl::TryGetPackageFileSize(AssetData, DiskBytes);
    const double DiskMegabytes = UEAgentTestingCommandsImpl::BytesToMegabytes(DiskBytes);
    Report->SetNumberField(TEXT("file_size_bytes"), DiskBytes);
    Report->SetNumberField(TEXT("file_size_mb"), DiskMegabytes);

    const UEAgentTestingCommandsImpl::FBlueprintShape Shape = UEAgentTestingCommandsImpl::MeasureBlueprintShape(Blueprint);
    Report->SetNumberField(TEXT("function_count"), Shape.Functions);
    Report->SetNumberField(TEXT("event_count"), Shape.Events);
    Report->SetNumberField(TEXT("variable_count"), Shape.Variables);
    Report->SetNumberField(TEXT("component_count"), Shape.Components);
    Report->SetNumberField(TEXT("total_nodes"), Shape.Nodes);
    Report->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : FString(TEXT("None")));
    Report->SetBoolField(TEXT("blueprint_compiled"), Blueprint->Status == BS_UpToDate);
    Report->SetStringField(TEXT("compilation_status"), UEAgentTestingCommandsImpl::CompileStatusLabel(Blueprint));

    TArray<FString> OwnFunctionNames;
    for (const UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
    {
        if (FunctionGraph)
        {
            OwnFunctionNames.Add(FunctionGraph->GetName());
        }
    }
    Report->SetArrayField(TEXT("function_names"), mcp_common::StringsToJson(OwnFunctionNames));

    // Flags are read off the stored variable description.
    TArray<TSharedPtr<FJsonValue>> VariableRows;
    for (const FBPVariableDescription& Declared : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Declared.VarName.ToString());
        Row->SetStringField(TEXT("type"), Declared.VarType.PinCategory.ToString());
        Row->SetStringField(TEXT("category"), Declared.Category.ToString());
        Row->SetBoolField(TEXT("is_public"), (Declared.PropertyFlags & CPF_BlueprintVisible) != 0);
        Row->SetBoolField(TEXT("is_editable"), (Declared.PropertyFlags & CPF_Edit) != 0);
        VariableRows.Add(MakeShared<FJsonValueObject>(Row));
    }
    Report->SetArrayField(TEXT("variable_info"), VariableRows);

    TArray<TSharedPtr<FJsonValue>> ComponentRows;
    if (USimpleConstructionScript* OwnScs = Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* ScsNode : OwnScs->GetAllNodes())
        {
            if (!ScsNode || !ScsNode->ComponentTemplate)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), ScsNode->GetVariableName().ToString());
            Row->SetStringField(TEXT("class"), ScsNode->ComponentTemplate->GetClass()->GetName());
            Row->SetBoolField(TEXT("is_root"), ScsNode->IsRootNode());
            ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }
    Report->SetArrayField(TEXT("component_info"), ComponentRows);

    const double Complexity = UEAgentTestingCommandsImpl::HeuristicComplexity(Shape);
    const TCHAR* ComplexityBand = TEXT("Simple");
    if (Complexity > 50.0)
    {
        ComplexityBand = TEXT("Very Complex");
    }
    else if (Complexity > 25.0)
    {
        ComplexityBand = TEXT("Complex");
    }
    else if (Complexity > 10.0)
    {
        ComplexityBand = TEXT("Moderate");
    }
    Report->SetNumberField(TEXT("complexity_score"), Complexity);
    Report->SetStringField(TEXT("complexity_level"), ComplexityBand);

    // Performance rating: the worst band any single metric reaches decides it.
    const TCHAR* Rating = TEXT("Excellent");
    if (Complexity > 50.0 || DiskMegabytes > 10.0)
    {
        Rating = TEXT("Poor");
    }
    else if (Complexity > 25.0 || DiskMegabytes > 5.0)
    {
        Rating = TEXT("Fair");
    }
    else if (Complexity > 10.0 || DiskMegabytes > 2.0)
    {
        Rating = TEXT("Good");
    }
    Report->SetStringField(TEXT("performance_rating"), Rating);

    // Advice, in a fixed order; only the entries whose threshold is exceeded are reported.
    struct FDebugAdvice
    {
        bool bApplies;
        const TCHAR* Text;
    };
    const FDebugAdvice AdviceTable[] =
    {
        { Shape.Nodes > 100,           TEXT("Consider breaking down complex functions into smaller ones") },
        { Shape.Functions > 20,        TEXT("Blueprint has many functions - consider organizing into parent classes") },
        { Shape.Events > 10,           TEXT("Consider consolidating event graphs") },
        { Shape.Variables > 50,        TEXT("High variable count - consider using structures or data assets") },
        { Shape.Components > 20,       TEXT("Many components - consider using child actor components") },
        { DiskBytes > 5 * 1024 * 1024, TEXT("Large file size - consider optimizing assets and reducing complexity") },
    };
    TArray<FString> Advice;
    for (const FDebugAdvice& Entry : AdviceTable)
    {
        if (Entry.bApplies)
        {
            Advice.Add(Entry.Text);
        }
    }
    Report->SetArrayField(TEXT("recommendations"), mcp_common::StringsToJson(Advice));


    if (!FunctionName.IsEmpty())
    {
        TestingWriteFunctionAnalysis(Blueprint, FunctionName, Report);
    }
    if (bIncludeGraphListing)
    {
        TestingWriteGraphListing(Blueprint, bIncludeComposites, Report);
    }
    return Report;
}

// Shared node-resolution for the breakpoint set/remove handlers: parse the two
// required params, resolve the UBlueprint via the Asset Registry, then locate a
// UEdGraphNode whose ListView title CONTAINS node_name across the function graphs
// first, then the event graphs (symmetry between set and remove), and log the match
// under CommandName. On any failure the corresponding out-error JSON is filled and
// false is returned.
static bool ResolveBreakpointTargetNode(
    const TSharedPtr<FJsonObject>& Params,
    const TCHAR* CommandName,
    UBlueprint*& OutBlueprint,
    UEdGraphNode*& OutNode,
    FString& OutBlueprintPath,
    FString& OutNodeName,
    TSharedPtr<FJsonObject>& OutError)
{
    OutBlueprint = nullptr;
    OutNode = nullptr;
    FString FoundInGraph;

    if (!Params->TryGetStringField(TEXT("blueprint_path"), OutBlueprintPath))
    {
        OutError = mcp_common::MakeErrorReply(TEXT("Missing 'blueprint_path' parameter"));
        return false;
    }
    if (!Params->TryGetStringField(TEXT("node_name"), OutNodeName))
    {
        OutError = mcp_common::MakeErrorReply(TEXT("Missing 'node_name' parameter"));
        return false;
    }

    // Shared flexible resolver (see TestingResolveBlueprintFlexible above).
    FAssetData AssetData;
    OutBlueprint = TestingResolveBlueprintFlexible(OutBlueprintPath, AssetData, OutError);
    if (!OutBlueprint)
    {
        return false;
    }

    // Search function graphs first, then event graphs, for a title-contains match.
    // Graph arrays are TArray<TObjectPtr<UEdGraph>>; the range-for yields UEdGraph*.
    auto ScanGraphs = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> bool
    {
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(OutNodeName))
                {
                    OutNode = Node;
                    FoundInGraph = Graph->GetName();
                    return true;
                }
            }
        }
        return false;
    };

    if (!ScanGraphs(OutBlueprint->FunctionGraphs))
    {
        ScanGraphs(OutBlueprint->EventGraphs);
    }

    UE_LOG(LogTemp, Verbose, TEXT("%s: bp='%s' node='%s' -> %s (graph='%s')"),
        CommandName, *OutBlueprintPath, *OutNodeName,
        OutNode ? TEXT("matched") : TEXT("no-match"), *FoundInGraph);

    // Node not found is NOT an error here -- the caller reports found_in_graph=false
    // as an honest (non-mutating) result. OutNode stays null; still return true.
    return true;
}

// MCP:set_blueprint_breakpoint - Set a real debugging breakpoint on a specific node within a blueprint graph via FKismetDebugUtilities::CreateBreakpoint. Idempotent (guarded by FindBreakpointForNode).
//   @param blueprint_path - Object path, package path, or bare short name of the blueprint (resolved via mcp_common::ResolveAssetFlexible; ambiguity errors out listing candidates)
//   @param node_name - Name or title of the graph node to set as a breakpoint (title-contains match)
//   @returns { blueprint_path: string, node_name: string, breakpoint_set: bool, already_existed: bool, found_in_graph: bool, node_class?: string, node_title?: string }

TSharedPtr<FJsonObject> HandleSetBlueprintBreakpoint(const TSharedPtr<FJsonObject>& Params)
{
    UBlueprint* Blueprint = nullptr;
    UEdGraphNode* TargetNode = nullptr;
    FString BlueprintPath;
    FString NodeName;
    TSharedPtr<FJsonObject> Error;
    if (!ResolveBreakpointTargetNode(Params, TEXT("set_blueprint_breakpoint"), Blueprint, TargetNode, BlueprintPath, NodeName, Error))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    Outcome->SetStringField(TEXT("node_name"), NodeName);
    auto Finish = [&Outcome](bool bBreakpointSet, bool bAlreadyExisted)
    {
        Outcome->SetBoolField(TEXT("breakpoint_set"), bBreakpointSet);
        Outcome->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
        return Outcome;
    };

    // No matching node: nothing is touched and the reply says the node was not found.
    if (!TargetNode)
    {
        Outcome->SetBoolField(TEXT("found_in_graph"), false);
        return Finish(false, false);
    }
    Outcome->SetStringField(TEXT("node_class"), TargetNode->GetClass()->GetName());
    Outcome->SetStringField(TEXT("node_title"), TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    Outcome->SetBoolField(TEXT("found_in_graph"), true);

    // Look before creating: CreateBreakpoint asserts (checkSlow) that the node has none yet,
    // which also makes a repeated call a no-op.
    if (FKismetDebugUtilities::FindBreakpointForNode(TargetNode, Blueprint) != nullptr)
    {
        UE_LOG(LogTemp, Verbose, TEXT("set_blueprint_breakpoint: breakpoint already present on node '%s'; no-op"), *NodeName);
        return Finish(true, true);
    }

    // A real editor breakpoint (editor-config state; works headless, no PIE needed), then read back.
    FKismetDebugUtilities::CreateBreakpoint(Blueprint, TargetNode, /*bIsEnabled=*/true);
    const bool bNowPresent = FKismetDebugUtilities::FindBreakpointForNode(TargetNode, Blueprint) != nullptr;
    UE_LOG(LogTemp, Verbose, TEXT("set_blueprint_breakpoint: CreateBreakpoint on node '%s' -> %s"),
        *NodeName, bNowPresent ? TEXT("set") : TEXT("FAILED-readback"));
    return Finish(bNowPresent, false);
}

// MCP:remove_blueprint_breakpoint - Remove a debugging breakpoint from a specific node within a blueprint graph via FKismetDebugUtilities::RemoveBreakpointFromNode. Idempotent (already_absent when none exists).
//   @param blueprint_path - Object path, package path, or bare short name of the blueprint (resolved via mcp_common::ResolveAssetFlexible; ambiguity errors out listing candidates)
//   @param node_name - Name or title of the graph node to remove the breakpoint from (title-contains match)
//   @returns { blueprint_path: string, node_name: string, removed: bool, already_absent: bool, found_in_graph: bool, node_class?: string, node_title?: string }

TSharedPtr<FJsonObject> HandleRemoveBlueprintBreakpoint(const TSharedPtr<FJsonObject>& Params)
{
    UBlueprint* Blueprint = nullptr;
    UEdGraphNode* TargetNode = nullptr;
    FString BlueprintPath;
    FString NodeName;
    TSharedPtr<FJsonObject> Error;
    if (!ResolveBreakpointTargetNode(Params, TEXT("remove_blueprint_breakpoint"), Blueprint, TargetNode, BlueprintPath, NodeName, Error))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("blueprint_path"), BlueprintPath);
    ResultObj->SetStringField(TEXT("node_name"), NodeName);

    if (!TargetNode)
    {
        // Node absent -> there is nothing to remove; honest non-mutating outcome.
        ResultObj->SetBoolField(TEXT("removed"), false);
        ResultObj->SetBoolField(TEXT("already_absent"), true);
        ResultObj->SetBoolField(TEXT("found_in_graph"), false);
        return ResultObj;
    }

    ResultObj->SetStringField(TEXT("node_class"), TargetNode->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("node_title"), TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
    ResultObj->SetBoolField(TEXT("found_in_graph"), true);

    // IDEMPOTENCY PRE-CHECK: no breakpoint on the node -> already_absent (success).
    if (FKismetDebugUtilities::FindBreakpointForNode(TargetNode, Blueprint) == nullptr)
    {
        UE_LOG(LogTemp, Verbose, TEXT("remove_blueprint_breakpoint: no breakpoint on node '%s'; already absent"), *NodeName);
        ResultObj->SetBoolField(TEXT("removed"), false);
        ResultObj->SetBoolField(TEXT("already_absent"), true);
        return ResultObj;
    }

    // REAL removal. Note both params const, order is (Node, Blueprint).
    FKismetDebugUtilities::RemoveBreakpointFromNode(TargetNode, Blueprint);

    // Honest read-back: the breakpoint must now be gone.
    const bool bRemoved = (FKismetDebugUtilities::FindBreakpointForNode(TargetNode, Blueprint) == nullptr);
    UE_LOG(LogTemp, Verbose, TEXT("remove_blueprint_breakpoint: RemoveBreakpointFromNode on node '%s' -> %s"),
        *NodeName, bRemoved ? TEXT("removed") : TEXT("FAILED-readback"));

    ResultObj->SetBoolField(TEXT("removed"), bRemoved);
    ResultObj->SetBoolField(TEXT("already_absent"), false);
    return ResultObj;
}

// =====================================================================
// watch_pie_property -- samples ONE property across a PIE session.
//
// Motivating case: a property flips back to `true` at runtime despite being `false`
// in the asset's template. With the other tools that is undetectable:
// get_actor_properties_by_class can only be called WITH PIE ALREADY RUNNING, and by
// then the BeginPlay write has already happened -- you only see the result, never
// the transition. That is why this watcher is ARMED BEFORE PIE:
//
//   FWorldDelegates::OnPostWorldInitialization  -> the PIE world exists (no actors yet);
//                                                  its OnWorldBeginPlay is hooked here
//   FWorldDelegates::OnWorldInitializedActors   -> actors created, BEFORE BeginPlay
//                                                  (UWorld.cpp, inside
//                                                   InitializeActorsForPlay)
//   UWorld::OnWorldBeginPlay                    -> AFTER GameMode->StartPlay(), that
//                                                  is, after the actors' BeginPlay
//                                                  (UWorld.cpp)
//   FTSTicker                                   -> periodic sampling afterwards
//
// The pair (actors_initialized, world_begin_play) is exactly the missing evidence:
// "before BeginPlay it was X, after it was Y".
//
// HONEST LIMIT, declared in the response as `identifies_writer:false`: this
// SAMPLES values, it does not intercept writes. It says WHEN it changed, not WHO changed
// it, and an A->B->A change between two samples is invisible. For the "who", chain it
// with set_blueprint_breakpoint on the suspected Variable Set node.
// =====================================================================

namespace
{
    struct FMcpTestingWatchSample
    {
        FString Phase;
        FString ObjectPath;
        FString ActorLabel;
        FString ValueText;
        TSharedPtr<FJsonValue> ValueJson;
        // Several properties sampled IN THE SAME tick. With just one,
        // ValueText/ValueJson are the bare value; with several, ValueText is the
        // readable row and this carries the breakdown.
        TArray<TPair<FString, FString>> ValuesByProperty;
        TArray<TPair<FString, TSharedPtr<FJsonValue>>> JsonByProperty;
        double  ElapsedRealSeconds = 0.0;
        double  WorldTimeSeconds = 0.0;
        FString WorldName;
        bool    bChangedFromPrevious = false;
    };

    struct FMcpTestingPropertyWatch
    {
        int32   Id = 0;
        FString PropertyName;                 // the first of PropertyNames (compatibility)
        TArray<FString> PropertyNames;        // >1 => one ROW per sample, dedup over the whole row
        FString ClassName;
        FString ActorLabel;
        FString ComponentName;
        double  IntervalSeconds = 0.1;
        double  DurationSeconds = 300.0;
        int32   MaxSamples = 400;
        bool    bActive = false;
        bool    bSawPieWorld = false;
        bool    bArmedWhilePieRunning = false;
        double  ArmRealTime = 0.0;
        FString StopReason;
        TMap<FString, FString> LastValueByObject;
        TArray<FMcpTestingWatchSample> Samples;
        FDelegateHandle PostWorldInitHandle;
        FDelegateHandle ActorsInitHandle;
        FTSTicker::FDelegateHandle TickerHandle;
        TArray<TWeakObjectPtr<UWorld>> HookedWorlds;
        TArray<FDelegateHandle> BeginPlayHandles;
    };

    TMap<int32, TSharedPtr<FMcpTestingPropertyWatch>>& McpTestingWatchRegistry()
    {
        static TMap<int32, TSharedPtr<FMcpTestingPropertyWatch>> Registry;
        return Registry;
    }

    int32 McpTestingNextWatchId()
    {
        static int32 NextId = 1;
        return NextId++;
    }

    // CANONICAL WORLD RESOLUTION: mcp_common::ResolveWorldFromParams with the 'pie'
    // selector (GEditor->PlayWorld, else the first EWorldType::PIE context), so this resolves
    // like every other tool -- two tools reading two different worlds contradict each other in
    // a way that looks like a game bug.
    //
    // A `world` parameter is NOT exposed on watch_pie_property, deliberately: the watcher
    // is armed with FWorldDelegates::OnPostWorldInitialization / OnWorldInitializedActors,
    // which fire for EVERY PIE world (server AND client) BEFORE the net mode is settled.
    // A 'server'/'client' filter would be honored by the ticker sampling but NOT by those
    // two hooks: a half filter lies more than none at all. What IS stamped is the evidence
    // (AddWorldEvidence) of the world the ticker is sampling NOW.
    UWorld* McpTestingResolvePieWorld()
    {
        TSharedPtr<FJsonObject> WorldParams = MakeShared<FJsonObject>();
        WorldParams->SetStringField(TEXT("world"), TEXT("pie"));
        FString UnusedError;
        return mcp_common::ResolveWorldFromParams(WorldParams, UnusedError);
    }

    // Reads the property on Target and, if it changed (or bForceRecord), records a sample.
    // Returns true if something was recorded.
    bool McpTestingRecordOne(
        const TSharedPtr<FMcpTestingPropertyWatch>& Watch,
        UObject* Target,
        const FString& ActorLabel,
        UWorld* World,
        const TCHAR* Phase,
        bool bForceRecord)
    {
        if (!Watch.IsValid() || !Target)
        {
            return false;
        }
        // ALL requested properties are sampled in the SAME pass, which
        // is the only way to answer a CORRELATION question -- "the movement mode
        // changed in the same tick the speed crossed the threshold". With one watch per
        // property there are N series with independent dedup and that question has no answer.
        TArray<TPair<FString, FString>> Values;
        TArray<TPair<FString, TSharedPtr<FJsonValue>>> ValuesJson;
        for (const FString& PropName : Watch->PropertyNames)
        {
            FProperty* Property = Target->GetClass()->FindPropertyByName(*PropName);
            if (!Property)
            {
                continue; // a property this object lacks: skipped, it does not invalidate the row
            }
            const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
            FString OneText;
            Property->ExportTextItem_Direct(OneText, ValuePtr, /*DefaultValue=*/nullptr, /*Parent=*/nullptr, PPF_None);
            Values.Emplace(PropName, OneText);
            ValuesJson.Emplace(PropName, mcp_common::PropertyValueToJson(Property, ValuePtr));
        }
        if (Values.Num() == 0)
        {
            return false; // none of the properties exists here
        }

        // The row is the dedup unit: if ANY field changes, the WHOLE row is recorded.
        // Recording only the field that changed would leave the columns misaligned in time,
        // which is exactly what makes the reading useless.
        FString ValueText;
        if (Values.Num() == 1)
        {
            ValueText = Values[0].Value; // just one: the bare value
        }
        else
        {
            TArray<FString> Parts;
            Parts.Reserve(Values.Num());
            for (const TPair<FString, FString>& P : Values)
            {
                Parts.Add(FString::Printf(TEXT("%s=%s"), *P.Key, *P.Value));
            }
            ValueText = FString::Join(Parts, TEXT(" | "));
        }

        const FString ObjectPath = Target->GetPathName();
        const FString* Previous = Watch->LastValueByObject.Find(ObjectPath);
        const bool bChanged = (Previous == nullptr) || (*Previous != ValueText);
        if (!bChanged && !bForceRecord)
        {
            return false;
        }
        Watch->LastValueByObject.Add(ObjectPath, ValueText);

        FMcpTestingWatchSample Sample;
        Sample.Phase = Phase;
        Sample.ObjectPath = ObjectPath;
        Sample.ActorLabel = ActorLabel;
        Sample.ValueText = ValueText;
        Sample.ValueJson = ValuesJson.Num() == 1 ? ValuesJson[0].Value : nullptr;
        Sample.ValuesByProperty = MoveTemp(Values);
        Sample.JsonByProperty = MoveTemp(ValuesJson);
        Sample.ElapsedRealSeconds = FPlatformTime::Seconds() - Watch->ArmRealTime;
        Sample.WorldTimeSeconds = World ? World->GetTimeSeconds() : 0.0;
        Sample.WorldName = World ? World->GetName() : FString();
        Sample.bChangedFromPrevious = bChanged;
        Watch->Samples.Add(MoveTemp(Sample));
        return true;
    }

    // Sweeps World for the objects that match the watch filter and records
    // one sample for each. bForceRecord=true at the milestones (arm / init / begin play)
    // so those anchors appear even if the value did not change; false in the
    // periodic sampling, which records only CHANGES (otherwise it would be an endless log).
    void McpTestingSampleWatch(
        const TSharedPtr<FMcpTestingPropertyWatch>& Watch,
        UWorld* World,
        const TCHAR* Phase,
        bool bForceRecord)
    {
        if (!Watch.IsValid() || !World)
        {
            return;
        }
        if (Watch->Samples.Num() >= Watch->MaxSamples)
        {
            return;
        }

        UClass* FilterClass = nullptr;
        if (!Watch->ClassName.IsEmpty())
        {
            FilterClass = FindFirstObject<UClass>(*Watch->ClassName);
            if (!FilterClass)
            {
                return; // unresolvable class: nothing to sample (reported in the start response)
            }
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            if (FilterClass && !Actor->IsA(FilterClass))
            {
                continue;
            }
            FString LabelForReport;
            LabelForReport = Actor->GetActorLabel();
            if (!Watch->ActorLabel.IsEmpty() && LabelForReport != Watch->ActorLabel && Actor->GetName() != Watch->ActorLabel)
            {
                continue;
            }

            UObject* Target = Actor;
            if (!Watch->ComponentName.IsEmpty())
            {
                UActorComponent* Found = nullptr;
                TArray<UActorComponent*> Components;
                Actor->GetComponents(Components);
                for (UActorComponent* Comp : Components)
                {
                    if (Comp && Comp->GetName() == Watch->ComponentName)
                    {
                        Found = Comp;
                        break;
                    }
                }
                if (!Found)
                {
                    continue;
                }
                Target = Found;
            }

            McpTestingRecordOne(Watch, Target, LabelForReport, World, Phase, bForceRecord);
            if (Watch->Samples.Num() >= Watch->MaxSamples)
            {
                return;
            }
        }
    }

    // Unhooks EVERYTHING. bRemoveTicker=false when called FROM the ticker itself
    // (which unregisters itself by returning false) -- removing the running handle from
    // inside the callback is exactly what must not be done.
    void McpTestingDisarmWatch(const TSharedPtr<FMcpTestingPropertyWatch>& Watch, bool bRemoveTicker)
    {
        if (!Watch.IsValid())
        {
            return;
        }
        Watch->bActive = false;

        if (Watch->PostWorldInitHandle.IsValid())
        {
            FWorldDelegates::OnPostWorldInitialization.Remove(Watch->PostWorldInitHandle);
            Watch->PostWorldInitHandle.Reset();
        }
        if (Watch->ActorsInitHandle.IsValid())
        {
            FWorldDelegates::OnWorldInitializedActors.Remove(Watch->ActorsInitHandle);
            Watch->ActorsInitHandle.Reset();
        }
        for (int32 Index = 0; Index < Watch->HookedWorlds.Num(); ++Index)
        {
            UWorld* HookedWorld = Watch->HookedWorlds[Index].Get();
            if (HookedWorld && Watch->BeginPlayHandles.IsValidIndex(Index) && Watch->BeginPlayHandles[Index].IsValid())
            {
                HookedWorld->OnWorldBeginPlay.Remove(Watch->BeginPlayHandles[Index]);
            }
        }
        Watch->HookedWorlds.Reset();
        Watch->BeginPlayHandles.Reset();

        if (Watch->TickerHandle.IsValid())
        {
            if (bRemoveTicker)
            {
                // Same style as UEAgentBridge.cpp. RemoveTicker only
                // spin-waits if the element is running on ANOTHER thread; here everything
                // is game thread, so there is no risk of blocking.
                FTSTicker::GetCoreTicker().RemoveTicker(Watch->TickerHandle);
            }
            Watch->TickerHandle.Reset();
        }
    }

    TSharedPtr<FJsonObject> McpTestingWatchToJson(const TSharedPtr<FMcpTestingPropertyWatch>& Watch, bool bIncludeSamples)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Watch.IsValid())
        {
            return Obj;
        }
        Obj->SetNumberField(TEXT("watch_id"), Watch->Id);
        Obj->SetStringField(TEXT("property_name"), Watch->PropertyName);
        if (Watch->PropertyNames.Num() > 1)
        {
            Obj->SetArrayField(TEXT("properties"), mcp_common::StringsToJson(Watch->PropertyNames));
        }
        Obj->SetStringField(TEXT("class_name"), Watch->ClassName);
        Obj->SetStringField(TEXT("actor_label"), Watch->ActorLabel);
        Obj->SetStringField(TEXT("component"), Watch->ComponentName);
        Obj->SetNumberField(TEXT("sample_interval_seconds"), Watch->IntervalSeconds);
        Obj->SetNumberField(TEXT("duration_seconds"), Watch->DurationSeconds);
        Obj->SetNumberField(TEXT("max_samples"), Watch->MaxSamples);
        Obj->SetBoolField(TEXT("active"), Watch->bActive);
        Obj->SetBoolField(TEXT("saw_pie_world"), Watch->bSawPieWorld);
        Obj->SetBoolField(TEXT("armed_while_pie_running"), Watch->bArmedWhilePieRunning);
        Obj->SetNumberField(TEXT("sample_count"), Watch->Samples.Num());
        Obj->SetNumberField(TEXT("elapsed_real_seconds"), FPlatformTime::Seconds() - Watch->ArmRealTime);
        Obj->SetStringField(TEXT("stop_reason"), Watch->StopReason);
        Obj->SetNumberField(TEXT("distinct_objects_seen"), Watch->LastValueByObject.Num());

        if (bIncludeSamples)
        {
            TArray<TSharedPtr<FJsonValue>> SampleArray;
            for (const FMcpTestingWatchSample& Sample : Watch->Samples)
            {
                TSharedPtr<FJsonObject> SampleObj = MakeShared<FJsonObject>();
                SampleObj->SetStringField(TEXT("phase"), Sample.Phase);
                SampleObj->SetStringField(TEXT("object_path"), Sample.ObjectPath);
                SampleObj->SetStringField(TEXT("actor_label"), Sample.ActorLabel);
                SampleObj->SetStringField(TEXT("value_text"), Sample.ValueText);
                if (Sample.ValueJson.IsValid())
                {
                    SampleObj->SetField(TEXT("value"), Sample.ValueJson);
                }
                else
                {
                    SampleObj->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
                }
                // Per-property breakdown ONLY when more than one was requested: with just
                // one, the sample carries no `values` object.
                if (Sample.ValuesByProperty.Num() > 1)
                {
                    TSharedPtr<FJsonObject> ByProp = MakeShared<FJsonObject>();
                    for (const TPair<FString, TSharedPtr<FJsonValue>>& P : Sample.JsonByProperty)
                    {
                        ByProp->SetField(P.Key, P.Value.IsValid() ? P.Value : MakeShared<FJsonValueNull>());
                    }
                    SampleObj->SetObjectField(TEXT("values"), ByProp);
                }
                SampleObj->SetNumberField(TEXT("elapsed_real_seconds"), Sample.ElapsedRealSeconds);
                SampleObj->SetNumberField(TEXT("world_time_seconds"), Sample.WorldTimeSeconds);
                SampleObj->SetStringField(TEXT("world"), Sample.WorldName);
                SampleObj->SetBoolField(TEXT("changed_from_previous"), Sample.bChangedFromPrevious);
                SampleArray.Add(MakeShared<FJsonValueObject>(SampleObj));
            }
            Obj->SetArrayField(TEXT("samples"), SampleArray);
        }
        return Obj;
    }
}

// MCP:watch_pie_property - Sample one or SEVERAL properties across a PIE session, arming BEFORE PIE starts so a write that happens in BeginPlay is still observable. Answers "what did this value do during play", NOT "who wrote it": it SAMPLES rather than intercepting writes, so `identifies_writer` is always false and a transient A->B->A between two samples is invisible. THE ORDER MATTERS: action="start" arms and returns immediately (it cannot block the game thread), THEN you start PIE, and afterwards action="status" returns the recorded samples. Arming while PIE is ALREADY running is honoured but flagged (`armed_while_pie_running`) -- the BeginPlay window has already passed for that session, which is precisely the failure this tool exists to avoid, so stop PIE, arm, then start PIE. It self-disarms on duration_seconds or max_samples (it can never leak a ticker) and the recorded samples survive until action="clear".
//   @param action - Optional: "start" (default) | "status" | "stop" | "clear" | "list".
//   @param property_name - Property to sample on the matched object. Required for "start" unless `properties` is given.
//   @param properties - Optional array of property names sampled TOGETHER, in the SAME tick, as one ROW. This is what answers correlation questions ("movement_mode changed in the same tick immersion crossed the threshold"), which N separate watches cannot: each would dedupe on its own clock and the columns would not line up. The row is the dedup unit -- if ANY field changes the whole row is recorded -- and each sample carries a `values` object with the per-property breakdown. With one property there is no `values` object.
//   @param class_name - Optional actor class filter (exact UClass name; a Blueprint class needs the "_C" suffix). At least one of class_name / actor_label is required for "start".
//   @param actor_label - Optional actor filter, matched against the editor label OR the object name.
//   @param component - Optional component name: sample the property on that component of the matched actor instead of on the actor.
//   @param sample_interval_seconds [number] - Optional periodic sampling interval (default 0.1, min 0.01).
//   @param duration_seconds [number] - Optional auto-disarm after this many real seconds (default 300; 0 = no time cap, disarms only on max_samples or an explicit stop).
//   @param max_samples [int] - Optional hard cap on recorded samples (default 400).
//   @param watch_id [int] - Target watch for "status" / "stop" / "clear" (omit = all of them).
//   @returns { action: string, watch_id?: int, armed?: bool, watches: [{ watch_id, property_name, class_name, actor_label, component, sample_interval_seconds, duration_seconds, max_samples, active, saw_pie_world, armed_while_pie_running, sample_count, elapsed_real_seconds, stop_reason, distinct_objects_seen, samples?: [{ phase, object_path, actor_label, value_text, value, elapsed_real_seconds, world_time_seconds, world, changed_from_previous }] }], identifies_writer: bool, sampling_only: bool, armed_pre_pie?: bool, pie_running?: bool, class_resolved?: bool, hint?: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }. The five world_* / net_mode / pie_instance fields identify the PIE world the ticker is sampling AT THE MOMENT OF THE CALL (all null / pie_instance -1 when no PIE session is running, which is the normal state right after arming).
// WARNING: Contract, stated so nobody reads more into it than it does:
//      - It SAMPLES, it does not intercept writes: `identifies_writer` is always false, and a
//        transient A->B->A between two samples is invisible (`sampling_only:true`).
//      - action="start" ARMS and returns immediately (it cannot block the game thread); then YOU
//        start PIE, and afterwards action="status" returns the recorded samples.
//      - Arming while PIE is ALREADY running is honoured but flagged (`armed_while_pie_running`):
//        the BeginPlay window has already passed for that session, which is precisely the failure
//        mode this tool exists to avoid. Stop PIE, arm, then start PIE.
//      - Phases recorded: arm_editor_world / arm_class_default (pre-PIE baseline),
//        post_world_init, actors_initialized (BEFORE actor BeginPlay), world_begin_play (AFTER
//        GameMode StartPlay), tick (periodic, changes only).
//      - The watcher self-disarms on duration_seconds or max_samples so it can never leak a
//        ticker; the recorded samples survive until action="clear".

TSharedPtr<FJsonObject> HandleWatchPieProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString Action = TEXT("start");
    Params->TryGetStringField(TEXT("action"), Action);
    Action = Action.ToLower();

    int32 RequestedWatchId = 0;
    const bool bHasWatchId = Params->TryGetNumberField(TEXT("watch_id"), RequestedWatchId);

    TMap<int32, TSharedPtr<FMcpTestingPropertyWatch>>& Registry = McpTestingWatchRegistry();

    // ---- status / list / stop / clear -----------------------------------
    if (Action == TEXT("status") || Action == TEXT("list") || Action == TEXT("stop") || Action == TEXT("clear"))
    {
        // Only `status` returns the samples; `stop` and `clear` are actions, not reads (and
        // returning every sample from them would make responses of hundreds of KB).
        const bool bIncludeSamples = (Action == TEXT("status"));

        TArray<int32> TargetIds;
        if (bHasWatchId)
        {
            if (!Registry.Contains(RequestedWatchId))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("watch_pie_property: no watch with watch_id=%d (use action='list')"), RequestedWatchId));
            }
            TargetIds.Add(RequestedWatchId);
        }
        else
        {
            Registry.GetKeys(TargetIds);
            TargetIds.Sort();
        }

        TArray<TSharedPtr<FJsonValue>> WatchArray;
        for (int32 Id : TargetIds)
        {
            TSharedPtr<FMcpTestingPropertyWatch> Watch = Registry.FindRef(Id);
            if (!Watch.IsValid())
            {
                continue;
            }
            if (Action == TEXT("stop") || Action == TEXT("clear"))
            {
                if (Watch->bActive)
                {
                    Watch->StopReason = TEXT("stopped_by_request");
                }
                McpTestingDisarmWatch(Watch, /*bRemoveTicker=*/true);
            }
            WatchArray.Add(MakeShared<FJsonValueObject>(McpTestingWatchToJson(Watch, bIncludeSamples)));
        }

        if (Action == TEXT("clear"))
        {
            for (int32 Id : TargetIds)
            {
                Registry.Remove(Id);
            }
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("action"), Action);
        Data->SetArrayField(TEXT("watches"), WatchArray);
        Data->SetNumberField(TEXT("watch_count"), WatchArray.Num());
        Data->SetBoolField(TEXT("identifies_writer"), false);
        Data->SetBoolField(TEXT("sampling_only"), true);
        UWorld* const CurrentPieWorld = McpTestingResolvePieWorld();
        Data->SetBoolField(TEXT("pie_running"), CurrentPieWorld != nullptr);
        // WHICH world the ticker is sampling right now. With 2+ PIE instances,
        // "pie_running: true" does not say which -- and that ambiguity is exactly the trap.
        // With PIE stopped the five fields come out null / pie_instance = -1, which is data too.
        mcp_common::AddWorldEvidence(Data, CurrentPieWorld);

        // Real verification: for stop/clear the watch state is re-read; for
        // status/list there is nothing to "verify" beyond returning what was recorded.
        if (Action == TEXT("stop") || Action == TEXT("clear"))
        {
            bool bAllDisarmed = true;
            for (int32 Id : TargetIds)
            {
                const TSharedPtr<FMcpTestingPropertyWatch>* Found = Registry.Find(Id);
                if (Found && Found->IsValid() && (*Found)->bActive)
                {
                    bAllDisarmed = false;
                }
            }
            return mcp_common::MakeVerifiedReply(
                Data, /*bSuccess=*/true,
                bAllDisarmed ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
                TEXT("watch_state_readback"),
                FString::Printf(TEXT("%d watch(es) targeted; active flag re-read after disarm"), TargetIds.Num()));
        }
        return mcp_common::MakeVerifiedReply(
            Data, /*bSuccess=*/true, mcp_common::EMcpVerification::Verified,
            TEXT("registry_readback"),
            FString::Printf(TEXT("%d watch(es) reported straight from the in-process registry"), WatchArray.Num()));
    }

    if (Action != TEXT("start"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("watch_pie_property: unknown action '%s' (expected start|status|stop|clear|list)"), *Action));
    }

    // ---- start ----------------------------------------------------------
    // `properties` (list) or `property_name` (single).
    // Both are accepted and unified: the list is what allows sampling several fields
    // in the SAME tick and answering correlation questions.
    TArray<FString> PropertyNames;
    const TArray<TSharedPtr<FJsonValue>>* PropsArray = nullptr;
    if (Params->TryGetArrayField(TEXT("properties"), PropsArray) && PropsArray)
    {
        for (const TSharedPtr<FJsonValue>& V : *PropsArray)
        {
            FString One;
            if (V.IsValid() && V->TryGetString(One))
            {
                One.TrimStartAndEndInline();
                if (!One.IsEmpty() && !PropertyNames.Contains(One))
                {
                    PropertyNames.Add(One);
                }
            }
        }
    }
    FString PropertyName;
    if (Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        PropertyName.TrimStartAndEndInline();
        if (!PropertyName.IsEmpty() && !PropertyNames.Contains(PropertyName))
        {
            PropertyNames.Insert(PropertyName, 0);
        }
    }
    if (PropertyNames.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing 'property_name' (or 'properties': a non-empty array of names)"));
    }
    PropertyName = PropertyNames[0];
    FString ClassName;
    Params->TryGetStringField(TEXT("class_name"), ClassName);
    FString ActorLabel;
    Params->TryGetStringField(TEXT("actor_label"), ActorLabel);
    FString ComponentName;
    Params->TryGetStringField(TEXT("component"), ComponentName);

    if (ClassName.IsEmpty() && ActorLabel.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("watch_pie_property: provide at least one of 'class_name' or 'actor_label' (an unfiltered watch would sample every actor in the world every tick)"));
    }

    double IntervalSeconds = 0.1;
    Params->TryGetNumberField(TEXT("sample_interval_seconds"), IntervalSeconds);
    IntervalSeconds = FMath::Max(IntervalSeconds, 0.01);

    double DurationSeconds = 300.0;
    Params->TryGetNumberField(TEXT("duration_seconds"), DurationSeconds);
    DurationSeconds = FMath::Max(DurationSeconds, 0.0);

    int32 MaxSamples = 400;
    Params->TryGetNumberField(TEXT("max_samples"), MaxSamples);
    MaxSamples = FMath::Clamp(MaxSamples, 1, 5000);

    // The class is resolved NOW so we can say honestly whether the filter is valid
    // (if it does not resolve, the watcher would never match anything and would stay silent).
    UClass* FilterClass = nullptr;
    if (!ClassName.IsEmpty())
    {
        FilterClass = FindFirstObject<UClass>(*ClassName);
        if (!FilterClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("watch_pie_property: class_name '%s' does not resolve to a UClass (a Blueprint class needs the '_C' suffix, e.g. 'BP_Foo_C')"), *ClassName));
        }
    }

    TSharedPtr<FMcpTestingPropertyWatch> Watch = MakeShared<FMcpTestingPropertyWatch>();
    Watch->Id = McpTestingNextWatchId();
    Watch->PropertyName = PropertyName;
    Watch->PropertyNames = PropertyNames;
    Watch->ClassName = ClassName;
    Watch->ActorLabel = ActorLabel;
    Watch->ComponentName = ComponentName;
    Watch->IntervalSeconds = IntervalSeconds;
    Watch->DurationSeconds = DurationSeconds;
    Watch->MaxSamples = MaxSamples;
    Watch->ArmRealTime = FPlatformTime::Seconds();
    Watch->bActive = true;
    Watch->bArmedWhilePieRunning = (McpTestingResolvePieWorld() != nullptr);

    // PRE-PIE baseline #1: the EDITOR world (values as saved in the
    // level). It is the "template" half of the comparison a BeginPlay write needs.
    if (UWorld* EditorWorld = mcp_common::GetEditorWorld())
    {
        McpTestingSampleWatch(Watch, EditorWorld, TEXT("arm_editor_world"), /*bForceRecord=*/true);
    }

    // PRE-PIE baseline #2: the class CDO (the asset default). Only when the
    // actor itself is observed: for a component the actor's CDO does not expose the instance.
    bool bSampledClassDefault = false;
    if (FilterClass && ComponentName.IsEmpty())
    {
        if (UObject* ClassDefault = FilterClass->GetDefaultObject())
        {
            bSampledClassDefault = McpTestingRecordOne(
                Watch, ClassDefault, FString(), /*World=*/nullptr, TEXT("arm_class_default"), /*bForceRecord=*/true);
        }
    }

    // ---- PRE-PIE arming ------------------------------------------------
    TWeakPtr<FMcpTestingPropertyWatch> WeakWatch = Watch;

    Watch->PostWorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddLambda(
        [WeakWatch](UWorld* World, const UWorld::InitializationValues /*IVS*/)
        {
            TSharedPtr<FMcpTestingPropertyWatch> Pinned = WeakWatch.Pin();
            if (!Pinned.IsValid() || !Pinned->bActive || !World || World->WorldType != EWorldType::PIE)
            {
                return;
            }
            Pinned->bSawPieWorld = true;
            // At this point the PIE world exists but usually has no actors yet:
            // the sample may come out empty, and that is information, not a failure.
            McpTestingSampleWatch(Pinned, World, TEXT("post_world_init"), /*bForceRecord=*/true);

            // Hook THIS world's BeginPlay: it fires AFTER the actors' BeginPlay,
            // which is where the writer we are looking for lives.
            TWeakObjectPtr<UWorld> WeakWorld(World);
            const FDelegateHandle BeginPlayHandle = World->OnWorldBeginPlay.AddLambda(
                [WeakWatch, WeakWorld]()
                {
                    TSharedPtr<FMcpTestingPropertyWatch> Inner = WeakWatch.Pin();
                    UWorld* LiveWorld = WeakWorld.Get();
                    if (!Inner.IsValid() || !Inner->bActive || !LiveWorld)
                    {
                        return;
                    }
                    McpTestingSampleWatch(Inner, LiveWorld, TEXT("world_begin_play"), /*bForceRecord=*/true);
                });
            Pinned->BeginPlayHandles.Add(BeginPlayHandle);
            Pinned->HookedWorlds.Add(WeakWorld);
        });

    Watch->ActorsInitHandle = FWorldDelegates::OnWorldInitializedActors.AddLambda(
        [WeakWatch](const FActorsInitializedParams& InitParams)
        {
            TSharedPtr<FMcpTestingPropertyWatch> Pinned = WeakWatch.Pin();
            if (!Pinned.IsValid() || !Pinned->bActive || !InitParams.World || InitParams.World->WorldType != EWorldType::PIE)
            {
                return;
            }
            Pinned->bSawPieWorld = true;
            // BEFORE the actors' BeginPlay: the "before" half of the evidence.
            McpTestingSampleWatch(Pinned, InitParams.World, TEXT("actors_initialized"), /*bForceRecord=*/true);
        });

    Watch->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
        [WeakWatch](float /*DeltaTime*/) -> bool
        {
            TSharedPtr<FMcpTestingPropertyWatch> Pinned = WeakWatch.Pin();
            if (!Pinned.IsValid())
            {
                return false; // watch destroyed -> unregister
            }
            if (!Pinned->bActive)
            {
                return false;
            }
            const double Now = FPlatformTime::Seconds();
            if (Pinned->DurationSeconds > 0.0 && (Now - Pinned->ArmRealTime) >= Pinned->DurationSeconds)
            {
                Pinned->StopReason = TEXT("duration_elapsed");
                McpTestingDisarmWatch(Pinned, /*bRemoveTicker=*/false);
                return false;
            }
            if (Pinned->Samples.Num() >= Pinned->MaxSamples)
            {
                Pinned->StopReason = TEXT("max_samples_reached");
                McpTestingDisarmWatch(Pinned, /*bRemoveTicker=*/false);
                return false;
            }
            if (UWorld* PieWorld = McpTestingResolvePieWorld())
            {
                Pinned->bSawPieWorld = true;
                McpTestingSampleWatch(Pinned, PieWorld, TEXT("tick"), /*bForceRecord=*/false);
            }
            return true;
        }), static_cast<float>(IntervalSeconds));

    Registry.Add(Watch->Id, Watch);

    // If PIE was already running, take an immediate sample as a baseline -- but without
    // pretending that equals pre-PIE arming (the flag says so).
    if (Watch->bArmedWhilePieRunning)
    {
        if (UWorld* PieWorld = McpTestingResolvePieWorld())
        {
            McpTestingSampleWatch(Watch, PieWorld, TEXT("arm_pie_already_running"), /*bForceRecord=*/true);
        }
    }

    // REAL verification of the arming: the three handles must exist.
    const bool bArmed =
        Watch->PostWorldInitHandle.IsValid() &&
        Watch->ActorsInitHandle.IsValid() &&
        Watch->TickerHandle.IsValid();
    if (!bArmed)
    {
        McpTestingDisarmWatch(Watch, /*bRemoveTicker=*/true);
        Watch->StopReason = TEXT("arm_failed");
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("action"), TEXT("start"));
    Data->SetNumberField(TEXT("watch_id"), Watch->Id);
    Data->SetBoolField(TEXT("armed"), bArmed);
    Data->SetBoolField(TEXT("armed_pre_pie"), bArmed && !Watch->bArmedWhilePieRunning);
    Data->SetBoolField(TEXT("pie_running"), Watch->bArmedWhilePieRunning);
    Data->SetBoolField(TEXT("class_resolved"), FilterClass != nullptr);
    Data->SetBoolField(TEXT("sampled_class_default"), bSampledClassDefault);
    Data->SetBoolField(TEXT("identifies_writer"), false);
    Data->SetBoolField(TEXT("sampling_only"), true);
    Data->SetStringField(TEXT("hint"), Watch->bArmedWhilePieRunning
        ? TEXT("PIE was ALREADY running when this watch was armed: the BeginPlay window of that session has already passed, so a BeginPlay writer will NOT be visible. Stop PIE, arm again, then start PIE, and read the samples with action='status'.")
        : TEXT("Armed before PIE. Now start PIE, let it run, then call action='status' to read the samples. Compare the 'actors_initialized' sample (before actor BeginPlay) with the 'world_begin_play' one (after) to place the write. To find WHO writes it, put set_blueprint_breakpoint on the suspected Variable Set node."));

    TArray<TSharedPtr<FJsonValue>> WatchArray;
    WatchArray.Add(MakeShared<FJsonValueObject>(McpTestingWatchToJson(Watch, /*bIncludeSamples=*/true)));
    Data->SetArrayField(TEXT("watches"), WatchArray);

    // WHICH PIE world exists right now (null in the normal case: arm BEFORE starting PIE).
    mcp_common::AddWorldEvidence(Data, McpTestingResolvePieWorld());

    return mcp_common::MakeVerifiedReply(
        Data, /*bSuccess=*/bArmed,
        bArmed ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("delegate_handles_readback"),
        TEXT("OnPostWorldInitialization + OnWorldInitializedActors + FTSTicker handles re-read after registration"),
        bArmed ? FString() : FString(TEXT("watch_pie_property: failed to register one or more pre-PIE delegates")));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("check_blueprint_errors"), &HandleCheckBlueprintErrors },
    { TEXT("get_blueprint_debug_info"), &HandleGetBlueprintDebugInfo },
    { TEXT("remove_blueprint_breakpoint"), &HandleRemoveBlueprintBreakpoint },
    { TEXT("set_blueprint_breakpoint"), &HandleSetBlueprintBreakpoint },
    { TEXT("watch_pie_property"), &HandleWatchPieProperty },
};
} // namespace UEAgentTestingCommands
