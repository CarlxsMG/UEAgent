// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

/**
 * Updating the plugin from its GitHub releases, from the editor: status bar > MCP > Updates, or
 * `mcp.update.*` in the console.
 *
 * The plugin asks GitHub only when the user checks, or on startup when "Check for updates at
 * startup" is on (at most once a day). An update is downloaded into Saved/UEAgentUpdate, checked
 * against the release's SHA256SUMS.txt, unpacked and checked again (its descriptor must name the new
 * version). The editor holds the plugin's DLLs, so the folder is replaced after it closes: on exit a
 * script (Resources/Update/apply_update.ps1, run from a copy) waits for the editor to close, moves the
 * old folder to Saved/UEAgentUpdate/Backup, moves the new one in and, when asked, opens the editor
 * again. If the new folder cannot be moved in, the old one goes back.
 *
 * Only a copy unpacked from a release zip into a project's Plugins folder is updated this way. A git
 * clone is updated with git, and a copy installed under the engine (Fab, the launcher) by its
 * installer.
 *
 * No Slate: used by the status bar, the console and the `UEAgent.Update` test.
 */
namespace McpUpdate
{
	// --- Pure parts (tested by UEAgent.Update) -------------------------------------------------------

	/** "2.3.0" or "v2.3.0" into {2, 3, 0}; false for anything else. */
	UEAGENT_API bool ParseVersion(const FString& Text, TArray<int32>& OutParts);
	/** Below, at or above zero as A is older than, the same as or newer than B; 0 when either does not parse. */
	UEAGENT_API int32 CompareVersions(const FString& A, const FString& B);
	/** UEAgent-<Version>-UE<EngineMajorMinor>-Win64.zip: the name package_release.py gives a release zip. */
	UEAGENT_API FString ZipName(const FString& Version, const FString& EngineMajorMinor);

	struct FRelease
	{
		/** Without the "v" of the tag. */
		FString Version;
		/** The release's page on GitHub. */
		FString PageUrl;
		/** The zip for this engine; both empty when the release has none. */
		FString ZipName;
		FString ZipUrl;
		int64 ZipSize = 0;
		/** SHA256SUMS.txt; empty when the release has none. */
		FString SumsUrl;
	};
	/** GitHub's JSON for a release, with the zip for this engine picked out by its name. */
	UEAGENT_API bool ParseRelease(const FString& Json, const FString& EngineMajorMinor, FRelease& OutRelease, FString& OutError);
	/** The SHA-256 (lowercase hex) that SHA256SUMS.txt gives FileName; false when it lists none. */
	UEAGENT_API bool FindSum(const FString& SumsText, const FString& FileName, FString& OutHex);
	/** SHA-256 of Data in lowercase hex, through Windows CNG; empty if it cannot be computed. */
	UEAGENT_API FString Sha256Hex(TArrayView<const uint8> Data);

	enum class EInstall : uint8
	{
		Release,     // unpacked from a release zip into a project's Plugins folder: updated here
		GitCheckout, // a git clone (a .git folder or file in the plugin folder): updated with git
		Engine,      // under the engine (Fab, the launcher): updated by its installer
	};
	UEAGENT_API EInstall InspectInstall(const FString& PluginDir, bool bUnderEngine);
	/** What to do instead, for a copy this does not update; empty for Release. */
	UEAGENT_API FString InstallAdvice(EInstall Install);

	/**
	 * Unpacks ZipPath into Dir, emptied first, and checks the result: everything under one UEAgent
	 * folder, no path that climbs out of it, and a UEAgent.uplugin whose VersionName is Version.
	 */
	UEAGENT_API bool Unpack(const FString& ZipPath, const FString& Dir, const FString& Version, FString& OutError);

	// --- The update ----------------------------------------------------------------------------------

	enum class EState : uint8
	{
		Unchecked,
		Checking,
		Current,     // the installed version is the latest release
		Available,   // a newer release
		Downloading,
		Ready,       // downloaded and checked: it is installed when the editor closes
		Failed,      // the last step failed; Message says why
	};

	struct FStatus
	{
		EState State = EState::Unchecked;
		/** The installed VersionName. */
		FString Installed;
		EInstall Install = EInstall::Release;
		/** The latest release, once a check found it. */
		FRelease Latest;
		/** One line for the user. */
		FString Message;
		/** While Downloading: 0 to 1. */
		float Progress = 0.0f;
		/** The last check was the startup one, not one the user asked for. */
		bool bAutomatic = false;
		/** The version the last exit installed; empty when it installed none. */
		FString JustInstalled;

		/** Install == Release and a release zip for this engine is there to download. */
		UEAGENT_API bool CanDownload() const;
	};

	UEAGENT_API const FStatus& Status();
	/** Broadcast on the game thread whenever Status changes. */
	UEAGENT_API FSimpleMulticastDelegate& OnChanged();

	/** Asks GitHub for the latest release: Current, Available or Failed. bAutomatic: the startup check. */
	UEAGENT_API void Check(bool bAutomatic = false);
	/** Downloads, verifies and unpacks the Available release: Ready or Failed. */
	UEAGENT_API void Download();
	/** Drops a Ready update: nothing is installed when the editor closes. */
	UEAGENT_API void Cancel();
	/** Ready: asks the editor to close (it offers to save first) and to open again once the update is in. */
	UEAGENT_API void RestartToInstall();

	/** Startup: picks up an update left Ready or installed by the last exit, and runs the startup check. */
	void Startup();
	void Shutdown();
}
