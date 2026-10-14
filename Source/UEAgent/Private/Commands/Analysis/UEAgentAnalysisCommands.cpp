#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "Commands/Common/UEAgentGraphUtils.h"   // ResolveClass / FindGraphByNameDeep / FindNodeByGuid / NodeToJson
#include "Commands/Asset/AssetCommandHelpers.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"            // also defines TSimpleRingBuffer (used by GetTraceStack)
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h"        // get_blueprint_scs_tree: FComponentKey / ICH override check
#include "Components/ActorComponent.h"                  // get_blueprint_scs_tree: EComponentCreationMethod / UActorComponent
#include "Components/SceneComponent.h"                  // get_blueprint_scs_tree: USceneComponent attach info
#include "Components/PrimitiveComponent.h"              // get_blueprint_components_list: physics / collision flags
#include "Components/StaticMeshComponent.h"             // get_blueprint_components_list: mesh path + material count
#include "Engine/StaticMesh.h"
#include "Blueprint/BlueprintSupport.h"
#include "Kismet2/BlueprintEditorUtils.h"              // FindBlueprintForNode
#include "Kismet2/KismetEditorUtilities.h"             // CompileBlueprint / EBlueprintCompileOptions
#include "Kismet2/KismetDebugUtilities.h"              // GetTraceStack / FindSourceNodeForCodeLocation
#include "Kismet2/CompilerResultsLog.h"                // FCompilerResultsLog
#include "Logging/TokenizedMessage.h"                  // FTokenizedMessage / EMessageSeverity
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "AnimGraphNode_Base.h"                         // FAnimNode config + property bindings
#include "Animation/AnimBlueprint.h"
#include "Editor.h"                                     // GEditor->PlayWorld
#include "Engine/Engine.h"
#include "Engine/World.h"                              // FWorldContext / EWorldType
#include "EngineUtils.h"                                // TActorIterator (PIE object resolve)
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"                         // FStructProperty / FObjectPropertyBase / FMapProperty
#include "UObject/TextProperty.h"                       // get_blueprint_component_overrides: FTextProperty (verify_on_disk needle)
#include "UObject/SoftObjectPtr.h"                      // get_blueprint_component_overrides: FSoftObjectPtr (verify_on_disk needle)
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Misc/DateTime.h"                              // snapshot_object persist: FDateTime timestamps
#include "Misc/FileHelper.h"                            // snapshot persist + verify_on_disk byte scan
#include "HAL/PlatformFileManager.h"                    // snapshot persist: CreateDirectoryTree / FileExists
#include "Serialization/JsonSerializer.h"               // snapshot persist: Serialize / Deserialize
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "K2Node_FunctionEntry.h"                       // get_blueprint_functions_list: GetFunctionFlags (is_pure)
#include "UObject/FieldPath.h"
#include "Algo/Reverse.h"

namespace UEAgentAnalysisCommands
{

// MCP-DOMAIN: Analysis

// ===========================================================================
// Shared helpers for the diff / snapshot / exec-path commands.
// ===========================================================================
namespace
{
    /** Load a UBlueprint OR UAnimBlueprint by full object/package path or a bare asset short name. */
    UBlueprint* LoadBlueprintFlexible(const FString& NameOrPath)
    {
        if (NameOrPath.IsEmpty())
        {
            return nullptr;
        }
        // Anything path-shaped ("/Game/X/BP", "/Game/X/BP.BP", or an object already in memory) loads as is.
        if (UBlueprint* Loaded = LoadObject<UBlueprint>(nullptr, *NameOrPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
        {
            return Loaded;
        }

        IAssetRegistry& Registry = mcp_common::AssetRegistry();
        TArray<FAssetData> Hits;
        Registry.GetAssetsByPackageName(FName(*NameOrPath), Hits);
        if (Hits.IsEmpty())
        {
            // A bare short name: look through every Blueprint-derived asset under /Game (subclasses included,
            // so AnimBlueprints and WidgetBlueprints count) and keep the first one with exactly that name.
            FARFilter AnyBlueprintKind;
            AnyBlueprintKind.PackagePaths.Add(TEXT("/Game"));
            AnyBlueprintKind.bRecursivePaths = true;
            AnyBlueprintKind.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
            AnyBlueprintKind.bRecursiveClasses = true;

            TArray<FAssetData> Pool;
            Registry.GetAssets(AnyBlueprintKind, Pool);
            const FAssetData* FirstMatch = Pool.FindByPredicate([&NameOrPath](const FAssetData& Candidate)
            {
                return Candidate.AssetName.ToString() == NameOrPath;
            });
            if (FirstMatch)
            {
                Hits.Add(*FirstMatch);
            }
        }
        return Hits.IsEmpty() ? nullptr : Cast<UBlueprint>(Hits[0].GetAsset());
    }

    /**
     * Collect a Blueprint's full source-classified component set (native CDO + own SCS +
     * inherited-from-ancestor-chain), most-derived-wins dedup by component name. Each record is
     * IDENTICAL to get_blueprint_scs_tree's, plus a `transform` {loc,rot,scale} for scene comps.
     * Fills an ordered array (for scs_tree) and a by-name map (for O(1) diffing).
     *
     * This walk DROPS components on two paths, and without a trace a caller could not tell a short
     * list from a complete one. Both paths return "keep going", unlike the max_results path, which
     * sets bOutTruncated:
     *   (a) a USCS_Node with a NULL ComponentTemplate  -- skipped;
     *   (b) a name already in SeenNames                -- correct most-derived-wins dedup: an
     *       ancestor's shadowed component, or a native/SCS name collision, is left out.
     * The omission is legitimate; being invisible is not. The two optional out-params below turn each
     * drop into a readable number + the names, WITHOUT changing what lands in OutOrdered/OutByName.
     * They default to nullptr for the callers that do not report drops (diff_blueprint_components and
     * get_blueprint_component_overrides).
     *
     * @param OutOmittedDuplicates    optional; receives {name, source, owner_class, shadowed_by} objects.
     * @param OutOmittedNullTemplates optional; receives {name, source, owner_class} objects. `name` is ""
     *                                only when the NODE ITSELF is null (nothing left to name it by).
     */
    void CollectBlueprintComponents(
        UBlueprint* Blueprint, bool bIncludeNative, bool bIncludeInherited, int32 MaxResults,
        TArray<TSharedPtr<FJsonValue>>& OutOrdered, TMap<FName, TSharedPtr<FJsonObject>>& OutByName,
        int32& OutNative, int32& OutScs, int32& OutInherited, bool& bOutTruncated,
        TArray<TSharedPtr<FJsonValue>>* OutOmittedDuplicates = nullptr,
        TArray<TSharedPtr<FJsonValue>>* OutOmittedNullTemplates = nullptr)
    {
        OutNative = OutScs = OutInherited = 0;
        bOutTruncated = false;
        if (OutOmittedDuplicates) { OutOmittedDuplicates->Reset(); }
        if (OutOmittedNullTemplates) { OutOmittedNullTemplates->Reset(); }
        if (!Blueprint) { return; }

        // Records an omission the walk would otherwise swallow. `ShadowedBy` is the owner_class of
        // the record that already claimed the name (empty for the null-template path).
        auto NoteOmission = [](TArray<TSharedPtr<FJsonValue>>* Sink, const FString& Name,
                               const TCHAR* Source, const FString& OwnerClass, const FString& ShadowedBy)
        {
            if (!Sink) { return; }
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), Name);
            O->SetStringField(TEXT("source"), Source);
            O->SetStringField(TEXT("owner_class"), OwnerClass);
            if (!ShadowedBy.IsEmpty()) { O->SetStringField(TEXT("shadowed_by"), ShadowedBy); }
            Sink->Add(MakeShared<FJsonValueObject>(O));
        };

        TSet<FName> SeenNames;
        UClass* GenClass = Blueprint->GeneratedClass;
        const FString GenClassPath = GenClass ? GenClass->GetPathName() : FString();

        auto AddTransform = [](const TSharedPtr<FJsonObject>& Obj, USceneComponent* SC)
        {
            if (!SC) { return; }
            TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
            T->SetArrayField(TEXT("loc"), mcp_common::VectorToJsonArray(SC->GetRelativeLocation()));
            T->SetArrayField(TEXT("rot"), mcp_common::RotatorToJsonArray(SC->GetRelativeRotation()));
            T->SetArrayField(TEXT("scale"), mcp_common::VectorToJsonArray(SC->GetRelativeScale3D()));
            Obj->SetObjectField(TEXT("transform"), T);
        };

        // Structural SCS parent lookup: a node attached to ANOTHER NODE IN THE SAME SCS (the common
        // case -- e.g. via add_component_to_blueprint's auto-attach to the root, which calls
        // USCS_Node::AddChildNode under the hood) is recorded ONLY in that parent's ChildNodes array;
        // Node->ParentComponentOrVariableName stays FName() / empty for that case -- it's populated
        // exclusively for the OTHER kind of parent (a NATIVE component, or a named component on an
        // ancestor class, found by name rather than by tree structure). Reading only
        // ParentComponentOrVariableName would report "attach_parent":"" for every structurally-childed
        // node, even though it is correctly attached at runtime.
        auto BuildStructuralParentMap = [](USimpleConstructionScript* SCS) -> TMap<FName, FName>
        {
            TMap<FName, FName> ParentByChildName;
            if (!SCS) { return ParentByChildName; }
            TFunction<void(USCS_Node*)> WalkChildren = [&](USCS_Node* ParentNode)
            {
                if (!ParentNode) { return; }
                for (USCS_Node* Child : ParentNode->GetChildNodes())
                {
                    if (Child)
                    {
                        ParentByChildName.Add(Child->GetVariableName(), ParentNode->GetVariableName());
                        WalkChildren(Child);
                    }
                }
            };
            for (USCS_Node* Root : SCS->GetRootNodes())
            {
                WalkChildren(Root);
            }
            return ParentByChildName;
        };

        auto EmitScsNode = [&](USCS_Node* Node, const TCHAR* Source, const FString& OwnerClass, int32& OutCounter, const TMap<FName, FName>& StructuralParents) -> bool
        {
            // Both early-outs below return true ("keep walking") and leave a countable trace -- see
            // the comment above CollectBlueprintComponents.
            if (!Node || !Node->ComponentTemplate)
            {
                NoteOmission(OutOmittedNullTemplates,
                             Node ? Node->GetVariableName().ToString() : FString(),
                             Source, OwnerClass, FString());
                return true;
            }
            const FName VarName = Node->GetVariableName();
            if (SeenNames.Contains(VarName))
            {
                FString ShadowedBy;
                if (const TSharedPtr<FJsonObject>* Winner = OutByName.Find(VarName))
                {
                    (*Winner)->TryGetStringField(TEXT("owner_class"), ShadowedBy);
                }
                NoteOmission(OutOmittedDuplicates, VarName.ToString(), Source, OwnerClass, ShadowedBy);
                return true;
            }
            if (OutOrdered.Num() >= MaxResults) { bOutTruncated = true; return false; }
            SeenNames.Add(VarName);

            UClass* CompClass = Node->ComponentClass ? Node->ComponentClass.Get() : Node->ComponentTemplate->GetClass();
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("name"), VarName.ToString());
            Obj->SetStringField(TEXT("source"), Source);
            Obj->SetStringField(TEXT("class"), CompClass ? CompClass->GetName() : TEXT(""));
            Obj->SetStringField(TEXT("class_path"), CompClass ? CompClass->GetPathName() : TEXT(""));
            Obj->SetStringField(TEXT("owner_class"), OwnerClass);
            Obj->SetBoolField(TEXT("is_root"), Node->IsRootNode());
            Obj->SetBoolField(TEXT("is_scene_component"), Node->ComponentTemplate->IsA<USceneComponent>());
            // Structural (same-SCS ChildNodes) parent wins when present; otherwise fall back to the
            // native/cross-class-by-name field -- the two are mutually exclusive in practice.
            const FName* StructuralParent = StructuralParents.Find(VarName);
            Obj->SetStringField(TEXT("attach_parent"), StructuralParent ? StructuralParent->ToString() : Node->ParentComponentOrVariableName.ToString());
            Obj->SetStringField(TEXT("attach_socket"), Node->AttachToName.ToString());
            Obj->SetBoolField(TEXT("parent_is_native"), Node->bIsParentComponentNative);
            if (FCString::Strcmp(Source, TEXT("inherited")) == 0 && Blueprint->InheritableComponentHandler)
            {
                const FComponentKey Key(Node);
                Obj->SetBoolField(TEXT("ich_overridden"),
                    Blueprint->InheritableComponentHandler->GetOverridenComponentTemplate(Key) != nullptr);
            }
            AddTransform(Obj, Cast<USceneComponent>(Node->ComponentTemplate));

            OutOrdered.Add(MakeShared<FJsonValueObject>(Obj));
            OutByName.Add(VarName, Obj);
            ++OutCounter;
            return true;
        };

        // NATIVE (CDO) -- includes native components inherited from C++ base classes.
        if (bIncludeNative && GenClass && GenClass->IsChildOf(AActor::StaticClass()))
        {
            if (AActor* CDO = Cast<AActor>(GenClass->GetDefaultObject()))
            {
                for (UActorComponent* Comp : CDO->GetComponents())
                {
                    if (!Comp || Comp->CreationMethod != EComponentCreationMethod::Native) { continue; }
                    const FName CompName = Comp->GetFName();
                    // Same dedup drop as EmitScsNode -- counted (see CollectBlueprintComponents' comment).
                    if (SeenNames.Contains(CompName))
                    {
                        FString ShadowedBy;
                        if (const TSharedPtr<FJsonObject>* Winner = OutByName.Find(CompName))
                        {
                            (*Winner)->TryGetStringField(TEXT("owner_class"), ShadowedBy);
                        }
                        NoteOmission(OutOmittedDuplicates, CompName.ToString(), TEXT("native"), GenClassPath, ShadowedBy);
                        continue;
                    }
                    if (OutOrdered.Num() >= MaxResults) { bOutTruncated = true; break; }
                    SeenNames.Add(CompName);

                    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                    Obj->SetStringField(TEXT("name"), CompName.ToString());
                    Obj->SetStringField(TEXT("source"), TEXT("native"));
                    Obj->SetStringField(TEXT("class"), Comp->GetClass()->GetName());
                    Obj->SetStringField(TEXT("class_path"), Comp->GetClass()->GetPathName());
                    Obj->SetStringField(TEXT("owner_class"), GenClassPath);
                    if (USceneComponent* SC = Cast<USceneComponent>(Comp))
                    {
                        Obj->SetBoolField(TEXT("is_root"), CDO->GetRootComponent() == SC);
                        Obj->SetBoolField(TEXT("is_scene_component"), true);
                        USceneComponent* AttachParent = SC->GetAttachParent();
                        Obj->SetStringField(TEXT("attach_parent"), AttachParent ? AttachParent->GetFName().ToString() : TEXT(""));
                        Obj->SetStringField(TEXT("attach_socket"), SC->GetAttachSocketName().ToString());
                        AddTransform(Obj, SC);
                    }
                    else
                    {
                        Obj->SetBoolField(TEXT("is_root"), false);
                        Obj->SetBoolField(TEXT("is_scene_component"), false);
                        Obj->SetStringField(TEXT("attach_parent"), TEXT(""));
                        Obj->SetStringField(TEXT("attach_socket"), TEXT(""));
                    }
                    Obj->SetBoolField(TEXT("parent_is_native"), true);
                    OutOrdered.Add(MakeShared<FJsonValueObject>(Obj));
                    OutByName.Add(CompName, Obj);
                    ++OutNative;
                }
            }
        }

        // OWN SCS.
        if (Blueprint->SimpleConstructionScript)
        {
            const TMap<FName, FName> OwnStructuralParents = BuildStructuralParentMap(Blueprint->SimpleConstructionScript);
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (!EmitScsNode(Node, TEXT("scs"), GenClassPath, OutScs, OwnStructuralParents)) { break; }
            }
        }

        // INHERITED -- ancestor Blueprint chain, nearest first.
        if (bIncludeInherited)
        {
            for (UClass* Super = Blueprint->ParentClass; Super && !bOutTruncated; Super = Super->GetSuperClass())
            {
                UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Super);
                if (!BPGC || !BPGC->SimpleConstructionScript) { continue; }
                const FString OwnerClassPath = BPGC->GetPathName();
                const TMap<FName, FName> AncestorStructuralParents = BuildStructuralParentMap(BPGC->SimpleConstructionScript);
                for (USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
                {
                    if (!EmitScsNode(Node, TEXT("inherited"), OwnerClassPath, OutInherited, AncestorStructuralParents)) { break; }
                }
            }
        }
    }

    // =======================================================================
    // get_blueprint_component_overrides helpers.
    // =======================================================================

    /**
     * One resolved Blueprint component TEMPLATE plus the provenance of where it came from.
     * Replicates the SCS -> child ICH override -> ancestor declaring SCS -> native CDO subobject
     * cascade of HandleGetBlueprintComponentProperty
     * (Blueprint/UEAgentBlueprintCommands.cpp) and, like it, NEVER mints an ICH
     * override (GetInheritableComponentHandler(false)).
     */
    struct FComponentTemplateResolution_Analysis
    {
        /** The template whose values we read (nearest authored one). */
        UActorComponent* Template = nullptr;
        /** The ancestor-declared SCS template, when the component is inherited (null otherwise). */
        UActorComponent* InheritedTemplate = nullptr;
        /** scs | inherited-ich | inherited-scs | inherited-native */
        FString Source;
        /** Path of the class that OWNS Template. */
        FString ResolvedFromClass;
        /** Path of the class that DECLARES the component. */
        FString DeclaringClass;
        /** Ground-truth pairing handle (USCS_Node*) -- null for native subobjects. */
        USCS_Node* MatchedNode = nullptr;
        /** True when Template belongs to THIS Blueprint (own SCS node or own ICH override). */
        bool bAuthoredHere = false;
    };

    bool ResolveComponentTemplate_Analysis(UBlueprint* BP, const FName& CompName, bool bIncludeInherited,
                                           FComponentTemplateResolution_Analysis& Out)
    {
        if (!BP) { return false; }

        // (a) OWN SCS node -- this Blueprint's own _GEN_VARIABLE archetype.
        if (BP->SimpleConstructionScript)
        {
            if (USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(CompName))
            {
                if (Node->ComponentTemplate)
                {
                    Out.Template = Node->ComponentTemplate;
                    Out.MatchedNode = Node;
                    Out.Source = TEXT("scs");
                    Out.ResolvedFromClass = BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : BP->GetPathName();
                    Out.DeclaringClass = Out.ResolvedFromClass;
                    Out.bAuthoredHere = true;
                    return true;
                }
            }
        }
        if (!bIncludeInherited) { return false; }

        // Locate the ancestor Blueprint that DECLARES the component.
        USCS_Node* Decl = nullptr;
        UBlueprintGeneratedClass* DeclClass = nullptr;
        for (UClass* C = BP->ParentClass; C; C = C->GetSuperClass())
        {
            if (UBlueprintGeneratedClass* BGC = Cast<UBlueprintGeneratedClass>(C))
            {
                if (BGC->SimpleConstructionScript)
                {
                    if (USCS_Node* N = BGC->SimpleConstructionScript->FindSCSNode(CompName))
                    {
                        Decl = N;
                        DeclClass = BGC;
                        break;
                    }
                }
            }
        }
        if (Decl)
        {
            Out.MatchedNode = Decl;
            Out.DeclaringClass = DeclClass ? DeclClass->GetPathName() : FString();
            Out.InheritedTemplate = Decl->ComponentTemplate;

            // (b) nearest existing ICH override, walking from THIS class up to (not incl.) the
            //     declaring class -- mirrors USCS_Node::GetActualComponentTemplate's resolution.
            const FComponentKey Key(Decl);
            for (UClass* C = BP->GeneratedClass; C && C != DeclClass && !Out.Template; C = C->GetSuperClass())
            {
                if (UBlueprintGeneratedClass* Cur = Cast<UBlueprintGeneratedClass>(C))
                {
                    if (UInheritableComponentHandler* ICH = Cur->GetInheritableComponentHandler(false))
                    {
                        if (UActorComponent* Ovr = ICH->GetOverridenComponentTemplate(Key))
                        {
                            Out.Template = Ovr;
                            Out.Source = TEXT("inherited-ich");
                            Out.ResolvedFromClass = Cur->GetPathName();
                            Out.bAuthoredHere = (Cur == BP->GeneratedClass);
                        }
                    }
                }
            }
            // (c) else the ancestor's declaring SCS template (the inherited default itself).
            if (!Out.Template && Decl->ComponentTemplate)
            {
                Out.Template = Decl->ComponentTemplate;
                Out.Source = TEXT("inherited-scs");
                Out.ResolvedFromClass = Out.DeclaringClass;
                Out.bAuthoredHere = false;
            }
            return Out.Template != nullptr;
        }

        // (d) native inherited subobject on the CDO (CreateDefaultSubobject, no declaring SCS node).
        if (BP->GeneratedClass)
        {
            if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject()))
            {
                for (UActorComponent* C : CDO->GetComponents())
                {
                    if (C && C->GetFName() == CompName)
                    {
                        Out.Template = C;
                        Out.Source = TEXT("inherited-native");
                        Out.ResolvedFromClass = BP->GeneratedClass->GetPathName();
                        Out.bAuthoredHere = false;
                        // Declaring class = the MOST BASE native class whose CDO already owns it.
                        UClass* NativeOwner = BP->GeneratedClass;
                        for (UClass* S = BP->GeneratedClass; S; S = S->GetSuperClass())
                        {
                            if (Cast<UBlueprintGeneratedClass>(S)) { continue; }
                            AActor* SuperCDO = Cast<AActor>(S->GetDefaultObject(/*bCreateIfNeeded=*/false));
                            if (!SuperCDO) { continue; }
                            for (UActorComponent* SC : SuperCDO->GetComponents())
                            {
                                if (SC && SC->GetFName() == CompName) { NativeOwner = S; break; }
                            }
                        }
                        Out.DeclaringClass = NativeOwner->GetPathName();
                        return true;
                    }
                }
            }
        }
        return false;
    }

    /** Literal byte search for a needle in raw package bytes, trying ANSI then UTF-16LE
     *  (both encodings an FString/FName can take inside a .uasset). Same "read the raw package
     *  bytes without loading the asset" technique as ScanFileForRefs_AssetUtil
     *  (Asset/UEAgentAssetUtilCommands.cpp), which is file-local (anonymous namespace)
     *  and therefore not linkable from here. */
    bool PackageBytesContain_Analysis(const TArray<uint8>& Bytes, const FString& Needle)
    {
        const int32 Len = Needle.Len();
        if (Len <= 0 || Bytes.Num() == 0) { return false; }

        auto FindPattern = [&Bytes](const TArray<uint8>& Pattern) -> bool
        {
            if (Pattern.Num() == 0 || Bytes.Num() < Pattern.Num()) { return false; }
            const int32 Last = Bytes.Num() - Pattern.Num();
            for (int32 i = 0; i <= Last; ++i)
            {
                if (FMemory::Memcmp(Bytes.GetData() + i, Pattern.GetData(), Pattern.Num()) == 0) { return true; }
            }
            return false;
        };

        const TCHAR* Chars = *Needle;
        bool bAnsiRepresentable = true;
        TArray<uint8> Ansi;
        Ansi.Reserve(Len);
        for (int32 i = 0; i < Len; ++i)
        {
            const TCHAR C = Chars[i];
            if (C > 127) { bAnsiRepresentable = false; break; }
            Ansi.Add(static_cast<uint8>(C));
        }
        if (bAnsiRepresentable && FindPattern(Ansi)) { return true; }

        TArray<uint8> Utf16;
        Utf16.Reserve(Len * 2);
        for (int32 i = 0; i < Len; ++i)
        {
            const uint16 U = static_cast<uint16>(Chars[i]);
            Utf16.Add(static_cast<uint8>(U & 0xFF));
            Utf16.Add(static_cast<uint8>((U >> 8) & 0xFF));
        }
        return FindPattern(Utf16);
    }

    /** on-disk .uasset bytes for the package that owns Obj, cached per package name.
     *  Returns nullptr when the package has no file on disk (transient/in-memory only). */
    const TArray<uint8>* GetPackageBytesCached_Analysis(const UObject* Obj,
                                                        TMap<FString, TArray<uint8>>& Cache,
                                                        TSet<FString>& MissingFiles,
                                                        FString& OutPackageName,
                                                        FString& OutFilePath)
    {
        if (!Obj) { return nullptr; }
        const UPackage* Pkg = Obj->GetOutermost();
        if (!Pkg) { return nullptr; }
        OutPackageName = Pkg->GetName();
        if (!FPackageName::TryConvertLongPackageNameToFilename(OutPackageName, OutFilePath,
                                                               FPackageName::GetAssetPackageExtension()))
        {
            OutFilePath.Reset();
            return nullptr;
        }
        if (MissingFiles.Contains(OutPackageName)) { return nullptr; }
        if (const TArray<uint8>* Hit = Cache.Find(OutPackageName)) { return Hit; }

        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *OutFilePath))
        {
            // A map package carries the .umap extension, not .uasset -- try it before giving up.
            FString MapPath;
            if (FPackageName::TryConvertLongPackageNameToFilename(OutPackageName, MapPath,
                    FPackageName::GetMapPackageExtension())
                && FFileHelper::LoadFileToArray(Bytes, *MapPath))
            {
                OutFilePath = MapPath;
            }
            else
            {
                MissingFiles.Add(OutPackageName);
                return nullptr;
            }
        }
        return &Cache.Add(OutPackageName, MoveTemp(Bytes));
    }

    /** The string-ish payload of a property value, for the verify_on_disk byte scan.
     *  Returns false for every non-string-ish type -- the caller must then report
     *  `found: null` / applicable=false, NEVER `found: false`. */
    bool GetStringishNeedle_Analysis(const FProperty* Prop, const void* Container, FString& OutNeedle)
    {
        if (!Prop || !Container) { return false; }
        if (const FStrProperty* SP = CastField<FStrProperty>(Prop))
        {
            OutNeedle = SP->GetPropertyValue_InContainer(Container);
            return true;
        }
        if (const FNameProperty* NP = CastField<FNameProperty>(Prop))
        {
            OutNeedle = NP->GetPropertyValue_InContainer(Container).ToString();
            return true;
        }
        if (const FTextProperty* TP = CastField<FTextProperty>(Prop))
        {
            OutNeedle = TP->GetPropertyValue_InContainer(Container).ToString();
            return true;
        }
        if (const FSoftObjectProperty* SOP = CastField<FSoftObjectProperty>(Prop))
        {
            OutNeedle = SOP->GetPropertyValue_InContainer(Container).ToString();
            return true;
        }
        return false;
    }

    /** Map an EBlueprintStatus to the same wire strings verify_reparent_success uses. */
    const TCHAR* BlueprintStatusToString_Analysis(EBlueprintStatus Status)
    {
        switch (Status)
        {
            case BS_Error:                 return TEXT("error");
            case BS_UpToDateWithWarnings:  return TEXT("warnings");
            case BS_UpToDate:              return TEXT("up_to_date");
            case BS_Dirty:                 return TEXT("dirty");
            case BS_BeingCreated:          return TEXT("being_created");
            default:                       return TEXT("unknown");
        }
    }

    /** Directory persisted object snapshots live in (Saved/MCPAudits/snapshots). */
    FString SnapshotDir_Analysis()
    {
        return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MCPAudits") / TEXT("snapshots"));
    }

    /** Sanitize a caller-supplied handle into a safe filename stem (whitelist, so path
     *  separators and ".." can never survive into the file name). */
    FString SanitizeHandleForFile_Analysis(const FString& Handle)
    {
        FString Safe;
        Safe.Reserve(Handle.Len());
        const TCHAR* Chars = *Handle;
        for (int32 i = 0; i < Handle.Len(); ++i)
        {
            const TCHAR C = Chars[i];
            const bool bAllowed = (C >= TEXT('A') && C <= TEXT('Z'))
                               || (C >= TEXT('a') && C <= TEXT('z'))
                               || (C >= TEXT('0') && C <= TEXT('9'))
                               || C == TEXT('_') || C == TEXT('-');
            Safe.AppendChar(bAllowed ? C : TEXT('_'));
        }
        if (Safe.IsEmpty()) { Safe = TEXT("snapshot"); }
        return Safe;
    }

    /** Write a snapshot to Saved/MCPAudits/snapshots/<handle>.json. Returns false + reason. */
    bool PersistSnapshot_Analysis(const FString& Handle, const TSharedPtr<FJsonObject>& Snapshot,
                                  FString& OutPath, FString& OutError)
    {
        if (!Snapshot.IsValid()) { OutError = TEXT("snapshot is null"); return false; }
        const FString Dir = SnapshotDir_Analysis();
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        if (!PF.DirectoryExists(*Dir) && !PF.CreateDirectoryTree(*Dir))
        {
            OutError = FString::Printf(TEXT("Could not create snapshot directory: %s"), *Dir);
            return false;
        }
        OutPath = Dir / (SanitizeHandleForFile_Analysis(Handle) + TEXT(".json"));

        FString JsonString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
        FJsonSerializer::Serialize(Snapshot.ToSharedRef(), Writer);
        if (!FFileHelper::SaveStringToFile(JsonString, *OutPath))
        {
            OutError = FString::Printf(TEXT("SaveStringToFile failed for: %s"), *OutPath);
            return false;
        }
        // Verify the write instead of trusting the bool: re-stat the file on disk.
        if (!PF.FileExists(*OutPath) || PF.FileSize(*OutPath) <= 0)
        {
            OutError = FString::Printf(TEXT("File missing or empty after write: %s"), *OutPath);
            return false;
        }
        return true;
    }

    /** Read back a persisted snapshot from an explicit path or from a bare handle. */
    TSharedPtr<FJsonObject> LoadPersistedSnapshot_Analysis(const FString& PathOrHandle, FString& OutResolvedPath)
    {
        FString Path = PathOrHandle;
        if (!Path.EndsWith(TEXT(".json")))
        {
            Path = SnapshotDir_Analysis() / (SanitizeHandleForFile_Analysis(PathOrHandle) + TEXT(".json"));
        }
        OutResolvedPath = FPaths::ConvertRelativePathToFull(Path);

        FString Contents;
        if (!FFileHelper::LoadFileToString(Contents, *OutResolvedPath)) { return nullptr; }
        TSharedPtr<FJsonObject> Parsed;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
        if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid()) { return nullptr; }
        return Parsed;
    }

    /**
     * True when the world param names a LIVE world rather than the editor one: anything that is
     * not absent/'editor', so 'server' / 'client' / 'pie:N' are runtime selectors too, and a TYPO
     * reaches the canonical resolver, which refuses it and says what is accepted.
     */
    bool IsRuntimeWorldParam(const FString& WorldParam)
    {
        const FString W = WorldParam.TrimStartAndEnd();
        return !W.IsEmpty() && !W.Equals(TEXT("editor"), ESearchCase::IgnoreCase);
    }

    /**
     * CANONICAL WORLD RESOLUTION, shared by every tool so that two tools never read two different
     * PIE instances (which disagree in a way that looks like a game bug). absent/'editor' -> editor
     * world; 'pie'/'game'/'runtime' -> the ambiguous live world; 'server', 'client',
     * 'pie:N' -> that world; anything else -> an error that lists the worlds that DO exist.
     */
    UWorld* ResolveWorldForParam(const FString& WorldParam, FString& OutError)
    {
        FString Selector = WorldParam.TrimStartAndEnd();
        if (Selector.IsEmpty())
        {
            Selector = TEXT("editor");
        }
        else if (Selector.Equals(TEXT("game"), ESearchCase::IgnoreCase)
              || Selector.Equals(TEXT("runtime"), ESearchCase::IgnoreCase))
        {
            Selector = TEXT("pie");
        }

        TSharedPtr<FJsonObject> WorldParams = MakeShared<FJsonObject>();
        WorldParams->SetStringField(TEXT("world"), Selector);
        UWorld* Resolved = mcp_common::ResolveWorldFromParams(WorldParams, OutError);
        if (Resolved)
        {
            return Resolved;
        }

        // An EWorldType::Game world (cooked/-game process) is accepted too; the
        // canonical 'pie' selector only scans PIE contexts, so that case is handled here.
        if (!Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
        {
            if (UWorld* Active = mcp_common::ActiveGameWorld())
            {
                OutError.Reset();
                return Active;
            }
        }
        return nullptr;
    }

    /** Overload for call sites that do not surface the resolution error. */
    UWorld* ResolveWorldForParam(const FString& WorldParam)
    {
        FString Unused;
        return ResolveWorldForParam(WorldParam, Unused);
    }

    /**
     * Resolve a path/name to a live object in the chosen world. First the broad side-effect-free
     * resolver (full object path / asset / already-loaded object, incl. a fully-qualified PIE path),
     * then a dotted "Actor[.Component[.Subobject]]" walk scoped to World (name OR editor label).
     */
    UObject* ResolveObjectForWorld(const FString& Path, UWorld* World)
    {
        if (UObject* Direct = mcp_common::ResolveObjectByPath(Path))
        {
            return Direct;
        }
        return AssetCommandHelpers::FindDottedPathInWorld(Path, World);
    }

    /** Serialize every reflected FProperty of an object into a name -> typed-JSON map. */
    void ObjectPropertiesToJson(const UObject* Obj, const TSharedPtr<FJsonObject>& OutProps)
    {
        if (!Obj || !OutProps.IsValid()) { return; }
        for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
        {
            FProperty* P = *It;
            if (!P) { continue; }
            const void* ValuePtr = P->ContainerPtrToValuePtr<void>(Obj);
            OutProps->SetField(P->GetName(), mcp_common::PropertyValueToJson(P, ValuePtr));
        }
    }

    /** Capture props (+ components for actors) of a live object into a snapshot JSON object.
     *  When the target is a Blueprint / BPGC / Blueprint CDO, ALSO captures the SCS + ICH
     *  component TEMPLATES. Actor::GetComponents() on a Blueprint CDO returns ONLY the native
     *  subobjects (SCS components are instantiated at spawn and never live on the CDO), so without
     *  this branch a persisted CDO snapshot could not verify a single SCS/ICH override. */
    TSharedPtr<FJsonObject> CaptureObjectSnapshot(UObject* Obj, const FString& WorldLabel,
                                                  bool bIncludeBlueprintTemplates = true)
    {
        TSharedPtr<FJsonObject> Snap = MakeShared<FJsonObject>();
        Snap->SetStringField(TEXT("object_path"), Obj->GetPathName());
        Snap->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());
        Snap->SetStringField(TEXT("world"), WorldLabel);
        Snap->SetNumberField(TEXT("captured_at"), FPlatformTime::Seconds());
        Snap->SetStringField(TEXT("captured_at_utc"), FDateTime::UtcNow().ToIso8601());

        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        ObjectPropertiesToJson(Obj, Props);
        Snap->SetObjectField(TEXT("properties"), Props);

        int32 NativeComponentCount = 0;
        if (AActor* Actor = Cast<AActor>(Obj))
        {
            TSharedPtr<FJsonObject> Comps = MakeShared<FJsonObject>();
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            for (UActorComponent* Comp : Components)
            {
                if (!Comp) { continue; }
                TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
                CObj->SetStringField(TEXT("class"), Comp->GetClass()->GetPathName());
                CObj->SetStringField(TEXT("template_source"), TEXT("instance_or_native"));
                TSharedPtr<FJsonObject> CProps = MakeShared<FJsonObject>();
                ObjectPropertiesToJson(Comp, CProps);
                CObj->SetObjectField(TEXT("properties"), CProps);
                Comps->SetObjectField(Comp->GetName(), CObj);
                ++NativeComponentCount;
            }
            Snap->SetObjectField(TEXT("components"), Comps);
        }
        Snap->SetNumberField(TEXT("instance_component_count"), NativeComponentCount);

        int32 TemplateCount = 0;
        if (bIncludeBlueprintTemplates)
        {
            UBlueprintGeneratedClass* BPGC = nullptr;
            if (UBlueprint* AsBP = Cast<UBlueprint>(Obj))
            {
                BPGC = Cast<UBlueprintGeneratedClass>(AsBP->GeneratedClass);
            }
            else if (UBlueprintGeneratedClass* AsClass = Cast<UBlueprintGeneratedClass>(Obj))
            {
                BPGC = AsClass;
            }
            else if (Obj->HasAnyFlags(RF_ClassDefaultObject))
            {
                BPGC = Cast<UBlueprintGeneratedClass>(Obj->GetClass());
            }

            if (BPGC)
            {
                TSharedPtr<FJsonObject> Comps = MakeShared<FJsonObject>();
                const TSharedPtr<FJsonObject>* Existing = nullptr;
                if (Snap->TryGetObjectField(TEXT("components"), Existing) && Existing) { Comps = *Existing; }

                TSet<FName> SeenTemplates;
                for (UClass* C = BPGC; C; C = C->GetSuperClass())
                {
                    UBlueprintGeneratedClass* CurBPGC = Cast<UBlueprintGeneratedClass>(C);
                    if (!CurBPGC || !CurBPGC->SimpleConstructionScript) { continue; }
                    for (USCS_Node* Node : CurBPGC->SimpleConstructionScript->GetAllNodes())
                    {
                        if (!Node) { continue; }
                        const FName VarName = Node->GetVariableName();
                        if (SeenTemplates.Contains(VarName)) { continue; }
                        // Resolves the nearest ICH override from BPGC's point of view, exactly as
                        // the engine does at spawn time; falls back to the declaring template.
                        UActorComponent* Effective = Node->GetActualComponentTemplate(BPGC);
                        if (!Effective) { Effective = Node->ComponentTemplate; }
                        if (!Effective) { continue; }
                        SeenTemplates.Add(VarName);

                        TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
                        CObj->SetStringField(TEXT("class"), Effective->GetClass()->GetPathName());
                        CObj->SetStringField(TEXT("template_source"), (CurBPGC == BPGC) ? TEXT("scs") : TEXT("inherited"));
                        CObj->SetStringField(TEXT("declaring_class"), CurBPGC->GetPathName());
                        CObj->SetStringField(TEXT("template_path"), Effective->GetPathName());
                        TSharedPtr<FJsonObject> CProps = MakeShared<FJsonObject>();
                        ObjectPropertiesToJson(Effective, CProps);
                        CObj->SetObjectField(TEXT("properties"), CProps);
                        Comps->SetObjectField(VarName.ToString(), CObj);
                        ++TemplateCount;
                    }
                }
                Snap->SetObjectField(TEXT("components"), Comps);
                Snap->SetStringField(TEXT("blueprint_class"), BPGC->GetPathName());
            }
        }
        Snap->SetNumberField(TEXT("component_template_count"), TemplateCount);
        return Snap;
    }

    /** In-memory snapshot store (handle -> snapshot). The analysis handler is a bridge singleton
     *  and MCP commands run serially on the game thread, so a plain file-static map is safe. */
    TMap<FString, TSharedPtr<FJsonObject>>& SnapshotStore()
    {
        static TMap<FString, TSharedPtr<FJsonObject>> Store;
        return Store;
    }

    /**
     * Resolve ONE side of diff_object_snapshots. Order:
     *   (1) `snapshot_a`/`snapshot_b` given as a JSON OBJECT -> inline snapshot;
     *   (2) `path_a`/`path_b` -> a persisted snapshot file;
     *   (3) a handle STRING (handle_a / a / snapshot_a) -> the in-memory store, and if that misses,
     *       the persisted copy under Saved/MCPAudits/snapshots/<handle>.json.
     * Step (3) keeps the handle aliases working; the disk fallback is what makes a baseline
     * survive an editor restart.
     */
    TSharedPtr<FJsonObject> ResolveSnapshotSide_Analysis(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* HandleField, const TCHAR* ShortAliasField, const TCHAR* SnapshotField, const TCHAR* PathField,
        FString& OutLabel, FString& OutOrigin, FString& OutError)
    {
        OutOrigin = TEXT("none");

        const TSharedPtr<FJsonObject>* InlineObj = nullptr;
        if (Params->TryGetObjectField(SnapshotField, InlineObj) && InlineObj && (*InlineObj).IsValid())
        {
            OutOrigin = TEXT("inline");
            OutLabel = TEXT("<inline>");
            return *InlineObj;
        }

        FString Path;
        if (Params->TryGetStringField(PathField, Path) && !Path.IsEmpty())
        {
            FString Resolved;
            TSharedPtr<FJsonObject> Loaded = LoadPersistedSnapshot_Analysis(Path, Resolved);
            OutLabel = Resolved;
            if (!Loaded.IsValid())
            {
                OutError = FString::Printf(TEXT("Could not read/parse snapshot file: %s"), *Resolved);
                return nullptr;
            }
            OutOrigin = TEXT("file");
            return Loaded;
        }

        FString Handle;
        if (!Params->TryGetStringField(HandleField, Handle)
            && !Params->TryGetStringField(ShortAliasField, Handle)
            && !Params->TryGetStringField(SnapshotField, Handle))
        {
            OutError = FString::Printf(TEXT("Missing required parameter: %s (aliases: %s, %s, or %s)"),
                HandleField, ShortAliasField, SnapshotField, PathField);
            return nullptr;
        }
        OutLabel = Handle;
        if (TSharedPtr<FJsonObject>* Hit = SnapshotStore().Find(Handle))
        {
            OutOrigin = TEXT("memory");
            return *Hit;
        }
        FString Resolved;
        TSharedPtr<FJsonObject> Loaded = LoadPersistedSnapshot_Analysis(Handle, Resolved);
        if (Loaded.IsValid())
        {
            OutOrigin = TEXT("file");
            return Loaded;
        }
        OutError = FString::Printf(
            TEXT("Snapshot handle not found in the in-memory store and no persisted copy at '%s': %s"),
            *Resolved, *Handle);
        return nullptr;
    }

    /** Structural equality of two typed JSON values (produced by PropertyValueToJson). */
    bool JsonValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
    {
        if (!A.IsValid() || !B.IsValid()) { return A.IsValid() == B.IsValid(); }
        if (A->Type != B->Type) { return false; }
        switch (A->Type)
        {
            case EJson::Null:    return true;
            case EJson::Boolean: return A->AsBool() == B->AsBool();
            case EJson::Number:  return A->AsNumber() == B->AsNumber();
            case EJson::String:  return A->AsString() == B->AsString();
            case EJson::Array:
            {
                const TArray<TSharedPtr<FJsonValue>>& Aa = A->AsArray();
                const TArray<TSharedPtr<FJsonValue>>& Ba = B->AsArray();
                if (Aa.Num() != Ba.Num()) { return false; }
                for (int32 i = 0; i < Aa.Num(); ++i)
                {
                    if (!JsonValuesEqual(Aa[i], Ba[i])) { return false; }
                }
                return true;
            }
            case EJson::Object:
            {
                const TSharedPtr<FJsonObject>* Ao = nullptr; const TSharedPtr<FJsonObject>* Bo = nullptr;
                if (!A->TryGetObject(Ao) || !B->TryGetObject(Bo) || !Ao || !Bo) { return false; }
                if ((*Ao)->Values.Num() != (*Bo)->Values.Num()) { return false; }
                for (const auto& Pair : (*Ao)->Values)
                {
                    const TSharedPtr<FJsonValue>* Other = (*Bo)->Values.Find(Pair.Key);
                    if (!Other || !JsonValuesEqual(Pair.Value, *Other)) { return false; }
                }
                return true;
            }
            default: return false;
        }
    }

    /** Diff two "name -> value" JSON prop maps into added/removed/changed arrays. Returns total diffs. */
    int32 DiffJsonPropMaps(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B,
                           TArray<TSharedPtr<FJsonValue>>& Added, TArray<TSharedPtr<FJsonValue>>& Removed,
                           TArray<TSharedPtr<FJsonValue>>& Changed)
    {
        int32 Count = 0;
        if (A.IsValid())
        {
            for (const auto& Pair : A->Values)
            {
                const TSharedPtr<FJsonValue>* BVal = B.IsValid() ? B->Values.Find(Pair.Key) : nullptr;
                if (!BVal)
                {
                    TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                    E->SetStringField(TEXT("property"), Pair.Key);
                    E->SetField(TEXT("value_a"), Pair.Value);
                    Removed.Add(MakeShared<FJsonValueObject>(E));
                    ++Count;
                }
                else if (!JsonValuesEqual(Pair.Value, *BVal))
                {
                    TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                    E->SetStringField(TEXT("property"), Pair.Key);
                    E->SetField(TEXT("value_a"), Pair.Value);
                    E->SetField(TEXT("value_b"), *BVal);
                    Changed.Add(MakeShared<FJsonValueObject>(E));
                    ++Count;
                }
            }
        }
        if (B.IsValid())
        {
            for (const auto& Pair : B->Values)
            {
                if (!A.IsValid() || !A->Values.Contains(Pair.Key))
                {
                    TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                    E->SetStringField(TEXT("property"), Pair.Key);
                    E->SetField(TEXT("value_b"), Pair.Value);
                    Added.Add(MakeShared<FJsonValueObject>(E));
                    ++Count;
                }
            }
        }
        return Count;
    }

    /** One section of a diff reply: { added, removed, changed }. */
    TSharedPtr<FJsonObject> DiffSection_Analysis(const TArray<TSharedPtr<FJsonValue>>& Added,
        const TArray<TSharedPtr<FJsonValue>>& Removed, const TArray<TSharedPtr<FJsonValue>>& Changed)
    {
        TSharedPtr<FJsonObject> Section = MakeShared<FJsonObject>();
        Section->SetArrayField(TEXT("added"), Added);
        Section->SetArrayField(TEXT("removed"), Removed);
        Section->SetArrayField(TEXT("changed"), Changed);
        return Section;
    }

    // --- Blueprint-graph diff helpers ------------------------------------

    /** The set of a pin's outgoing links as sorted "targetGuid:targetPin" tokens (order-independent). */
    FString PinLinksSignature(UEdGraphPin* Pin)
    {
        if (!Pin) { return FString(); }
        TArray<FString> Links;
        for (UEdGraphPin* Other : Pin->LinkedTo)
        {
            if (Other && Other->GetOwningNodeUnchecked())
            {
                Links.Add(FString::Printf(TEXT("%s:%s"), *Other->GetOwningNode()->NodeGuid.ToString(), *Other->PinName.ToString()));
            }
        }
        Links.Sort();
        return FString::Join(Links, TEXT(","));
    }

    /** Read an AnimGraphNode's internal FAnimNode struct as a prop -> export-text map. */
    void ReadAnimNodeConfig(UAnimGraphNode_Base* AnimNode, TMap<FString, FString>& Out)
    {
        if (!AnimNode) { return; }
        FStructProperty* NodeProp = AnimNode->GetFNodeProperty();
        UScriptStruct* NodeStruct = AnimNode->GetFNodeType();
        if (!NodeProp || !NodeStruct) { return; }
        const void* NodeMem = NodeProp->ContainerPtrToValuePtr<void>(AnimNode);
        for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
        {
            FProperty* P = *It;
            const void* ValuePtr = P->ContainerPtrToValuePtr<void>(NodeMem);
            FString S;
            P->ExportTextItem_Direct(S, ValuePtr, /*Default=*/nullptr, /*Parent=*/nullptr, PPF_None);
            Out.Add(P->GetName(), S);
        }
    }

    /** Read an AnimGraphNode's per-pin property bindings as pin -> "path_as_text" map (via the
     *  reflected private 'Binding' subobject; empty if the node has no bindings). */
    void ReadAnimNodeBindings(UAnimGraphNode_Base* AnimNode, TMap<FString, FString>& Out)
    {
        if (!AnimNode) { return; }
        FObjectPropertyBase* BindingObjProp = CastField<FObjectPropertyBase>(
            UAnimGraphNode_Base::StaticClass()->FindPropertyByName(TEXT("Binding")));
        if (!BindingObjProp) { return; }
        UObject* BindingObj = BindingObjProp->GetObjectPropertyValue_InContainer(AnimNode);
        if (!BindingObj) { return; }
        FMapProperty* MapProp = FindFProperty<FMapProperty>(BindingObj->GetClass(), TEXT("PropertyBindings"));
        if (!MapProp) { return; }
        const TMap<FName, FAnimGraphNodePropertyBinding>* BindMap =
            MapProp->ContainerPtrToValuePtr<TMap<FName, FAnimGraphNodePropertyBinding>>(BindingObj);
        if (!BindMap) { return; }
        for (const TPair<FName, FAnimGraphNodePropertyBinding>& Pair : *BindMap)
        {
            const FAnimGraphNodePropertyBinding& B = Pair.Value;
            Out.Add(Pair.Key.ToString(), FString::Printf(TEXT("%s%s"),
                B.bIsBound ? TEXT("") : TEXT("(unbound) "), *B.PathAsText.ToString()));
        }
    }

    /** Diff two "key -> string" maps into a JSON array of {key, a, b} change records. */
    void DiffStringMaps(const TMap<FString, FString>& A, const TMap<FString, FString>& B,
                        const TCHAR* KeyField, TArray<TSharedPtr<FJsonValue>>& OutChanges)
    {
        auto AddChange = [KeyField, &OutChanges](const FString& Key, const FString& ValueA, const FString& ValueB)
        {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(KeyField, Key);
            E->SetStringField(TEXT("a"), ValueA);
            E->SetStringField(TEXT("b"), ValueB);
            OutChanges.Add(MakeShared<FJsonValueObject>(E));
        };
        for (const TPair<FString, FString>& Pair : A)
        {
            const FString* BVal = B.Find(Pair.Key);
            if (!BVal)
            {
                AddChange(Pair.Key, Pair.Value, TEXT("(absent)"));
            }
            else if (*BVal != Pair.Value)
            {
                AddChange(Pair.Key, Pair.Value, *BVal);
            }
        }
        for (const TPair<FString, FString>& Pair : B)
        {
            if (!A.Contains(Pair.Key))
            {
                AddChange(Pair.Key, TEXT("(absent)"), Pair.Value);
            }
        }
    }

    /** Compare two GUID-matched nodes; return a `changes` object, or nullptr when identical. */
    TSharedPtr<FJsonObject> DiffNodePair(UEdGraphNode* A, UEdGraphNode* B)
    {
        TSharedPtr<FJsonObject> Changes = MakeShared<FJsonObject>();
        bool bAny = false;

        if (A->GetClass() != B->GetClass())
        {
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("a"), A->GetClass()->GetName());
            C->SetStringField(TEXT("b"), B->GetClass()->GetName());
            Changes->SetObjectField(TEXT("class"), C);
            bAny = true;
        }

        const FString TitleA = A->GetNodeTitle(ENodeTitleType::ListView).ToString();
        const FString TitleB = B->GetNodeTitle(ENodeTitleType::ListView).ToString();
        if (TitleA != TitleB)
        {
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("a"), TitleA);
            C->SetStringField(TEXT("b"), TitleB);
            Changes->SetObjectField(TEXT("title"), C);
            bAny = true;
        }

        // Pins: match by name+direction; report default-value and link changes, plus added/removed pins.
        TArray<TSharedPtr<FJsonValue>> PinChanges;
        TSet<FString> SeenBPins;
        for (UEdGraphPin* PinA : A->Pins)
        {
            if (!PinA) { continue; }
            UEdGraphPin* PinB = FUEAgentGraphUtils::FindPinByName(B, PinA->PinName.ToString(), PinA->Direction);
            const FString PinKey = FString::Printf(TEXT("%s|%d"), *PinA->PinName.ToString(), (int32)PinA->Direction);
            if (!PinB)
            {
                TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                E->SetStringField(TEXT("pin"), PinA->PinName.ToString());
                E->SetStringField(TEXT("change"), TEXT("removed"));
                PinChanges.Add(MakeShared<FJsonValueObject>(E));
                continue;
            }
            SeenBPins.Add(PinKey);
            const FString DefA = PinA->GetDefaultAsString();
            const FString DefB = PinB->GetDefaultAsString();
            const FString LinksA = PinLinksSignature(PinA);
            const FString LinksB = PinLinksSignature(PinB);
            if (DefA != DefB || LinksA != LinksB)
            {
                TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                E->SetStringField(TEXT("pin"), PinA->PinName.ToString());
                if (DefA != DefB)
                {
                    E->SetStringField(TEXT("default_a"), DefA);
                    E->SetStringField(TEXT("default_b"), DefB);
                }
                if (LinksA != LinksB)
                {
                    E->SetStringField(TEXT("links_a"), LinksA);
                    E->SetStringField(TEXT("links_b"), LinksB);
                }
                PinChanges.Add(MakeShared<FJsonValueObject>(E));
            }
        }
        for (UEdGraphPin* PinB : B->Pins)
        {
            if (!PinB) { continue; }
            const FString PinKey = FString::Printf(TEXT("%s|%d"), *PinB->PinName.ToString(), (int32)PinB->Direction);
            if (!SeenBPins.Contains(PinKey) &&
                !FUEAgentGraphUtils::FindPinByName(A, PinB->PinName.ToString(), PinB->Direction))
            {
                TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                E->SetStringField(TEXT("pin"), PinB->PinName.ToString());
                E->SetStringField(TEXT("change"), TEXT("added"));
                PinChanges.Add(MakeShared<FJsonValueObject>(E));
            }
        }
        if (PinChanges.Num() > 0)
        {
            Changes->SetArrayField(TEXT("pins"), PinChanges);
            bAny = true;
        }

        // AnimGraph node config + property bindings (e.g. bGenerateTrajectory / a Trajectory binding).
        UAnimGraphNode_Base* AnimA = Cast<UAnimGraphNode_Base>(A);
        UAnimGraphNode_Base* AnimB = Cast<UAnimGraphNode_Base>(B);
        if (AnimA && AnimB)
        {
            TMap<FString, FString> CfgA, CfgB;
            ReadAnimNodeConfig(AnimA, CfgA);
            ReadAnimNodeConfig(AnimB, CfgB);
            TArray<TSharedPtr<FJsonValue>> CfgChanges;
            DiffStringMaps(CfgA, CfgB, TEXT("property"), CfgChanges);
            if (CfgChanges.Num() > 0)
            {
                Changes->SetArrayField(TEXT("anim_config"), CfgChanges);
                bAny = true;
            }

            TMap<FString, FString> BindA, BindB;
            ReadAnimNodeBindings(AnimA, BindA);
            ReadAnimNodeBindings(AnimB, BindB);
            TArray<TSharedPtr<FJsonValue>> BindChanges;
            DiffStringMaps(BindA, BindB, TEXT("pin"), BindChanges);
            if (BindChanges.Num() > 0)
            {
                Changes->SetArrayField(TEXT("anim_bindings"), BindChanges);
                bAny = true;
            }
        }

        return bAny ? Changes : nullptr;
    }

    /** Compact identity of a node for added/removed lists. */
    TSharedPtr<FJsonObject> NodeIdentity(UEdGraphNode* Node)
    {
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
        E->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        E->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
        return E;
    }

    /** What a single DiffGraphPair call actually looked at -- so a truncated result can never
     *  be mistaken for an identical one. */
    struct FGraphPairDiffStats_Analysis
    {
        int32 NodesA = 0;             // GUID-keyed nodes present on side A
        int32 NodesB = 0;             // GUID-keyed nodes present on side B
        int32 ExaminedA = 0;          // nodes of A actually compared before the budget ran out
        int32 ExaminedB = 0;          // nodes of B actually scanned for "added"
        bool  bBudgetExhausted = false;
    };

    /**
     * Diff one pair of graphs by GUID-matched nodes. Fills the added/removed/changed arrays and
     * advances NodeBudget.
     *
     * Each loop `break`s on budget exhaustion (a `return` from inside the MapA loop would skip the
     * MapB loop that produces `nodes_added`, and a capped diff would read "no added nodes" instead
     * of "not examined"), and the caller gets explicit per-side examined counters plus
     * `budget_exhausted`.
     */
    void DiffGraphPair(UEdGraph* GraphA, UEdGraph* GraphB,
                       TArray<TSharedPtr<FJsonValue>>& NodesAdded,
                       TArray<TSharedPtr<FJsonValue>>& NodesRemoved,
                       TArray<TSharedPtr<FJsonValue>>& NodesChanged,
                       int32& NodeBudget, bool& bTruncated,
                       FGraphPairDiffStats_Analysis* OutStats = nullptr)
    {
        FGraphPairDiffStats_Analysis Stats;

        TMap<FString, UEdGraphNode*> MapA, MapB;
        if (GraphA)
        {
            for (UEdGraphNode* N : GraphA->Nodes) { if (N) { MapA.Add(N->NodeGuid.ToString(), N); } }
        }
        if (GraphB)
        {
            for (UEdGraphNode* N : GraphB->Nodes) { if (N) { MapB.Add(N->NodeGuid.ToString(), N); } }
        }
        Stats.NodesA = MapA.Num();
        Stats.NodesB = MapB.Num();

        for (const TPair<FString, UEdGraphNode*>& Pair : MapA)
        {
            if (NodeBudget <= 0) { bTruncated = true; Stats.bBudgetExhausted = true; break; }
            --NodeBudget;
            ++Stats.ExaminedA;
            UEdGraphNode** Match = MapB.Find(Pair.Key);
            if (!Match)
            {
                NodesRemoved.Add(MakeShared<FJsonValueObject>(NodeIdentity(Pair.Value)));
            }
            else if (TSharedPtr<FJsonObject> Changes = DiffNodePair(Pair.Value, *Match))
            {
                TSharedPtr<FJsonObject> E = NodeIdentity(Pair.Value);
                E->SetObjectField(TEXT("changes"), Changes);
                NodesChanged.Add(MakeShared<FJsonValueObject>(E));
            }
        }
        for (const TPair<FString, UEdGraphNode*>& Pair : MapB)
        {
            if (NodeBudget <= 0) { bTruncated = true; Stats.bBudgetExhausted = true; break; }
            --NodeBudget;
            ++Stats.ExaminedB;
            if (!MapA.Contains(Pair.Key))
            {
                NodesAdded.Add(MakeShared<FJsonValueObject>(NodeIdentity(Pair.Value)));
            }
        }

        if (OutStats) { *OutStats = Stats; }
    }

    /**
     * The diff section of two name -> value maps: a name only in B is added and one only in A removed, both as
     * { name, ValueKey }; a name whose value differs is changed, as { name, ValueKeyA, ValueKeyB }. Adds the
     * number of differences to InOutTotal.
     */
    TSharedPtr<FJsonObject> DiffNamedValues_Analysis(const TMap<FString, FString>& A, const TMap<FString, FString>& B,
        const TCHAR* ValueKey, const TCHAR* ValueKeyA, const TCHAR* ValueKeyB, int32& InOutTotal)
    {
        TArray<TSharedPtr<FJsonValue>> Added;
        TArray<TSharedPtr<FJsonValue>> Removed;
        TArray<TSharedPtr<FJsonValue>> Changed;
        for (const TPair<FString, FString>& Pair : A)
        {
            const FString* BValue = B.Find(Pair.Key);
            if (!BValue)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Pair.Key);
                Entry->SetStringField(ValueKey, Pair.Value);
                Removed.Add(MakeShared<FJsonValueObject>(Entry));
            }
            else if (*BValue != Pair.Value)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Pair.Key);
                Entry->SetStringField(ValueKeyA, Pair.Value);
                Entry->SetStringField(ValueKeyB, *BValue);
                Changed.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        for (const TPair<FString, FString>& Pair : B)
        {
            if (!A.Contains(Pair.Key))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("name"), Pair.Key);
                Entry->SetStringField(ValueKey, Pair.Value);
                Added.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
        InOutTotal += Added.Num() + Removed.Num() + Changed.Num();
        return DiffSection_Analysis(Added, Removed, Changed);
    }

    /**
     * The two Blueprints diff_blueprints and diff_blueprint_graphs compare (blueprint_a, blueprint_b) and their
     * max_nodes budget (default 20000, also for a value below 1). Returns the error reply to send back, or
     * nullptr once both Blueprints are loaded.
     */
    TSharedPtr<FJsonObject> LoadDiffPair_Analysis(const TSharedPtr<FJsonObject>& Params, FString& OutNameA, FString& OutNameB,
        int32& OutMaxNodes, UBlueprint*& OutA, UBlueprint*& OutB)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_a"), OutNameA, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_b"), OutNameB, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        OutMaxNodes = 20000;
        Params->TryGetNumberField(TEXT("max_nodes"), OutMaxNodes);
        if (OutMaxNodes < 1)
        {
            OutMaxNodes = 20000;
        }

        OutA = LoadBlueprintFlexible(OutNameA);
        if (!OutA)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blueprint A not found: %s"), *OutNameA));
        }
        OutB = LoadBlueprintFlexible(OutNameB);
        if (!OutB)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blueprint B not found: %s"), *OutNameB));
        }
        return nullptr;
    }
}

// ===========================================================================
// Shared plumbing of the asset-registry readers below: the parameter errors they
// all answer with, the by-name Blueprint and asset lookups, the per-entry JSON
// shapes, the package-edge listing and the cycle search.
// ===========================================================================
namespace UEAgentAnalysisCommandsImpl
{
    using AssetCommandHelpers::AppendAmbiguity;
    using AssetCommandHelpers::AssetNotFoundError;
    using AssetCommandHelpers::EmptyParamError;
    using AssetCommandHelpers::RangeError;

    /** A Blueprint asset found by name, plus every same-named candidate when the short-name scan was ambiguous. */
    using FBlueprintByName = AssetCommandHelpers::FNamedAsset;

    TSharedPtr<FJsonObject> DependencyKindError()
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'dependency_type' must be 'hard', 'soft', or 'all'"));
    }

    /** The required blueprint_name param: the error reply to send back, or nullptr once OutName holds a name. */
    TSharedPtr<FJsonObject> ReadBlueprintName(const TSharedPtr<FJsonObject>& Params, FString& OutName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), OutName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (OutName.IsEmpty())
        {
            return EmptyParamError(TEXT("blueprint_name"));
        }
        return nullptr;
    }

    /**
     * blueprint_name and the optional max_results (1..10000, default 1000) of the Blueprint listings: the error
     * reply to send back, or nullptr once both are valid.
     */
    TSharedPtr<FJsonObject> ReadBlueprintListing(const TSharedPtr<FJsonObject>& Params, FString& OutName, int32& OutCap)
    {
        if (TSharedPtr<FJsonObject> Invalid = ReadBlueprintName(Params, OutName))
        {
            return Invalid;
        }
        OutCap = 1000;
        Params->TryGetNumberField(TEXT("max_results"), OutCap);
        return RangeError(TEXT("max_results"), OutCap, 1, 10000);
    }

    /**
     * dependency_type (already lower-cased) as a registry query. "all" is the default-constructed query:
     * no required or excluded flags, so hard and soft edges together -- exactly what the two-argument
     * GetDependencies/GetReferencers overloads mean, so a call without the parameter gets both.
     * Any other word than all/hard/soft is refused.
     */
    bool ParseDependencyKind(const FString& Kind, UE::AssetRegistry::FDependencyQuery& OutQuery)
    {
        if (Kind == TEXT("all"))
        {
            OutQuery = UE::AssetRegistry::FDependencyQuery();
            return true;
        }
        const bool bHard = (Kind == TEXT("hard"));
        if (!bHard && Kind != TEXT("soft"))
        {
            return false;
        }
        OutQuery = UE::AssetRegistry::FDependencyQuery(bHard
            ? UE::AssetRegistry::EDependencyQuery::Hard
            : UE::AssetRegistry::EDependencyQuery::Soft);
        return true;
    }

    /**
     * Resolve-and-load step of the by-name Blueprint readers: Name is looked up among the plain UBlueprint assets
     * (that class only, not its subclasses), where a dated backup copy is the usual second hit of an ambiguous name.
     * Returns the error reply to send back, or nullptr once OutBlueprint is loaded (and, with bNeedGeneratedClass,
     * also has a generated class).
     */
    TSharedPtr<FJsonObject> LoadNamedBlueprint(const FString& Name, bool bNeedGeneratedClass,
        FBlueprintByName& OutFound, UBlueprint*& OutBlueprint)
    {
        OutBlueprint = nullptr;
        if (!AssetCommandHelpers::FindAssetByExactName(Name, UBlueprint::StaticClass(), OutFound))
        {
            return mcp_common::BlueprintNotFoundReply(Name);
        }
        OutBlueprint = Cast<UBlueprint>(OutFound.Asset.GetAsset());
        if (bNeedGeneratedClass && (!OutBlueprint || !OutBlueprint->GeneratedClass))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to load blueprint or generated class: %s"), *Name));
        }
        if (!OutBlueprint)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load blueprint: %s"), *Name));
        }
        return nullptr;
    }

    /** { blueprint_name, blueprint_path }: the identity the Blueprint readers open their reply with. */
    TSharedPtr<FJsonObject> StartBlueprintReply(const FString& Requested, const FString& BlueprintPath)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("blueprint_name"), Requested);
        Reply->SetStringField(TEXT("blueprint_path"), BlueprintPath);
        return Reply;
    }

    /** The listing part of a Blueprint reader's reply: { <Field>, <Field>_count, max_results, results_truncated }. */
    void AddBlueprintListing(const TSharedPtr<FJsonObject>& Reply, const FString& Field,
        const TArray<TSharedPtr<FJsonValue>>& Listed, int32 Cap, bool bTruncated)
    {
        Reply->SetArrayField(Field, Listed);
        Reply->SetNumberField(Field + TEXT("_count"), Listed.Num());
        Reply->SetNumberField(TEXT("max_results"), Cap);
        Reply->SetBoolField(TEXT("results_truncated"), bTruncated);
    }

    const TCHAR* BlueprintTypeLabel(EBlueprintType Type)
    {
        switch (Type)
        {
            case BPTYPE_Normal:          return TEXT("Normal");
            case BPTYPE_Const:           return TEXT("Const");
            case BPTYPE_MacroLibrary:    return TEXT("MacroLibrary");
            case BPTYPE_Interface:       return TEXT("Interface");
            case BPTYPE_LevelScript:     return TEXT("LevelScript");
            case BPTYPE_FunctionLibrary: return TEXT("FunctionLibrary");
            default:                     return TEXT("Unknown");
        }
    }

    /** One node of the Blueprint's OWN construction script, in the shape get_blueprint_components_list reports. */
    TSharedPtr<FJsonObject> DescribeOwnScsNode(const USCS_Node* Node)
    {
        UActorComponent* Template = Node->ComponentTemplate;
        const FString VariableName = Node->GetVariableName().ToString();

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), VariableName);
        Entry->SetStringField(TEXT("class"), Template->GetClass()->GetName());
        // The text before the first underscore doubles as a loose category ("Mesh_Body" -> "Mesh").
        int32 Underscore = INDEX_NONE;
        Entry->SetStringField(TEXT("category"),
            VariableName.FindChar(TEXT('_'), Underscore) ? VariableName.Left(Underscore) : FString(TEXT("Default")));
        Entry->SetBoolField(TEXT("is_root"), Node->IsRootNode());
        Entry->SetBoolField(TEXT("is_scene_component"), Template->IsA<USceneComponent>());

        if (const USceneComponent* Scene = Cast<USceneComponent>(Template))
        {
            Entry->SetStringField(TEXT("mobility"), mcp_common::MobilityLabel(Scene->Mobility));
        }
        if (const UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(Template))
        {
            Entry->SetBoolField(TEXT("simulates_physics"), Primitive->IsSimulatingPhysics());
            Entry->SetBoolField(TEXT("collision_enabled"), Primitive->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
        }
        if (const UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(Template))
        {
            const UStaticMesh* Mesh = MeshComponent->GetStaticMesh();
            Entry->SetBoolField(TEXT("has_static_mesh"), Mesh != nullptr);
            if (Mesh != nullptr)
            {
                Entry->SetStringField(TEXT("static_mesh_path"), Mesh->GetPathName());
            }
            Entry->SetNumberField(TEXT("material_count"), MeshComponent->GetNumMaterials());
        }
        return Entry;
    }

    /** One entry of UBlueprint::NewVariables: identity, tooltip and the property flags that matter for co-op and UI. */
    TSharedPtr<FJsonObject> DescribeNewVariable(const FBPVariableDescription& Variable)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Variable.VarName.ToString());
        Entry->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
        Entry->SetStringField(TEXT("category"), Variable.Category.ToString());
        // FriendlyName is a plain FString on the description, not an FText.
        Entry->SetStringField(TEXT("tooltip"), Variable.FriendlyName);

        struct FFlagField
        {
            const TCHAR* Key;
            uint64 Mask;
        };
        const FFlagField FlagFields[] = {
            { TEXT("is_public"),              CPF_BlueprintVisible },
            { TEXT("is_editable"),            CPF_Edit },
            { TEXT("is_blueprint_read_only"), CPF_BlueprintReadOnly },
            { TEXT("is_replicated"),          CPF_Net },
            { TEXT("is_expose_on_spawn"),     CPF_ExposeOnSpawn },
        };
        for (const FFlagField& Field : FlagFields)
        {
            Entry->SetBoolField(Field.Key, (Variable.PropertyFlags & Field.Mask) != 0);
        }
        return Entry;
    }

    /**
     * True when Graph overrides something: a function of the same name already on the parent chain, or one
     * of an interface the Blueprint implements. OutDeclaringClass names where it was found.
     */
    bool FindOverriddenDeclaration(const UBlueprint* Blueprint, const UEdGraph* Graph, FString& OutDeclaringClass)
    {
        const FName GraphName = Graph->GetFName();
        if (UClass* Parent = Blueprint->ParentClass)
        {
            if (const UFunction* Inherited = Parent->FindFunctionByName(GraphName))
            {
                const UClass* Owner = Inherited->GetOwnerClass();
                OutDeclaringClass = Owner ? Owner->GetPathName() : FString();
                return true;
            }
        }
        for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
        {
            const UClass* InterfaceClass = Implemented.Interface.Get();
            if (InterfaceClass && InterfaceClass->FindFunctionByName(GraphName))
            {
                OutDeclaringClass = InterfaceClass->GetPathName();
                return true;
            }
        }
        return false;
    }

    /** Purity is read off the graph's own entry node (FUNC_BlueprintPure), never inferred. */
    bool IsPureGraph(UEdGraph* Graph)
    {
        TArray<UK2Node_FunctionEntry*> EntryNodes;
        Graph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
        if (EntryNodes.Num() == 0 || EntryNodes[0] == nullptr)
        {
            return false;
        }
        return (static_cast<uint32>(EntryNodes[0]->GetFunctionFlags()) & static_cast<uint32>(FUNC_BlueprintPure)) != 0;
    }

    /**
     * get_asset_dependencies and get_asset_dependents are one query run in opposite directions, so both
     * handlers land here and only choose the direction: outgoing edges (what the asset needs) or incoming
     * ones (who needs it). Package-level edges from the Asset Registry, optionally without /Engine and
     * /Script entries -- those are skipped without spending the max_results budget, yet still counted in
     * the raw total.
     */
    TSharedPtr<FJsonObject> ListPackageEdges(const TSharedPtr<FJsonObject>& Params, bool bIncoming)
    {
        FString Target;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), Target, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        int32 Cap = 5000;
        Params->TryGetNumberField(TEXT("max_results"), Cap);
        bool bSkipEngine = true;
        Params->TryGetBoolField(TEXT("exclude_engine"), bSkipEngine);
        FString Kind = TEXT("all");
        Params->TryGetStringField(TEXT("dependency_type"), Kind);

        if (Target.IsEmpty())
        {
            return UEAgentAnalysisCommandsImpl::EmptyParamError(TEXT("asset_name"));
        }
        if (TSharedPtr<FJsonObject> OutOfRange = UEAgentAnalysisCommandsImpl::RangeError(TEXT("max_results"), Cap, 1, 50000))
        {
            return OutOfRange;
        }
        Kind = Kind.ToLower();
        UE::AssetRegistry::FDependencyQuery Query;
        if (!UEAgentAnalysisCommandsImpl::ParseDependencyKind(Kind, Query))
        {
            return UEAgentAnalysisCommandsImpl::DependencyKindError();
        }

        IAssetRegistry& Registry = mcp_common::AssetRegistry();
        AssetCommandHelpers::FNamedAsset Found;
        if (!AssetCommandHelpers::FindAssetByExactName(Target, nullptr, Found))
        {
            return UEAgentAnalysisCommandsImpl::AssetNotFoundError(Target);
        }
        const FAssetData& Subject = Found.Asset;

        TArray<FName> Edges;
        if (bIncoming)
        {
            Registry.GetReferencers(Subject.PackageName, Edges, UE::AssetRegistry::EDependencyCategory::Package, Query);
        }
        else
        {
            Registry.GetDependencies(Subject.PackageName, Edges, UE::AssetRegistry::EDependencyCategory::Package, Query);
        }

        // Truncated means an edge that would have been listed was left out -- not that the list
        // happens to hold exactly Cap entries.
        TArray<TSharedPtr<FJsonValue>> Listed;
        bool bTruncated = false;
        for (const FName& Edge : Edges)
        {
            const FString EdgePackage = Edge.ToString();
            const bool bEngineSide = EdgePackage.StartsWith(TEXT("/Engine/")) || EdgePackage.StartsWith(TEXT("/Script/"));
            if (bSkipEngine && bEngineSide)
            {
                continue;
            }
            if (Listed.Num() >= Cap)
            {
                bTruncated = true;
                break;
            }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("package_name"), EdgePackage);
            // Class and loaded state come from the first asset the registry knows in that package, if any.
            TArray<FAssetData> PackageAssets;
            Registry.GetAssetsByPackageName(Edge, PackageAssets);
            if (PackageAssets.Num() > 0)
            {
                Entry->SetStringField(TEXT("asset_type"), PackageAssets[0].AssetClassPath.ToString());
                Entry->SetBoolField(TEXT("is_loaded"), PackageAssets[0].IsAssetLoaded());
            }
            Listed.Add(MakeShared<FJsonValueObject>(Entry));
        }

        const FString ListKey = bIncoming ? TEXT("dependents") : TEXT("dependencies");
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("asset_name"), Target);
        Reply->SetStringField(TEXT("asset_path"), Subject.GetObjectPathString());
        Reply->SetArrayField(ListKey, Listed);
        Reply->SetNumberField(ListKey + TEXT("_count"), Listed.Num());
        Reply->SetNumberField(TEXT("total_") + ListKey, Edges.Num());
        Reply->SetNumberField(TEXT("max_results"), Cap);
        Reply->SetBoolField(TEXT("results_truncated"), bTruncated);
        Reply->SetBoolField(TEXT("exclude_engine"), bSkipEngine);
        Reply->SetStringField(TEXT("dependency_type"), Kind);
        return Reply;
    }

    /**
     * Depth-first walk over package dependencies that stops at the first cycle it closes. A package already
     * on the current branch closes a cycle; one finished on another branch is not entered again. While the
     * recursion unwinds after a hit, every package on the way back is appended to CycleTail, so it reads
     * innermost first.
     */
    struct FCycleProbe
    {
        FCycleProbe(IAssetRegistry& InRegistry, const UE::AssetRegistry::FDependencyQuery& InQuery,
            int32 InDepthLimit, int32 InVisitLimit)
            : Registry(InRegistry)
            , Query(InQuery)
            , DepthLimit(InDepthLimit)
            , VisitLimit(InVisitLimit)
        {
        }

        IAssetRegistry& Registry;
        UE::AssetRegistry::FDependencyQuery Query;
        int32 DepthLimit;
        int32 VisitLimit;

        TSet<FName> Seen;
        TSet<FName> Branch;
        TArray<FName> CycleTail;
        int32 SeenCount = 0;
        int32 DeepestLevel = 0;
        bool bCycle = false;
        bool bLimited = false;

        bool Walk(FName Package, int32 Level)
        {
            if (Level > DepthLimit || SeenCount > VisitLimit)
            {
                bLimited = true;
                return false;
            }
            DeepestLevel = FMath::Max(DeepestLevel, Level);
            if (Branch.Contains(Package))
            {
                bCycle = true;
                return true;
            }
            if (Seen.Contains(Package))
            {
                return false;
            }
            Seen.Add(Package);
            Branch.Add(Package);
            ++SeenCount;

            TArray<FName> Next;
            Registry.GetDependencies(Package, Next, UE::AssetRegistry::EDependencyCategory::Package, Query);
            for (const FName& Dependency : Next)
            {
                if (Walk(Dependency, Level + 1))
                {
                    CycleTail.Add(Dependency);
                    return true;
                }
            }
            Branch.Remove(Package);
            return false;
        }
    };
}

// Blueprint structure

// MCP:get_blueprint_parent - Read which class a Blueprint ASSET inherits from: parent class name and path, whether that parent is native C++ or another Blueprint, and the Blueprint's own type (Normal / Interface / MacroLibrary / FunctionLibrary / LevelScript / Const). The first question to ask about an unfamiliar Blueprint, and the way to tell whether a library class sneaked into the parent chain. It returns ONE level -- get_class_hierarchy walks the whole ancestor chain of the compiled class, get_blueprint_children lists descendants, and verify_reparent_success checks the chain against an expectation after a reparent. WARNING: an ambiguous bare name still resolves to the FIRST match and only lists the rest in `ambiguous_matches` (the other match is often a dated backup copy that backup_asset wrote under /Game/MCP_Backups), so run resolve_asset_by_name first or pass the full path.
//   @param blueprint_name - Blueprint asset short name (exact) or full package name. The full package name is tried first; the bare-name fallback scans /Game only, matching UBlueprint assets, and picks the first hit.
//   @returns { blueprint_name: string, blueprint_path: string, blueprint_valid: bool, parent_class: string, parent_class_path: string, has_parent: bool, parent_is_native: bool, blueprint_type: string, ambiguous_matches?: [string] }
TSharedPtr<FJsonObject> HandleGetBlueprintParent(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Requested;
    if (TSharedPtr<FJsonObject> Invalid = Impl::ReadBlueprintName(Params, Requested))
    {
        return Invalid;
    }

    Impl::FBlueprintByName Found;
    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::LoadNamedBlueprint(Requested, /*bNeedGeneratedClass*/ false, Found, Blueprint))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartBlueprintReply(Requested, Found.Asset.GetObjectPathString());
    Reply->SetBoolField(TEXT("blueprint_valid"), IsValid(Blueprint));
    Reply->SetStringField(TEXT("blueprint_type"), Impl::BlueprintTypeLabel(Blueprint->BlueprintType));

    // Exactly one level up. has_parent is always present; the parent_* fields only when there is one.
    UClass* const Parent = Blueprint->ParentClass;
    Reply->SetBoolField(TEXT("has_parent"), Parent != nullptr);
    if (Parent != nullptr)
    {
        Reply->SetStringField(TEXT("parent_class"), Parent->GetName());
        Reply->SetStringField(TEXT("parent_class_path"), Parent->GetPathName());
        Reply->SetBoolField(TEXT("parent_is_native"), Parent->IsNative());
    }

    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_blueprint_children - List every Blueprint asset that inherits from a given Blueprint -- the "what breaks if I change this base class?" blast-radius query, and the inverse of get_blueprint_parent. Use find_blueprint_assets_by_class when the base is a NATIVE C++ class rather than a Blueprint, get_class_hierarchy to walk upwards instead, and get_asset_referencers for references that are not inheritance at all.
//   @param blueprint_name - Blueprint asset short name (exact) or full package name; an ambiguous bare name resolves to the first hit and lists the rest in `ambiguous_matches`
//   @param max_results - Maximum children to return, 1..10000 (default 1000); compare with children_count and results_truncated
//   @returns { blueprint_name: string, blueprint_path: string, children: [object], children_count: int, max_results: int, results_truncated: bool, parent_blueprint_valid: bool, ambiguous_matches?: [string] }
TSharedPtr<FJsonObject> HandleGetBlueprintChildren(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Requested;
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = Impl::ReadBlueprintListing(Params, Requested, Cap))
    {
        return Invalid;
    }

    Impl::FBlueprintByName Found;
    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::LoadNamedBlueprint(Requested, /*bNeedGeneratedClass*/ true, Found, Blueprint))
    {
        return Failure;
    }

    // Descendants come from the class tree the registry keeps in memory, never from loading candidate
    // assets: loading every Blueprint of the project one by one on the game thread would hang the
    // editor. Loading the ONE base Blueprint above is fine.
    const FTopLevelAssetPath BaseClass = Blueprint->GeneratedClass->GetClassPathName();
    TSet<FTopLevelAssetPath> Descendants;
    mcp_common::AssetRegistry().GetDerivedClassNames({ BaseClass }, TSet<FTopLevelAssetPath>(), Descendants);

    TArray<TSharedPtr<FJsonValue>> Children;
    bool bTruncated = false;
    for (const FTopLevelAssetPath& ChildClass : Descendants)
    {
        const FString Package = ChildClass.GetPackageName().ToString();
        if (ChildClass == BaseClass || !Package.StartsWith(TEXT("/Game")))
        {
            continue;   // the base's own class, or a class that is not project content
        }
        if (Children.Num() >= Cap)
        {
            bTruncated = true;   // a child that would have been listed was left out
            break;
        }

        // The generated class is "<Asset>_C" and lives in the asset's own package.
        FString AssetName = ChildClass.GetAssetName().ToString();
        AssetName.RemoveFromEnd(TEXT("_C"));

        TSharedPtr<FJsonObject> Child = MakeShared<FJsonObject>();
        Child->SetStringField(TEXT("name"), AssetName);
        Child->SetStringField(TEXT("path"), Package + TEXT(".") + AssetName);
        Child->SetStringField(TEXT("package"), Package);
        Child->SetStringField(TEXT("class_path"), ChildClass.ToString());
        Children.Add(MakeShared<FJsonValueObject>(Child));
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartBlueprintReply(Requested, Found.Asset.GetObjectPathString());
    Reply->SetBoolField(TEXT("parent_blueprint_valid"), IsValid(Blueprint));
    Impl::AddBlueprintListing(Reply, TEXT("children"), Children, Cap, bTruncated);
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_blueprint_components_list - List the components a Blueprint declares in its OWN Simple Construction Script, each with class, root flag, scene/primitive flags, mobility, physics and collision state, and static-mesh path + material count when it has one. WARNING: it shows ONLY this Blueprint's own SCS: components inherited from a parent Blueprint and native components from the C++ CDO are INVISIBLE here, so a short list is not proof the component is missing -- get_blueprint_scs_tree returns native + own SCS + inherited, source-classified, and is the right call whenever the answer matters. Use get_actor_components for a live actor instance instead of an asset.
//   @param blueprint_name - Blueprint asset short name (exact) or full package name; an ambiguous bare name resolves to the first hit and lists the rest in `ambiguous_matches`
//   @param max_results - Maximum components to return, 1..10000 (default 1000)
//   @param include_inherited - IGNORED: accepted and parsed but never used. Inherited components are never included here whatever you pass -- call get_blueprint_scs_tree for those.
//   @returns { blueprint_name: string, blueprint_path: string, components: [object], components_count: int, max_results: int, results_truncated: bool, has_simple_construction_script: bool, blueprint_valid: bool, ambiguous_matches?: [string] }
TSharedPtr<FJsonObject> HandleGetBlueprintComponentsList(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Requested;
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = Impl::ReadBlueprintListing(Params, Requested, Cap))
    {
        return Invalid;
    }
    // Accepted for compatibility and deliberately unused: this listing only ever covers the Blueprint's
    // own SCS (get_blueprint_scs_tree is the one that walks native and inherited components).
    bool bIgnoredIncludeInherited = false;
    Params->TryGetBoolField(TEXT("include_inherited"), bIgnoredIncludeInherited);

    Impl::FBlueprintByName Found;
    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::LoadNamedBlueprint(Requested, /*bNeedGeneratedClass*/ false, Found, Blueprint))
    {
        return Failure;
    }

    TArray<TSharedPtr<FJsonValue>> Components;
    bool bTruncated = false;
    if (USimpleConstructionScript* OwnScs = Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* Node : OwnScs->GetAllNodes())
        {
            if (Node == nullptr || Node->ComponentTemplate == nullptr)
            {
                continue;
            }
            if (Components.Num() >= Cap)
            {
                bTruncated = true;   // a component that would have been listed was left out
                break;
            }
            Components.Add(MakeShared<FJsonValueObject>(Impl::DescribeOwnScsNode(Node)));
        }
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartBlueprintReply(Requested, Found.Asset.GetObjectPathString());
    Reply->SetBoolField(TEXT("blueprint_valid"), IsValid(Blueprint));
    Reply->SetBoolField(TEXT("has_simple_construction_script"), Blueprint->SimpleConstructionScript != nullptr);
    Impl::AddBlueprintListing(Reply, TEXT("components"), Components, Cap, bTruncated);
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_blueprint_scs_tree - Enumerate the COMPLETE component tree of a Blueprint asset -- native components from the C++ CDO, its own SCS nodes, and everything inherited down the ancestor chain -- each classified by source with its class, attach parent/socket and transform, most-derived-wins. The component listing to use by default: get_blueprint_components_list sees ONLY the Blueprint's own SCS and silently omits native and inherited ones. `component_class` filters by class OR ANY SUBCLASS across all three sources, which Actor::GetComponentsByClass cannot do (it only ever sees a constructed instance's OwnedComponents, never SCS templates on a Blueprint nobody instantiated). WARNING: before concluding a component does not exist, read `omitted_total`, NOT results_truncated: null templates and name-shadowed ancestor entries are dropped without setting it, and omitted_duplicate_names[]/omitted_null_templates[] name what disappeared (counted before the component_class filter). `scs_cdo_desync` / `generated_property_exists` flag an SCS that disagrees with the compiled class -- the fingerprint of a half-reverted asset. See diff_blueprint_components to compare two trees, get_actor_components for a live instance.
//   @param blueprint_name - Blueprint or AnimBlueprint asset short name (exact) or full /Game path; an ambiguous bare name resolves to one match without saying which, so pass the path when it matters
//   @param include_native - Include C++/native components from the CDO (default true)
//   @param include_inherited - Include components inherited from ancestor Blueprint classes (default true)
//   @param component_class - Optional class filter (short name, /Script/Module.Class, or /Game/... Blueprint path). Matches this class OR ANY SUBCLASS of it (same semantics as the engine's own GetComponentsByClass), evaluated against every source (native CDO, own SCS, inherited SCS) -- unlike Actor::GetComponentsByClass/K2_GetComponentsByClass, which only ever see a live actor's already-constructed OwnedComponents and are blind to SCS node templates on a Blueprint that hasn't been instantiated yet.
//   @param max_results - Cap on components returned, 1..10000 (default 1000)
//   @returns { blueprint_name: string, blueprint_path: string, blueprint_valid: bool, parent_class: string, parent_class_path: string, components: [object], components_count: int, counts: object, has_simple_construction_script: bool, results_truncated: bool, max_results: int, component_class_filter: string|null, component_class_resolved: bool, desync_count: int, blueprint_status: string, generated_class_available: bool, omitted_duplicate_names: [{name, source, owner_class, shadowed_by?}], omitted_duplicate_count: int, omitted_null_templates: [{name, source, owner_class}], omitted_null_template_count: int, omitted_total: int } -- each component record also carries generated_property_exists (bool|null) and scs_cdo_desync (bool|null): the compiler mints one FObjectProperty per SCS node with the SAME FName as the node (SCS_Node.h, KismetCompiler.cpp), so a MISSING property means the SCS and the compiled class disagree -- the fingerprint of an asset left half-reverted. This tool emits Node->GetVariableName() VERBATIM; it does NOT normalize a trailing digit.
//   WARNING: omitted_*: `components` is NOT necessarily every component the walk saw. Two paths drop entries and
//     neither sets results_truncated (which only ever covered the max_results cap): a USCS_Node with a NULL
//     ComponentTemplate, and a name already claimed by a more-derived record (correct most-derived-wins
//     dedup -- the shadowed ancestor entry is what disappears; `shadowed_by` names the class that won).
//     CHECK omitted_total, not just results_truncated, before concluding a
//     component does not exist. The counts are taken BEFORE the component_class filter, so they describe
//     the raw native+SCS+inherited walk rather than the filtered array.
TSharedPtr<FJsonObject> HandleGetBlueprintScsTree(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Requested;
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = Impl::ReadBlueprintListing(Params, Requested, Cap))
    {
        return Invalid;
    }

    // Which sources the walk covers besides the Blueprint's own SCS; both are on unless switched off.
    bool bWithNative = true;
    bool bWithInherited = true;
    Params->TryGetBoolField(TEXT("include_native"), bWithNative);
    Params->TryGetBoolField(TEXT("include_inherited"), bWithInherited);

    // component_class is the SCS-aware "by class" filter. Actor::GetComponentsByClass (and the Blueprint
    // node of the same name) only ever sees a constructed instance's OwnedComponents, never the SCS node
    // templates of a Blueprint that nobody has instantiated.
    FString FilterText;
    const bool bHasClassFilter = Params->TryGetStringField(TEXT("component_class"), FilterText) && !FilterText.IsEmpty();
    UClass* FilterClass = bHasClassFilter ? FUEAgentGraphUtils::ResolveClass(FilterText) : nullptr;
    if (bHasClassFilter && FilterClass == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve component_class: %s"), *FilterText));
    }

    // Short name, full path or AnimBlueprint alike.
    UBlueprint* Blueprint = LoadBlueprintFlexible(Requested);
    if (Blueprint == nullptr)
    {
        return mcp_common::BlueprintNotFoundReply(Requested);
    }

    // Native CDO + own SCS + inherited ancestors, through the collector diff_blueprint_components shares
    // (same walk and most-derived-wins dedup; scene components also carry a `transform`).
    TArray<TSharedPtr<FJsonValue>> Records;
    TMap<FName, TSharedPtr<FJsonObject>> RecordsByName;   // only diff_blueprint_components reads it
    int32 NativeCount = 0;
    int32 ScsCount = 0;
    int32 InheritedCount = 0;
    bool bCapped = false;
    // The collector drops entries on two paths (a null ComponentTemplate, and a name already claimed
    // further down the ancestor chain), and results_truncated only describes the max_results cap -- so
    // without these two lists a short list would look exactly like a complete one. They are the
    // collector's own account of what it dropped; Records is unchanged.
    TArray<TSharedPtr<FJsonValue>> DroppedDuplicates;
    TArray<TSharedPtr<FJsonValue>> DroppedNullTemplates;
    CollectBlueprintComponents(Blueprint, bWithNative, bWithInherited, Cap,
        Records, RecordsByName, NativeCount, ScsCount, InheritedCount, bCapped,
        &DroppedDuplicates, &DroppedNullTemplates);

    // Class filter (subclasses included), applied once the whole native+SCS+inherited set is known so the
    // shared collector keeps the signature and behavior its other caller relies on. The per-source counts
    // are recomputed over the survivors.
    if (FilterClass != nullptr)
    {
        TArray<TSharedPtr<FJsonValue>> Kept;
        NativeCount = 0;
        ScsCount = 0;
        InheritedCount = 0;
        for (const TSharedPtr<FJsonValue>& Record : Records)
        {
            const TSharedPtr<FJsonObject> Fields = Record->AsObject();
            if (!Fields.IsValid())
            {
                continue;
            }
            FString ClassPath;
            Fields->TryGetStringField(TEXT("class_path"), ClassPath);
            UClass* RecordClass = ClassPath.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *ClassPath);
            if (RecordClass == nullptr || !RecordClass->IsChildOf(FilterClass))
            {
                continue;
            }
            Kept.Add(Record);

            FString Source;
            Fields->TryGetStringField(TEXT("source"), Source);
            if (Source == TEXT("native"))
            {
                ++NativeCount;
            }
            else if (Source == TEXT("scs"))
            {
                ++ScsCount;
            }
            else if (Source == TEXT("inherited"))
            {
                ++InheritedCount;
            }
        }
        Records = MoveTemp(Kept);
    }

    // Additive desync fields. The compiler mints ONE FObjectProperty on the generated class per SCS
    // node, with the SAME FName as the node's variable (SCS_Node.h + KismetCompiler.cpp --
    // which is why there is no `generated_property_name` field: it would always equal `name`). A node whose
    // property is MISSING means the SCS and the compiled class disagree, the signature of an asset left
    // half-reverted by a partial rollback. NOTE: `scs_cdo_desync` compares SCS node against generated class,
    // NOT against the CDO's component list -- a Blueprint CDO legitimately owns only the native components
    // (SCS ones are instantiated at spawn), so that comparison would flag every SCS component. The fields
    // are written onto the records here, after the shared collector ran, so diff_blueprint_components'
    // FieldDiff set is untouched.
    UClass* CompiledClass = Blueprint->GeneratedClass;
    int32 DesyncCount = 0;
    for (const TSharedPtr<FJsonValue>& Record : Records)
    {
        const TSharedPtr<FJsonObject> Fields = Record.IsValid() ? Record->AsObject() : nullptr;
        if (!Fields.IsValid())
        {
            continue;
        }
        FString ComponentName;
        FString Source;
        Fields->TryGetStringField(TEXT("name"), ComponentName);
        Fields->TryGetStringField(TEXT("source"), Source);
        if (ComponentName.IsEmpty())
        {
            continue;
        }
        if (CompiledClass == nullptr)
        {
            // No compiled class at all: say so instead of guessing a bool.
            Fields->SetField(TEXT("generated_property_exists"), MakeShared<FJsonValueNull>());
            Fields->SetField(TEXT("scs_cdo_desync"), MakeShared<FJsonValueNull>());
            continue;
        }
        const bool bPropertyExists = FindFProperty<FObjectProperty>(CompiledClass, FName(*ComponentName)) != nullptr;
        // Native records are read FROM the CDO and can never be missing from it; only scs / inherited
        // records can disagree with the compiled class.
        const bool bDesync = Source != TEXT("native") && !bPropertyExists;
        Fields->SetBoolField(TEXT("generated_property_exists"), bPropertyExists);
        Fields->SetBoolField(TEXT("scs_cdo_desync"), bDesync);
        if (bDesync)
        {
            ++DesyncCount;
        }
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartBlueprintReply(Requested, Blueprint->GetPathName());
    Reply->SetBoolField(TEXT("blueprint_valid"), IsValid(Blueprint));
    UClass* const ParentClass = Blueprint->ParentClass;
    Reply->SetStringField(TEXT("parent_class"), ParentClass ? ParentClass->GetName() : FString());
    Reply->SetStringField(TEXT("parent_class_path"), ParentClass ? ParentClass->GetPathName() : FString());
    Reply->SetBoolField(TEXT("has_simple_construction_script"), Blueprint->SimpleConstructionScript != nullptr);
    Reply->SetStringField(TEXT("blueprint_status"), BlueprintStatusToString_Analysis(Blueprint->Status));
    Reply->SetBoolField(TEXT("generated_class_available"), CompiledClass != nullptr);

    TSharedPtr<FJsonObject> PerSource = MakeShared<FJsonObject>();
    PerSource->SetNumberField(TEXT("native"), NativeCount);
    PerSource->SetNumberField(TEXT("scs"), ScsCount);
    PerSource->SetNumberField(TEXT("inherited"), InheritedCount);
    Reply->SetObjectField(TEXT("counts"), PerSource);
    Impl::AddBlueprintListing(Reply, TEXT("components"), Records, Cap, bCapped);
    Reply->SetNumberField(TEXT("desync_count"), DesyncCount);

    // What the walk DROPPED. Counted BEFORE the component_class filter, so these describe the raw
    // native+SCS+inherited walk rather than the filtered `components` -- an omitted component never got
    // the chance to be filtered. omitted_total > 0 with results_truncated false is a list that is
    // short without being capped.
    Reply->SetArrayField(TEXT("omitted_duplicate_names"), DroppedDuplicates);
    Reply->SetNumberField(TEXT("omitted_duplicate_count"), DroppedDuplicates.Num());
    Reply->SetArrayField(TEXT("omitted_null_templates"), DroppedNullTemplates);
    Reply->SetNumberField(TEXT("omitted_null_template_count"), DroppedNullTemplates.Num());
    Reply->SetNumberField(TEXT("omitted_total"), DroppedDuplicates.Num() + DroppedNullTemplates.Num());

    if (bHasClassFilter)
    {
        Reply->SetStringField(TEXT("component_class_filter"), FilterClass->GetPathName());
    }
    else
    {
        Reply->SetField(TEXT("component_class_filter"), MakeShared<FJsonValueNull>());
    }
    Reply->SetBoolField(TEXT("component_class_resolved"), bHasClassFilter);
    return Reply;
}

// MCP:get_blueprint_functions_list - List every graph a Blueprint AUTHORED -- function graphs, MACROS, interface implementation graphs, event graphs and ubergraph pages -- each with its node count and a REAL is_override (resolved against the parent class chain AND every implemented interface, reporting the declaring class) and is_pure (read from the graph's UK2Node_FunctionEntry FUNC_BlueprintPure flag, not guessed). The map to read before get_graph_nodes, which needs an exact graph name. WARNING: `ubergraph_pages` is the authored EventGraph you actually see in the editor; `event_graphs` are the TRANSIENT compiled graphs and come back EMPTY on a Blueprint that has not compiled this session -- an empty event_graphs is not an absence of events. Use get_class_functions for the compiled class surface (including inherited and native functions) instead of the authored graphs, and get_function_info for one function's signature.
//   @param blueprint_name - Blueprint asset short name (exact) or full package name; an ambiguous bare name resolves to the first hit and lists the rest in `ambiguous_matches`
//   @param max_results - Maximum entries to return, 1..10000 (default 1000)
//   @param filter_type - Filter to 'all', 'functions', or 'events'. 'functions' covers Function/Macro/Interface graphs; 'events' covers Event/Ubergraph pages. Case-insensitive, and the singular ('function', 'event') is accepted as the same value.
//   @param include_macros - Include MacroGraphs (type 'Macro'). Default true.
//   @param include_interface_graphs - Include ImplementedInterfaces[].Graphs (type 'Interface'). Default true.
//   @param include_ubergraph_pages - Include UbergraphPages (type 'Ubergraph'). Default true.
//   @returns { blueprint_name: string, blueprint_path: string, functions: [{name, type, nodes_count, is_pure?, is_override?, override_declaring_class?, interface_class?}], functions_count: int, function_graphs_count: int, event_graphs_count: int, macro_graphs_count: int, interface_graphs_count: int, ubergraph_pages_count: int, override_count: int, filter_type: string, max_results: int, results_truncated: bool, blueprint_valid: bool, ambiguous_matches?: [string] }
TSharedPtr<FJsonObject> HandleGetBlueprintFunctionsList(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Requested;
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = Impl::ReadBlueprintListing(Params, Requested, Cap))
    {
        return Invalid;
    }

    // The description reads "all, functions, or events", which invites the singular, so the VALUE is
    // normalized before validation (trimmed, lower-cased, singular accepted) instead of failing a
    // filter_type="function" over one letter.
    FString Scope = TEXT("all");
    Params->TryGetStringField(TEXT("filter_type"), Scope);
    Scope = Scope.TrimStartAndEnd().ToLower();
    if (Scope == TEXT("function"))
    {
        Scope = TEXT("functions");
    }
    else if (Scope == TEXT("event"))
    {
        Scope = TEXT("events");
    }

    bool bWithMacros = true;
    bool bWithInterfaceGraphs = true;
    bool bWithUbergraphPages = true;
    Params->TryGetBoolField(TEXT("include_macros"), bWithMacros);
    Params->TryGetBoolField(TEXT("include_interface_graphs"), bWithInterfaceGraphs);
    Params->TryGetBoolField(TEXT("include_ubergraph_pages"), bWithUbergraphPages);

    const bool bFunctionSide = (Scope == TEXT("all") || Scope == TEXT("functions"));
    const bool bEventSide = (Scope == TEXT("all") || Scope == TEXT("events"));
    if (!bFunctionSide && !bEventSide)
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'filter_type' must be 'all', 'functions', or 'events'"));
    }

    Impl::FBlueprintByName Found;
    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::LoadNamedBlueprint(Requested, /*bNeedGeneratedClass*/ false, Found, Blueprint))
    {
        return Failure;
    }

    // One list in a fixed order -- functions, macros, interface graphs, compiled event graphs, ubergraph
    // pages -- that stops growing as soon as it holds Cap entries.
    TArray<TSharedPtr<FJsonValue>> Listed;
    int32 FunctionCount = 0;
    int32 MacroCount = 0;
    int32 InterfaceCount = 0;
    int32 EventCount = 0;
    int32 UbergraphCount = 0;
    int32 OverrideCount = 0;

    // HasRoom ends a walk once an entry has been turned away; TakeSlot is asked right before an
    // entry would be listed, so results_truncated means "something listable was left out", not "the
    // list happens to hold exactly Cap entries".
    bool bTruncated = false;
    auto HasRoom = [&bTruncated]() { return !bTruncated; };
    auto TakeSlot = [&Listed, &bTruncated, Cap]()
    {
        if (Listed.Num() >= Cap)
        {
            bTruncated = true;
            return false;
        }
        return true;
    };
    auto StartEntry = [](UEdGraph* Graph, const TCHAR* Kind)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Graph->GetName());
        Entry->SetStringField(TEXT("type"), Kind);
        Entry->SetNumberField(TEXT("nodes_count"), Graph->Nodes.Num());
        return Entry;
    };
    // Lists each non-null graph of Graphs, as Describe builds its entry, while there is room; returns how
    // many it listed.
    auto ListGraphs = [&Listed, &HasRoom, &TakeSlot](const TArray<TObjectPtr<UEdGraph>>& Graphs, auto&& Describe)
    {
        int32 Count = 0;
        for (UEdGraph* Graph : Graphs)
        {
            if (!HasRoom())
            {
                break;
            }
            if (Graph == nullptr)
            {
                continue;
            }
            if (!TakeSlot())
            {
                break;
            }
            Listed.Add(MakeShared<FJsonValueObject>(Describe(Graph)));
            ++Count;
        }
        return Count;
    };

    if (bFunctionSide)
    {
        // is_override is resolved against the parent chain and every implemented interface, naming
        // where the overridden function is declared.
        FunctionCount = ListGraphs(Blueprint->FunctionGraphs, [&StartEntry, &OverrideCount, Blueprint](UEdGraph* Graph)
        {
            FString DeclaredIn;
            const bool bOverrides = Impl::FindOverriddenDeclaration(Blueprint, Graph, DeclaredIn);
            TSharedPtr<FJsonObject> Entry = StartEntry(Graph, TEXT("Function"));
            Entry->SetBoolField(TEXT("is_pure"), Impl::IsPureGraph(Graph));
            Entry->SetBoolField(TEXT("is_override"), bOverrides);
            if (bOverrides)
            {
                Entry->SetStringField(TEXT("override_declaring_class"), DeclaredIn);
                ++OverrideCount;
            }
            return Entry;
        });

        // Macros are authored graphs that FunctionGraphs never contained. A macro has no UFunction,
        // so it is stated as neither pure nor an override rather than left to be guessed.
        if (bWithMacros)
        {
            MacroCount = ListGraphs(Blueprint->MacroGraphs, [&StartEntry](UEdGraph* Graph)
            {
                TSharedPtr<FJsonObject> Entry = StartEntry(Graph, TEXT("Macro"));
                Entry->SetBoolField(TEXT("is_pure"), false);
                Entry->SetBoolField(TEXT("is_override"), false);
                return Entry;
            });
        }

        // Interface implementation graphs (ImplementedInterfaces[].Graphs) always override.
        if (bWithInterfaceGraphs)
        {
            for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
            {
                if (!HasRoom())
                {
                    break;
                }
                const UClass* InterfaceClass = Implemented.Interface.Get();
                const FString InterfacePath = InterfaceClass ? InterfaceClass->GetPathName() : FString();
                const int32 Implementations = ListGraphs(Implemented.Graphs, [&StartEntry, &InterfacePath](UEdGraph* Graph)
                {
                    TSharedPtr<FJsonObject> Entry = StartEntry(Graph, TEXT("Interface"));
                    Entry->SetBoolField(TEXT("is_pure"), Impl::IsPureGraph(Graph));
                    Entry->SetBoolField(TEXT("is_override"), true);
                    Entry->SetStringField(TEXT("interface_class"), InterfacePath);
                    Entry->SetStringField(TEXT("override_declaring_class"), InterfacePath);
                    return Entry;
                });
                InterfaceCount += Implementations;
                OverrideCount += Implementations;
            }
        }
    }

    if (bEventSide)
    {
        // EventGraphs are the TRANSIENT compiled graphs: empty on a Blueprint not compiled this session.
        EventCount = ListGraphs(Blueprint->EventGraphs, [&StartEntry](UEdGraph* Graph)
        {
            return StartEntry(Graph, TEXT("Event"));
        });

        // Ubergraph pages are the AUTHORED EventGraph, which survives a session without a compile.
        if (bWithUbergraphPages)
        {
            UbergraphCount = ListGraphs(Blueprint->UbergraphPages, [&StartEntry](UEdGraph* Graph)
            {
                return StartEntry(Graph, TEXT("Ubergraph"));
            });
        }
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartBlueprintReply(Requested, Found.Asset.GetObjectPathString());
    Reply->SetBoolField(TEXT("blueprint_valid"), IsValid(Blueprint));
    Reply->SetStringField(TEXT("filter_type"), Scope);
    Reply->SetArrayField(TEXT("functions"), Listed);
    Reply->SetNumberField(TEXT("functions_count"), Listed.Num());
    Reply->SetNumberField(TEXT("function_graphs_count"), FunctionCount);
    Reply->SetNumberField(TEXT("macro_graphs_count"), MacroCount);
    Reply->SetNumberField(TEXT("interface_graphs_count"), InterfaceCount);
    Reply->SetNumberField(TEXT("event_graphs_count"), EventCount);
    Reply->SetNumberField(TEXT("ubergraph_pages_count"), UbergraphCount);
    Reply->SetNumberField(TEXT("override_count"), OverrideCount);
    Reply->SetNumberField(TEXT("max_results"), Cap);
    Reply->SetBoolField(TEXT("results_truncated"), bTruncated);
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// MCP:get_blueprint_variables_list - List the variables a Blueprint AUTHORED itself (UBlueprint::NewVariables) with name, pin category, category, tooltip and the flags that matter for co-op and UI work: is_public (BlueprintVisible), is_editable, is_blueprint_read_only, is_replicated (CPF_Net) and is_expose_on_spawn. The quick "what state does this Blueprint own, and is it replicated?" listing. WARNING: own variables only: inherited variables, native C++ UPROPERTYs and component-backed variables are NOT here -- use get_class_properties on the generated class (..._C) for the full property surface, or get_blueprint_variable_detailed_info for one variable's full descriptor and default value.
//   @param blueprint_name - Blueprint asset short name (exact) or full package name; an ambiguous bare name resolves to the first hit and lists the rest in `ambiguous_matches`
//   @param max_results - Maximum variables to return, 1..10000 (default 1000)
//   @returns { blueprint_name: string, blueprint_path: string, variables: [object], variables_count: int, max_results: int, results_truncated: bool, blueprint_valid: bool, ambiguous_matches?: [string] }
TSharedPtr<FJsonObject> HandleGetBlueprintVariablesList(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Requested;
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = Impl::ReadBlueprintListing(Params, Requested, Cap))
    {
        return Invalid;
    }

    Impl::FBlueprintByName Found;
    UBlueprint* Blueprint = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::LoadNamedBlueprint(Requested, /*bNeedGeneratedClass*/ false, Found, Blueprint))
    {
        return Failure;
    }

    // Only what this Blueprint declares itself; inherited and native properties are not in NewVariables.
    TArray<TSharedPtr<FJsonValue>> Variables;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Variables.Num() >= Cap)
        {
            break;
        }
        Variables.Add(MakeShared<FJsonValueObject>(Impl::DescribeNewVariable(Variable)));
    }

    TSharedPtr<FJsonObject> Reply = Impl::StartBlueprintReply(Requested, Found.Asset.GetObjectPathString());
    Reply->SetBoolField(TEXT("blueprint_valid"), IsValid(Blueprint));
    Impl::AddBlueprintListing(Reply, TEXT("variables"), Variables, Cap, Blueprint->NewVariables.Num() > Variables.Num());
    Impl::AppendAmbiguity(Reply, Found.AmbiguousMatches);
    return Reply;
}

// Asset dependencies

// MCP:get_asset_dependencies - List every package an asset DEPENDS ON (its outgoing edges), each with its class and loaded state -- "what does this asset drag in with it?", the input to a migration or import budget. Direction matters: this is what the asset NEEDS; get_asset_dependents / get_asset_referencers answer who needs IT. Use get_asset_dependencies_recursive for the transitive closure and check_asset_circular_dependency to detect cycles. WARNING: these are PACKAGE-level edges from the Asset Registry: they cannot tell you WHICH node or property creates the edge (that is find_blueprint_node_references), and a name built at runtime or resolved through a DataRegistry produces no edge at all.
//   @param asset_name - Exact asset name or full package name; the bare-name fallback scans /Game only and stops at the FIRST match, without reporting that others existed
//   @param max_results - Maximum dependencies to return, 1..50000 (default 5000). Read `results_truncated` for the real answer: `total_dependencies` counts the RAW registry edges before exclude_engine, so it exceeds dependencies_count on almost every asset without anything having been cut.
//   @param exclude_engine - Skip /Engine and /Script dependencies, default true. Filtered entries do NOT consume the max_results budget, but they ARE counted in total_dependencies.
//   @param dependency_type - Filter by dependency kind: "hard", "soft", or "all" (case-insensitive), default "all"
//   @returns { asset_name: string, asset_path: string, dependencies: [object], dependencies_count: int, total_dependencies: int, max_results: int, results_truncated: bool, exclude_engine: bool, dependency_type: string }
TSharedPtr<FJsonObject> HandleGetAssetDependencies(const TSharedPtr<FJsonObject>& Params)
{
    using UEAgentAnalysisCommandsImpl::ListPackageEdges;
    // Outgoing edges: what the asset needs.
    return ListPackageEdges(Params, /*bIncoming*/ false);
}

// MCP:get_asset_dependents - List every package that REFERENCES an asset (its incoming edges), each with its class and loaded state -- "who breaks if I delete or move this?", addressed by asset NAME. Direction matters: this is who needs IT; get_asset_dependencies is what it needs. Nearly the same query as get_asset_referencers -- prefer THAT one when the name could be ambiguous (it refuses to guess and echoes `resolved_package`, while this one silently takes the first /Game match), and prefer this one when you want the hard/soft split via dependency_type. Package-level edges only: use find_blueprint_node_references to find call sites inside graphs.
//   @param asset_name - Exact asset name or full package name; the bare-name fallback scans /Game only and stops at the FIRST match, without reporting that others existed
//   @param max_results - Maximum dependents to return, 1..50000 (default 5000). Read `results_truncated`: `total_dependents` counts the RAW registry edges before exclude_engine, so it can exceed dependents_count with nothing cut.
//   @param exclude_engine - Skip /Engine and /Script referencers, default true. Filtered entries do NOT consume the max_results budget, but they ARE counted in total_dependents.
//   @param dependency_type - Filter by dependency kind: "hard", "soft", or "all" (case-insensitive), default "all"
//   @returns { asset_name: string, asset_path: string, dependents: [object], dependents_count: int, total_dependents: int, max_results: int, results_truncated: bool, exclude_engine: bool, dependency_type: string }
TSharedPtr<FJsonObject> HandleGetAssetDependents(const TSharedPtr<FJsonObject>& Params)
{
    using UEAgentAnalysisCommandsImpl::ListPackageEdges;
    // Incoming edges (referencers): who needs the asset.
    return ListPackageEdges(Params, /*bIncoming*/ true);
}

// MCP:check_asset_circular_dependency - Depth-first search of an asset's dependency graph looking for a CYCLE, returning has_circular_dependency plus the actual circular_path that closes the loop and its length -- the diagnosis for cook failures, load-order surprises and Blueprints that will not compile in isolation. Complements get_asset_dependencies (one level out) and get_asset_dependencies_recursive (the whole closure without cycle detection). WARNING: check `search_limit_reached` before trusting a false: the walk stops at max_depth / max_visited, and a cycle beyond those limits reads exactly like no cycle at all.
//   @param asset_name - Exact asset name or full package name; the bare-name fallback scans /Game only and takes the first match
//   @param max_depth - Maximum recursion depth, default 100. Hitting it sets search_limit_reached and makes a `false` answer inconclusive.
//   @param max_visited - Maximum total nodes visited, default 10000. Hitting it also sets search_limit_reached.
//   @param dependency_type - Filter by dependency kind considered at every DFS step: "hard", "soft", or "all" (case-insensitive), default "all"
//   @returns { asset_name: string, asset_path: string, has_circular_dependency: bool, nodes_visited: int, max_depth_reached: int, search_limit_reached: bool, circular_path: [string] (the cycle itself, its first package repeated at the end), cycle_length: int (packages in the cycle), root_in_cycle: bool, lead_in_path?: [string] (root ... first package of the cycle, when the root only leads into it), suggestion?: string (the edge that closes the cycle), dependency_type: string }
TSharedPtr<FJsonObject> HandleCheckAssetCircularDependency(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentAnalysisCommandsImpl;

    FString Target;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), Target, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Safety valves: how deep one branch may go, and how many packages the whole walk may enter.
    int32 DepthLimit = 100;
    Params->TryGetNumberField(TEXT("max_depth"), DepthLimit);
    int32 VisitLimit = 10000;
    Params->TryGetNumberField(TEXT("max_visited"), VisitLimit);
    FString Kind = TEXT("all");
    Params->TryGetStringField(TEXT("dependency_type"), Kind);

    if (Target.IsEmpty())
    {
        return Impl::EmptyParamError(TEXT("asset_name"));
    }
    if (TSharedPtr<FJsonObject> OutOfRange = Impl::RangeError(TEXT("max_depth"), DepthLimit, 1, 500))
    {
        return OutOfRange;
    }
    if (TSharedPtr<FJsonObject> OutOfRange = Impl::RangeError(TEXT("max_visited"), VisitLimit, 100, 100000))
    {
        return OutOfRange;
    }
    // The kind applies at EVERY step of the walk, not only to the root's own edges.
    Kind = Kind.ToLower();
    UE::AssetRegistry::FDependencyQuery Query;
    if (!Impl::ParseDependencyKind(Kind, Query))
    {
        return Impl::DependencyKindError();
    }

    IAssetRegistry& Registry = mcp_common::AssetRegistry();
    AssetCommandHelpers::FNamedAsset Found;
    if (!AssetCommandHelpers::FindAssetByExactName(Target, nullptr, Found))
    {
        return Impl::AssetNotFoundError(Target);
    }
    const FAssetData& Root = Found.Asset;

    Impl::FCycleProbe Probe(Registry, Query, DepthLimit, VisitLimit);
    Probe.Walk(Root.PackageName, 0);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("asset_name"), Target);
    Reply->SetStringField(TEXT("asset_path"), Root.GetObjectPathString());
    Reply->SetStringField(TEXT("dependency_type"), Kind);
    Reply->SetBoolField(TEXT("has_circular_dependency"), Probe.bCycle);
    Reply->SetBoolField(TEXT("search_limit_reached"), Probe.bLimited);
    Reply->SetNumberField(TEXT("nodes_visited"), Probe.SeenCount);
    Reply->SetNumberField(TEXT("max_depth_reached"), Probe.DeepestLevel);
    Reply->SetNumberField(TEXT("max_depth_limit"), DepthLimit);
    Reply->SetNumberField(TEXT("max_visited_limit"), VisitLimit);

    if (Probe.bCycle)
    {
        // CycleTail was filled while the recursion unwound (innermost first): reversed and led by
        // the root it is the DFS path root -> ... -> X, whose last package X is the one met again.
        // The cycle is the stretch from X's first appearance to the end; what comes before it only
        // leads into the cycle (the root need not be part of it).
        TArray<FName> Path;
        Path.Add(Root.PackageName);
        for (int32 Index = Probe.CycleTail.Num() - 1; Index >= 0; --Index)
        {
            Path.Add(Probe.CycleTail[Index]);
        }
        const int32 CycleStart = Path.IndexOfByKey(Path.Last());
        auto ToJson = [&Path](int32 From, int32 To)
        {
            TArray<TSharedPtr<FJsonValue>> Out;
            for (int32 Index = From; Index <= To; ++Index)
            {
                Out.Add(MakeShared<FJsonValueString>(Path[Index].ToString()));
            }
            return Out;
        };
        Reply->SetArrayField(TEXT("circular_path"), ToJson(CycleStart, Path.Num() - 1));
        Reply->SetNumberField(TEXT("cycle_length"), Path.Num() - 1 - CycleStart);
        Reply->SetBoolField(TEXT("root_in_cycle"), CycleStart == 0);
        if (CycleStart > 0)
        {
            Reply->SetArrayField(TEXT("lead_in_path"), ToJson(0, CycleStart));
        }
        if (Path.Num() >= 2)
        {
            // The edge that closes the loop: the last step of the path.
            Reply->SetStringField(TEXT("suggestion"), FString::Printf(
                TEXT("Consider removing dependency from '%s' to '%s' to break the cycle"),
                *Path[Path.Num() - 2].ToString(), *Path.Last().ToString()));
        }
    }
    else if (Probe.bLimited)
    {
        // A cycle beyond the limits reads exactly like no cycle at all, so a negative answer is inconclusive.
        Reply->SetStringField(TEXT("warning"),
            TEXT("Search terminated early due to limits. Consider increasing max_depth or max_visited parameters."));
    }
    return Reply;
}

// ---------------------------------------------------------------------------
// Class-based BP enumeration + hard-reference lookup.
// ---------------------------------------------------------------------------

// MCP:find_blueprint_assets_by_class - Search a content path for every Blueprint ASSET whose generated class derives from a target UClass, returning asset name, asset path, generated class and the deepest NATIVE parent -- "give me every Blueprint that is a Pawn / a Widget / an ActorComponent" without hand-scanning folders. Use get_blueprint_children when the base is itself a Blueprint rather than a C++ class, list_derived_classes for classes instead of assets, and get_assets_by_tag when you want a registry tag rather than an inheritance test. WARNING: it LOADS every Blueprint under package_path to read its GeneratedClass, so scope the path narrowly on a big project rather than sweeping all of /Game.
//   @param class_name - UClass short name or /Script/ path (e.g. "Pawn", "/Script/Engine.Pawn"); a bare name is also retried with a "U" prefix. An unresolvable name is an error, not an empty result.
//   @param package_path - Content path to scan, recursive (default "/Game"). Narrow it: every Blueprint underneath gets loaded.
//   @returns { class_name, target_class, package_path, total, assets:[{ asset_name, asset_path, generated_class, native_parent }] }

TSharedPtr<FJsonObject> HandleFindBlueprintAssetsByClass(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
    FString PackagePath;
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    if (PackagePath.IsEmpty()) { PackagePath = TEXT("/Game"); }

    UClass* TargetClass = nullptr;
    // Try direct FindObject first (handles /Script/Module.Class paths).
    TargetClass = FindObject<UClass>(nullptr, *ClassName);
    if (!TargetClass)
    {
        // Try without the leading "U" (engine classes drop it: "APawn" -> "/Script/Engine.Pawn" doesn't work,
        // but "Pawn" alone usually works via LoadObject).
        TargetClass = LoadObject<UClass>(nullptr, *ClassName);
    }
    if (!TargetClass)
    {
        // Last resort: append "U" if missing.
        if (!ClassName.StartsWith(TEXT("U")) && !ClassName.StartsWith(TEXT("A")))
        {
            const FString Prefixed = FString::Printf(TEXT("U%s"), *ClassName);
            TargetClass = FindObject<UClass>(nullptr, *Prefixed);
        }
    }
    if (!TargetClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown class: %s (tried direct + LoadObject + U-prefix)"), *ClassName));
    }

    IAssetRegistry& AR = mcp_common::AssetRegistry();

    // Enumerate ALL assets under PackagePath, then filter to UBlueprint-derived via AssetClassPath.
    // Avoid GetAssetsByClass(UBlueprint, bSearchSubClasses=true) which can hang on a large registry.
    TArray<FAssetData> AllAssets;
    AR.GetAssetsByPath(FName(*PackagePath), AllAssets, /*bRecursive*/ true);

    const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FAssetData& AD : AllAssets)
    {
        // AssetClassPath filter -- fast, doesn't load the asset.
        if (AD.AssetClassPath != BlueprintClassPath)
        {
            continue;
        }
        UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
        if (!BP) { continue; }
        UClass* GenClass = BP->GeneratedClass;
        if (!GenClass || !GenClass->IsChildOf(TargetClass)) { continue; }

        // Find the deepest native parent class.
        UClass* NativeParent = GenClass;
        while (NativeParent && !NativeParent->IsNative())
        {
            NativeParent = NativeParent->GetSuperClass();
        }

        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("asset_name"), BP->GetName());
        E->SetStringField(TEXT("asset_path"), BP->GetPathName());
        E->SetStringField(TEXT("generated_class"), GenClass->GetName());
        E->SetStringField(TEXT("native_parent"), NativeParent ? NativeParent->GetName() : FString());
        Entries.Add(MakeShared<FJsonValueObject>(E));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("class_name"), ClassName);
    Data->SetStringField(TEXT("target_class"), TargetClass->GetName());
    Data->SetStringField(TEXT("package_path"), PackagePath);
    Data->SetNumberField(TEXT("total"), Entries.Num());
    Data->SetArrayField(TEXT("assets"), Entries);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_asset_referencers - List every asset that REFERENCES a target asset -- "who breaks if I delete, move or rename this?" -- wrapping AssetRegistry::GetReferencers over the Package category and returning each referencing package with its primary asset's name, path and class. It accepts an object path, a package path or a bare name, echoes `resolved_package` so you can confirm it queried the package you meant, and REFUSES an ambiguous bare name with the candidate list instead of picking one. Hard references only by default (the "what keeps this loaded" audit); set include_soft_references=true to include TSoftObjectPtr / FSoftObjectPath edges. Its mirror is get_asset_dependencies (what this asset needs); get_asset_dependents answers the same direction as this one from a bare asset name, and find_blueprint_node_references finds call sites INSIDE graphs, which package-level references cannot see.
//   @param asset_path - Target asset: an object path ('/Game/X/DT_ItemRows.DT_ItemRows'), a package path ('/Game/X/DT_ItemRows'), or an unambiguous bare asset name ('DT_ItemRows'). An ambiguous bare name is an error listing the candidates, never a silent pick.
//   @param include_soft_references - If true, also include soft referencers (default false -> hard-only).
//   @param max_results - Cap on number of returned referencers (1..50000); defaults to 5000.
//   @returns { asset_path, resolved_package, include_soft_references, total, returned, results_truncated, referencers:[{ package_name, asset_name?, asset_path?, asset_class? }] }

TSharedPtr<FJsonObject> HandleGetAssetReferencers(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    // Hard referencers only unless soft ones are asked for; max_results caps the returned list.
    bool bIncludeSoftRefs = false;
    Params->TryGetBoolField(TEXT("include_soft_references"), bIncludeSoftRefs);
    int32 MaxResults = 5000;
    Params->TryGetNumberField(TEXT("max_results"), MaxResults);
    if (TSharedPtr<FJsonObject> OutOfRange = UEAgentAnalysisCommandsImpl::RangeError(TEXT("max_results"), MaxResults, 1, 50000))
    {
        return OutOfRange;
    }

    IAssetRegistry& AR = mcp_common::AssetRegistry();

    // GetReferencers keys off a PACKAGE NAME ("/Game/Foo/DT_ItemRows"), so the caller's
    // asset_path must be normalized into one -- a bare basename ("DT_ItemRows") names no package,
    // and querying it would answer total:0. A silent "nobody references this" is the most
    // dangerous answer this tool can give (it invites a blind delete/migrate), so the package is
    // resolved properly, and an input that resolves to none fails loudly.
    // Accepts: object path ("/Game/X/DT_ItemRows.DT_ItemRows"), package path ("/Game/X/DT_ItemRows"),
    // or a bare short name ("DT_ItemRows") like the rest of the toolkit.
    FString PackageNameStr = FPackageName::ObjectPathToPackageName(AssetPath); // no-op without a ".Object" suffix
    {
        TArray<FAssetData> PackageAssets;
        if (FPackageName::IsValidLongPackageName(PackageNameStr))
        {
            AR.GetAssetsByPackageName(FName(*PackageNameStr), PackageAssets, /*bIncludeOnlyOnDiskAssets*/ false);
        }
        if (PackageAssets.Num() == 0)
        {
            // Not a resolvable /Game package path -> treat it as a bare name via the plugin's
            // canonical resolver (which surfaces ambiguity instead of silently picking one).
            TArray<FAssetData> Found;
            FString ResolveError;
            if (!mcp_common::ResolveAssetFlexible(AssetPath, nullptr, TEXT("/Game"), Found, ResolveError))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Asset not found: '%s' (%s). Pass an object path, a package path, or an unambiguous asset name."),
                    *AssetPath, *ResolveError));
            }
            if (Found.Num() > 1)
            {
                // Different assets have different referencers -- picking one arbitrarily would
                // hand back a confidently wrong answer. Make the caller disambiguate.
                FString Candidates;
                for (int32 i = 0; i < Found.Num(); ++i)
                {
                    Candidates += (i ? TEXT(", ") : TEXT("")) + Found[i].PackageName.ToString();
                }
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Ambiguous asset_path '%s': %d matches (%s). Pass a full path to disambiguate."),
                    *AssetPath, Found.Num(), *Candidates));
            }
            PackageNameStr = Found[0].PackageName.ToString();
        }
    }
    const FName PackageName(*PackageNameStr);
    // GetReferencers takes (FName, TArray<FName>&, EDependencyCategory, FDependencyQuery).
    // Hard-only by default (EDependencyQuery::Hard). When soft refs are requested we drop the flag
    // (default-constructed FDependencyQuery) so the query returns both hard and soft referencers.
    UE::AssetRegistry::FDependencyQuery Query = bIncludeSoftRefs
        ? UE::AssetRegistry::FDependencyQuery()
        : UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);

    TArray<FName> Referencers;
    AR.GetReferencers(PackageName, Referencers,
        UE::AssetRegistry::EDependencyCategory::Package,
        Query);

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FName& Ref : Referencers)
    {
        if (Entries.Num() >= MaxResults)
        {
            break;
        }
        // EDependencyCategory::Package makes GetReferencers yield PACKAGE names
        // ("/Game/Foo/BP_Enemy"), never object paths, so the lookup is GetAssetsByPackageName
        // (GetAssetByObjectPath finds nothing for a package name, and every entry would degrade
        // to a bare package_name).
        TArray<FAssetData> RefAssets;
        AR.GetAssetsByPackageName(Ref, RefAssets, /*bIncludeOnlyOnDiskAssets*/ false);

        // A package can hold several objects; prefer the primary asset (the one named after
        // the package, i.e. the UAsset) and fall back to the first entry.
        const FAssetData* Primary = nullptr;
        const FString ShortName = FPackageName::GetShortName(Ref.ToString());
        for (const FAssetData& Candidate : RefAssets)
        {
            if (Candidate.AssetName.ToString() == ShortName) { Primary = &Candidate; break; }
        }
        if (!Primary && RefAssets.Num() > 0) { Primary = &RefAssets[0]; }

        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        // Always emitted: the referencing PACKAGE is the stable identity of the edge, and it is
        // the only thing available for packages the registry holds no asset for (e.g. a World
        // Partition __ExternalActors__ package).
        E->SetStringField(TEXT("package_name"), Ref.ToString());
        if (Primary)
        {
            E->SetStringField(TEXT("asset_name"), Primary->AssetName.ToString());
            E->SetStringField(TEXT("asset_path"), Primary->GetSoftObjectPath().ToString());
            E->SetStringField(TEXT("asset_class"), Primary->AssetClassPath.ToString());
        }
        Entries.Add(MakeShared<FJsonValueObject>(E));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    // What asset_path actually resolved to. Lets a caller confirm the query hit the intended
    // package instead of trusting a bare count -- a wrong package also answers total:0.
    Data->SetStringField(TEXT("resolved_package"), PackageNameStr);
    Data->SetBoolField(TEXT("include_soft_references"), bIncludeSoftRefs);
    Data->SetNumberField(TEXT("total"), Referencers.Num());
    Data->SetNumberField(TEXT("returned"), Entries.Num());
    Data->SetBoolField(TEXT("results_truncated"), Referencers.Num() > Entries.Num());
    Data->SetArrayField(TEXT("referencers"), Entries);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// diff_blueprints / diff_blueprint_graphs (structural Blueprint diff)
// ---------------------------------------------------------------------------

// MCP:diff_blueprints - Structural diff of TWO Blueprints (or AnimBlueprints): parent class, SCS components, variables, functions, and per graph a node-by-node diff of what was added, removed and changed, where a changed node reports its title, per-pin default-value and link changes and -- for AnimGraph nodes -- the internal FAnimNode config and property-binding differences. WARNING: nodes are matched by NodeGuid, which duplicating or forking an asset PRESERVES: this is built to compare a fork against its original, and two independently authored Blueprints will show every single node as added/removed, which is noise, not a finding. Read-only. Siblings: diff_blueprint_graphs for ONE named graph, diff_blueprint_components for components only, get_blueprint_component_overrides for per-property provenance, compare_objects for two LIVE UObjects instead of authored structure. Watch `graphs_not_examined[]` -- when the node budget runs out the remaining graphs are listed there instead of being reported as identical.
//   @param blueprint_a - First Blueprint asset path or short name.
//   @param blueprint_b - Second Blueprint asset path or short name.
//   @param max_nodes - TOTAL cap on nodes examined across every graph (default 20000). When it runs out, the remaining graphs are listed in graphs_not_examined[] instead of being silently reported as identical.
//   @param max_nodes_per_graph - Per-graph cap so one fat graph cannot starve the rest (default = max_nodes).
//   @returns { blueprint_a, blueprint_b, a_path, b_path, a_class, b_class, parent_class:{a,b,changed}, components:{added,removed,changed}, variables:{added,removed,changed}, functions:{added,removed,common}, graphs:[{graph_name, nodes_added, nodes_removed, nodes_changed, node_count_a, node_count_b, nodes_examined_a, nodes_examined_b, truncated}], graphs_identical:[string], graphs_not_examined:[{graph_name, reason}], graphs_only_in_a, graphs_only_in_b, truncated, max_nodes, max_nodes_per_graph, nodes_examined, total_differences }
TSharedPtr<FJsonObject> HandleDiffBlueprints(const TSharedPtr<FJsonObject>& Params)
{
    FString NameA;
    FString NameB;
    int32 MaxNodes = 0;
    UBlueprint* BPA = nullptr;
    UBlueprint* BPB = nullptr;
    if (TSharedPtr<FJsonObject> Failure = LoadDiffPair_Analysis(Params, NameA, NameB, MaxNodes, BPA, BPB))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_a"), NameA);
    Result->SetStringField(TEXT("blueprint_b"), NameB);
    Result->SetStringField(TEXT("a_path"), BPA->GetPathName());
    Result->SetStringField(TEXT("b_path"), BPB->GetPathName());
    Result->SetStringField(TEXT("a_class"), BPA->GetClass()->GetName());
    Result->SetStringField(TEXT("b_class"), BPB->GetClass()->GetName());

    int32 TotalDiffs = 0;

    // --- Parent class ---
    {
        const FString PA = BPA->ParentClass ? BPA->ParentClass->GetPathName() : FString();
        const FString PB = BPB->ParentClass ? BPB->ParentClass->GetPathName() : FString();
        TSharedPtr<FJsonObject> Parent = MakeShared<FJsonObject>();
        Parent->SetStringField(TEXT("a"), PA);
        Parent->SetStringField(TEXT("b"), PB);
        Parent->SetBoolField(TEXT("changed"), PA != PB);
        Result->SetObjectField(TEXT("parent_class"), Parent);
        if (PA != PB) { ++TotalDiffs; }
    }

    // --- SCS components (by variable name; class change reported) ---
    {
        TMap<FString, FString> CompA, CompB;
        if (BPA->SimpleConstructionScript)
        {
            for (USCS_Node* N : BPA->SimpleConstructionScript->GetAllNodes())
            {
                if (N && N->ComponentTemplate) { CompA.Add(N->GetVariableName().ToString(), N->ComponentTemplate->GetClass()->GetName()); }
            }
        }
        if (BPB->SimpleConstructionScript)
        {
            for (USCS_Node* N : BPB->SimpleConstructionScript->GetAllNodes())
            {
                if (N && N->ComponentTemplate) { CompB.Add(N->GetVariableName().ToString(), N->ComponentTemplate->GetClass()->GetName()); }
            }
        }
        Result->SetObjectField(TEXT("components"),
            DiffNamedValues_Analysis(CompA, CompB, TEXT("class"), TEXT("a_class"), TEXT("b_class"), TotalDiffs));
    }

    // --- Variables (by name; type change reported) ---
    {
        auto VarType = [](const FBPVariableDescription& V) -> FString
        {
            FString T = V.VarType.PinCategory.ToString();
            if (V.VarType.PinSubCategoryObject.IsValid()) { T += TEXT(":") + V.VarType.PinSubCategoryObject->GetName(); }
            if (V.VarType.ContainerType == EPinContainerType::Array) { T += TEXT("[]"); }
            else if (V.VarType.ContainerType == EPinContainerType::Set) { T += TEXT("<Set>"); }
            else if (V.VarType.ContainerType == EPinContainerType::Map) { T += TEXT("<Map>"); }
            return T;
        };
        TMap<FString, FString> VarA, VarB;
        for (const FBPVariableDescription& V : BPA->NewVariables) { VarA.Add(V.VarName.ToString(), VarType(V)); }
        for (const FBPVariableDescription& V : BPB->NewVariables) { VarB.Add(V.VarName.ToString(), VarType(V)); }
        Result->SetObjectField(TEXT("variables"),
            DiffNamedValues_Analysis(VarA, VarB, TEXT("type"), TEXT("a_type"), TEXT("b_type"), TotalDiffs));
    }

    // --- Gather all graphs (function/event/macro/delegate + AnimGraph & children) grouped by name. ---
    TArray<UEdGraph*> GraphsA, GraphsB;
    BPA->GetAllGraphs(GraphsA);
    BPB->GetAllGraphs(GraphsB);
    TMap<FString, TArray<UEdGraph*>> ByNameA, ByNameB;
    for (UEdGraph* G : GraphsA) { if (G) { ByNameA.FindOrAdd(G->GetName()).Add(G); } }
    for (UEdGraph* G : GraphsB) { if (G) { ByNameB.FindOrAdd(G->GetName()).Add(G); } }

    // --- Functions summary (graph names present per side). ---
    {
        TArray<TSharedPtr<FJsonValue>> Added, Removed, Common;
        for (const TPair<FString, TArray<UEdGraph*>>& P : ByNameA)
        {
            if (ByNameB.Contains(P.Key)) { Common.Add(MakeShared<FJsonValueString>(P.Key)); }
            else { Removed.Add(MakeShared<FJsonValueString>(P.Key)); }
        }
        for (const TPair<FString, TArray<UEdGraph*>>& P : ByNameB)
        {
            if (!ByNameA.Contains(P.Key)) { Added.Add(MakeShared<FJsonValueString>(P.Key)); }
        }
        TSharedPtr<FJsonObject> Funcs = MakeShared<FJsonObject>();
        Funcs->SetArrayField(TEXT("added"), Added);
        Funcs->SetArrayField(TEXT("removed"), Removed);
        Funcs->SetArrayField(TEXT("common"), Common);
        Result->SetObjectField(TEXT("functions"), Funcs);
        TotalDiffs += Added.Num() + Removed.Num();
    }

    // --- Per-graph node diff (GUID-matched). ---
    // Each graph gets its OWN budget (max_nodes_per_graph), so one fat graph cannot starve the ones
    // after it; the TOTAL cap only decides how many graphs get examined at all, and a graph that was
    // never looked at lands in graphs_not_examined[] instead of vanishing. A graph examined with ZERO
    // differences is named in graphs_identical[], so "identical" and "never examined" never serialize
    // the same way.
    int32 MaxNodesPerGraph = MaxNodes;
    Params->TryGetNumberField(TEXT("max_nodes_per_graph"), MaxNodesPerGraph);
    if (MaxNodesPerGraph < 1) { MaxNodesPerGraph = MaxNodes; }

    int32 TotalNodeBudget = MaxNodes;
    bool bTruncated = false;
    int32 TotalNodesExamined = 0;
    TArray<TSharedPtr<FJsonValue>> GraphDiffs;
    TArray<TSharedPtr<FJsonValue>> GraphsOnlyA, GraphsOnlyB;
    TArray<TSharedPtr<FJsonValue>> GraphsIdentical, GraphsNotExamined;
    for (const TPair<FString, TArray<UEdGraph*>>& P : ByNameA)
    {
        const TArray<UEdGraph*>* BSide = ByNameB.Find(P.Key);
        if (!BSide)
        {
            GraphsOnlyA.Add(MakeShared<FJsonValueString>(P.Key));
            continue;
        }
        const int32 PairCount = FMath::Min(P.Value.Num(), BSide->Num());
        for (int32 i = 0; i < PairCount; ++i)
        {
            if (TotalNodeBudget <= 0)
            {
                bTruncated = true;
                TSharedPtr<FJsonObject> NE = MakeShared<FJsonObject>();
                NE->SetStringField(TEXT("graph_name"), P.Key);
                NE->SetStringField(TEXT("reason"), TEXT("total node budget (max_nodes) exhausted before this graph"));
                GraphsNotExamined.Add(MakeShared<FJsonValueObject>(NE));
                continue;
            }

            // Per-graph budget, capped by whatever is left of the total.
            int32 GraphBudget = FMath::Min(MaxNodesPerGraph, TotalNodeBudget);
            const int32 GraphBudgetStart = GraphBudget;
            bool bGraphTruncated = false;
            FGraphPairDiffStats_Analysis Stats;
            TArray<TSharedPtr<FJsonValue>> NAdded, NRemoved, NChanged;
            DiffGraphPair(P.Value[i], (*BSide)[i], NAdded, NRemoved, NChanged, GraphBudget, bGraphTruncated, &Stats);
            const int32 Spent = GraphBudgetStart - GraphBudget;
            TotalNodeBudget -= Spent;
            TotalNodesExamined += Spent;
            bTruncated = bTruncated || bGraphTruncated;

            const int32 GraphDiffCount = NAdded.Num() + NRemoved.Num() + NChanged.Num();
            if (GraphDiffCount > 0 || bGraphTruncated)
            {
                TSharedPtr<FJsonObject> G = MakeShared<FJsonObject>();
                G->SetStringField(TEXT("graph_name"), P.Key);
                G->SetArrayField(TEXT("nodes_added"), NAdded);
                G->SetArrayField(TEXT("nodes_removed"), NRemoved);
                G->SetArrayField(TEXT("nodes_changed"), NChanged);
                G->SetNumberField(TEXT("node_count_a"), Stats.NodesA);
                G->SetNumberField(TEXT("node_count_b"), Stats.NodesB);
                G->SetNumberField(TEXT("nodes_examined_a"), Stats.ExaminedA);
                G->SetNumberField(TEXT("nodes_examined_b"), Stats.ExaminedB);
                G->SetBoolField(TEXT("truncated"), bGraphTruncated);
                GraphDiffs.Add(MakeShared<FJsonValueObject>(G));
                TotalDiffs += GraphDiffCount;
            }
            else
            {
                // Fully examined, zero differences -- say so explicitly.
                GraphsIdentical.Add(MakeShared<FJsonValueString>(P.Key));
            }
        }
    }
    for (const TPair<FString, TArray<UEdGraph*>>& P : ByNameB)
    {
        if (!ByNameA.Contains(P.Key)) { GraphsOnlyB.Add(MakeShared<FJsonValueString>(P.Key)); }
    }

    Result->SetArrayField(TEXT("graphs"), GraphDiffs);
    Result->SetArrayField(TEXT("graphs_identical"), GraphsIdentical);
    Result->SetArrayField(TEXT("graphs_not_examined"), GraphsNotExamined);
    Result->SetArrayField(TEXT("graphs_only_in_a"), GraphsOnlyA);
    Result->SetArrayField(TEXT("graphs_only_in_b"), GraphsOnlyB);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetNumberField(TEXT("max_nodes"), MaxNodes);
    Result->SetNumberField(TEXT("max_nodes_per_graph"), MaxNodesPerGraph);
    Result->SetNumberField(TEXT("nodes_examined"), TotalNodesExamined);
    Result->SetNumberField(TEXT("total_differences"), TotalDiffs);
    return Result;
}

// MCP:diff_blueprint_graphs - Diff ONE named graph across two Blueprints or AnimBlueprints: which nodes were added, removed and changed, with per-pin default-value and link changes and, for AnimGraph nodes, the FAnimNode config and property-binding differences. The focused, far cheaper subset of diff_blueprints for when you already know the single graph you care about (an "AnimGraph", a function, a collapsed composite). Same NodeGuid matching caveat: it compares a fork against its original meaningfully, but two independently authored Blueprints show every node as added/removed. Read-only.
//   @param blueprint_a - First Blueprint asset path or short name.
//   @param blueprint_b - Second Blueprint asset path or short name.
//   @param graph_name - Graph to diff (deep-resolved: function/event/macro/AnimGraph or a collapsed composite by title). Defaults to the event graph.
//   @param max_nodes - Cap on GUID-matched nodes examined (default 20000).
//   @returns { blueprint_a, blueprint_b, graph_name, resolved_graph_a, resolved_graph_b, node_count_a, node_count_b, nodes_added, nodes_removed, nodes_changed, truncated, nodes_examined_a, nodes_examined_b, total_differences }
TSharedPtr<FJsonObject> HandleDiffBlueprintGraphs(const TSharedPtr<FJsonObject>& Params)
{
    FString NameA;
    FString NameB;
    int32 MaxNodes = 0;
    UBlueprint* BPA = nullptr;
    UBlueprint* BPB = nullptr;
    if (TSharedPtr<FJsonObject> Failure = LoadDiffPair_Analysis(Params, NameA, NameB, MaxNodes, BPA, BPB))
    {
        return Failure;
    }

    FString GraphName;
    Params->TryGetStringField(TEXT("graph_name"), GraphName);

    UEdGraph* GraphA = FUEAgentGraphUtils::FindGraphByNameDeep(BPA, GraphName);
    if (!GraphA) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Graph '%s' not found in %s"), *GraphName, *NameA)); }
    UEdGraph* GraphB = FUEAgentGraphUtils::FindGraphByNameDeep(BPB, GraphName);
    if (!GraphB) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Graph '%s' not found in %s"), *GraphName, *NameB)); }

    int32 NodeBudget = MaxNodes;
    bool bTruncated = false;
    FGraphPairDiffStats_Analysis Stats;
    TArray<TSharedPtr<FJsonValue>> NAdded, NRemoved, NChanged;
    DiffGraphPair(GraphA, GraphB, NAdded, NRemoved, NChanged, NodeBudget, bTruncated, &Stats);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_a"), NameA);
    Result->SetStringField(TEXT("blueprint_b"), NameB);
    Result->SetStringField(TEXT("graph_name"), GraphName);
    Result->SetStringField(TEXT("resolved_graph_a"), GraphA->GetName());
    Result->SetStringField(TEXT("resolved_graph_b"), GraphB->GetName());
    Result->SetNumberField(TEXT("node_count_a"), GraphA->Nodes.Num());
    Result->SetNumberField(TEXT("node_count_b"), GraphB->Nodes.Num());
    Result->SetArrayField(TEXT("nodes_added"), NAdded);
    Result->SetArrayField(TEXT("nodes_removed"), NRemoved);
    Result->SetArrayField(TEXT("nodes_changed"), NChanged);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    // Examined counters so a budget-truncated run can never read as "no differences".
    Result->SetNumberField(TEXT("nodes_examined_a"), Stats.ExaminedA);
    Result->SetNumberField(TEXT("nodes_examined_b"), Stats.ExaminedB);
    Result->SetNumberField(TEXT("total_differences"), NAdded.Num() + NRemoved.Num() + NChanged.Num());
    return Result;
}

// ---------------------------------------------------------------------------
// find_erroring_blueprints
// ---------------------------------------------------------------------------

// MCP:find_erroring_blueprints - Scan a content path for Blueprints and AnimBlueprints sitting in a compile ERROR state and report each one with its status, error/warning counts and its FIRST error message -- the search for the culprit when PIE refuses to start because of an asset nobody was working on. It reads the loaded compile Status and recompiles the erroring ones with a results log to extract that message. WARNING: loading every Blueprint under a broad path is heavy enough to stall the editor on thousands of assets: scope `path` narrowly, and reach for only_loaded=true first -- the Blueprint that aborts PIE is usually still in memory, so that fast in-memory pass normally finds it without a single disk load. Complements validate_assets_in_path and audit_blueprint_health by listing EVERY erroring Blueprint with its first error in one call. WARNING: the scope parameter is named `path`, NOT search_path: pass the wrong name and it is silently ignored, the default /Game applies, and the tool sweeps the whole project while the answer looks scoped to your folder.
//   @param path - Content path to scan, recursive (default "/Game"). WARNING: this exact name -- a `search_path` key is ignored and you get the full /Game sweep. Scope it narrowly to stay cheap.
//   @param only_loaded - When true, only inspect Blueprints already loaded in memory (no disk load). Default false.
//   @param include_warnings - Also report Blueprints with compile WARNINGS (not just errors). Default false.
//   @param compile_all - Recompile EVERY scanned Blueprint (thorough but heavy); otherwise only recompile ones whose loaded Status is already Error. Default false.
//   @param max_scan - Max Blueprints to load/inspect (default 5000).
//   @param max_results - Max erroring/warning Blueprints to return (default 500).
//   @returns { path, scanned, inspected, only_loaded, erroring_count, warning_count, results_truncated, blueprints:[{ asset_path, name, class, status, num_errors, num_warnings, first_error }] }
TSharedPtr<FJsonObject> HandleFindErroringBlueprints(const TSharedPtr<FJsonObject>& Params)
{
    FString Path = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), Path);
    if (Path.IsEmpty()) { Path = TEXT("/Game"); }

    bool bOnlyLoaded = false;
    Params->TryGetBoolField(TEXT("only_loaded"), bOnlyLoaded);
    bool bIncludeWarnings = false;
    Params->TryGetBoolField(TEXT("include_warnings"), bIncludeWarnings);
    bool bCompileAll = false;
    Params->TryGetBoolField(TEXT("compile_all"), bCompileAll);

    int32 MaxScan = 5000;
    Params->TryGetNumberField(TEXT("max_scan"), MaxScan);
    if (MaxScan < 1) { MaxScan = 5000; }
    int32 MaxResults = 500;
    Params->TryGetNumberField(TEXT("max_results"), MaxResults);
    if (MaxResults < 1) { MaxResults = 500; }

    IAssetRegistry& AR = mcp_common::AssetRegistry();

    // Enumerate Blueprint-derived assets (recursive classes -> AnimBlueprint/WidgetBlueprint too).
    FARFilter Filter;
    Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    Filter.bRecursiveClasses = true;
    Filter.bRecursivePaths = true;
    Filter.PackagePaths.Add(FName(*Path));
    TArray<FAssetData> Assets;
    AR.GetAssets(Filter, Assets);

    // Helper: extract the first error message from a compile results log.
    auto FirstError = [](const FCompilerResultsLog& Log) -> FString
    {
        for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
        {
            if (Msg->GetSeverity() == EMessageSeverity::Error)
            {
                return Msg->ToText().ToString();
            }
        }
        return FString();
    };

    TArray<TSharedPtr<FJsonValue>> Out;
    int32 Inspected = 0;
    int32 Scanned = 0;
    int32 ErroringCount = 0;
    int32 WarningCount = 0;
    bool bResultsTruncated = false;

    for (const FAssetData& AD : Assets)
    {
        if (Scanned >= MaxScan) { break; }
        ++Scanned;

        UObject* Asset = bOnlyLoaded ? AD.FastGetAsset(false) : AD.GetAsset();
        UBlueprint* BP = Cast<UBlueprint>(Asset);
        if (!BP) { continue; }   // only_loaded: not in memory, skip
        ++Inspected;

        int32 NumErrors = 0;
        int32 NumWarnings = 0;
        FString FirstErr;
        EBlueprintStatus Status = BP->Status;

        const bool bStatusError = (Status == BS_Error);
        if (bCompileAll || bStatusError)
        {
            // Recompile to resolve real status + capture the first error message.
            FCompilerResultsLog Results;
            FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Results);
            NumErrors = Results.NumErrors;
            NumWarnings = Results.NumWarnings;
            Status = BP->Status;
            if (NumErrors > 0) { FirstErr = FirstError(Results); }
        }

        const bool bHasError = (Status == BS_Error) || (NumErrors > 0);
        const bool bHasWarning = (Status == BS_UpToDateWithWarnings) || (NumWarnings > 0);

        if (bHasError || (bIncludeWarnings && bHasWarning))
        {
            if (bHasError) { ++ErroringCount; } else { ++WarningCount; }
            if (Out.Num() >= MaxResults)
            {
                bResultsTruncated = true;
                continue;
            }
            const TCHAR* StatusStr = BlueprintStatusToString_Analysis(Status);
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("asset_path"), BP->GetPathName());
            E->SetStringField(TEXT("name"), BP->GetName());
            E->SetStringField(TEXT("class"), BP->GetClass()->GetName());
            E->SetStringField(TEXT("status"), StatusStr);
            E->SetNumberField(TEXT("num_errors"), NumErrors);
            E->SetNumberField(TEXT("num_warnings"), NumWarnings);
            E->SetStringField(TEXT("first_error"), FirstErr);
            Out.Add(MakeShared<FJsonValueObject>(E));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetNumberField(TEXT("scanned"), Scanned);
    Result->SetNumberField(TEXT("inspected"), Inspected);
    Result->SetBoolField(TEXT("only_loaded"), bOnlyLoaded);
    Result->SetNumberField(TEXT("erroring_count"), ErroringCount);
    Result->SetNumberField(TEXT("warning_count"), WarningCount);
    Result->SetBoolField(TEXT("results_truncated"), bResultsTruncated || Scanned >= MaxScan);
    Result->SetArrayField(TEXT("blueprints"), Out);
    return Result;
}

// ---------------------------------------------------------------------------
// snapshot_object / diff_object_snapshots
// ---------------------------------------------------------------------------

// MCP:snapshot_object - Capture an object's full property and component state to a named handle so it can be diffed LATER with diff_object_snapshots. compare_objects diffs two live objects at one instant; this one stores a state, which is what lets you compare ACROSS TIME (before/after an action, the honest oracle for "did that call actually change anything?") or ACROSS WORLDS (the editor CDO/defaults against the running PIE instance, or the server instance against the client's -- the co-op check that a server-side reading can never make alone). Accepts `world` including 'server' / 'client' / 'pie:N', and the response stamps which world it read. persist=true writes the snapshot under Saved/MCPAudits/snapshots/ so a baseline SURVIVES an editor restart -- without it the store is a process-lifetime map and the baseline vanishes exactly when a mandatory restart makes you need it. return_snapshot=true hands the snapshot back inline to pass straight into diff_object_snapshots.
//   @param object_path - Object path or dotted "Actor[.Component[.Subobject]]" (name or editor label for the first segment).
//   @param world - Optional world selector: absent/'editor' (default) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'pie'/'game'/'runtime' (ambiguous: PlayWorld, else the first PIE/Game context) to resolve a live runtime instance.
//   @param handle - Optional handle to store under (overwrites); auto-generated when omitted (alias: snapshot_id).
//   @param persist - Also write the snapshot to Saved/MCPAudits/snapshots/<handle>.json so it SURVIVES an editor restart. The in-memory store is a process-lifetime map: without this, a baseline captured before a mandatory restart is gone exactly when you need to compare against it. Default false.
//   @param return_snapshot - Return the full snapshot inline in the response (so the caller can hold it and pass it back as diff_object_snapshots' snapshot_a/snapshot_b). Default false.
//   @param include_component_templates - For a Blueprint / BPGC / Blueprint CDO target, also capture the SCS + ICH component TEMPLATES (a Blueprint CDO owns ONLY its native components, so without this a CDO snapshot cannot verify a single SCS/ICH override). Default true.
//   @returns { handle, object_path, resolved_path, class, world (the coarse "pie"/"editor" family label the snapshot itself carries), world_selector (what was asked for), property_count, component_count, component_template_count, persisted, snapshot_path?, persist_error?, snapshot?, world_name, world_package, world_type, net_mode, pie_instance }
TSharedPtr<FJsonObject> HandleSnapshotObject(const TSharedPtr<FJsonObject>& Params)
{
    FString ObjectPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath)) { return Missing; }
    FString WorldParam = TEXT("editor");
    Params->TryGetStringField(TEXT("world"), WorldParam);

    FString WorldError;
    UWorld* World = ResolveWorldForParam(WorldParam, WorldError);
    if (IsRuntimeWorldParam(WorldParam) && !World)
    {
        // The canonical error lists the worlds that DO exist -- "no PIE/Game world is
        // running" alone is true and useless.
        return mcp_common::MakeErrorReply(WorldError.IsEmpty()
            ? TEXT("world='pie'/'game' requested but no PIE/Game world is running")
            : WorldError);
    }

    UObject* Obj = ResolveObjectForWorld(ObjectPath, World);
    if (!Obj)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Object not found: %s (world=%s)"), *ObjectPath, *WorldParam));
    }

    bool bIncludeTemplates = true;
    Params->TryGetBoolField(TEXT("include_component_templates"), bIncludeTemplates);

    const FString WorldLabel = IsRuntimeWorldParam(WorldParam) ? TEXT("pie") : TEXT("editor");
    TSharedPtr<FJsonObject> Snapshot = CaptureObjectSnapshot(Obj, WorldLabel, bIncludeTemplates);

    FString Handle;
    if (!Params->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
    {
        Params->TryGetStringField(TEXT("snapshot_id"), Handle);
    }
    if (Handle.IsEmpty())
    {
        Handle = FString::Printf(TEXT("snap_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    }
    Snapshot->SetStringField(TEXT("handle"), Handle);
    SnapshotStore().Add(Handle, Snapshot);

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    int32 PropertyCount = 0;
    if (Snapshot->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj) { PropertyCount = (*PropsObj)->Values.Num(); }
    const TSharedPtr<FJsonObject>* CompsObj = nullptr;
    int32 ComponentCount = 0;
    if (Snapshot->TryGetObjectField(TEXT("components"), CompsObj) && CompsObj) { ComponentCount = (*CompsObj)->Values.Num(); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("handle"), Handle);
    Result->SetStringField(TEXT("object_path"), ObjectPath);
    Result->SetStringField(TEXT("resolved_path"), Obj->GetPathName());
    Result->SetStringField(TEXT("class"), Obj->GetClass()->GetPathName());
    Result->SetStringField(TEXT("world"), WorldLabel);
    // `world` stays the coarse "pie"/"editor" family label the SNAPSHOT is stamped with
    // (changing it would change stored snapshot content). `world_selector` is what the caller
    // actually asked for, and the five evidence fields are the world that was really read --
    // a snapshot taken on the client and diffed against one from the server is otherwise an
    // invisible mistake.
    Result->SetStringField(TEXT("world_selector"), WorldParam);
    mcp_common::AddWorldEvidence(Result, Obj->GetWorld() ? Obj->GetWorld() : World);
    Result->SetNumberField(TEXT("property_count"), PropertyCount);
    Result->SetNumberField(TEXT("component_count"), ComponentCount);
    double TemplateCountD = 0.0;
    Snapshot->TryGetNumberField(TEXT("component_template_count"), TemplateCountD);
    Result->SetNumberField(TEXT("component_template_count"), TemplateCountD);

    // Optional durable copy. The in-memory SnapshotStore() dies with the process, and some
    // workflows REQUIRE an editor restart (e.g. to read inherited defaults) -- i.e. the baseline
    // evaporates exactly when it is needed. `persisted` is the RE-STATTED file, not the return of
    // a write call.
    bool bPersist = false;
    Params->TryGetBoolField(TEXT("persist"), bPersist);
    bool bPersisted = false;
    FString PersistError;
    if (bPersist)
    {
        FString OutPath;
        bPersisted = PersistSnapshot_Analysis(Handle, Snapshot, OutPath, PersistError);
        if (bPersisted) { Result->SetStringField(TEXT("snapshot_path"), OutPath); }
        else { Result->SetStringField(TEXT("persist_error"), PersistError); }
    }
    Result->SetBoolField(TEXT("persisted"), bPersisted);

    bool bReturnSnapshot = false;
    Params->TryGetBoolField(TEXT("return_snapshot"), bReturnSnapshot);
    if (bReturnSnapshot) { Result->SetObjectField(TEXT("snapshot"), Snapshot); }

    // The capture itself is always observable (it is a read); only the PERSIST leg can fail.
    // Evidence is written FLAT onto the existing payload (AddVerificationEvidence) instead of
    // re-enveloping under `data`, so no existing caller's field paths move.
    if (bPersist)
    {
        mcp_common::AddVerificationEvidence(Result,
            bPersisted ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
            TEXT("re-stat of Saved/MCPAudits/snapshots/<handle>.json after write"),
            bPersisted ? FString(TEXT("file exists and is non-empty")) : PersistError);
        Result->SetBoolField(TEXT("success"), bPersisted);
        if (!bPersisted) { Result->SetStringField(TEXT("error"), PersistError); }
    }
    else
    {
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotAttempted,
            TEXT("persist not requested -- snapshot lives only in the process-lifetime in-memory store"),
            TEXT("pass persist=true for a copy that survives an editor restart"));
        Result->SetBoolField(TEXT("success"), true);
    }
    return Result;
}

// MCP:diff_object_snapshots - Compare two snapshots taken by snapshot_object and report which properties were added, removed or CHANGED, at the object level and per component, with both values side by side. The second half of the before/after measurement: capture a baseline, run the action, capture again, and this tells you what actually moved -- instead of trusting the acting call's own success flag. Also the way to diff editor-vs-PIE and, in co-op, SERVER-vs-CLIENT state for the same actor, which is the only proof that a value really replicated. Sides can be given as stored handles, as inline snapshot objects (from snapshot_object(return_snapshot=true)), or as paths to persisted snapshot files; `a_origin`/`b_origin` report which route each side came from. compare_objects is the same idea for two LIVE objects at one instant.
//   @param handle_a - Handle of the first (baseline) snapshot (aliases: a, snapshot_a). Resolved from the in-memory store first, then from the persisted Saved/MCPAudits/snapshots/<handle>.json copy.
//   @param handle_b - Handle of the second snapshot (aliases: b, snapshot_b). Same resolution.
//   @param snapshot_a - The baseline snapshot INLINE (a JSON object, e.g. what snapshot_object(return_snapshot=true) handed back). A STRING here is read as a handle.
//   @param snapshot_b - The second snapshot INLINE (a JSON object). A STRING here is read as a handle.
//   @param path_a - Path to a persisted baseline snapshot file (bare handles resolve under Saved/MCPAudits/snapshots/).
//   @param path_b - Path to a persisted second snapshot file.
//   @returns { handle_a, handle_b, a_origin, b_origin, a_object_path, b_object_path, a_class, b_class, a_world, b_world, properties:{added,removed,changed}, components:{added,removed,changed:[{component, properties:{added,removed,changed}}]}, total_differences }
TSharedPtr<FJsonObject> HandleDiffObjectSnapshots(const TSharedPtr<FJsonObject>& Params)
{
    FString HandleA, HandleB, OriginA, OriginB, ErrA, ErrB;
    TSharedPtr<FJsonObject> SnapA = ResolveSnapshotSide_Analysis(
        Params, TEXT("handle_a"), TEXT("a"), TEXT("snapshot_a"), TEXT("path_a"), HandleA, OriginA, ErrA);
    if (!SnapA.IsValid()) { return mcp_common::MakeErrorReply(ErrA); }
    TSharedPtr<FJsonObject> SnapB = ResolveSnapshotSide_Analysis(
        Params, TEXT("handle_b"), TEXT("b"), TEXT("snapshot_b"), TEXT("path_b"), HandleB, OriginB, ErrB);
    if (!SnapB.IsValid()) { return mcp_common::MakeErrorReply(ErrB); }

    int32 TotalDiffs = 0;
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("handle_a"), HandleA);
    Result->SetStringField(TEXT("handle_b"), HandleB);
    Result->SetStringField(TEXT("a_origin"), OriginA);
    Result->SetStringField(TEXT("b_origin"), OriginB);
    // TryGet (not GetStringField) -- an inline/persisted snapshot may legitimately lack a key,
    // and GetStringField logs an error + returns an empty string for a missing field.
    auto SnapStr = [](const TSharedPtr<FJsonObject>& S, const TCHAR* Field) -> FString
    { FString V; if (S.IsValid()) { S->TryGetStringField(Field, V); } return V; };
    Result->SetStringField(TEXT("a_object_path"), SnapStr(SnapA, TEXT("object_path")));
    Result->SetStringField(TEXT("b_object_path"), SnapStr(SnapB, TEXT("object_path")));
    Result->SetStringField(TEXT("a_class"), SnapStr(SnapA, TEXT("class")));
    Result->SetStringField(TEXT("b_class"), SnapStr(SnapB, TEXT("class")));
    Result->SetStringField(TEXT("a_world"), SnapStr(SnapA, TEXT("world")));
    Result->SetStringField(TEXT("b_world"), SnapStr(SnapB, TEXT("world")));

    // Object-level properties.
    {
        const TSharedPtr<FJsonObject>* PA = nullptr; const TSharedPtr<FJsonObject>* PB = nullptr;
        SnapA->TryGetObjectField(TEXT("properties"), PA);
        SnapB->TryGetObjectField(TEXT("properties"), PB);
        TArray<TSharedPtr<FJsonValue>> Added, Removed, Changed;
        TotalDiffs += DiffJsonPropMaps(PA ? *PA : nullptr, PB ? *PB : nullptr, Added, Removed, Changed);
        Result->SetObjectField(TEXT("properties"), DiffSection_Analysis(Added, Removed, Changed));
    }

    // Component-level (matched by component name).
    {
        const TSharedPtr<FJsonObject>* CA = nullptr; const TSharedPtr<FJsonObject>* CB = nullptr;
        const bool bHasA = SnapA->TryGetObjectField(TEXT("components"), CA) && CA;
        const bool bHasB = SnapB->TryGetObjectField(TEXT("components"), CB) && CB;
        TArray<TSharedPtr<FJsonValue>> CompsAdded, CompsRemoved, CompsChanged;
        if (bHasA)
        {
            for (const auto& Pair : (*CA)->Values)
            {
                const TSharedPtr<FJsonObject>* CompBEntry = nullptr;
                if (!bHasB || !(*CB)->Values.Contains(Pair.Key))
                {
                    CompsRemoved.Add(MakeShared<FJsonValueString>(FString(*Pair.Key)));
                    ++TotalDiffs;
                    continue;
                }
                (*CB)->TryGetObjectField(Pair.Key, CompBEntry);
                const TSharedPtr<FJsonObject>* CompAEntry = nullptr;
                (*CA)->TryGetObjectField(Pair.Key, CompAEntry);
                const TSharedPtr<FJsonObject>* PA = nullptr; const TSharedPtr<FJsonObject>* PB = nullptr;
                if (CompAEntry) { (*CompAEntry)->TryGetObjectField(TEXT("properties"), PA); }
                if (CompBEntry) { (*CompBEntry)->TryGetObjectField(TEXT("properties"), PB); }
                TArray<TSharedPtr<FJsonValue>> Added, Removed, Changed;
                const int32 CompDiffs = DiffJsonPropMaps(PA ? *PA : nullptr, PB ? *PB : nullptr, Added, Removed, Changed);
                if (CompDiffs > 0)
                {
                    TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
                    C->SetStringField(TEXT("component"), Pair.Key);
                    C->SetObjectField(TEXT("properties"), DiffSection_Analysis(Added, Removed, Changed));
                    CompsChanged.Add(MakeShared<FJsonValueObject>(C));
                    TotalDiffs += CompDiffs;
                }
            }
        }
        if (bHasB)
        {
            for (const auto& Pair : (*CB)->Values)
            {
                if (!bHasA || !(*CA)->Values.Contains(Pair.Key))
                {
                    CompsAdded.Add(MakeShared<FJsonValueString>(FString(*Pair.Key)));
                    ++TotalDiffs;
                }
            }
        }
        Result->SetObjectField(TEXT("components"), DiffSection_Analysis(CompsAdded, CompsRemoved, CompsChanged));
    }

    Result->SetNumberField(TEXT("total_differences"), TotalDiffs);
    return Result;
}

// ---------------------------------------------------------------------------
// get_blueprint_exec_path
// ---------------------------------------------------------------------------

// MCP:get_blueprint_exec_path - Report which exec nodes and BRANCHES actually RAN inside a Blueprint at runtime -- which Branch went true, which FunctionResult fired -- read from the editor's Blueprint-debugger trace stack. Unlike trace_blueprint_execution and profile_blueprint_execution, which are STATIC and list every node that exists, this reflects real execution, so it answers "did that path run at all?" when a graph fails silently. WARNING: two-phase by construction: the VM only records the trace for the object currently being DEBUGGED, so call it ONCE with object_path to arm recording (SetObjectBeingDebugged), then drive the PIE scenario, then call it AGAIN to read the recorded path -- a single call returns nothing and that is not a bug. Requires a running PIE session with Blueprint debugging; `world` (including 'server' / 'client' / 'pie:N') selects which instance gets armed, and the response stamps which world it was.
//   @param blueprint_name - Blueprint / AnimBlueprint asset path or short name to filter the trace to.
//   @param function_name - Optional: only include samples in a function/graph whose name contains this.
//   @param object_path - Optional live PIE object to set as the debugged instance (arms trace recording).
//   @param set_debug_object - When object_path is given, call SetObjectBeingDebugged on it. Default true.
//   @param world - World to resolve object_path in: absent/'editor' (default) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'pie'/'game'/'runtime' (ambiguous). A fully-qualified PIE object path (...UEDPIE...) resolves regardless of this.
//   @param max_samples - Max trace-stack samples to scan (default 500).
//   @returns { blueprint_name, blueprint_path, debug_object_set, object_path, trace_stack_size, filtered_count, path:[{index, node_id, node_class, node_title, graph, function, context, is_branch, is_function_result, observation_time}], note, world_selector, world_name, world_package, world_type, net_mode, pie_instance (the world of the ARMED debug object; nulls when none was armed) }
TSharedPtr<FJsonObject> HandleGetBlueprintExecPath(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    FString FunctionFilter;
    Params->TryGetStringField(TEXT("function_name"), FunctionFilter);
    FString ObjectPath;
    Params->TryGetStringField(TEXT("object_path"), ObjectPath);
    bool bSetDebugObject = true;
    Params->TryGetBoolField(TEXT("set_debug_object"), bSetDebugObject);
    FString WorldParam = TEXT("editor");
    Params->TryGetStringField(TEXT("world"), WorldParam);
    int32 MaxSamples = 500;
    Params->TryGetNumberField(TEXT("max_samples"), MaxSamples);
    if (MaxSamples < 1) { MaxSamples = 500; }

    UBlueprint* Blueprint = LoadBlueprintFlexible(BlueprintName);
    if (!Blueprint) { return mcp_common::BlueprintNotFoundReply(BlueprintName); }

    // Phase 1 (optional): arm trace recording by setting the debugged object.
    bool bDebugObjectSet = false;
    UWorld* DebugWorld = nullptr;   // The world the debug object was resolved in (evidence)
    if (!ObjectPath.IsEmpty() && bSetDebugObject)
    {
        UWorld* World = ResolveWorldForParam(WorldParam);
        if (UObject* Obj = ResolveObjectForWorld(ObjectPath, World))
        {
            Blueprint->SetObjectBeingDebugged(Obj);
            bDebugObjectSet = true;
            DebugWorld = Obj->GetWorld() ? Obj->GetWorld() : World;
        }
    }

    // Phase 2: read the trace stack (index 0 = most recent). Match samples to THIS Blueprint by the
    // node's owning Blueprint (authoritative) or the context object's generated class.
    const TSimpleRingBuffer<FKismetTraceSample>& TraceStack = FKismetDebugUtilities::GetTraceStack();
    const int32 StackSize = TraceStack.Num();

    UClass* GenClass = Blueprint->GeneratedClass;
    TArray<TSharedPtr<FJsonValue>> ReversePath; // most-recent-first while scanning
    const int32 Scan = FMath::Min(StackSize, MaxSamples);
    for (int32 i = 0; i < Scan; ++i)
    {
        const FKismetTraceSample& Sample = TraceStack(i);
        UObject* Context = Sample.Context.Get();
        UFunction* Function = Sample.Function.Get();
        if (!Context) { continue; }

        UEdGraphNode* Node = FKismetDebugUtilities::FindSourceNodeForCodeLocation(
            Context, Function, Sample.Offset, /*bAllowImpreciseHit=*/true);

        // Filter to this Blueprint.
        bool bBelongs = false;
        if (Node)
        {
            if (UBlueprint* OwningBP = FBlueprintEditorUtils::FindBlueprintForNode(Node))
            {
                bBelongs = (OwningBP == Blueprint);
            }
        }
        if (!bBelongs && GenClass && Context->GetClass() && Context->GetClass()->IsChildOf(GenClass))
        {
            bBelongs = true;
        }
        if (!bBelongs) { continue; }

        // Optional function/graph name filter.
        FString GraphName;
        if (Node && Node->GetGraph()) { GraphName = Node->GetGraph()->GetName(); }
        const FString FunctionName = Function ? Function->GetName() : FString();
        if (!FunctionFilter.IsEmpty()
            && !GraphName.Contains(FunctionFilter) && !FunctionName.Contains(FunctionFilter))
        {
            continue;
        }

        const FString NodeClass = Node ? Node->GetClass()->GetName() : FString();
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetNumberField(TEXT("index"), i);
        E->SetStringField(TEXT("node_id"), Node ? Node->NodeGuid.ToString() : FString());
        E->SetStringField(TEXT("node_class"), NodeClass);
        E->SetStringField(TEXT("node_title"), Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
        E->SetStringField(TEXT("graph"), GraphName);
        E->SetStringField(TEXT("function"), FunctionName);
        E->SetStringField(TEXT("context"), Context->GetName());
        E->SetBoolField(TEXT("is_branch"), NodeClass.Contains(TEXT("IfThenElse")) || NodeClass.Contains(TEXT("Branch")) || NodeClass.Contains(TEXT("Switch")));
        E->SetBoolField(TEXT("is_function_result"), NodeClass.Contains(TEXT("FunctionResult")) || NodeClass.Contains(TEXT("Return")));
        E->SetNumberField(TEXT("observation_time"), Sample.ObservationTime);
        ReversePath.Add(MakeShared<FJsonValueObject>(E));
    }

    // Present chronologically (oldest -> newest).
    Algo::Reverse(ReversePath);

    TSharedPtr<FJsonObject> Result = UEAgentAnalysisCommandsImpl::StartBlueprintReply(BlueprintName, Blueprint->GetPathName());
    Result->SetBoolField(TEXT("debug_object_set"), bDebugObjectSet);
    Result->SetStringField(TEXT("object_path"), ObjectPath);
    Result->SetNumberField(TEXT("trace_stack_size"), StackSize);
    Result->SetNumberField(TEXT("filtered_count"), ReversePath.Num());
    Result->SetArrayField(TEXT("path"), ReversePath);
    Result->SetStringField(TEXT("note"),
        bDebugObjectSet
            ? TEXT("Debugged object armed. Drive your PIE scenario now, then call again (without needing object_path) to READ the recorded exec path.")
            : (StackSize == 0
                ? TEXT("Trace stack is empty. Pass object_path (a live PIE instance) to arm SetObjectBeingDebugged, then drive the scenario and re-read. Requires PIE + Blueprint debugging.")
                : TEXT("Read from the current trace stack. Only the object being debugged records into it.")));
    // WHICH world the debugged instance lives in (null when no object was armed).
    // Arming the client's copy and then driving the server's is a silent dead end.
    Result->SetStringField(TEXT("world_selector"), WorldParam);
    mcp_common::AddWorldEvidence(Result, DebugWorld);
    return Result;
}

// MCP:diff_blueprint_components - Compare the COMPONENT trees of two Blueprints (or of one Blueprint against a components[] array captured earlier from get_blueprint_scs_tree) and report which components were added, removed or changed, where "changed" means the class, attach_parent, attach_socket, source or relative transform (0.01 tolerance) differ. It reuses get_blueprint_scs_tree's full enumeration, so native + own SCS + inherited components are all in scope, most-derived-wins. Use it for "did a reparent or a merge lose a component?" -- feeding a pre-change scs_tree as components_b makes it a before/after check without a second asset. removed = in A only, added = in B only. Read-only. diff_blueprints covers components AND graphs AND variables; get_blueprint_component_overrides goes deeper into ONE component's per-property provenance.
//   @param blueprint_a - First Blueprint asset path or short name (the "before" / baseline side).
//   @param blueprint_b - Second Blueprint (the "after" side). Omit if passing components_b.
//   @param components_b - Alternative to blueprint_b: a prior get_blueprint_scs_tree components array (snapshot). A snapshot without transforms reads every transform as changed.
//   @param include_native - Include native/CDO components on the live side(s) (default true).
//   @param include_inherited - Include components inherited from ancestor Blueprints (default true).
//   @param max_results - Cap on components enumerated per live side, 1..10000 (default 2000).
//   @returns { blueprint_a, a_path, blueprint_b, b_path, side_b_source, added:[record], removed:[record], changed:[{name, class, changes:{class?:{a,b}, attach_parent?:{a,b}, attach_socket?:{a,b}, source?:{a,b}, transform?:{a,b}}}], unchanged_count, counts_a:{native,scs,inherited}, counts_b:{...}, truncated, total_differences }
TSharedPtr<FJsonObject> HandleDiffBlueprintComponents(const TSharedPtr<FJsonObject>& Params)
{
    FString NameA;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_a"), NameA)) { return Missing; }

    bool bIncludeNative = true;    Params->TryGetBoolField(TEXT("include_native"), bIncludeNative);
    bool bIncludeInherited = true; Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
    int32 MaxResults = 2000;       Params->TryGetNumberField(TEXT("max_results"), MaxResults);
    if (MaxResults < 1 || MaxResults > 10000) { MaxResults = 2000; }

    UBlueprint* BPA = LoadBlueprintFlexible(NameA);
    if (!BPA) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blueprint A not found: %s"), *NameA)); }

    // Side A -- always a live walk.
    TArray<TSharedPtr<FJsonValue>> OrderedA;
    TMap<FName, TSharedPtr<FJsonObject>> ByNameA;
    int32 NatA = 0, ScsA = 0, InhA = 0; bool TrA = false;
    CollectBlueprintComponents(BPA, bIncludeNative, bIncludeInherited, MaxResults, OrderedA, ByNameA, NatA, ScsA, InhA, TrA);

    // Side B -- live BP (blueprint_b) OR a prior get_blueprint_scs_tree components[] snapshot (components_b).
    TMap<FName, TSharedPtr<FJsonObject>> ByNameB;
    int32 NatB = 0, ScsB = 0, InhB = 0; bool TrB = false;
    FString NameB, BPathB, SideBSource;
    const TArray<TSharedPtr<FJsonValue>>* SnapArr = nullptr;
    if (Params->TryGetStringField(TEXT("blueprint_b"), NameB) && !NameB.IsEmpty())
    {
        UBlueprint* BPB = LoadBlueprintFlexible(NameB);
        if (!BPB) { return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blueprint B not found: %s"), *NameB)); }
        TArray<TSharedPtr<FJsonValue>> OrderedB;
        CollectBlueprintComponents(BPB, bIncludeNative, bIncludeInherited, MaxResults, OrderedB, ByNameB, NatB, ScsB, InhB, TrB);
        BPathB = BPB->GetPathName();
        SideBSource = TEXT("blueprint");
    }
    else if (Params->TryGetArrayField(TEXT("components_b"), SnapArr) && SnapArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *SnapArr)
        {
            const TSharedPtr<FJsonObject>* OPtr = nullptr;
            if (!V.IsValid() || !V->TryGetObject(OPtr) || !OPtr) { continue; }
            const TSharedPtr<FJsonObject>& O = *OPtr;
            FString CName;
            if (!O->TryGetStringField(TEXT("name"), CName) || CName.IsEmpty()) { continue; }
            ByNameB.Add(FName(*CName), O);
            FString Src; O->TryGetStringField(TEXT("source"), Src);
            if (Src == TEXT("native")) { ++NatB; } else if (Src == TEXT("scs")) { ++ScsB; } else if (Src == TEXT("inherited")) { ++InhB; }
        }
        SideBSource = TEXT("snapshot");
    }
    else
    {
        return mcp_common::MakeErrorReply(TEXT("Provide either 'blueprint_b' (a Blueprint) or 'components_b' (a prior get_blueprint_scs_tree components array)"));
    }

    // --- Comparators (JSON-record-based so live and snapshot sides compare identically) ---
    auto GetStr = [](const TSharedPtr<FJsonObject>& O, const TCHAR* F) -> FString
    { FString S; if (O.IsValid()) { O->TryGetStringField(F, S); } return S; };
    auto GetXform = [](const TSharedPtr<FJsonObject>& O, const TCHAR* Axis) -> const TArray<TSharedPtr<FJsonValue>>*
    {
        const TSharedPtr<FJsonObject>* T = nullptr;
        if (!O.IsValid() || !O->TryGetObjectField(TEXT("transform"), T) || !T) { return nullptr; }
        const TArray<TSharedPtr<FJsonValue>>* A = nullptr; (*T)->TryGetArrayField(Axis, A); return A;
    };
    const double Tol = 0.01;
    auto AxisEq = [Tol](const TArray<TSharedPtr<FJsonValue>>* A, const TArray<TSharedPtr<FJsonValue>>* B) -> bool
    {
        if (!A && !B) { return true; }
        if (!A || !B || A->Num() != B->Num()) { return false; }
        for (int32 i = 0; i < A->Num(); ++i)
        { if (FMath::Abs((*A)[i]->AsNumber() - (*B)[i]->AsNumber()) > Tol) { return false; } }
        return true;
    };
    auto TransformEq = [&](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B) -> bool
    {
        const bool HasA = GetXform(A, TEXT("loc")) != nullptr;
        const bool HasB = GetXform(B, TEXT("loc")) != nullptr;
        if (HasA != HasB) { return false; }
        if (!HasA) { return true; }
        return AxisEq(GetXform(A, TEXT("loc")), GetXform(B, TEXT("loc")))
            && AxisEq(GetXform(A, TEXT("rot")), GetXform(B, TEXT("rot")))
            && AxisEq(GetXform(A, TEXT("scale")), GetXform(B, TEXT("scale")));
    };

    TArray<TSharedPtr<FJsonValue>> Added, Removed, Changed;
    int32 Unchanged = 0;
    for (const TPair<FName, TSharedPtr<FJsonObject>>& P : ByNameA)
    {
        const TSharedPtr<FJsonObject>* BEntry = ByNameB.Find(P.Key);
        if (!BEntry) { Removed.Add(MakeShared<FJsonValueObject>(P.Value)); continue; }
        const TSharedPtr<FJsonObject>& A = P.Value;
        const TSharedPtr<FJsonObject>& B = *BEntry;

        TSharedPtr<FJsonObject> Changes = MakeShared<FJsonObject>();
        bool bAny = false;
        auto FieldDiff = [&](const TCHAR* Field)
        {
            const FString VA = GetStr(A, Field), VB = GetStr(B, Field);
            if (VA != VB)
            {
                TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
                C->SetStringField(TEXT("a"), VA); C->SetStringField(TEXT("b"), VB);
                Changes->SetObjectField(Field, C); bAny = true;
            }
        };
        FieldDiff(TEXT("class"));
        FieldDiff(TEXT("attach_parent"));
        FieldDiff(TEXT("attach_socket"));
        FieldDiff(TEXT("source"));
        if (!TransformEq(A, B))
        {
            const TSharedPtr<FJsonObject>* TA = nullptr; A->TryGetObjectField(TEXT("transform"), TA);
            const TSharedPtr<FJsonObject>* TB = nullptr; B->TryGetObjectField(TEXT("transform"), TB);
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetObjectField(TEXT("a"), TA ? *TA : MakeShared<FJsonObject>());
            C->SetObjectField(TEXT("b"), TB ? *TB : MakeShared<FJsonObject>());
            Changes->SetObjectField(TEXT("transform"), C); bAny = true;
        }
        if (bAny)
        {
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetStringField(TEXT("name"), P.Key.ToString());
            E->SetStringField(TEXT("class"), GetStr(A, TEXT("class")));
            E->SetObjectField(TEXT("changes"), Changes);
            Changed.Add(MakeShared<FJsonValueObject>(E));
        }
        else { ++Unchanged; }
    }
    for (const TPair<FName, TSharedPtr<FJsonObject>>& P : ByNameB)
    {
        if (!ByNameA.Contains(P.Key)) { Added.Add(MakeShared<FJsonValueObject>(P.Value)); }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_a"), NameA);
    Result->SetStringField(TEXT("a_path"), BPA->GetPathName());
    Result->SetStringField(TEXT("blueprint_b"), NameB);
    Result->SetStringField(TEXT("b_path"), BPathB);
    Result->SetStringField(TEXT("side_b_source"), SideBSource);
    Result->SetArrayField(TEXT("added"), Added);
    Result->SetArrayField(TEXT("removed"), Removed);
    Result->SetArrayField(TEXT("changed"), Changed);
    Result->SetNumberField(TEXT("unchanged_count"), Unchanged);
    TSharedPtr<FJsonObject> CA = MakeShared<FJsonObject>();
    CA->SetNumberField(TEXT("native"), NatA); CA->SetNumberField(TEXT("scs"), ScsA); CA->SetNumberField(TEXT("inherited"), InhA);
    TSharedPtr<FJsonObject> CB = MakeShared<FJsonObject>();
    CB->SetNumberField(TEXT("native"), NatB); CB->SetNumberField(TEXT("scs"), ScsB); CB->SetNumberField(TEXT("inherited"), InhB);
    Result->SetObjectField(TEXT("counts_a"), CA);
    Result->SetObjectField(TEXT("counts_b"), CB);
    Result->SetBoolField(TEXT("truncated"), TrA || TrB);
    Result->SetNumberField(TEXT("total_differences"), Added.Num() + Removed.Num() + Changed.Num());
    return Result;
}

// MCP:get_blueprint_component_overrides - Per-property PROVENANCE on a Blueprint's component templates: which values this Blueprint actually OVERRODE and which it merely inherits. Over the same components as get_blueprint_scs_tree (native CDO + own SCS + inherited chain) it resolves the effective template down the SCS -> child ICH override -> ancestor SCS -> native CDO cascade, never minting an ICH override, and compares each editable property against a baseline -- the archetype by default, or the same-named template in `other_blueprint` for the "fork vs original" question -- with the ENGINE's equality (FProperty::Identical, PPF_DeepComparison) instead of exported text. Each result is CLASSIFIED (identical | real_override | text_localization_metadata_only | object_outer_only | export_text_only | type_mismatch | no_baseline), so FText localization and object-outer noise stop masquerading as edits. WARNING: component templates can go STALE in memory (values can flip just from an editor restart): pass verify_on_disk to cross-check the raw .uasset bytes -- for anything not string-ish it answers found:null / applicable:false, never a misleading false. Read-only. list_instance_overrides asks the same question about a PLACED ACTOR.
//   @param blueprint_name - Blueprint asset path or short name to audit.
//   @param component_name - Optional: restrict the audit to this one component (exact variable name).
//   @param property_name - Optional: restrict the audit to this one property (FName or authored name).
//   @param other_blueprint - Optional: compare against the SAME-NAMED template in this other Blueprint instead of against the archetype (the "fork vs original" question).
//   @param include_native - Include native/CDO components (default true).
//   @param include_inherited - Include components inherited from ancestor Blueprints (default true).
//   @param include_identical - Also emit properties that are equal in BOTH the engine comparison and the exported text (default false -- they are the vast majority).
//   @param only_editable - Only consider CPF_Edit properties (default true). Transient/DuplicateTransient/NonPIEDuplicateTransient/Deprecated are always skipped.
//   @param verify_on_disk - Cross-check string-ish values against the raw .uasset bytes of the package that owns the template (default false).
//   @param max_components - Cap on components enumerated, 1..10000 (default 500).
//   @param max_properties_per_component - Cap on properties examined per component, 1..20000 (default 1000).
//   @returns { blueprint_name, blueprint_path, other_blueprint?, other_blueprint_path?, baseline_mode, components:[{component, resolved, template_name, template_path, scs_variable_name, component_key_guid, matched_by_scs_node, component_class, source, declaring_class, resolved_from_class, authored_here, baseline_source, baseline_path, baseline_class, component_class_mismatch, properties:[{component, property, property_display, value, baseline_value, inherited_value, inherited_value_available, authored_here, declaring_class, resolved_from_class, template_source, baseline_source, kind, differs, identical_deep, export_text_equal, export_text_value, export_text_baseline, note, on_disk?}], properties_reported, properties_examined, properties_truncated}], components_examined, components_truncated, properties_examined, override_count, noise_count, identical_count, no_baseline_count, on_disk:{found,not_found,not_applicable}, templates_may_be_stale, success }
TSharedPtr<FJsonObject> HandleGetBlueprintComponentOverrides(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }

    FString ComponentFilter;
    Params->TryGetStringField(TEXT("component_name"), ComponentFilter);
    FString PropertyFilter;
    Params->TryGetStringField(TEXT("property_name"), PropertyFilter);
    FString OtherBlueprintName;
    Params->TryGetStringField(TEXT("other_blueprint"), OtherBlueprintName);

    bool bIncludeNative = true;     Params->TryGetBoolField(TEXT("include_native"), bIncludeNative);
    bool bIncludeInherited = true;  Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);
    bool bIncludeIdentical = false; Params->TryGetBoolField(TEXT("include_identical"), bIncludeIdentical);
    bool bOnlyEditable = true;      Params->TryGetBoolField(TEXT("only_editable"), bOnlyEditable);
    bool bVerifyOnDisk = false;     Params->TryGetBoolField(TEXT("verify_on_disk"), bVerifyOnDisk);

    int32 MaxComponents = 500;
    Params->TryGetNumberField(TEXT("max_components"), MaxComponents);
    if (MaxComponents < 1 || MaxComponents > 10000) { MaxComponents = 500; }
    int32 MaxPropsPerComponent = 1000;
    Params->TryGetNumberField(TEXT("max_properties_per_component"), MaxPropsPerComponent);
    if (MaxPropsPerComponent < 1 || MaxPropsPerComponent > 20000) { MaxPropsPerComponent = 1000; }

    UBlueprint* BP = LoadBlueprintFlexible(BlueprintName);
    if (!BP)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }
    UBlueprint* OtherBP = nullptr;
    if (!OtherBlueprintName.IsEmpty())
    {
        OtherBP = LoadBlueprintFlexible(OtherBlueprintName);
        if (!OtherBP)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("other_blueprint not found: %s"), *OtherBlueprintName));
        }
    }

    // Enumerate components with the SAME collector as get_blueprint_scs_tree.
    TArray<TSharedPtr<FJsonValue>> Ordered;
    TMap<FName, TSharedPtr<FJsonObject>> ByName;
    int32 NatCount = 0, ScsCount = 0, InhCount = 0;
    bool bCollectTruncated = false;
    CollectBlueprintComponents(BP, bIncludeNative, bIncludeInherited, MaxComponents,
        Ordered, ByName, NatCount, ScsCount, InhCount, bCollectTruncated);

    // Package-bytes cache for verify_on_disk (one read per package, not per property).
    TMap<FString, TArray<uint8>> PackageBytesCache;
    TSet<FString> MissingPackageFiles;

    TArray<TSharedPtr<FJsonValue>> ComponentsOut;
    int32 ComponentsExamined = 0;
    int32 PropertiesExamined = 0;
    int32 OverrideCount = 0, NoiseCount = 0, IdenticalCount = 0, NoBaselineCount = 0;
    int32 OnDiskFound = 0, OnDiskNotFound = 0, OnDiskNotApplicable = 0;

    for (const TSharedPtr<FJsonValue>& Entry : Ordered)
    {
        const TSharedPtr<FJsonObject> Rec = Entry.IsValid() ? Entry->AsObject() : nullptr;
        if (!Rec.IsValid()) { continue; }
        FString CompName;
        Rec->TryGetStringField(TEXT("name"), CompName);
        if (CompName.IsEmpty()) { continue; }
        if (!ComponentFilter.IsEmpty() && CompName != ComponentFilter) { continue; }

        FComponentTemplateResolution_Analysis Res;
        if (!ResolveComponentTemplate_Analysis(BP, FName(*CompName), bIncludeInherited, Res) || !Res.Template)
        {
            TSharedPtr<FJsonObject> Unresolved = MakeShared<FJsonObject>();
            Unresolved->SetStringField(TEXT("component"), CompName);
            Unresolved->SetBoolField(TEXT("resolved"), false);
            Unresolved->SetStringField(TEXT("note"),
                TEXT("No template resolved (own SCS / child ICH / ancestor SCS / native CDO all missed) -- reported, not skipped."));
            ComponentsOut.Add(MakeShared<FJsonValueObject>(Unresolved));
            continue;
        }
        ++ComponentsExamined;

        // --- Baseline for THIS component ---
        UObject* Baseline = nullptr;
        FString BaselineSource;
        if (OtherBP)
        {
            FComponentTemplateResolution_Analysis OtherRes;
            if (ResolveComponentTemplate_Analysis(OtherBP, FName(*CompName), /*bIncludeInherited=*/true, OtherRes)
                && OtherRes.Template)
            {
                Baseline = OtherRes.Template;
                BaselineSource = TEXT("other_blueprint");
            }
            else
            {
                BaselineSource = TEXT("other_blueprint_missing_component");
            }
        }
        else
        {
            Baseline = Res.Template->GetArchetype();
            BaselineSource = TEXT("archetype");
            if (!Baseline)
            {
                Baseline = Res.Template->GetClass()->GetDefaultObject();
                BaselineSource = TEXT("class_default");
            }
        }

        TArray<TSharedPtr<FJsonValue>> PropsOut;
        int32 PropsSeen = 0;
        bool bPropsTruncated = false;

        for (TFieldIterator<FProperty> It(Res.Template->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop) { continue; }
            if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated))
            {
                continue;
            }
            if (bOnlyEditable && !Prop->HasAnyPropertyFlags(CPF_Edit)) { continue; }
            if (!PropertyFilter.IsEmpty()
                && Prop->GetName() != PropertyFilter
                && Prop->GetAuthoredName() != PropertyFilter)
            {
                continue;
            }
            if (PropsSeen >= MaxPropsPerComponent) { bPropsTruncated = true; break; }
            ++PropsSeen;
            ++PropertiesExamined;

            FProperty* BaselineProp = Baseline
                ? FindFProperty<FProperty>(Baseline->GetClass(), Prop->GetFName())
                : nullptr;

            TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
            PropObj->SetStringField(TEXT("component"), CompName);
            PropObj->SetStringField(TEXT("property"), Prop->GetName());
            PropObj->SetStringField(TEXT("property_display"), Prop->GetAuthoredName());
            PropObj->SetField(TEXT("value"),
                mcp_common::PropertyValueToJson(Prop, Prop->ContainerPtrToValuePtr<void>(Res.Template)));
            PropObj->SetBoolField(TEXT("authored_here"), Res.bAuthoredHere);
            PropObj->SetStringField(TEXT("declaring_class"), Res.DeclaringClass);
            PropObj->SetStringField(TEXT("resolved_from_class"), Res.ResolvedFromClass);
            PropObj->SetStringField(TEXT("template_source"), Res.Source);
            PropObj->SetStringField(TEXT("baseline_source"), BaselineSource);

            // The ancestor-declared value, when the component is inherited. Independent of the
            // baseline: with other_blueprint the baseline is a sibling, not the ancestor.
            FProperty* InhProp = Res.InheritedTemplate
                ? FindFProperty<FProperty>(Res.InheritedTemplate->GetClass(), Prop->GetFName())
                : nullptr;
            if (InhProp)
            {
                PropObj->SetField(TEXT("inherited_value"),
                    mcp_common::PropertyValueToJson(InhProp, InhProp->ContainerPtrToValuePtr<void>(Res.InheritedTemplate)));
                PropObj->SetBoolField(TEXT("inherited_value_available"), true);
            }
            else
            {
                PropObj->SetField(TEXT("inherited_value"), MakeShared<FJsonValueNull>());
                PropObj->SetBoolField(TEXT("inherited_value_available"), false);
            }

            bool bIsOverride = false, bIsIdentical = false, bIsNoise = false, bNoBaseline = false;
            if (!Baseline || !BaselineProp)
            {
                // Honest third state: NOT compared. Reporting this as an override would invent a
                // difference; reporting it as identical would hide one.
                bNoBaseline = true;
                PropObj->SetField(TEXT("baseline_value"), MakeShared<FJsonValueNull>());
                PropObj->SetStringField(TEXT("kind"), TEXT("no_baseline"));
                PropObj->SetField(TEXT("differs"), MakeShared<FJsonValueNull>());
                PropObj->SetStringField(TEXT("note"), Baseline
                    ? TEXT("The baseline object's class has no property of this name -- NOT compared.")
                    : TEXT("No baseline object could be resolved for this component -- NOT compared."));
            }
            else
            {
                const mcp_common::FPropertyDiffResult Diff = mcp_common::ComparePropertyValues(
                    Prop, Res.Template, BaselineProp, Baseline, PPF_DeepComparison);
                PropObj->SetField(TEXT("baseline_value"),
                    mcp_common::PropertyValueToJson(BaselineProp, BaselineProp->ContainerPtrToValuePtr<void>(Baseline)));
                PropObj->SetStringField(TEXT("kind"), mcp_common::PropertyDiffKindToString(Diff.Kind));
                PropObj->SetBoolField(TEXT("differs"), Diff.bDiffers);
                PropObj->SetBoolField(TEXT("identical_deep"), Diff.bIdenticalDeep);
                PropObj->SetBoolField(TEXT("export_text_equal"), Diff.bExportTextEqual);
                PropObj->SetStringField(TEXT("export_text_value"), Diff.ValueA);
                PropObj->SetStringField(TEXT("export_text_baseline"), Diff.ValueB);
                PropObj->SetStringField(TEXT("note"), Diff.Note);

                bIsOverride = (Diff.Kind == mcp_common::EPropertyDiffKind::RealOverride)
                           || (Diff.Kind == mcp_common::EPropertyDiffKind::TypeMismatch);
                bIsIdentical = (Diff.Kind == mcp_common::EPropertyDiffKind::Identical);
                bIsNoise = !bIsOverride && !bIsIdentical;
            }

            const bool bEmit = bIsOverride || bIsNoise || bNoBaseline || (bIncludeIdentical && bIsIdentical);
            if (bIsOverride)      { ++OverrideCount; }
            else if (bIsIdentical){ ++IdenticalCount; }
            else if (bNoBaseline) { ++NoBaselineCount; }
            else                  { ++NoiseCount; }
            if (!bEmit) { continue; }

            if (bVerifyOnDisk)
            {
                TSharedPtr<FJsonObject> OnDisk = MakeShared<FJsonObject>();
                FString Needle;
                const bool bStringish = GetStringishNeedle_Analysis(Prop, Res.Template, Needle);
                if (!bStringish || Needle.Len() < 3)
                {
                    OnDisk->SetBoolField(TEXT("applicable"), false);
                    OnDisk->SetField(TEXT("found"), MakeShared<FJsonValueNull>());
                    OnDisk->SetStringField(TEXT("reason"), bStringish
                        ? TEXT("string value shorter than 3 chars -- a raw byte scan would not be discriminating")
                        : TEXT("not a string-ish property (FStr/FName/FText/FSoftObject) -- a raw byte scan cannot decide"));
                    ++OnDiskNotApplicable;
                }
                else
                {
                    FString PkgName, FilePath;
                    const TArray<uint8>* Bytes = GetPackageBytesCached_Analysis(
                        Res.Template, PackageBytesCache, MissingPackageFiles, PkgName, FilePath);
                    OnDisk->SetStringField(TEXT("package_name"), PkgName);
                    OnDisk->SetStringField(TEXT("package_file"), FilePath);
                    OnDisk->SetStringField(TEXT("needle"), Needle);
                    if (!Bytes)
                    {
                        OnDisk->SetBoolField(TEXT("applicable"), false);
                        OnDisk->SetField(TEXT("found"), MakeShared<FJsonValueNull>());
                        OnDisk->SetStringField(TEXT("reason"), TEXT("owning package has no readable file on disk"));
                        ++OnDiskNotApplicable;
                    }
                    else
                    {
                        const bool bFound = PackageBytesContain_Analysis(*Bytes, Needle);
                        OnDisk->SetBoolField(TEXT("applicable"), true);
                        OnDisk->SetBoolField(TEXT("found"), bFound);
                        if (bFound) { ++OnDiskFound; } else { ++OnDiskNotFound; }
                    }
                }
                PropObj->SetObjectField(TEXT("on_disk"), OnDisk);
            }

            PropsOut.Add(MakeShared<FJsonValueObject>(PropObj));
        }

        TSharedPtr<FJsonObject> CompObj = MakeShared<FJsonObject>();
        CompObj->SetStringField(TEXT("component"), CompName);
        CompObj->SetBoolField(TEXT("resolved"), true);
        // GROUND TRUTH: the template's RAW UObject name (never normalized), plus the SCS node /
        // FComponentKey it was paired by -- so a trailing-digit rename can never be papered over.
        CompObj->SetStringField(TEXT("template_name"), Res.Template->GetName());
        CompObj->SetStringField(TEXT("template_path"), Res.Template->GetPathName());
        if (Res.MatchedNode)
        {
            const FComponentKey Key(Res.MatchedNode);
            CompObj->SetStringField(TEXT("scs_variable_name"), Res.MatchedNode->GetVariableName().ToString());
            CompObj->SetStringField(TEXT("component_key_guid"), Key.GetAssociatedGuid().ToString());
        }
        else
        {
            CompObj->SetStringField(TEXT("scs_variable_name"), FString());
            CompObj->SetStringField(TEXT("component_key_guid"), FString());
        }
        CompObj->SetBoolField(TEXT("matched_by_scs_node"), Res.MatchedNode != nullptr);
        CompObj->SetStringField(TEXT("component_class"), Res.Template->GetClass()->GetPathName());
        CompObj->SetStringField(TEXT("source"), Res.Source);
        CompObj->SetStringField(TEXT("declaring_class"), Res.DeclaringClass);
        CompObj->SetStringField(TEXT("resolved_from_class"), Res.ResolvedFromClass);
        CompObj->SetBoolField(TEXT("authored_here"), Res.bAuthoredHere);
        CompObj->SetStringField(TEXT("baseline_source"), BaselineSource);
        CompObj->SetStringField(TEXT("baseline_path"), Baseline ? Baseline->GetPathName() : FString());
        CompObj->SetStringField(TEXT("baseline_class"), Baseline ? Baseline->GetClass()->GetPathName() : FString());
        CompObj->SetBoolField(TEXT("component_class_mismatch"),
            Baseline != nullptr && Baseline->GetClass() != Res.Template->GetClass());
        CompObj->SetArrayField(TEXT("properties"), PropsOut);
        CompObj->SetNumberField(TEXT("properties_reported"), PropsOut.Num());
        CompObj->SetNumberField(TEXT("properties_examined"), PropsSeen);
        CompObj->SetBoolField(TEXT("properties_truncated"), bPropsTruncated);
        ComponentsOut.Add(MakeShared<FJsonValueObject>(CompObj));
    }

    TSharedPtr<FJsonObject> Result = UEAgentAnalysisCommandsImpl::StartBlueprintReply(BlueprintName, BP->GetPathName());
    if (OtherBP)
    {
        Result->SetStringField(TEXT("other_blueprint"), OtherBlueprintName);
        Result->SetStringField(TEXT("other_blueprint_path"), OtherBP->GetPathName());
    }
    Result->SetStringField(TEXT("baseline_mode"), OtherBP ? TEXT("other_blueprint") : TEXT("archetype"));
    Result->SetArrayField(TEXT("components"), ComponentsOut);
    Result->SetNumberField(TEXT("components_examined"), ComponentsExamined);
    Result->SetBoolField(TEXT("components_truncated"), bCollectTruncated);
    Result->SetNumberField(TEXT("properties_examined"), PropertiesExamined);
    Result->SetNumberField(TEXT("override_count"), OverrideCount);
    Result->SetNumberField(TEXT("noise_count"), NoiseCount);
    Result->SetNumberField(TEXT("identical_count"), IdenticalCount);
    Result->SetNumberField(TEXT("no_baseline_count"), NoBaselineCount);
    TSharedPtr<FJsonObject> OnDiskTotals = MakeShared<FJsonObject>();
    OnDiskTotals->SetNumberField(TEXT("found"), OnDiskFound);
    OnDiskTotals->SetNumberField(TEXT("not_found"), OnDiskNotFound);
    OnDiskTotals->SetNumberField(TEXT("not_applicable"), OnDiskNotApplicable);
    Result->SetObjectField(TEXT("on_disk"), OnDiskTotals);
    Result->SetBoolField(TEXT("templates_may_be_stale"), !bVerifyOnDisk);

    // Honest verification envelope, written FLAT so the payload stays at result.* .
    if (!bVerifyOnDisk)
    {
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotAttempted,
            TEXT("in-memory component templates only"),
            TEXT("templates can be frozen/stale in a long editor session -- pass verify_on_disk=true to cross-check string-ish values against the raw .uasset bytes"));
    }
    else if (OnDiskFound == 0 && OnDiskNotFound == 0)
    {
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotVerifiable,
            TEXT("raw .uasset byte scan"),
            TEXT("no reported value was string-ish enough to look for in the package bytes"));
    }
    else
    {
        mcp_common::AddVerificationEvidence(Result,
            OnDiskNotFound == 0 ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
            TEXT("raw .uasset byte scan of the package owning each template"),
            FString::Printf(TEXT("%d value(s) found on disk, %d NOT found, %d not applicable"),
                OnDiskFound, OnDiskNotFound, OnDiskNotApplicable));
    }
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

// MCP:verify_reparent_success - Check, after a reparent_blueprint, that the Blueprint really ended up where you wanted: it confirms the parent class against an expectation, reports the compile status, and -- when you feed it counts captured BEFOREHAND -- verifies no components or variables were lost, listing every failed invariant in `regressions[]`. This is the oracle for a reparent, because reparent_blueprint's own return value says nothing about whether the class survived it. WARNING: read-only by default, which means it reports the CACHED Blueprint->Status: pass force_compile=true to recompile and get the real status plus the compiler messages (compile_messages[] is empty without it, and `compile_ran` tells you which mode you got). Component count comes from the compiled CDO (native + SCS + inherited). Omit the expected_* counts to skip those invariants; get_blueprint_scs_tree or get_blueprint_variables_list before the reparent is where they come from.
//   @param blueprint_name - Blueprint asset name or path.
//   @param expected_parent_class - Class name or path the Blueprint should now inherit from.
//   @param expected_component_count - Optional: CDO component count captured before the reparent.
//   @param expected_variable_count - Optional: own-variable (NewVariables) count captured before the reparent.
//   @param force_compile - Optional (default false): recompile to obtain the real compile status AND the compiler messages (compile_messages[] is empty without it -- compile_ran says which).
//   @returns { blueprint_name, blueprint_path, parent_class, expected_parent_class, parent_match, compile_status, compile_match, compile_ran, compile_messages:[{severity,message}], compile_num_errors, compile_num_warnings, component_count, expected_component_count, components_match, variable_count, expected_variable_count, variables_match, regressions:[string], success }
TSharedPtr<FJsonObject> HandleVerifyReparentSuccess(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ExpectedParentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("expected_parent_class"), ExpectedParentName)) { return Missing; }

    UBlueprint* Blueprint = LoadBlueprintFlexible(BlueprintName);
    if (!Blueprint)
        return mcp_common::BlueprintNotFoundReply(BlueprintName);

    UClass* ExpectedParent = FUEAgentGraphUtils::ResolveClass(ExpectedParentName);
    if (!ExpectedParent)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve expected_parent_class: %s"), *ExpectedParentName));

    TArray<TSharedPtr<FJsonValue>> Regressions;

    // 1) Parent chain.
    const bool bParentMatch = (Blueprint->ParentClass == ExpectedParent);
    if (!bParentMatch)
        Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("parent mismatch: is '%s', expected '%s'"),
            Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("None"), *ExpectedParent->GetName())));

    // 2) Compile state (optional recompile for the real status). The FCompilerResultsLog is
    // returned, so errors like "Internal Compiler Error: Tried to create a property
    // DefaultSceneRoot..." reach the caller instead of staying in the output log.
    bool bForceCompile = false;
    Params->TryGetBoolField(TEXT("force_compile"), bForceCompile);
    TArray<TSharedPtr<FJsonValue>> CompileMessages;
    int32 CompileNumErrors = 0;
    int32 CompileNumWarnings = 0;
    if (bForceCompile)
    {
        FCompilerResultsLog Results;
        Results.SetSilentMode(true);
        FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
        CompileNumErrors = Results.NumErrors;
        CompileNumWarnings = Results.NumWarnings;
        CompileMessages = AssetCommandHelpers::CompilerMessagesToJson(Results);
    }
    const EBlueprintStatus Status = Blueprint->Status;
    const bool bCompileMatch = (Status != BS_Error);
    const TCHAR* StatusStr = BlueprintStatusToString_Analysis(Status);
    if (!bCompileMatch)
        Regressions.Add(MakeShared<FJsonValueString>(TEXT("blueprint is in an error state after reparent")));

    // 3) Component count from the compiled CDO (native + SCS + inherited). Actor BPs only.
    int32 ComponentCount = -1;
    UClass* GenClass = Blueprint->GeneratedClass;
    if (GenClass && GenClass->IsChildOf(AActor::StaticClass()))
    {
        if (AActor* CDO = Cast<AActor>(GenClass->GetDefaultObject(false)))
        {
            ComponentCount = CDO->GetComponents().Num();
        }
    }
    bool bComponentsMatch = true;
    double ExpectedCompD = 0.0;
    const bool bHasExpectedComp = Params->TryGetNumberField(TEXT("expected_component_count"), ExpectedCompD);
    if (bHasExpectedComp)
    {
        bComponentsMatch = (ComponentCount == (int32)ExpectedCompD);
        if (!bComponentsMatch)
            Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("component count regressed: is %d, expected %d"), ComponentCount, (int32)ExpectedCompD)));
    }

    // 4) Own-variable count (NewVariables -- the set a reparent can clobber).
    const int32 VariableCount = Blueprint->NewVariables.Num();
    bool bVariablesMatch = true;
    double ExpectedVarD = 0.0;
    const bool bHasExpectedVar = Params->TryGetNumberField(TEXT("expected_variable_count"), ExpectedVarD);
    if (bHasExpectedVar)
    {
        bVariablesMatch = (VariableCount == (int32)ExpectedVarD);
        if (!bVariablesMatch)
            Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("variable count regressed: is %d, expected %d"), VariableCount, (int32)ExpectedVarD)));
    }

    const bool bSuccess = bParentMatch && bCompileMatch && bComponentsMatch && bVariablesMatch;

    TSharedPtr<FJsonObject> Result = UEAgentAnalysisCommandsImpl::StartBlueprintReply(BlueprintName, Blueprint->GetPathName());
    Result->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : TEXT("None"));
    Result->SetStringField(TEXT("expected_parent_class"), ExpectedParent->GetName());
    Result->SetBoolField(TEXT("parent_match"), bParentMatch);
    Result->SetStringField(TEXT("compile_status"), StatusStr);
    Result->SetBoolField(TEXT("compile_match"), bCompileMatch);
    // The compiler log is surfaced, not discarded. compile_ran=false means the
    // status above is the CACHED Blueprint->Status, not a fresh compile -- pass force_compile=true.
    Result->SetBoolField(TEXT("compile_ran"), bForceCompile);
    Result->SetArrayField(TEXT("compile_messages"), CompileMessages);
    Result->SetNumberField(TEXT("compile_num_errors"), CompileNumErrors);
    Result->SetNumberField(TEXT("compile_num_warnings"), CompileNumWarnings);
    Result->SetNumberField(TEXT("component_count"), ComponentCount);
    if (bHasExpectedComp) Result->SetNumberField(TEXT("expected_component_count"), (int32)ExpectedCompD);
    Result->SetBoolField(TEXT("components_match"), bComponentsMatch);
    Result->SetNumberField(TEXT("variable_count"), VariableCount);
    if (bHasExpectedVar) Result->SetNumberField(TEXT("expected_variable_count"), (int32)ExpectedVarD);
    Result->SetBoolField(TEXT("variables_match"), bVariablesMatch);
    Result->SetArrayField(TEXT("regressions"), Regressions);
    Result->SetBoolField(TEXT("success"), bSuccess);
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("check_asset_circular_dependency"), &HandleCheckAssetCircularDependency },
    { TEXT("diff_blueprint_components"), &HandleDiffBlueprintComponents },
    { TEXT("diff_blueprint_graphs"), &HandleDiffBlueprintGraphs },
    { TEXT("diff_blueprints"), &HandleDiffBlueprints },
    { TEXT("diff_object_snapshots"), &HandleDiffObjectSnapshots },
    { TEXT("find_blueprint_assets_by_class"), &HandleFindBlueprintAssetsByClass },
    { TEXT("find_erroring_blueprints"), &HandleFindErroringBlueprints },
    { TEXT("get_asset_dependencies"), &HandleGetAssetDependencies },
    { TEXT("get_asset_dependents"), &HandleGetAssetDependents },
    { TEXT("get_asset_referencers"), &HandleGetAssetReferencers },
    { TEXT("get_blueprint_children"), &HandleGetBlueprintChildren },
    { TEXT("get_blueprint_component_overrides"), &HandleGetBlueprintComponentOverrides },
    { TEXT("get_blueprint_components_list"), &HandleGetBlueprintComponentsList },
    { TEXT("get_blueprint_exec_path"), &HandleGetBlueprintExecPath },
    { TEXT("get_blueprint_functions_list"), &HandleGetBlueprintFunctionsList },
    { TEXT("get_blueprint_parent"), &HandleGetBlueprintParent },
    { TEXT("get_blueprint_scs_tree"), &HandleGetBlueprintScsTree },
    { TEXT("get_blueprint_variables_list"), &HandleGetBlueprintVariablesList },
    { TEXT("snapshot_object"), &HandleSnapshotObject },
    { TEXT("verify_reparent_success"), &HandleVerifyReparentSuccess },
};
} // namespace UEAgentAnalysisCommands
