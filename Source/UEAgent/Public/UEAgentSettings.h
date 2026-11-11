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
	 * Only the tools that read, or that only capture an image under Saved, are listed and run: anything that changes
	 * the project or runs code is refused, undo and the transaction tools included. `-McpReadOnly` on the command line
	 * or UEAGENT_READONLY=1 in the environment turns it on whatever this says. Also in the "MCP" status bar menu.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Permissions", meta = (DisplayName = "Read-only mode"))
	bool bReadOnlyMode = false;

	/**
	 * Off, the tools that run code or a process the caller supplies (execute_python, execute_python_file,
	 * execute_console_command, call_function, run_external_project_script, launch_dedicated_server, start_cpp_compile)
	 * are not listed and are refused. With it on, execute_python has the editor's own rights.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Permissions")
	bool bAllowCodeExecution = true;

	/** Every tool of these domains (as find_tools names them) is not listed and is refused. */
	UPROPERTY(config, EditAnywhere, Category = "Permissions", meta = (GetOptions = "GetDomainOptions"))
	TArray<FString> DisabledDomains;

	/** These tools, by name, are not listed and are refused. An internal check of the preconditions or the readback
	 *  that reads with one of them still runs. */
	UPROPERTY(config, EditAnywhere, Category = "Permissions")
	TArray<FString> DisabledTools;

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

	/**
	 * The capture tools (capture_viewport, capture_high_res_screenshot, capture_pie_screenshot,
	 * render_widget_preview, generate_asset_thumbnail) answer with the picture as an MCP image next to
	 * their JSON, so the agent sees it. Off, they answer with the file path alone. A call can also say
	 * `return_image: false`, and a client `?images=off` at the end of the server URL.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Images", meta = (DisplayName = "Return Images"))
	bool bReturnImages = true;

	/** Longest side of the image sent to the client, in pixels. A larger capture is sent downscaled; the file on disk keeps its size. */
	UPROPERTY(config, EditAnywhere, Category = "Images", meta = (ClampMin = "64", ClampMax = "8192"))
	int32 ImageMaxDimension = 1568;

	/** Largest encoded image sent to the client, in bytes. A larger one is sent downscaled as JPEG; the file on disk is not changed. */
	UPROPERTY(config, EditAnywhere, Category = "Images", meta = (ClampMin = "65536", ClampMax = "20971520"))
	int32 ImageMaxBytes = 1048576;

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** The port actually used: the command line wins over the settings. */
	uint16 GetEffectiveHttpPort() const;

	/** Whether the server keeps connections alive: off with bHttpKeepAlive off or -McpHttpNoKeepAlive. */
	bool GetEffectiveHttpKeepAlive() const;

	/** Whether read-only mode is on: bReadOnlyMode, -McpReadOnly or UEAGENT_READONLY=1. OutSource says which. */
	bool GetEffectiveReadOnly(FString& OutSource) const;
	/** Whether the command line or the environment turns read-only mode on, so the setting cannot turn it off. */
	static bool IsReadOnlyForcedByLaunch(FString* OutSource = nullptr);
	/** Sets bReadOnlyMode, saves it and applies it (FMcpToolPolicy::Rebuild). Game thread. */
	void SetReadOnlyMode(bool bReadOnly);

	/** The catalog's domains, offered for DisabledDomains. */
	UFUNCTION()
	TArray<FString> GetDomainOptions() const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif
};
