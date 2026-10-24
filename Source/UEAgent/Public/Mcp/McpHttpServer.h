// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter64.h"

class FSocket;
class FRunnableThread;
class UUEAgentBridge;
class FMcpHttpAcceptor;
class FMcpHttpConnection;

/**
 * Which tools tools/list carries for a client: the core tools plus the meta-tools (the default; enable_toolset adds
 * domains per session), every tool, or the compact meta-tools alone. UEAGENT_TOOLS=core|all|compact sets the server's
 * default and a client's URL can ask for another (?tools=core|all|compact; `full` is another spelling of `all`).
 */
enum class EMcpToolsMode : uint8
{
	Core,
	All,
	Compact,
};

/**
 * Native MCP server: Streamable HTTP served from the editor itself, with no external process in
 * front of it. It speaks the protocol era that uses `initialize` (2024-11-05 ... 2025-11-25),
 * which Claude Code falls back to for compatibility since the 2026-07-28 spec revision.
 *
 * THREADING -- the decision that sets it apart from Epic's MCP server (ModelContextProtocol, on
 * the HTTPServer module): that one processes everything on the GAME THREAD, so with the editor
 * busy it does not even answer `initialize`. Here there is an acceptor plus one thread per
 * connection: `initialize`, `ping` and `tools/list` are answered on the network thread, and only
 * `tools/call` crosses to the game thread, through the bridge's single command queue
 * (UUEAgentBridge::EnqueueToolCall) -- the same guards, order and serialization for every call.
 *
 * SECURITY: 127.0.0.1 only, and `Host` and `Origin` are validated on every request. With
 * `execute_python` allowed without confirmation, any web page could drive the editor through DNS
 * rebinding if they were not checked (the MCP spec requires validating Origin).
 */
class UEAGENT_API FMcpHttpServer
{
public:
	explicit FMcpHttpServer(UUEAgentBridge* InBridge);
	~FMcpHttpServer();

	/**
	 * Opens 127.0.0.1:InPort. Returns false with OutError if it cannot (e.g. the port is in use).
	 * Empty InAuthToken = no authentication; otherwise every request must carry `Authorization: Bearer`.
	 * UEAGENT_TOOLS=all|compact in the environment changes the default list (EMcpToolsMode). Game thread (reads the
	 * plugin version).
	 */
	bool Start(uint16 InPort, const FString& InAuthToken, FString& OutError);
	/** Closes everything and waits for the threads. Call from the game thread. */
	void Stop();

	bool IsRunning() const { return bRunning; }
	uint16 GetPort() const { return Port; }
	const FString& GetAuthToken() const { return AuthToken; }
	const FString& GetServerVersion() const { return ServerVersion; }
	/** The list tools/list carries when a client's URL asks for none (?tools=core|all|compact). */
	EMcpToolsMode GetDefaultToolsMode() const { return DefaultToolsMode; }
	int32 GetNumConnections();
	int64 GetNumRequests() const { return NumRequests.GetValue(); }
	int64 GetNumToolCalls() const { return NumToolCalls.GetValue(); }

	/** Cap on simultaneous connections: each session's HTTP client opens several. */
	static constexpr int32 MaxConnections = 64;

	// --- internal use by the server threads ---------------------------------------------
	UUEAgentBridge* GetBridge() const { return Bridge; }
	void RunAcceptLoop(const FThreadSafeBool& bAcceptorRunning);
	void CountRequest() { NumRequests.Increment(); }
	void CountToolCall() { NumToolCalls.Increment(); }

	/**
	 * Sessions: `initialize` on the core list opens one and answers its id in Mcp-Session-Id, which the client sends
	 * back on every request; the session remembers the packs and domains enable_toolset added to that client's list. The server
	 * requires no session: a request without one, or with one it does not know (the editor restarted), gets the list
	 * its URL asks for. At most MaxSessions are kept, the least recently used dropped first. Any thread.
	 */
	FString OpenSession();
	/** The packs and domains Session enabled (FMcpToolCatalog::FindToolset names); false when the server does not know Session. */
	bool GetSessionDomains(const FString& Session, TSet<FString>& OutDomains);
	/** Adds Domains to Session's list, opening it under that id if the server does not know it. */
	void AddSessionDomains(const FString& Session, const TArray<FString>& Domains);

	static constexpr int32 MaxSessions = 256;

private:
	void ReapFinishedConnections();

	struct FSession
	{
		TSet<FString> Domains;
		double LastUsed = 0.0;
	};
	/** Drops the least recently used sessions above MaxSessions. SessionsLock held. */
	void TrimSessions();

	struct FConnectionHandle
	{
		FRunnableThread* Thread = nullptr;
		FMcpHttpConnection* Runnable = nullptr;
	};

	UUEAgentBridge* Bridge;
	TSharedPtr<FSocket> Listener;
	FMcpHttpAcceptor* AcceptorRunnable = nullptr;
	FRunnableThread* AcceptorThread = nullptr;
	TArray<FConnectionHandle> Connections;
	FCriticalSection ConnectionsLock;
	FThreadSafeBool bRunning;
	uint16 Port = 0;
	// Set in Start() and read-only while the server runs: the threads read them without a lock.
	FString AuthToken;
	FString ServerVersion;
	EMcpToolsMode DefaultToolsMode = EMcpToolsMode::Core;
	TMap<FString, FSession> Sessions;
	FCriticalSection SessionsLock;
	uint32 NextConnectionId = 0;
	FThreadSafeCounter64 NumRequests;
	FThreadSafeCounter64 NumToolCalls;
};
