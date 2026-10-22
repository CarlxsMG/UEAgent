// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpHttpServer.h"

#include "Mcp/McpMetaTools.h"
#include "Mcp/McpToolCatalog.h"
#include "UEAgentBridge.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "SocketTypes.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeLock.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Interfaces/IPluginManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEAgentHttp, Log, All);

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpHttpServerPrivate
{
	// The protocol era that uses `initialize`. If the client asks for another version, we answer
	// with the newest one and the client decides (spec 2025-11-25, "Version Negotiation").
	static const TCHAR* const SupportedProtocolVersions[] = {
		TEXT("2025-11-25"), TEXT("2025-06-18"), TEXT("2025-03-26"), TEXT("2024-11-05") };
	static const TCHAR* const LatestProtocolVersion = TEXT("2025-11-25");

	// Identity seen by the PIE ownership guard (CheckPIEOwnershipGuard). ONE for all HTTP
	// clients: every session served over HTTP shares this single session_id (so the guard
	// treats them all as the same owner).
	static const TCHAR* const HttpSessionId = TEXT("mcp-http");

	static const TCHAR* const McpPath = TEXT("/mcp");

	// What every agent gets in `initialize`: how to read the answers, and how to avoid the usual refusals.
	static const TCHAR* const AgentInstructions =
		TEXT("UEAgent drives the Unreal Editor this server runs in. Tool calls run on the editor's game thread, one at a time.\n")
		TEXT("- Read every answer: its success is checked. A `readback` whose `verdict` is not `ok` means the call did not do what ")
		TEXT("it reported; its `note` says why and how to check. `unknown_params` lists the arguments the tool ignored.\n")
		TEXT("- A refusal (isError) comes before the call runs: the call is known to crash or freeze the editor, or is unreliable ")
		TEXT("while Play-in-Editor runs. The message says what to do instead; stop PIE before compiling Blueprints or saving.\n")
		TEXT("- Look assets and actors up instead of guessing their names, and name assets by their full path (/Game/...).\n")
		TEXT("- A tool's name starts with its verb, one verb per operation: get_ reads one thing (get_<x>_info, all of it), list_ ")
		TEXT("enumerates, find_ searches by name or condition, is_/has_ answer yes or no; create_ makes an asset, add_ puts a part ")
		TEXT("into an asset or actor, spawn_ places an actor in a world; set_ writes; remove_ takes a part out, delete_ deletes an ")
		TEXT("asset or actor, clear_ empties a list; diff_ compares two things; execute_ runs the Python, console command or batch ")
		TEXT("you pass, run_ starts a test, query, tree or utility that already exists.\n")
		TEXT("- Similar tools: get_properties reads named properties of one object you can path to, get_object_info one you know ")
		TEXT("nothing about, get_actor_properties_by_class placed actors by class or label, get_component_properties a whole ")
		TEXT("component. set_properties writes any setting of an asset, an actor or a component (\"Actor.Component\", by name or ")
		TEXT("class) -- a light, fog, sky, decal, camera, post-process volume, sound, Niagara system... -- and saves an asset with ")
		TEXT("save=true; set_component_property writes a component in a live PIE world. spawn_actor places an actor of any class ")
		TEXT("with its properties, spawn_box_volume a volume. A typed tool says in its description what it does that these do not.\n")
		TEXT("- For many small edits, use `execute_batch`: one call, one pass on the game thread.\n")
		TEXT("- After editing a Blueprint graph, compile it and read the compiler's messages.\n")
		TEXT("- When no tool fits, `execute_python` runs Unreal's Python API in the editor.");

	constexpr int32 MaxHeaderBytes = 64 * 1024;
	constexpr int64 MaxBodyBytes = 256ll * 1024 * 1024;   // a larger request body is refused
	constexpr double IdleCloseSeconds = 60.0;             // an idle keep-alive connection is closed after 60 s...
	constexpr int32 KeepAliveHintSeconds = 30;            // ...and the client is told 30, so it does not reuse it right as we close it
	constexpr double ReadStallSeconds = 30.0;             // a half-received request that makes no progress
	constexpr double SseAfterSeconds = 10.0;              // with a progressToken, switch to SSE after this
	constexpr double ProgressEverySeconds = 15.0;         // then a progress notification at least this often
	constexpr int32 RecvChunkBytes = 64 * 1024;

	struct FHttpRequest
	{
		FString Method;
		FString Path;
		/** What follows the '?' of the target, if anything. */
		FString Query;
		bool bHttp11 = true;
		/** Lower-case names; repeated headers are joined with ", ". */
		TMap<FString, FString> Headers;
		TArray<uint8> Body;

		const FString* Header(const TCHAR* LowerName) const { return Headers.Find(LowerName); }
	};

	/** A `tools` value (core, all, full, compact) as a mode; Default, and bOutKnown false, for any other. */
	static EMcpToolsMode ParseToolsMode(const FString& Value, EMcpToolsMode Default, bool& bOutKnown)
	{
		bOutKnown = true;
		if (Value.Equals(TEXT("core"), ESearchCase::IgnoreCase))
		{
			return EMcpToolsMode::Core;
		}
		if (Value.Equals(TEXT("all"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("full"), ESearchCase::IgnoreCase))
		{
			return EMcpToolsMode::All;
		}
		if (Value.Equals(TEXT("compact"), ESearchCase::IgnoreCase))
		{
			return EMcpToolsMode::Compact;
		}
		bOutKnown = false;
		return Default;
	}

	static const TCHAR* ToolsModeName(EMcpToolsMode Mode)
	{
		return Mode == EMcpToolsMode::All ? TEXT("all") : Mode == EMcpToolsMode::Compact ? TEXT("compact") : TEXT("core");
	}

	/** What the URL asks for: the list (`tools=core|all|compact`, else the server's default) and the domains to add to the core list (`domains=A,B`). */
	struct FToolsRequest
	{
		EMcpToolsMode Mode = EMcpToolsMode::Core;
		TSet<FString> Domains;
	};

	static FToolsRequest ReadToolsRequest(const FHttpRequest& Request, EMcpToolsMode Default, const FMcpToolCatalog* Catalog)
	{
		FToolsRequest Out;
		Out.Mode = Default;
		TArray<FString> Pairs;
		Request.Query.ParseIntoArray(Pairs, TEXT("&"));
		for (const FString& Pair : Pairs)
		{
			FString Key, Value;
			if (!Pair.Split(TEXT("="), &Key, &Value))
			{
				continue;
			}
			if (Key.Equals(TEXT("tools"), ESearchCase::IgnoreCase))
			{
				bool bKnown = false;
				Out.Mode = ParseToolsMode(Value, Out.Mode, bKnown);
			}
			else if (Key.Equals(TEXT("domains"), ESearchCase::IgnoreCase) && Catalog)
			{
				TArray<FString> Names;
				Value.Replace(TEXT("%2C"), TEXT(","), ESearchCase::IgnoreCase).ParseIntoArray(Names, TEXT(","));
				for (const FString& Name : Names)
				{
					const FString Domain = Catalog->FindDomain(Name);
					if (!Domain.IsEmpty())
					{
						Out.Domains.Add(Domain);
					}
				}
			}
		}
		return Out;
	}

	/** The session id the request carries (Mcp-Session-Id), or empty. */
	static FString SessionOf(const FHttpRequest& Request)
	{
		const FString* Session = Request.Header(TEXT("mcp-session-id"));
		return Session ? Session->TrimStartAndEnd() : FString();
	}

	static bool IsSupportedVersion(const FString& Version)
	{
		for (const TCHAR* Supported : SupportedProtocolVersions)
		{
			if (Version == Supported)
			{
				return true;
			}
		}
		return false;
	}

	static FString ToJsonText(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	static TArray<uint8> ToUtf8(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	static FString FromUtf8(const uint8* Data, int32 Count)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data), Count);
		return FString(Converted.Length(), Converted.Get());
	}

	static TSharedRef<FJsonObject> MakeEnvelope(const TSharedPtr<FJsonValue>& Id)
	{
		TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Envelope->SetField(TEXT("id"), Id.IsValid() ? Id : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
		return Envelope;
	}

	static FString MakeRpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Envelope = MakeEnvelope(Id);
		Envelope->SetObjectField(TEXT("result"), Result);
		return ToJsonText(Envelope);
	}

	static FString MakeRpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Message);
		TSharedRef<FJsonObject> Envelope = MakeEnvelope(Id);
		Envelope->SetObjectField(TEXT("error"), Error);
		return ToJsonText(Envelope);
	}

	/** CallToolResult with a single text block. */
	static TSharedRef<FJsonObject> MakeToolResult(const FString& Text, bool bIsError)
	{
		TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		TArray<TSharedPtr<FJsonValue>> Content;
		Content.Add(MakeShared<FJsonValueObject>(Block));
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("content"), Content);
		Result->SetBoolField(TEXT("isError"), bIsError);
		return Result;
	}

	/**
	 * What EnqueueToolCall returns: {"is_error": bool, "result": <what the model sees>}. The
	 * bridge envelope is already unwrapped (McpReadback::Unwrap) and has the READBACK contract
	 * applied.
	 * A `success:false` is NOT flagged isError: the model reads that field itself. isError is
	 * reserved for calls that never ran: the refusals (`result.refused`, from the PIE guards and
	 * the preconditions), which come back as a tool error.
	 */
	static TSharedRef<FJsonObject> UnwrapOutcome(const FString& OutcomeText, bool& bOutRefused)
	{
		bOutRefused = false;
		TSharedPtr<FJsonObject> Outcome;
		const TSharedPtr<FJsonObject>* Result = nullptr;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OutcomeText), Outcome) || !Outcome.IsValid()
			|| !Outcome->TryGetObjectField(TEXT("result"), Result) || !Result || !Result->IsValid())
		{
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetBoolField(TEXT("success"), false);
			Error->SetStringField(TEXT("error"), TEXT("Invalid response from the Unreal bridge (not JSON)"));
			return Error;
		}
		Outcome->TryGetBoolField(TEXT("is_error"), bOutRefused);
		return Result->ToSharedRef();
	}

	static bool IsLoopbackHost(const FString& HostOnly)
	{
		return HostOnly.Equals(TEXT("127.0.0.1"))
			|| HostOnly.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
			|| HostOnly.Equals(TEXT("[::1]"));
	}

	/** "host[:port]" -> host (keeping the brackets for IPv6). */
	static FString StripPort(const FString& HostPort)
	{
		if (HostPort.StartsWith(TEXT("[")))
		{
			int32 Close = INDEX_NONE;
			return HostPort.FindChar(TEXT(']'), Close) ? HostPort.Left(Close + 1) : HostPort;
		}
		int32 Colon = INDEX_NONE;
		return HostPort.FindLastChar(TEXT(':'), Colon) ? HostPort.Left(Colon) : HostPort;
	}

	/** Host must be the loopback: under DNS rebinding the browser sends the attacker's domain. */
	static bool IsAllowedHostHeader(const FString* Host, uint16 Port)
	{
		if (!Host)
		{
			return false; // HTTP/1.1 requires it
		}
		const FString Value = Host->TrimStartAndEnd();
		const FString HostOnly = StripPort(Value);
		if (!IsLoopbackHost(HostOnly))
		{
			return false;
		}
		return Value.Len() == HostOnly.Len() || Value.Mid(HostOnly.Len() + 1) == FString::FromInt(Port);
	}

	/** No Origin (a non-browser client) or a loopback Origin. Any web page is rejected. */
	static bool IsAllowedOrigin(const FString* Origin)
	{
		if (!Origin)
		{
			return true;
		}
		const FString Value = Origin->TrimStartAndEnd();
		FString Rest;
		if (Value.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
		{
			Rest = Value.Mid(7);
		}
		else if (Value.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
		{
			Rest = Value.Mid(8);
		}
		else
		{
			return false; // "null", file://, unusual schemes
		}
		int32 Slash = INDEX_NONE;
		if (Rest.FindChar(TEXT('/'), Slash))
		{
			Rest = Rest.Left(Slash);
		}
		return IsLoopbackHost(StripPort(Rest));
	}

	static const TCHAR* StatusText(int32 Status)
	{
		switch (Status)
		{
		case 200: return TEXT("OK");
		case 202: return TEXT("Accepted");
		case 400: return TEXT("Bad Request");
		case 401: return TEXT("Unauthorized");
		case 403: return TEXT("Forbidden");
		case 404: return TEXT("Not Found");
		case 405: return TEXT("Method Not Allowed");
		case 408: return TEXT("Request Timeout");
		case 413: return TEXT("Payload Too Large");
		case 431: return TEXT("Request Header Fields Too Large");
		case 503: return TEXT("Service Unavailable");
		default:  return TEXT("Internal Server Error");
		}
	}

	static int32 HexDigit(TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) { return C - TEXT('0'); }
		if (C >= TEXT('a') && C <= TEXT('f')) { return 10 + C - TEXT('a'); }
		if (C >= TEXT('A') && C <= TEXT('F')) { return 10 + C - TEXT('A'); }
		return -1;
	}

	static void DestroySocket(FSocket* Socket)
	{
		if (Socket)
		{
			if (ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				Subsystem->DestroySocket(Socket);
			}
		}
	}
}

// ===========================================================================
// Acceptor: one thread that only accepts; each connection gets its own thread.
// ===========================================================================

class FMcpHttpAcceptor : public FRunnable
{
public:
	explicit FMcpHttpAcceptor(FMcpHttpServer* InServer)
		: Server(InServer)
		, bAcceptorRunning(true)
	{
	}

	virtual uint32 Run() override
	{
		Server->RunAcceptLoop(bAcceptorRunning);
		return 0;
	}

	virtual void Stop() override
	{
		bAcceptorRunning = false;
	}

private:
	FMcpHttpServer* Server;
	FThreadSafeBool bAcceptorRunning;
};

// ===========================================================================
// Connection: HTTP/1.1 with keep-alive, one request at a time.
// ===========================================================================

class FMcpHttpConnection : public FRunnable
{
public:
	FMcpHttpConnection(FMcpHttpServer* InServer, TSharedPtr<FSocket> InSocket)
		: Server(InServer)
		, Socket(MoveTemp(InSocket))
		, bConnectionRunning(true)
		, bFinished(false)
	{
	}

	virtual uint32 Run() override;

	virtual void Stop() override
	{
		bConnectionRunning = false;
		// Wakes a Wait()/Recv()/Send() blocked on this socket.
		if (Socket.IsValid())
		{
			Socket->Close();
		}
	}

	/** The last thing Run() does: from then on the server may delete this object. */
	bool IsFinished() const { return bFinished; }

private:
	enum class EReadResult { Ok, Closed, BadRequest, HeadersTooLarge, BodyTooLarge, Stalled };

	bool KeepGoing() const { return bConnectionRunning && Server->IsRunning(); }
	bool FillBuffer(double MaxWaitSeconds);
	int32 FindCrlf(int32 From) const;
	int32 FindHeaderEnd() const;
	void Consume(int32 Count) { Buffer.RemoveAt(0, Count, EAllowShrinking::No); }
	EReadResult ReadRequest(McpHttpServerPrivate::FHttpRequest& Out);
	EReadResult ReadChunkedBody(TArray<uint8>& OutBody);

	bool SendBytes(const uint8* Data, int32 Count);
	bool SendResponse(int32 Status, const TCHAR* ContentType, const TArray<uint8>& Body, bool bKeepAlive, const FString& ExtraHeaders = FString());
	bool SendJson(int32 Status, const FString& Json, bool bKeepAlive);
	bool SendEmpty(int32 Status, bool bKeepAlive, const FString& ExtraHeaders = FString());
	bool SendSseHead();
	bool SendSseEvent(const FString& Json);
	bool PeerHasClosed();

	/** Answers the request. Returns whether the connection stays alive for the next one. */
	bool HandleRequest(const McpHttpServerPrivate::FHttpRequest& Request);
	bool HandleJsonRpc(const McpHttpServerPrivate::FHttpRequest& Request, bool bKeepAlive);
	bool HandleToolsCall(const McpHttpServerPrivate::FHttpRequest& Request, const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params, bool bKeepAlive);
	/** enable_toolset: adds domains to the connection's session and, when the list changed and the client takes SSE, says so before the reply. */
	bool AnswerEnableToolset(const McpHttpServerPrivate::FHttpRequest& Request, const TSharedPtr<FJsonValue>& Id, const FMcpToolCatalog& Catalog, const TSharedPtr<FJsonObject>& Arguments, bool bKeepAlive);

	FMcpHttpServer* Server;
	TSharedPtr<FSocket> Socket;
	/** Bytes read but not yet consumed (keep-alive: may hold the start of the next request). */
	TArray<uint8> Buffer;
	TArray<uint8> RecvScratch;
	FThreadSafeBool bConnectionRunning;
	FThreadSafeBool bFinished;
};

uint32 FMcpHttpConnection::Run()
{
	using namespace McpHttpServerPrivate;

	if (Socket.IsValid())
	{
		Socket->SetNoDelay(true);
		Socket->SetLinger(false, 0);
		// Blocking: what keeps Stop() observable is the bounded Wait() in FillBuffer.
		Socket->SetNonBlocking(false);
		RecvScratch.SetNumUninitialized(RecvChunkBytes);

		while (KeepGoing())
		{
			FHttpRequest Request;
			const EReadResult Read = ReadRequest(Request);
			if (Read == EReadResult::Closed)
			{
				break;
			}
			if (Read != EReadResult::Ok)
			{
				const int32 Status =
					Read == EReadResult::HeadersTooLarge ? 431 :
					Read == EReadResult::BodyTooLarge ? 413 :
					Read == EReadResult::Stalled ? 408 : 400;
				UE_LOG(LogUEAgentHttp, Warning, TEXT("HTTP request dropped (%d %s)"), Status, StatusText(Status));
				SendEmpty(Status, false);
				break;
			}
			Server->CountRequest();
			if (!HandleRequest(Request))
			{
				break;
			}
		}

		Socket->Close();
		Socket.Reset();
	}

	bFinished = true;
	return 0;
}

bool FMcpHttpConnection::FillBuffer(double MaxWaitSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;
	while (KeepGoing() && Socket.IsValid())
	{
		const double Remaining = Deadline - FPlatformTime::Seconds();
		if (Remaining <= 0.0)
		{
			return false;
		}
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(FMath::Min(Remaining, 1.0))))
		{
			continue;
		}
		int32 Read = 0;
		if (!Socket->Recv(RecvScratch.GetData(), RecvScratch.Num(), Read) || Read <= 0)
		{
			return false; // peer closed, or error
		}
		Buffer.Append(RecvScratch.GetData(), Read);
		return true;
	}
	return false;
}

int32 FMcpHttpConnection::FindCrlf(int32 From) const
{
	for (int32 Index = From; Index + 1 < Buffer.Num(); ++Index)
	{
		if (Buffer[Index] == '\r' && Buffer[Index + 1] == '\n')
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 FMcpHttpConnection::FindHeaderEnd() const
{
	for (int32 Index = 0; Index + 3 < Buffer.Num(); ++Index)
	{
		if (Buffer[Index] == '\r' && Buffer[Index + 1] == '\n' && Buffer[Index + 2] == '\r' && Buffer[Index + 3] == '\n')
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

FMcpHttpConnection::EReadResult FMcpHttpConnection::ReadRequest(McpHttpServerPrivate::FHttpRequest& Out)
{
	using namespace McpHttpServerPrivate;

	int32 HeaderEnd = FindHeaderEnd();
	while (HeaderEnd == INDEX_NONE)
	{
		if (Buffer.Num() > MaxHeaderBytes)
		{
			return EReadResult::HeadersTooLarge;
		}
		const bool bStarted = Buffer.Num() > 0;
		if (!FillBuffer(bStarted ? ReadStallSeconds : IdleCloseSeconds))
		{
			return bStarted ? EReadResult::Stalled : EReadResult::Closed;
		}
		HeaderEnd = FindHeaderEnd();
	}
	if (HeaderEnd > MaxHeaderBytes)
	{
		return EReadResult::HeadersTooLarge;
	}

	const FString HeaderText = FromUtf8(Buffer.GetData(), HeaderEnd);
	Consume(HeaderEnd + 4);

	TArray<FString> Lines;
	HeaderText.ParseIntoArray(Lines, TEXT("\r\n"), /*bCullEmpty*/ false);
	if (Lines.Num() == 0)
	{
		return EReadResult::BadRequest;
	}
	TArray<FString> RequestLine;
	Lines[0].ParseIntoArrayWS(RequestLine);
	if (RequestLine.Num() != 3)
	{
		return EReadResult::BadRequest;
	}
	Out.Method = RequestLine[0].ToUpper();
	Out.bHttp11 = RequestLine[2].Equals(TEXT("HTTP/1.1"), ESearchCase::IgnoreCase);
	if (!Out.bHttp11 && !RequestLine[2].Equals(TEXT("HTTP/1.0"), ESearchCase::IgnoreCase))
	{
		return EReadResult::BadRequest;
	}
	int32 QueryStart = INDEX_NONE;
	if (RequestLine[1].FindChar(TEXT('?'), QueryStart))
	{
		Out.Path = RequestLine[1].Left(QueryStart);
		Out.Query = RequestLine[1].Mid(QueryStart + 1);
	}
	else
	{
		Out.Path = RequestLine[1];
	}

	for (int32 LineIndex = 1; LineIndex < Lines.Num(); ++LineIndex)
	{
		const FString& Line = Lines[LineIndex];
		if (Line.IsEmpty())
		{
			continue;
		}
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), Colon) || Colon == 0)
		{
			return EReadResult::BadRequest;
		}
		const FString Name = Line.Left(Colon).TrimStartAndEnd().ToLower();
		const FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();
		if (FString* Existing = Out.Headers.Find(Name))
		{
			*Existing += TEXT(", ") + Value;
		}
		else
		{
			Out.Headers.Add(Name, Value);
		}
	}

	if (const FString* Expect = Out.Header(TEXT("expect")))
	{
		if (Expect->Equals(TEXT("100-continue"), ESearchCase::IgnoreCase))
		{
			static const char Continue[] = "HTTP/1.1 100 Continue\r\n\r\n";
			SendBytes(reinterpret_cast<const uint8*>(Continue), sizeof(Continue) - 1);
		}
	}

	const FString* TransferEncoding = Out.Header(TEXT("transfer-encoding"));
	if (TransferEncoding && TransferEncoding->Contains(TEXT("chunked"), ESearchCase::IgnoreCase))
	{
		return ReadChunkedBody(Out.Body);
	}
	if (const FString* ContentLength = Out.Header(TEXT("content-length")))
	{
		for (const TCHAR C : *ContentLength)
		{
			if (!FChar::IsDigit(C))
			{
				return EReadResult::BadRequest;
			}
		}
		const int64 Length = FCString::Atoi64(**ContentLength);
		if (Length > MaxBodyBytes)
		{
			return EReadResult::BodyTooLarge;
		}
		while (Buffer.Num() < Length)
		{
			if (!FillBuffer(ReadStallSeconds))
			{
				return EReadResult::Stalled;
			}
		}
		Out.Body.Append(Buffer.GetData(), static_cast<int32>(Length));
		Consume(static_cast<int32>(Length));
	}
	return EReadResult::Ok;
}

FMcpHttpConnection::EReadResult FMcpHttpConnection::ReadChunkedBody(TArray<uint8>& OutBody)
{
	using namespace McpHttpServerPrivate;

	for (;;)
	{
		int32 LineEnd = FindCrlf(0);
		while (LineEnd == INDEX_NONE)
		{
			if (Buffer.Num() > 4096)
			{
				return EReadResult::BadRequest;
			}
			if (!FillBuffer(ReadStallSeconds))
			{
				return EReadResult::Stalled;
			}
			LineEnd = FindCrlf(0);
		}
		FString SizeLine = FromUtf8(Buffer.GetData(), LineEnd);
		Consume(LineEnd + 2);
		int32 Extension = INDEX_NONE;
		if (SizeLine.FindChar(TEXT(';'), Extension))
		{
			SizeLine.LeftInline(Extension);
		}
		SizeLine.TrimStartAndEndInline();
		if (SizeLine.IsEmpty())
		{
			return EReadResult::BadRequest;
		}
		int64 Size = 0;
		for (const TCHAR C : SizeLine)
		{
			const int32 Digit = HexDigit(C);
			if (Digit < 0)
			{
				return EReadResult::BadRequest;
			}
			Size = Size * 16 + Digit;
			if (Size > MaxBodyBytes)
			{
				return EReadResult::BodyTooLarge;
			}
		}

		if (Size == 0)
		{
			// Trailers (ignored) up to the empty line.
			for (;;)
			{
				int32 TrailerEnd = FindCrlf(0);
				while (TrailerEnd == INDEX_NONE)
				{
					if (Buffer.Num() > MaxHeaderBytes)
					{
						return EReadResult::HeadersTooLarge;
					}
					if (!FillBuffer(ReadStallSeconds))
					{
						return EReadResult::Stalled;
					}
					TrailerEnd = FindCrlf(0);
				}
				Consume(TrailerEnd + 2);
				if (TrailerEnd == 0)
				{
					return EReadResult::Ok;
				}
			}
		}

		if (OutBody.Num() + Size > MaxBodyBytes)
		{
			return EReadResult::BodyTooLarge;
		}
		while (Buffer.Num() < Size + 2)
		{
			if (!FillBuffer(ReadStallSeconds))
			{
				return EReadResult::Stalled;
			}
		}
		OutBody.Append(Buffer.GetData(), static_cast<int32>(Size));
		Consume(static_cast<int32>(Size) + 2);
	}
}

bool FMcpHttpConnection::SendBytes(const uint8* Data, int32 Count)
{
	int32 Sent = 0;
	while (Sent < Count)
	{
		if (!Socket.IsValid())
		{
			return false;
		}
		int32 SentNow = 0;
		if (!Socket->Send(Data + Sent, FMath::Min(Count - Sent, 65536), SentNow) || SentNow <= 0)
		{
			return false;
		}
		Sent += SentNow;
	}
	return true;
}

bool FMcpHttpConnection::SendResponse(int32 Status, const TCHAR* ContentType, const TArray<uint8>& Body, bool bKeepAlive, const FString& ExtraHeaders)
{
	using namespace McpHttpServerPrivate;

	FString Head = FString::Printf(TEXT("HTTP/1.1 %d %s\r\n"), Status, StatusText(Status));
	if (ContentType && *ContentType)
	{
		Head += FString::Printf(TEXT("Content-Type: %s\r\n"), ContentType);
	}
	Head += FString::Printf(TEXT("Content-Length: %d\r\n"), Body.Num());
	Head += TEXT("Cache-Control: no-store\r\n");
	Head += bKeepAlive
		? FString::Printf(TEXT("Connection: keep-alive\r\nKeep-Alive: timeout=%d\r\n"), KeepAliveHintSeconds)
		: FString(TEXT("Connection: close\r\n"));
	Head += ExtraHeaders;
	Head += TEXT("\r\n");

	const TArray<uint8> HeadBytes = ToUtf8(Head);
	return SendBytes(HeadBytes.GetData(), HeadBytes.Num())
		&& (Body.Num() == 0 || SendBytes(Body.GetData(), Body.Num()));
}

bool FMcpHttpConnection::SendJson(int32 Status, const FString& Json, bool bKeepAlive)
{
	return SendResponse(Status, TEXT("application/json"), McpHttpServerPrivate::ToUtf8(Json), bKeepAlive);
}

bool FMcpHttpConnection::SendEmpty(int32 Status, bool bKeepAlive, const FString& ExtraHeaders)
{
	return SendResponse(Status, nullptr, TArray<uint8>(), bKeepAlive, ExtraHeaders);
}

bool FMcpHttpConnection::SendSseHead()
{
	// No length and no chunking: the end of the stream is the connection close (Connection: close).
	static const char Head[] =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-cache\r\n"
		"X-Accel-Buffering: no\r\n"
		"Connection: close\r\n"
		"\r\n";
	return SendBytes(reinterpret_cast<const uint8*>(Head), sizeof(Head) - 1);
}

bool FMcpHttpConnection::SendSseEvent(const FString& Json)
{
	const TArray<uint8> Bytes = McpHttpServerPrivate::ToUtf8(TEXT("event: message\r\ndata: ") + Json + TEXT("\r\n\r\n"));
	return SendBytes(Bytes.GetData(), Bytes.Num());
}

bool FMcpHttpConnection::PeerHasClosed()
{
	if (!Socket.IsValid())
	{
		return true;
	}
	if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
	{
		return false; // neither data nor EOF
	}
	uint8 Byte = 0;
	int32 Read = 0;
	// Peek: if what is there is the start of another request, it stays in the socket.
	return !Socket->Recv(&Byte, 1, Read, ESocketReceiveFlags::Peek) || Read <= 0;
}

bool FMcpHttpConnection::HandleRequest(const McpHttpServerPrivate::FHttpRequest& Request)
{
	using namespace McpHttpServerPrivate;

	bool bKeepAlive = Request.bHttp11;
	if (const FString* Connection = Request.Header(TEXT("connection")))
	{
		if (Connection->Contains(TEXT("close"), ESearchCase::IgnoreCase))
		{
			bKeepAlive = false;
		}
		else if (Connection->Contains(TEXT("keep-alive"), ESearchCase::IgnoreCase))
		{
			bKeepAlive = true;
		}
	}

	// Security FIRST, before looking at path or method: a browser must not even get a 404.
	const FString* Host = Request.Header(TEXT("host"));
	if (!IsAllowedHostHeader(Host, Server->GetPort()))
	{
		UE_LOG(LogUEAgentHttp, Warning, TEXT("Rejected: Host '%s' is not the loopback (DNS rebinding?)"), Host ? **Host : TEXT("<no Host>"));
		SendEmpty(403, false);
		return false;
	}
	const FString* Origin = Request.Header(TEXT("origin"));
	if (!IsAllowedOrigin(Origin))
	{
		UE_LOG(LogUEAgentHttp, Warning, TEXT("Rejected: Origin '%s' is not local"), **Origin);
		SendEmpty(403, false);
		return false;
	}

	if (Request.Path != McpPath && Request.Path != FString(McpPath) + TEXT("/"))
	{
		return SendEmpty(404, bKeepAlive) && bKeepAlive;
	}
	if (Request.Method != TEXT("POST"))
	{
		// There is no SSE channel over GET (the server never initiates messages) and no session
		// to close with DELETE: 405 is what the spec asks for in both cases.
		return SendEmpty(405, bKeepAlive, TEXT("Allow: POST\r\n")) && bKeepAlive;
	}

	const FString& Token = Server->GetAuthToken();
	if (!Token.IsEmpty())
	{
		const FString* Authorization = Request.Header(TEXT("authorization"));
		if (!Authorization || *Authorization != TEXT("Bearer ") + Token)
		{
			return SendEmpty(401, bKeepAlive, TEXT("WWW-Authenticate: Bearer realm=\"ueagent\"\r\n")) && bKeepAlive;
		}
	}

	return HandleJsonRpc(Request, bKeepAlive);
}

bool FMcpHttpConnection::HandleJsonRpc(const McpHttpServerPrivate::FHttpRequest& Request, bool bKeepAlive)
{
	using namespace McpHttpServerPrivate;

	if (const FString* Version = Request.Header(TEXT("mcp-protocol-version")))
	{
		if (!IsSupportedVersion(Version->TrimStartAndEnd()))
		{
			// 400 with an EMPTY body, on purpose: a client of the 2026-07-28 revision tries its own
			// version first and, on a 400 without a recognizable modern JSON-RPC error, falls back
			// to `initialize` (spec 2026-07-28, "Backward Compatibility"). A JSON-RPC body here
			// could be read as "this server is modern" and break the negotiation.
			UE_LOG(LogUEAgentHttp, Log, TEXT("MCP-Protocol-Version '%s' not supported: 400 so the client negotiates through initialize"), **Version);
			return SendEmpty(400, bKeepAlive) && bKeepAlive;
		}
	}

	const FString BodyText = FromUtf8(Request.Body.GetData(), Request.Body.Num());
	TSharedPtr<FJsonValue> Parsed;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BodyText), Parsed) || !Parsed.IsValid())
	{
		return SendJson(400, MakeRpcError(nullptr, -32700, TEXT("Parse error: the body is not JSON")), bKeepAlive) && bKeepAlive;
	}
	if (Parsed->Type != EJson::Object)
	{
		const TCHAR* Why = Parsed->Type == EJson::Array ? TEXT("Invalid Request: JSON-RPC batches are not supported") : TEXT("Invalid Request");
		return SendJson(400, MakeRpcError(nullptr, -32600, Why), bKeepAlive) && bKeepAlive;
	}

	const TSharedPtr<FJsonObject> Message = Parsed->AsObject();
	FString Method;
	if (!Message->TryGetStringField(TEXT("method"), Method))
	{
		// A response from the client: this server never sends it requests, so it expects none.
		return SendJson(400, MakeRpcError(nullptr, -32600, TEXT("Invalid Request: missing 'method'")), bKeepAlive) && bKeepAlive;
	}

	// Notification (no `id`): initialized, cancelled... Nothing to answer.
	if (!Message->HasField(TEXT("id")))
	{
		return SendEmpty(202, bKeepAlive) && bKeepAlive;
	}
	const TSharedPtr<FJsonValue> Id = Message->TryGetField(TEXT("id"));
	TSharedPtr<FJsonObject> Params;
	const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
	if (Message->TryGetObjectField(TEXT("params"), ParamsObject) && ParamsObject)
	{
		Params = *ParamsObject;
	}

	if (Method == TEXT("tools/call"))
	{
		return HandleToolsCall(Request, Id, Params, bKeepAlive);
	}

	const TSharedPtr<const FMcpToolCatalog> CurrentCatalog = FMcpToolCatalog::GetCurrent();
	const FToolsRequest ToolsRequest = ReadToolsRequest(Request, Server->GetDefaultToolsMode(), CurrentCatalog.Get());
	if (Method == TEXT("tools/list"))
	{
		// Megabytes that never change: served pre-serialized, without going through the game
		// thread and without rebuilding the JSON. Only the envelope is built around them.
		const TSharedPtr<const FMcpToolCatalog>& Catalog = CurrentCatalog;
		FString Prefix = ToJsonText(MakeEnvelope(Id));
		Prefix.LeftChopInline(1);
		Prefix += TEXT(",\"result\":");
		TArray<uint8> Body = ToUtf8(Prefix);
		if (Catalog.IsValid())
		{
			if (ToolsRequest.Mode == EMcpToolsMode::Compact)
			{
				Body.Append(Catalog->GetCompactToolsListUtf8());
			}
			else if (ToolsRequest.Mode == EMcpToolsMode::All)
			{
				Body.Append(Catalog->GetToolsListUtf8());
			}
			else
			{
				// The core list, plus the domains the URL names and the ones this client's session enabled.
				TSet<FString> Domains = ToolsRequest.Domains;
				const FString Session = SessionOf(Request);
				if (!Session.IsEmpty())
				{
					Server->GetSessionDomains(Session, Domains);
				}
				Body.Append(Domains.Num() > 0 ? Catalog->BuildCoreToolsListUtf8(Domains) : Catalog->GetCoreToolsListUtf8());
			}
		}
		else
		{
			UE_LOG(LogUEAgentHttp, Error, TEXT("tools/list with no catalog loaded: returning an empty list (see the startup log)"));
			const TArray<uint8> Empty = ToUtf8(TEXT("{\"tools\":[]}"));
			Body.Append(Empty);
		}
		Body.Add('}');
		return SendResponse(200, TEXT("application/json"), Body, bKeepAlive) && bKeepAlive;
	}

	if (Method == TEXT("initialize"))
	{
		FString Requested;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), Requested);
		}
		const FString Negotiated = IsSupportedVersion(Requested) ? Requested : FString(LatestProtocolVersion);

		FString ClientName = TEXT("?"), ClientVersion = TEXT("?");
		const TSharedPtr<FJsonObject>* ClientInfo = nullptr;
		if (Params.IsValid() && Params->TryGetObjectField(TEXT("clientInfo"), ClientInfo) && ClientInfo)
		{
			(*ClientInfo)->TryGetStringField(TEXT("name"), ClientName);
			(*ClientInfo)->TryGetStringField(TEXT("version"), ClientVersion);
		}
		UE_LOG(LogUEAgentHttp, Display, TEXT("initialize: %s %s requests %s -> %s (%s tool list)"),
			*ClientName, *ClientVersion, Requested.IsEmpty() ? TEXT("<none>") : *Requested, *Negotiated, ToolsModeName(ToolsRequest.Mode));

		// The core list can grow (enable_toolset): it says so, and the session is what remembers the growth.
		const bool bCoreList = ToolsRequest.Mode == EMcpToolsMode::Core;
		TSharedRef<FJsonObject> Tools = MakeShared<FJsonObject>();
		Tools->SetBoolField(TEXT("listChanged"), bCoreList);
		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), Tools);
		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("UEAgent"));
		ServerInfo->SetStringField(TEXT("version"), Server->GetServerVersion());

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Negotiated);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		FString Instructions = AgentInstructions;
		if (CurrentCatalog.IsValid() && ToolsRequest.Mode != EMcpToolsMode::All)
		{
			Instructions = (ToolsRequest.Mode == EMcpToolsMode::Compact ? McpMetaTools::Instructions(*CurrentCatalog) : McpMetaTools::CoreInstructions(*CurrentCatalog))
				+ TEXT("\n\n") + Instructions;
		}
		Result->SetStringField(TEXT("instructions"), Instructions);
		const FString SessionHeader = bCoreList ? FString::Printf(TEXT("Mcp-Session-Id: %s\r\n"), *Server->OpenSession()) : FString();
		return SendResponse(200, TEXT("application/json"), ToUtf8(MakeRpcResult(Id, Result)), bKeepAlive, SessionHeader) && bKeepAlive;
	}

	if (Method == TEXT("ping"))
	{
		return SendJson(200, MakeRpcResult(Id, MakeShared<FJsonObject>()), bKeepAlive) && bKeepAlive;
	}

	return SendJson(200, MakeRpcError(Id, -32601, FString::Printf(TEXT("Method not found: %s"), *Method)), bKeepAlive) && bKeepAlive;
}

bool FMcpHttpConnection::HandleToolsCall(const McpHttpServerPrivate::FHttpRequest& Request, const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Params, bool bKeepAlive)
{
	using namespace McpHttpServerPrivate;

	FString ToolName;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
	{
		return SendJson(200, MakeRpcError(Id, -32602, TEXT("Invalid params: tools/call needs params.name")), bKeepAlive) && bKeepAlive;
	}

	// Drop the nulls: a handler that receives `"x": null` reads it differently depending on the
	// TryGet it uses, so a null argument is treated as absent.
	TSharedPtr<FJsonObject> Arguments = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* ArgumentsObject = nullptr;
	if (Params->TryGetObjectField(TEXT("arguments"), ArgumentsObject) && ArgumentsObject && ArgumentsObject->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*ArgumentsObject)->Values)
		{
			if (Field.Value.IsValid() && !Field.Value->IsNull())
			{
				Arguments->SetField(Field.Key, Field.Value);
			}
		}
	}

	TSharedPtr<FJsonValue> ProgressToken;
	const TSharedPtr<FJsonObject>* Meta = nullptr;
	if (Params->TryGetObjectField(TEXT("_meta"), Meta) && Meta && Meta->IsValid())
	{
		ProgressToken = (*Meta)->TryGetField(TEXT("progressToken"));
		if (ProgressToken.IsValid() && ProgressToken->IsNull())
		{
			ProgressToken.Reset();
		}
	}

	// call_tool runs the tool it names, through everything below.
	if (ToolName.Equals(McpMetaTools::CallTool, ESearchCase::IgnoreCase))
	{
		FString UnwrapError;
		TSharedPtr<FJsonObject> InnerArguments;
		if (!McpMetaTools::UnwrapCall(Arguments, ToolName, InnerArguments, UnwrapError))
		{
			return SendJson(200, MakeRpcResult(Id, MakeToolResult(UnwrapError, true)), bKeepAlive) && bKeepAlive;
		}
		Arguments = InnerArguments;
	}

	const TSharedPtr<const FMcpToolCatalog> Catalog = FMcpToolCatalog::GetCurrent();
	if (Catalog.IsValid())
	{
		// enable_toolset changes this connection's list, which lives here: answered here, off the game thread.
		if (ToolName.Equals(McpMetaTools::EnableToolset, ESearchCase::IgnoreCase))
		{
			Server->CountToolCall();
			return AnswerEnableToolset(Request, Id, *Catalog, Arguments, bKeepAlive);
		}
		// find_tools and describe_tool only read the catalog: answered here, off the game thread.
		bool bMetaError = false;
		if (const TSharedPtr<FJsonObject> MetaAnswer = McpMetaTools::Answer(*Catalog, ToolName, Arguments, bMetaError))
		{
			Server->CountToolCall();
			return SendJson(200, MakeRpcResult(Id, MakeToolResult(ToJsonText(MetaAnswer.ToSharedRef()), bMetaError)), bKeepAlive) && bKeepAlive;
		}
	}
	// A renamed tool's old name runs the new one; the bridge resolves it again and says so in the reply.
	const FMcpToolCatalog::FTool* Tool = Catalog.IsValid() ? Catalog->FindTool(FMcpToolCatalog::ResolveCallName(ToolName)) : nullptr;
	if (!Tool)
	{
		FString Unknown = FString::Printf(TEXT("Unknown tool: %s"), *ToolName);
		const TArray<FString> Closest = Catalog.IsValid() ? McpMetaTools::SuggestNames(*Catalog, ToolName) : TArray<FString>();
		if (Closest.Num() > 0)
		{
			Unknown += FString::Printf(TEXT(". Closest names: %s"), *FString::Join(Closest, TEXT(", ")));
		}
		return SendJson(200, MakeRpcResult(Id, MakeToolResult(Unknown, true)), bKeepAlive) && bKeepAlive;
	}
	const FString Missing = Catalog->CheckRequired(*Tool, Arguments);
	if (!Missing.IsEmpty())
	{
		return SendJson(200, MakeRpcResult(Id, MakeToolResult(Missing, true)), bKeepAlive) && bKeepAlive;
	}

	Server->CountToolCall();
	UE_LOG(LogUEAgentHttp, Verbose, TEXT("tools/call %s"), *ToolName);
	TFuture<FString> Future = Server->GetBridge()->EnqueueToolCall(ToolName, Arguments, HttpSessionId);

	// While it runs on the game thread: watch for the client closing and, if it asked for
	// progress, switch to SSE and send notifications/progress -- without that, Claude Code aborts
	// an HTTP call that has not answered after 5 min (CLAUDE_CODE_MCP_TOOL_IDLE_TIMEOUT).
	const double Start = FPlatformTime::Seconds();
	bool bStreaming = false;
	double LastProgress = 0.0;
	int32 ProgressCount = 0;
	while (!Future.WaitFor(FTimespan::FromSeconds(1.0)))
	{
		if (!KeepGoing())
		{
			return false;
		}
		const double Elapsed = FPlatformTime::Seconds() - Start;
		if (PeerHasClosed())
		{
			UE_LOG(LogUEAgentHttp, Warning, TEXT("The client closed the connection while '%s' was running (%.0f s): the command still finishes and its result is discarded"), *ToolName, Elapsed);
			return false;
		}
		if (!ProgressToken.IsValid())
		{
			continue;
		}
		if (!bStreaming && Elapsed >= SseAfterSeconds)
		{
			if (!SendSseHead())
			{
				return false;
			}
			bStreaming = true;
			LastProgress = Elapsed - ProgressEverySeconds;
		}
		if (bStreaming && Elapsed - LastProgress >= ProgressEverySeconds)
		{
			TSharedRef<FJsonObject> ProgressParams = MakeShared<FJsonObject>();
			ProgressParams->SetField(TEXT("progressToken"), ProgressToken);
			ProgressParams->SetNumberField(TEXT("progress"), ++ProgressCount);
			ProgressParams->SetStringField(TEXT("message"), FString::Printf(TEXT("'%s' still running in Unreal (%.0f s)"), *ToolName, Elapsed));
			TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
			Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Notification->SetStringField(TEXT("method"), TEXT("notifications/progress"));
			Notification->SetObjectField(TEXT("params"), ProgressParams);
			if (!SendSseEvent(ToJsonText(Notification)))
			{
				return false;
			}
			LastProgress = Elapsed;
		}
	}

	bool bRefused = false;
	const TSharedRef<FJsonObject> Payload = UnwrapOutcome(Future.Get(), bRefused);
	const FString Response = MakeRpcResult(Id, MakeToolResult(ToJsonText(Payload), bRefused));
	if (bStreaming)
	{
		SendSseEvent(Response);
		return false; // the stream ends with the connection
	}
	return SendJson(200, Response, bKeepAlive) && bKeepAlive;
}

bool FMcpHttpConnection::AnswerEnableToolset(const McpHttpServerPrivate::FHttpRequest& Request, const TSharedPtr<FJsonValue>& Id,
	const FMcpToolCatalog& Catalog, const TSharedPtr<FJsonObject>& Arguments, bool bKeepAlive)
{
	using namespace McpHttpServerPrivate;

	const FToolsRequest ToolsRequest = ReadToolsRequest(Request, Server->GetDefaultToolsMode(), &Catalog);
	if (ToolsRequest.Mode != EMcpToolsMode::Core)
	{
		TSharedRef<FJsonObject> Reply = MakeShared<FJsonObject>();
		Reply->SetBoolField(TEXT("success"), true);
		Reply->SetNumberField(TEXT("tools_added"), 0);
		Reply->SetBoolField(TEXT("list_changed"), false);
		Reply->SetStringField(TEXT("note"), ToolsRequest.Mode == EMcpToolsMode::All
			? TEXT("This connection already lists every tool (?tools=all): call them directly.")
			: TEXT("This connection uses the compact list (?tools=compact), which does not grow: run the domain's tools with call_tool."));
		return SendJson(200, MakeRpcResult(Id, MakeToolResult(ToJsonText(Reply), false)), bKeepAlive) && bKeepAlive;
	}

	TArray<FString> Domains;
	FString Error;
	if (!McpMetaTools::ReadToolsetDomains(Catalog, Arguments, Domains, Error))
	{
		return SendJson(200, MakeRpcResult(Id, MakeToolResult(Error, true)), bKeepAlive) && bKeepAlive;
	}
	const FString Session = SessionOf(Request);
	TSet<FString> Listed = ToolsRequest.Domains;
	if (!Session.IsEmpty())
	{
		Server->GetSessionDomains(Session, Listed);
		Server->AddSessionDomains(Session, Domains);
	}
	const TSharedRef<FJsonObject> Reply = McpMetaTools::ToolsetReply(Catalog, Domains, Listed, !Session.IsEmpty());
	const FString Response = MakeRpcResult(Id, MakeToolResult(ToJsonText(Reply), false));
	UE_LOG(LogUEAgentHttp, Display, TEXT("enable_toolset: %s -> %d tools added%s"), *FString::Join(Domains, TEXT(", ")),
		static_cast<int32>(Reply->GetNumberField(TEXT("tools_added"))), Session.IsEmpty() ? TEXT(" (no session: the list cannot change)") : TEXT(""));

	// The list changed: a client that takes SSE hears it before the reply, on this response's stream (the server has no
	// GET stream), and re-reads tools/list -- which already carries the domains, the session holds them.
	const FString* Accept = Request.Header(TEXT("accept"));
	if (Reply->GetBoolField(TEXT("list_changed")) && Accept && Accept->Contains(TEXT("text/event-stream"), ESearchCase::IgnoreCase))
	{
		TSharedRef<FJsonObject> Notification = MakeShared<FJsonObject>();
		Notification->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Notification->SetStringField(TEXT("method"), TEXT("notifications/tools/list_changed"));
		if (!SendSseHead() || !SendSseEvent(ToJsonText(Notification)))
		{
			return false;
		}
		SendSseEvent(Response);
		return false; // the stream ends with the connection
	}
	return SendJson(200, Response, bKeepAlive) && bKeepAlive;
}

// ===========================================================================
// FMcpHttpServer
// ===========================================================================

FMcpHttpServer::FMcpHttpServer(UUEAgentBridge* InBridge)
	: Bridge(InBridge)
	, bRunning(false)
{
}

FMcpHttpServer::~FMcpHttpServer()
{
	Stop();
}

bool FMcpHttpServer::Start(uint16 InPort, const FString& InAuthToken, FString& OutError)
{
	using namespace McpHttpServerPrivate;

	if (bRunning)
	{
		return true;
	}

	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* RawListener = Subsystem ? Subsystem->CreateSocket(NAME_Stream, TEXT("UEAgentHttpListener"), false) : nullptr;
	if (!RawListener)
	{
		OutError = TEXT("could not create the HTTP server socket");
		return false;
	}
	TSharedPtr<FSocket> NewListener(RawListener, &DestroySocket);

	// No SetReuseAddr, on purpose: if the port is taken (another editor, another server) the bind
	// must FAIL and be reported, not be shared.
	FIPv4Address Loopback;
	FIPv4Address::Parse(TEXT("127.0.0.1"), Loopback);
	const FIPv4Endpoint Endpoint(Loopback, InPort);
	if (!NewListener->Bind(*Endpoint.ToInternetAddr()))
	{
		OutError = FString::Printf(TEXT("could not open 127.0.0.1:%d -- is another editor or process using the port?"), InPort);
		return false;
	}
	if (!NewListener->Listen(64))
	{
		OutError = FString::Printf(TEXT("could not listen on 127.0.0.1:%d"), InPort);
		return false;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UEAgent"));
	ServerVersion = Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(TEXT("unknown"));

	// The list a client gets when its URL asks for none: the core list unless UEAGENT_TOOLS says otherwise.
	const FString ToolsMode = FPlatformMisc::GetEnvironmentVariable(TEXT("UEAGENT_TOOLS")).TrimStartAndEnd();
	bool bKnownMode = true;
	DefaultToolsMode = ToolsMode.IsEmpty() ? EMcpToolsMode::Core : ParseToolsMode(ToolsMode, EMcpToolsMode::Core, bKnownMode);
	if (!bKnownMode)
	{
		UE_LOG(LogUEAgentHttp, Warning, TEXT("UEAGENT_TOOLS='%s' is none of core, all, compact: tools/list carries the core list"), *ToolsMode);
	}
	{
		FScopeLock Lock(&SessionsLock);
		Sessions.Reset();
	}
	Listener = NewListener;
	Port = InPort;
	AuthToken = InAuthToken;
	bRunning = true;

	AcceptorRunnable = new FMcpHttpAcceptor(this);
	AcceptorThread = FRunnableThread::Create(AcceptorRunnable, TEXT("UEAgentHttpAcceptor"), 0, TPri_Normal);
	if (!AcceptorThread)
	{
		bRunning = false;
		delete AcceptorRunnable;
		AcceptorRunnable = nullptr;
		Listener.Reset();
		OutError = TEXT("could not create the HTTP server acceptor thread");
		return false;
	}
	return true;
}

void FMcpHttpServer::Stop()
{
	if (!bRunning && !AcceptorThread)
	{
		return;
	}
	bRunning = false;

	if (Listener.IsValid())
	{
		Listener->Close();
	}
	if (AcceptorThread)
	{
		AcceptorThread->Kill(true);
		delete AcceptorThread;
		AcceptorThread = nullptr;
	}
	delete AcceptorRunnable;
	AcceptorRunnable = nullptr;

	// Stop ALL of them first (closes each socket and wakes its thread), then wait for each one:
	// that way they finish in parallel. A connection waiting on a tools/call notices within <= 1 s.
	TArray<FConnectionHandle> ToStop;
	{
		FScopeLock Lock(&ConnectionsLock);
		Swap(ToStop, Connections);
	}
	for (FConnectionHandle& Handle : ToStop)
	{
		if (Handle.Runnable)
		{
			Handle.Runnable->Stop();
		}
	}
	for (FConnectionHandle& Handle : ToStop)
	{
		if (Handle.Thread)
		{
			Handle.Thread->Kill(true);
			delete Handle.Thread;
		}
		delete Handle.Runnable;
	}

	Listener.Reset();
	UE_LOG(LogUEAgentHttp, Display, TEXT("MCP HTTP server stopped (port %d)"), Port);
}

int32 FMcpHttpServer::GetNumConnections()
{
	FScopeLock Lock(&ConnectionsLock);
	return Connections.Num();
}

void FMcpHttpServer::ReapFinishedConnections()
{
	TArray<FConnectionHandle> Finished;
	{
		FScopeLock Lock(&ConnectionsLock);
		for (int32 Index = Connections.Num() - 1; Index >= 0; --Index)
		{
			if (Connections[Index].Runnable && Connections[Index].Runnable->IsFinished())
			{
				Finished.Add(Connections[Index]);
				Connections.RemoveAtSwap(Index);
			}
		}
	}
	for (FConnectionHandle& Handle : Finished)
	{
		if (Handle.Thread)
		{
			Handle.Thread->WaitForCompletion();
			delete Handle.Thread;
		}
		delete Handle.Runnable;
	}
}

void FMcpHttpServer::RunAcceptLoop(const FThreadSafeBool& bAcceptorRunning)
{
	using namespace McpHttpServerPrivate;

	while (bAcceptorRunning && bRunning && Listener.IsValid())
	{
		ReapFinishedConnections();

		bool bPending = false;
		if (!Listener->WaitForPendingConnection(bPending, FTimespan::FromMilliseconds(250)))
		{
			FPlatformProcess::Sleep(0.05f); // listener in error (e.g. closing): do not spin
			continue;
		}
		if (!bPending)
		{
			continue;
		}
		FSocket* RawClient = Listener->Accept(TEXT("UEAgentHttpClient"));
		if (!RawClient)
		{
			continue;
		}
		TSharedPtr<FSocket> Client(RawClient, &DestroySocket);

		if (GetNumConnections() >= MaxConnections)
		{
			// Better an immediate 503 than leaving it hanging in the backlog with no answer.
			static const char Busy[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
			int32 Sent = 0;
			Client->Send(reinterpret_cast<const uint8*>(Busy), sizeof(Busy) - 1, Sent);
			Client->Close();
			UE_LOG(LogUEAgentHttp, Warning, TEXT("Connection cap of %d reached: 503"), MaxConnections);
			continue;
		}

		FMcpHttpConnection* Runnable = new FMcpHttpConnection(this, Client);
		FRunnableThread* Thread = FRunnableThread::Create(Runnable, *FString::Printf(TEXT("UEAgentHttp_%u"), NextConnectionId++), 0, TPri_Normal);
		if (!Thread)
		{
			delete Runnable;
			Client->Close();
			continue;
		}
		FScopeLock Lock(&ConnectionsLock);
		Connections.Add({ Thread, Runnable });
	}
}

FString FMcpHttpServer::OpenSession()
{
	const FString Session = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
	FScopeLock Lock(&SessionsLock);
	Sessions.Add(Session).LastUsed = FPlatformTime::Seconds();
	TrimSessions();
	return Session;
}

bool FMcpHttpServer::GetSessionDomains(const FString& Session, TSet<FString>& OutDomains)
{
	FScopeLock Lock(&SessionsLock);
	FSession* Found = Sessions.Find(Session);
	if (!Found)
	{
		return false;
	}
	Found->LastUsed = FPlatformTime::Seconds();
	OutDomains.Append(Found->Domains);
	return true;
}

void FMcpHttpServer::AddSessionDomains(const FString& Session, const TArray<FString>& InDomains)
{
	FScopeLock Lock(&SessionsLock);
	FSession& Found = Sessions.FindOrAdd(Session);
	Found.LastUsed = FPlatformTime::Seconds();
	Found.Domains.Append(InDomains);
	TrimSessions();
}

void FMcpHttpServer::TrimSessions()
{
	while (Sessions.Num() > MaxSessions)
	{
		const FString* Oldest = nullptr;
		double OldestTime = TNumericLimits<double>::Max();
		for (const TPair<FString, FSession>& Pair : Sessions)
		{
			if (Pair.Value.LastUsed < OldestTime)
			{
				OldestTime = Pair.Value.LastUsed;
				Oldest = &Pair.Key;
			}
		}
		if (!Oldest)
		{
			break;
		}
		const FString Key = *Oldest;
		Sessions.Remove(Key);
	}
}
