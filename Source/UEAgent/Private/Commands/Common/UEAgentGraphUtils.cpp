// Copyright (c) 2026 CarlxsMG.

#include "Commands/Common/UEAgentGraphUtils.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Composite.h"
#include "K2Node_MacroInstance.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h" // TObjectIterator<UClass> -- DescribeClassNotFound's near-miss sweep (failure path only)

UClass* FUEAgentGraphUtils::ResolveClass(const FString& Identifier)
{
    if (Identifier.IsEmpty())
    {
        return nullptr;
    }

    // 1. Direct path load: /Script/Engine.Actor, /Game/Path/BP.BP_C, etc.
    if (UClass* Loaded = LoadObject<UClass>(nullptr, *Identifier, nullptr, LOAD_NoWarn | LOAD_Quiet))
    {
        return Loaded;
    }
    if (UClass* Loaded = LoadClass<UObject>(nullptr, *Identifier, nullptr, LOAD_NoWarn | LOAD_Quiet))
    {
        return Loaded;
    }

    // 2. Blueprint asset path -> generated class.
    if (UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *Identifier, nullptr, LOAD_NoWarn | LOAD_Quiet))
    {
        if (BP->GeneratedClass)
        {
            return BP->GeneratedClass;
        }
    }

    // 3. Bare/native name, trying common A/U prefixes and the Engine script package.
    TArray<FString> Candidates;
    Candidates.Add(Identifier);
    // Reflection class names carry no A/U C++ prefix. If the caller passed a
    // prefixed name ("AGameModeBase", "UEngineSubsystem"), also try the stripped
    // form (the original is tried first, so genuinely-prefixed reflection names
    // like "AIController" still match). Otherwise try the prefixed forms so a bare
    // "GameMode"/"PointLight" can resolve to A.../U... .
    const bool bLikelyPrefixed = Identifier.Len() >= 2
        && (Identifier[0] == TEXT('A') || Identifier[0] == TEXT('U'))
        && FChar::IsUpper(Identifier[1]);
    if (bLikelyPrefixed)
    {
        Candidates.Add(Identifier.RightChop(1));
    }
    else
    {
        Candidates.Add(TEXT("A") + Identifier);
        Candidates.Add(TEXT("U") + Identifier);
    }

    for (const FString& Name : Candidates)
    {
        if (UClass* Found = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst))
        {
            return Found;
        }
        if (UClass* Found = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Name), nullptr, LOAD_NoWarn | LOAD_Quiet))
        {
            return Found;
        }
        if (UClass* Found = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/CoreUObject.%s"), *Name), nullptr, LOAD_NoWarn | LOAD_Quiet))
        {
            return Found;
        }
    }

    return nullptr;
}

UScriptStruct* FUEAgentGraphUtils::ResolveScriptStruct(const FString& Identifier)
{
    if (Identifier.IsEmpty())
    {
        return nullptr;
    }

    // Common engine struct aliases.
    const FString Lower = Identifier.ToLower();
    if (Lower == TEXT("vector") || Lower == TEXT("fvector")) { return TBaseStructure<FVector>::Get(); }
    if (Lower == TEXT("rotator") || Lower == TEXT("frotator")) { return TBaseStructure<FRotator>::Get(); }
    if (Lower == TEXT("transform") || Lower == TEXT("ftransform")) { return TBaseStructure<FTransform>::Get(); }
    if (Lower == TEXT("vector2d") || Lower == TEXT("fvector2d")) { return TBaseStructure<FVector2D>::Get(); }
    if (Lower == TEXT("linearcolor") || Lower == TEXT("flinearcolor")) { return TBaseStructure<FLinearColor>::Get(); }
    if (Lower == TEXT("color") || Lower == TEXT("fcolor")) { return TBaseStructure<FColor>::Get(); }
    if (Lower == TEXT("quat") || Lower == TEXT("fquat")) { return TBaseStructure<FQuat>::Get(); }

    if (UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *Identifier))
    {
        return Loaded;
    }

    TArray<FString> Candidates;
    Candidates.Add(Identifier);
    if (!Identifier.StartsWith(TEXT("F")))
    {
        Candidates.Add(TEXT("F") + Identifier);
    }
    for (const FString& Name : Candidates)
    {
        if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Name, EFindFirstObjectOptions::NativeFirst))
        {
            return Found;
        }
        if (UScriptStruct* Found = LoadObject<UScriptStruct>(nullptr, *FString::Printf(TEXT("/Script/CoreUObject.%s"), *Name)))
        {
            return Found;
        }
    }
    return nullptr;
}

UEnum* FUEAgentGraphUtils::ResolveEnum(const FString& Identifier)
{
    if (Identifier.IsEmpty())
    {
        return nullptr;
    }
    if (UEnum* Loaded = LoadObject<UEnum>(nullptr, *Identifier))
    {
        return Loaded;
    }
    if (UEnum* Found = FindFirstObject<UEnum>(*Identifier, EFindFirstObjectOptions::NativeFirst))
    {
        return Found;
    }
    return nullptr;
}

// See the header for WHY this exists (the MetaHumanCharacterInstance false negative).
// Everything here is on the FAILURE path: ResolveClass already
// returned nullptr by the time anyone calls this.
FString FUEAgentGraphUtils::DescribeClassNotFound(const FString& Identifier)
{
    FString Msg = FString::Printf(TEXT("Class not found: %s"), *Identifier);
    if (Identifier.IsEmpty())
    {
        return Msg;
    }

    // Is Needle a subsequence of Haystack (same order, gaps allowed)? This is the rule
    // that catches a registered FName hiding inside the C++ type name -- "MetaHumanInstance"
    // inside "MetaHumanCharacterInstance" -- which Contains() cannot see.
    auto IsSubsequenceOf = [](const FString& Needle, const FString& Haystack) -> bool
    {
        if (Needle.IsEmpty() || Needle.Len() > Haystack.Len())
        {
            return false;
        }
        int32 N = 0;
        for (int32 H = 0; H < Haystack.Len() && N < Needle.Len(); ++H)
        {
            if (FChar::ToLower(Haystack[H]) == FChar::ToLower(Needle[N]))
            {
                ++N;
            }
        }
        return N == Needle.Len();
    };

    auto CommonPrefixLen = [](const FString& A, const FString& B) -> int32
    {
        const int32 Max = FMath::Min(A.Len(), B.Len());
        int32 I = 0;
        while (I < Max && FChar::ToLower(A[I]) == FChar::ToLower(B[I]))
        {
            ++I;
        }
        return I;
    };

    struct FNearMiss
    {
        FString Name;
        FString Path;
        int32   Score;
    };
    TArray<FNearMiss> NearMisses;

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Candidate = *It;
        if (!Candidate)
        {
            continue;
        }
        const FString Name = Candidate->GetName();

        // An exact (case-insensitive) match would already have resolved; if it is here it is
        // noise, not a suggestion.
        if (Name.Equals(Identifier, ESearchCase::IgnoreCase))
        {
            continue;
        }

        int32 Score = 0;
        if (Name.Contains(Identifier, ESearchCase::IgnoreCase) || Identifier.Contains(Name, ESearchCase::IgnoreCase))
        {
            Score = 50;
        }
        else if (IsSubsequenceOf(Name, Identifier) || IsSubsequenceOf(Identifier, Name))
        {
            Score = 30;
        }
        if (Score == 0)
        {
            continue;
        }

        // A shared head of at least 3 characters keeps "Vector"/"Actor" style coincidences out.
        const int32 Prefix = CommonPrefixLen(Name, Identifier);
        if (Prefix < 3)
        {
            continue;
        }
        Score += Prefix;
        Score -= FMath::Abs(Name.Len() - Identifier.Len()); // the closer in length, the likelier

        NearMisses.Add({ Name, Candidate->GetPathName(), Score });
    }

    // Score first, then name: the message must be identical across two identical editors,
    // or it stops being evidence.
    NearMisses.Sort([](const FNearMiss& A, const FNearMiss& B)
    {
        return A.Score != B.Score ? A.Score > B.Score : A.Name < B.Name;
    });

    if (NearMisses.Num() > 0)
    {
        const int32 Shown = FMath::Min(NearMisses.Num(), 8);
        TArray<FString> Parts;
        Parts.Reserve(Shown);
        for (int32 I = 0; I < Shown; ++I)
        {
            Parts.Add(FString::Printf(TEXT("%s (%s)"), *NearMisses[I].Name, *NearMisses[I].Path));
        }
        Msg += FString::Printf(TEXT(". Did you mean: %s?"), *FString::Join(Parts, TEXT(", ")));
        if (NearMisses.Num() > Shown)
        {
            Msg += FString::Printf(TEXT(" (+%d more near matches)"), NearMisses.Num() - Shown);
        }
    }

    // The other way a reflection lookup answers "not found" about something that exists:
    // the name IS reflected, just not as a UCLASS.
    if (UScriptStruct* AsStruct = ResolveScriptStruct(Identifier))
    {
        Msg += FString::Printf(
            TEXT(" NOTE: a USTRUCT named '%s' exists (%s) -- this resolves UCLASSes only; use get_struct_info."),
            *Identifier, *AsStruct->GetPathName());
    }
    else if (UEnum* AsEnum = ResolveEnum(Identifier))
    {
        Msg += FString::Printf(
            TEXT(" NOTE: a UENUM named '%s' exists (%s) -- this resolves UCLASSes only; use get_enum_info."),
            *Identifier, *AsEnum->GetPathName());
    }

    Msg += TEXT(" Accepted spellings: the bare UClass FName, /Script/Module.Class, a Blueprint asset path,");
    Msg += TEXT(" or a Blueprint generated class (_C suffix). WARNING: the registered FName can differ from");
    Msg += TEXT(" the C++ type name and from the name the Python bindings expose -- UMetaHumanCharacterInstance");
    Msg += TEXT(" is registered as 'MetaHumanInstance'. A 'not found' here is NOT evidence the class is absent.");

    return Msg;
}

bool FUEAgentGraphUtils::ResolvePinType(const FString& TypeName, FEdGraphPinType& OutPinType)
{
    FString Base = TypeName.TrimStartAndEnd();
    OutPinType = FEdGraphPinType();
    OutPinType.ContainerType = EPinContainerType::None;

    // Detect a container suffix/wrapper.
    auto StripWrapper = [&Base](const TCHAR* Prefix) -> bool
    {
        FString P = Prefix;
        if (Base.StartsWith(P + FString(TEXT("<")), ESearchCase::IgnoreCase) && Base.EndsWith(TEXT(">")))
        {
            Base = Base.Mid(P.Len() + 1, Base.Len() - P.Len() - 2).TrimStartAndEnd();
            return true;
        }
        return false;
    };

    if (Base.EndsWith(TEXT("[]")))
    {
        OutPinType.ContainerType = EPinContainerType::Array;
        Base = Base.LeftChop(2).TrimStartAndEnd();
    }
    else if (StripWrapper(TEXT("Array")) || StripWrapper(TEXT("TArray")))
    {
        OutPinType.ContainerType = EPinContainerType::Array;
    }
    else if (StripWrapper(TEXT("Set")) || StripWrapper(TEXT("TSet")))
    {
        OutPinType.ContainerType = EPinContainerType::Set;
    }

    const FString Lower = Base.ToLower();
    const UEdGraphSchema_K2* K2 = GetDefault<UEdGraphSchema_K2>();

    if (Lower == TEXT("bool") || Lower == TEXT("boolean"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
        return true;
    }
    if (Lower == TEXT("int") || Lower == TEXT("int32") || Lower == TEXT("integer"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
        return true;
    }
    if (Lower == TEXT("int64"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
        return true;
    }
    if (Lower == TEXT("float") || Lower == TEXT("double") || Lower == TEXT("real"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Real;
        OutPinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
        return true;
    }
    if (Lower == TEXT("string"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_String;
        return true;
    }
    if (Lower == TEXT("name"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Name;
        return true;
    }
    if (Lower == TEXT("text"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Text;
        return true;
    }
    if (Lower == TEXT("byte"))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        return true;
    }

    // Struct?
    if (UScriptStruct* Struct = ResolveScriptStruct(Base))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
        OutPinType.PinSubCategoryObject = Struct;
        return true;
    }

    // Enum?
    if (UEnum* Enum = ResolveEnum(Base))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
        OutPinType.PinSubCategoryObject = Enum;
        return true;
    }

    // Object / class reference?
    if (UClass* Class = ResolveClass(Base))
    {
        OutPinType.PinCategory = UEdGraphSchema_K2::PC_Object;
        OutPinType.PinSubCategoryObject = Class;
        return true;
    }

    return false;
}

UEdGraph* FUEAgentGraphUtils::FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
    {
        return nullptr;
    }

    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
    {
        return mcp_common::FindOrCreateEventGraph(Blueprint);
    }

    auto Match = [&GraphName](UEdGraph* G) -> bool
    {
        return G && G->GetName().Equals(GraphName, ESearchCase::IgnoreCase);
    };

    for (UEdGraph* G : Blueprint->UbergraphPages) { if (Match(G)) return G; }
    for (UEdGraph* G : Blueprint->FunctionGraphs) { if (Match(G)) return G; }
    for (UEdGraph* G : Blueprint->MacroGraphs) { if (Match(G)) return G; }
    for (UEdGraph* G : Blueprint->DelegateSignatureGraphs) { if (Match(G)) return G; }
    // Interface-override FUNCTION graphs (interface functions WITH a return value / outputs,
    // which cannot be placed as events) do NOT live in FunctionGraphs -- the engine's
    // ConformImplementedInterfaces stores them in each implemented interface's own Graphs
    // array (bAllowDeletion=false, carrying the Entry+Result terminators for the signature).
    // Mirrors UBlueprint::GetAllGraphs. Without this, every read/write tool that routes
    // through FindGraphByName(Deep) reports "Graph not found" for e.g. BPI_MyInterface::MyFunction.
    for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
    {
        for (UEdGraph* G : InterfaceDesc.Graphs) { if (Match(G)) return G; }
    }
    return nullptr;
}

namespace
{
    // Split a "Parent > Child > Grandchild" path into trimmed segments. The path
    // separator is ">" ONLY -- never "/" or "\\": a graph title may legitimately
    // contain those (e.g. "Mesh/Actor Spawning"), so splitting on them would mangle
    // a valid whole title into bogus segments. A name with no ">" yields one segment.
    void GraphDeep_SplitPath(const FString& In, TArray<FString>& OutSegments)
    {
        In.ParseIntoArray(OutSegments, TEXT(">"), /*CullEmpty*/ true);
        for (FString& S : OutSegments)
        {
            S.TrimStartAndEndInline();
        }
    }

    // Does Graph match Name by object name? Case-insensitive, and tolerant of
    // whitespace differences (display names may carry spaces; sanitized ones don't).
    bool GraphDeep_NameMatches(const UEdGraph* Graph, const FString& Name)
    {
        if (!Graph)
        {
            return false;
        }
        const FString GraphName = Graph->GetName();
        if (GraphName.Equals(Name, ESearchCase::IgnoreCase))
        {
            return true;
        }
        FString A = GraphName; A.ReplaceInline(TEXT(" "), TEXT(""));
        FString B = Name;      B.ReplaceInline(TEXT(" "), TEXT(""));
        return !B.IsEmpty() && A.Equals(B, ESearchCase::IgnoreCase);
    }

    // Immediate nested graphs of Graph: composite/tunnel bound graphs (aggregated
    // by the engine in UEdGraph::SubGraphs) plus macro graphs referenced by nodes.
    void GraphDeep_CollectChildren(UEdGraph* Graph, TArray<UEdGraph*>& Out)
    {
        if (!Graph)
        {
            return;
        }
        for (UEdGraph* Sub : Graph->SubGraphs)
        {
            if (Sub)
            {
                Out.AddUnique(Sub);
            }
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (UEdGraph* NodeGraph = FUEAgentGraphUtils::GetNodeSubGraph(Node))
            {
                Out.AddUnique(NodeGraph);
            }
        }
    }

    // Depth-first search for a nested graph named Name, starting below Graph.
    UEdGraph* GraphDeep_FindChild(UEdGraph* Graph, const FString& Name, TSet<UEdGraph*>& Visited)
    {
        if (!Graph || Visited.Contains(Graph))
        {
            return nullptr;
        }
        Visited.Add(Graph);

        TArray<UEdGraph*> Children;
        GraphDeep_CollectChildren(Graph, Children);
        for (UEdGraph* Child : Children)
        {
            if (GraphDeep_NameMatches(Child, Name))
            {
                return Child;
            }
        }
        for (UEdGraph* Child : Children)
        {
            if (UEdGraph* Found = GraphDeep_FindChild(Child, Name, Visited))
            {
                return Found;
            }
        }
        return nullptr;
    }

    // Resolve a SINGLE graph title (no ">" path separators): exact top-level match
    // first (fast, unambiguous), then a depth-first descent into nested
    // composite/macro/tunnel subgraphs. Matches by graph object name -- the same
    // name get_graph_nodes advertises for a collapsed composite's bound graph.
    UEdGraph* GraphDeep_ResolveSingle(UBlueprint* Blueprint, const FString& Name)
    {
        if (UEdGraph* Flat = FUEAgentGraphUtils::FindGraphByName(Blueprint, Name))
        {
            return Flat;
        }
        TSet<UEdGraph*> Visited;
        auto SearchRoots = [&](const auto& Graphs) -> UEdGraph*
        {
            for (UEdGraph* Root : Graphs)
            {
                if (UEdGraph* Found = GraphDeep_FindChild(Root, Name, Visited))
                {
                    return Found;
                }
            }
            return nullptr;
        };
        if (UEdGraph* Found = SearchRoots(Blueprint->UbergraphPages))          { return Found; }
        if (UEdGraph* Found = SearchRoots(Blueprint->FunctionGraphs))          { return Found; }
        if (UEdGraph* Found = SearchRoots(Blueprint->MacroGraphs))             { return Found; }
        if (UEdGraph* Found = SearchRoots(Blueprint->DelegateSignatureGraphs)) { return Found; }
        // Also descend from interface-override function graphs (a collapsed composite may be
        // nested inside e.g. BPI_MyInterface::MyFunction, addressable as "MyFunction > Sub").
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            if (UEdGraph* Found = SearchRoots(InterfaceDesc.Graphs)) { return Found; }
        }
        return nullptr;
    }
}

UEdGraph* FUEAgentGraphUtils::GetNodeSubGraph(UEdGraphNode* Node)
{
    if (!Node)
    {
        return nullptr;
    }
    if (const UK2Node_Composite* Composite = Cast<UK2Node_Composite>(Node))
    {
        return Composite->BoundGraph;
    }
    if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node))
    {
        return Macro->GetMacroGraph();
    }
    return nullptr;
}

UEdGraph* FUEAgentGraphUtils::FindGraphByNameDeep(UBlueprint* Blueprint, const FString& GraphName)
{
    if (!Blueprint)
    {
        return nullptr;
    }
    if (GraphName.IsEmpty() || GraphName.Equals(TEXT("EventGraph"), ESearchCase::IgnoreCase))
    {
        return mcp_common::FindOrCreateEventGraph(Blueprint);
    }

    // 1) Try the WHOLE name as an exact title first: a top-level page, then a
    //    recursive descent into nested composite/macro/tunnel subgraphs. This MUST
    //    precede any path interpretation so a title that literally contains '/'
    //    (e.g. "Mesh/Actor Spawning") resolves whole and is never split apart.
    if (UEdGraph* Whole = GraphDeep_ResolveSingle(Blueprint, GraphName))
    {
        return Whole;
    }

    // 2) Path form ("Outer > Inner"): resolve the first segment, then step down one
    //    named child per remaining segment. The separator is ">" only (never "/").
    TArray<FString> Segments;
    GraphDeep_SplitPath(GraphName, Segments);
    if (Segments.Num() > 1)
    {
        UEdGraph* Current = GraphDeep_ResolveSingle(Blueprint, Segments[0]);
        for (int32 i = 1; Current && i < Segments.Num(); ++i)
        {
            TArray<UEdGraph*> Children;
            GraphDeep_CollectChildren(Current, Children);
            UEdGraph* Next = nullptr;
            for (UEdGraph* Child : Children)
            {
                if (GraphDeep_NameMatches(Child, Segments[i]))
                {
                    Next = Child;
                    break;
                }
            }
            if (!Next)
            {
                TSet<UEdGraph*> Visited;
                Next = GraphDeep_FindChild(Current, Segments[i], Visited);
            }
            Current = Next;
        }
        return Current;
    }

    return nullptr;
}

UEdGraphNode* FUEAgentGraphUtils::FindNodeByGuid(UEdGraph* Graph, const FString& NodeId)
{
    if (!Graph)
    {
        return nullptr;
    }
    FGuid Target;
    const bool bParsed = FGuid::Parse(NodeId, Target);
    for (UEdGraphNode* Node : Graph->Nodes)
    {
        if (!Node)
        {
            continue;
        }
        if (bParsed && Node->NodeGuid == Target)
        {
            return Node;
        }
        if (!bParsed && Node->NodeGuid.ToString() == NodeId)
        {
            return Node;
        }
    }
    return nullptr;
}

UEdGraphPin* FUEAgentGraphUtils::FindPinByName(UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction)
{
    if (!Node)
    {
        return nullptr;
    }
    // Exact match first.
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName.ToString() == PinName && (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            return Pin;
        }
    }
    // Case-insensitive fallback.
    for (UEdGraphPin* Pin : Node->Pins)
    {
        if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase) &&
            (Direction == EGPD_MAX || Pin->Direction == Direction))
        {
            return Pin;
        }
    }
    return nullptr;
}

TSharedPtr<FJsonObject> FUEAgentGraphUtils::PinToJson(UEdGraphPin* Pin, bool bCompact)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Pin)
    {
        return Obj;
    }
    Obj->SetStringField(TEXT("name"), Pin->PinName.ToString());
    Obj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
    // Compact mode emits only name+direction+linked_to, plus default_value
    // ONLY when non-empty; it drops category/sub_category_object/container to cut
    // the per-pin token cost that blows the budget on large EventGraphs.
    if (!bCompact)
    {
        Obj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
        if (Pin->PinType.PinSubCategoryObject.IsValid())
        {
            Obj->SetStringField(TEXT("sub_category_object"), Pin->PinType.PinSubCategoryObject->GetName());
        }

        const TCHAR* Container = TEXT("none");
        switch (Pin->PinType.ContainerType)
        {
            case EPinContainerType::Array: Container = TEXT("array"); break;
            case EPinContainerType::Set:   Container = TEXT("set");   break;
            case EPinContainerType::Map:   Container = TEXT("map");   break;
            default: break;
        }
        Obj->SetStringField(TEXT("container"), Container);
        Obj->SetStringField(TEXT("default_value"), Pin->GetDefaultAsString());
    }
    else
    {
        const FString DefaultStr = Pin->GetDefaultAsString();
        if (!DefaultStr.IsEmpty())
        {
            Obj->SetStringField(TEXT("default_value"), DefaultStr);
        }
    }

    TArray<TSharedPtr<FJsonValue>> Linked;
    for (UEdGraphPin* Other : Pin->LinkedTo)
    {
        if (Other && Other->GetOwningNodeUnchecked())
        {
            TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
            Link->SetStringField(TEXT("node_id"), Other->GetOwningNode()->NodeGuid.ToString());
            Link->SetStringField(TEXT("pin_name"), Other->PinName.ToString());
            Linked.Add(MakeShared<FJsonValueObject>(Link));
        }
    }
    Obj->SetArrayField(TEXT("linked_to"), Linked);
    return Obj;
}

TSharedPtr<FJsonObject> FUEAgentGraphUtils::NodeToJson(UEdGraphNode* Node, bool bIncludePins, bool bCompact)
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    if (!Node)
    {
        return Obj;
    }
    Obj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
    Obj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
    Obj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
    // Compact mode drops positions + node comment (authoring metadata that
    // costs tokens but is not needed to read the logic). Full mode is unchanged.
    if (!bCompact)
    {
        Obj->SetNumberField(TEXT("pos_x"), Node->NodePosX);
        Obj->SetNumberField(TEXT("pos_y"), Node->NodePosY);
        Obj->SetStringField(TEXT("comment"), Node->NodeComment);
    }

    if (bIncludePins)
    {
        TArray<TSharedPtr<FJsonValue>> Pins;
        for (UEdGraphPin* Pin : Node->Pins)
        {
            // In compact mode skip pins that carry no information -- unlinked
            // AND with an empty default. PinToJson re-reads the default; acceptable.
            if (bCompact && Pin && Pin->LinkedTo.Num() == 0 && Pin->GetDefaultAsString().IsEmpty())
            {
                continue;
            }
            Pins.Add(MakeShared<FJsonValueObject>(PinToJson(Pin, bCompact)));
        }
        Obj->SetArrayField(TEXT("pins"), Pins);
    }
    return Obj;
}
