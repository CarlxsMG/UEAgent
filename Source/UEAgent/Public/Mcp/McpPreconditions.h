// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class FJsonObject;

/**
 * PRECONDITION guard: refuse cleanly instead of crashing (or freezing) the editor.
 *
 * Runs in DispatchSingleCommand, after aliases and rotations are translated and BEFORE the
 * handler, so it covers every tools/call, every in-process call and every sub-command of `execute_batch`.
 *
 * Three families:
 *   (a) by ARGUMENTS -- zero cost;
 *   (b) by a READ -- a read-only probe in the SAME game-thread step (no network round trip, so
 *       no race between the probe and the call);
 *   (c) by the CODE about to run (`execute_python`), read with McpPythonScan.
 *
 * THE ASYMMETRY: when what is prevented is a CRASH, "could not verify" inside the crashing family
 * means REFUSE. Outside it, checks fail open. Each check states which of the two it does.
 *
 * The session state (World Partition maps created, save_level names used) lives here, in the
 * editor process, so it resets exactly when the editor restarts.
 */
namespace McpPreconditions
{
	/**
	 * Read-only probe: runs another command and returns its unwrapped payload (`data` if present,
	 * otherwise the top level), or null if it failed or said success:false.
	 */
	using FProbe = TFunction<TSharedPtr<FJsonObject>(const FString& Command, const TSharedPtr<FJsonObject>& Params)>;

	struct FRefusal
	{
		/** The reason, including the way that DOES work. */
		FString Message;
		/** Which check refused (for the envelope and the log). */
		FString Check;
	};

	/** The refusal, or unset if the call may proceed. Game thread. Params already canonicalized. */
	UEAGENT_API TOptional<FRefusal> Check(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const FProbe& Probe);

	/**
	 * Records what changes the risk of LATER calls (currently: a World Partition map that stays
	 * resident). Receives the {"status", "result"} envelope the dispatch returned. Game thread.
	 */
	UEAGENT_API void NoteResult(const FString& CommandType, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Envelope);

	/** Forgets the session state (for the self-test). */
	UEAGENT_API void ResetSessionState();

	/** The refusal envelope, shaped like the one of the PIE guards
	 *  ({"status":"error","error":"REFUSED: ...","result":{"refused":true,...}}). */
	UEAGENT_API TSharedPtr<FJsonObject> MakeRefusalEnvelope(const FString& CommandType, const FRefusal& Refusal);

	/** The payload of a dispatch envelope (`data` if present, otherwise `result`), or null if the
	 *  command failed or said success:false. */
	UEAGENT_API TSharedPtr<FJsonObject> PayloadFromEnvelope(const TSharedPtr<FJsonObject>& Envelope);

	/**
	 * The identity part of the `get_asset_by_name` response (name/path/class/package/is_loaded, `class`
	 * being the class path) taken from the Asset Registry, WITHOUT loading the asset: classifying a MAP
	 * by loading it loads the whole world (minutes and many GB on a large map), and the check only needs
	 * the class. Only for a package name or object path asked with load_full_data=false and no tags.
	 * Null otherwise, or if the registry does not know the asset: the probe then falls back to the real handler.
	 */
	UEAGENT_API TSharedPtr<FJsonObject> AssetMetadataFromRegistry(const TSharedPtr<FJsonObject>& ProbeParams);

	/**
	 * Self-test against the contract test vectors. Reads a JSON file of cases, runs them with FAKE
	 * probes (nothing reaches any handler) and writes the report next to the input file
	 * (<input>.result.json). Returns "N/M" and fills OutReportPath.
	 * Console: `mcp.contract.selftest <path>`.
	 */
	UEAGENT_API FString RunSelfTest(const FString& VectorsPath, FString& OutReportPath);
}
