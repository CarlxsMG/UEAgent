// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"

/**
 * Reads compiler and linker diagnostics out of build output, for get_cpp_compile_status. A pure function of the
 * text, so the automation test (Tests/McpCompilerDiagnosticsTest.cpp) checks it without a compiler.
 *
 * Formats read, with or without leading whitespace:
 *   MSVC and clang-cl   C:\Src\File.cpp(12): error C2065: 'x': undeclared identifier
 *                       C:\Src\File.cpp(12,5): warning C4996: ...
 *                       C:\Src\File.cpp(12,5): error: use of undeclared identifier 'x' [-Wfoo]
 *   clang and gcc       /src/File.cpp:12:5: error: use of undeclared identifier 'x'
 *                       /src/File.cpp:12:5: warning: unused variable 'y' [-Wunused-variable]
 *   linker              File.obj : error LNK2019: unresolved external symbol ...
 * MSVC translates the message but keeps `error`, `warning` and the code in English, which is what the parser keys on.
 * Notes (`note:`) are not diagnostics of their own and are skipped.
 */
namespace McpCompilerDiagnostics
{
    struct FDiagnostic
    {
        FString File;
        int32 Line = 0;
        int32 Column = 0;
        /** "error" or "warning" (a fatal error is an error). */
        FString Severity;
        /** C2065, LNK2019, -Wunused-variable, or empty when the compiler gave none. */
        FString Code;
        FString Message;

        bool operator==(const FDiagnostic& Other) const
        {
            return File == Other.File && Line == Other.Line && Column == Other.Column && Severity == Other.Severity
                && Code == Other.Code && Message == Other.Message;
        }
    };

    /** The diagnostic one line holds, if it holds one. */
    bool ParseLine(const FString& Line, FDiagnostic& Out);

    /** Every diagnostic of Text (split on line breaks), in order, each one once. */
    void Parse(const FString& Text, TArray<FDiagnostic>& OutErrors, TArray<FDiagnostic>& OutWarnings);
}
