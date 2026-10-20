// Test of the skills installer (Mcp/McpSkills.h) on temporary folders: it touches neither the
// project's .claude/skills or .agents/skills nor the plugin's Resources/Skills. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.Skills" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// What is tested above all is what must NOT be touched: a folder the installer did not make, even
// with one of its names, and the user's own skills next to them.

#include "Mcp/McpSkills.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpSkillsTestPrivate
{
	static FString CaseDir(const TCHAR* Name)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / TEXT("McpSkills") / Name);
		IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
		return Dir;
	}

	static void WriteText(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static FString ReadText(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	static void WriteSkill(const FString& Dir, const FString& Name, const FString& Extra = FString())
	{
		WriteText(Dir / Name / TEXT("SKILL.md"), FString::Printf(TEXT("---\nname: %s\ndescription: Test skill %s.\n%s---\n\n# %s\n\nBody.\n"), *Name, *Name, *Extra, *Name));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpSkillsTest, "UEAgent.Skills",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpSkillsTest::RunTest(const FString& Parameters)
{
	using namespace McpSkillsTestPrivate;
	using McpSkills::ESkillState;

	const FString Root = CaseDir(TEXT("Run"));
	const FString Shipped = Root / TEXT("Shipped");
	const FString Target = Root / TEXT("Project/.agents/skills");
	WriteSkill(Shipped, TEXT("ueagent-alpha"));
	WriteSkill(Shipped, TEXT("ueagent-beta"), TEXT("metadata:\n  author: someone\n"));
	WriteText(Shipped / TEXT("ueagent-alpha/template.md"), TEXT("template\n"));
	// The user's own skill, and a folder with one of our names that we did not install.
	WriteSkill(Target, TEXT("my-skill"));
	WriteSkill(Target, TEXT("ueagent-beta"));
	const FString ForeignText = ReadText(Target / TEXT("ueagent-beta/SKILL.md"));
	FString Message;

	// 1. Before: alpha missing, beta someone else's.
	{
		const McpSkills::FStatus Status = McpSkills::Inspect(Shipped, Target, TEXT("1.0.0"));
		TestEqual(TEXT("1 shipped"), Status.Skills.Num(), 2);
		TestEqual(TEXT("1 missing"), Status.Count(ESkillState::Missing), 1);
		TestEqual(TEXT("1 foreign"), Status.Count(ESkillState::Foreign), 1);
		TestFalse(TEXT("1 not installed"), Status.AnyInstalled());
	}

	// 2. Install: alpha copied whole and stamped; beta and the user's skill untouched.
	{
		TestTrue(TEXT("2 installs"), McpSkills::Install(Shipped, Target, TEXT("1.0.0"), Message));
		AddInfo(Message);
		TestEqual(TEXT("2 stamped"), ReadText(Target / TEXT("ueagent-alpha/SKILL.md")),
			FString(TEXT("---\nname: ueagent-alpha\ndescription: Test skill ueagent-alpha.\nmetadata:\n  ueagent-version: \"1.0.0\"\n---\n\n# ueagent-alpha\n\nBody.\n")));
		TestTrue(TEXT("2 other files copied"), FPaths::FileExists(Target / TEXT("ueagent-alpha/template.md")));
		TestEqual(TEXT("2 foreign untouched"), ReadText(Target / TEXT("ueagent-beta/SKILL.md")), ForeignText);
		TestTrue(TEXT("2 user's skill untouched"), FPaths::FileExists(Target / TEXT("my-skill/SKILL.md")));
		TestTrue(TEXT("2 says what it left alone"), Message.Contains(TEXT("ueagent-beta")));
		const McpSkills::FStatus Status = McpSkills::Inspect(Shipped, Target, TEXT("1.0.0"));
		TestEqual(TEXT("2 current"), Status.Count(ESkillState::Current), 1);
		TestFalse(TEXT("2 nothing to update"), Status.NeedsUpdate());
	}

	// 3. A stamp joins an existing metadata map.
	{
		IFileManager::Get().DeleteDirectory(*(Target / TEXT("ueagent-beta")), false, true);
		TestTrue(TEXT("3 installs"), McpSkills::Install(Shipped, Target, TEXT("1.0.0"), Message));
		TestEqual(TEXT("3 stamp in the map"), ReadText(Target / TEXT("ueagent-beta/SKILL.md")),
			FString(TEXT("---\nname: ueagent-beta\ndescription: Test skill ueagent-beta.\nmetadata:\n  ueagent-version: \"1.0.0\"\n  author: someone\n---\n\n# ueagent-beta\n\nBody.\n")));
	}

	// 4. A new version: outdated, then updated whole (a file the new version dropped goes away), and
	//    a skill no longer shipped is deleted.
	{
		IFileManager::Get().Delete(*(Shipped / TEXT("ueagent-alpha/template.md")));
		WriteSkill(Shipped, TEXT("ueagent-gamma"));
		IFileManager::Get().DeleteDirectory(*(Shipped / TEXT("ueagent-beta")), false, true);
		const McpSkills::FStatus Before = McpSkills::Inspect(Shipped, Target, TEXT("1.1.0"));
		TestEqual(TEXT("4 outdated"), Before.Count(ESkillState::Outdated), 1);
		TestEqual(TEXT("4 retired"), Before.Retired.Num(), 1);
		TestTrue(TEXT("4 needs update"), Before.NeedsUpdate());
		TestTrue(TEXT("4 updates"), McpSkills::Install(Shipped, Target, TEXT("1.1.0"), Message));
		AddInfo(Message);
		TestFalse(TEXT("4 dropped file gone"), FPaths::FileExists(Target / TEXT("ueagent-alpha/template.md")));
		TestFalse(TEXT("4 retired deleted"), IFileManager::Get().DirectoryExists(*(Target / TEXT("ueagent-beta"))));
		TestTrue(TEXT("4 new one installed"), FPaths::FileExists(Target / TEXT("ueagent-gamma/SKILL.md")));
		TestTrue(TEXT("4 restamped"), ReadText(Target / TEXT("ueagent-alpha/SKILL.md")).Contains(TEXT("ueagent-version: \"1.1.0\"")));
		TestTrue(TEXT("4 user's skill untouched"), FPaths::FileExists(Target / TEXT("my-skill/SKILL.md")));
	}

	// 5. Remove: only ours.
	{
		TestTrue(TEXT("5 removes"), McpSkills::Remove(Target, Message));
		AddInfo(Message);
		TestFalse(TEXT("5 alpha gone"), IFileManager::Get().DirectoryExists(*(Target / TEXT("ueagent-alpha"))));
		TestFalse(TEXT("5 gamma gone"), IFileManager::Get().DirectoryExists(*(Target / TEXT("ueagent-gamma"))));
		TestTrue(TEXT("5 user's skill kept"), FPaths::FileExists(Target / TEXT("my-skill/SKILL.md")));
		TestTrue(TEXT("5 second remove is a no-op"), McpSkills::Remove(Target, Message) && Message.Contains(TEXT("nothing was deleted")));
	}

	// 6. No version: nothing is installed.
	TestFalse(TEXT("6 no version"), McpSkills::Install(Shipped, Root / TEXT("Other"), FString(), Message));
	TestFalse(TEXT("6 nothing written"), IFileManager::Get().DirectoryExists(*(Root / TEXT("Other/ueagent-alpha"))));

	// 7. The plugin's own skills are there and stamp cleanly.
	TestTrue(TEXT("7 plugin ships skills"), McpSkills::ShippedNames(McpSkills::ShippedDir()).Num() > 0);
	TestFalse(TEXT("7 plugin version"), McpSkills::PluginVersion().IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
