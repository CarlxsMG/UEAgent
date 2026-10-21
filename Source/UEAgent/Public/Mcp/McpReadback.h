// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Mcp/McpPy.h"

/**
 * READBACK contract: a mutator must NOT report success if the state did not move -- and a reader
 * must not present as "there is none" a zero that proves nothing.
 *
 * It runs in the SAME game-thread step as the command, so the re-reads see exactly the state the
 * command left, with no race against a second round trip.
 *
 * Why it exists: the dominant defect of a large tool catalog is not the crash, it is the false
 * success. Many mutating handlers report `success:true` unconditionally and many commands only echo
 * their input, so a silent write gets blamed on the data and the real cause stays buried.
 *
 * The families, in dispatch order:
 *   ON_FAILURE        the command failed: notes WHY the stated reason is not the real one
 *   BEFORE/_CHECK     a snapshot taken before the command and its check, which WINS over any other finding
 *   IN_RESPONSE       the evidence is already in the response: zero cost
 *   FOLLOW_UP_CUSTOM  one or two tailored re-reads
 *   FOLLOW_UP         the generic re-read (set_properties -> get_properties)
 * Each family runs only if the one before found nothing. Then, for EVERY command, the notice for a
 * response whose heavy field was offloaded to disk (AnnotateOffload).
 *
 * It works on the result the model sees: the bridge envelope already unwrapped (success -> the
 * payload; error -> {"success": false, "error": ..., "result"?: ...}).
 */
namespace McpReadback
{
	using FV = McpPy::FV;
	using FO = McpPy::FO;

	/** A re-read: the unwrapped result of another command (or null if there was no response). */
	using FSend = TFunction<FO(const FString& Command, const FO& Params)>;

	using FInResponse = TFunction<FO(const FO& Params, const FO& Result)>;
	using FFollowUpCustom = TFunction<FO(const FO& Params, const FSend& Send)>;
	using FOnFailure = TFunction<FO(const FO& Params, const FO& Result, const FSend& Send)>;
	using FBefore = TFunction<FV(const FO& Params, const FSend& Send)>;
	using FBeforeCheck = TFunction<FO(const FString& Name, const FO& Params, const FV& Before, const FSend& Send)>;

	struct FFollowUp
	{
		FString Reader;
		/** destination (reader param) <- source (command param) */
		TArray<TPair<FString, FString>> ParamMap;
		/** The field of the reader's reply that holds the stored value. */
		FString ValueKey;
		/** Optional: ValueKey is a map keyed by the value of this command param (get_properties answers
		 *  values.<property_name>), and the stored value is that entry. */
		FString ValueKeyParam;
		FString RequestKey;
	};

	struct FTables
	{
		TMap<FString, FInResponse> InResponse;
		TMap<FString, FFollowUpCustom> FollowUpCustom;
		TMap<FString, FFollowUp> FollowUp;
		TMap<FString, FOnFailure> OnFailure;
		TMap<FString, FBefore> Before;
		TMap<FString, FBeforeCheck> BeforeCheck;
	};

	/** The tables, filled once by the check families (Mcp/Readback/*.cpp). */
	UEAGENT_API const FTables& Tables();

	/** Does the command have any readback check? */
	UEAGENT_API bool HasContract(const FString& Name);

	/** The pre-command snapshot, or null. Never fails. */
	UEAGENT_API FV TakeBefore(const FString& Name, const FO& Params, const FSend& Send);

	/** Adds `readback` to the result and degrades a silent success. Returns the same object. */
	UEAGENT_API FO Verify(const FString& Name, const FO& Params, const FO& Result, const FSend& Send, const FV& Before);

	/** Flags a response whose heavy field the bridge offloaded to disk. */
	UEAGENT_API FO AnnotateOffload(const FString& Name, const FO& Result);

	/** Records what later readback checks need (compiled Blueprints, censuses taken in PIE). */
	UEAGENT_API void NoteResult(const FString& Name, const FO& Params, const FO& Result);

	/**
	 * The bridge envelope unwrapped into what the model sees.
	 * bOutRefused: the envelope was a refusal (`result.refused`) and the command never ran.
	 */
	UEAGENT_API FO Unwrap(const FO& Envelope, bool& bOutRefused);

	// --- session state ---------------------------------------------------------------
	struct FSessionState
	{
		/** Short names of the Blueprints compiled in this session. */
		TArray<FString> CompiledBlueprints;
		/** World packages of actor censuses taken in a PIE world. */
		TArray<FString> PieCensus;
		/** Distinct terrain height probes (vertex|landscape). */
		TArray<FString> ProbedTerrain;
		/** Levels PIE was started on. */
		TArray<FString> PieLaunched;
		/** Actors whose construction scripts were rerun. */
		TArray<FString> UcsRerun;
		/** Blueprint -> parent Blueprint asset path, or None (case-sensitive keys). */
		TArray<TPair<FString, FV>> ParentCache;
	};
	UEAGENT_API FSessionState& State();
	UEAGENT_API FO StateToJson();
	UEAGENT_API void StateFromJson(const FO& Json);

	// --- helpers for the checks ------------------------------------------------------------
	/** A re-read that fails OPEN: an empty object means "could not tell". */
	UEAGENT_API FO ProbeOrEmpty(const FSend& Send, const FString& Command, const FO& Params);
	/** `{"checked": true}` followed by the finding's fields. */
	UEAGENT_API FO Checked(const FO& Finding);
	/** Adds a value to a set (a TArray without duplicates, case-sensitive); false if it was already there. */
	UEAGENT_API bool SetAdd(TArray<FString>& Set, const FString& Value);

	/**
	 * Self-test: replays recorded contract vectors (readback, take_before, annotate_offload,
	 * note_result) with FAKE probes and compares the output. Fills OutFailures.
	 * OutSkipped: records that cannot be replayed here, with the reason (not counted as checks).
	 */
	UEAGENT_API void RunRecords(const TArray<FV>& Records, int32& OutChecks, int32& OutPassed, TArray<FV>& OutFailures, TArray<FV>& OutSkipped);

	/** The commands of each table, sorted (reported by the self-test). */
	UEAGENT_API FO TablesToJson();

	// Registration of the core family (the others register from their own .cpp files).
	void RegisterCore(FTables& Tables);
}
