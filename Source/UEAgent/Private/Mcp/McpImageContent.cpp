// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpImageContent.h"

#include "Commands/Common/McpCommonUtils.h"
#include "UEAgentSettings.h"

#include "Dom/JsonValue.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"

namespace McpImageContentPrivate
{
	/** A place in the result that listed an image: the object to report into, and the file. */
	struct FSite
	{
		TSharedPtr<FJsonObject> Holder;
		FString Path;
	};

	static void CollectFrom(const TSharedPtr<FJsonObject>& Object, TArray<FSite>& Out)
	{
		if (!Object.IsValid() || !Object->HasField(mcp_common::ImagesResultKey))
		{
			return;
		}
		TArray<TSharedPtr<FJsonValue>> Entries;
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (Object->TryGetArrayField(mcp_common::ImagesResultKey, List) && List)
		{
			Entries = *List;
		}
		Object->RemoveField(mcp_common::ImagesResultKey);
		for (const TSharedPtr<FJsonValue>& Entry : Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			FString Path;
			if (Entry.IsValid() && Entry->TryGetObject(EntryObject) && EntryObject && (*EntryObject)->TryGetStringField(TEXT("path"), Path))
			{
				Out.Add({ Object, Path });
			}
		}
	}

	static TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Child = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Child) && Child ? *Child : nullptr;
	}

	/** Every listed image of Result, in reply order, each removed from where it was listed. */
	static TArray<FSite> CollectAll(const FString& CommandType, const TSharedPtr<FJsonObject>& Result)
	{
		TArray<FSite> Sites;
		CollectFrom(Result, Sites);
		CollectFrom(ObjectField(Result, TEXT("data")), Sites);
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (CommandType == TEXT("execute_batch") && Result.IsValid() && Result->TryGetArrayField(TEXT("results"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* ItemObject = nullptr;
				if (Item.IsValid() && Item->TryGetObject(ItemObject) && ItemObject)
				{
					const TSharedPtr<FJsonObject> Payload = ObjectField(*ItemObject, TEXT("payload"));
					CollectFrom(Payload, Sites);
					CollectFrom(ObjectField(Payload, TEXT("data")), Sites);
				}
			}
		}
		return Sites;
	}

	static FString DescribeBytes(int64 Bytes)
	{
		return Bytes >= 1024 * 1024
			? FString::Printf(TEXT("%.1f MB"), Bytes / (1024.0 * 1024.0))
			: FString::Printf(TEXT("%lld KB"), (Bytes + 1023) / 1024);
	}

	static void NotReturned(const TSharedPtr<FJsonObject>& Holder, const FString& Note)
	{
		Holder->SetBoolField(TEXT("image_returned"), false);
		Holder->SetStringField(TEXT("image_note"), Note);
	}
}

McpImageContent::FPrepared McpImageContent::PrepareBytes(const TArray64<uint8>& Encoded, const FLimits& Limits)
{
	FPrepared Out;
	Out.SourceBytes = Encoded.Num();
	if (Encoded.Num() == 0)
	{
		Out.Error = TEXT("the file is empty");
		return Out;
	}
	IImageWrapperModule& ImageWrapper = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	const EImageFormat Format = ImageWrapper.DetectImageFormat(Encoded.GetData(), Encoded.Num());
	FImage Image;
	if (!ImageWrapper.DecompressImage(Encoded.GetData(), Encoded.Num(), Image) || Image.SizeX <= 0 || Image.SizeY <= 0)
	{
		Out.Error = TEXT("the file is not an image this editor can decode");
		return Out;
	}
	Out.SourceWidth = Image.SizeX;
	Out.SourceHeight = Image.SizeY;

	const int32 MaxDimension = FMath::Max(1, Limits.MaxDimension);
	const int32 LongSide = FMath::Max(Image.SizeX, Image.SizeY);
	const bool bClientFormat = Format == EImageFormat::PNG || Format == EImageFormat::JPEG;
	if (bClientFormat && LongSide <= MaxDimension && Encoded.Num() <= Limits.MaxBytes)
	{
		Out.bOk = true;
		Out.Block.MimeType = Format == EImageFormat::PNG ? TEXT("image/png") : TEXT("image/jpeg");
		Out.Block.Data = FBase64::Encode(Encoded.GetData(), static_cast<uint32>(Encoded.Num()));
		Out.Width = Image.SizeX;
		Out.Height = Image.SizeY;
		Out.SentBytes = Encoded.Num();
		return Out;
	}

	// Over a limit: a JPEG copy, smaller each round until it fits the byte limit.
	double Scale = FMath::Min(1.0, static_cast<double>(MaxDimension) / LongSide);
	int32 Quality = 85;
	constexpr int32 MaxRounds = 8;
	for (int32 Round = 0; Round < MaxRounds; ++Round)
	{
		const int32 Width = FMath::Max(1, FMath::RoundToInt(Image.SizeX * Scale));
		const int32 Height = FMath::Max(1, FMath::RoundToInt(Image.SizeY * Scale));
		FImage Small;
		if (Width == Image.SizeX && Height == Image.SizeY)
		{
			Image.CopyTo(Small, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		}
		else
		{
			FImageCore::ResizeImageAllocDest(Image, Small, Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		}
		TArray64<uint8> Jpeg;
		if (!ImageWrapper.CompressImage(Jpeg, EImageFormat::JPEG, Small, Quality) || Jpeg.Num() == 0)
		{
			Out.Error = TEXT("the downscaled copy could not be encoded as JPEG");
			return Out;
		}
		if (Jpeg.Num() <= Limits.MaxBytes)
		{
			Out.bOk = true;
			Out.bShrunk = true;
			Out.Block.MimeType = TEXT("image/jpeg");
			Out.Block.Data = FBase64::Encode(Jpeg.GetData(), static_cast<uint32>(Jpeg.Num()));
			Out.Width = Width;
			Out.Height = Height;
			Out.SentBytes = Jpeg.Num();
			return Out;
		}
		Scale *= 0.75;
		Quality = 75;
	}
	Out.Error = FString::Printf(TEXT("no downscaled copy fit in %lld bytes"), Limits.MaxBytes);
	return Out;
}

McpImageContent::FPrepared McpImageContent::PrepareFile(const FString& FilePath, const FLimits& Limits)
{
	TArray64<uint8> Encoded;
	if (!FFileHelper::LoadFileToArray(Encoded, *FilePath))
	{
		FPrepared Out;
		Out.Error = FString::Printf(TEXT("could not read %s"), *FilePath);
		return Out;
	}
	return PrepareBytes(Encoded, Limits);
}

McpImageContent::FLimits McpImageContent::LimitsFromSettings()
{
	const UUEAgentSettings* Settings = GetDefault<UUEAgentSettings>();
	FLimits Limits;
	Limits.MaxDimension = FMath::Max(64, Settings->ImageMaxDimension);
	Limits.MaxBytes = FMath::Max(65536, Settings->ImageMaxBytes);
	return Limits;
}

int32 McpImageContent::TakeImages(const FString& CommandType, const TSharedPtr<FJsonObject>& Result, bool bWanted,
	const FString& NotWantedReason, const FLimits& Limits, TArray<FBlock>& OutBlocks)
{
	using namespace McpImageContentPrivate;
	const TArray<FSite> Sites = CollectAll(CommandType, Result);
	TArray<FString> LeftOut;
	for (const FSite& Site : Sites)
	{
		if (!bWanted)
		{
			NotReturned(Site.Holder, NotWantedReason);
			continue;
		}
		if (OutBlocks.Num() >= MaxImagesPerCall)
		{
			NotReturned(Site.Holder, FString::Printf(TEXT("not returned: a call returns at most %d images; the picture is at %s"),
				MaxImagesPerCall, *Site.Path));
			LeftOut.Add(Site.Path);
			continue;
		}
		const FPrepared Prepared = PrepareFile(Site.Path, Limits);
		if (!Prepared.bOk)
		{
			NotReturned(Site.Holder, FString::Printf(TEXT("not returned: %s"), *Prepared.Error));
			continue;
		}
		OutBlocks.Add(Prepared.Block);
		Site.Holder->SetBoolField(TEXT("image_returned"), true);
		Site.Holder->SetNumberField(TEXT("image_width"), Prepared.Width);
		Site.Holder->SetNumberField(TEXT("image_height"), Prepared.Height);
		if (Prepared.bShrunk)
		{
			Site.Holder->SetStringField(TEXT("image_note"), FString::Printf(
				TEXT("the image sent is a smaller copy: %dx%d, %s on disk -> %dx%d JPEG, %s (limits %d px and %lld bytes, ")
				TEXT("Editor Preferences > Plugins > UEAgent > Images); the file keeps its full resolution"),
				Prepared.SourceWidth, Prepared.SourceHeight, *DescribeBytes(Prepared.SourceBytes),
				Prepared.Width, Prepared.Height, *DescribeBytes(Prepared.SentBytes), Limits.MaxDimension, Limits.MaxBytes));
		}
	}
	if (LeftOut.Num() > 0 && Result.IsValid())
	{
		Result->SetStringField(TEXT("image_note"), FString::Printf(TEXT("%d image(s) beyond the first %d were not returned: %s"),
			LeftOut.Num(), MaxImagesPerCall, *FString::Join(LeftOut, TEXT(", "))));
	}
	return Sites.Num();
}

void McpImageContent::DropImages(const FString& CommandType, const TSharedPtr<FJsonObject>& Result)
{
	McpImageContentPrivate::CollectAll(CommandType, Result);
}

TSharedRef<FJsonObject> McpImageContent::MakeToolResult(const FString& Text, bool bIsError, const TArray<FBlock>& Images)
{
	TArray<TSharedPtr<FJsonValue>> Content;
	TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), Text);
	Content.Add(MakeShared<FJsonValueObject>(TextBlock));
	for (const FBlock& Image : Images)
	{
		TSharedRef<FJsonObject> ImageBlock = MakeShared<FJsonObject>();
		ImageBlock->SetStringField(TEXT("type"), TEXT("image"));
		ImageBlock->SetStringField(TEXT("data"), Image.Data);
		ImageBlock->SetStringField(TEXT("mimeType"), Image.MimeType);
		Content.Add(MakeShared<FJsonValueObject>(ImageBlock));
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("content"), Content);
	Result->SetBoolField(TEXT("isError"), bIsError);
	return Result;
}

void McpImageContent::WriteBlocks(FJsonObject& Outcome, const TArray<FBlock>& Blocks)
{
	if (Blocks.Num() == 0)
	{
		return;
	}
	TArray<TSharedPtr<FJsonValue>> List;
	for (const FBlock& Block : Blocks)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("data"), Block.Data);
		Entry->SetStringField(TEXT("mimeType"), Block.MimeType);
		List.Add(MakeShared<FJsonValueObject>(Entry));
	}
	Outcome.SetArrayField(TEXT("images"), List);
}

TArray<McpImageContent::FBlock> McpImageContent::ReadBlocks(const FJsonObject& Outcome)
{
	TArray<FBlock> Blocks;
	const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
	if (!Outcome.TryGetArrayField(TEXT("images"), List) || !List)
	{
		return Blocks;
	}
	for (const TSharedPtr<FJsonValue>& Entry : *List)
	{
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;
		FBlock Block;
		if (Entry.IsValid() && Entry->TryGetObject(EntryObject) && EntryObject
			&& (*EntryObject)->TryGetStringField(TEXT("data"), Block.Data) && (*EntryObject)->TryGetStringField(TEXT("mimeType"), Block.MimeType))
		{
			Blocks.Add(MoveTemp(Block));
		}
	}
	return Blocks;
}
