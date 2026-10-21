// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

// K2 node RTTI surface used to classify a match. Every member read below is
// public in Engine/Source/Editor/BlueprintGraph/Classes
// (GENERATED_UCLASS_BODY leaves the first block public).
#include "K2Node_Variable.h"            // UK2Node_Variable::VariableReference (public)
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_CallFunction.h"        // GetTargetFunction() public; GetTargetFunctionFromSkeletonClass() is PROTECTED -> not used
#include "K2Node_MacroInstance.h"       // GetMacroGraph() public
#include "K2Node_Event.h"               // EventReference / bOverrideFunction / CustomFunctionName public
#include "K2Node_CustomEvent.h"
#include "K2Node_ComponentBoundEvent.h" // ComponentPropertyName / DelegatePropertyName public
#include "K2Node_BaseMCDelegate.h"      // GetPropertyName() public
#include "K2Node_CreateDelegate.h"      // GetFunctionName() BLUEPRINTGRAPH_API
#include "K2Node_Timeline.h"            // TimelineName public
#include "K2Node_Literal.h"             // ObjectRef is PRIVATE -> GetObjectRef() public
#include "K2Node_DynamicCast.h"         // TargetType public

#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"

#include "UObject/UnrealType.h"      // FProperty / FObjectProperty
#include "UObject/Class.h"           // UClass::IsChildOf / CLASS_Interface
#include "UObject/Package.h"         // UPackage::IsDirty
#include "UObject/UObjectGlobals.h"  // FindPackage / FindObject / LoadObject
#include "Engine/MemberReference.h"  // FMemberReference::ResolveMember

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformTime.h"

/**
 * Blueprint SYMBOL-SEARCH commands ("Find in Blueprints" over MCP).
 *
 *   - find_blueprint_node_references : given a symbol (variable / function / event /
 *                                     delegate / macro / component / class), walk every
 *                                     Blueprint under a content path and report each
 *                                     node that references it, with the graph chain that
 *                                     leads to it and the RTTI-derived match kind.
 *
 * Lives in its own translation unit because the scan is project-wide and read-only:
 * it must never share a file with the mutating Blueprint commands, and it carries a
 * byte-level .uasset prefilter (name-map oracle) that nothing else here needs.
 *
 * HONESTY CONTRACT: this tool ALWAYS returns its own
 * coverage counters -- assets_scanned / assets_prefiltered_out / assets_loaded /
 * graphs_visited / truncated -- because an empty `matches` array without them is
 * indistinguishable from "I never loaded anything".
 */
namespace UEAgentSearchCommands
{

// MCP-DOMAIN: Blueprint

// ==========================================================================
// File-local helpers.
// ==========================================================================
namespace
{
    // ---- The search query, resolved once per call ------------------------
    struct FSearchQuery_Search
    {
        FString  Symbol;
        bool     bCaseSensitive = false;
        FString  Mode = TEXT("auto");
        bool     bIncludePinDefaults = true;
        // Resolved from owner_blueprint + component_name (both optional). When set,
        // a GetComponentByClass whose class pin defaults to this class counts as a
        // reference to the component even though the component NAME never appears.
        UClass*  ComponentClass = nullptr;

        bool NameEq(const FString& In) const
        {
            return In.Equals(Symbol, bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
        }
        bool NameEq(const FName& In) const
        {
            return In != NAME_None && NameEq(In.ToString());
        }
        bool SubstringOf(const FString& In) const
        {
            return !In.IsEmpty() && In.Contains(Symbol, bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
        }
    };

    // ---- One classified reference to the symbol on one node --------------
    struct FNodeMatch_Search
    {
        FString Kind;
        FString MemberName;
        FString DeclaringClass;
        FString Detail;
        FString PinName;
        FString PinValue;
        bool    bResolved = false;      // the engine resolved the member (not a name-only guess)
        bool    bComponentRef = false;  // the referenced member IS an actor component
        bool    bInterfaceCall = false;
        bool    bExact = true;          // false => substring hit inside a pin default
    };

    bool IsKnownMode_Search(const FString& Mode)
    {
        return Mode == TEXT("auto") || Mode == TEXT("all") || Mode == TEXT("variable") ||
               Mode == TEXT("function") || Mode == TEXT("event") || Mode == TEXT("delegate") ||
               Mode == TEXT("macro") || Mode == TEXT("component") || Mode == TEXT("class") ||
               Mode == TEXT("pin_default");
    }

    bool ModeAllows_Search(const FSearchQuery_Search& Q, const FNodeMatch_Search& M)
    {
        const FString& Mode = Q.Mode;
        if (Mode == TEXT("auto") || Mode == TEXT("all"))    { return true; }
        if (Mode == TEXT("variable"))    { return M.Kind == TEXT("variable_get") || M.Kind == TEXT("variable_set"); }
        if (Mode == TEXT("function"))    { return M.Kind == TEXT("function_call") || M.Kind == TEXT("get_component_by_class"); }
        if (Mode == TEXT("event"))       { return M.Kind == TEXT("event") || M.Kind == TEXT("event_ref") || M.Kind == TEXT("custom_event") || M.Kind == TEXT("component_bound_event"); }
        if (Mode == TEXT("delegate"))    { return M.Kind == TEXT("delegate") || M.Kind == TEXT("delegate_binding") || M.Kind == TEXT("component_bound_event"); }
        if (Mode == TEXT("macro"))       { return M.Kind == TEXT("macro_instance"); }
        // `component`: a proven component reference, PLUS the name-matching variable
        // nodes whose member did NOT resolve. Dropping those would be a silent false
        // negative (we cannot prove they are NOT the component); they ship with
        // resolved:false + is_component_ref:false so the caller can see the doubt.
        if (Mode == TEXT("component"))   { return M.bComponentRef || (!M.bResolved && (M.Kind == TEXT("variable_get") || M.Kind == TEXT("variable_set"))); }
        if (Mode == TEXT("class"))       { return M.Kind == TEXT("cast") || M.Kind == TEXT("get_component_by_class") || M.Kind == TEXT("pin_default_object") || M.Kind == TEXT("pin_type"); }
        if (Mode == TEXT("pin_default")) { return M.Kind == TEXT("pin_default") || M.Kind == TEXT("pin_default_object") || M.Kind == TEXT("pin_type"); }
        // Unknown mode: never silently swallow results -- the caller is told via
        // `mode_recognized:false` in the response and gets the unfiltered set.
        return true;
    }

    FString ClassPathOrEmpty_Search(const UClass* InClass)
    {
        return InClass ? InClass->GetPathName() : FString();
    }

    // The non-empty strings of Values (absent when null), each without one trailing '/'.
    TArray<FString> PathPrefixes_Search(const TArray<TSharedPtr<FJsonValue>>* Values)
    {
        TArray<FString> Prefixes;
        if (Values)
        {
            for (const TSharedPtr<FJsonValue>& V : *Values)
            {
                FString S;
                if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
                {
                    S.RemoveFromEnd(TEXT("/"));
                    Prefixes.Add(S);
                }
            }
        }
        return Prefixes;
    }

    bool IsComponentClass_Search(const UClass* InClass)
    {
        return InClass && InClass->IsChildOf(UActorComponent::StaticClass());
    }

    // ---- Byte-level .uasset prefilter (the name-map oracle) --------------
    // An FName can only be served from a package if its STRING lives in that
    // package's name map. So: no occurrence of the symbol in the raw bytes =>
    // no node in that package can possibly reference it => skip the load.
    // We search the whole file (never a prefix) because the name map's extent
    // is not knowable without parsing the summary, and a truncated read would
    // manufacture false negatives -- the exact failure mode this tool exists
    // to kill.
    int64 CountOccurrences_Search(const TArray<uint8>& Hay, const TArray<uint8>& NeedleNormalized, bool bCaseSensitive)
    {
        const int32 N = NeedleNormalized.Num();
        if (N == 0 || Hay.Num() < N)
        {
            return 0;
        }
        const uint8* H = Hay.GetData();
        const uint8* Nd = NeedleNormalized.GetData();
        const int32 Last = Hay.Num() - N;
        int64 Count = 0;
        for (int32 i = 0; i <= Last; ++i)
        {
            uint8 C = H[i];
            if (!bCaseSensitive && C >= 'A' && C <= 'Z') { C = static_cast<uint8>(C + 32); }
            if (C != Nd[0]) { continue; }
            int32 j = 1;
            for (; j < N; ++j)
            {
                uint8 D = H[i + j];
                if (!bCaseSensitive && D >= 'A' && D <= 'Z') { D = static_cast<uint8>(D + 32); }
                if (D != Nd[j]) { break; }
            }
            if (j == N) { ++Count; }
        }
        return Count;
    }

    // Build the two byte patterns a name map can hold: 8-bit (ANSI) and UTF-16LE.
    void BuildNeedles_Search(const FString& Symbol, bool bCaseSensitive, TArray<uint8>& OutAnsi, TArray<uint8>& OutUtf16)
    {
        OutAnsi.Reset();
        OutUtf16.Reset();
        bool bAllAscii = true;
        for (int32 i = 0; i < Symbol.Len(); ++i)
        {
            const TCHAR Ch = Symbol[i];
            uint32 Code = static_cast<uint32>(Ch);
            if (!bCaseSensitive && Code >= 'A' && Code <= 'Z') { Code += 32; }
            if (Code > 0x7F) { bAllAscii = false; }
            if (Code <= 0xFF)
            {
                OutAnsi.Add(static_cast<uint8>(Code));
            }
            OutUtf16.Add(static_cast<uint8>(Code & 0xFF));
            OutUtf16.Add(static_cast<uint8>((Code >> 8) & 0xFF));
        }
        // A non-ASCII symbol cannot be matched byte-for-byte in the 8-bit form
        // without knowing the encoding -- drop that needle rather than guess.
        if (!bAllAscii)
        {
            OutAnsi.Reset();
        }
    }

    // Returns false when the file could not be read at all (caller must then
    // treat the prefilter as UNAVAILABLE and load the asset anyway).
    bool ScanFileForSymbol_Search(const FString& FilePath, const TArray<uint8>& Ansi, const TArray<uint8>& Utf16,
                                  bool bCaseSensitive, int64& OutOccurrences)
    {
        OutOccurrences = 0;
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
        {
            return false;
        }
        OutOccurrences  = CountOccurrences_Search(Bytes, Ansi, bCaseSensitive);
        OutOccurrences += CountOccurrences_Search(Bytes, Utf16, bCaseSensitive);
        return true;
    }

    // Resolve a package name to its on-disk file. Mirrors the resolver already
    // proven in Asset/UEAgentAssetUtilCommands.cpp (ResolveScanFilePath_AssetUtil).
    bool PackageToFilename_Search(const FString& PackageName, FString& OutFile)
    {
        FString Base;
        if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, Base))
        {
            return false;
        }
        for (const TCHAR* Ext : { TEXT(".uasset"), TEXT(".umap") })
        {
            const FString Candidate = Base + Ext;
            if (FPaths::FileExists(Candidate))
            {
                OutFile = Candidate;
                return true;
            }
        }
        return false;
    }

    // ---- Component-class resolution for the `component_name` mode --------
    // Walks the SCS of the owner and of its Blueprint ancestors, then the native
    // CDO components. Returns the component's class + where it was declared.
    UClass* ResolveComponentClass_Search(UBlueprint* OwnerBP, const FName CompName, FString& OutDeclaredIn)
    {
        for (UBlueprint* Cur = OwnerBP; Cur != nullptr; )
        {
            if (Cur->SimpleConstructionScript)
            {
                for (USCS_Node* Node : Cur->SimpleConstructionScript->GetAllNodes())
                {
                    if (Node && Node->GetVariableName() == CompName)
                    {
                        OutDeclaredIn = Cur->GetPathName();
                        if (Node->ComponentClass)
                        {
                            return Node->ComponentClass.Get();
                        }
                        return Node->ComponentTemplate ? Node->ComponentTemplate->GetClass() : nullptr;
                    }
                }
            }
            UClass* ParentClass = Cur->ParentClass;
            Cur = ParentClass ? Cast<UBlueprint>(ParentClass->ClassGeneratedBy) : nullptr;
        }
        if (OwnerBP)
        {
            UClass* GenClass = OwnerBP->GeneratedClass;
            AActor* CDO = GenClass ? Cast<AActor>(GenClass->GetDefaultObject(false)) : nullptr;
            if (CDO)
            {
                for (UActorComponent* Comp : CDO->GetComponents())
                {
                    if (Comp && Comp->GetFName() == CompName)
                    {
                        OutDeclaredIn = TEXT("native_cdo");
                        return Comp->GetClass();
                    }
                }
            }
        }
        return nullptr;
    }

    // ---- Pin-side helpers ------------------------------------------------
    bool PinClassDefaultMatches_Search(const UEdGraphPin* Pin, const FSearchQuery_Search& Q, FString& OutValue, const UClass*& OutClass)
    {
        OutClass = nullptr;
        if (!Pin)
        {
            return false;
        }
        if (const UClass* AsClass = Cast<UClass>(Pin->DefaultObject))
        {
            OutValue = AsClass->GetPathName();
            OutClass = AsClass;
            if (Q.ComponentClass && (AsClass == Q.ComponentClass || AsClass->IsChildOf(Q.ComponentClass)))
            {
                return true;
            }
            // Match the bare class name AND the _C-suffixed generated-class name,
            // so "AC_MyComponent" hits a pin defaulted to AC_MyComponent_C.
            FString Bare = AsClass->GetName();
            Bare.RemoveFromEnd(TEXT("_C"));
            return Q.NameEq(AsClass->GetName()) || Q.NameEq(Bare);
        }
        return false;
    }

    // ---- The classifier: RTTI first, pin defaults only as a last resort --
    void CollectNodeMatches_Search(UEdGraphNode* Node, UClass* SelfScope, const FSearchQuery_Search& Q,
                                   TArray<FNodeMatch_Search>& Out)
    {
        if (!Node)
        {
            return;
        }
        const int32 StructuralStart = Out.Num();

        // (1) Variables. Resolve the FProperty (never a raw string compare) so a
        //     same-named variable on an unrelated class is distinguishable, and so
        //     declaring_class is real. Falls back to the serialized member name --
        //     flagged resolved:false -- when the member no longer resolves.
        if (UK2Node_Variable* VarNode = Cast<UK2Node_Variable>(Node))
        {
            FProperty* Prop = SelfScope ? VarNode->VariableReference.ResolveMember<FProperty>(SelfScope) : nullptr;
            const FName MemberName = VarNode->VariableReference.GetMemberName();
            const bool bHit = Prop ? Q.NameEq(Prop->GetFName()) : Q.NameEq(MemberName);
            if (bHit)
            {
                FNodeMatch_Search M;
                M.Kind = Cast<UK2Node_VariableSet>(Node) ? TEXT("variable_set") : TEXT("variable_get");
                M.MemberName = Prop ? Prop->GetName() : MemberName.ToString();
                M.bResolved = (Prop != nullptr);
                if (Prop)
                {
                    M.DeclaringClass = ClassPathOrEmpty_Search(Prop->GetOwnerClass());
                    if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
                    {
                        M.bComponentRef = IsComponentClass_Search(ObjProp->PropertyClass.Get());
                    }
                }
                else
                {
                    M.DeclaringClass = ClassPathOrEmpty_Search(VarNode->VariableReference.GetMemberParentClass(SelfScope));
                    M.Detail = TEXT("member did not resolve on the skeleton class (stale or renamed reference)");
                }
                Out.Add(M);
            }
        }

        // (2) Function calls (+ the GetComponentByClass family, whose reference to
        //     a component lives in the CLASS PIN, not in the function name).
        if (UK2Node_CallFunction* CallNode = Cast<UK2Node_CallFunction>(Node))
        {
            UFunction* Func = CallNode->GetTargetFunction();
            const FName MemberName = CallNode->FunctionReference.GetMemberName();
            UClass* ParentClass = CallNode->FunctionReference.GetMemberParentClass();
            const bool bHit = Func ? Q.NameEq(Func->GetFName()) : Q.NameEq(MemberName);
            if (bHit)
            {
                FNodeMatch_Search M;
                M.Kind = TEXT("function_call");
                M.MemberName = Func ? Func->GetName() : MemberName.ToString();
                M.bResolved = (Func != nullptr);
                M.DeclaringClass = ClassPathOrEmpty_Search(Func ? Func->GetOwnerClass() : ParentClass);
                // bIsInterfaceCall is deprecated -- the engine's own
                // replacement is CLASS_Interface on the member parent class.
                M.bInterfaceCall = ParentClass && ParentClass->HasAnyClassFlags(CLASS_Interface);
                Out.Add(M);
            }

            const FString FuncName = Func ? Func->GetName() : MemberName.ToString();
            if (FuncName.Contains(TEXT("GetComponentByClass")) || FuncName.Contains(TEXT("GetComponentsByClass")) ||
                FuncName.Contains(TEXT("AddComponentByClass")) || FuncName.Contains(TEXT("FindComponentByClass")))
            {
                for (UEdGraphPin* Pin : CallNode->Pins)
                {
                    FString ClassValue;
                    const UClass* MatchedClass = nullptr;
                    if (Pin && Pin->Direction == EGPD_Input && PinClassDefaultMatches_Search(Pin, Q, ClassValue, MatchedClass))
                    {
                        FNodeMatch_Search M;
                        M.Kind = TEXT("get_component_by_class");
                        M.MemberName = FuncName;
                        M.bResolved = (Func != nullptr);
                        M.bComponentRef = true;
                        M.DeclaringClass = ClassPathOrEmpty_Search(Func ? Func->GetOwnerClass() : ParentClass);
                        M.PinName = Pin->PinName.ToString();
                        M.PinValue = ClassValue;
                        Out.Add(M);
                    }
                }
            }
        }

        // (3) Macro instances.
        if (UK2Node_MacroInstance* MacroNode = Cast<UK2Node_MacroInstance>(Node))
        {
            if (UEdGraph* MacroGraph = MacroNode->GetMacroGraph())
            {
                if (Q.NameEq(MacroGraph->GetFName()))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("macro_instance");
                    M.MemberName = MacroGraph->GetName();
                    M.bResolved = true;
                    if (UBlueprint* MacroOwner = MacroGraph->GetTypedOuter<UBlueprint>())
                    {
                        M.Detail = FString::Printf(TEXT("macro declared in %s"), *MacroOwner->GetPathName());
                    }
                    Out.Add(M);
                }
            }
        }

        // (4) Component-bound events (checked BEFORE the generic event branch: a
        //     UK2Node_ComponentBoundEvent IS-A UK2Node_Event and would otherwise
        //     also report its delegate signature as a plain event).
        bool bHandledAsComponentBound = false;
        if (UK2Node_ComponentBoundEvent* BoundEvent = Cast<UK2Node_ComponentBoundEvent>(Node))
        {
            if (Q.NameEq(BoundEvent->ComponentPropertyName))
            {
                FNodeMatch_Search M;
                M.Kind = TEXT("component_bound_event");
                M.MemberName = BoundEvent->ComponentPropertyName.ToString();
                M.bResolved = true;
                M.bComponentRef = true;
                M.Detail = FString::Printf(TEXT("bound to delegate %s"), *BoundEvent->DelegatePropertyName.ToString());
                M.DeclaringClass = ClassPathOrEmpty_Search(BoundEvent->DelegateOwnerClass.Get());
                Out.Add(M);
                bHandledAsComponentBound = true;
            }
            else if (Q.NameEq(BoundEvent->DelegatePropertyName))
            {
                FNodeMatch_Search M;
                M.Kind = TEXT("component_bound_event");
                M.MemberName = BoundEvent->DelegatePropertyName.ToString();
                M.bResolved = true;
                M.Detail = FString::Printf(TEXT("on component %s"), *BoundEvent->ComponentPropertyName.ToString());
                M.DeclaringClass = ClassPathOrEmpty_Search(BoundEvent->DelegateOwnerClass.Get());
                Out.Add(M);
                bHandledAsComponentBound = true;
            }
        }

        // (5) Events / custom events.
        if (!bHandledAsComponentBound)
        {
            if (UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node))
            {
                const FName MemberName = EventNode->EventReference.GetMemberName();
                if (Q.NameEq(MemberName))
                {
                    FNodeMatch_Search M;
                    M.Kind = EventNode->bOverrideFunction ? TEXT("event") : TEXT("event_ref");
                    M.MemberName = MemberName.ToString();
                    M.bResolved = true;
                    M.DeclaringClass = ClassPathOrEmpty_Search(EventNode->EventReference.GetMemberParentClass());
                    Out.Add(M);
                }
                else if (Q.NameEq(EventNode->CustomFunctionName))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("custom_event");
                    M.MemberName = EventNode->CustomFunctionName.ToString();
                    M.bResolved = true;
                    Out.Add(M);
                }
            }
        }

        // (6) Multicast delegates (Call/Add/Remove/Clear/Assign) + Create Delegate.
        if (UK2Node_BaseMCDelegate* DelegateNode = Cast<UK2Node_BaseMCDelegate>(Node))
        {
            const FName PropName = DelegateNode->GetPropertyName();
            if (Q.NameEq(PropName))
            {
                FNodeMatch_Search M;
                M.Kind = TEXT("delegate");
                M.MemberName = PropName.ToString();
                M.bResolved = (DelegateNode->GetProperty() != nullptr);
                M.DeclaringClass = ClassPathOrEmpty_Search(DelegateNode->DelegateReference.GetMemberParentClass());
                Out.Add(M);
            }
        }
        if (UK2Node_CreateDelegate* CreateDelegateNode = Cast<UK2Node_CreateDelegate>(Node))
        {
            const FName BoundFunc = CreateDelegateNode->GetFunctionName();
            if (Q.NameEq(BoundFunc))
            {
                FNodeMatch_Search M;
                M.Kind = TEXT("delegate_binding");
                M.MemberName = BoundFunc.ToString();
                M.bResolved = true;
                Out.Add(M);
            }
        }

        // (7) Timelines.
        if (UK2Node_Timeline* TimelineNode = Cast<UK2Node_Timeline>(Node))
        {
            if (Q.NameEq(TimelineNode->TimelineName))
            {
                FNodeMatch_Search M;
                M.Kind = TEXT("timeline");
                M.MemberName = TimelineNode->TimelineName.ToString();
                M.bResolved = true;
                Out.Add(M);
            }
        }

        // (8) Casts.
        if (UK2Node_DynamicCast* CastNode = Cast<UK2Node_DynamicCast>(Node))
        {
            if (UClass* Target = CastNode->TargetType)
            {
                FString Bare = Target->GetName();
                Bare.RemoveFromEnd(TEXT("_C"));
                if (Q.NameEq(Target->GetName()) || Q.NameEq(Bare) ||
                    (Q.ComponentClass && (Target == Q.ComponentClass || Target->IsChildOf(Q.ComponentClass))))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("cast");
                    M.MemberName = Target->GetName();
                    M.bResolved = true;
                    M.bComponentRef = IsComponentClass_Search(Target);
                    M.DeclaringClass = Target->GetPathName();
                    Out.Add(M);
                }
            }
        }

        // (9) Object literals (level-Blueprint actor references).
        if (UK2Node_Literal* LiteralNode = Cast<UK2Node_Literal>(Node))
        {
            if (UObject* Ref = LiteralNode->GetObjectRef())
            {
                if (Q.NameEq(Ref->GetFName()))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("object_literal");
                    M.MemberName = Ref->GetName();
                    M.bResolved = true;
                    M.Detail = Ref->GetPathName();
                    Out.Add(M);
                }
            }
        }

        // (10) Pin defaults + pin types. Only when NO structural match was found on
        //      this node, so a matched function call is not re-reported once per pin.
        if (Q.bIncludePinDefaults && Out.Num() == StructuralStart)
        {
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin)
                {
                    continue;
                }
                FString ClassValue;
                const UClass* MatchedClass = nullptr;
                if (PinClassDefaultMatches_Search(Pin, Q, ClassValue, MatchedClass))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("pin_default_object");
                    M.MemberName = ClassValue;
                    M.bResolved = true;
                    M.bComponentRef = IsComponentClass_Search(MatchedClass);
                    M.PinName = Pin->PinName.ToString();
                    M.PinValue = ClassValue;
                    Out.Add(M);
                    continue;
                }
                if (Pin->DefaultObject && Q.NameEq(Pin->DefaultObject->GetFName()))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("pin_default_object");
                    M.MemberName = Pin->DefaultObject->GetName();
                    M.bResolved = true;
                    M.PinName = Pin->PinName.ToString();
                    M.PinValue = Pin->DefaultObject->GetPathName();
                    Out.Add(M);
                    continue;
                }
                if (const UObject* SubCat = Pin->PinType.PinSubCategoryObject.Get())
                {
                    FString Bare = SubCat->GetName();
                    Bare.RemoveFromEnd(TEXT("_C"));
                    if (Q.NameEq(SubCat->GetFName()) || Q.NameEq(Bare))
                    {
                        FNodeMatch_Search M;
                        M.Kind = TEXT("pin_type");
                        M.MemberName = SubCat->GetName();
                        M.bResolved = true;
                        M.PinName = Pin->PinName.ToString();
                        M.PinValue = SubCat->GetPathName();
                        Out.Add(M);
                        continue;
                    }
                }
                const FString& DefaultValue = Pin->DefaultValue;
                if (!DefaultValue.IsEmpty() && Q.SubstringOf(DefaultValue))
                {
                    FNodeMatch_Search M;
                    M.Kind = TEXT("pin_default");
                    M.MemberName = DefaultValue;
                    M.bResolved = false;
                    M.bExact = Q.NameEq(DefaultValue);
                    M.PinName = Pin->PinName.ToString();
                    M.PinValue = DefaultValue.Left(256);
                    Out.Add(M);
                }
            }
        }

        // Mode filter is applied last so the classifier stays honest about what it
        // actually saw (and so `component` mode can key off bComponentRef).
        for (int32 i = Out.Num() - 1; i >= StructuralStart; --i)
        {
            if (!ModeAllows_Search(Q, Out[i]))
            {
                Out.RemoveAt(i);
            }
        }
    }

    // ---- Graph traversal -------------------------------------------------
    struct FGraphVisit_Search
    {
        UEdGraph* Graph = nullptr;
        FString   Breadcrumb;
        FString   Kind;
    };

    void SeedRoots_Search(UBlueprint* BP, TArray<FGraphVisit_Search>& OutQueue)
    {
        // The root set of GraphDeep_ResolveSingle (Common/UEAgentGraphUtils.cpp):
        // Ubergraph + Function + Macro + DelegateSignature + ImplementedInterfaces[].Graphs.
        // NOT the narrower lint_blueprint_graph scope, which drops interfaces and composites.
        auto AddAll = [&OutQueue](const TArray<TObjectPtr<UEdGraph>>& Graphs, const TCHAR* Kind)
        {
            for (UEdGraph* G : Graphs)
            {
                if (G)
                {
                    FGraphVisit_Search V;
                    V.Graph = G;
                    V.Breadcrumb = G->GetName();
                    V.Kind = Kind;
                    OutQueue.Add(V);
                }
            }
        };
        AddAll(BP->UbergraphPages, TEXT("ubergraph"));
        AddAll(BP->FunctionGraphs, TEXT("function"));
        AddAll(BP->MacroGraphs, TEXT("macro"));
        AddAll(BP->DelegateSignatureGraphs, TEXT("delegate_signature"));
        for (const FBPInterfaceDescription& InterfaceDesc : BP->ImplementedInterfaces)
        {
            AddAll(InterfaceDesc.Graphs, TEXT("interface"));
        }
    }
}

// IMPLEMENTATION NOTES (not part of the MCP description below -- the parser stops at the first
// non-annotation line, and a description must live on ONE line):
//   * Enumeration is Asset-Registry-by-path, never GetAssetsByClass with bSearchSubClasses, which
//     hangs on a 29k-asset registry.
//   * Every candidate is BYTE-PREFILTERED against the raw .uasset name map before being loaded: an
//     FName cannot be served from a package whose name map lacks its string, so a 0-occurrence
//     package provably cannot reference the symbol. A package that is DIRTY in memory is never
//     prefiltered (the disk bytes are stale) and an unreadable file is loaded anyway, never skipped.
//   * Graph roots are the FULL set (Ubergraph + Function + Macro + DelegateSignature + implemented
//     interface graphs), recursing into collapsed composites and same-Blueprint macro graphs.
//   * match_kind is derived by RTTI and the member is RESOLVED (FMemberReference::ResolveMember /
//     UK2Node_CallFunction::GetTargetFunction), never string-compared, so `declaring_class` is real
//     and a same-named member on an unrelated class is distinguishable.
// MCP:find_blueprint_node_references - "Find in Blueprints" over MCP: locate every Blueprint NODE that references a symbol -- variable, function, event, custom event, multicast delegate, macro, timeline, cast target, component, or a class/object in a pin default -- across a content path, returning each hit's asset, graph chain, node_id, title, match_kind and RESOLVED declaring class (resolved:false = a name-only guess). READ-ONLY. THE RULE FOR READING A ZERO: matches_found:0 only measures NON-EXISTENCE when `assets_hit_prefilter_without_graph_match` comes back EMPTY -- if it names assets, the symbol's string IS in those packages, so either the real name is spelled differently or the reference lives outside a graph (component template, property default). Check coverage_complete too: a truncated, timed-out or capped scan proves nothing. Blind by construction: Level Script Blueprints inside a .umap, and C++ callers.
//   @param symbol - Required (unless component_name is given): the name to look for, matched EXACTLY (variable/function/event/macro/class/component name). A misspelling returns 0 with a full-coverage report -- see assets_hit_prefilter_without_graph_match before believing it.
//   @param component_name - Optional: search for an actor COMPONENT by name; implies mode="component" and, with owner_blueprint, also matches GetComponentByClass/Cast pins that target the component's class.
//   @param owner_blueprint - Optional: Blueprint that DECLARES component_name (asset name or path); required to resolve the component's class for the class-pin matching.
//   @param mode - Optional (default "auto"): auto|all|variable|function|event|delegate|macro|component|class|pin_default. Filters which match kinds are returned; an unrecognized value returns everything and sets mode_recognized=false.
//   @param path - Optional (default "/Game"): content path root to scan.
//   @param recursive - Optional (default true): recurse into subfolders of path.
//   @param exclude_paths - Optional: array of package-path prefixes to skip (e.g. ["/Game/VendorPack/Demo"]).
//   @param include_paths - Optional: array of package-path prefixes to restrict the scan to (applied after path).
//   @param prefilter - Optional (default true): byte-scan the .uasset name map before loading. Turning it OFF loads every Blueprint under path (slow, but the ground truth to compare against).
//   @param case_sensitive - Optional (default false): case-sensitive symbol matching (both the byte prefilter and the node classifier).
//   @param include_pin_defaults - Optional (default true): also report pin defaults / pin types that name the symbol, on nodes with no structural match.
//   @param include_node_json - Optional (default false): embed the full NodeToJson serialization of each matching node.
//   @param include_pins - Optional (default false): when include_node_json is on, include the node's pins.
//   @param max_assets - Optional (default 20000): hard cap on candidate assets examined.
//   @param max_matches - Optional (default 200): stop after this many matches (sets truncated=true).
//   @param max_seconds - Optional (default 60): wall-clock budget; on expiry the scan stops and timed_out=true.
//   @returns { symbol, mode, mode_recognized, path, recursive, prefilter_enabled, component_class, component_declared_in, assets_in_path, assets_scanned, assets_prefiltered_out, assets_prefilter_unavailable, assets_prefilter_bypassed_dirty, assets_loaded, assets_load_failed, assets_with_matches, graphs_visited, nodes_visited, foreign_subgraphs_skipped, matches_found, truncated, timed_out, coverage_complete, elapsed_ms, assets_hit_prefilter_without_graph_match:[string], notes:[string], matches:[{asset_name, asset_path, package_name, graph, graph_chain, graph_kind, node_id, node_class, node_title, match_kind, member_name, declaring_class, resolved, is_component_ref, is_interface_call, exact, detail, pin, pin_value, node?}], success }
TSharedPtr<FJsonObject> HandleFindBlueprintNodeReferences(const TSharedPtr<FJsonObject>& Params)
{
    const double StartSeconds = FPlatformTime::Seconds();

    if (!Params.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing parameters object"));
    }

    // ---- Parameters ------------------------------------------------------
    // What to look for, and where. All optional here; the mode logic below decides
    // which combinations are valid.
    FString Symbol, ComponentName, OwnerBlueprintName, Mode, FolderPath;
    Params->TryGetStringField(TEXT("symbol"), Symbol);
    Params->TryGetStringField(TEXT("component_name"), ComponentName);
    Params->TryGetStringField(TEXT("owner_blueprint"), OwnerBlueprintName);
    Params->TryGetStringField(TEXT("mode"), Mode);
    Params->TryGetStringField(TEXT("path"), FolderPath);

    // Switches, each starting at its documented default.
    bool bRecursive = true, bPrefilter = true, bCaseSensitive = false;
    bool bIncludePinDefaults = true, bIncludeNodeJson = false, bIncludePins = false;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);
    Params->TryGetBoolField(TEXT("prefilter"), bPrefilter);
    Params->TryGetBoolField(TEXT("case_sensitive"), bCaseSensitive);
    Params->TryGetBoolField(TEXT("include_pin_defaults"), bIncludePinDefaults);
    Params->TryGetBoolField(TEXT("include_node_json"), bIncludeNodeJson);
    Params->TryGetBoolField(TEXT("include_pins"), bIncludePins);

    double MaxAssetsD = 20000.0;
    Params->TryGetNumberField(TEXT("max_assets"), MaxAssetsD);
    double MaxMatchesD = 200.0;
    Params->TryGetNumberField(TEXT("max_matches"), MaxMatchesD);
    double MaxSecondsD = 60.0;
    Params->TryGetNumberField(TEXT("max_seconds"), MaxSecondsD);

    const int32 MaxAssets  = FMath::Clamp(static_cast<int32>(MaxAssetsD), 1, 1000000);
    const int32 MaxMatches = FMath::Clamp(static_cast<int32>(MaxMatchesD), 1, 100000);
    const double MaxSeconds = FMath::Clamp(MaxSecondsD, 1.0, 3600.0);

    const TArray<TSharedPtr<FJsonValue>>* ExcludeArray = nullptr;
    Params->TryGetArrayField(TEXT("exclude_paths"), ExcludeArray);
    const TArray<FString> ExcludePaths = PathPrefixes_Search(ExcludeArray);
    const TArray<TSharedPtr<FJsonValue>>* IncludeArray = nullptr;
    Params->TryGetArrayField(TEXT("include_paths"), IncludeArray);
    const TArray<FString> IncludePaths = PathPrefixes_Search(IncludeArray);

    if (FolderPath.IsEmpty())
    {
        FolderPath = TEXT("/Game");
    }
    FolderPath.RemoveFromEnd(TEXT("/"));

    Mode = Mode.IsEmpty() ? TEXT("auto") : Mode.ToLower();
    if (!ComponentName.IsEmpty())
    {
        if (Symbol.IsEmpty())          { Symbol = ComponentName; }
        if (Mode == TEXT("auto"))      { Mode = TEXT("component"); }
    }
    if (Symbol.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: symbol (or component_name)"));
    }

    TArray<TSharedPtr<FJsonValue>> Notes;
    const bool bModeRecognized = IsKnownMode_Search(Mode);
    if (!bModeRecognized)
    {
        Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("mode '%s' is not one of auto|all|variable|function|event|delegate|macro|component|class|pin_default -- returning the UNFILTERED match set"), *Mode)));
    }

    FSearchQuery_Search Query;
    Query.Symbol = Symbol;
    Query.bCaseSensitive = bCaseSensitive;
    Query.Mode = Mode;
    Query.bIncludePinDefaults = bIncludePinDefaults;

    // ---- Optional owner_blueprint / component-class resolution -----------
    FString ComponentDeclaredIn;
    if (!OwnerBlueprintName.IsEmpty())
    {
        UBlueprint* OwnerBP = mcp_common::ResolveBlueprint(OwnerBlueprintName);
        if (!OwnerBP)
        {
            OwnerBP = LoadObject<UBlueprint>(nullptr, *OwnerBlueprintName);
        }
        if (!OwnerBP)
        {
            // Fail loudly: owner_blueprint changes what counts as a match, so
            // degrading silently would under-report and look like "no readers".
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("owner_blueprint '%s' not found (it selects which component class counts as a match -- refusing to run a search whose semantics silently changed)"),
                *OwnerBlueprintName));
        }
        if (!ComponentName.IsEmpty())
        {
            Query.ComponentClass = ResolveComponentClass_Search(OwnerBP, FName(*ComponentName), ComponentDeclaredIn);
            if (!Query.ComponentClass)
            {
                Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
                    TEXT("component '%s' was not found on %s (SCS chain + native CDO) -- class-pin matching is DISABLED; only name-based hits are reported"),
                    *ComponentName, *OwnerBP->GetPathName())));
            }
        }
    }
    else if (!ComponentName.IsEmpty())
    {
        Notes.Add(MakeShared<FJsonValueString>(
            TEXT("component_name without owner_blueprint: the component's class is unknown, so GetComponentByClass/Cast pins that target it by CLASS cannot be matched (only by name)")));
    }

    // ---- Asset enumeration ----------------------------------------------
    IAssetRegistry& AR = mcp_common::AssetRegistry();
    // Same guard the audit commands use: avoid partial results while the registry
    // is still background-scanning.
    AR.SearchAllAssets(/*bSynchronousSearch=*/true);

    TArray<FAssetData> AllAssets;
    AR.GetAssetsByPath(FName(*FolderPath), AllAssets, bRecursive);

    // Blueprint-ness WITHOUT GetAssetsByClass(bSearchSubClasses=true) -- that call
    // hangs on this registry size (see Audit/UEAgentAuditCommands.cpp).
    // Resolving the (already-loaded, native) class behind AssetClassPath is cheap and,
    // unlike an `== UBlueprint` equality test, does NOT drop WidgetBlueprint /
    // AnimBlueprint / EditorUtilityBlueprint assets.
    const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();
    TMap<FTopLevelAssetPath, bool> ClassIsBlueprintCache;

    TArray<uint8> NeedleAnsi, NeedleUtf16;
    BuildNeedles_Search(Symbol, bCaseSensitive, NeedleAnsi, NeedleUtf16);
    if (NeedleAnsi.Num() == 0 && bPrefilter)
    {
        Notes.Add(MakeShared<FJsonValueString>(
            TEXT("symbol contains non-ASCII characters: the 8-bit name-map needle was dropped, only the UTF-16 form is prefiltered")));
    }

    // ---- Scan ------------------------------------------------------------
    int32 AssetsScanned = 0;          // blueprint-ish candidates actually examined
    int32 AssetsPrefilteredOut = 0;   // provably cannot reference the symbol -> not loaded
    int32 PrefilterUnavailable = 0;   // file unreadable/absent -> loaded anyway (never skipped)
    int32 PrefilterBypassedDirty = 0; // in-memory package is dirty -> disk copy is stale, loaded anyway
    int32 AssetsLoaded = 0;
    int32 AssetsLoadFailed = 0;
    int32 AssetsWithMatches = 0;
    int32 GraphsVisited = 0;
    int32 NodesVisited = 0;
    int32 ForeignSubgraphsSkipped = 0;
    bool  bTruncated = false;
    bool  bTimedOut = false;
    bool  bAssetCapHit = false;

    TArray<TSharedPtr<FJsonValue>> Matches;
    TArray<TSharedPtr<FJsonValue>> PrefilterHitNoGraphMatch;

    for (const FAssetData& AssetData : AllAssets)
    {
        if (bTruncated || bTimedOut)
        {
            break;
        }
        if (FPlatformTime::Seconds() - StartSeconds > MaxSeconds)
        {
            bTimedOut = true;
            break;
        }

        // -- blueprint-ness (no load) --
        bool bIsBlueprintAsset = false;
        if (const bool* Cached = ClassIsBlueprintCache.Find(AssetData.AssetClassPath))
        {
            bIsBlueprintAsset = *Cached;
        }
        else
        {
            if (AssetData.AssetClassPath == BlueprintClassPath)
            {
                bIsBlueprintAsset = true;
            }
            else if (UClass* AssetClass = FindObject<UClass>(nullptr, *AssetData.AssetClassPath.ToString()))
            {
                bIsBlueprintAsset = AssetClass->IsChildOf(UBlueprint::StaticClass());
            }
            ClassIsBlueprintCache.Add(AssetData.AssetClassPath, bIsBlueprintAsset);
        }
        if (!bIsBlueprintAsset)
        {
            continue;
        }

        const FString PackageName = AssetData.PackageName.ToString();

        bool bExcluded = false;
        for (const FString& Prefix : ExcludePaths)
        {
            if (PackageName.StartsWith(Prefix, ESearchCase::IgnoreCase)) { bExcluded = true; break; }
        }
        if (bExcluded)
        {
            continue;
        }
        if (IncludePaths.Num() > 0)
        {
            bool bIncluded = false;
            for (const FString& Prefix : IncludePaths)
            {
                if (PackageName.StartsWith(Prefix, ESearchCase::IgnoreCase)) { bIncluded = true; break; }
            }
            if (!bIncluded)
            {
                continue;
            }
        }

        if (AssetsScanned >= MaxAssets)
        {
            bAssetCapHit = true;
            break;
        }
        ++AssetsScanned;

        // -- byte prefilter (skippable ONLY when we could actually read the file) --
        bool bMustLoad = true;
        bool bPrefilterHit = false;
        if (bPrefilter)
        {
            UPackage* Resident = FindPackage(nullptr, *PackageName);
            if (Resident && Resident->IsDirty())
            {
                // The on-disk bytes no longer describe what is in memory: prefiltering
                // here would be a manufactured false negative.
                ++PrefilterBypassedDirty;
            }
            else
            {
                FString FileName;
                if (PackageToFilename_Search(PackageName, FileName))
                {
                    int64 Occurrences = 0;
                    if (ScanFileForSymbol_Search(FileName, NeedleAnsi, NeedleUtf16, bCaseSensitive, Occurrences))
                    {
                        if (Occurrences == 0)
                        {
                            ++AssetsPrefilteredOut;
                            bMustLoad = false;
                        }
                        else
                        {
                            bPrefilterHit = true;
                        }
                    }
                    else
                    {
                        ++PrefilterUnavailable;
                    }
                }
                else
                {
                    ++PrefilterUnavailable;
                }
            }
        }
        if (!bMustLoad)
        {
            continue;
        }

        UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
        if (!Blueprint)
        {
            ++AssetsLoadFailed;
            continue;
        }
        ++AssetsLoaded;

        // Same skeleton-first fallback the graph handlers already use
        // (Blueprint/UEAgentGraphCommands.cpp).
        UClass* SelfScope = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;

        TArray<FGraphVisit_Search> Queue;
        SeedRoots_Search(Blueprint, Queue);

        TSet<UEdGraph*> Visited;
        const int32 MatchesBefore = Matches.Num();

        for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
        {
            if (bTruncated || bTimedOut)
            {
                break;
            }
            UEdGraph* Graph = Queue[QueueIndex].Graph;
            if (!Graph || Visited.Contains(Graph))
            {
                continue;
            }
            Visited.Add(Graph);
            ++GraphsVisited;

            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node)
                {
                    continue;
                }
                ++NodesVisited;

                TArray<FNodeMatch_Search> NodeMatches;
                CollectNodeMatches_Search(Node, SelfScope, Query, NodeMatches);
                for (const FNodeMatch_Search& M : NodeMatches)
                {
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
                    Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
                    Entry->SetStringField(TEXT("package_name"), PackageName);
                    Entry->SetStringField(TEXT("graph"), Queue[QueueIndex].Breadcrumb);
                    Entry->SetStringField(TEXT("graph_kind"), Queue[QueueIndex].Kind);
                    {
                        TArray<TSharedPtr<FJsonValue>> Chain;
                        TArray<FString> Parts;
                        Queue[QueueIndex].Breadcrumb.ParseIntoArray(Parts, TEXT(" > "), /*InCullEmpty=*/true);
                        for (const FString& Part : Parts)
                        {
                            Chain.Add(MakeShared<FJsonValueString>(Part));
                        }
                        Entry->SetArrayField(TEXT("graph_chain"), Chain);
                    }
                    Entry->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
                    Entry->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
                    Entry->SetStringField(TEXT("node_title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
                    Entry->SetStringField(TEXT("match_kind"), M.Kind);
                    Entry->SetStringField(TEXT("member_name"), M.MemberName);
                    Entry->SetStringField(TEXT("declaring_class"), M.DeclaringClass);
                    Entry->SetBoolField(TEXT("resolved"), M.bResolved);
                    Entry->SetBoolField(TEXT("is_component_ref"), M.bComponentRef);
                    Entry->SetBoolField(TEXT("is_interface_call"), M.bInterfaceCall);
                    Entry->SetBoolField(TEXT("exact"), M.bExact);
                    Entry->SetStringField(TEXT("detail"), M.Detail);
                    Entry->SetStringField(TEXT("pin"), M.PinName);
                    Entry->SetStringField(TEXT("pin_value"), M.PinValue);
                    if (bIncludeNodeJson)
                    {
                        Entry->SetObjectField(TEXT("node"), FUEAgentGraphUtils::NodeToJson(Node, bIncludePins));
                    }
                    Matches.Add(MakeShared<FJsonValueObject>(Entry));

                    if (Matches.Num() >= MaxMatches)
                    {
                        bTruncated = true;
                        break;
                    }
                }
                if (bTruncated)
                {
                    break;
                }
            }
            if (bTruncated)
            {
                break;
            }

            // -- descend: collapsed composites (UEdGraph::SubGraphs) + macro graphs --
            TArray<UEdGraph*> Children;
            for (UEdGraph* Sub : Graph->SubGraphs)
            {
                if (Sub) { Children.AddUnique(Sub); }
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (UEdGraph* NodeGraph = FUEAgentGraphUtils::GetNodeSubGraph(Node))
                {
                    Children.AddUnique(NodeGraph);
                }
            }
            for (UEdGraph* Child : Children)
            {
                // A macro instance can point at a macro graph owned by ANOTHER
                // Blueprint (a macro library). Walking it here would attribute that
                // library's nodes to THIS asset, so it is skipped and counted --
                // scan the library itself (it is its own asset) to see inside it.
                if (Child->GetTypedOuter<UBlueprint>() != Blueprint)
                {
                    ++ForeignSubgraphsSkipped;
                    continue;
                }
                if (Visited.Contains(Child))
                {
                    continue;
                }
                FGraphVisit_Search V;
                V.Graph = Child;
                V.Breadcrumb = Queue[QueueIndex].Breadcrumb + TEXT(" > ") + Child->GetName();
                V.Kind = Queue[QueueIndex].Kind;
                Queue.Add(V);
            }
        }

        if (Matches.Num() > MatchesBefore)
        {
            ++AssetsWithMatches;
        }
        else if (bPrefilterHit && PrefilterHitNoGraphMatch.Num() < 50)
        {
            // The symbol's string IS in this package's bytes but no graph node
            // referenced it. Surfacing this is the whole point: it is either a
            // non-graph reference (component template, property default, DataTable)
            // or a hole in the classifier -- both must be visible, not swallowed.
            PrefilterHitNoGraphMatch.Add(MakeShared<FJsonValueString>(PackageName));
        }
    }

    // ---- Response --------------------------------------------------------
    const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
    const bool bCoverageComplete = !bTruncated && !bTimedOut && !bAssetCapHit &&
                                   AssetsLoadFailed == 0 && PrefilterUnavailable == 0;

    if (bAssetCapHit)
    {
        Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("max_assets (%d) reached -- the scan is INCOMPLETE; raise max_assets or narrow `path`"), MaxAssets)));
    }
    if (bTimedOut)
    {
        Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("max_seconds (%.0f) reached -- the scan is INCOMPLETE; raise max_seconds or narrow `path`"), MaxSeconds)));
    }
    if (PrefilterUnavailable > 0)
    {
        Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%d package(s) could not be byte-prefiltered (file missing/unreadable) -- they were LOADED anyway, never skipped"), PrefilterUnavailable)));
    }
    if (AssetsLoadFailed > 0)
    {
        Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%d candidate asset(s) failed to load and were NOT searched"), AssetsLoadFailed)));
    }
    if (AssetsScanned == 0)
    {
        Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("ZERO Blueprint assets were examined under '%s' (%d asset(s) of any type there): the path is probably wrong or the filters excluded everything -- an empty `matches` here says NOTHING about the symbol"),
            *FolderPath, AllAssets.Num())));
    }
    Notes.Add(MakeShared<FJsonValueString>(
        TEXT("scope: Blueprint assets in the Asset Registry under `path`. Level Script Blueprints living inside a .umap are NOT enumerated, and C++ callers are out of scope by construction")));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("symbol"), Symbol);
    Data->SetStringField(TEXT("mode"), Mode);
    Data->SetBoolField(TEXT("mode_recognized"), bModeRecognized);
    Data->SetStringField(TEXT("path"), FolderPath);
    Data->SetBoolField(TEXT("recursive"), bRecursive);
    Data->SetBoolField(TEXT("prefilter_enabled"), bPrefilter);
    Data->SetBoolField(TEXT("case_sensitive"), bCaseSensitive);
    Data->SetStringField(TEXT("component_class"), ClassPathOrEmpty_Search(Query.ComponentClass));
    Data->SetStringField(TEXT("component_declared_in"), ComponentDeclaredIn);
    Data->SetNumberField(TEXT("assets_in_path"), AllAssets.Num());
    Data->SetNumberField(TEXT("assets_scanned"), AssetsScanned);
    Data->SetNumberField(TEXT("assets_prefiltered_out"), AssetsPrefilteredOut);
    Data->SetNumberField(TEXT("assets_prefilter_unavailable"), PrefilterUnavailable);
    Data->SetNumberField(TEXT("assets_prefilter_bypassed_dirty"), PrefilterBypassedDirty);
    Data->SetNumberField(TEXT("assets_loaded"), AssetsLoaded);
    Data->SetNumberField(TEXT("assets_load_failed"), AssetsLoadFailed);
    Data->SetNumberField(TEXT("assets_with_matches"), AssetsWithMatches);
    Data->SetNumberField(TEXT("graphs_visited"), GraphsVisited);
    Data->SetNumberField(TEXT("nodes_visited"), NodesVisited);
    Data->SetNumberField(TEXT("foreign_subgraphs_skipped"), ForeignSubgraphsSkipped);
    Data->SetNumberField(TEXT("matches_found"), Matches.Num());
    Data->SetBoolField(TEXT("truncated"), bTruncated);
    Data->SetBoolField(TEXT("timed_out"), bTimedOut);
    Data->SetBoolField(TEXT("asset_cap_hit"), bAssetCapHit);
    Data->SetBoolField(TEXT("coverage_complete"), bCoverageComplete);
    Data->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
    Data->SetArrayField(TEXT("assets_hit_prefilter_without_graph_match"), PrefilterHitNoGraphMatch);
    Data->SetArrayField(TEXT("notes"), Notes);
    Data->SetArrayField(TEXT("matches"), Matches);

    // The honest claim this command can make is about its own COVERAGE, not
    // about the caller's intent. "Verified" here means: every candidate asset was
    // either provably eliminated by the name-map oracle or actually loaded and
    // graph-walked. Anything less is reported as not_verifiable with the reason --
    // a `matches: []` from a partial scan must never read as "there are no readers".
    if (bCoverageComplete)
    {
        mcp_common::AddVerificationEvidence(Data, mcp_common::EMcpVerification::Verified,
            TEXT("full_scan_coverage"),
            FString::Printf(TEXT("%d Blueprint asset(s) under %s: %d eliminated by the .uasset name-map oracle, %d loaded and graph-walked (%d graphs, %d nodes)"),
                AssetsScanned, *FolderPath, AssetsPrefilteredOut, AssetsLoaded, GraphsVisited, NodesVisited));
    }
    else
    {
        mcp_common::AddVerificationEvidence(Data, mcp_common::EMcpVerification::NotVerifiable,
            TEXT("full_scan_coverage"),
            FString::Printf(TEXT("PARTIAL scan (truncated=%s, timed_out=%s, asset_cap_hit=%s, prefilter_unavailable=%d, load_failed=%d): absence of matches does NOT prove absence of references"),
                bTruncated ? TEXT("true") : TEXT("false"),
                bTimedOut ? TEXT("true") : TEXT("false"),
                bAssetCapHit ? TEXT("true") : TEXT("false"),
                PrefilterUnavailable, AssetsLoadFailed));
    }

    return mcp_common::MakeOutcomeReply(Data, true);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("find_blueprint_node_references"), &HandleFindBlueprintNodeReferences },
};
} // namespace UEAgentSearchCommands
