// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_EditablePinBase.h"

#include "Editor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"   // FCompilerResultsLog (UNREALED_API; UnrealEd already linked)
#include "Logging/TokenizedMessage.h"     // FTokenizedMessage / EMessageSeverity
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"    // UActorComponent::CreationMethod (native-component filter)
#include "Components/SceneComponent.h"    // USceneComponent::GetDefaultSceneRootVariableName()

#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"       // FProperty, TFieldIterator
#include "UObject/ObjectMacros.h"     // CPF_* flags
#include "UObject/Class.h"            // UFunction::FindFunctionByName / UStruct::FindPropertyByName
#include "ScopedTransaction.h"

// fork_class_chain asset-creation building blocks (all already-linked modules).
#include "Misc/PackageName.h"
#include "UObject/Package.h"          // UPackage
#include "UObject/UObjectGlobals.h"   // CreatePackage / FindObject
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UEAgentProjectSettings.h"

#define LOCTEXT_NAMESPACE "UEAgentReparent"

/**
 * Blueprint RE-PARENT tooling: larger, stateful, mutating tools around a parent change.
 *
 *   - reparent_blueprint         : snapshot (variables + SCS nodes BY GUID) ->
 *                                  optional collision-var rename -> reparent with
 *                                  the editor's SCS scene-root reconciliation and
 *                                  the delta compile flags, capturing the compiler's
 *                                  FCompilerResultsLog -> post-check invariants ->
 *                                  in-memory rollback that RESTORES the SCS names
 *                                  and re-verifies itself (rollback_complete)
 *                                  (with a dry_run analysis mode).
 *   - reconcile_function_signature: repair a LOCAL function graph's Entry/Result
 *                                  parameter pins against a parent UFUNCTION,
 *                                  preserving downstream nodes.
 *   - fork_class_chain           : thin-fork a base class into a new child
 *                                  Blueprint, optionally reparent a target onto
 *                                  it, tag its fork origin, and verify.
 *
 * Isolated from UEAgentGraphCommands so this destructive multi-step
 * transaction surface lives in its own translation unit. The reparent core here
 * sets the delta-serialization compile flags itself so it does
 * not depend on UEAgentGraphCommands' HandleReparentBlueprint.
 */
namespace UEAgentReparentCommands
{

// MCP-DOMAIN: Blueprint

namespace
{
    // Short-name via mcp_common::ResolveBlueprint, else a full object path via LoadObject.
    // Mirrors UEAgentGraphCommands' anon FindBlueprintFlexible.
    UBlueprint* FindBlueprintFlexible_Reparent(const FString& Name)
    {
        UBlueprint* BP = mcp_common::ResolveBlueprint(Name);
        if (!BP)
        {
            BP = LoadObject<UBlueprint>(nullptr, *Name);
        }
        return BP;
    }

    // Find a function graph on FunctionGraphs, then in interface-override graphs
    // (ImplementedInterfaces[].Graphs). Mirrors GraphCommands' anon FindFunctionGraph.
    UEdGraph* FindFunctionGraphLocal(UBlueprint* Blueprint, const FString& FunctionName)
    {
        if (!Blueprint)
        {
            return nullptr;
        }
        const FName FN(*FunctionName);
        for (UEdGraph* G : Blueprint->FunctionGraphs)
        {
            if (G && G->GetFName() == FN)
            {
                return G;
            }
        }
        for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
        {
            for (UEdGraph* G : InterfaceDesc.Graphs)
            {
                if (G && G->GetFName() == FN)
                {
                    return G;
                }
            }
        }
        return nullptr;
    }

    const TCHAR* BlueprintStatusToString(EBlueprintStatus Status)
    {
        switch (Status)
        {
        case BS_Error:                return TEXT("error");
        case BS_UpToDateWithWarnings: return TEXT("warnings");
        case BS_UpToDate:             return TEXT("up_to_date");
        case BS_Dirty:                return TEXT("dirty");
        case BS_BeingCreated:         return TEXT("being_created");
        default:                      return TEXT("unknown");
        }
    }

    // Component count from the compiled CDO (native + SCS + inherited). Actor BPs only; -1 otherwise.
    int32 CountCdoComponents(UBlueprint* Blueprint)
    {
        UClass* GenClass = Blueprint ? Blueprint->GeneratedClass : nullptr;
        if (GenClass && GenClass->IsChildOf(AActor::StaticClass()))
        {
            if (AActor* CDO = Cast<AActor>(GenClass->GetDefaultObject(false)))
            {
                return CDO->GetComponents().Num();
            }
        }
        return -1;
    }

    // ---------------------------------------------------------------------------------------
    // SCS node snapshot.
    // A reparent can silently RENAME an SCS node ("Mesh" -> "Mesh1"), and such a rename is
    // easily misread as a blocking defect. Names alone cannot distinguish a rename from a
    // delete+add, so identity is
    // taken from USCS_Node::VariableGuid (SCS_Node.h), the only field that survives a rename.
    // Nodes carrying an INVALID guid fall back to name identity and are counted separately rather
    // than being silently dropped from the diff.
    // ---------------------------------------------------------------------------------------
    struct FScsNodeSnapshot_Reparent
    {
        FGuid  VariableGuid;
        FName  VariableName = NAME_None;
        bool   bIsDefaultSceneRoot = false;
        bool   bInAllNodes = false;      // false => reachable only via GetDefaultSceneRootNode()
    };

    // Snapshot every SCS node of the Blueprint's OWN SimpleConstructionScript. The DefaultSceneRoot
    // node is included even when it is not in AllNodes, because ValidateSceneRootNodes moves it in
    // and out of RootNodes/AllNodes (SimpleConstructionScript.cpp) and we
    // want that transition reported, not mistaken for a deletion.
    void SnapshotScsNodes_Reparent(UBlueprint* Blueprint, TArray<FScsNodeSnapshot_Reparent>& OutSnapshot)
    {
        OutSnapshot.Reset();
        USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
        if (!SCS)
        {
            return;
        }
        const FName DefaultRootName = USceneComponent::GetDefaultSceneRootVariableName();

        TSet<const USCS_Node*> Seen;
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node)
            {
                continue;
            }
            Seen.Add(Node);
            FScsNodeSnapshot_Reparent Entry;
            Entry.VariableGuid = Node->VariableGuid;
            Entry.VariableName = Node->GetVariableName();
            Entry.bIsDefaultSceneRoot = (Entry.VariableName == DefaultRootName);
            Entry.bInAllNodes = true;
            OutSnapshot.Add(Entry);
        }
        if (USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode())
        {
            if (!Seen.Contains(DefaultRoot))
            {
                FScsNodeSnapshot_Reparent Entry;
                Entry.VariableGuid = DefaultRoot->VariableGuid;
                Entry.VariableName = DefaultRoot->GetVariableName();
                Entry.bIsDefaultSceneRoot = true;
                Entry.bInAllNodes = false;
                OutSnapshot.Add(Entry);
            }
        }
    }

    // Current-state lookup tables for the post-check. ByGuid/ByName also carry the DefaultSceneRoot
    // node when it has been pulled out of AllNodes; InAllNodes holds only what is really in the tree.
    void BuildScsLookup_Reparent(UBlueprint* Blueprint,
                        TMap<FGuid, USCS_Node*>& OutByGuid,
                        TMap<FName, USCS_Node*>& OutByName,
                        TSet<FName>& OutInAllNodes)
    {
        OutByGuid.Reset();
        OutByName.Reset();
        OutInAllNodes.Reset();
        USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
        if (!SCS)
        {
            return;
        }
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node)
            {
                continue;
            }
            if (Node->VariableGuid.IsValid())
            {
                OutByGuid.Add(Node->VariableGuid, Node);
            }
            OutByName.Add(Node->GetVariableName(), Node);
            OutInAllNodes.Add(Node->GetVariableName());
        }
        if (USCS_Node* DefaultRoot = SCS->GetDefaultSceneRootNode())
        {
            if (DefaultRoot->VariableGuid.IsValid() && !OutByGuid.Contains(DefaultRoot->VariableGuid))
            {
                OutByGuid.Add(DefaultRoot->VariableGuid, DefaultRoot);
            }
            if (!OutByName.Contains(DefaultRoot->GetVariableName()))
            {
                OutByName.Add(DefaultRoot->GetVariableName(), DefaultRoot);
            }
        }
    }

    // Resolve a snapshot entry against the CURRENT SCS: guid first (survives renames), name as the
    // documented fallback for guid-less nodes. Returns nullptr when the node is really gone.
    USCS_Node* ResolveSnapshotNode_Reparent(const FScsNodeSnapshot_Reparent& Entry,
                                   const TMap<FGuid, USCS_Node*>& ByGuid,
                                   const TMap<FName, USCS_Node*>& ByName,
                                   bool& bOutMatchedByGuid)
    {
        bOutMatchedByGuid = false;
        if (Entry.VariableGuid.IsValid())
        {
            if (USCS_Node* const* Found = ByGuid.Find(Entry.VariableGuid))
            {
                bOutMatchedByGuid = true;
                return *Found;
            }
            return nullptr;
        }
        if (USCS_Node* const* Found = ByName.Find(Entry.VariableName))
        {
            return *Found;
        }
        return nullptr;
    }

    // Compile diagnostics -> JSON. Same severity mapping as compile_blueprint's
    // return_messages path (UEAgentBlueprintCommands.cpp) so the two tools agree.
    void CompilerLogToJson_Reparent(const FCompilerResultsLog& Log, TArray<TSharedPtr<FJsonValue>>& OutMessages)
    {
        for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
        {
            TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
            const EMessageSeverity::Type Sev = Msg->GetSeverity();
            FString SevStr = TEXT("note");
            if (Sev == EMessageSeverity::Error) { SevStr = TEXT("error"); }
            else if (Sev == EMessageSeverity::Warning || Sev == EMessageSeverity::PerformanceWarning) { SevStr = TEXT("warning"); }
            M->SetStringField(TEXT("severity"), SevStr);
            M->SetStringField(TEXT("text"), Msg->ToText().ToString());
            OutMessages.Add(MakeShared<FJsonValueObject>(M));
        }
    }

    // Each node of SCS as variable name -> component class path (names already present are kept).
    void AddScsComponentNames_Reparent(USimpleConstructionScript& SCS, TMap<FName, FString>& OutNameToClassPath)
    {
        for (USCS_Node* Node : SCS.GetAllNodes())
        {
            if (!Node || !Node->ComponentTemplate)
            {
                continue;
            }
            UClass* CompClass = Node->ComponentClass ? Node->ComponentClass.Get() : Node->ComponentTemplate->GetClass();
            OutNameToClassPath.FindOrAdd(Node->GetVariableName(), CompClass ? CompClass->GetPathName() : FString());
        }
    }

    // Minimal component-surface collector for the dry_run report.
    // AnalysisCommands' richer CollectBlueprintComponents lives in an ANONYMOUS namespace in another
    // translation unit (UEAgentAnalysisCommands.cpp) -> not linkable from here, and that file
    // belongs to another module area. This is the minimum the report needs:
    // component NAME -> class path, native CDO components plus the SCS of the whole ancestor chain,
    // most-derived-wins (FindOrAdd from derived to base never overwrites).
    void CollectClassComponentNames_Reparent(UClass* InClass, TMap<FName, FString>& OutNameToClassPath)
    {
        if (!InClass)
        {
            return;
        }
        for (UClass* Cur = InClass; Cur; Cur = Cur->GetSuperClass())
        {
            UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(Cur);
            if (!BPGC || !BPGC->SimpleConstructionScript)
            {
                continue;
            }
            AddScsComponentNames_Reparent(*BPGC->SimpleConstructionScript, OutNameToClassPath);
        }
        if (InClass->IsChildOf(AActor::StaticClass()))
        {
            if (AActor* CDO = Cast<AActor>(InClass->GetDefaultObject(false)))
            {
                for (UActorComponent* Comp : CDO->GetComponents())
                {
                    if (!Comp || Comp->CreationMethod != EComponentCreationMethod::Native)
                    {
                        continue;
                    }
                    OutNameToClassPath.FindOrAdd(Comp->GetFName(), Comp->GetClass()->GetPathName());
                }
            }
        }
    }

    // Own SCS component surface of a Blueprint (NOT including anything inherited).
    void CollectOwnScsComponentNames_Reparent(UBlueprint* Blueprint, TMap<FName, FString>& OutNameToClassPath)
    {
        USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
        if (SCS)
        {
            AddScsComponentNames_Reparent(*SCS, OutNameToClassPath);
        }
    }

    // The fields every reparent_blueprint reply starts with: the Blueprint and both parents.
    TSharedPtr<FJsonObject> StartReparentReply_Reparent(const FString& BlueprintName, const UBlueprint& Blueprint,
        const FString& OldParentName, const UClass& NewParent)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
        Result->SetStringField(TEXT("blueprint_path"), Blueprint.GetPathName());
        Result->SetStringField(TEXT("old_parent"), OldParentName);
        Result->SetStringField(TEXT("new_parent"), NewParent.GetName());
        return Result;
    }

    // Portable core of FBlueprintEditor::EnsureBlueprintIsUpToDate (BlueprintEditor.cpp).
    // The editor's version is a NON-STATIC member of FBlueprintEditor (BlueprintEditor.h, Kismet
    // module) -- it needs an open Blueprint editor toolkit, which a headless MCP call does not have, so
    // it is NOT reachable from this plugin. Every step below is replicated against public engine API
    // that this module already links:
    //   FBlueprintEditorUtils::PurgeNullGraphs               (BlueprintEditorUtils.h, UNREALED_API)
    //   FKismetEditorUtilities::UpgradeCosmeticallyStaleBlueprint (KismetEditorUtilities.h, UNREALED_API)
    //   FBlueprintEditorUtils::SupportsConstructionScript    (BlueprintEditorUtils.h, UNREALED_API)
    //   USimpleConstructionScript::ValidateSceneRootNodes    (SimpleConstructionScript.h, ENGINE_API)  <-- THE unblocker
    //   Conform{CallsToParentFunctions,ImplementedEvents,ImplementedInterfaces} / UpdateOutOfDateCompositeNodes /
    //   UpdateTransactionalFlags                             (BlueprintEditorUtils.h/795/1495/1513/1522)
    // DELIBERATELY NOT replicated, and reported instead of faked:
    //   - CreateSubobjectEditors(): pure Slate.
    //   - the User-Construction-Script auto-create branch: gated on
    //     UBlueprintEditorSettings::IsFunctionAllowedForAsset; authoring a UCS without that gate would
    //     create a graph the project settings may forbid.
    //   - the "SCS not supported -> delete the whole SCS" else-branch: destructive,
    //     and no reparent in this tool's scope needs it.
    // Returns true when ValidateSceneRootNodes() actually ran.
    bool EnsureBlueprintIsUpToDate_Reparent(UBlueprint* Blueprint, bool& bOutScsCreated)
    {
        if (!Blueprint)
        {
            return false;
        }
        FBlueprintEditorUtils::PurgeNullGraphs(Blueprint);
        FKismetEditorUtilities::UpgradeCosmeticallyStaleBlueprint(Blueprint);

        bool bValidatedSceneRoot = false;
        if (FBlueprintEditorUtils::SupportsConstructionScript(Blueprint))
        {
            if (Blueprint->SimpleConstructionScript == nullptr && Blueprint->GeneratedClass != nullptr)
            {
                // Mirrors BlueprintEditor.cpp (minus the Slate CreateSubobjectEditors call).
                // The engine uses check() on GeneratedClass; we guard instead so a half-built asset
                // cannot crash the user's open editor.
                Blueprint->SimpleConstructionScript = NewObject<USimpleConstructionScript>(Blueprint->GeneratedClass);
                Blueprint->SimpleConstructionScript->SetFlags(RF_Transactional);
                bOutScsCreated = true;
            }
            if (Blueprint->SimpleConstructionScript != nullptr)
            {
                // "Check to see if we have gained a component from our parent (that would require us
                // removing our scene root)" -- BlueprintEditor.cpp.
                Blueprint->SimpleConstructionScript->ValidateSceneRootNodes();
                bValidatedSceneRoot = true;
            }
        }

        FBlueprintEditorUtils::ConformCallsToParentFunctions(Blueprint);
        FBlueprintEditorUtils::ConformImplementedEvents(Blueprint);
        FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);
        FBlueprintEditorUtils::UpdateOutOfDateCompositeNodes(Blueprint);
        FBlueprintEditorUtils::UpdateTransactionalFlags(Blueprint);

        return bValidatedSceneRoot;
    }

    // Measured outcome of one ApplyReparentCore pass -- everything the response reports about the
    // reparent itself is taken from HERE, never assumed.
    struct FReparentCoreOutcome
    {
        bool  bSceneRootReconciledPre  = false;   // ValidateSceneRootNodes() ran BEFORE the compile
        bool  bSceneRootReconciledPost = false;   // ...and again AFTER it
        bool  bScsCreated              = false;
        bool  bSparseClassDataConformPrepared = false;
        int32 CompileErrors            = 0;
        int32 CompileWarnings          = 0;
    };

    // Reparent core WITH the delta-serialization compile flags: BPs with >10
    // type-pinned variables crash during reinstancing unless these two public UBlueprint transient
    // flags are set around the compile. Replicates HandleReparentBlueprint's data-side sequence so
    // this TU does not depend on GraphCommands -- plus the SCS-root reconciliation the editor does,
    // and an optional caller-owned results log.
    void ApplyReparentCore(UBlueprint* Blueprint, UClass* NewParent,
                           FCompilerResultsLog* OutResultsLog = nullptr,
                           bool bReconcileSceneRoot = true,
                           FReparentCoreOutcome* OutOutcome = nullptr)
    {
        FReparentCoreOutcome LocalOutcome;
        FReparentCoreOutcome& Outcome = OutOutcome ? *OutOutcome : LocalOutcome;

        Blueprint->Modify();
        if (Blueprint->SimpleConstructionScript)
        {
            Blueprint->SimpleConstructionScript->Modify();
            for (USCS_Node* Node : Blueprint->SimpleConstructionScript->GetAllNodes())
            {
                if (Node)
                {
                    Node->Modify();
                }
            }
        }

        Blueprint->ParentClass = NewParent;

        // --- (A) SCS-root reconciliation, the step FBlueprintEditor::ReparentBlueprint does at
        // BlueprintEditor.cpp. Without it the new parent's inherited
        // scene root and this Blueprint's own DefaultSceneRoot both survive into the compile and the
        // Kismet compiler emits "Internal Compiler Error: Tried to create a property DefaultSceneRoot".
        // UBlueprintEditorLibrary::ReparentBlueprint (BlueprintEditorLibrary.cpp) is the same
        // call; we invoke USimpleConstructionScript::ValidateSceneRootNodes() DIRECTLY (ENGINE_API,
        // SimpleConstructionScript.h) so no extra module dependency is introduced. ---
        if (bReconcileSceneRoot)
        {
            Outcome.bSceneRootReconciledPre = EnsureBlueprintIsUpToDate_Reparent(Blueprint, Outcome.bScsCreated);
        }

        FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
        FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

        // Changing the parent may change the sparse data used, so mark any current sparse data as
        // requiring a conform post-compile against the new archetype (BlueprintEditor.cpp).
        if (bReconcileSceneRoot)
        {
            if (UBlueprintGeneratedClass* GenClass = Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass))
            {
                GenClass->PrepareToConformSparseClassData(NewParent ? NewParent->GetSparseClassDataStruct() : nullptr);
                Outcome.bSparseClassDataConformPrepared = true;
            }
        }

        // --- The compile flags. UseDeltaSerializationDuringReinstancing /
        // SkipNewVariableDefaultsDetection are fields of FKismetCompilerOptions (Blueprint.h/154),
        // NOT settable members of UBlueprint, so they are reachable ONLY through these
        // EBlueprintCompileOptions flags -- matching UEAgentGraphCommands::HandleReparentBlueprint.
        // Without them the Kismet reinstancer takes the normal reference-replacement path and asserts
        // check(RefCount<=1) on BPs carrying type-pinned (object-class) variables with external refs. ---
        FCompilerResultsLog SilentLog;
        FCompilerResultsLog* Log = OutResultsLog;
        if (!Log)
        {
            SilentLog.SetSilentMode(true);
            Log = &SilentLog;
        }
        FKismetEditorUtilities::CompileBlueprint(
            Blueprint,
            EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing |
            EBlueprintCompileOptions::SkipNewVariableDefaultsDetection,
            Log);
        Outcome.CompileErrors   = Log->NumErrors;
        Outcome.CompileWarnings = Log->NumWarnings;

        // Post-compile pass, exactly as BlueprintEditor.cpp -- the compile can regenerate the SCS
        // against the new parent, so the root has to be validated a second time.
        if (bReconcileSceneRoot)
        {
            bool bUnusedScsCreated = false;
            Outcome.bSceneRootReconciledPost = EnsureBlueprintIsUpToDate_Reparent(Blueprint, bUnusedScsCreated);
            Outcome.bScsCreated = Outcome.bScsCreated || bUnusedScsCreated;
        }
    }
}

// MCP:reparent_blueprint - Change a Blueprint's parent class, guarded and rollback-capable; asking for the parent it already has is a no-op (reparented:false), not an error. On top of the reparent itself it takes: a pre-flight snapshot (parent chain, own-variable count, CDO component count); an OPTIONAL variable de-collision rename (rename_colliding_vars) for own variables whose name collides with a member the NEW parent already declares, which a plain reparent would otherwise clobber; the delta-serialization compile flags, without which a Blueprint with more than ~10 type-pinned variables CRASHES the editor during reinstancing; the same SCS-root reconciliation the Blueprint editor itself does, so a silently renamed component ("Mesh" -> "Mesh1") is reported instead of hidden; an invariant post-check (parent matches, no compile error, own-variable count did not shrink); and, on failure, an in-memory auto_rollback that restores the original parent, restores the component names and recompiles (auto_rollback=false leaves a failed reparent in place). It never saves to disk. This is a genuinely destructive multi-step transaction: run it with dry_run=true first on any real fork -- that reports the plan and the would-rename set without mutating anything.
//   @param blueprint_name - Blueprint asset name or path to reparent.
//   @param new_parent_class - New parent class name or path (native /Script, /Game/.../BP.BP_C, or a BP asset path -> its GeneratedClass).
//   @param dry_run - Optional (default false): analyze + report the plan (snapshot, would-rename vars, component collisions) without mutating.
//   @param rename_colliding_vars - Optional (default false): before reparenting, rename own NewVariables that collide by name with a member on the new parent.
//   @param auto_rollback - Optional (default true): on post-check failure, restore the original parent, restore the SCS node names and recompile.
//   @param reconcile_scene_root - Optional (default true): run the editor's own post-reparent conform pass -- ValidateSceneRootNodes + PrepareToConformSparseClassData + a second pass after the compile. Setting it FALSE disables all three; it exists only as a bisect escape hatch.
//   @param treat_component_renames_as_regression - Optional (default false): count a silently renamed SCS component as a post-check regression (which triggers auto_rollback).
//   @returns { blueprint_name, blueprint_path, old_parent, new_parent, dry_run, reparented, renamed_variables:[{from,to}], pre_variable_count, post_variable_count, pre_component_count, post_component_count, pre_scs_node_count, post_scs_node_count, scene_root_reconciled, scene_root_reconciled_post_compile, default_scene_root_removed, sparse_class_data_conform_prepared, compile_messages:[{severity,text}], compile_error_count, compile_warning_count, renamed_components:[{guid,from,to,matched_by}], removed_components:[{guid,name}], components_without_guid, colliding_components:[{name,own_class,parent_class}], child_only_components:[{name,class}], compile_status, parent_match, compile_ok, variables_intact, components_intact, post_check_ok, rolled_back, rollback_complete, rollback_restored_components:[{guid,name}] (snapshotted nodes whose name MATCHES again after the rollback), rollback_unrestored_components:[{guid,expected_name,actual_name}], rollback_compile_messages:[{severity,text}], regressions:[string], saved_to_disk (always false -- this tool never saves), persistence_note, verification, verification_checks, success }
//   NOTE (impl): the reparent core performs the SAME SCS-root reconciliation the Blueprint editor does
//   (ValidateSceneRootNodes + sparse-class-data conform + a second pass after the compile), captures
//   the compiler's own FCompilerResultsLog into compile_messages[], and diffs the SCS nodes BY GUID so
//   a silent component rename ("Mesh" -> "Mesh1") is reported instead of hidden -- and is really undone
//   (and re-verified) by the rollback.
TSharedPtr<FJsonObject> HandleReparentBlueprintSafe(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, NewParentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_parent_class"), NewParentName)) { return Missing; }

    UBlueprint* Blueprint = FindBlueprintFlexible_Reparent(BlueprintName);
    if (!Blueprint)
        return mcp_common::BlueprintNotFoundReply(BlueprintName);

    UClass* NewParent = FUEAgentGraphUtils::ResolveClass(NewParentName);
    if (!NewParent)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve new_parent_class: %s"), *NewParentName));

    bool bDryRun = false;            Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
    bool bRenameColliding = false;   Params->TryGetBoolField(TEXT("rename_colliding_vars"), bRenameColliding);
    bool bAutoRollback = true;       Params->TryGetBoolField(TEXT("auto_rollback"), bAutoRollback);
    // Default TRUE on purpose: it is the scene-root reconciliation the editor does. Turning it off
    // exists only to compare both behaviours, e.g. to bisect a problem.
    bool bReconcileSceneRoot = true; Params->TryGetBoolField(TEXT("reconcile_scene_root"), bReconcileSceneRoot);
    // Default FALSE keeps renames out of the post-check verdict. Renames are ALWAYS reported in
    // renamed_components[]; this only decides whether they also fail the post-check (and therefore
    // trigger auto_rollback). It is opt-in because a rename can be the legitimate consequence of a
    // name clash with an inherited component, and making it fatal by default would turn a reported
    // fact into a hard block.
    bool bRenamesAreRegression = false; Params->TryGetBoolField(TEXT("treat_component_renames_as_regression"), bRenamesAreRegression);

    UClass* OldParent = Blueprint->ParentClass;
    const FString OldParentName = OldParent ? OldParent->GetName() : TEXT("None");

    // --- Pre-flight snapshot ---
    const int32 PreVarCount   = Blueprint->NewVariables.Num();
    const int32 PreCompCount  = CountCdoComponents(Blueprint);

    // (C) SCS identity snapshot, taken BEFORE anything mutates. Keyed by VariableGuid.
    TArray<FScsNodeSnapshot_Reparent> ScsSnapshot;
    SnapshotScsNodes_Reparent(Blueprint, ScsSnapshot);
    const int32 PreScsNodeCount = Blueprint->SimpleConstructionScript
        ? Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0;

    const FName DefaultSceneRootName = USceneComponent::GetDefaultSceneRootVariableName();
    bool bHadDefaultSceneRootInTree = false;
    int32 SnapshotEntriesWithoutGuid = 0;
    for (const FScsNodeSnapshot_Reparent& Entry : ScsSnapshot)
    {
        if (Entry.bIsDefaultSceneRoot && Entry.bInAllNodes) { bHadDefaultSceneRootInTree = true; }
        if (!Entry.VariableGuid.IsValid())                  { ++SnapshotEntriesWithoutGuid; }
    }

    // (D) Component-surface REPORT (never a gate -- DefaultSceneRoot collides on essentially every
    // Actor reparent, so blocking on collision would refuse 100% of them; the report rejects that
    // explicitly). own SCS vs everything the NEW parent already brings (native CDO + SCS chain).
    TMap<FName, FString> OwnComponents;
    CollectOwnScsComponentNames_Reparent(Blueprint, OwnComponents);
    TMap<FName, FString> ParentComponents;
    CollectClassComponentNames_Reparent(NewParent, ParentComponents);

    TArray<TSharedPtr<FJsonValue>> CollidingComponentsJson;
    TArray<TSharedPtr<FJsonValue>> ChildOnlyComponentsJson;
    for (const TPair<FName, FString>& Own : OwnComponents)
    {
        if (const FString* ParentClassPath = ParentComponents.Find(Own.Key))
        {
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("name"), Own.Key.ToString());
            C->SetStringField(TEXT("own_class"), Own.Value);
            C->SetStringField(TEXT("parent_class"), *ParentClassPath);
            C->SetBoolField(TEXT("is_default_scene_root"), Own.Key == DefaultSceneRootName);
            CollidingComponentsJson.Add(MakeShared<FJsonValueObject>(C));
        }
        else
        {
            TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
            C->SetStringField(TEXT("name"), Own.Key.ToString());
            C->SetStringField(TEXT("class"), Own.Value);
            ChildOnlyComponentsJson.Add(MakeShared<FJsonValueObject>(C));
        }
    }

    // Identify own variables whose name collides with a member the NEW parent already declares.
    // A reparent would clobber those, so they are the de-collision rename candidates.
    TArray<FName> CollisionVarNames;
    for (const FBPVariableDescription& Var : Blueprint->NewVariables)
    {
        if (NewParent->FindPropertyByName(Var.VarName) != nullptr)
        {
            CollisionVarNames.Add(Var.VarName);
        }
    }

    TArray<TSharedPtr<FJsonValue>> RenamedVarsJson;

    // Idempotent early-out: already parented to the requested class.
    if (OldParent == NewParent)
    {
        TSharedPtr<FJsonObject> Result = StartReparentReply_Reparent(BlueprintName, *Blueprint, OldParentName, *NewParent);
        Result->SetBoolField(TEXT("dry_run"), bDryRun);
        Result->SetBoolField(TEXT("reparented"), false);
        Result->SetStringField(TEXT("note"), TEXT("Blueprint already has that parent class; no change made."));
        Result->SetBoolField(TEXT("post_check_ok"), true);
        Result->SetBoolField(TEXT("rolled_back"), false);
        Result->SetBoolField(TEXT("rollback_complete"), false);
        // Honest evidence -- nothing was written, so there is nothing to verify. This is
        // NotAttempted, NOT a fake "verified".
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotAttempted,
            TEXT("no-op: parent already matches"), TEXT("Blueprint->ParentClass == new_parent_class on entry; no write performed."));
        Result->SetBoolField(TEXT("success"), true);
        return Result;
    }

    if (bDryRun)
    {
        for (const FName& VarName : CollisionVarNames)
        {
            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("from"), VarName.ToString());
            R->SetStringField(TEXT("to"), FString::Printf(TEXT("%s_Fork"), *VarName.ToString()));
            RenamedVarsJson.Add(MakeShared<FJsonValueObject>(R));
        }

        TSharedPtr<FJsonObject> Result = StartReparentReply_Reparent(BlueprintName, *Blueprint, OldParentName, *NewParent);
        Result->SetBoolField(TEXT("dry_run"), true);
        Result->SetBoolField(TEXT("reparented"), false);
        Result->SetNumberField(TEXT("pre_variable_count"), PreVarCount);
        Result->SetNumberField(TEXT("pre_component_count"), PreCompCount);
        Result->SetNumberField(TEXT("pre_scs_node_count"), PreScsNodeCount);
        Result->SetArrayField(TEXT("would_rename_variables"), RenamedVarsJson);
        Result->SetBoolField(TEXT("rename_colliding_vars"), bRenameColliding);
        // (D) INFORMATIVE ONLY. A collision here does NOT block the reparent: DefaultSceneRoot
        // collides on virtually every Actor-to-Actor reparent, so gating on this would refuse the
        // very operation the tool exists for.
        Result->SetArrayField(TEXT("colliding_components"), CollidingComponentsJson);
        Result->SetArrayField(TEXT("child_only_components"), ChildOnlyComponentsJson);
        Result->SetNumberField(TEXT("parent_component_count"), ParentComponents.Num());
        Result->SetNumberField(TEXT("components_without_guid"), SnapshotEntriesWithoutGuid);
        Result->SetBoolField(TEXT("has_default_scene_root"), bHadDefaultSceneRootInTree);
        Result->SetStringField(TEXT("components_note"),
            TEXT("colliding_components is a REPORT, not a gate: DefaultSceneRoot collides on almost every ")
            TEXT("Actor reparent and the scene-root reconciliation is what resolves it."));
        mcp_common::AddVerificationEvidence(Result, mcp_common::EMcpVerification::NotAttempted,
            TEXT("dry_run"), TEXT("dry_run=true: analysis only, nothing was mutated, so nothing was verified."));
        Result->SetBoolField(TEXT("success"), true);
        return Result;
    }

    FScopedTransaction Transaction(LOCTEXT("ReparentSafe", "MCP: Safe Reparent Blueprint"));

    // --- Pre-flight de-collision rename (opt-in) ---
    if (bRenameColliding)
    {
        for (const FName& OldName : CollisionVarNames)
        {
            // Pick a unique replacement not present on the new parent nor as an existing own var.
            FName NewName;
            for (int32 Suffix = 0; Suffix < 1000; ++Suffix)
            {
                const FString Candidate = (Suffix == 0)
                    ? FString::Printf(TEXT("%s_Fork"), *OldName.ToString())
                    : FString::Printf(TEXT("%s_Fork%d"), *OldName.ToString(), Suffix);
                const FName CandName(*Candidate);
                const bool bCollidesParent = (NewParent->FindPropertyByName(CandName) != nullptr);
                const bool bCollidesOwn = (FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, CandName) != INDEX_NONE);
                if (!bCollidesParent && !bCollidesOwn)
                {
                    NewName = CandName;
                    break;
                }
            }
            if (NewName.IsNone())
            {
                continue; // could not find a free name; leave it (reparent will clobber, caught by post-check)
            }
            FBlueprintEditorUtils::RenameMemberVariable(Blueprint, OldName, NewName);

            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("from"), OldName.ToString());
            R->SetStringField(TEXT("to"), NewName.ToString());
            RenamedVarsJson.Add(MakeShared<FJsonValueObject>(R));
        }
    }

    // --- Reparent (core with the compile flags + SCS-root reconciliation) ---
    // (B) The compiler's OWN log is captured here, BEFORE any rollback recompile can overwrite it.
    // Not silent: the messages still reach the output log, exactly like compile_blueprint's
    // return_messages path, so the editor's Message Log stays a second source of truth.
    FCompilerResultsLog ReparentLog;
    FReparentCoreOutcome CoreOutcome;
    ApplyReparentCore(Blueprint, NewParent, &ReparentLog, bReconcileSceneRoot, &CoreOutcome);

    TArray<TSharedPtr<FJsonValue>> CompileMessagesJson;
    CompilerLogToJson_Reparent(ReparentLog, CompileMessagesJson);

    // --- Post-check invariants ---
    TArray<TSharedPtr<FJsonValue>> Regressions;

    const bool bParentMatch = (Blueprint->ParentClass == NewParent);
    if (!bParentMatch)
        Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("parent mismatch after reparent: is '%s', expected '%s'"),
            Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("None"), *NewParent->GetName())));

    const EBlueprintStatus Status = Blueprint->Status;
    // Two independent signals, both reported. EBlueprintStatus is the asset's persisted verdict;
    // the results log is what the compiler ACTUALLY emitted on this run. They can disagree (a compile
    // that errors and then gets marked dirty), so compile_ok requires BOTH to be clean instead of
    // trusting the enum alone.
    const bool bStatusOk = (Status != BS_Error);
    const bool bCompileOk = bStatusOk && (ReparentLog.NumErrors == 0);
    if (!bStatusOk)
        Regressions.Add(MakeShared<FJsonValueString>(TEXT("blueprint is in an error state after reparent")));
    if (ReparentLog.NumErrors > 0)
        Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("compiler reported %d error(s) during the reparent compile (see compile_messages)"), ReparentLog.NumErrors)));

    const int32 PostVarCount  = Blueprint->NewVariables.Num();
    const int32 PostCompCount = CountCdoComponents(Blueprint);
    // A reparent should NOT delete the BP's own NewVariables. A shrink is a genuine regression
    // (the clobber the safe wrapper exists to catch). A component-count change is EXPECTED across a
    // parent change, so it is reported but never treated as a regression.
    const bool bVariablesIntact = (PostVarCount >= PreVarCount);
    if (!bVariablesIntact)
        Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("own-variable count shrank after reparent: was %d, now %d"), PreVarCount, PostVarCount)));

    // (C) SCS diff BY GUID -- this is what turns the invisible "Mesh -> Mesh1" rename into data.
    TMap<FGuid, USCS_Node*> PostByGuid;
    TMap<FName, USCS_Node*> PostByName;
    TSet<FName>             PostInAllNodes;
    BuildScsLookup_Reparent(Blueprint, PostByGuid, PostByName, PostInAllNodes);

    TArray<TSharedPtr<FJsonValue>> RenamedComponentsJson;
    TArray<TSharedPtr<FJsonValue>> RemovedComponentsJson;
    int32 RenamedComponentCount = 0;
    int32 RemovedComponentCount = 0;
    bool  bDefaultSceneRootRemoved = false;

    for (const FScsNodeSnapshot_Reparent& Entry : ScsSnapshot)
    {
        bool bMatchedByGuid = false;
        USCS_Node* Now = ResolveSnapshotNode_Reparent(Entry, PostByGuid, PostByName, bMatchedByGuid);
        if (!Now)
        {
            // The DefaultSceneRoot leaving the tree is the EXPECTED, correct outcome of
            // ValidateSceneRootNodes when the new parent already provides a scene root
            // (SimpleConstructionScript.cpp) -- it is reported as its own field, never as
            // a lost component.
            if (Entry.bIsDefaultSceneRoot)
            {
                bDefaultSceneRootRemoved = bHadDefaultSceneRootInTree;
                continue;
            }
            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("guid"), Entry.VariableGuid.ToString());
            R->SetStringField(TEXT("name"), Entry.VariableName.ToString());
            RemovedComponentsJson.Add(MakeShared<FJsonValueObject>(R));
            ++RemovedComponentCount;
            continue;
        }

        if (Entry.bIsDefaultSceneRoot && Entry.bInAllNodes && !PostInAllNodes.Contains(Now->GetVariableName()))
        {
            // Still reachable through GetDefaultSceneRootNode(), but pulled out of the tree.
            bDefaultSceneRootRemoved = true;
        }

        const FName NowName = Now->GetVariableName();
        if (NowName != Entry.VariableName)
        {
            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("guid"), Entry.VariableGuid.ToString());
            R->SetStringField(TEXT("from"), Entry.VariableName.ToString());
            R->SetStringField(TEXT("to"), NowName.ToString());
            R->SetStringField(TEXT("matched_by"), bMatchedByGuid ? TEXT("guid") : TEXT("name"));
            RenamedComponentsJson.Add(MakeShared<FJsonValueObject>(R));
            ++RenamedComponentCount;
        }
    }

    // A removed (non-DefaultSceneRoot) SCS node is ALWAYS a regression -- that is component loss.
    // A rename is always reported; whether it also fails the post-check is the caller's call.
    if (RemovedComponentCount > 0)
        Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%d SCS component(s) disappeared after reparent (see removed_components)"), RemovedComponentCount)));
    if (RenamedComponentCount > 0 && bRenamesAreRegression)
        Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
            TEXT("%d SCS component(s) were silently renamed (see renamed_components)"), RenamedComponentCount)));

    const bool bComponentsIntact = (RemovedComponentCount == 0) && (!bRenamesAreRegression || RenamedComponentCount == 0);
    const bool bPostCheckOk = bParentMatch && bCompileOk && bVariablesIntact && bComponentsIntact;

    // --- Auto rollback on failure ---
    // Restore the parent, restore every snapshotted SCS name by GUID, recompile, and RE-READ the
    // result -> rollback_complete reflects the measured state, not the attempt.
    bool bRolledBack = false;
    bool bRollbackComplete = false;
    TArray<TSharedPtr<FJsonValue>> RollbackRestoredJson;
    TArray<TSharedPtr<FJsonValue>> RollbackUnrestoredJson;
    TArray<TSharedPtr<FJsonValue>> RollbackCompileMessagesJson;
    if (!bPostCheckOk && bAutoRollback && OldParent)
    {
        FCompilerResultsLog RollbackLog;
        FReparentCoreOutcome RollbackOutcome;
        ApplyReparentCore(Blueprint, OldParent, &RollbackLog, bReconcileSceneRoot, &RollbackOutcome);
        bRolledBack = true;

        // Restore the SCS variable names the reparent moved. bRenameTemplate=true keeps the component
        // TEMPLATE object's name in sync with the variable (SCS_Node.cpp) -- without it the
        // archetype stays under the renamed name and instances stop routing back to it.
        TMap<FGuid, USCS_Node*> RbByGuid; TMap<FName, USCS_Node*> RbByName; TSet<FName> RbInAllNodes;
        BuildScsLookup_Reparent(Blueprint, RbByGuid, RbByName, RbInAllNodes);
        // LiveNames tracks the names ACTUALLY in use as we rename, so the collision guard below does
        // not consult a map that our own renames have already invalidated.
        TSet<FName> LiveNames;
        for (const TPair<FName, USCS_Node*>& Pair : RbByName) { LiveNames.Add(Pair.Key); }

        bool bAnyRenameApplied = false;
        for (const FScsNodeSnapshot_Reparent& Entry : ScsSnapshot)
        {
            bool bMatchedByGuid = false;
            USCS_Node* Now = ResolveSnapshotNode_Reparent(Entry, RbByGuid, RbByName, bMatchedByGuid);
            if (!Now)
            {
                continue;
            }
            const FName CurrentName = Now->GetVariableName();
            if (CurrentName == Entry.VariableName)
            {
                continue;
            }
            // Do not steal a name another node currently holds -- that would trade one corruption
            // for another. Report it (the re-verify below records it as unrestored).
            if (LiveNames.Contains(Entry.VariableName))
            {
                continue;
            }
            Now->Modify();
            Now->SetVariableName(Entry.VariableName, /*bRenameTemplate=*/true);
            LiveNames.Remove(CurrentName);
            LiveNames.Add(Entry.VariableName);
            bAnyRenameApplied = true;
        }

        // Renaming after the compile leaves the generated class stale, so recompile once more and
        // keep THAT log as the rollback's compile evidence. Two distinct log objects (never an
        // assignment onto a live one -- FCompilerResultsLog owns an event scope).
        FCompilerResultsLog RollbackRecompileLog;
        const FCompilerResultsLog* EffectiveRollbackLog = &RollbackLog;
        if (bAnyRenameApplied)
        {
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FKismetEditorUtilities::CompileBlueprint(
                Blueprint,
                EBlueprintCompileOptions::UseDeltaSerializationDuringReinstancing |
                EBlueprintCompileOptions::SkipNewVariableDefaultsDetection,
                &RollbackRecompileLog);
            EffectiveRollbackLog = &RollbackRecompileLog;
        }
        CompilerLogToJson_Reparent(*EffectiveRollbackLog, RollbackCompileMessagesJson);
        const int32 RollbackCompileErrors = EffectiveRollbackLog->NumErrors;

        // --- RE-VERIFY the rollback against the snapshot. No bool is set from intent. ---
        TMap<FGuid, USCS_Node*> VerByGuid; TMap<FName, USCS_Node*> VerByName; TSet<FName> VerInAllNodes;
        BuildScsLookup_Reparent(Blueprint, VerByGuid, VerByName, VerInAllNodes);
        bool bAllNamesRestored = true;
        for (const FScsNodeSnapshot_Reparent& Entry : ScsSnapshot)
        {
            bool bMatchedByGuid = false;
            USCS_Node* Now = ResolveSnapshotNode_Reparent(Entry, VerByGuid, VerByName, bMatchedByGuid);
            const FName ActualName = Now ? Now->GetVariableName() : FName(NAME_None);
            if (Now && ActualName == Entry.VariableName)
            {
                TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
                R->SetStringField(TEXT("guid"), Entry.VariableGuid.ToString());
                R->SetStringField(TEXT("name"), Entry.VariableName.ToString());
                RollbackRestoredJson.Add(MakeShared<FJsonValueObject>(R));
                continue;
            }
            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("guid"), Entry.VariableGuid.ToString());
            R->SetStringField(TEXT("expected_name"), Entry.VariableName.ToString());
            R->SetStringField(TEXT("actual_name"), Now ? ActualName.ToString() : FString(TEXT("<missing>")));
            RollbackUnrestoredJson.Add(MakeShared<FJsonValueObject>(R));
            bAllNamesRestored = false;
        }

        const bool bParentRestored = (Blueprint->ParentClass == OldParent);
        bRollbackComplete = bParentRestored && bAllNamesRestored && (RollbackCompileErrors == 0);
        if (!bParentRestored)
            Regressions.Add(MakeShared<FJsonValueString>(TEXT("rollback did NOT restore the original parent class")));
        if (!bAllNamesRestored)
            Regressions.Add(MakeShared<FJsonValueString>(TEXT("rollback did NOT restore every SCS component name (see rollback_unrestored_components)")));
        if (RollbackCompileErrors > 0)
            Regressions.Add(MakeShared<FJsonValueString>(FString::Printf(
                TEXT("rollback recompile reported %d error(s) (see rollback_compile_messages)"), RollbackCompileErrors)));
    }

    const bool bSuccess = bPostCheckOk;

    TSharedPtr<FJsonObject> Result = StartReparentReply_Reparent(BlueprintName, *Blueprint, OldParentName, *NewParent);
    Result->SetBoolField(TEXT("dry_run"), false);
    Result->SetBoolField(TEXT("reparented"), bParentMatch && !bRolledBack);
    Result->SetArrayField(TEXT("renamed_variables"), RenamedVarsJson);
    Result->SetNumberField(TEXT("pre_variable_count"), PreVarCount);
    Result->SetNumberField(TEXT("post_variable_count"), PostVarCount);
    Result->SetNumberField(TEXT("pre_component_count"), PreCompCount);
    Result->SetNumberField(TEXT("post_component_count"), PostCompCount);
    Result->SetNumberField(TEXT("pre_scs_node_count"), PreScsNodeCount);
    Result->SetNumberField(TEXT("post_scs_node_count"),
        Blueprint->SimpleConstructionScript ? Blueprint->SimpleConstructionScript->GetAllNodes().Num() : 0);
    // (A) measured, not assumed: these come back from ApplyReparentCore's outcome struct.
    Result->SetBoolField(TEXT("scene_root_reconciled"), CoreOutcome.bSceneRootReconciledPre);
    Result->SetBoolField(TEXT("scene_root_reconciled_post_compile"), CoreOutcome.bSceneRootReconciledPost);
    Result->SetBoolField(TEXT("default_scene_root_removed"), bDefaultSceneRootRemoved);
    Result->SetBoolField(TEXT("sparse_class_data_conform_prepared"), CoreOutcome.bSparseClassDataConformPrepared);
    Result->SetBoolField(TEXT("scs_created"), CoreOutcome.bScsCreated);
    // (B)
    Result->SetArrayField(TEXT("compile_messages"), CompileMessagesJson);
    Result->SetNumberField(TEXT("compile_error_count"), ReparentLog.NumErrors);
    Result->SetNumberField(TEXT("compile_warning_count"), ReparentLog.NumWarnings);
    // (C)
    Result->SetArrayField(TEXT("renamed_components"), RenamedComponentsJson);
    Result->SetArrayField(TEXT("removed_components"), RemovedComponentsJson);
    Result->SetNumberField(TEXT("components_without_guid"), SnapshotEntriesWithoutGuid);
    // (D) report-only
    Result->SetArrayField(TEXT("colliding_components"), CollidingComponentsJson);
    Result->SetArrayField(TEXT("child_only_components"), ChildOnlyComponentsJson);
    Result->SetStringField(TEXT("compile_status"), BlueprintStatusToString(Blueprint->Status));
    Result->SetBoolField(TEXT("parent_match"), bParentMatch);
    Result->SetBoolField(TEXT("compile_ok"), bCompileOk);
    Result->SetBoolField(TEXT("variables_intact"), bVariablesIntact);
    Result->SetBoolField(TEXT("components_intact"), bComponentsIntact);
    Result->SetBoolField(TEXT("post_check_ok"), bPostCheckOk);
    Result->SetBoolField(TEXT("rolled_back"), bRolledBack);
    Result->SetBoolField(TEXT("rollback_complete"), bRollbackComplete);
    Result->SetArrayField(TEXT("rollback_restored_components"), RollbackRestoredJson);
    Result->SetArrayField(TEXT("rollback_unrestored_components"), RollbackUnrestoredJson);
    Result->SetArrayField(TEXT("rollback_compile_messages"), RollbackCompileMessagesJson);
    Result->SetArrayField(TEXT("regressions"), Regressions);

    // The ONE effect this command genuinely cannot observe gets its own explicit field instead
    // of a fake check: the tool never saves, so nothing here says anything about disk. Stated as a
    // field (per the honesty rule) rather than folded into the verification aggregate, which would
    // have made every clean reparent read as "not_verifiable".
    Result->SetBoolField(TEXT("saved_to_disk"), false);
    Result->SetStringField(TEXT("persistence_note"),
        TEXT("reparent_blueprint mutates IN MEMORY only and leaves the package dirty. ")
        TEXT("Call save_asset afterwards and check ITS outcome; nothing in this response is evidence about disk."));

    // --- Honest verification evidence (the effect was RE-READ, not assumed) ---
    mcp_common::AddVerificationEvidence(Result,
        bParentMatch ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("re-read Blueprint->ParentClass"),
        FString::Printf(TEXT("after reparent ParentClass='%s'"),
            Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("None")));
    mcp_common::AddVerificationEvidence(Result,
        (ReparentLog.NumErrors == 0) ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("FCompilerResultsLog from the reparent compile"),
        FString::Printf(TEXT("%d error(s), %d warning(s)"), ReparentLog.NumErrors, ReparentLog.NumWarnings));
    mcp_common::AddVerificationEvidence(Result,
        (RemovedComponentCount == 0 && RenamedComponentCount == 0)
            ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("SCS node diff by VariableGuid"),
        FString::Printf(TEXT("%d renamed, %d removed, %d node(s) had no guid (matched by name)"),
            RenamedComponentCount, RemovedComponentCount, SnapshotEntriesWithoutGuid));
    if (bRolledBack)
    {
        mcp_common::AddVerificationEvidence(Result,
            bRollbackComplete ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
            TEXT("re-read parent + SCS names after rollback"),
            // Precise wording: these counts are "matches the pre-flight snapshot" / "does not",
            // measured AFTER the rollback -- not "how many renames we issued".
            FString::Printf(TEXT("%d snapshotted component name(s) match again, %d do not; parent restored=%s"),
                RollbackRestoredJson.Num(), RollbackUnrestoredJson.Num(),
                (Blueprint->ParentClass == OldParent) ? TEXT("true") : TEXT("false")));
    }
    Result->SetBoolField(TEXT("success"), bSuccess);
    return Result;
}

// MCP:reconcile_function_signature - Repair a LOCAL function graph's parameter pins so they match a parent UFUNCTION -- for an override whose signature went stale after a reparent -- WITHOUT nuking the graph's downstream nodes, which is what re-creating the override by hand costs you. It diffs the parent's parameters against the function's entry/result pins: mode='add_missing' (the default) only ADDS the missing pins, so it is non-destructive and safe to run blind; mode='remove_extra' / 'mirror' also DROP pins the parent lacks, which can sever downstream connections, and are therefore gated behind force_remove=true -- without it the extra pins are merely REPORTED (partially_implemented=true).
//   @param blueprint_name - Blueprint asset name or path owning the function.
//   @param function_name - Name of the local function graph to reconcile.
//   @param parent_class - Optional: class whose UFUNCTION defines the target signature (default: the Blueprint's current ParentClass).
//   @param mode - Optional (default 'add_missing'): 'add_missing' | 'remove_extra' | 'mirror'.
//   @param force_remove - Optional (default false): actually remove extra pins for 'remove_extra'/'mirror' (may drop connections).
//   @returns { blueprint_name, function_name, parent_class, mode, added:[{name,direction}], removed:[{name,direction}], extra_pins:[{name,direction}], missing_pins:[{name,direction}], removal_performed, partially_implemented, note, success }
//   NOTE (impl): the parent's parameters are classified input / output / return via CPF_ReturnParm and
//   CPF_OutParm & !CPF_ReferenceParm, converted to pin types, and diffed against the function's
//   UK2Node_FunctionEntry (inputs) and UK2Node_FunctionResult (outputs); adding ReconstructNode()s the
//   affected node.
//   WARNING: the diff reads two DIFFERENT sets on purpose. `missing_pins`/`added` are measured against the
//   node's REAL pins (Pins, filtered by direction -- inverted on a terminator -- minus exec and hidden
//   ones), because on an INTERFACE OVERRIDE the parameter pins come from the interface signature and
//   are absent from UserDefinedPins: reading UserDefinedPins there would report every parameter as
//   missing and each run would mint a numbered phantom (`Required1`, then `Required2`). `extra_pins`/`removed` do
//   read UserDefinedPins, because only a user-defined pin can be removed at all. Pin creation passes
//   bUseUniqueName=false so a colliding name is refused instead of silently renamed.
TSharedPtr<FJsonObject> HandleReconcileFunctionSignature(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName)) { return Missing; }

    FString Mode = TEXT("add_missing");
    Params->TryGetStringField(TEXT("mode"), Mode);
    Mode = Mode.ToLower();
    if (Mode != TEXT("add_missing") && Mode != TEXT("remove_extra") && Mode != TEXT("mirror"))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid mode '%s' (expected add_missing|remove_extra|mirror)"), *Mode));

    bool bForceRemove = false;
    Params->TryGetBoolField(TEXT("force_remove"), bForceRemove);

    UBlueprint* Blueprint = FindBlueprintFlexible_Reparent(BlueprintName);
    if (!Blueprint)
        return mcp_common::BlueprintNotFoundReply(BlueprintName);

    UEdGraph* FuncGraph = FindFunctionGraphLocal(Blueprint, FunctionName);
    if (!FuncGraph)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function graph not found: %s"), *FunctionName));

    // Resolve the parent UFUNCTION that defines the target signature.
    UClass* ParentClass = nullptr;
    FString ParentClassOverride;
    if (Params->TryGetStringField(TEXT("parent_class"), ParentClassOverride) && !ParentClassOverride.IsEmpty())
    {
        ParentClass = FUEAgentGraphUtils::ResolveClass(ParentClassOverride);
        if (!ParentClass)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve parent_class: %s"), *ParentClassOverride));
    }
    else
    {
        ParentClass = Blueprint->ParentClass;
    }
    if (!ParentClass)
        return mcp_common::MakeErrorReply(TEXT("Blueprint has no parent class to reconcile against"));

    UFunction* ParentFunc = ParentClass->FindFunctionByName(FName(*FunctionName));
    if (!ParentFunc)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Parent class '%s' has no function named '%s' to reconcile against"), *ParentClass->GetName(), *FunctionName));

    // Locate the function Entry (and, lazily, Result) nodes.
    TArray<UK2Node_FunctionEntry*> EntryNodes;
    FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
    if (EntryNodes.Num() == 0)
        return mcp_common::MakeErrorReply(TEXT("Function has no entry node"));
    UK2Node_FunctionEntry* EntryNode = EntryNodes[0];

    TArray<UK2Node_FunctionResult*> ResultNodes;
    FuncGraph->GetNodesOfClass<UK2Node_FunctionResult>(ResultNodes);
    UK2Node_FunctionResult* ResultNode = ResultNodes.Num() > 0 ? ResultNodes[0] : nullptr;

    const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

    // --- Build the DESIRED signature from the parent function's parameters. ---
    struct FDesiredParam { FName Name; FEdGraphPinType PinType; bool bOutput; };
    TArray<FDesiredParam> DesiredInputs;
    TArray<FDesiredParam> DesiredOutputs;
    for (TFieldIterator<FProperty> It(ParentFunc); It && (It->PropertyFlags & CPF_Parm); ++It)
    {
        FProperty* Prop = *It;
        const bool bReturn = Prop->HasAnyPropertyFlags(CPF_ReturnParm);
        const bool bPureOut = Prop->HasAnyPropertyFlags(CPF_OutParm) && !Prop->HasAnyPropertyFlags(CPF_ReferenceParm);
        const bool bOutput = bReturn || bPureOut;

        FEdGraphPinType PinType;
        if (!K2Schema->ConvertPropertyToPinType(Prop, PinType))
        {
            continue; // skip params whose type cannot be represented as a pin
        }
        FDesiredParam D; D.Name = Prop->GetFName(); D.PinType = PinType; D.bOutput = bOutput;
        if (bOutput) { DesiredOutputs.Add(D); } else { DesiredInputs.Add(D); }
    }

    // --- What is PRESENT vs what is USER-DEFINED: two different questions, two pairs of sets. ---
    //
    // An interface override's parameter pins come from the interface UFunction's signature, so
    // they exist in EntryNode->Pins but are NOT in UserDefinedPins. Reading UserDefinedPins alone,
    // every parameter would look missing, and re-adding it would collide with the pin already
    // there and coin a numbered phantom (`Required1`) on a PUBLIC interface.
    //
    // PRESENT (drives missing/add): read the REAL pins, filtered by direction -- on a terminator node
    // the direction is INVERTED with respect to the parameter (a function INPUT is an output pin on
    // the Entry node, a function OUTPUT is an input pin on the Result node). Skip the exec pin and
    // hidden pins (WorldContext and friends), which are not parameters.
    auto CollectPresent = [](const UK2Node_EditablePinBase* Node, EEdGraphPinDirection PinDir, TSet<FName>& Out)
    {
        if (!Node) { return; }
        for (const UEdGraphPin* P : Node->Pins)
        {
            if (!P || P->Direction != PinDir) { continue; }
            if (P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) { continue; }
            if (P->bHidden) { continue; }
            Out.Add(P->PinName);
        }
    };
    TSet<FName> PresentInputNames;
    CollectPresent(EntryNode, EGPD_Output, PresentInputNames);
    TSet<FName> PresentOutputNames;
    CollectPresent(ResultNode, EGPD_Input, PresentOutputNames);

    // USER-DEFINED (drives extra/remove): only a user-defined pin CAN be removed, so the removal side
    // must keep looking here. Reporting an inherited signature pin as "extra" and trying to delete it
    // would be the same mistake in the opposite direction.
    TSet<FName> ExistingInputNames;
    for (const TSharedPtr<FUserPinInfo>& Info : EntryNode->UserDefinedPins)
    {
        if (Info.IsValid()) { ExistingInputNames.Add(Info->PinName); }
    }
    TSet<FName> ExistingOutputNames;
    if (ResultNode)
    {
        for (const TSharedPtr<FUserPinInfo>& Info : ResultNode->UserDefinedPins)
        {
            if (Info.IsValid()) { ExistingOutputNames.Add(Info->PinName); }
        }
    }

    TSet<FName> DesiredInputNames;  for (const FDesiredParam& D : DesiredInputs)  { DesiredInputNames.Add(D.Name); }
    TSet<FName> DesiredOutputNames; for (const FDesiredParam& D : DesiredOutputs) { DesiredOutputNames.Add(D.Name); }

    // Diff lists for reporting.
    TArray<TSharedPtr<FJsonValue>> MissingJson;
    TArray<TSharedPtr<FJsonValue>> ExtraJson;
    auto MakePinEntry = [](const FString& InName, const TCHAR* Dir) -> TSharedPtr<FJsonValue>
    {
        TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
        O->SetStringField(TEXT("name"), InName);
        O->SetStringField(TEXT("direction"), Dir);
        return MakeShared<FJsonValueObject>(O);
    };

    // "Missing" is measured against what is PRESENT on the node, never against UserDefinedPins.
    for (const FDesiredParam& D : DesiredInputs)
        if (!PresentInputNames.Contains(D.Name)) { MissingJson.Add(MakePinEntry(D.Name.ToString(), TEXT("input"))); }
    for (const FDesiredParam& D : DesiredOutputs)
        if (!PresentOutputNames.Contains(D.Name)) { MissingJson.Add(MakePinEntry(D.Name.ToString(), TEXT("output"))); }

    for (const FName& N : ExistingInputNames)
        if (!DesiredInputNames.Contains(N)) { ExtraJson.Add(MakePinEntry(N.ToString(), TEXT("input"))); }
    if (ResultNode)
    {
        for (const FName& N : ExistingOutputNames)
            if (!DesiredOutputNames.Contains(N)) { ExtraJson.Add(MakePinEntry(N.ToString(), TEXT("output"))); }
    }

    FScopedTransaction Transaction(LOCTEXT("ReconcileFnSig", "MCP: Reconcile Function Signature"));
    Blueprint->Modify();

    TArray<TSharedPtr<FJsonValue>> AddedJson;
    TArray<TSharedPtr<FJsonValue>> RemovedJson;
    bool bEntryModified = false;
    bool bResultModified = false;

    // --- ADD missing pins (add_missing + mirror). ---
    if (Mode == TEXT("add_missing") || Mode == TEXT("mirror"))
    {
        for (const FDesiredParam& D : DesiredInputs)
        {
            if (PresentInputNames.Contains(D.Name)) { continue; }
            EntryNode->Modify();
            // A function INPUT is an OUTPUT pin on the entry node (direction inverted).
            // bUseUniqueName=false ON PURPOSE: with true, a name that is already taken gets a numeric
            // suffix (K2Node_FunctionTerminator::CreateUniquePinName), which is precisely how a
            // phantom `Required1` / `Required2` gets minted. Refusing to create the duplicate is the
            // honest outcome -- the pin we would be "adding" is already there.
            if (EntryNode->CreateUserDefinedPin(D.Name, D.PinType, EGPD_Output, /*bUseUniqueName=*/false))
            {
                bEntryModified = true;
                AddedJson.Add(MakePinEntry(D.Name.ToString(), TEXT("input")));
            }
        }

        if (DesiredOutputs.Num() > 0)
        {
            if (!ResultNode)
            {
                ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
            }
            if (ResultNode)
            {
                // The Result node may have just been created above, in which case the pins it already
                // carries were never sampled. Re-read before diffing so a fresh node is not diffed
                // against a stale empty set.
                if (PresentOutputNames.Num() == 0) { CollectPresent(ResultNode, EGPD_Input, PresentOutputNames); }

                for (const FDesiredParam& D : DesiredOutputs)
                {
                    if (PresentOutputNames.Contains(D.Name)) { continue; }
                    ResultNode->Modify();
                    // A function OUTPUT/return is an INPUT pin on the result node (direction inverted).
                    // bUseUniqueName=false: see the note on the entry-node call above.
                    if (ResultNode->CreateUserDefinedPin(D.Name, D.PinType, EGPD_Input, /*bUseUniqueName=*/false))
                    {
                        bResultModified = true;
                        AddedJson.Add(MakePinEntry(D.Name.ToString(), TEXT("output")));
                    }
                }
            }
        }
    }

    // --- REMOVE extra pins (remove_extra + mirror), opt-in behind force_remove. ---
    bool bRemovalPerformed = false;
    bool bPartiallyImplemented = false;
    if (Mode == TEXT("remove_extra") || Mode == TEXT("mirror"))
    {
        if (bForceRemove)
        {
            for (const FName& N : ExistingInputNames)
            {
                if (DesiredInputNames.Contains(N)) { continue; }
                EntryNode->Modify();
                EntryNode->RemoveUserDefinedPinByName(N);
                bEntryModified = true;
                bRemovalPerformed = true;
                RemovedJson.Add(MakePinEntry(N.ToString(), TEXT("input")));
            }
            if (ResultNode)
            {
                for (const FName& N : ExistingOutputNames)
                {
                    if (DesiredOutputNames.Contains(N)) { continue; }
                    ResultNode->Modify();
                    ResultNode->RemoveUserDefinedPinByName(N);
                    bResultModified = true;
                    bRemovalPerformed = true;
                    RemovedJson.Add(MakePinEntry(N.ToString(), TEXT("output")));
                }
            }
        }
        else if (ExtraJson.Num() > 0)
        {
            // Extra pins detected but not removed (force_remove=false): report as partial.
            bPartiallyImplemented = true;
        }
    }

    if (bEntryModified) { EntryNode->ReconstructNode(); }
    if (bResultModified && ResultNode) { ResultNode->ReconstructNode(); }
    if (bEntryModified || bResultModified)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    }

    FString Note;
    if (bPartiallyImplemented)
    {
        Note = TEXT("Extra pins were detected but NOT removed (pass force_remove=true to remove them; ")
               TEXT("removal may sever downstream connections). Additions, if any, were applied.");
    }
    else if (Mode == TEXT("add_missing"))
    {
        Note = TEXT("add_missing: created any missing parameter pins; extra pins (if any) left untouched.");
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetStringField(TEXT("function_name"), FunctionName);
    Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
    Result->SetStringField(TEXT("mode"), Mode);
    Result->SetArrayField(TEXT("added"), AddedJson);
    Result->SetArrayField(TEXT("removed"), RemovedJson);
    Result->SetArrayField(TEXT("extra_pins"), ExtraJson);
    Result->SetArrayField(TEXT("missing_pins"), MissingJson);
    Result->SetBoolField(TEXT("removal_performed"), bRemovalPerformed);
    Result->SetBoolField(TEXT("partially_implemented"), bPartiallyImplemented);
    if (!Note.IsEmpty()) { Result->SetStringField(TEXT("note"), Note); }
    Result->SetBoolField(TEXT("success"), true);
    return Result;
}

// MCP:fork_class_chain - One-call thin-fork + optional reparent + verify: collapses the create->reparent->verify pattern into a single call. It creates a NEW thin child Blueprint of base_class at fork_name (the seam you insert between a shared-library base and your own class), optionally tags it with the fork-origin metadata tag (project setting ForkOriginTag), saves it, and then -- only if reparent_target is given -- reparents that existing Blueprint onto the new fork through the guarded reparent_blueprint path (snapshot + compile flags + post-check + rollback) and reports its verification. WARNING: no durable backup is taken -- the rollback is in-memory only, so call backup_asset yourself first if you want a rollback-to-disk safety net.
//   @param fork_name - Name or full /Game path for the NEW thin child Blueprint to create.
//   @param base_class - Parent class for the new fork (native /Script, /Game/.../BP.BP_C, or a BP asset path).
//   @param path - Optional folder for fork_name when it is a bare name (default /Game/Blueprints).
//   @param fork_origin - Optional: value stored in the fork's fork-origin metadata tag (project setting ForkOriginTag, default 'mcp.forkOrigin'), recording its fork lineage.
//   @param reparent_target - Optional: an existing Blueprint to reparent onto the new fork.
//   @param auto_rollback - Optional (default true): forwarded to the safe reparent of reparent_target.
//   @param rename_colliding_vars - Optional (default false): forwarded to the safe reparent of reparent_target.
//   @returns { fork_name, fork_path, fork_generated_class, base_class, already_exists, fork_origin_set, saved, save_error?, reparent:{...safe-reparent result...}|null, verification, verification_checks, error?, success }
//   NOTE: `success` is the AND of (package actually saved) AND (the nested reparent passed its own
//   post-check).
TSharedPtr<FJsonObject> HandleForkClassChain(const TSharedPtr<FJsonObject>& Params)
{
    FString ForkName, BaseClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("fork_name"), ForkName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("base_class"), BaseClassName)) { return Missing; }

    UClass* BaseClass = FUEAgentGraphUtils::ResolveClass(BaseClassName);
    if (!BaseClass)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve base_class: %s"), *BaseClassName));
    if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(BaseClass))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class cannot be a Blueprint parent: %s"), *BaseClass->GetPathName()));

    // --- Resolve the fork package path (mirrors create_blueprint's resolution). ---
    FString PackageName;
    if (ForkName.StartsWith(TEXT("/")))
    {
        PackageName = FPackageName::ObjectPathToPackageName(ForkName);
    }
    else
    {
        FString Folder;
        Params->TryGetStringField(TEXT("path"), Folder);
        if (Folder.IsEmpty()) { Folder = TEXT("/Game/Blueprints"); }
        Folder.RemoveFromEnd(TEXT("/"));
        PackageName = Folder / ForkName;
    }
    if (!FPackageName::IsValidLongPackageName(PackageName))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Invalid fork package path from fork_name='%s': '%s'"), *ForkName, *PackageName));
    const FString AssetName = FPackageName::GetShortName(PackageName);

    // --- Create (or reuse) the thin child Blueprint. ---
    if (const FString AssetConflict = mcp_common::AssetNameConflict(PackageName, AssetName, UBlueprint::StaticClass()); !AssetConflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(AssetConflict);
    }
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
    Package->FullyLoad();

    UBlueprint* Fork = FindObject<UBlueprint>(Package, *AssetName);
    const bool bAlreadyExists = (Fork != nullptr);
    if (!Fork)
    {
        Fork = Cast<UBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            BaseClass, Package, FName(*AssetName), BPTYPE_Normal,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("ForkClassChainMCP"))));
        if (!Fork)
            return mcp_common::MakeErrorReply(TEXT("Failed to create fork blueprint"));
        FAssetRegistryModule::AssetCreated(Fork);
        Fork->MarkPackageDirty();
    }

    // --- Optional fork-origin metadata tag. ---
    bool bForkOriginSet = false;
    FString ForkOrigin;
    if (Params->TryGetStringField(TEXT("fork_origin"), ForkOrigin) && !ForkOrigin.IsEmpty())
    {
        UEditorAssetLibrary::SetMetadataTag(Fork, GetDefault<UUEAgentProjectSettings>()->GetForkOriginTag(), ForkOrigin);
        bForkOriginSet = true;
    }

    // --- Save the fork. ---
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, Fork, SaveError);

    const FString ForkGeneratedClass = Fork->GeneratedClass ? Fork->GeneratedClass->GetPathName() : FString();

    // --- Optional: reparent an existing target onto the new fork, via the guarded path. ---
    TSharedPtr<FJsonObject> ReparentResult;
    FString ReparentTarget;
    if (Params->TryGetStringField(TEXT("reparent_target"), ReparentTarget) && !ReparentTarget.IsEmpty())
    {
        // Prefer the fully-qualified generated class path so ResolveClass is unambiguous;
        // fall back to the fork's package path (ResolveClass -> its GeneratedClass).
        const FString NewParentForTarget = !ForkGeneratedClass.IsEmpty() ? ForkGeneratedClass : PackageName;

        TSharedPtr<FJsonObject> SubParams = MakeShared<FJsonObject>();
        SubParams->SetStringField(TEXT("blueprint_name"), ReparentTarget);
        SubParams->SetStringField(TEXT("new_parent_class"), NewParentForTarget);
        bool bAutoRollback = true;      Params->TryGetBoolField(TEXT("auto_rollback"), bAutoRollback);
        bool bRenameColliding = false;  Params->TryGetBoolField(TEXT("rename_colliding_vars"), bRenameColliding);
        SubParams->SetBoolField(TEXT("auto_rollback"), bAutoRollback);
        SubParams->SetBoolField(TEXT("rename_colliding_vars"), bRenameColliding);

        ReparentResult = HandleReparentBlueprintSafe(SubParams);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("fork_name"), Fork->GetName());
    Result->SetStringField(TEXT("fork_path"), Fork->GetPathName());
    Result->SetStringField(TEXT("fork_generated_class"), ForkGeneratedClass);
    Result->SetStringField(TEXT("base_class"), BaseClass->GetPathName());
    Result->SetBoolField(TEXT("already_exists"), bAlreadyExists);
    Result->SetBoolField(TEXT("fork_origin_set"), bForkOriginSet);
    Result->SetBoolField(TEXT("saved"), bSaved);
    if (!bSaved && !SaveError.IsEmpty()) { Result->SetStringField(TEXT("save_error"), SaveError); }
    bool bReparentOk = true;
    if (ReparentResult.IsValid())
    {
        Result->SetObjectField(TEXT("reparent"), ReparentResult);
        ReparentResult->TryGetBoolField(TEXT("success"), bReparentOk);
    }
    else
    {
        Result->SetField(TEXT("reparent"), MakeShared<FJsonValueNull>());
    }

    // The outcome is the AND of the two measured steps: the save, and the guarded reparent (which
    // may have rolled back).
    const bool bOverallSuccess = bSaved && bReparentOk;
    mcp_common::AddVerificationEvidence(Result,
        bSaved ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("SaveAssetPackage outcome"),
        bSaved ? FString(TEXT("fork package written")) : SaveError);
    if (ReparentResult.IsValid())
    {
        mcp_common::AddVerificationEvidence(Result,
            bReparentOk ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
            TEXT("nested reparent_blueprint post-check"),
            TEXT("see the nested `reparent` object for its own verification evidence"));
    }
    Result->SetBoolField(TEXT("success"), bOverallSuccess);
    if (!bOverallSuccess)
    {
        Result->SetStringField(TEXT("error"), !bSaved
            ? FString::Printf(TEXT("fork created but NOT saved: %s"), *SaveError)
            : FString(TEXT("fork saved, but the reparent of reparent_target did not pass its post-check (see result.reparent)")));
    }
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("fork_class_chain"), &HandleForkClassChain },
    { TEXT("reconcile_function_signature"), &HandleReconcileFunctionSignature },
    { TEXT("reparent_blueprint"), &HandleReparentBlueprintSafe },
};
} // namespace UEAgentReparentCommands

#undef LOCTEXT_NAMESPACE
