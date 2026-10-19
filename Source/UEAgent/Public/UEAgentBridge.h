#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "Containers/Ticker.h"
#include "Async/Future.h"           // TFuture -- EnqueueToolCall's return type
#include "Http.h"
#include "Json.h"
#include "UEAgentBridge.generated.h"

// Native MCP server (Streamable HTTP) -- see Mcp/McpHttpServer.h.
class FMcpHttpServer;

/**
 * The plugin's editor subsystem: owns the command handlers and the MCP server that reaches them.
 * Every request funnels into ONE game-thread queue, where each command runs through the
 * preconditions, its handler, and the readback contract before the answer goes back.
 */
UCLASS()
class UEAGENT_API UUEAgentBridge : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Registers the editor delegates the guards need (PIE start/end, map loads) and starts the
	 * MCP HTTP server unless the settings disable it.
	 */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	/** Stops the server and drains the queue before the module can unload. */
	virtual void Deinitialize() override;

	// --- Native MCP server (HTTP) -----------------------------------------------------------
	// Serves MCP directly from the editor (Mcp/McpHttpServer.h) through the command queue.
	// Loads the tool catalog on start, so restarting it (console: `mcp.http.restart`) also
	// reloads Resources/command_catalog.json.
	// Game thread.
	bool StartHttpServer();
	void StopHttpServer();
	bool IsHttpRunning() const;
	/** Does tools/list carry the compact meta-tools by default (UEAGENT_TOOLS=compact)? */
	bool IsHttpCompactTools() const;
	/** One readable line: port, connections, requests, catalog tools. */
	FString DescribeHttpStatus() const;
	/** Why it failed to start last time (empty if it started or is disabled). Shown by the status bar. */
	const FString& GetLastHttpError() const { return LastHttpError; }
	/** The port it serves on, or the configured one if it is stopped. */
	uint16 GetHttpPort() const;
	/** HTTP server counters (zero if it is stopped). Game thread. */
	void GetHttpCounters(int32& OutConnections, int64& OutRequests, int64& OutToolCalls) const;

	// Running a command
	// tools/call of the native MCP server: the dispatch plus the READBACK contract (before
	// snapshot, re-read, offload note) in the SAME game-thread pass. The future carries
	// {"is_error": bool, "result": <what the model sees>}.
	TFuture<FString> EnqueueToolCall(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& SessionId);

	// Dispatches ONE command by name on the CALLING thread (no thread hop, no queueing) and
	// returns the {"status":"success"|"error", ...} envelope. Game thread only: it runs from a
	// queued work item, or in a loop for `execute_batch`. A handler must never wait on the queue: the
	// queue is drained by the very call stack that is running the handler.
	TSharedPtr<FJsonObject> DispatchSingleCommand(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

private:
	// --- PIE ownership guard (game thread only -> no locks) --------------------
	// So that concurrent sessions don't yank each other's Play-in-Editor out from
	// under them, start_/stop_play_in_editor are gated by which session_id owns the
	// running PIE. All of this is touched ONLY on the game thread: the guard runs in
	// the ticker-drained Work lambda, and the FEditorDelegates hooks fire on the game
	// thread -- so no locking is needed.
	//
	// The session_id that owns the currently-running PIE. Empty ("") means a human
	// started it from the UI, or a caller with no session_id -- either way it
	// is "unknown", and any DIFFERENT (non-empty) session is refused. Set by
	// OnPostPIEStarted from PendingPIEOwner; cleared by OnEndPIE.
	FString PIEOwnerSessionId;
	// Staging slot: the session_id of an in-flight start_play_in_editor request.
	// RequestPlaySession is async (PIE starts a tick later), so we stash the owner
	// here and OnPostPIEStarted promotes it to PIEOwnerSessionId once PIE is live.
	FString PendingPIEOwner;
	FDelegateHandle PostPIEStartedHandle;
	FDelegateHandle EndPIEHandle;
	// Fires as the FIRST statement of FEditorFileUtils::LoadMap, whoever opens the map --
	// Content Browser and File menu included, not just our own load_level. That is the only
	// way to cover map loads whose callstack is ContentBrowser -> AssetTools -> UnrealEd,
	// without a single UEAgent frame.
	FDelegateHandle LoadMapStartHandle;

	// FEditorDelegates hooks (game thread) -- record/clear the PIE owner.
	void OnPostPIEStarted(bool bIsSimulating);
	void OnEndPIE(bool bIsSimulating);

	// PIE ownership gate for start_/stop_play_in_editor. Returns a fully-formed OUTER
	// error envelope to REFUSE (the Work lambda serializes it and skips dispatch), or
	// null to PROCEED. For an allowed start it also stamps PendingPIEOwner. Game thread
	// only (reads GEditor + the owner state above). See the .cpp for the exact rules.
	TSharedPtr<FJsonObject> CheckPIEOwnershipGuard(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FString& SessionId);

	// --- PIE SAFETY gate (separate from the ownership guard above) ---------------
	// The one above answers "is this PIE YOURS?"; this one answers "can this be done WITH
	// PIE running, whoever owns it?". With PIE live, two families of commands stop being
	// reliable, and both fail SILENTLY:
	//   - compiling a Blueprint reinstances its live actors, and the `UREINST_*` the
	//     reinstancer leaves behind can retain a world -> CheckForWorldGCLeaks is Fatal and
	//     the editor DIES (a Fatal log, not an Error);
	//   - saving is refused by the engine (Editor.AllowSavingAssetsDuringPIE=0) BEFORE it
	//     tries anything, and a `{saved:false}` would come back wrapped in `status:success`.
	// The table of which command is which is DERIVED from the C++ (McpPieSafety.gen.h,
	// generated by Tools/codegen_pie_safety.py) so it does not rot as commands are added.
	//
	// Returns the refusal envelope, or null to PROCEED. `force:true` bypasses it -- the same
	// escape hatch as the ownership guard. It sits at the start of DispatchSingleCommand, not in
	// the queued work item: `execute_batch` calls DispatchSingleCommand directly, so a gate in the work
	// item would be skipped by a batch -- which is exactly how a bulk compile gets triggered.
	TSharedPtr<FJsonObject> CheckPIESafetyGuard(const FString& CommandType, const TSharedPtr<FJsonObject>& Params);

	// Command marshaling. The network thread enqueues each command's work item
	// here; an FTSTicker drains it on the game thread at the top-level frame point
	// (FEngineLoop::Tick), which is OUTSIDE Mass/TEDS processing. AsyncTask(GameThread)
	// could otherwise be retired nested inside a Mass processor tick (Mass pumps the
	// task graph), and an editor actor mutation there hits the engine assert
	// "FMassEntityManager::AddFragmentToEntity called during mass processing".
	TArray<TUniqueFunction<void()>> PendingCommands;
	FCriticalSection PendingCommandsLock;
	FTSTicker::FDelegateHandle CommandTickerHandle;
	bool DrainCommandQueue(float DeltaTime);
	// The ticker is registered while the server runs and removed (after a last drain) once it
	// stops, so no request is left queued with nothing to run it.
	void EnsureCommandTicker();
	void ReleaseCommandTickerIfIdle();

	// HTTP server. Raw pointer: StartHttpServer/StopHttpServer create and destroy it, and the
	// header does not need the complete type.
	FMcpHttpServer* HttpServer = nullptr;
	FString LastHttpError;
	// Notification visible in the editor (besides the log): a failed bind must not be silent.
	void NotifyEditor(const FString& Message, bool bIsError);
};