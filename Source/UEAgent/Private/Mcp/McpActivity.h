#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * What the agents did in this editor session: one entry per tool call, with its duration and its
 * outcome as the model saw it. The Activity panel (Mcp/McpActivityPanel.h) shows it, and the console
 * command `mcp.activity [N]` prints it to the log. Game thread only.
 */
namespace McpActivity
{
	enum class EOutcome : uint8
	{
		Ok,       // ran, and the readback found nothing wrong
		Warning,  // ran, but the readback says it did not do what it reported
		Failed,   // ran and answered success:false
		Refused,  // never ran: a precondition or a PIE guard refused it
	};

	struct FEntry
	{
		FDateTime Started;
		FString Tool;
		double Milliseconds = 0.0;
		EOutcome Outcome = EOutcome::Ok;
		/** The error, the refusal or the readback note, on one line. */
		FString Detail;
	};

	/** How many calls are kept; older ones drop out. */
	constexpr int32 MaxEntries = 200;

	/** Records a finished call from the result the model sees. */
	void Record(const FString& Tool, const FDateTime& Started, double Milliseconds, const TSharedPtr<FJsonObject>& Result, bool bRefused);

	/** Oldest first. */
	const TArray<TSharedPtr<const FEntry>>& GetEntries();

	void Clear();

	/** Fires after Record and Clear. */
	FSimpleMulticastDelegate& OnChanged();

	const TCHAR* OutcomeName(EOutcome Outcome);
}
