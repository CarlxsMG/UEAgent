// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Screenshots as MCP image content. A capture handler names its file with mcp_common::AttachImage; the bridge takes
 * that list out of the result at the outermost dispatch (TakeImages), reads and if needed shrinks each file there,
 * on the game thread, and writes what happened into the result as JSON (image_returned, image_width, image_height,
 * image_note). The server appends the blocks after the text block (MakeToolResult). The JSON every other stage reads
 * (offload, readback, Activity) carries no pixels.
 */
namespace McpImageContent
{
	/** At most this many images go back in one call; an execute_batch names the rest in a note. */
	constexpr int32 MaxImagesPerCall = 4;

	/** One MCP image content block. */
	struct FBlock
	{
		FString MimeType;
		/** The encoded file, base64. */
		FString Data;
	};

	struct FLimits
	{
		int32 MaxDimension = 1568;
		int64 MaxBytes = 1048576;
	};

	/** What PrepareBytes made of one file. */
	struct FPrepared
	{
		bool bOk = false;
		FString Error;
		FBlock Block;
		int32 Width = 0;
		int32 Height = 0;
		int32 SourceWidth = 0;
		int32 SourceHeight = 0;
		int64 SourceBytes = 0;
		int64 SentBytes = 0;
		/** Downscaled or re-encoded to fit the limits. */
		bool bShrunk = false;
	};

	/**
	 * An encoded image as the client gets it: the file's own bytes when it is a PNG or JPEG within both limits,
	 * otherwise a copy that fits MaxDimension, re-encoded as JPEG and made smaller until it fits MaxBytes.
	 * Game thread (it loads the ImageWrapper module on first use).
	 */
	FPrepared PrepareBytes(const TArray64<uint8>& Encoded, const FLimits& Limits);

	/** PrepareBytes on the file at FilePath, which is only read. */
	FPrepared PrepareFile(const FString& FilePath, const FLimits& Limits);

	/** The limits of Editor Preferences > Plugins > UEAgent > Images. */
	FLimits LimitsFromSettings();

	/**
	 * Takes the images AttachImage listed out of Result: from Result itself, its `data`, and for execute_batch each
	 * `results[].payload` (and its `data`), in that order. With bWanted, each image (up to MaxImagesPerCall) is read
	 * and prepared into OutBlocks and the object that listed it gets image_returned / image_width / image_height
	 * and, when shrunk or left out, image_note; without it the key is removed and the object says why
	 * (NotWantedReason). Returns how many images were listed.
	 */
	int32 TakeImages(const FString& CommandType, const TSharedPtr<FJsonObject>& Result, bool bWanted,
		const FString& NotWantedReason, const FLimits& Limits, TArray<FBlock>& OutBlocks);

	/** Removes the listed images from Result without reading them (a probe's dispatch, a nested call). */
	void DropImages(const FString& CommandType, const TSharedPtr<FJsonObject>& Result);

	/** CallToolResult: one text block, then one image block per entry of Images, and isError. */
	TSharedRef<FJsonObject> MakeToolResult(const FString& Text, bool bIsError, const TArray<FBlock>& Images);

	/** The blocks as the bridge's outcome carries them to the server ("images": [{data, mimeType}]), and back. */
	void WriteBlocks(FJsonObject& Outcome, const TArray<FBlock>& Blocks);
	TArray<FBlock> ReadBlocks(const FJsonObject& Outcome);
}
