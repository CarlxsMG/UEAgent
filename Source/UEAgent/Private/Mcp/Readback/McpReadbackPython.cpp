// Copyright (c) 2026 CarlxsMG.

// IN_RESPONSE for execute_python: what the script CALLS (read without running it) and what its
// output reveals. The code is read with McpPythonScan, the same reader the refusals use.

#include "McpReadbackCheckUtil.h"
#include "McpReadbackPythonTables.h"
#include "../McpReadbackChecks.h"
#include "Mcp/McpPythonScan.h"

// Unique namespace per file: the module is built as a unity build (see Build.cs).
namespace McpReadbackPython
{
	using namespace McpPy;
	using namespace McpReadback;
	using namespace McpReadbackText;
	using namespace McpReadbackUtil;
	using McpReadbackPythonTables::FSymbolWarning;

	/** The text of an engine log line: its `message` if the line is a dict, otherwise the line. */
	static FV LineText(const FV& Line)
	{
		return IsDict(Line) ? Get(Line, TEXT("message")) : Line;
	}

	/** The script output and the engine log, lowercased. */
	static FString RunOutputText(const FO& Result)
	{
		const FO Data = Payload(Result);
		TArray<FString> Chunks;
		for (const TCHAR* Key : { TEXT("output"), TEXT("log_output") })
		{
			const FV Value = Get(Data, Key);
			if (IsStr(Value))
			{
				Chunks.Add(AsStr(Value));
			}
		}
		for (const FV& Line : AsList(Get(Data, TEXT("engine_log"))))
		{
			const FV Text = LineText(Line);
			if (IsStr(Text))
			{
				Chunks.Add(AsStr(Text));
			}
		}
		return Lower(Join(Chunks, RB_run_output_text_0));
	}

	struct FPythonFinding
	{
		FString What;
		FString Warning;
	};

	/** Python APIs and arguments known to return misleading results, and telltale output markers. */
	static FO CheckPythonCode(const FO& Params, const FO& Result)
	{
		const FV Code = Get(Params, TEXT("command"));
		if (!IsStr(Code) || Strip(AsStr(Code)).IsEmpty())
		{
			return nullptr;
		}
		const TArray<FString> CalledFunctions = McpPythonScan::CalledNames(AsStr(Code));
		if (CalledFunctions.Num() == 0)
		{
			return nullptr;
		}
		const TArray<FString> StringArgs = McpPythonScan::CallStringArguments(AsStr(Code));

		bool bPieActive = false;
		for (const FV& Line : AsList(Get(Payload(Result), TEXT("engine_log"))))
		{
			const FV Text = LineText(Line);
			if (IsStr(Text) && Contains(Lower(AsStr(Text)), TEXT("currently in a play mode"))) // PIE marker in the log
			{
				bPieActive = true;
				break;
			}
		}

		TArray<FPythonFinding> Findings;
		if (bPieActive)
		{
			for (const FSymbolWarning& Row : McpReadbackPythonTables::LyingApisDuringPie)
			{
				if (McpPythonScan::ContainsExact(CalledFunctions, Row.Symbol))
				{
					Findings.Add({ Row.Symbol, Row.Warning });
				}
			}
		}
		for (const FSymbolWarning& Row : McpReadbackPythonTables::LyingArguments)
		{
			if (McpPythonScan::ContainsExact(StringArgs, Row.Symbol))
			{
				Findings.Add({ Percent(TEXT("'%s' (argument)"), { S(Row.Symbol) }), Row.Warning });
			}
		}
		for (const FSymbolWarning& Row : McpReadbackPythonTables::LyingApis)
		{
			if (McpPythonScan::ContainsExact(CalledFunctions, Row.Symbol))
			{
				Findings.Add({ Row.Symbol, Row.Warning });
			}
		}
		// Asking a CDO for its components returns only the native ones: SCS components do not live there.
		if (McpPythonScan::ContainsExact(CalledFunctions, TEXT("get_default_object")) && McpPythonScan::ContainsExact(CalledFunctions, TEXT("get_components_by_class")))
		{
			Findings.Add({ TEXT("get_default_object + get_components_by_class"), RB_check_python_code_0 });
		}

		// Markers revealed by the OUTPUT, in this order.
		struct FOutputMark { const TCHAR* Pattern; const TCHAR* What; const TCHAR* Warning; };
		const FOutputMark Marks[] = {
			{ TEXT("navmesh bounds are too large"), TEXT("'Navmesh bounds are too large' (in the output)"), RB_check_python_code_1 },
			{ TEXT("<array object at"), TEXT("'<Array object at ...>' (in the output)"), RB_check_python_code_2 },
			{ TEXT("cannot pythonize"), TEXT("'Cannot pythonize' (in the output)"), RB_check_python_code_3 },
			{ TEXT("(byteproperty)"), TEXT("'(ByteProperty)' (in the output)"), RB_check_python_code_4 },
			{ TEXT("<unbound>"), TEXT("'<Unbound>' (in the output)"), RB_check_python_code_5 },
			{ TEXT("widgetblueprintlibrary"), TEXT("'WidgetBlueprintLibrary' (in the output)"), RB_check_python_code_6 },
			{ TEXT("movementmode."), TEXT("'MovementMode.' (in the output)"), RB_check_python_code_7 },
			{ TEXT("<struct '"), TEXT("'<Struct ...>' (in the output)"), RB_check_python_code_8 },
			{ TEXT("deprecationwarning"), TEXT("'DeprecationWarning' (in the output)"), RB_check_python_code_9 },
			{ TEXT("failed to find property"), TEXT("'Failed to find property' (in the output)"), RB_check_python_code_10 },
		};
		const FString RunText = RunOutputText(Result);
		for (const FOutputMark& Mark : Marks)
		{
			if (Contains(RunText, Mark.Pattern))
			{
				Findings.Add({ Mark.What, Mark.Warning });
			}
		}

		if (Findings.Num() == 0)
		{
			return nullptr;
		}
		TArray<FString> Whats;
		TArray<FString> Lines;
		for (const FPythonFinding& Finding : Findings)
		{
			Whats.Add(Finding.What);
			Lines.Add(Percent(TEXT("%s: %s"), { S(Finding.What), S(Finding.Warning) }));
		}
		return WarningFinding(TEXT("python_api_lies"), L(Whats), None(), TEXT("ast:execute_python"),
			Percent(RB_check_python_code_11, { N(Findings.Num()) }) + Join(Lines, TEXT(" | ")) + RB_check_python_code_13);
	}
}

void McpReadbackChecks::RegisterPython(McpReadback::FTables& Tables)
{
	Tables.InResponse.Add(TEXT("execute_python"), &McpReadbackPython::CheckPythonCode);
}
