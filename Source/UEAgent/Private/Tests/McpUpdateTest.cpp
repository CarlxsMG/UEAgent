// Copyright (c) 2026 CarlxsMG.

// Test of the parts of the updater (Mcp/McpUpdate.h) that need no network: versions, the release JSON,
// the checksums, the kind of install and the unpacking, on temporary folders. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.Update" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// What matters most is what must be refused: a zip of another version, a path that climbs out of
// the plugin folder, a git clone or an engine install to overwrite.

#include "Mcp/McpUpdate.h"

#include "FileUtilities/ZipArchiveWriter.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpUpdateTestPrivate
{
	static FString CaseDir(const TCHAR* Name)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpUpdate") / Name);
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}

	static TArray<uint8> Utf8(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}

	// A deflated zip, as package_release.py writes them, holding Files (name -> text).
	static FString WriteZip(const FString& Path, const TMap<FString, FString>& Files)
	{
		{
			FZipArchiveWriter Writer(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Path), EZipArchiveOptions::Deflate);
			for (const TPair<FString, FString>& File : Files)
			{
				Writer.AddFile(File.Key, Utf8(File.Value), FDateTime::Now());
			}
		}
		return Path;
	}

	static FString Descriptor(const TCHAR* Version)
	{
		return FString::Printf(TEXT("{\n\t\"FileVersion\": 3,\n\t\"VersionName\": \"%s\",\n\t\"FriendlyName\": \"UEAgent\"\n}\n"), Version);
	}

	static const TCHAR* const ReleaseJson = TEXT(R"({
		"tag_name": "v2.4.0",
		"html_url": "https://github.com/CarlxsMG/UEAgent/releases/tag/v2.4.0",
		"assets": [
			{ "name": "SHA256SUMS.txt", "size": 400, "browser_download_url": "https://example.test/SHA256SUMS.txt" },
			{ "name": "UEAgent-2.4.0-UE5.7-Win64.zip", "size": 9000000, "browser_download_url": "https://example.test/57.zip" },
			{ "name": "UEAgent-2.4.0-UE5.8-Win64-symbols.zip", "size": 69000000, "browser_download_url": "https://example.test/58-symbols.zip" },
			{ "name": "UEAgent-2.4.0-UE5.8-Win64.zip", "size": 9729646, "browser_download_url": "https://example.test/58.zip" }
		]
	})");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpUpdateTest, "UEAgent.Update",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpUpdateTest::RunTest(const FString& Parameters)
{
	using namespace McpUpdateTestPrivate;
	using McpUpdate::EInstall;

	// Versions.
	TArray<int32> Parts;
	TestTrue(TEXT("2.3.0 parses"), McpUpdate::ParseVersion(TEXT("2.3.0"), Parts) && Parts == TArray<int32>({ 2, 3, 0 }));
	TestTrue(TEXT("a tag's v is dropped"), McpUpdate::ParseVersion(TEXT("v2.10.1"), Parts) && Parts == TArray<int32>({ 2, 10, 1 }));
	TestFalse(TEXT("one number is not a version"), McpUpdate::ParseVersion(TEXT("2"), Parts));
	TestFalse(TEXT("a letter is not a version"), McpUpdate::ParseVersion(TEXT("2.x.0"), Parts));
	TestFalse(TEXT("a pre-release suffix is not a version"), McpUpdate::ParseVersion(TEXT("2.3.0-beta"), Parts));
	TestFalse(TEXT("empty is not a version"), McpUpdate::ParseVersion(TEXT(""), Parts));
	TestTrue(TEXT("2.10.0 is newer than 2.9.9 (numbers, not text)"), McpUpdate::CompareVersions(TEXT("2.10.0"), TEXT("2.9.9")) > 0);
	TestTrue(TEXT("2.2.0 is older than 2.3.0"), McpUpdate::CompareVersions(TEXT("2.2.0"), TEXT("2.3.0")) < 0);
	TestEqual(TEXT("v2.3.0 is 2.3.0"), McpUpdate::CompareVersions(TEXT("v2.3.0"), TEXT("2.3.0")), 0);
	TestEqual(TEXT("2.3 is 2.3.0"), McpUpdate::CompareVersions(TEXT("2.3"), TEXT("2.3.0")), 0);
	TestEqual(TEXT("an unparsable version compares as equal, so it never looks newer"), McpUpdate::CompareVersions(TEXT("next"), TEXT("2.3.0")), 0);
	TestEqual(TEXT("the zip name package_release.py gives"), McpUpdate::ZipName(TEXT("2.3.0"), TEXT("5.8")), FString(TEXT("UEAgent-2.3.0-UE5.8-Win64.zip")));

	// The release JSON.
	McpUpdate::FRelease Release;
	FString Error;
	TestTrue(TEXT("the release parses"), McpUpdate::ParseRelease(ReleaseJson, TEXT("5.8"), Release, Error));
	TestEqual(TEXT("its version, without the v"), Release.Version, FString(TEXT("2.4.0")));
	TestEqual(TEXT("the 5.8 zip, not its symbols"), Release.ZipUrl, FString(TEXT("https://example.test/58.zip")));
	TestEqual(TEXT("the zip's size"), Release.ZipSize, static_cast<int64>(9729646));
	TestEqual(TEXT("the checksums"), Release.SumsUrl, FString(TEXT("https://example.test/SHA256SUMS.txt")));
	TestTrue(TEXT("the page"), Release.PageUrl.EndsWith(TEXT("/v2.4.0")));
	TestTrue(TEXT("a release parses for an engine it has no zip for"), McpUpdate::ParseRelease(ReleaseJson, TEXT("5.9"), Release, Error));
	TestTrue(TEXT("... and has no zip to download"), Release.ZipUrl.IsEmpty());
	TestFalse(TEXT("not JSON"), McpUpdate::ParseRelease(TEXT("<html>"), TEXT("5.8"), Release, Error));
	TestFalse(TEXT("a tag that is not a version"), McpUpdate::ParseRelease(TEXT(R"({"tag_name": "latest", "assets": []})"), TEXT("5.8"), Release, Error));

	// Checksums, in SHA256SUMS.txt's format.
	const FString A64 = FString::ChrN(64, TEXT('A'));
	const FString B64 = FString::ChrN(64, TEXT('b'));
	const FString Sums = FString::Printf(TEXT("%s  UEAgent-2.4.0-UE5.8-Win64.zip\n%s *UEAgent-2.4.0-UE5.7-Win64.zip\n"), *A64, *B64);
	FString Hex;
	TestTrue(TEXT("the sum of a listed file, in lowercase"), McpUpdate::FindSum(Sums, TEXT("UEAgent-2.4.0-UE5.8-Win64.zip"), Hex) && Hex == A64.ToLower());
	TestTrue(TEXT("sha256sum's binary mode (*name)"), McpUpdate::FindSum(Sums, TEXT("UEAgent-2.4.0-UE5.7-Win64.zip"), Hex) && Hex == B64);
	TestFalse(TEXT("a file it does not list"), McpUpdate::FindSum(Sums, TEXT("UEAgent-2.4.0-UE5.9-Win64.zip"), Hex));
	TestEqual(TEXT("SHA-256 of abc (FIPS 180-2)"), McpUpdate::Sha256Hex(Utf8(TEXT("abc"))),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("SHA-256 of nothing"), McpUpdate::Sha256Hex(TArray<uint8>()),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));

	// The kind of install: only a release copy is replaced.
	const FString Installs = CaseDir(TEXT("Installs"));
	IFileManager::Get().MakeDirectory(*(Installs / TEXT("Clone/.git")), /*Tree*/ true);
	FFileHelper::SaveStringToFile(TEXT("gitdir: ../.git/worktrees/x\n"), *(Installs / TEXT("Worktree/.git")));
	IFileManager::Get().MakeDirectory(*(Installs / TEXT("Release")), /*Tree*/ true);
	TestTrue(TEXT("a clone"), McpUpdate::InspectInstall(Installs / TEXT("Clone"), false) == EInstall::GitCheckout);
	TestTrue(TEXT("a worktree (.git is a file)"), McpUpdate::InspectInstall(Installs / TEXT("Worktree"), false) == EInstall::GitCheckout);
	TestTrue(TEXT("a release copy"), McpUpdate::InspectInstall(Installs / TEXT("Release"), false) == EInstall::Release);
	TestTrue(TEXT("under the engine, even without .git"), McpUpdate::InspectInstall(Installs / TEXT("Release"), true) == EInstall::Engine);
	TestTrue(TEXT("a clone is told to use git"), McpUpdate::InstallAdvice(EInstall::GitCheckout).Contains(TEXT("git pull")));
	TestTrue(TEXT("a release copy needs no advice"), McpUpdate::InstallAdvice(EInstall::Release).IsEmpty());

	// Unpacking.
	const FString Root = CaseDir(TEXT("Unpack"));
	const FString Good = WriteZip(Root / TEXT("good.zip"), {
		{ TEXT("UEAgent/UEAgent.uplugin"), Descriptor(TEXT("2.4.0")) },
		{ TEXT("UEAgent/Resources/Update/apply_update.ps1"), TEXT("# script\n") } });
	const FString Staged = Root / TEXT("Staged");
	FFileHelper::SaveStringToFile(TEXT("stale"), *(Staged / TEXT("UEAgent/Old.txt")));
	TestTrue(TEXT("a good zip unpacks"), McpUpdate::Unpack(Good, Staged, TEXT("2.4.0"), Error));
	TestTrue(TEXT("... with its files"), FPaths::FileExists(Staged / TEXT("UEAgent/Resources/Update/apply_update.ps1")));
	TestFalse(TEXT("... into an emptied folder"), FPaths::FileExists(Staged / TEXT("UEAgent/Old.txt")));
	TestFalse(TEXT("a zip of another version is refused"), McpUpdate::Unpack(Good, Staged, TEXT("2.5.0"), Error));
	TestTrue(TEXT("... and says which it holds"), Error.Contains(TEXT("2.4.0")));
	const FString Climbs = WriteZip(Root / TEXT("climbs.zip"), {
		{ TEXT("UEAgent/UEAgent.uplugin"), Descriptor(TEXT("2.4.0")) },
		{ TEXT("UEAgent/../../Evil.txt"), TEXT("x") } });
	TestFalse(TEXT("a path that climbs out is refused"), McpUpdate::Unpack(Climbs, Root / TEXT("Climbs"), TEXT("2.4.0"), Error));
	TestFalse(TEXT("... and nothing lands outside"), FPaths::FileExists(Root / TEXT("Evil.txt")));
	const FString Elsewhere = WriteZip(Root / TEXT("elsewhere.zip"), {
		{ TEXT("Other/UEAgent.uplugin"), Descriptor(TEXT("2.4.0")) } });
	TestFalse(TEXT("a zip without the UEAgent folder is refused"), McpUpdate::Unpack(Elsewhere, Root / TEXT("Elsewhere"), TEXT("2.4.0"), Error));
	FFileHelper::SaveStringToFile(TEXT("not a zip"), *(Root / TEXT("text.zip")));
	TestFalse(TEXT("a file that is not a zip is refused"), McpUpdate::Unpack(Root / TEXT("text.zip"), Root / TEXT("Text"), TEXT("2.4.0"), Error));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
