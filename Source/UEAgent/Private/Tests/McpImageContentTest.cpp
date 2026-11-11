// Copyright (c) 2026 CarlxsMG.

// Test of the capture tools' image content (Mcp/McpImageContent.h): the CallToolResult the server sends, how the
// images are taken out of a result, and the copy sent for a picture over the limits. Headless:
//   UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests UEAgent.ImageContent" -testexit="Automation Test Queue Empty" -unattended -nopause -NullRHI

#include "Mcp/McpImageContent.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Math/RandomStream.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace McpImageContentTestPrivate
{
	/** A W x H image of noise, so that its PNG is large. */
	static TArray64<uint8> NoisePng(int32 Width, int32 Height)
	{
		FImage Image(Width, Height, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		FRandomStream Random(1234);
		for (uint8& Byte : Image.RawData)
		{
			Byte = static_cast<uint8>(Random.RandRange(0, 255));
		}
		FImageCore::SetAlphaOpaque(Image);
		TArray64<uint8> Png;
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CompressImage(Png, EImageFormat::PNG, Image);
		return Png;
	}

	static bool StartsWith(const TArray<uint8>& Bytes, const TArray<uint8>& Magic)
	{
		int32 Index = 0;
		for (uint8 Expected : Magic)
		{
			if (!Bytes.IsValidIndex(Index) || Bytes[Index] != Expected)
			{
				return false;
			}
			++Index;
		}
		return true;
	}

	static TArray<uint8> Decode(const FString& Base64)
	{
		TArray<uint8> Bytes;
		FBase64::Decode(Base64, Bytes);
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMcpImageContentTest, "UEAgent.ImageContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMcpImageContentTest::RunTest(const FString& Parameters)
{
	using namespace McpImageContentTestPrivate;

	// --- The result builder: text first, then each image in order, isError as given.
	{
		TArray<McpImageContent::FBlock> Images = { { TEXT("image/png"), TEXT("AAAA") }, { TEXT("image/jpeg"), TEXT("BBBB") } };
		for (const bool bIsError : { false, true })
		{
			const TSharedRef<FJsonObject> Result = McpImageContent::MakeToolResult(TEXT("{\"success\":true}"), bIsError, Images);
			const TArray<TSharedPtr<FJsonValue>>& Content = Result->GetArrayField(TEXT("content"));
			TestEqual(TEXT("one text block and two image blocks"), Content.Num(), 3);
			TestEqual(TEXT("isError unchanged"), Result->GetBoolField(TEXT("isError")), bIsError);
			if (Content.Num() == 3)
			{
				TestEqual(TEXT("text first"), Content[0]->AsObject()->GetStringField(TEXT("type")), FString(TEXT("text")));
				TestEqual(TEXT("text kept"), Content[0]->AsObject()->GetStringField(TEXT("text")), FString(TEXT("{\"success\":true}")));
				TestEqual(TEXT("image second"), Content[1]->AsObject()->GetStringField(TEXT("type")), FString(TEXT("image")));
				TestEqual(TEXT("mimeType of the first"), Content[1]->AsObject()->GetStringField(TEXT("mimeType")), FString(TEXT("image/png")));
				TestEqual(TEXT("data of the first"), Content[1]->AsObject()->GetStringField(TEXT("data")), FString(TEXT("AAAA")));
				TestEqual(TEXT("mimeType of the second"), Content[2]->AsObject()->GetStringField(TEXT("mimeType")), FString(TEXT("image/jpeg")));
			}
		}
		const TSharedRef<FJsonObject> TextOnly = McpImageContent::MakeToolResult(TEXT("x"), false, {});
		TestEqual(TEXT("no image: the text block alone"), TextOnly->GetArrayField(TEXT("content")).Num(), 1);

		FJsonObject Outcome;
		McpImageContent::WriteBlocks(Outcome, Images);
		const TArray<McpImageContent::FBlock> Back = McpImageContent::ReadBlocks(Outcome);
		TestTrue(TEXT("the outcome carries the blocks in order"), Back.Num() == 2 && Back[1].MimeType == TEXT("image/jpeg") && Back[1].Data == TEXT("BBBB"));
	}

	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("McpImageContentTest"));
	IFileManager::Get().MakeDirectory(*Dir, true);

	// --- A picture within the limits goes as it is.
	const TArray64<uint8> SmallPng = NoisePng(64, 32);
	const FString SmallPath = Dir / TEXT("small.png");
	FFileHelper::SaveArrayToFile(SmallPng, *SmallPath);
	{
		const McpImageContent::FPrepared Prepared = McpImageContent::PrepareFile(SmallPath, McpImageContent::FLimits());
		TestTrue(TEXT("small: prepared"), Prepared.bOk);
		TestFalse(TEXT("small: not shrunk"), Prepared.bShrunk);
		TestEqual(TEXT("small: png"), Prepared.Block.MimeType, FString(TEXT("image/png")));
		TestTrue(TEXT("small: the file's own bytes"), Decode(Prepared.Block.Data).Num() == SmallPng.Num());
		TestTrue(TEXT("small: 64x32"), Prepared.Width == 64 && Prepared.Height == 32);
	}

	// --- Over the limits: a smaller JPEG copy, and the file on disk untouched.
	const TArray64<uint8> BigPng = NoisePng(2400, 1200);
	const FString BigPath = Dir / TEXT("big.png");
	FFileHelper::SaveArrayToFile(BigPng, *BigPath);
	const FMD5Hash HashBefore = FMD5Hash::HashFile(*BigPath);
	{
		McpImageContent::FLimits Limits;
		Limits.MaxDimension = 1568;
		Limits.MaxBytes = 300 * 1024;
		const McpImageContent::FPrepared Prepared = McpImageContent::PrepareFile(BigPath, Limits);
		TestTrue(TEXT("big: prepared"), Prepared.bOk);
		TestTrue(TEXT("big: shrunk"), Prepared.bShrunk);
		TestEqual(TEXT("big: jpeg"), Prepared.Block.MimeType, FString(TEXT("image/jpeg")));
		TestTrue(TEXT("big: longest side within the limit"), FMath::Max(Prepared.Width, Prepared.Height) <= Limits.MaxDimension);
		TestTrue(TEXT("big: aspect kept"), FMath::Abs(Prepared.Width - 2 * Prepared.Height) <= 2);
		const TArray<uint8> Bytes = Decode(Prepared.Block.Data);
		TestTrue(TEXT("big: within the byte limit"), Bytes.Num() <= Limits.MaxBytes);
		TestTrue(TEXT("big: JPEG magic"), StartsWith(Bytes, TArray<uint8>{ 0xFF, 0xD8, 0xFF }));
	}
	TestTrue(TEXT("big: the file on disk is unchanged"), FMD5Hash::HashFile(*BigPath) == HashBefore);

	// --- Taking the images out of a result: the key goes, the fields come, the cap holds.
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("file_path"), BigPath);
		mcp_common::AttachImage(Data, MakeShared<FJsonObject>(), BigPath);
		TSharedPtr<FJsonObject> Reply = mcp_common::MakeOkReply(Data);
		TArray<McpImageContent::FBlock> Blocks;
		McpImageContent::FLimits Limits;
		Limits.MaxBytes = 300 * 1024;
		McpImageContent::TakeImages(TEXT("capture_high_res_screenshot"), Reply, true, FString(), Limits, Blocks);
		TestEqual(TEXT("one block"), Blocks.Num(), 1);
		TestFalse(TEXT("the reserved key is gone"), Data->HasField(mcp_common::ImagesResultKey));
		TestTrue(TEXT("image_returned"), Data->GetBoolField(TEXT("image_returned")));
		TestTrue(TEXT("image_note on a shrunk image"), Data->HasField(TEXT("image_note")));
		TestTrue(TEXT("image_width"), Data->GetNumberField(TEXT("image_width")) <= 1568);
	}
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetBoolField(TEXT("return_image"), false);
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		mcp_common::AttachImage(Result, Params, SmallPath);
		TArray<McpImageContent::FBlock> Blocks;
		McpImageContent::TakeImages(TEXT("capture_viewport"), Result, true, FString(), McpImageContent::FLimits(), Blocks);
		TestEqual(TEXT("return_image false: no block"), Blocks.Num(), 0);
		TestFalse(TEXT("return_image false: image_returned false"), Result->GetBoolField(TEXT("image_returned")));
	}
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		mcp_common::AttachImage(Result, nullptr, SmallPath);
		TArray<McpImageContent::FBlock> Blocks;
		McpImageContent::TakeImages(TEXT("capture_viewport"), Result, false, TEXT("off"), McpImageContent::FLimits(), Blocks);
		TestEqual(TEXT("not wanted: no block"), Blocks.Num(), 0);
		TestFalse(TEXT("not wanted: key gone"), Result->HasField(mcp_common::ImagesResultKey));
		TestEqual(TEXT("not wanted: the reason"), Result->GetStringField(TEXT("image_note")), FString(TEXT("off")));
	}
	{
		TArray<TSharedPtr<FJsonValue>> Items;
		for (int32 Index = 0; Index < 6; ++Index)
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			mcp_common::AttachImage(Payload, nullptr, SmallPath);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Index);
			Item->SetObjectField(TEXT("payload"), Payload);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		TSharedPtr<FJsonObject> Batch = MakeShared<FJsonObject>();
		Batch->SetArrayField(TEXT("results"), Items);
		TArray<McpImageContent::FBlock> Blocks;
		const int32 Listed = McpImageContent::TakeImages(TEXT("execute_batch"), Batch, true, FString(), McpImageContent::FLimits(), Blocks);
		TestEqual(TEXT("batch: six listed"), Listed, 6);
		TestEqual(TEXT("batch: capped"), Blocks.Num(), McpImageContent::MaxImagesPerCall);
		TestTrue(TEXT("batch: a note names the rest"), Batch->HasField(TEXT("image_note")));
		TestFalse(TEXT("batch: the fifth item says it was left out"),
			Items[4]->AsObject()->GetObjectField(TEXT("payload"))->GetBoolField(TEXT("image_returned")));
	}

	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
