// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "UnrealClient.h"
#include "LevelEditorViewport.h"
#include "EditorViewportClient.h"
#include "ImageUtils.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "Misc/App.h"                 // FApp::CanEverRender (honest -nullrhi gate)
#include "HighResScreenshot.h"        // FHighResScreenshotConfig / GetHighResScreenshotConfig
#include "CoreGlobals.h"              // GScreenshotResolutionX / GScreenshotResolutionY
#include "Engine/Engine.h"            // GEngine->GameViewport (prefer the PIE view)
#include "Engine/GameViewportClient.h"

/**
 * Editor viewport capture commands — the AI's "eyes". Each one writes a PNG synchronously, under
 * <Project>/Saved/Screenshots unless the filename is an absolute path, and returns the file path.
 */
namespace UEAgentViewportCommands
{

// MCP-DOMAIN: Editor

namespace
{
    /**
     * Where a screenshot is written: the filename param, "<DefaultPrefix>_<timestamp>.png" when it is absent or
     * empty, with ".png" appended when missing. A bare or relative name lands under <ProjectSaved>/Screenshots/; an
     * absolute path (FPaths::IsRelative treats a Windows drive path as absolute) is kept verbatim, so it never
     * becomes "<ProjectSaved>/Screenshots/D:/Shots/x.png".
     */
    FString Viewport_ScreenshotPath(const TSharedPtr<FJsonObject>& Params, const TCHAR* DefaultPrefix)
    {
        FString FileName;
        if (!Params->TryGetStringField(TEXT("filename"), FileName) || FileName.IsEmpty())
        {
            FileName = FString::Printf(TEXT("%s_%s.png"), DefaultPrefix, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
        }
        else if (!FileName.EndsWith(TEXT(".png")))
        {
            FileName += TEXT(".png");
        }
        return FPaths::IsRelative(FileName)
            ? (FPaths::ProjectSavedDir() / TEXT("Screenshots") / FileName)
            : FileName;
    }
}

// MCP:capture_viewport - Screenshot the active editor viewport to a PNG (forcing a fresh draw first), optionally resized and optionally returned inline as base64. Use it to SHOW a result -- proof that something looks right, an image to hand to a human. Do NOT use it to diagnose: an image costs far more context than a command and is read far less reliably than one, so for "is the actor there / did the value change / did it move" prefer get_actor_bounds, get_component_properties, raycast_scene or a property read, which answer exactly and cheaply. Captures the EDITOR viewport, not a PIE window; capture_high_res_screenshot is the higher-resolution sibling.
//   @param width - Optional target width to downscale/upscale to (omit to keep native viewport size).
//   @param height - Optional target height (only used when width is also supplied).
//   @param filename - Optional output filename (default MCP_Capture_<timestamp>.png); ".png" extension is added if missing.
//   @param return_base64 - bool, also include image_base64 + image_format in the response (default false).
//   @returns { file_path: string, width: int, height: int, byte_size: int, image_base64: string, image_format: string }

TSharedPtr<FJsonObject> HandleCaptureViewport(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor unavailable"));
    }

    FViewport* Viewport = GEditor->GetActiveViewport();
    if (!Viewport)
    {
        // Fall back to the first available level editor viewport (no window focus needed).
        for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
        {
            if (VC && VC->Viewport)
            {
                Viewport = VC->Viewport;
                break;
            }
        }
    }
    if (!Viewport)
    {
        return mcp_common::MakeErrorReply(TEXT("No editor viewport available"));
    }

    // Force a fresh draw so the readback reflects current scene state.
    Viewport->Draw();

    const FIntPoint Size = Viewport->GetSizeXY();
    if (Size.X <= 0 || Size.Y <= 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Active viewport has zero size"));
    }

    TArray<FColor> Bitmap;
    if (!Viewport->ReadPixels(Bitmap) || Bitmap.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to read viewport pixels"));
    }

    int32 OutWidth = Size.X;
    int32 OutHeight = Size.Y;

    int32 ReqWidth = 0, ReqHeight = 0;
    Params->TryGetNumberField(TEXT("width"), ReqWidth);
    Params->TryGetNumberField(TEXT("height"), ReqHeight);

    TArray<FColor> Resized;
    TArray<FColor>* Pixels = &Bitmap;
    if (ReqWidth > 0 && ReqHeight > 0 && (ReqWidth != Size.X || ReqHeight != Size.Y))
    {
        Resized.SetNumUninitialized(ReqWidth * ReqHeight);
        // NOTE: FImageUtils::ImageResize takes pre-sized destination data.
        FImageUtils::ImageResize(Size.X, Size.Y, Bitmap, ReqWidth, ReqHeight, Resized, false);
        Pixels = &Resized;
        OutWidth = ReqWidth;
        OutHeight = ReqHeight;
    }

    // ReadPixels can leave alpha at 0; force opaque so the PNG isn't fully transparent.
    for (FColor& Color : *Pixels)
    {
        Color.A = 255;
    }

    IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
    if (!PngWrapper.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create PNG image wrapper"));
    }
    if (!PngWrapper->SetRaw(Pixels->GetData(), Pixels->Num() * sizeof(FColor), OutWidth, OutHeight, ERGBFormat::BGRA, 8))
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to set raw image data"));
    }
    const TArray64<uint8>& PngData = PngWrapper->GetCompressed(100);

    const FString FilePath = Viewport_ScreenshotPath(Params, TEXT("MCP_Capture"));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);

    if (!FFileHelper::SaveArrayToFile(PngData, *FilePath))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to write screenshot to %s"), *FilePath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("file_path"), FPaths::ConvertRelativePathToFull(FilePath));
    Result->SetNumberField(TEXT("width"), OutWidth);
    Result->SetNumberField(TEXT("height"), OutHeight);
    Result->SetNumberField(TEXT("byte_size"), PngData.Num());

    bool bReturnBase64 = false;
    Params->TryGetBoolField(TEXT("return_base64"), bReturnBase64);
    if (bReturnBase64)
    {
        const FString Base64 = FBase64::Encode(PngData.GetData(), PngData.Num());
        Result->SetStringField(TEXT("image_base64"), Base64);
        Result->SetStringField(TEXT("image_format"), TEXT("png"));
    }

    return Result;
}

// ---------------------------------------------------------------------------
// High-resolution screenshot of the active editor viewport (a viewport
// capture, not Sequencer).
// ---------------------------------------------------------------------------

// MCP:capture_high_res_screenshot - Capture a high-resolution (supersampled) screenshot and write it to a PNG SYNCHRONOUSLY. Prefers the Play-In-Editor game view when PIE is active (captured_pie=true); otherwise the active editor viewport. Drives FViewport::TakeHighResScreenShot() + FViewport::Draw(), which renders at resolution_multiplier x the viewport size and blocks (via ProcessScreenShots) until the file is on disk. The "HighResShot" exec path defers the write to the next render frame, which never runs headless, so it writes nothing. Requires a real RHI (errors honestly under -nullrhi) and a live viewport. Note: the supersampled path re-renders the scene, so the UMG/Slate HUD is NOT included; use capture_pie_screenshot for a HUD-inclusive PIE shot.
//   @param resolution_multiplier - Resolution multiplier (clamped 1..16, default 2).
//   @param filename - Optional output path; a bare name lands under <ProjectSaved>/Screenshots/, an absolute path is respected verbatim. ".png" is appended if missing. Defaults to a timestamped name.
//   @returns { written: bool, file_path: string, resolution_multiplier: int, width: int, height: int, captured_pie: bool }

TSharedPtr<FJsonObject> HandleHighResScreenshot(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor not available"));
    }
    // Honest -nullrhi gate: without a real RHI the render produces no pixels and
    // no file is ever written. Fail cleanly instead of faking a request.
    if (!FApp::CanEverRender())
    {
        return mcp_common::MakeErrorReply(
            TEXT("capture_high_res_screenshot requires a real RHI; this editor is running without rendering (-nullrhi)"));
    }

    // Prefer the PIE game viewport when Play-In-Editor is active, so we capture the
    // actual play view: GEditor->GetActiveViewport() is the editor camera, so during PIE
    // it would silently grab the (brighter) editor view.
    // Outside PIE, resolve the viewport the SAME way capture_viewport does (the proven
    // path): the active viewport first, then the first level-editor viewport client (the
    // bare level-client Viewport can report size 0 while the active one is properly sized).
    // Whichever we pick, its Draw() is what writes the file inside ProcessScreenShots().
    FViewport* Viewport = nullptr;
    bool bCapturedPie = false;
    if (GEditor->PlayWorld != nullptr && GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
    {
        Viewport = GEngine->GameViewport->Viewport;
        bCapturedPie = true;
    }
    if (!Viewport)
    {
        Viewport = GEditor->GetActiveViewport();
    }
    if (!Viewport)
    {
        for (FLevelEditorViewportClient* VC : GEditor->GetLevelViewportClients())
        {
            if (VC && VC->Viewport)
            {
                Viewport = VC->Viewport;
                break;
            }
        }
    }
    if (!Viewport)
    {
        return mcp_common::MakeErrorReply(TEXT("No editor viewport available"));
    }

    double MultiplierD = 2.0;
    Params->TryGetNumberField(TEXT("resolution_multiplier"), MultiplierD);
    const int32 Multiplier = FMath::Clamp(FMath::RoundToInt(MultiplierD), 1, 16);

    const FString FilePath = FPaths::ConvertRelativePathToFull(Viewport_ScreenshotPath(Params, TEXT("MCP_HighRes")));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);

    // The viewport reports size 0 until it has been drawn at least once; the
    // high-res path would then create a 0x0 render texture and assert in the RHI
    // (editor crash: "TextureDesc.Extent.X > 0 ..."). Warm it up with real
    // draws until it reports a valid size.
    FIntPoint Size = Viewport->GetSizeXY();
    for (int32 Warm = 0; Warm < 8 && (Size.X <= 0 || Size.Y <= 0); ++Warm)
    {
        Viewport->Draw();
        Size = Viewport->GetSizeXY();
    }
    if (Size.X <= 0 || Size.Y <= 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("capture_high_res_screenshot: editor viewport produced no drawable frame (size 0)"));
    }
    const int32 OutW = Size.X * Multiplier;
    const int32 OutH = Size.Y * Multiplier;

    // Drive the synchronous high-res path. TakeHighResScreenShot() sets the capture
    // flag; the very next Draw() runs the tiling capture and, via ProcessScreenShots(),
    // BLOCKS on the image-write queue, so the PNG exists on disk before Draw() returns.
    // A non-empty FilenameOverride makes the output deterministic (verbatim, no numeric
    // suffix) so re-runs overwrite the same file (idempotent).
    FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
    Config.SetHDRCapture(false);
    Config.SetFilename(FilePath);
    Config.ResolutionMultiplier = static_cast<float>(Multiplier);
    // Set the target resolution EXPLICITLY and non-zero (viewport size x multiplier)
    // so TakeHighResScreenShot() never derives a 0x0 destination from an un-sized
    // viewport (the RHI assert above).
    GScreenshotResolutionX = static_cast<uint32>(OutW);
    GScreenshotResolutionY = static_cast<uint32>(OutH);

    const bool bRequested = Viewport->TakeHighResScreenShot();
    if (bRequested)
    {
        Viewport->Draw();
    }
    const bool bWritten = bRequested && IFileManager::Get().FileExists(*FilePath);

    // Reset shared state so later captures start clean (HighResScreenshot() clears
    // GIsHighResScreenshot / bTakeHighResScreenShot itself).
    Config.SetFilename(FString());
    Config.ResolutionMultiplier = 1.0f;
    GScreenshotResolutionX = 0;
    GScreenshotResolutionY = 0;

    if (!bWritten)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("capture_high_res_screenshot: capture produced no file at %s (requested=%s)"),
            *FilePath, bRequested ? TEXT("true") : TEXT("false")));
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetBoolField(TEXT("written"), true);
    Response->SetStringField(TEXT("file_path"), FilePath);
    Response->SetNumberField(TEXT("resolution_multiplier"), Multiplier);
    Response->SetNumberField(TEXT("width"), OutW);
    Response->SetNumberField(TEXT("height"), OutH);
    Response->SetBoolField(TEXT("captured_pie"), bCapturedPie);
    return mcp_common::MakeOkReply(Response);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("capture_viewport"), &HandleCaptureViewport },
    { TEXT("capture_high_res_screenshot"), &HandleHighResScreenshot },
};
} // namespace UEAgentViewportCommands
