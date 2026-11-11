// Copyright (c) 2026 CarlxsMG.

// Test of the parser get_cpp_compile_status reads compiler output with (Commands/Code/McpCompilerDiagnostics.h).
// Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.CompilerDiagnostics" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI
//
// The MSVC samples are lines as UnrealBuildTool logs them, the first one from a Spanish Visual Studio: the
// message is translated, "error" and the code are not.

#include "Commands/Code/McpCompilerDiagnostics.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpCompilerDiagnosticsTest, "UEAgent.CompilerDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpCompilerDiagnosticsTest::RunTest(const FString& Parameters)
{
	using McpCompilerDiagnostics::FDiagnostic;

	auto Expect = [this](const TCHAR* What, const FString& Line, const TCHAR* File, int32 LineNo, int32 Column,
		const TCHAR* Severity, const TCHAR* Code, const TCHAR* Message)
	{
		FDiagnostic D;
		if (!TestTrue(FString::Printf(TEXT("%s: parsed"), What), McpCompilerDiagnostics::ParseLine(Line, D)))
		{
			return;
		}
		TestEqual(FString::Printf(TEXT("%s: file"), What), D.File, FString(File));
		TestEqual(FString::Printf(TEXT("%s: line"), What), D.Line, LineNo);
		TestEqual(FString::Printf(TEXT("%s: column"), What), D.Column, Column);
		TestEqual(FString::Printf(TEXT("%s: severity"), What), D.Severity, FString(Severity));
		TestEqual(FString::Printf(TEXT("%s: code"), What), D.Code, FString(Code));
		TestEqual(FString::Printf(TEXT("%s: message"), What), D.Message, FString(Message));
	};

	// MSVC
	Expect(TEXT("msvc error with column"),
		TEXT("E:\\B64\\Spike57\\Source\\Spike\\SpikeThing.cpp(6,9): error C2065: 'UndeclaredThing': identificador no declarado"),
		TEXT("E:\\B64\\Spike57\\Source\\Spike\\SpikeThing.cpp"), 6, 9, TEXT("error"), TEXT("C2065"), TEXT("'UndeclaredThing': identificador no declarado"));
	Expect(TEXT("msvc error without column, indented"),
		TEXT("  C:\\Program Files (x86)\\Proj\\Src\\A.cpp(12): error C2143: syntax error: missing ';' before '}'"),
		TEXT("C:\\Program Files (x86)\\Proj\\Src\\A.cpp"), 12, 0, TEXT("error"), TEXT("C2143"), TEXT("syntax error: missing ';' before '}'"));
	Expect(TEXT("msvc warning"),
		TEXT("D:\\P\\Source\\P\\B.cpp(40,17): warning C4996: 'UObject::GetAssetRegistryTags': Implement the version that takes FAssetRegistryTagsContext instead."),
		TEXT("D:\\P\\Source\\P\\B.cpp"), 40, 17, TEXT("warning"), TEXT("C4996"), TEXT("'UObject::GetAssetRegistryTags': Implement the version that takes FAssetRegistryTagsContext instead."));
	Expect(TEXT("msvc fatal error"),
		TEXT("D:\\P\\Source\\P\\C.cpp(3): fatal error C1083: Cannot open include file: 'Missing.h': No such file or directory"),
		TEXT("D:\\P\\Source\\P\\C.cpp"), 3, 0, TEXT("error"), TEXT("C1083"), TEXT("Cannot open include file: 'Missing.h': No such file or directory"));
	Expect(TEXT("linker"),
		TEXT("Module.P.cpp.obj : error LNK2019: unresolved external symbol \"int __cdecl Missing(void)\" referenced in function \"void __cdecl F(void)\""),
		TEXT("Module.P.cpp.obj"), 0, 0, TEXT("error"), TEXT("LNK2019"), TEXT("unresolved external symbol \"int __cdecl Missing(void)\" referenced in function \"void __cdecl F(void)\""));

	// clang-cl (MSVC location, clang message)
	Expect(TEXT("clang-cl warning"),
		TEXT("D:\\P\\Source\\P\\D.cpp(8,8): warning: unused variable 'Unused' [-Wunused-variable]"),
		TEXT("D:\\P\\Source\\P\\D.cpp"), 8, 8, TEXT("warning"), TEXT("-Wunused-variable"), TEXT("unused variable 'Unused'"));

	// clang
	Expect(TEXT("clang error"),
		TEXT("/home/u/P/Source/P/E.cpp:6:9: error: use of undeclared identifier 'UndeclaredThing'"),
		TEXT("/home/u/P/Source/P/E.cpp"), 6, 9, TEXT("error"), TEXT(""), TEXT("use of undeclared identifier 'UndeclaredThing'"));
	Expect(TEXT("clang warning with flag"),
		TEXT("/home/u/P/Source/P/E.cpp:5:8: warning: unused variable 'Unused' [-Wunused-variable]"),
		TEXT("/home/u/P/Source/P/E.cpp"), 5, 8, TEXT("warning"), TEXT("-Wunused-variable"), TEXT("unused variable 'Unused'"));
	Expect(TEXT("clang error, Windows path"),
		TEXT("C:\\P\\Source\\P\\F.cpp:10:3: error: expected ';' after expression"),
		TEXT("C:\\P\\Source\\P\\F.cpp"), 10, 3, TEXT("error"), TEXT(""), TEXT("expected ';' after expression"));

	// Not diagnostics
	FDiagnostic Ignored;
	TestFalse(TEXT("a clang note"), McpCompilerDiagnostics::ParseLine(TEXT("/p/E.cpp:3:7: note: previous definition is here"), Ignored));
	TestFalse(TEXT("an editor log line"), McpCompilerDiagnostics::ParseLine(TEXT("LogLiveCoding: Error: Live coding failed, please see Live console for more information"), Ignored));
	TestFalse(TEXT("a build summary"), McpCompilerDiagnostics::ParseLine(TEXT("Result: Failed (OtherCompilationError)"), Ignored));
	TestFalse(TEXT("a time stamp"), McpCompilerDiagnostics::ParseLine(TEXT("Log started at 11/11/2026 13:07:11 (2026-11-11T12:07:11Z)"), Ignored));
	TestFalse(TEXT("an empty line"), McpCompilerDiagnostics::ParseLine(TEXT("   "), Ignored));
	// A line that ends in the character the parser looks for (an endless search once).
	TestFalse(TEXT("a line ending in a colon"), McpCompilerDiagnostics::ParseLine(TEXT("Compile error:"), Ignored));
	TestFalse(TEXT("a line ending in a parenthesis"), McpCompilerDiagnostics::ParseLine(TEXT("Running link.exe ("), Ignored));

	// A whole UBT log excerpt: one error and one warning, the error printed twice.
	const FString Log =
		TEXT("[1/2] Compile [x64] SpikeThing.cpp\r\n")
		TEXT("E:\\P\\Source\\P\\SpikeThing.cpp(6,9): error C2065: 'UndeclaredThing': undeclared identifier\r\n")
		TEXT("\treturn UndeclaredThing;\r\n")
		TEXT("\t       ^\r\n")
		TEXT("E:\\P\\Source\\P\\Other.cpp(2,1): warning C4005: 'X': macro redefinition\r\n")
		TEXT("E:\\P\\Source\\P\\SpikeThing.cpp(6,9): error C2065: 'UndeclaredThing': undeclared identifier\r\n")
		TEXT("Result: Failed (OtherCompilationError)\r\n");
	TArray<FDiagnostic> Errors, Warnings;
	McpCompilerDiagnostics::Parse(Log, Errors, Warnings);
	TestEqual(TEXT("log: one error, once"), Errors.Num(), 1);
	TestEqual(TEXT("log: one warning"), Warnings.Num(), 1);
	if (Errors.Num() == 1)
	{
		TestEqual(TEXT("log: error line"), Errors[0].Line, 6);
	}
	if (Warnings.Num() == 1)
	{
		TestEqual(TEXT("log: warning code"), Warnings[0].Code, FString(TEXT("C4005")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
