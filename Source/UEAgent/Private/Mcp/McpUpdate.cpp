// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpUpdate.h"

#include "UEAgentSettings.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileUtilities/ZipArchiveReader.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Interfaces/IMainFrameModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// Namespace name unique per file: the module builds as unity (see Build.cs).
namespace McpUpdatePrivate
{
	using McpUpdate::EState;

	static const TCHAR* const Repo = TEXT("CarlxsMG/UEAgent");
	// The startup check waits this long after the last check, whoever asked for it.
	static const FTimespan StartupCheckInterval = FTimespan::FromHours(20.0);
	// "Restart now" relaunches the editor only if it then closes within this many seconds: a close the
	// user cancels (an unsaved asset, say) must not turn a later, ordinary exit into a relaunch.
	static const double RestartWindowSeconds = 120.0;

	static McpUpdate::FStatus GStatus;
	static FSimpleMulticastDelegate GOnChanged;
	static double GRestartRequestedAt = -1.0;
	static FDelegateHandle PreExitHandle;
	static FDelegateHandle EditorInitializedHandle;
	static FTSTicker::FDelegateHandle StartupTickerHandle;

	static TSharedPtr<IPlugin> Plugin()
	{
		return IPluginManager::Get().FindPlugin(TEXT("UEAgent"));
	}

	static FString PluginDir()
	{
		const TSharedPtr<IPlugin> P = Plugin();
		return P.IsValid() ? FPaths::ConvertRelativePathToFull(P->GetBaseDir()) : FString();
	}

	// Everything an update writes lives here, in the project's Saved folder (never in source control).
	static FString WorkDir()      { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("UEAgentUpdate")); }
	static FString StagedDir()    { return WorkDir() / TEXT("Staged"); }
	static FString StagedPlugin() { return StagedDir() / TEXT("UEAgent"); }
	static FString BackupDir()    { return WorkDir() / TEXT("Backup"); }
	// Written when an update is Ready, read on the next startup: {"version": "x.y.z"}.
	static FString PendingFile()  { return WorkDir() / TEXT("pending.json"); }
	// Written by apply_update.ps1: one line per attempt, "OK <version>" or "FAILED <version>: <reason>".
	static FString LogFile()      { return WorkDir() / TEXT("apply.log"); }

	static FString EngineMajorMinor()
	{
		return FString::Printf(TEXT("%d.%d"), FEngineVersion::Current().GetMajor(), FEngineVersion::Current().GetMinor());
	}

	static void SetState(EState State, const FString& Message)
	{
		GStatus.State = State;
		GStatus.Message = Message;
		if (State == EState::Failed)
		{
			UE_LOG(LogTemp, Warning, TEXT("[MCP] update: %s"), *Message);
		}
		else if (State != EState::Downloading && State != EState::Checking)
		{
			UE_LOG(LogTemp, Display, TEXT("[MCP] update: %s"), *Message);
		}
		GOnChanged.Broadcast();
	}

	static TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpGet(const FString& Url, float TimeoutSeconds)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		// GitHub's API refuses a request without a User-Agent.
		Request->SetHeader(TEXT("User-Agent"), FString::Printf(TEXT("UEAgent/%s"), *GStatus.Installed));
		Request->SetTimeout(TimeoutSeconds);
		return Request;
	}

	// Why a request failed, for the user.
	static FString HttpFailure(const FHttpResponsePtr& Response, bool bConnected, const FString& What)
	{
		if (!bConnected || !Response.IsValid())
		{
			return FString::Printf(TEXT("Could not reach GitHub to %s: check the connection."), *What);
		}
		const int32 Code = Response->GetResponseCode();
		if (Code == 403 || Code == 429)
		{
			return FString::Printf(TEXT("GitHub refused to %s (HTTP %d, probably its limit of requests per hour): try again later."), *What, Code);
		}
		return FString::Printf(TEXT("GitHub answered HTTP %d when asked to %s."), Code, *What);
	}

	static FString ReadPendingVersion()
	{
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (!FFileHelper::LoadFileToString(Text, *PendingFile())
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
		{
			return FString();
		}
		return Json->GetStringField(TEXT("version"));
	}

	// The last line apply_update.ps1 wrote, without its timestamp; empty when it never ran.
	static FString LastLogLine()
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *LogFile());
		for (int32 i = Lines.Num() - 1; i >= 0; --i)
		{
			const FString Line = Lines[i].TrimStartAndEnd();
			if (!Line.IsEmpty())
			{
				// "2026-10-21 12:00:00Z OK 2.4.0": the result starts after the date and the time.
				int32 Space = INDEX_NONE;
				const int32 First = Line.Find(TEXT(" "));
				if (First != INDEX_NONE)
				{
					Space = Line.Find(TEXT(" "), ESearchCase::CaseSensitive, ESearchDir::FromStart, First + 1);
				}
				return Space != INDEX_NONE ? Line.Mid(Space + 1) : Line;
			}
		}
		return FString();
	}

	static void DropStaged()
	{
		IFileManager::Get().DeleteDirectory(*StagedDir(), /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().Delete(*PendingFile(), /*RequireExists*/ false, /*EvenReadOnly*/ true);
	}

	// What the last exit did with a Ready update.
	static void PickUpLastRun()
	{
		const FString Pending = ReadPendingVersion();
		if (Pending.IsEmpty())
		{
			return;
		}
		if (McpUpdate::CompareVersions(GStatus.Installed, Pending) >= 0)
		{
			// Installed: the script moved the staged folder in. The backup stays until the next update.
			DropStaged();
			GStatus.JustInstalled = Pending;
			GStatus.Message = FString::Printf(TEXT("UEAgent %s was installed when the editor closed; the previous version is in %s."), *Pending, *BackupDir());
			UE_LOG(LogTemp, Display, TEXT("[MCP] update: %s"), *GStatus.Message);
			return;
		}
		if (!FPaths::FileExists(StagedPlugin() / TEXT("UEAgent.uplugin")))
		{
			DropStaged();
			return;
		}
		GStatus.Latest.Version = Pending;
		const FString Last = LastLogLine();
		// "FAILED 2.4.0: <reason>": the reason is what the user needs.
		FString Reason;
		const bool bFailed = Last.StartsWith(TEXT("FAILED"));
		if (bFailed && !Last.Split(TEXT(": "), nullptr, &Reason))
		{
			Reason = Last;
		}
		GStatus.State = EState::Ready;
		GStatus.Message = bFailed
			? FString::Printf(TEXT("Installing UEAgent %s failed when the editor closed: %s It is tried again when the editor closes, or cancel it from the MCP menu."), *Pending, *Reason)
			: FString::Printf(TEXT("UEAgent %s is ready: it is installed when the editor closes."), *Pending);
		UE_LOG(LogTemp, Display, TEXT("[MCP] update: %s"), *GStatus.Message);
	}

	// On exit with an update Ready: start the script that installs it once the editor has closed.
	static void LaunchInstaller()
	{
		if (GStatus.State != EState::Ready || !FPaths::FileExists(StagedPlugin() / TEXT("UEAgent.uplugin")))
		{
			return;
		}
		// The script runs from a copy: the plugin folder it ships in is the one it replaces.
		const FString Script = WorkDir() / TEXT("apply_update.ps1");
		if (IFileManager::Get().Copy(*Script, *(PluginDir() / TEXT("Resources/Update/apply_update.ps1"))) != COPY_OK)
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] update: could not copy apply_update.ps1 to %s; UEAgent %s was not installed."), *Script, *GStatus.Latest.Version);
			return;
		}
		auto Quoted = [](const FString& Path) { return FString::Printf(TEXT("\"%s\""), *FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("/"), TEXT("\\"))); };
		FString Args = FString::Printf(
			TEXT("-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File %s -EditorPid %u -PluginDir %s -StagedPlugin %s -BackupDir %s -Version \"%s\" -LogPath %s"),
			*Quoted(Script), FPlatformProcess::GetCurrentProcessId(), *Quoted(PluginDir()), *Quoted(StagedPlugin()),
			*Quoted(BackupDir()), *GStatus.Latest.Version, *Quoted(LogFile()));
		const bool bRelaunch = GRestartRequestedAt >= 0.0 && FPlatformTime::Seconds() - GRestartRequestedAt < RestartWindowSeconds;
		if (bRelaunch)
		{
			Args += FString::Printf(TEXT(" -EditorExe %s -Project %s"), *Quoted(FPlatformProcess::ExecutablePath()), *Quoted(FPaths::GetProjectFilePath()));
		}
		FProcHandle Process = FPlatformProcess::CreateProc(TEXT("powershell.exe"), *Args,
			/*bLaunchDetached*/ true, /*bLaunchHidden*/ true, /*bLaunchReallyHidden*/ true,
			/*OutProcessID*/ nullptr, /*PriorityModifier*/ 0, /*OptionalWorkingDirectory*/ nullptr, /*PipeWriteChild*/ nullptr);
		if (Process.IsValid())
		{
			UE_LOG(LogTemp, Display, TEXT("[MCP] update: installing UEAgent %s once the editor has closed%s (log: %s)."),
				*GStatus.Latest.Version, bRelaunch ? TEXT(", then opening it again") : TEXT(""), *LogFile());
			FPlatformProcess::CloseProc(Process);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[MCP] update: could not start powershell.exe; UEAgent %s stays ready for the next exit."), *GStatus.Latest.Version);
		}
	}

	static void OnPreExit()
	{
		LaunchInstaller();
	}

	static void OnEditorInitialized(double /*Duration*/)
	{
		const UUEAgentSettings* Settings = GetDefault<UUEAgentSettings>();
		if (!Settings->bCheckForUpdatesAtStartup || IsRunningCommandlet() || FApp::IsUnattended() || GIsAutomationTesting
			|| GStatus.State != EState::Unchecked)
		{
			return;
		}
		const FDateTime Last(Settings->LastUpdateCheckTicks);
		if (Settings->LastUpdateCheckTicks > 0 && FDateTime::UtcNow() - Last < StartupCheckInterval)
		{
			return;
		}
		// A few seconds after startup, like the connect hint: the editor chains its own notifications first.
		StartupTickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			StartupTickerHandle.Reset();
			McpUpdate::Check(/*bAutomatic*/ true);
			return false;
		}), 8.0f);
	}

	static void RememberCheck()
	{
		UUEAgentSettings* Settings = GetMutableDefault<UUEAgentSettings>();
		Settings->LastUpdateCheckTicks = FDateTime::UtcNow().GetTicks();
		Settings->SaveConfig();
	}

	static void OnZipDownloaded(FHttpResponsePtr Response, bool bConnected, const FString& ExpectedSum)
	{
		const McpUpdate::FRelease Release = GStatus.Latest;
		if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			SetState(EState::Failed, HttpFailure(Response, bConnected, FString::Printf(TEXT("download %s"), *Release.ZipName)));
			return;
		}
		const TArray<uint8>& Bytes = Response->GetContent();
		const FString Sum = McpUpdate::Sha256Hex(Bytes);
		if (Sum.IsEmpty() || Sum != ExpectedSum)
		{
			SetState(EState::Failed, FString::Printf(TEXT("%s does not match the SHA-256 that SHA256SUMS.txt gives it (%s, downloaded %s): nothing was installed."),
				*Release.ZipName, *ExpectedSum, Sum.IsEmpty() ? TEXT("not computed") : *Sum));
			return;
		}
		const FString ZipPath = WorkDir() / Release.ZipName;
		FString Error;
		if (!FFileHelper::SaveArrayToFile(Bytes, *ZipPath))
		{
			SetState(EState::Failed, FString::Printf(TEXT("Could not save %s: nothing was installed."), *ZipPath));
			return;
		}
		if (!McpUpdate::Unpack(ZipPath, StagedDir(), Release.Version, Error))
		{
			DropStaged();
			SetState(EState::Failed, Error + TEXT(" Nothing was installed."));
			return;
		}
		IFileManager::Get().Delete(*ZipPath, /*RequireExists*/ false, /*EvenReadOnly*/ true);
		IFileManager::Get().Delete(*LogFile(), /*RequireExists*/ false, /*EvenReadOnly*/ true);
		const FString Pending = FString::Printf(TEXT("{\"version\": \"%s\"}\n"), *Release.Version);
		if (!FFileHelper::SaveStringToFile(Pending, *PendingFile()))
		{
			DropStaged();
			SetState(EState::Failed, FString::Printf(TEXT("Could not write %s: nothing was installed."), *PendingFile()));
			return;
		}
		GStatus.Progress = 1.0f;
		SetState(EState::Ready, FString::Printf(TEXT("UEAgent %s is ready: it is installed when the editor closes."), *Release.Version));
	}
}

// --- Pure parts ------------------------------------------------------------------------------------

bool McpUpdate::ParseVersion(const FString& Text, TArray<int32>& OutParts)
{
	OutParts.Reset();
	FString Body = Text.TrimStartAndEnd();
	if (Body.StartsWith(TEXT("v"), ESearchCase::IgnoreCase))
	{
		Body.RightChopInline(1);
	}
	TArray<FString> Parts;
	Body.ParseIntoArray(Parts, TEXT("."), /*CullEmpty*/ false);
	if (Parts.Num() < 2 || Parts.Num() > 4)
	{
		return false;
	}
	for (const FString& Part : Parts)
	{
		if (Part.IsEmpty() || Part.Len() > 6)
		{
			return false;
		}
		for (const TCHAR Char : Part)
		{
			if (!FChar::IsDigit(Char))
			{
				return false;
			}
		}
		OutParts.Add(FCString::Atoi(*Part));
	}
	return true;
}

int32 McpUpdate::CompareVersions(const FString& A, const FString& B)
{
	TArray<int32> PartsA, PartsB;
	if (!ParseVersion(A, PartsA) || !ParseVersion(B, PartsB))
	{
		return 0;
	}
	const int32 Count = FMath::Max(PartsA.Num(), PartsB.Num());
	for (int32 i = 0; i < Count; ++i)
	{
		const int32 PartA = PartsA.IsValidIndex(i) ? PartsA[i] : 0;
		const int32 PartB = PartsB.IsValidIndex(i) ? PartsB[i] : 0;
		if (PartA != PartB)
		{
			return PartA < PartB ? -1 : 1;
		}
	}
	return 0;
}

FString McpUpdate::ZipName(const FString& Version, const FString& EngineMajorMinor)
{
	return FString::Printf(TEXT("UEAgent-%s-UE%s-Win64.zip"), *Version, *EngineMajorMinor);
}

bool McpUpdate::ParseRelease(const FString& Json, const FString& EngineMajorMinor, FRelease& OutRelease, FString& OutError)
{
	OutRelease = FRelease();
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
	{
		OutError = TEXT("GitHub's answer is not a JSON object.");
		return false;
	}
	FString Tag;
	if (!Object->TryGetStringField(TEXT("tag_name"), Tag))
	{
		OutError = TEXT("GitHub's answer names no release (no tag_name).");
		return false;
	}
	TArray<int32> Parts;
	if (!ParseVersion(Tag, Parts))
	{
		OutError = FString::Printf(TEXT("The latest release's tag, \"%s\", is not a version."), *Tag);
		return false;
	}
	OutRelease.Version = Tag.TrimStartAndEnd();
	if (OutRelease.Version.StartsWith(TEXT("v"), ESearchCase::IgnoreCase))
	{
		OutRelease.Version.RightChopInline(1);
	}
	Object->TryGetStringField(TEXT("html_url"), OutRelease.PageUrl);
	const FString WantedZip = ZipName(OutRelease.Version, EngineMajorMinor);
	const TArray<TSharedPtr<FJsonValue>>* Assets = nullptr;
	if (Object->TryGetArrayField(TEXT("assets"), Assets))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Assets)
		{
			const TSharedPtr<FJsonObject>* Asset = nullptr;
			FString Name, Url;
			if (!Value.IsValid() || !Value->TryGetObject(Asset) || !(*Asset)->TryGetStringField(TEXT("name"), Name)
				|| !(*Asset)->TryGetStringField(TEXT("browser_download_url"), Url))
			{
				continue;
			}
			if (Name == WantedZip)
			{
				OutRelease.ZipName = Name;
				OutRelease.ZipUrl = Url;
				double Size = 0.0;
				(*Asset)->TryGetNumberField(TEXT("size"), Size);
				OutRelease.ZipSize = static_cast<int64>(Size);
			}
			else if (Name == TEXT("SHA256SUMS.txt"))
			{
				OutRelease.SumsUrl = Url;
			}
		}
	}
	return true;
}

bool McpUpdate::FindSum(const FString& SumsText, const FString& FileName, FString& OutHex)
{
	TArray<FString> Lines;
	SumsText.ParseIntoArrayLines(Lines, /*CullEmpty*/ true);
	for (const FString& Line : Lines)
	{
		// "<hex>  <name>", or "<hex> *<name>" (sha256sum's binary mode).
		FString Hex, Name;
		if (!Line.TrimStartAndEnd().Split(TEXT(" "), &Hex, &Name))
		{
			continue;
		}
		Name.TrimStartInline();
		Name.RemoveFromStart(TEXT("*"));
		if (Name == FileName && Hex.Len() == 64)
		{
			OutHex = Hex.ToLower();
			return true;
		}
	}
	return false;
}

FString McpUpdate::Sha256Hex(TArrayView<const uint8> Data)
{
#if PLATFORM_WINDOWS
	BCRYPT_ALG_HANDLE Algorithm = nullptr;
	BCRYPT_HASH_HANDLE Hash = nullptr;
	uint8 Digest[32] = {};
	bool bOk = BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0
		&& BCryptCreateHash(Algorithm, &Hash, nullptr, 0, nullptr, 0, 0) == 0;
	// BCryptHashData takes a ULONG length: feed it in pieces.
	for (int64 Offset = 0; bOk && Offset < Data.Num(); Offset += MAX_int32)
	{
		const ULONG Piece = static_cast<ULONG>(FMath::Min<int64>(Data.Num() - Offset, MAX_int32));
		bOk = BCryptHashData(Hash, const_cast<PUCHAR>(Data.GetData() + Offset), Piece, 0) == 0;
	}
	bOk = bOk && BCryptFinishHash(Hash, Digest, sizeof(Digest), 0) == 0;
	if (Hash)
	{
		BCryptDestroyHash(Hash);
	}
	if (Algorithm)
	{
		BCryptCloseAlgorithmProvider(Algorithm, 0);
	}
	return bOk ? BytesToHex(Digest, sizeof(Digest)).ToLower() : FString();
#else
	return FString();
#endif
}

McpUpdate::EInstall McpUpdate::InspectInstall(const FString& PluginDir, bool bUnderEngine)
{
	if (bUnderEngine)
	{
		return EInstall::Engine;
	}
	// A clone has a .git folder; a worktree or a submodule, a .git file.
	const FString Git = PluginDir / TEXT(".git");
	if (IFileManager::Get().DirectoryExists(*Git) || IFileManager::Get().FileExists(*Git))
	{
		return EInstall::GitCheckout;
	}
	return EInstall::Release;
}

FString McpUpdate::InstallAdvice(EInstall Install)
{
	switch (Install)
	{
	case EInstall::GitCheckout:
		return TEXT("This copy of UEAgent is a git clone: update it with git pull, not from here.");
	case EInstall::Engine:
		return TEXT("This copy of UEAgent is installed in the engine: update it from your Fab Library in the Epic Games Launcher.");
	default:
		return FString();
	}
}

bool McpUpdate::Unpack(const FString& ZipPath, const FString& Dir, const FString& Version, FString& OutError)
{
	IFileManager& Files = IFileManager::Get();
	Files.DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
	if (Files.DirectoryExists(*Dir) || !Files.MakeDirectory(*Dir, /*Tree*/ true))
	{
		OutError = FString::Printf(TEXT("Could not empty %s."), *Dir);
		return false;
	}
	IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*ZipPath);
	if (!Handle)
	{
		OutError = FString::Printf(TEXT("Could not open %s."), *ZipPath);
		return false;
	}
	FZipArchiveReader Reader(Handle); // owns the handle from here on
	if (!Reader.IsValid())
	{
		OutError = FString::Printf(TEXT("%s is not a zip archive."), *ZipPath);
		return false;
	}
	int32 Count = 0;
	for (const FString& Name : Reader.GetFileNames())
	{
		if (Name.EndsWith(TEXT("/")))
		{
			continue; // a folder entry
		}
		TArray<FString> Segments;
		Name.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty*/ false);
		const bool bClimbs = Segments.ContainsByPredicate([](const FString& S) { return S.IsEmpty() || S == TEXT(".") || S == TEXT(".."); });
		if (Segments.Num() < 2 || Segments[0] != TEXT("UEAgent") || bClimbs || Name.Contains(TEXT("\\")) || Name.Contains(TEXT(":")))
		{
			OutError = FString::Printf(TEXT("The zip holds \"%s\", which is not inside its UEAgent folder."), *Name);
			return false;
		}
		TArray<uint8> Bytes;
		if (!Reader.TryReadFile(Name, Bytes) || !FFileHelper::SaveArrayToFile(Bytes, *(Dir / Name)))
		{
			OutError = FString::Printf(TEXT("Could not unpack %s from the zip."), *Name);
			return false;
		}
		++Count;
	}
	FString Descriptor;
	TSharedPtr<FJsonObject> Json;
	FString Found;
	if (!FFileHelper::LoadFileToString(Descriptor, *(Dir / TEXT("UEAgent/UEAgent.uplugin")))
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Descriptor), Json) || !Json.IsValid()
		|| !Json->TryGetStringField(TEXT("VersionName"), Found))
	{
		OutError = FString::Printf(TEXT("The zip (%d files) has no readable UEAgent/UEAgent.uplugin."), Count);
		return false;
	}
	if (Found != Version)
	{
		OutError = FString::Printf(TEXT("The zip holds UEAgent %s, not the %s it was downloaded as."), *Found, *Version);
		return false;
	}
	return true;
}

// --- The update ------------------------------------------------------------------------------------

bool McpUpdate::FStatus::CanDownload() const
{
	return Install == EInstall::Release && !Latest.ZipUrl.IsEmpty() && !Latest.SumsUrl.IsEmpty();
}

const McpUpdate::FStatus& McpUpdate::Status()
{
	return McpUpdatePrivate::GStatus;
}

FSimpleMulticastDelegate& McpUpdate::OnChanged()
{
	return McpUpdatePrivate::GOnChanged;
}

void McpUpdate::Check(bool bAutomatic)
{
	using namespace McpUpdatePrivate;
	if (GStatus.State == EState::Checking || GStatus.State == EState::Downloading)
	{
		return;
	}
	const EState Before = GStatus.State;
	const FString ReadyVersion = Before == EState::Ready ? GStatus.Latest.Version : FString();
	GStatus.bAutomatic = bAutomatic;
	SetState(EState::Checking, TEXT("Asking GitHub for the latest release..."));

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = HttpGet(FString::Printf(TEXT("https://api.github.com/repos/%s/releases/latest"), Repo), 30.0f);
	Request->SetHeader(TEXT("Accept"), TEXT("application/vnd.github+json"));
	Request->OnProcessRequestComplete().BindLambda([ReadyVersion](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			SetState(EState::Failed, HttpFailure(Response, bConnected, TEXT("check for updates")));
			return;
		}
		RememberCheck();
		McpUpdate::FRelease Release;
		FString Error;
		if (!McpUpdate::ParseRelease(Response->GetContentAsString(), EngineMajorMinor(), Release, Error))
		{
			SetState(EState::Failed, Error);
			return;
		}
		GStatus.Latest = Release;
		if (!ReadyVersion.IsEmpty() && McpUpdate::CompareVersions(Release.Version, ReadyVersion) <= 0)
		{
			SetState(EState::Ready, FString::Printf(TEXT("UEAgent %s is ready: it is installed when the editor closes."), *ReadyVersion));
			return;
		}
		if (McpUpdate::CompareVersions(Release.Version, GStatus.Installed) <= 0)
		{
			SetState(EState::Current, FString::Printf(TEXT("UEAgent %s is the latest version."), *GStatus.Installed));
			return;
		}
		FString Message = FString::Printf(TEXT("UEAgent %s is available (you have %s)."), *Release.Version, *GStatus.Installed);
		if (GStatus.Install != McpUpdate::EInstall::Release)
		{
			Message += TEXT(" ") + McpUpdate::InstallAdvice(GStatus.Install);
		}
		else if (Release.ZipUrl.IsEmpty())
		{
			Message += FString::Printf(TEXT(" It has no zip for Unreal Engine %s, so it cannot be installed from here."), *EngineMajorMinor());
		}
		else if (Release.SumsUrl.IsEmpty())
		{
			Message += TEXT(" It has no SHA256SUMS.txt to check the download against, so it is not installed from here.");
		}
		SetState(EState::Available, Message);
	});
	Request->ProcessRequest();
}

void McpUpdate::Download()
{
	using namespace McpUpdatePrivate;
	if (GStatus.State != EState::Available || !GStatus.CanDownload())
	{
		UE_LOG(LogTemp, Warning, TEXT("[MCP] update: nothing to download (%s)"),
			GStatus.State == EState::Available ? *GStatus.Message : TEXT("check for updates first"));
		return;
	}
	if (!IFileManager::Get().MakeDirectory(*WorkDir(), /*Tree*/ true))
	{
		SetState(EState::Failed, FString::Printf(TEXT("Could not create %s."), *WorkDir()));
		return;
	}
	GStatus.Progress = 0.0f;
	SetState(EState::Downloading, FString::Printf(TEXT("Downloading UEAgent %s..."), *GStatus.Latest.Version));

	// The checksums first: a release without one for this zip is not installed.
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Sums = HttpGet(GStatus.Latest.SumsUrl, 60.0f);
	Sums->OnProcessRequestComplete().BindLambda([](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		const McpUpdate::FRelease Release = GStatus.Latest;
		if (!bConnected || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			SetState(EState::Failed, HttpFailure(Response, bConnected, TEXT("download SHA256SUMS.txt")));
			return;
		}
		FString Expected;
		if (!McpUpdate::FindSum(Response->GetContentAsString(), Release.ZipName, Expected))
		{
			SetState(EState::Failed, FString::Printf(TEXT("SHA256SUMS.txt lists no checksum for %s, so it is not installed."), *Release.ZipName));
			return;
		}
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Zip = HttpGet(Release.ZipUrl, 600.0f);
		Zip->OnRequestProgress64().BindLambda([Size = Release.ZipSize](FHttpRequestPtr, uint64 /*Sent*/, uint64 Received)
		{
			if (Size > 0)
			{
				GStatus.Progress = FMath::Clamp(static_cast<float>(static_cast<double>(Received) / static_cast<double>(Size)), 0.0f, 1.0f);
				GOnChanged.Broadcast();
			}
		});
		Zip->OnProcessRequestComplete().BindLambda([Expected](FHttpRequestPtr, FHttpResponsePtr ZipResponse, bool bZipConnected)
		{
			OnZipDownloaded(ZipResponse, bZipConnected, Expected);
		});
		Zip->ProcessRequest();
	});
	Sums->ProcessRequest();
}

void McpUpdate::Cancel()
{
	using namespace McpUpdatePrivate;
	if (GStatus.State != EState::Ready)
	{
		return;
	}
	DropStaged();
	const bool bKnown = !GStatus.Latest.ZipUrl.IsEmpty();
	SetState(bKnown ? EState::Available : EState::Unchecked,
		FString::Printf(TEXT("The update to UEAgent %s was cancelled: nothing is installed when the editor closes."), *GStatus.Latest.Version));
}

void McpUpdate::RestartToInstall()
{
	using namespace McpUpdatePrivate;
	if (GStatus.State != EState::Ready || !FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")))
	{
		return;
	}
	GRestartRequestedAt = FPlatformTime::Seconds();
	FModuleManager::GetModuleChecked<IMainFrameModule>(TEXT("MainFrame")).RequestCloseEditor();
}

void McpUpdate::Startup()
{
	using namespace McpUpdatePrivate;
	const TSharedPtr<IPlugin> P = Plugin();
	GStatus.Installed = P.IsValid() ? P->GetDescriptor().VersionName : FString();
	GStatus.Install = InspectInstall(PluginDir(), P.IsValid() && P->GetLoadedFrom() == EPluginLoadedFrom::Engine);
	PickUpLastRun();
	PreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&OnPreExit);
	EditorInitializedHandle = FEditorDelegates::OnEditorInitialized.AddStatic(&OnEditorInitialized);
}

void McpUpdate::Shutdown()
{
	using namespace McpUpdatePrivate;
	if (StartupTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StartupTickerHandle);
		StartupTickerHandle.Reset();
	}
	if (EditorInitializedHandle.IsValid())
	{
		FEditorDelegates::OnEditorInitialized.Remove(EditorInitializedHandle);
		EditorInitializedHandle.Reset();
	}
	if (PreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		PreExitHandle.Reset();
	}
}

// Console: the same as the status-bar menu, for an editor without a UI or a test.
namespace McpUpdateConsole
{
	static FAutoConsoleCommand CheckCommand(
		TEXT("mcp.update.check"),
		TEXT("Asks GitHub whether a newer UEAgent has been released; the answer is logged."),
		FConsoleCommandDelegate::CreateLambda([]() { McpUpdate::Check(); }));

	static FAutoConsoleCommand StatusCommand(
		TEXT("mcp.update.status"),
		TEXT("Logs the installed version and the state of the update."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			const McpUpdate::FStatus& S = McpUpdate::Status();
			static const TCHAR* const States[] = { TEXT("unchecked"), TEXT("checking"), TEXT("current"), TEXT("available"), TEXT("downloading"), TEXT("ready"), TEXT("failed") };
			UE_LOG(LogTemp, Display, TEXT("[MCP] mcp.update.status: installed %s, %s, %s%s"), *S.Installed, States[static_cast<uint8>(S.State)],
				S.Message.IsEmpty() ? TEXT("never checked.") : *S.Message,
				S.Install == McpUpdate::EInstall::Release ? TEXT("") : *(TEXT(" ") + McpUpdate::InstallAdvice(S.Install)));
		}));

	static FAutoConsoleCommand DownloadCommand(
		TEXT("mcp.update.download"),
		TEXT("Downloads and checks the release mcp.update.check found; it is installed when the editor closes."),
		FConsoleCommandDelegate::CreateLambda([]() { McpUpdate::Download(); }));

	static FAutoConsoleCommand CancelCommand(
		TEXT("mcp.update.cancel"),
		TEXT("Drops a downloaded update: nothing is installed when the editor closes."),
		FConsoleCommandDelegate::CreateLambda([]() { McpUpdate::Cancel(); }));
}
