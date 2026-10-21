// Copyright (c) 2026 CarlxsMG.

#include "UEAgentBridge.h"
// per-command KNOWN-PARAM table, generated from the catalog (Tools/codegen_param_table.py).
// Used by the single point ALL responses go through (DispatchSingleCommand) to REPORT
// parameter keys that nobody reads -- never to refuse the call.
#include "McpCommandParamTable.gen.h"
// Parameter ALIAS table, generated from the same catalog
// (Tools/codegen_param_aliases.py). Same single point: it rewrites the non-canonical name to
// the canonical one BEFORE dispatching, and only when the command knows the canonical name and
// does NOT know the alias. Rewrites are reported in data.aliased_params -- never applied silently.
#include "McpParamAliases.gen.h"
// Table of commands that are UNRELIABLE with PIE running, derived from the C++
// (Tools/codegen_pie_safety.py). Consulted by the same single point
// (DispatchSingleCommand), and here the call IS refused: the alternative failure is
// a dead editor or a save that lies. See CheckPIESafetyGuard below.
#include "McpPieSafety.gen.h"
#include "Mcp/McpCommandFamilies.h"
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h" // ULevelEditorSubsystem::IsInPlayInEditor -- the same check is_play_in_editor_active uses
#include "Editor.h"                     // GEditor + FEditorDelegates (PIE ownership guard)
#include "ScopedTransaction.h"         // FScopedTransaction -- one undo step per call
#include "Editor/Transactor.h"         // FTransaction -- the PIE-session check of the undo step
#include "Mcp/McpActivity.h"           // the Activity panel's record of each call
#include "HAL/PlatformTime.h"          // FPlatformTime -- each call's duration
#include "HAL/IConsoleManager.h"       // mcp.UndoStepPerCall
#include "FileHelpers.h"                // FEditorFileUtils::GetOnLoadMapStartDelegate -- fires for ANY map open, UI included
#include "Editor/EditorEngine.h"        // UEditorEngine::PlayWorld (full definition for GEditor->PlayWorld)
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
// McpOversize: write the payload that does not fit in the response to Saved/MCPAudits/.
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "JsonObjectConverter.h"
#include "GameFramework/Actor.h"
#include "Engine/Selection.h"
#include "Kismet/GameplayStatics.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"       // FTSTicker -- game-thread command drain
#include "CoreGlobals.h"             // GIsRunningUnattendedScript
#include "Templates/UnrealTemplate.h" // TGuardValue
// Blueprint editing
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
// Engine and editor
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
// Blueprint graph
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node_CallFunction.h"
#include "K2Node_InputAction.h"
#include "K2Node_Self.h"
#include "GameFramework/InputSettings.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"
// Helpers shared with the command families (MakeErrorReply, ...)
#include "Commands/Common/McpCommonUtils.h"
// Collateral-dirty attribution: UPackage's dirty/save multicast events.
#include "UObject/Package.h"                 // UPackage::PackageMarkedDirtyEvent / PackageDirtyStateChangedEvent / PackageSavedWithContextEvent
#include "UObject/ObjectSaveContext.h"       // FObjectPostSaveContext (PackageSavedWithContextEvent's 3rd param)
#include "Misc/PackageName.h"                // FPackageName::DoesPackageExist -- file path of a collaterally dirtied package
#include "HAL/IConsoleManager.h"             // FAutoConsoleVariableRef -- the session-wide opt-in switch
#include "Mcp/McpHttpServer.h"               // native MCP server (Streamable HTTP)
#include "Mcp/McpToolCatalog.h"              // tools/list + rotation normalization
#include "Mcp/McpPreconditions.h"            // refusals that prevent a crash
#include "Mcp/McpReadback.h"                 // READBACK contract
#include "Mcp/McpPy.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "UEAgentSettings.h"               // HTTP server port / enable switch / token
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

// =====================================================================================
// COLLATERAL-DIRTY ATTRIBUTION.
//
// Why: a command can dirty a package under a read-only (vendored) content root while
// NOTHING in its response says so; the drift then only shows up later in source control,
// with no way left to attribute it to a specific call. `list_dirty_packages` answers
// "what is dirty NOW"; this answers "what did THIS call dirty", which is the attribution
// question.
//
// WHY NOT A BEFORE/AFTER SNAPSHOT.
// The obvious design is a `FEditorFileUtils::GetDirtyPackages` snapshot before/after every
// command. It has two problems, both fatal for a tool whose whole point is
// honesty:
//   1. THE PROBE HAS SIDE EFFECTS. FEditorFileUtils::GetDirtyWorldPackages ->
//      EnumerateDirtyWorldPackages (FileHelpers.cpp ~5571/~5690) itself calls
//      MarkPackageDirty in two narrow cases (unnamed MapBuildData of an already-dirty
//      world; a PKG_NewlyCreated unnamed world package). Running that twice per command,
//      on every command, makes the measuring instrument a writer.
//   2. A SET DIFF CANNOT SEE A RE-DIRTY. If the package was ALREADY dirty when the command
//      started (very common: a compile earlier in the same session), before/after sets are
//      identical and the tool reports "nothing" -- exactly the case the feature exists for.
// Instead this binds UPackage's own multicast events, which are COREUOBJECT_API, unguarded
// by any #if (Package.h), free when not recording, and exact:
//   - PackageMarkedDirtyEvent  (UObjectBaseUtility.cpp) -- fires on EVERY
//     MarkPackageDirty, "even when the package is already dirty", and hands us bWasDirty.
//   - PackageDirtyStateChangedEvent (Package.cpp) -- covers the direct
//     UPackage::SetDirtyFlag(true) path, which does NOT go through MarkPackageDirty.
//   - PackageSavedWithContextEvent -- the exact complement: what actually hit disk during
//     this command, whatever API did it (SaveAssetPackage or not).
//
// OPT-IN: nothing is recorded and no field is emitted unless the
// caller passes `track_collateral_dirty: true` on the command, or the session has
// `mcp.TrackCollateralDirty 1` set. With it off, the delegate handlers are one integer
// compare and every response is byte-identical to an untracked one.
//
// GAME THREAD ONLY -> no locking. DispatchSingleCommand is game-thread by contract, and
// every package dirty/save a command can cause happens on that same thread. RecordDepth
// is a DEPTH (not a bool) because `execute_batch` re-enters DispatchSingleCommand: a nested
// tracked command must not disarm the outer one, and its findings merge upward.
// =====================================================================================
// =====================================================================================
// OFFLOADING of responses that do not fit.
//
// Very large responses get cut by the transport/client (in practice somewhere around
// 45-49k characters). The tools that hit it most are the listing ones: get_actors_in_level,
// list_all_input_mappings, get_component_properties, get_graph_nodes.
//
// Why here and not tool by tool: most of those tools ALREADY have a cap parameter, and
// passing it is not enough -- the cap cannot be chosen well without seeing the size first. This is
// the single point every command goes through (and every `execute_batch` sub-command, which re-enters).
//
// What is kept: ALL the scalars of the payload (the counters, which is what is asked about
// most of the time) and, for each array/object moved out, its length. The detail is lost from
// the response -- which is exactly what the cut would lose anyway, only silently and with
// nowhere to look for it. The file goes to Saved/MCPAudits/, which read_mcp_audit_file reads.
// =====================================================================================
namespace McpOversize
{
    // Deliberately HIGH threshold. The point is not to trim large responses but to rescue
    // the ones that would not arrive whole: below this everything travels as usual and
    // no response changes shape.
    static int32 GOversizeCharsCVar = 40000;
    static FAutoConsoleVariableRef CVarOversizeChars(
        TEXT("mcp.OversizeResponseChars"),
        GOversizeCharsCVar,
        TEXT("Serialized payload size (characters) above which a command's detail is written to "
             "Saved/MCPAudits/ and the response keeps only scalars + array lengths + report_path. "
             "0 disables the offload entirely."),
        ECVF_Default);

    static FString SerializeToString(const TSharedPtr<FJsonObject>& Obj)
    {
        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
        return Out;
    }

    /** Writes Payload to Saved/MCPAudits/oversize_<command>_<UTC>.json. Same directory and
     *  format as AU_WriteReport, so read_mcp_audit_file reads it unchanged. */
    static bool WriteReport(const FString& CommandType, const FString& JsonString, FString& OutPath)
    {
        const FString AuditDir = FPaths::ProjectSavedDir() / TEXT("MCPAudits");
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (!PlatformFile.DirectoryExists(*AuditDir))
        {
            PlatformFile.CreateDirectoryTree(*AuditDir);
        }
        const FString FileName = FString::Printf(
            TEXT("oversize_%s_%s.json"), *CommandType, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")));
        const FString FilePath = AuditDir / FileName;
        if (!FFileHelper::SaveStringToFile(JsonString, *FilePath))
        {
            return false;
        }
        OutPath = FPaths::ConvertRelativePathToFull(FilePath);
        return true;
    }

    /** EXEMPT commands: those whose job IS to return a large report.
     *
     * Without this it is a LOOP: an offloaded report is read back with read_mcp_audit_file,
     * that read overflows TOO and returns yet another path -- and so on indefinitely, one
     * file per attempt, with the detail never arriving. Offloading the offload reader makes
     * UNREACHABLE exactly what was just saved. */
    static bool IsExempt(const FString& CommandType)
    {
        return CommandType == TEXT("read_mcp_audit_file");
    }

    /** If the payload does not fit, writes it out and replaces it with scalars + lengths + path.
     *  Touches nothing below the threshold. */
    static void MaybeOffload(const FString& CommandType, TSharedPtr<FJsonObject>& ResultJson)
    {
        if (GOversizeCharsCVar <= 0 || !ResultJson.IsValid() || IsExempt(CommandType))
        {
            return;
        }

        const FString Serialized = SerializeToString(ResultJson);
        if (Serialized.Len() <= GOversizeCharsCVar)
        {
            return;
        }

        FString ReportPath;
        const bool bWritten = WriteReport(CommandType, Serialized, ReportPath);

        // Rebuild the payload: scalars as is, containers by their size.
        TSharedPtr<FJsonObject> Slim = MakeShareable(new FJsonObject);
        TArray<TSharedPtr<FJsonValue>> Omitted;
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ResultJson->Values)
        {
            const TSharedPtr<FJsonValue>& V = Pair.Value;
            if (!V.IsValid()) { continue; }
            if (V->Type == EJson::Array)
            {
                TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject);
                Info->SetStringField(TEXT("field"), Pair.Key);
                Info->SetNumberField(TEXT("length"), V->AsArray().Num());
                Omitted.Add(MakeShared<FJsonValueObject>(Info));
            }
            else if (V->Type == EJson::Object)
            {
                TSharedPtr<FJsonObject> Info = MakeShareable(new FJsonObject);
                Info->SetStringField(TEXT("field"), Pair.Key);
                Info->SetNumberField(TEXT("keys"), V->AsObject().IsValid() ? V->AsObject()->Values.Num() : 0);
                Omitted.Add(MakeShared<FJsonValueObject>(Info));
            }
            else
            {
                Slim->SetField(Pair.Key, V); // scalars: kept whole
            }
        }

        Slim->SetBoolField(TEXT("response_offloaded"), true);
        Slim->SetNumberField(TEXT("response_chars"), Serialized.Len());
        Slim->SetNumberField(TEXT("offload_threshold_chars"), GOversizeCharsCVar);
        Slim->SetArrayField(TEXT("offloaded_fields"), Omitted);
        if (bWritten)
        {
            Slim->SetStringField(TEXT("report_path"), ReportPath);
            Slim->SetStringField(TEXT("offload_note"),
                TEXT("This response did not fit; its scalar fields are intact and the full payload was "
                     "written to disk. Read it with read_mcp_audit_file(path=report_path). To get it "
                     "inline instead, narrow the query (max_results / a filter) and call again."));
        }
        else
        {
            // If the write fails, say so, but do NOT return the giant payload: the transport
            // would cut it anyway, and without saying it did.
            Slim->SetStringField(TEXT("offload_note"),
                TEXT("This response did not fit and the disk write FAILED, so the detail is lost. "
                     "Re-run with a narrower query (max_results / a filter)."));
        }

        ResultJson = Slim;
    }
}

namespace McpCollateralDirty
{
    static int32 GTrackCollateralDirtyCVar = 0;
    static FAutoConsoleVariableRef CVarTrackCollateralDirty(
        TEXT("mcp.TrackCollateralDirty"),
        GTrackCollateralDirtyCVar,
        TEXT("1 = every MCP command reports collateral_dirtied_packages/readonly_dirtied/readonly_violation. ")
        TEXT("0 (default) = only commands that pass track_collateral_dirty:true. Read-only accounting; never saves."),
        ECVF_Default);

    // Recording state (game thread only).
    static int32 RecordDepth = 0;
    static TSet<FName> NewlyDirtied;   // MarkPackageDirty with bWasDirty == false
    static TSet<FName> ReDirtied;      // MarkPackageDirty on an already-dirty package
    static TSet<FName> SavedPackages;  // PackageSavedWithContextEvent
    static FDelegateHandle MarkedDirtyHandle;
    static FDelegateHandle DirtyStateHandle;
    static FDelegateHandle SavedHandle;

    static void OnPackageMarkedDirty(UPackage* Package, bool bWasDirty)
    {
        if (RecordDepth <= 0 || !Package) { return; }
        (bWasDirty ? ReDirtied : NewlyDirtied).Add(Package->GetFName());
    }

    static void OnPackageDirtyStateChanged(UPackage* Package)
    {
        // Fires on BOTH transitions; only a transition INTO dirty is a collateral dirty.
        if (RecordDepth <= 0 || !Package || !Package->IsDirty()) { return; }
        NewlyDirtied.Add(Package->GetFName());
    }

    static void OnPackageSaved(const FString& /*PackageFileName*/, UPackage* Package, FObjectPostSaveContext /*Context*/)
    {
        if (RecordDepth <= 0 || !Package) { return; }
        SavedPackages.Add(Package->GetFName());
    }

    static void Bind()
    {
        if (!MarkedDirtyHandle.IsValid())
        {
            MarkedDirtyHandle = UPackage::PackageMarkedDirtyEvent.AddStatic(&OnPackageMarkedDirty);
        }
        if (!DirtyStateHandle.IsValid())
        {
            DirtyStateHandle = UPackage::PackageDirtyStateChangedEvent.AddStatic(&OnPackageDirtyStateChanged);
        }
        if (!SavedHandle.IsValid())
        {
            SavedHandle = UPackage::PackageSavedWithContextEvent.AddStatic(&OnPackageSaved);
        }
    }

    static void Unbind()
    {
        if (MarkedDirtyHandle.IsValid())
        {
            UPackage::PackageMarkedDirtyEvent.Remove(MarkedDirtyHandle);
            MarkedDirtyHandle.Reset();
        }
        if (DirtyStateHandle.IsValid())
        {
            UPackage::PackageDirtyStateChangedEvent.Remove(DirtyStateHandle);
            DirtyStateHandle.Reset();
        }
        if (SavedHandle.IsValid())
        {
            UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
            SavedHandle.Reset();
        }
        RecordDepth = 0;
        NewlyDirtied.Empty();
        ReDirtied.Empty();
        SavedPackages.Empty();
    }

    // True when this command must be tracked: explicit per-call param wins, cvar is the
    // session-wide fallback. Absent param + cvar 0 => no tracking, no fields, no cost.
    static bool ShouldTrack(const TSharedPtr<FJsonObject>& Params)
    {
        bool bTrack = false;
        // track_collateral_dirty - opt-in: report which packages THIS call dirtied.
        if (Params.IsValid() && Params->TryGetBoolField(TEXT("track_collateral_dirty"), bTrack))
        {
            return bTrack;
        }
        return GTrackCollateralDirtyCVar != 0;
    }

    // Arm on construction, harvest on Finish(). Finish() is idempotent and the destructor
    // calls it, so a handler that THROWS (DispatchSingleCommand runs inside a try/catch)
    // can never leave RecordDepth armed. On Finish the parent's sets come back and this
    // scope's findings are merged into them, so a `execute_batch` reports the union of its
    // sub-commands as well as its own.
    struct FScopedRecorder
    {
        bool bActive = false;
        bool bFinished = false;
        TSet<FName> ParentNew, ParentRe, ParentSaved;   // the enclosing scope's findings
        TSet<FName> MyNew, MyRe, MySaved;               // THIS scope's findings (after Finish)

        explicit FScopedRecorder(bool bInActive)
            : bActive(bInActive)
        {
            if (!bActive) { return; }
            Bind();
            ParentNew   = MoveTemp(NewlyDirtied);   NewlyDirtied.Empty();
            ParentRe    = MoveTemp(ReDirtied);      ReDirtied.Empty();
            ParentSaved = MoveTemp(SavedPackages);  SavedPackages.Empty();
            ++RecordDepth;
        }

        void Finish()
        {
            if (!bActive || bFinished) { return; }
            bFinished = true;
            --RecordDepth;

            MyNew   = MoveTemp(NewlyDirtied);   NewlyDirtied.Empty();
            MyRe    = MoveTemp(ReDirtied);      ReDirtied.Empty();
            MySaved = MoveTemp(SavedPackages);  SavedPackages.Empty();

            NewlyDirtied  = ParentNew;   NewlyDirtied.Append(MyNew);
            ReDirtied     = ParentRe;    ReDirtied.Append(MyRe);
            SavedPackages = ParentSaved; SavedPackages.Append(MySaved);
        }

        ~FScopedRecorder() { Finish(); }
    };
}

namespace UEAgentBridgeImpl
{
    void SetFailure(const TSharedPtr<FJsonObject>& Envelope, const FString& Reason)
    {
        Envelope->SetStringField(TEXT("status"), TEXT("error"));
        Envelope->SetStringField(TEXT("error"), Reason);
    }

    // The transport envelope around a handler's answer. A handler either states its outcome
    // ({"success": bool, "error"?: ...}) or returns a bare payload, which counts as a success.
    // A failure still carries the whole handler payload under "result": its diagnostics
    // (regressions, rolled_back, compile_status, ...) are the point of reporting it.
    void WrapHandlerResult(const TSharedPtr<FJsonObject>& Envelope, const TSharedPtr<FJsonObject>& HandlerResult)
    {
        const bool bStatesOutcome = HandlerResult.IsValid() && HandlerResult->HasField(TEXT("success"));
        if (bStatesOutcome && !HandlerResult->GetBoolField(TEXT("success")))
        {
            // Never a bare "Unknown error" for a structured failure: say where the detail is.
            SetFailure(Envelope, HandlerResult->HasField(TEXT("error"))
                ? HandlerResult->GetStringField(TEXT("error"))
                : FString(TEXT("Command reported success=false (see result for details)")));
        }
        else
        {
            Envelope->SetStringField(TEXT("status"), TEXT("success"));
        }
        if (HandlerResult.IsValid())
        {
            Envelope->SetObjectField(TEXT("result"), HandlerResult);
        }
    }
}

void UUEAgentBridge::Initialize(FSubsystemCollectionBase& Collection)
{
    UE_LOG(LogTemp, Display, TEXT("UEAgentBridge: initializing"));

    // PIE ownership guard: track who owns the running Play-in-Editor. These fire on
    // the game thread, so the FString owner state they touch needs no locking.
    PostPIEStartedHandle = FEditorDelegates::PostPIEStarted.AddUObject(this, &UUEAgentBridge::OnPostPIEStarted);
    EndPIEHandle = FEditorDelegates::EndPIE.AddUObject(this, &UUEAgentBridge::OnEndPIE);

    // Release Python's grip on every world about to be destroyed, whoever starts the load.
    // FEditorFileUtils::LoadMap broadcasts this as its first statement, so this covers the
    // Content Browser and the File menu too -- not just our own load_level. Without it,
    // FPyReferenceCollector can end up as the SOLE retainer of the world the engine is
    // destroying, which is fatal. See McpCommonUtils.h for the full rationale.
    LoadMapStartHandle = FEditorFileUtils::GetOnLoadMapStartDelegate().AddLambda([]()
    {
        // No destination is known here (the engine does not pass one), so nothing is exempted
        // as a reuse candidate. Purging a world the engine then reuses is harmless: the purge
        // only nulls Python's handles to it, it does not touch the world itself.
        const int32 PurgedRoots = mcp_common::PurgeWorldsBeforeLoadMap(FString());
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] LoadMap starting; %d root(s) purged of script refs"), PurgedRoots);
    });

    // The native MCP server (HTTP) is the entry point for MCP clients; it registers the queue
    // ticker itself.
    StartHttpServer();
}

// Teardown: editor delegates off, then the server (whose stop drains the queue one last time).
void UUEAgentBridge::Deinitialize()
{
    UE_LOG(LogTemp, Display, TEXT("UEAgentBridge: shutting down"));

    // Unhook the PIE ownership delegates before teardown.
    if (PostPIEStartedHandle.IsValid())
    {
        FEditorDelegates::PostPIEStarted.Remove(PostPIEStartedHandle);
        PostPIEStartedHandle.Reset();
    }
    if (EndPIEHandle.IsValid())
    {
        FEditorDelegates::EndPIE.Remove(EndPIEHandle);
        EndPIEHandle.Reset();
    }
    if (LoadMapStartHandle.IsValid())
    {
        FEditorFileUtils::GetOnLoadMapStartDelegate().Remove(LoadMapStartHandle);
        LoadMapStartHandle.Reset();
    }

    // Collateral-dirty attribution: these are AddStatic bindings on UPackage's process-wide
    // delegates pointing at code in THIS module. They MUST come off before the module can be
    // unloaded, or a later MarkPackageDirty would jump into freed code. Bound lazily (only
    // once a tracked command runs), so this is usually a no-op.
    McpCollateralDirty::Unbind();

    // StopHttpServer joins every connection thread first, then removes the ticker and drains the
    // queue one last time, so no request is left without an answer.
    StopHttpServer();
}

void UUEAgentBridge::EnsureCommandTicker()
{
    if (!CommandTickerHandle.IsValid())
    {
        CommandTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateUObject(this, &UUEAgentBridge::DrainCommandQueue), 0.0f);
    }
}

void UUEAgentBridge::ReleaseCommandTickerIfIdle()
{
    if (IsHttpRunning())
    {
        return; // the server is still enqueuing
    }
    if (CommandTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(CommandTickerHandle);
        CommandTickerHandle.Reset();
    }
    DrainCommandQueue(0.0f);
}

bool UUEAgentBridge::IsHttpRunning() const
{
    return HttpServer && HttpServer->IsRunning();
}

bool UUEAgentBridge::IsHttpCompactTools() const
{
    return IsHttpRunning() && HttpServer->IsCompactTools();
}

bool UUEAgentBridge::StartHttpServer()
{
    if (IsHttpRunning())
    {
        return true;
    }

    const UUEAgentSettings* Settings = GetDefault<UUEAgentSettings>();
    LastHttpError.Reset();
    if (!Settings->bEnableHttpServer)
    {
        UE_LOG(LogTemp, Display, TEXT("UEAgentBridge: MCP HTTP server disabled in the settings (Editor Preferences > Plugins > UEAgent)"));
        return false;
    }

    // The catalog before the socket: without it, tools/list comes back empty. The server starts
    // anyway -- initialize and ping still help to diagnose -- but it warns loudly.
    FString CatalogError;
    const FString CatalogPath = FMcpToolCatalog::DefaultCatalogPath();
    TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::LoadFromFile(CatalogPath, CatalogError);
    FMcpToolCatalog::SetCurrent(Catalog);
    if (!Catalog.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("UEAgentBridge: %s -- the MCP HTTP server will expose no tools"), *CatalogError);
        NotifyEditor(FString::Printf(TEXT("UEAgent: %s"), *CatalogError), true);
    }

    EnsureCommandTicker();
    if (!HttpServer)
    {
        HttpServer = new FMcpHttpServer(this);
    }
    FString Error;
    const uint16 HttpPort = Settings->GetEffectiveHttpPort();
    if (!HttpServer->Start(HttpPort, Settings->AuthToken, Error))
    {
        LastHttpError = Error;
        UE_LOG(LogTemp, Error, TEXT("UEAgentBridge: MCP HTTP server NOT started: %s"), *Error);
        NotifyEditor(FString::Printf(TEXT("UEAgent: %s. Retry from \"MCP\" in the status bar (or mcp.http.restart in the console)."), *Error), true);
        ReleaseCommandTickerIfIdle();
        return false;
    }

    UE_LOG(LogTemp, Display, TEXT("UEAgentBridge: MCP HTTP server started on http://127.0.0.1:%d/mcp (%d tools, %d params)%s%s"),
        HttpPort,
        Catalog.IsValid() ? Catalog->NumTools() : 0,
        Catalog.IsValid() ? Catalog->NumParams() : 0,
        Settings->AuthToken.IsEmpty() ? TEXT("") : TEXT(", with token"),
        HttpServer->IsCompactTools() ? TEXT(", compact tool list (UEAGENT_TOOLS)") : TEXT(""));
    return true;
}

void UUEAgentBridge::StopHttpServer()
{
    if (HttpServer)
    {
        HttpServer->Stop();
        delete HttpServer;
        HttpServer = nullptr;
    }
    ReleaseCommandTickerIfIdle();
}

FString UUEAgentBridge::DescribeHttpStatus() const
{
    const TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::GetCurrent();
    const FString CatalogText = Catalog.IsValid()
        ? FString::Printf(TEXT("%d tools (%s)"), Catalog->NumTools(), *Catalog->GetSourcePath())
        : FString(TEXT("NO catalog"));
    if (!IsHttpRunning())
    {
        return FString::Printf(TEXT("MCP HTTP: stopped | %s"), *CatalogText);
    }
    return FString::Printf(TEXT("MCP HTTP: http://127.0.0.1:%d/mcp | %d connections | %lld requests | %lld tools/call | %s%s"),
        HttpServer->GetPort(), HttpServer->GetNumConnections(), HttpServer->GetNumRequests(), HttpServer->GetNumToolCalls(), *CatalogText,
        HttpServer->IsCompactTools() ? TEXT(" | compact tool list") : TEXT(""));
}

uint16 UUEAgentBridge::GetHttpPort() const
{
    return IsHttpRunning() ? HttpServer->GetPort() : GetDefault<UUEAgentSettings>()->GetEffectiveHttpPort();
}

void UUEAgentBridge::GetHttpCounters(int32& OutConnections, int64& OutRequests, int64& OutToolCalls) const
{
    OutConnections = IsHttpRunning() ? HttpServer->GetNumConnections() : 0;
    OutRequests = HttpServer ? HttpServer->GetNumRequests() : 0;
    OutToolCalls = HttpServer ? HttpServer->GetNumToolCalls() : 0;
}

void UUEAgentBridge::NotifyEditor(const FString& Message, bool bIsError)
{
    // During editor startup Slate may not be ready yet: retry on the next tick until it is
    // (the log has the message in any case).
    auto Show = [Message, bIsError](float) -> bool
    {
        if (!FSlateApplication::IsInitialized())
        {
            return true; // keep waiting
        }
        FNotificationInfo Info(FText::FromString(Message));
        Info.bFireAndForget = true;
        Info.ExpireDuration = bIsError ? 20.0f : 6.0f;
        Info.bUseLargeFont = false;
        if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
        {
            Item->SetCompletionState(bIsError ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
        }
        return false;
    };
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(Show), 0.5f);
}

// Console: mcp.http.status / mcp.http.restart. The same as the "MCP" status-bar menu
// (Mcp/McpEditorUI.cpp), for an editor without a UI or for whoever prefers the console.
namespace McpHttpConsole
{
    static UUEAgentBridge* FindBridge()
    {
        return GEditor ? GEditor->GetEditorSubsystem<UUEAgentBridge>() : nullptr;
    }

    static FAutoConsoleCommand StatusCommand(
        TEXT("mcp.http.status"),
        TEXT("Status of the native MCP server (HTTP): port, connections, requests and catalog."),
        FConsoleCommandDelegate::CreateLambda([]()
        {
            if (UUEAgentBridge* Bridge = FindBridge())
            {
                UE_LOG(LogTemp, Display, TEXT("%s"), *Bridge->DescribeHttpStatus());
            }
        }));

    static FAutoConsoleCommand RestartCommand(
        TEXT("mcp.http.restart"),
        TEXT("Restarts the native MCP server (HTTP) and reloads Resources/command_catalog.json."),
        FConsoleCommandDelegate::CreateLambda([]()
        {
            if (UUEAgentBridge* Bridge = FindBridge())
            {
                Bridge->StopHttpServer();
                Bridge->StartHttpServer();
                UE_LOG(LogTemp, Display, TEXT("%s"), *Bridge->DescribeHttpStatus());
            }
        }));
}

// --- PIE ownership guard -----------------------------------------------------
// All three run on the game thread (delegate callbacks fire there; the guard runs
// in the ticker-drained Work lambda), so the FString owner state needs no locking.

void UUEAgentBridge::OnPostPIEStarted(bool /*bIsSimulating*/)
{
    // PIE is now live. Promote whoever requested the start (PendingPIEOwner) to the
    // owner. If a human started PIE from the UI, no start_play_in_editor command ran
    // to stamp PendingPIEOwner, so it is empty here -> owner "" = unknown (protected
    // against any other session).
    PIEOwnerSessionId = PendingPIEOwner;
    PendingPIEOwner.Empty();
    UE_LOG(LogTemp, Verbose, TEXT("[MCP] PIE started; owner session = %s"),
        PIEOwnerSessionId.IsEmpty() ? TEXT("<unknown/human>") : *PIEOwnerSessionId);
}

void UUEAgentBridge::OnEndPIE(bool /*bIsSimulating*/)
{
    PIEOwnerSessionId.Empty();
    PendingPIEOwner.Empty();

    // We are at PlayLevel.cpp (EndPIE.Broadcast). The engine's CollectGarbage is at :502
    // and the checkf that KILLS the editor -- "Object 'Package /Game/.../UEDPIE_...' from PIE
    // level still referenced" -- at :552-553. 247 lines of margin, same tick, and the only
    // intermediate return skips the checkf too. The crash this prevents names
    // FPyReferenceCollector::AddReferencedObjects as the sole retainer of the PIE world.
    const int32 PurgedRoots = mcp_common::PurgePIEScriptRefs();
    UE_LOG(LogTemp, Verbose, TEXT("[MCP] PIE ended; owner cleared, %d PIE root(s) purged of script refs"), PurgedRoots);

    // The same checkf fires when the undo history still holds the PIE GameInstance (a transaction that recorded a
    // widget made in PIE, by any path). The engine resets the history for its own PIE objects only after this point
    // and does not see this kind; the PIE worlds still exist here, so they can be recognised.
    if (mcp_common::ResetUndoHistoryHoldingPieSession())
    {
        UE_LOG(LogTemp, Display, TEXT("[MCP] PIE ended; the undo history held objects of the PIE session and was reset"));
    }
}

TSharedPtr<FJsonObject> UUEAgentBridge::CheckPIEOwnershipGuard(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& SessionId)
{
    const bool bIsStart = (CommandType == TEXT("start_play_in_editor"));
    const bool bIsStop  = (CommandType == TEXT("stop_play_in_editor"));
    if (!bIsStart && !bIsStop)
    {
        return nullptr; // not a guarded command -> proceed to normal dispatch
    }

    bool bForce = false;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("force"), bForce);
    }

    // Is a PIE session active right now? PlayWorld is set for the duration of PIE and
    // nulled on end -- the canonical "is PIE running" check, coincident with the
    // PostPIEStarted/EndPIE hooks that maintain PIEOwnerSessionId.
    const bool bPIEActive = (GEditor != nullptr) && (GEditor->PlayWorld != nullptr);

    // A different session owns PIE when one is active and its recorded owner differs
    // from this requester. Note "" == "" (a no-session client acting on a
    // human-started PIE) is treated as SAME owner -> allowed, kept for compatibility
    // with clients that send no session. Only a real (uuid)
    // session is ever refused against a "" (human/unknown) owner.
    const bool bOwnedByOther = bPIEActive && (PIEOwnerSessionId != SessionId);

    // Helper display strings for the refusal envelope.
    auto Display = [](const FString& S) -> FString { return S.IsEmpty() ? FString(TEXT("unknown")) : S; };

    // Builds the refusal envelope with the diagnostics INSIDE `result`.
    // WARNING: fields at the envelope level do NOT reach the caller: on `status:"error"` the
    // unwrapping layer forwards only `error` and `result` and drops the rest. Keeping them in
    // `result` is what makes the promise in the start_/stop_play_in_editor description
    // -- "refused with pie_owner/requester in the payload" -- true. The PIE-safety guard
    // below follows the same rule.
    auto MakeRefusal = [&](const FString& Message) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
        Data->SetBoolField(TEXT("refused"), true);
        Data->SetStringField(TEXT("pie_owner"), Display(PIEOwnerSessionId));
        Data->SetStringField(TEXT("requester"), Display(SessionId));
        Data->SetStringField(TEXT("hint"), TEXT("pass force=true to override"));

        TSharedPtr<FJsonObject> Envelope = MakeShareable(new FJsonObject);
        Envelope->SetStringField(TEXT("status"), TEXT("error"));
        Envelope->SetStringField(TEXT("error"), Message);
        Envelope->SetObjectField(TEXT("result"), Data);
        return Envelope;
    };

    if (bIsStart)
    {
        if (bOwnedByOther && !bForce)
        {
            return MakeRefusal(FString::Printf(
                TEXT("A Play-in-Editor session is already running, started by another session (or manually) [pie_owner=%s]. Refusing to start a new one from [requester=%s]. Pass force=true to override."),
                *Display(PIEOwnerSessionId), *Display(SessionId)));
        }

        // Allowed to start (no PIE active, we already own it, or force override). Stage
        // THIS session as the pending owner so OnPostPIEStarted records us once PIE is
        // live. A force-start that seizes another session's PIE takes ownership too.
        if (bForce && bOwnedByOther)
        {
            UE_LOG(LogTemp, Warning, TEXT("[MCP] start_play_in_editor force-override: session %s seizing PIE owned by %s"),
                *Display(SessionId), *Display(PIEOwnerSessionId));
        }
        PendingPIEOwner = SessionId;
        return nullptr; // proceed to the real handler
    }

    // bIsStop
    if (bOwnedByOther && !bForce)
    {
        return MakeRefusal(FString::Printf(
            TEXT("The running Play-in-Editor session is owned by another session (or was started manually) [pie_owner=%s]. Refusing to stop it from [requester=%s]. Pass force=true to override."),
            *Display(PIEOwnerSessionId), *Display(SessionId)));
    }
    if (bForce && bOwnedByOther)
    {
        UE_LOG(LogTemp, Warning, TEXT("[MCP] stop_play_in_editor force-override: session %s stopping PIE owned by %s"),
            *Display(SessionId), *Display(PIEOwnerSessionId));
    }
    return nullptr; // owner (or force, or no PIE active) -> let the handler run
}

// --- PIE SAFETY guard --------------------------------------------------------
// The guard above answers "is this PIE YOURS?". This one answers a different question:
// "can this be done WITH PIE running, whoever owns it?".
//
// WHY IT EXISTS (both families fail SILENTLY):
//
//   1) COMPILING A BLUEPRINT CAN KILL THE EDITOR. Compiling reinstances every live actor
//      of that class and the reinstancer leaves a `UREINST_<Class>` trampoline behind.
//      If one of those retains a world, UEditorEngine::CheckForWorldGCLeaks logs Fatal --
//      not Error: Editor.CheckForWorldGCLeaksAreFatal defaults to true -- and the process
//      goes down with every in-memory edit. The typical crash is a "World Memory Leaks"
//      fatal a few seconds after starting a multiplayer PIE, naming a `UREINST_<Widget>_C_N`
//      as the object that retains the world.
//
//   2) SAVING RETURNS SUCCESS WITHOUT WRITING. The engine guard
//      (Editor.AllowSavingAssetsDuringPIE=0) refuses BEFORE attempting anything, and a
//      handler's `{saved:false}` would come back wrapped in the `status:success` the
//      bridge synthesizes. And `list_dirty_packages.pie_active`, the field a caller would
//      check first, is not reliable (it can report false with PIE live and responding),
//      which invites exactly that mistake.
//
// WHY IT REFUSES INSTEAD OF STOPPING PIE. Stopping PIE to save would end the session of
// whoever is playing as a side effect of saving. The plugin ANSWERS and leaves the PIE alone:
// a refusal is a visible, recoverable error; a dead editor is a lost session, and a save that
// lies is drift nobody sees until it matters.
//
// WHY THE LIST IS NOT WRITTEN BY HAND. "compiles a Blueprint" is a property OF THE C++,
// so it is DERIVED from the C++ (McpPieSafety.gen.h, generated by
// Tools/codegen_pie_safety.py, which also runs positive and negative controls
// before writing). A hand-written list rots as soon as someone adds a command, and a
// command that compiles but is missing from the table reads as "safe".
//
// THE ESCAPE HATCH. `force: true` bypasses it -- the same one as the ownership guard.
// Without an escape hatch, a guard that forbids normal work ends up being removed, and here
// "normal work" includes legitimate cases: measuring something in PIE that requires
// touching a graph.
TSharedPtr<FJsonObject> UUEAgentBridge::CheckPIESafetyGuard(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // 1) Is the command in the table? Asked BEFORE checking for PIE because it is a TMap
    // lookup and the other check touches the engine: without PIE, this is the whole cost.
    const McpPieSafety::FEntry* Entry = McpPieSafety::Find(FName(*CommandType));
    if (Entry == nullptr)
    {
        return nullptr; // not listed as unsafe -> normal dispatch
    }

    // 2) Explicit escape hatch from the caller.
    bool bForce = false;
    if (Params.IsValid())
    {
        Params->TryGetBoolField(TEXT("force"), bForce);
    }
    if (bForce)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("[MCP] PIE-safety override: '%s' runs with force=true (%s). Evidence: %s"),
            *CommandType, McpPieSafety::ReasonText(Entry->Reason), Entry->Evidence);
        return nullptr;
    }

    // 3) Is PIE running? Uses the SAME detector that is_play_in_editor_active reports, which
    // is the check callers are told to use -- if the guard and that check could disagree,
    // the refusal would be impossible to believe. GEditor->PlayWorld is the fallback for
    // when the subsystem is not available (the same pair CheckPIEOwnershipGuard uses
    // above).
    ULevelEditorSubsystem* LevelEditorSubsystem =
        GEditor ? GEditor->GetEditorSubsystem<ULevelEditorSubsystem>() : nullptr;
    const bool bPIEActive = LevelEditorSubsystem
        ? LevelEditorSubsystem->IsInPlayInEditor()
        : (GEditor != nullptr && GEditor->PlayWorld != nullptr);
    if (!bPIEActive)
    {
        return nullptr;
    }

    // 4) Refusal. Same envelope as the ownership guard (status/error/refused), so a client
    // that already reads that one understands this one without any change. The text says
    // WHAT to do, not just that it cannot be done: stop PIE and retry.
    const bool bIsSave = (Entry->Reason == McpPieSafety::EReason::Saves);

    // WARNING: the diagnostic fields go INSIDE `result`, not at the envelope level. On
    // `status:"error"` the unwrapping layer forwards EXACTLY two things -- `error` and
    // `result` -- and DROPS any other top-level key, so refused/pie_active/reason/evidence
    // placed at the top would never reach the caller: only the text would. `result` is the
    // sanctioned channel ("forward the handler's diagnostic payload when the bridge
    // includes it on a structured failure").
    TSharedPtr<FJsonObject> Data = MakeShareable(new FJsonObject);
    Data->SetBoolField(TEXT("refused"), true);
    Data->SetBoolField(TEXT("pie_active"), true);
    Data->SetStringField(TEXT("command"), CommandType);
    Data->SetStringField(TEXT("reason"), McpPieSafety::ReasonText(Entry->Reason));
    // The C++ line that classifies this command: it makes the refusal AUDITABLE rather than
    // just believable. If that line does not say so, the row is stale -- rerun the codegen.
    Data->SetStringField(TEXT("evidence"), Entry->Evidence);
    Data->SetStringField(TEXT("hint"), TEXT("stop_play_in_editor, then retry; or pass force=true"));

    TSharedPtr<FJsonObject> Refusal = MakeShareable(new FJsonObject);
    Refusal->SetStringField(TEXT("status"), TEXT("error"));
    Refusal->SetStringField(TEXT("error"), FString::Printf(
        TEXT("REFUSED: '%s' %s, and a Play-in-Editor session is running. %s ")
        TEXT("Stop PIE first (stop_play_in_editor), then retry. Pass force=true to override at your own risk."),
        *CommandType,
        McpPieSafety::ReasonText(Entry->Reason),
        bIsSave
            ? TEXT("The engine guard Editor.AllowSavingAssetsDuringPIE=0 refuses the write BEFORE attempting anything, so this would report success without writing a byte.")
            : TEXT("Compiling reinstances the live PIE actors of that class, and the UREINST_* trampoline left behind can retain a world -- CheckForWorldGCLeaks is Fatal, which KILLS the editor and every unsaved edit with it.")));
    Refusal->SetObjectField(TEXT("result"), Data);
    return Refusal;
}

// parameter-alias rewrite (McpParamAliases.gen.h), in its own function: used by the
// dispatch (in place) and by the READBACK contract's view of the arguments (on a copy).
namespace McpBridgeParams
{
    // The catalog names "the path of a blueprint" in SIX different ways -- blueprint_name,
    // asset_path, path... -- and the argument dictionary of four sibling tools in THREE
    // (parameters / params / args_json), so calls get lost just because of a parameter name.
    //
    // The rewrite happens ONLY under the four conditions below, and NEVER silently: whatever
    // is rewritten travels back in data.aliased_params (if OutAliased is not null).
    static void RewriteAliases(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, TArray<TSharedPtr<FJsonValue>>* OutAliased)
    {
        const TSet<FName>* KnownParams = McpCommandParamTable::FindKnownParams(FName(*CommandType));
        if (!KnownParams || !Params.IsValid())
        {
            return;
        }
        for (const McpParamAliases::FAlias& Rule : McpParamAliases::For(FName(*CommandType)))
        {
            const FString AliasKey = Rule.Alias.ToString();
            const FString CanonicalKey = Rule.Canonical.ToString();

            //  1. the caller sent the alias
            //  2. and did NOT send the canonical name -> a value the caller set is never overwritten
            //  3. the command knows the canonical name
            //  4. and does NOT know the alias  -> if it does, that name means SOMETHING ELSE
            //                                     there (set_property_access_path declares `path`
            //                                     AND `blueprint_name`: it excludes itself)
            if (!Params->HasField(AliasKey)) { continue; }
            if (Params->HasField(CanonicalKey)) { continue; }
            if (!KnownParams->Contains(Rule.Canonical)) { continue; }
            if (KnownParams->Contains(Rule.Alias)) { continue; }

            Params->SetField(CanonicalKey, Params->TryGetField(AliasKey));
            // Removing it keeps CollectUnknownParams from ALSO reporting it as unknown: one
            // answer per key, and it says what really happened to it.
            Params->RemoveField(AliasKey);

            if (OutAliased)
            {
                TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
                Entry->SetStringField(TEXT("from"), AliasKey);
                Entry->SetStringField(TEXT("to"), CanonicalKey);
                OutAliased->Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    /**
     * The arguments as the READBACK contract reads them: ALIASES rewritten to the canonical
     * name and nulls removed BEFORE take_before / verify_readback, so the checks read canonical
     * names (`blueprint_name`, not `blueprint_path`).
     *
     * WARNING: rotation is NOT converted to an array here, on purpose: converting it before the
     * readback would make the check that warns "the array's 3rd component is ROLL" fire
     * on {roll: X} -- the very object form that warning recommends as the way out.
     */
    static TSharedPtr<FJsonObject> ContractView(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
    {
        TSharedPtr<FJsonObject> View = McpPy::Clone(Params);
        RewriteAliases(CommandType, View, nullptr);
        TArray<FString> Nulls;
        for (const auto& Field : View->Values)
        {
            if (McpPy::IsNone(Field.Value))
            {
                Nulls.Add(FString(Field.Key));
            }
        }
        for (const FString& Key : Nulls)
        {
            View->RemoveField(Key);
        }
        return View;
    }
}

// Dispatches ONE command by name on the CALLING thread (no thread hop, no queueing) and
// returns the fully-wrapped {"status":"success"|"error", ...} envelope. It is a function of its
// own so the `execute_batch` meta-command can call it in a loop within a single game-thread pass.
//
// Game thread only: it runs from a queued work item, or in a loop for `execute_batch`.
// Each tool call is ONE undo step, named after the tool ("UEAgent: spawn_actor"), so Ctrl+Z reverts a
// call as a whole. The handlers' own FScopedTransactions nest into it. A call that changes nothing leaves
// no entry: the engine drops a transaction without changes, and one that only touched PIE objects
// (FTransaction::IsTransient); a step that holds an object of the running PIE session the engine does not
// see as one (HoldsPieSessionObjects) is dropped here. A tool that resets the buffer mid-call (a map load) is safe too:
// UTransBuffer::Reset purges the open transaction, and End tolerates the purge.
namespace McpUndoStep
{
    // Whether calls get their undo step (1, the default) or leave the buffer to the handlers (0). The
    // console variable references this int32, as mcp.ReadOnlyRootEnforcement does.
    int32 GMcpUndoStepPerCall = 1;

    FAutoConsoleVariableRef CVarMcpUndoStepPerCall(
        TEXT("mcp.UndoStepPerCall"),
        GMcpUndoStepPerCall,
        TEXT("1 (default) = each MCP tool call is one undo step, named after the tool. ")
        TEXT("0 = calls leave the undo buffer to each tool, as before."),
        ECVF_Default);

    // The tools that drive or read the undo buffer themselves stay outside: undo and redo refuse to run
    // inside a transaction, the history readers would see it open, and build_blueprint_graph rolls back
    // with UndoTransaction, which an enclosing transaction would block.
    static bool ManagesUndoBuffer(const FString& CommandType)
    {
        static const TSet<FString> Commands = {
            TEXT("undo"), TEXT("redo"), TEXT("get_undo_state"), TEXT("get_transaction_history"),
            TEXT("begin_transaction"), TEXT("end_transaction"), TEXT("cancel_transaction"),
            TEXT("reset_transaction_buffer"), TEXT("set_undo_barrier"), TEXT("remove_undo_barrier"),
            TEXT("build_blueprint_graph") };
        return Commands.Contains(CommandType);
    }

    // Whether the open undo step holds an object of the running Play-in-Editor session the engine does not drop by
    // itself: a PIE GameInstance, or what it outers (a widget made with CreateWidget). See
    // mcp_common::TransactionHoldsPieSessionObjects.
    static bool HoldsPieSessionObjects()
    {
        // GUndo is the open transaction; the editor's buffer makes only FTransactions (the engine casts it the same way).
        return GEditor && GEditor->PlayWorld && GUndo
            && mcp_common::TransactionHoldsPieSessionObjects(static_cast<const FTransaction*>(GUndo));
    }

    // A `execute_batch` is one step for all its commands, unless it opens its own transaction
    // (`transaction: true`) or holds a command that has to run outside one.
    static bool ShouldWrap(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
    {
        if (GMcpUndoStepPerCall == 0 || ManagesUndoBuffer(CommandType))
        {
            return false;
        }
        if (CommandType != TEXT("execute_batch"))
        {
            return true;
        }
        bool bOwnTransaction = false;
        const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
        if (!Params.IsValid() || (Params->TryGetBoolField(TEXT("transaction"), bOwnTransaction) && bOwnTransaction)
            || !Params->TryGetArrayField(TEXT("commands"), Commands) || !Commands)
        {
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Item : *Commands)
        {
            const TSharedPtr<FJsonObject>* ItemObject = nullptr;
            FString SubCommand;
            if (!Item.IsValid() || !Item->TryGetObject(ItemObject) || !ItemObject
                || !(*ItemObject)->TryGetStringField(TEXT("command_type"), SubCommand)
                || ManagesUndoBuffer(SubCommand) || SubCommand == TEXT("execute_batch"))
            {
                return false;
            }
        }
        return true;
    }
}

TSharedPtr<FJsonObject> UUEAgentBridge::DispatchSingleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params)
{
    // PIE-safety gate. It goes HERE and not in the queued work item -- where the PIE OWNERSHIP
    // guard lives -- because `execute_batch` calls DispatchSingleCommand DIRECTLY: any batch would skip a
    // gate in the work item, and a batch is exactly how a bulk compile gets triggered.
    //
    // It returns BEFORE the TGuardValue and the `goto dispatch_end` jumps of the generated
    // block: a clean return that crosses no initialization. And it returns the OUTER envelope
    // already built (status/error/refused), not a payload, because the refusal is not the
    // result of having run anything.
    if (TSharedPtr<FJsonObject> PieRefusal = CheckPIESafetyGuard(CommandType, Params))
    {
        return PieRefusal;
    }

    // Run every command as an "unattended script". This makes the editor
    // suppress modal dialogs (overwrite/replace/save/checkout/confirm prompts)
    // and auto-answer them, so a command can never block the Game Thread waiting
    // for human input (which would hang the connection / hit the client timeout).
    // Mirrors UE's own editor-scripting subsystems (UEditorAssetSubsystem, AssetTools);
    // AssetToolsImpl::CanShowDialogs() and FMessageDialog both honor this flag.
    TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);

    TSharedPtr<FJsonObject> ResponseJson = MakeShareable(new FJsonObject);

    // collateral-dirty attribution (see McpCollateralDirty near the top of this file).
    // OPT-IN: nothing is bound, recorded or emitted unless this command asked for it
    // (`track_collateral_dirty: true`) or the session enabled `mcp.TrackCollateralDirty 1`.
    // Declared HERE, before the first `goto dispatch_end`, because every jump in the
    // generated block skips forward over this scope -- a declaration below it would be a
    // "jump bypasses variable initialization" compile error.
    const bool bTrackCollateral = McpCollateralDirty::ShouldTrack(Params);
    McpCollateralDirty::FScopedRecorder CollateralRecorder(bTrackCollateral);

    // param-alias rewrite (McpParamAliases.gen.h). The catalog names "the path of a
    // blueprint" in SIX different ways -- blueprint_name, asset_path, path... -- and the
    // argument dictionary of four sibling tools in THREE
    // (parameters / params / args_json), so calls get lost just because of a parameter name.
    //
    // The rewrite happens ONLY under the four conditions in RewriteAliases, and NEVER silently:
    // whatever is rewritten travels back in data.aliased_params, next to data.unknown_params.
    // An alias applied without saying so would be one more tool that "does something other
    // than what you asked and answers yes".
    //
    // Declared HERE for the same reason as bTrackCollateral: every `goto dispatch_end` of the
    // generated block jumps forward over this scope, and a later declaration would be a
    // "jump bypasses variable initialization" error.
    TArray<TSharedPtr<FJsonValue>> AliasedParamsJson;
    McpBridgeParams::RewriteAliases(CommandType, Params, &AliasedParamsJson);

    // Rotation by axis NAME. The {roll,pitch,yaw} object form is rewritten here into the
    // wire array [pitch, yaw, roll], so it works for every entry point: tools/call, in-process
    // callers and each `execute_batch` sub-command. Most handlers that read `rotation` accept the object
    // (mcp_common::ReadJsonRotator); the ones that read it by hand with TryGetArrayField
    // would ignore it SILENTLY -- with this, all of them accept it the same way.
    FMcpToolCatalog::NormalizeRotationParams(CommandType, Params);

    // PRECONDITIONS -- what would KILL the editor never reaches the handler (including
    // the positional Rotator guard; see Mcp/McpPreconditions.h). It goes here, with the aliases
    // already rewritten, so an alias cannot sneak the fatal parameter in, and inside
    // DispatchSingleCommand so `execute_batch` is not a back door. The read probes are this same
    // dispatch, in the same game-thread pass: no race is possible between what the probe sees
    // and what the call would do.
    {
        const McpPreconditions::FProbe Probe = [this](const FString& ProbeCommand, const TSharedPtr<FJsonObject>& ProbeParams) -> TSharedPtr<FJsonObject>
        {
            // The check only needs the asset's class, and the Asset Registry knows it without loading
            // anything (loading a MAP loads the whole world: minutes on a large map). The probe asks
            // with load_full_data=false; if the registry does not know the asset, the handler answers.
            if (ProbeCommand == TEXT("get_asset_by_name"))
            {
                if (TSharedPtr<FJsonObject> FromRegistry = McpPreconditions::AssetMetadataFromRegistry(ProbeParams))
                {
                    return FromRegistry;
                }
            }
            return McpPreconditions::PayloadFromEnvelope(DispatchSingleCommand(ProbeCommand, ProbeParams));
        };
        if (const TOptional<McpPreconditions::FRefusal> Refusal = McpPreconditions::Check(CommandType, Params, Probe))
        {
            UE_LOG(LogTemp, Warning, TEXT("[MCP] '%s' refused before running (%s)"), *CommandType, *Refusal->Check);
            return McpPreconditions::MakeRefusalEnvelope(CommandType, *Refusal);
        }
    }

    try
    {
        TSharedPtr<FJsonObject> ResultJson;
        {
            const bool bWrap = McpUndoStep::ShouldWrap(CommandType, Params);
            // The step is the whole undo entry only when no transaction is open around it (not inside a batch,
            // a begin_transaction or another dispatch): only then may a call that changed nothing drop it.
            const bool bOutermostStep = bWrap && GEditor && GEditor->Trans && !GEditor->IsTransactionActive();
            mcp_common::ConsumeDiscardUndoStepRequest(); // a request left by a probe dispatch is not this call's
            FScopedTransaction UndoStep(TEXT("UEAgent"),
                FText::Format(NSLOCTEXT("UEAgent", "UndoStep", "UEAgent: {0}"), FText::FromString(CommandType)),
                nullptr, bWrap);
            ResultJson = McpCommandFamilies::Run(CommandType, Params);
            // UTransBuffer::Cancel removes the step without applying it, and gives back the redo history its
            // Begin had set aside: right for a step the handler proved holds no change.
            if (mcp_common::ConsumeDiscardUndoStepRequest() && bOutermostStep)
            {
                UndoStep.Cancel();
            }
            // A step that holds the running game's objects is dropped (its changes stay): undoing them means
            // nothing once PIE ends, and keeping them closes the editor when PIE stops.
            else if (bOutermostStep && McpUndoStep::HoldsPieSessionObjects())
            {
                UndoStep.Cancel();
            }
        }

            // collateral-dirty attribution -- harvest (opt-in only; see the
            // McpCollateralDirty comment block near the top of this file for why this uses
            // UPackage's own events instead of a before/after GetDirtyPackages
            // snapshot).
            if (bTrackCollateral)
            {
                CollateralRecorder.Finish();

                const TArray<FString> ReadOnlyRoots = mcp_common::GetDefaultReadonlyRoots();

                // Union of "dirtied from clean" and "already dirty, touched again", each
                // labelled -- a set diff can only ever see the first kind.
                TSet<FName> Touched = CollateralRecorder.MyNew;
                Touched.Append(CollateralRecorder.MyRe);

                TArray<TSharedPtr<FJsonValue>> DirtiedJson;
                TArray<TSharedPtr<FJsonValue>> ReadOnlyJson;
                int32 NumStillDirty = 0;

                for (const FName& PackageFName : Touched)
                {
                    const FString PackageName = PackageFName.ToString();
                    const bool bUnderReadOnly = mcp_common::IsUnderReadOnlyRoot(PackageName, ReadOnlyRoots);

                    // Re-resolve rather than caching a UPackage* across the command: the
                    // pointer could have been GC'd, and a fabricated "still dirty" is
                    // exactly the kind of lie this feature exists to kill.
                    UPackage* Resident = FindPackage(nullptr, *PackageName);
                    const bool bStillDirty = (Resident != nullptr) && Resident->IsDirty();
                    if (bStillDirty) { ++NumStillDirty; }

                    // Guarded: DoesPackageExist LOGS A WARNING for any name it cannot convert
                    // to a mounted path (PackageName.cpp), and a dirty in-memory-only
                    // package is exactly such a name. The guard keeps the log clean and the
                    // answer honest (exists_on_disk:false, no fabricated file_path).
                    FString PackageFile;
                    const bool bExistsOnDisk =
                        FPackageName::IsValidLongPackageName(PackageName) &&
                        FPackageName::DoesPackageExist(PackageName, &PackageFile);

                    TSharedPtr<FJsonObject> Entry = MakeShareable(new FJsonObject);
                    Entry->SetStringField(TEXT("package_name"), PackageName);
                    Entry->SetBoolField(TEXT("newly_dirtied"), CollateralRecorder.MyNew.Contains(PackageFName));
                    Entry->SetBoolField(TEXT("was_already_dirty"), CollateralRecorder.MyRe.Contains(PackageFName));
                    Entry->SetBoolField(TEXT("still_dirty"), bStillDirty);
                    Entry->SetBoolField(TEXT("package_resident"), Resident != nullptr);
                    Entry->SetBoolField(TEXT("saved_during_command"), CollateralRecorder.MySaved.Contains(PackageFName));
                    Entry->SetBoolField(TEXT("under_readonly_root"), bUnderReadOnly);
                    Entry->SetBoolField(TEXT("exists_on_disk"), bExistsOnDisk);
                    if (bExistsOnDisk)
                    {
                        Entry->SetStringField(TEXT("file_path"), PackageFile);
                    }
                    DirtiedJson.Add(MakeShared<FJsonValueObject>(Entry));

                    if (bUnderReadOnly)
                    {
                        ReadOnlyJson.Add(MakeShared<FJsonValueString>(PackageName));
                    }
                }

                // Saves that actually hit disk during this command, whatever API performed
                // them (the exact complement), with the ones under a read-only root apart.
                TArray<TSharedPtr<FJsonValue>> SavedJson;
                TArray<TSharedPtr<FJsonValue>> ReadOnlySavedJson;
                for (const FName& SavedFName : CollateralRecorder.MySaved)
                {
                    const FString SavedName = SavedFName.ToString();
                    SavedJson.Add(MakeShared<FJsonValueString>(SavedName));
                    if (mcp_common::IsUnderReadOnlyRoot(SavedName, ReadOnlyRoots))
                    {
                        ReadOnlySavedJson.Add(MakeShared<FJsonValueString>(SavedName));
                    }
                }

                if (!ResultJson.IsValid())
                {
                    // Only reachable on the opt-in path (a handler that returned null): the
                    // untracked responses are untouched.
                    ResultJson = MakeShareable(new FJsonObject);
                }

                ResultJson->SetArrayField(TEXT("collateral_dirtied_packages"), DirtiedJson);
                ResultJson->SetArrayField(TEXT("readonly_dirtied"), ReadOnlyJson);
                ResultJson->SetBoolField(TEXT("readonly_violation"), ReadOnlyJson.Num() > 0);
                ResultJson->SetArrayField(TEXT("collateral_saved_packages"), SavedJson);
                ResultJson->SetArrayField(TEXT("readonly_saved"), ReadOnlySavedJson);
                ResultJson->SetNumberField(TEXT("collateral_dirtied_count"), DirtiedJson.Num());
                ResultJson->SetNumberField(TEXT("collateral_still_dirty_count"), NumStillDirty);

                // Honesty envelope for the accounting itself: say what was measured and how,
                // so an empty array is never confused with "I did not look".
                TSharedPtr<FJsonObject> Tracking = MakeShareable(new FJsonObject);
                Tracking->SetStringField(TEXT("mechanism"),
                    TEXT("UPackage::PackageMarkedDirtyEvent + PackageDirtyStateChangedEvent + PackageSavedWithContextEvent"));
                Tracking->SetStringField(TEXT("scope"), CommandType);
                Tracking->SetBoolField(TEXT("includes_sub_commands"), true);
                TArray<TSharedPtr<FJsonValue>> RootsJson;
                for (const FString& Root : ReadOnlyRoots)
                {
                    RootsJson.Add(MakeShared<FJsonValueString>(Root));
                }
                Tracking->SetArrayField(TEXT("readonly_roots"), RootsJson);
                Tracking->SetStringField(TEXT("caveat"),
                    TEXT("Attribution is by wall-clock overlap on the game thread: anything that dirties a package while this command runs is attributed to it. A package dirtied through neither MarkPackageDirty nor SetDirtyFlag (there is no such engine path today) would be missed."));
                ResultJson->SetObjectField(TEXT("collateral_tracking"), Tracking);
            }

            // unknown-param report. No command validates unknown keys: a `TryGet*Field`
            // that fails leaves the default SILENTLY, so a typo in a key reads the same as
            // "that option did nothing". A real consequence: build_blueprint_graph can kill
            // the editor with an EXCEPTION_ACCESS_VIOLATION when its params are sent at the
            // root level instead of inside params:{}.
            //
            // This is the ONLY place all responses go through -- including those of `execute_batch`
            // sub-commands, which re-enter here through DispatchSingleCommand and therefore
            // each get THEIR OWN verdict on THEIR OWN params (the batch also stamps its own on
            // its own response; they are distinct objects, so there is no double stamping and
            // no recursion: CollectUnknownParams dispatches nothing).
            //
            // Rules, in order of importance:
            //   1. It NEVER refuses. An unknown parameter is REPORTED and the command runs
            //      anyway: refusing would break existing callers.
            //   2. A command that is NOT in the table (absent from the catalog) => NOTHING
            //      is added. No verdict is invented
            //      about something unknown.
            //   3. The `data` object is only created when there is something to report.
            // Alias report. Same three rules as unknown_params (additive, inside `data`,
            // created only when there is something to say). It goes FIRST so that, if a
            // response carries both, the `data` object already exists and is not created twice.
            if (ResultJson.IsValid() && AliasedParamsJson.Num() > 0)
            {
                const TSharedPtr<FJsonObject>* ExistingData = nullptr;
                if (ResultJson->TryGetObjectField(TEXT("data"), ExistingData)
                    && ExistingData != nullptr && ExistingData->IsValid())
                {
                    (*ExistingData)->SetArrayField(TEXT("aliased_params"), AliasedParamsJson);
                }
                else
                {
                    TSharedPtr<FJsonObject> NewData = MakeShareable(new FJsonObject);
                    NewData->SetArrayField(TEXT("aliased_params"), AliasedParamsJson);
                    ResultJson->SetObjectField(TEXT("data"), NewData);
                }
            }

            if (ResultJson.IsValid() && Params.IsValid())
            {
                TArray<FString> UnknownParams;
                if (McpCommandParamTable::CollectUnknownParams(CommandType, Params, UnknownParams)
                    && UnknownParams.Num() > 0)
                {
                    TArray<TSharedPtr<FJsonValue>> UnknownJson;
                    UnknownJson.Reserve(UnknownParams.Num());
                    for (const FString& UnknownName : UnknownParams)
                    {
                        UnknownJson.Add(MakeShared<FJsonValueString>(UnknownName));
                    }

                    // Additive: the report lives INSIDE `data` (the payload slot of
                    // mcp_common::MakeOkReply/MakeOutcomeReply/
                    // MakeVerifiedReply), so no top-level field moves. If the response
                    // has no `data` (e.g. MakeErrorReply or a flat payload), it is
                    // created NOW, which is when there is something to put in it.
                    const TSharedPtr<FJsonObject>* ExistingData = nullptr;
                    if (ResultJson->TryGetObjectField(TEXT("data"), ExistingData)
                        && ExistingData != nullptr && ExistingData->IsValid())
                    {
                        (*ExistingData)->SetArrayField(TEXT("unknown_params"), UnknownJson);
                    }
                    else
                    {
                        TSharedPtr<FJsonObject> NewData = MakeShareable(new FJsonObject);
                        NewData->SetArrayField(TEXT("unknown_params"), UnknownJson);
                        ResultJson->SetObjectField(TEXT("data"), NewData);
                    }
                }
            }

            // OFFLOADING of giant responses. Responses past a few tens of thousands of
            // characters get cut by the client harness (and dumped to a raw .txt), mostly the
            // listing tools: get_actors_in_level, list_all_input_mappings, get_graph_nodes.
            //
            // Per-tool caps do not solve it: a caller CAN pass max_* and still overflow,
            // because the cap cannot be chosen before seeing the size. It is solved at the
            // single point every command goes through, with the pattern the plugin already
            // uses (capture_pie_state / audit_*): the detail is written to Saved/MCPAudits/ and
            // the response keeps the SCALARS -- the counters, which are what is asked about
            // most of the time -- plus the length of each array moved out.
            if (ResultJson.IsValid())
            {
                McpOversize::MaybeOffload(CommandType, ResultJson);
            }

            UEAgentBridgeImpl::WrapHandlerResult(ResponseJson, ResultJson);
        }
        catch (const std::exception& e)
        {
            UEAgentBridgeImpl::SetFailure(ResponseJson, UTF8_TO_TCHAR(e.what()));
        }

        // Whatever changes the risk of the NEXT calls (e.g. the World Partition map that stays
        // resident and that load_level must not open from now on). Only looks at successes.
        McpPreconditions::NoteResult(CommandType, Params, ResponseJson);
        // And what the READBACK contract needs to remember (compiled Blueprint, PIE census). It
        // goes here and not only in tools/call so a compile done inside a batch is seen too.
        {
            bool bIgnoredRefusal = false;
            McpReadback::NoteResult(CommandType, Params, McpReadback::Unwrap(ResponseJson, bIgnoredRefusal));
        }

        return ResponseJson;
}

// tools/call of the native MCP server: the normal dispatch plus the READBACK contract, all in
// the SAME game-thread pass -- the before snapshot, the command, the re-read and the offload
// note see exactly the same state, with no race from network round trips in between.
TFuture<FString> UUEAgentBridge::EnqueueToolCall(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& SessionId)
{
    TPromise<FString> Promise;
    TFuture<FString> Future = Promise.GetFuture();

    TUniqueFunction<void()> Work = [this, CommandType, Params, SessionId, Promise = MoveTemp(Promise)]() mutable
    {
        const FDateTime Started = FDateTime::Now();
        const double StartSeconds = FPlatformTime::Seconds();

        // The contract's re-reads are this same dispatch, unwrapped the way the model sees it.
        const McpReadback::FSend Send = [this](const FString& ProbeCommand, const TSharedPtr<FJsonObject>& ProbeParams) -> TSharedPtr<FJsonObject>
        {
            bool bIgnoredRefusal = false;
            return McpReadback::Unwrap(DispatchSingleCommand(ProbeCommand, ProbeParams), bIgnoredRefusal);
        };

        // The contract reads a COPY (see ContractView): the dispatch rewrites aliases and
        // rotations IN PLACE on Params.
        const TSharedPtr<FJsonObject> ContractParams = McpBridgeParams::ContractView(CommandType, Params);

        // The before snapshot is the ONE part of the contract that cannot be taken after the write.
        TSharedPtr<FJsonValue> Before;
        if (McpReadback::Tables().Before.Contains(CommandType))
        {
            Before = McpReadback::TakeBefore(CommandType, ContractParams, Send);
        }

        TSharedPtr<FJsonObject> Envelope = CheckPIEOwnershipGuard(CommandType, Params, SessionId);
        if (!Envelope.IsValid())
        {
            Envelope = DispatchSingleCommand(CommandType, Params); // rewrites aliases and rotations IN PLACE
        }

        bool bRefused = false;
        TSharedPtr<FJsonObject> Result = McpReadback::Unwrap(Envelope, bRefused);
        // A PRECONDITION refusal never ran: there is nothing to verify (it never reaches the
        // readback). The PIE guards' refusals do go through: ON_FAILURE explains why their
        // stated reason is not the real one.
        const TSharedPtr<FJsonObject> RefusalInfo = McpPy::AsDict(McpPy::Get(Result, TEXT("result")));
        const bool bPrecondition = bRefused && McpPy::TextOf(McpPy::Get(RefusalInfo, TEXT("refused_by"))) == TEXT("precondition");
        if (!bPrecondition && McpReadback::HasContract(CommandType))
        {
            Result = McpReadback::Verify(CommandType, ContractParams, Result, Send, Before);
        }
        Result = McpReadback::AnnotateOffload(CommandType, Result);
        McpActivity::Record(CommandType, Started, (FPlatformTime::Seconds() - StartSeconds) * 1000.0, Result, bRefused);

        TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
        Outcome->SetBoolField(TEXT("is_error"), bRefused);
        Outcome->SetObjectField(TEXT("result"), Result);
        FString OutcomeText;
        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutcomeText);
        FJsonSerializer::Serialize(Outcome.ToSharedRef(), Writer);
        Promise.SetValue(OutcomeText);
    };

    {
        FScopeLock Lock(&PendingCommandsLock);
        PendingCommands.Add(MoveTemp(Work));
    }
    return Future;
}

// Drain queued commands on the game thread. Registered on the core ticker, so it
// runs at FEngineLoop's top-level tick -- outside Mass/TEDS processing -- which is
// what makes editor actor mutation (delete/spawn) safe. Returns true to keep ticking.
bool UUEAgentBridge::DrainCommandQueue(float /*DeltaTime*/)
{
    TArray<TUniqueFunction<void()>> ToRun;
    {
        FScopeLock Lock(&PendingCommandsLock);
        Swap(ToRun, PendingCommands);
    }
    for (TUniqueFunction<void()>& Work : ToRun)
    {
        Work();
    }
    return true;
}