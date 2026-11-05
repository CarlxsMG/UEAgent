// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UEAgentSettings.generated.h"

/**
 * Settings of the native MCP server (Editor Preferences > Plugins > UEAgent).
 *
 * They are PER USER (Saved/Config, outside source control): the port the team really shares is
 * the one in `.mcp.json`, which does travel in the repo, and this value has to match it.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "UEAgent"))
class UEAGENT_API UUEAgentSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Serves MCP over HTTP (Streamable HTTP) from the editor itself: this is what `.mcp.json` points to. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server")
	bool bEnableHttpServer = true;

	/** Port on 127.0.0.1. `-McpHttpPort=N` on the command line overrides it. */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 HttpPort = 55561;

	/**
	 * Optional. If not empty, every request must carry `Authorization: Bearer <token>`. In Claude
	 * Code it goes into `.mcp.json` as a header ("headers"), preferably through an environment
	 * variable rather than written into the repo.
	 */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (PasswordField = true))
	FString AuthToken;

	/**
	 * Keeps a connection open after each response, for the client's next request. Turn it off when a
	 * proxy, antivirus or VPN between the client and the editor relays a response only once the
	 * server closes the connection, so the client times out waiting for `initialize`: each response
	 * then says `Connection: close` and its connection closes. `-McpHttpNoKeepAlive` on the command
	 * line turns it off too. It applies when the server starts (mcp.http.restart).
	 */
	UPROPERTY(config, EditAnywhere, Category = "MCP Server", meta = (DisplayName = "HTTP Keep-Alive"))
	bool bHttpKeepAlive = true;

	/**
	 * On editor startup, warns if the project's `.mcp.json` does not lead Claude Code to this
	 * server, with a button to fix it. "Don't show again" turns it off here.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Claude Code")
	bool bShowConnectHint = true;

	/**
	 * On editor startup, asks GitHub at most once a day whether a newer UEAgent has been released,
	 * and says so with a notification. Off, UEAgent connects to GitHub only when you click "Check for
	 * updates" in the "MCP" menu of the status bar.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Updates")
	bool bCheckForUpdatesAtStartup = false;

	/** When UEAgent last asked GitHub for the latest release (UTC ticks): the startup check waits a day. */
	UPROPERTY(config)
	int64 LastUpdateCheckTicks = 0;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** The port actually used: the command line wins over the settings. */
	uint16 GetEffectiveHttpPort() const;

	/** Whether the server keeps connections alive: off with bHttpKeepAlive off or -McpHttpNoKeepAlive. */
	bool GetEffectiveHttpKeepAlive() const;
};
