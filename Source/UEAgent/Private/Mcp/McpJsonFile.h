// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * A JSON config file that belongs to someone else: the team's `.mcp.json`, a client's
 * `.cursor/mcp.json`, `.vscode/mcp.json` or `.agents/mcp_config.json`. It is read keeping each number
 * as its original text, and rewritten by hand with the indentation and line endings it already
 * had, so the diff is only the entry that changed.
 */
namespace McpJsonFile
{
	struct FLayout
	{
		FString Indent = TEXT("  "); // the one `claude mcp add --scope project` writes
		FString Eol = TEXT("\n");
		bool bTrailingEol = true;
	};

	/** False with OutError "could not be read" or "is not valid JSON". */
	bool Load(const FString& Path, FString& OutText, TSharedPtr<FJsonObject>& OutRoot, FString& OutError);
	FLayout DetectLayout(const FString& Text);
	FString Serialize(const TSharedPtr<FJsonObject>& Root, const FLayout& Layout);
	/** Through a temporary file next to Path, so a failure halfway through cannot leave it half-written. */
	bool WriteAtomically(const FString& Path, const FString& Text, FString& OutError);

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
	/** An "Authorization" header (any case) with a non-empty value. */
	bool HasAuthorizationHeader(const TSharedPtr<FJsonObject>& Entry);
	/** The same server: compared without the query (?tools=compact picks the list, not the server), the case or trailing slashes. */
	bool SameUrl(const FString& A, const FString& B);
}
