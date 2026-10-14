#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Rendering
#include "Kismet/KismetRenderingLibrary.h"                  // UKismetRenderingLibrary, FDrawToRenderTargetContext
#include "Engine/TextureRenderTarget2D.h"                   // UTextureRenderTarget2D, ETextureRenderTargetFormat
#include "Engine/TextureRenderTarget2DArray.h"              // UTextureRenderTarget2DArray (get_render_target_2d_array_info)
#include "Engine/TextureRenderTargetCube.h"                 // UTextureRenderTargetCube (get_render_target_cube_info)
#include "Engine/Canvas.h"                                  // UCanvas (K2_DrawText/K2_DrawLine/K2_DrawTexture/K2_DrawMaterial/K2_DrawTile)
#include "Engine/Texture.h"                                 // UTexture
#include "Engine/Font.h"                                    // UFont
#include "Engine/Engine.h"                                  // GEngine (GetTinyFont/GetMediumFont)
#include "Materials/MaterialInterface.h"                    // UMaterialInterface
#include "PixelFormat.h"                                    // GetPixelFormatString, EPixelFormat

// Engine / editor
#include "Editor.h"                                         // GEditor
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"                                // IFileManager

/**
 * Render target and canvas drawing commands.
 *
 * They operate on render target assets persisted under /Game (UTextureRenderTarget2D; the 2D-array and cube
 * variants are only inspected) through the canonical Blueprint-exposed rendering API:
 *   - UKismetRenderingLibrary (Runtime/Engine/Classes/Kismet/KismetRenderingLibrary.h): ClearRenderTarget2D,
 *     DrawMaterialToRenderTarget, ExportRenderTarget and the BeginDrawCanvasToRenderTarget /
 *     EndDrawCanvasToRenderTarget canvas batch;
 *   - UTextureRenderTarget2D (Runtime/Engine/Classes/Engine/TextureRenderTarget2D.h): InitCustomFormat /
 *     GetFormat(), SizeX / SizeY / RenderTargetFormat (ETextureRenderTargetFormat).
 *
 * Engine module only. All operations are headless-safe in the editor world
 * (GEditor->GetEditorWorldContext().World()).
 */
namespace UEAgentRenderTargetCommands
{

// MCP-DOMAIN: Asset

namespace
{
    // Map a string format name to ETextureRenderTargetFormat. Defaults to RTF_RGBA8
    // (a fixed-point LDR format that exports to PNG; HDR formats export to .HDR).
    ETextureRenderTargetFormat ParseRenderTargetFormat(const FString& InFormat, bool& bOutRecognized)
    {
        bOutRecognized = true;
        const FString F = InFormat.TrimStartAndEnd();
        if (F.Equals(TEXT("RTF_R8"), ESearchCase::IgnoreCase) || F.Equals(TEXT("R8"), ESearchCase::IgnoreCase)) { return RTF_R8; }
        if (F.Equals(TEXT("RTF_RG8"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RG8"), ESearchCase::IgnoreCase)) { return RTF_RG8; }
        if (F.Equals(TEXT("RTF_RGBA8"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RGBA8"), ESearchCase::IgnoreCase)) { return RTF_RGBA8; }
        if (F.Equals(TEXT("RTF_RGBA8_SRGB"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RGBA8_SRGB"), ESearchCase::IgnoreCase)) { return RTF_RGBA8_SRGB; }
        if (F.Equals(TEXT("RTF_R16f"), ESearchCase::IgnoreCase) || F.Equals(TEXT("R16f"), ESearchCase::IgnoreCase)) { return RTF_R16f; }
        if (F.Equals(TEXT("RTF_RG16f"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RG16f"), ESearchCase::IgnoreCase)) { return RTF_RG16f; }
        if (F.Equals(TEXT("RTF_RGBA16f"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RGBA16f"), ESearchCase::IgnoreCase)) { return RTF_RGBA16f; }
        if (F.Equals(TEXT("RTF_R32f"), ESearchCase::IgnoreCase) || F.Equals(TEXT("R32f"), ESearchCase::IgnoreCase)) { return RTF_R32f; }
        if (F.Equals(TEXT("RTF_RG32f"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RG32f"), ESearchCase::IgnoreCase)) { return RTF_RG32f; }
        if (F.Equals(TEXT("RTF_RGBA32f"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RGBA32f"), ESearchCase::IgnoreCase)) { return RTF_RGBA32f; }
        if (F.Equals(TEXT("RTF_RGB10A2"), ESearchCase::IgnoreCase) || F.Equals(TEXT("RGB10A2"), ESearchCase::IgnoreCase)) { return RTF_RGB10A2; }
        bOutRecognized = false;
        return RTF_RGBA8;
    }

    const TCHAR* RenderTargetFormatToString(ETextureRenderTargetFormat InFormat)
    {
        switch (InFormat)
        {
        case RTF_R8:          return TEXT("RTF_R8");
        case RTF_RG8:         return TEXT("RTF_RG8");
        case RTF_RGBA8:       return TEXT("RTF_RGBA8");
        case RTF_RGBA8_SRGB:  return TEXT("RTF_RGBA8_SRGB");
        case RTF_R16f:        return TEXT("RTF_R16f");
        case RTF_RG16f:       return TEXT("RTF_RG16f");
        case RTF_RGBA16f:     return TEXT("RTF_RGBA16f");
        case RTF_R32f:        return TEXT("RTF_R32f");
        case RTF_RG32f:       return TEXT("RTF_RG32f");
        case RTF_RGBA32f:     return TEXT("RTF_RGBA32f");
        case RTF_RGB10A2:     return TEXT("RTF_RGB10A2");
        default:              return TEXT("Unknown");
        }
    }

    // Reads a 4-element [R,G,B,A] color array with a caller-supplied default.
    FLinearColor ParseLinearColorWithDefault(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, const FLinearColor& Default)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        Params->TryGetArrayField(FieldName, Arr);
        return mcp_common::LinearColorFromJsonArray(Arr, Default);
    }

    // Reads a 2-element [X,Y] array with a caller-supplied default.
    FVector2D ParseVector2D(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, const FVector2D& Default)
    {
        FVector2D V = Default;
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(FieldName, Arr) && Arr)
        {
            if (Arr->Num() > 0) { V.X = (*Arr)[0]->AsNumber(); }
            if (Arr->Num() > 1) { V.Y = (*Arr)[1]->AsNumber(); }
        }
        return V;
    }

    // Resolves Path to an AssetType; null on success, else the "<Label> not found: <Path>" error reply.
    template <typename AssetType>
    TSharedPtr<FJsonObject> RenderTarget_ResolveAsset(const FString& Path, const TCHAR* Label, AssetType*& OutAsset)
    {
        OutAsset = mcp_common::ResolveAsset<AssetType>(Path);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s not found: %s"), Label, *Path));
        }
        return nullptr;
    }

    // Reads the required "render_target_path" and resolves it to a RenderTargetType (Label names the type in the
    // not-found error); null on success, else the error reply.
    template <typename RenderTargetType>
    TSharedPtr<FJsonObject> RenderTarget_ReadTarget(const TSharedPtr<FJsonObject>& Params, const TCHAR* Label, RenderTargetType*& OutTarget)
    {
        FString RTPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("render_target_path"), RTPath)) { return Missing; }
        return RenderTarget_ResolveAsset(RTPath, Label, OutTarget);
    }

    // RenderTarget_ReadTarget for a UTextureRenderTarget2D, then the editor world to draw in; null on success, else the
    // error reply.
    TSharedPtr<FJsonObject> RenderTarget_ReadDrawTarget(const TSharedPtr<FJsonObject>& Params, UTextureRenderTarget2D*& OutRT,
        UWorld*& OutWorld)
    {
        if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadTarget(Params, TEXT("Render target"), OutRT))
        {
            return Error;
        }
        return mcp_common::RequireEditorWorld(OutWorld);
    }

    // Runs DrawOnCanvas on a canvas over RT, between BeginDrawCanvasToRenderTarget and EndDrawCanvasToRenderTarget,
    // with the canvas size; null on success, else the error reply (nothing is drawn without a canvas).
    TSharedPtr<FJsonObject> RenderTarget_DrawCanvas(UWorld* World, UTextureRenderTarget2D* RT,
        TFunctionRef<void(UCanvas&, const FVector2D&)> DrawOnCanvas)
    {
        UCanvas* Canvas = nullptr;
        FVector2D CanvasSize(0.0f, 0.0f);
        FDrawToRenderTargetContext DrawContext;
        UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, Canvas, CanvasSize, DrawContext);
        if (!Canvas)
        {
            return mcp_common::MakeErrorReply(TEXT("BeginDrawCanvasToRenderTarget returned a null canvas"));
        }
        DrawOnCanvas(*Canvas, CanvasSize);
        UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, DrawContext);
        return nullptr;
    }

    // Reads the required "render_target_path" and "material_path", resolves both, and gets the editor world to
    // draw in; null on success, else the error reply.
    TSharedPtr<FJsonObject> RenderTarget_ReadMaterialDraw(const TSharedPtr<FJsonObject>& Params, UTextureRenderTarget2D*& OutRT,
        UMaterialInterface*& OutMaterial, UWorld*& OutWorld)
    {
        FString RTPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("render_target_path"), RTPath)) { return Missing; }

        FString MaterialPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

        if (TSharedPtr<FJsonObject> Error = RenderTarget_ResolveAsset(RTPath, TEXT("Render target"), OutRT))
        {
            return Error;
        }

        // Load the material (try as given, then with .Object suffix).
        if (TSharedPtr<FJsonObject> Error = RenderTarget_ResolveAsset(MaterialPath, TEXT("Material"), OutMaterial))
        {
            return Error;
        }

        return mcp_common::RequireEditorWorld(OutWorld);
    }
}

// ---------------------------------------------------------------------------
// create_render_target_2d
// Creates (or loads, idempotently) a persisted UTextureRenderTarget2D under a
// /Game path and saves it. Uses NewObject + InitCustomFormat (when a format is
// requested) / InitAutoFormat, which both create the FTextureResource. The asset
// is a real saveable /Game asset (NOT a transient render target).
// ---------------------------------------------------------------------------
// MCP:create_render_target_2d - Create or load a persisted UTextureRenderTarget2D asset
//   @param name - Name of the render target asset to create
//   @param package_path - Target content path, defaults to /Game/RenderTargets
//   @param width - Render target width in pixels, default 256
//   @param height - Render target height in pixels, default 256
//   @param format - RTF_RGBA8 default, RTF_RGBA16f for HDR, RTF_R8, RTF_RGBA8_SRGB, etc
//   @returns { render_target_name: string, asset_path: string, created: bool, already_exists: bool, saved: bool, width: int, height: int, format: string, pixel_format: string, format_warning: string, save_error: string }
TSharedPtr<FJsonObject> HandleCreateRenderTarget2D(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/RenderTargets");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    int32 Width = 256;
    int32 Height = 256;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("width"), Tmp))  { Width = FMath::Max(1, (int32)Tmp); }
        if (Params->TryGetNumberField(TEXT("height"), Tmp)) { Height = FMath::Max(1, (int32)Tmp); }
    }

    // Optional explicit format; default RTF_RGBA8 (LDR, exports as PNG).
    FString FormatStr;
    bool bHasFormat = Params->TryGetStringField(TEXT("format"), FormatStr) && !FormatStr.IsEmpty();
    bool bFormatRecognized = true;
    ETextureRenderTargetFormat Format = RTF_RGBA8;
    if (bHasFormat)
    {
        Format = ParseRenderTargetFormat(FormatStr, bFormatRecognized);
    }

    const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);

    // Idempotent: if it already exists, load and return a stable payload shape.
    if (UTextureRenderTarget2D* Existing = LoadObject<UTextureRenderTarget2D>(nullptr, *ObjectPath))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("render_target_name"), Existing->GetName());
        Data->SetStringField(TEXT("asset_path"), ObjectPath);
        Data->SetBoolField(TEXT("created"), false);
        Data->SetBoolField(TEXT("already_exists"), true);
        Data->SetNumberField(TEXT("width"), Existing->SizeX);
        Data->SetNumberField(TEXT("height"), Existing->SizeY);
        Data->SetStringField(TEXT("format"), RenderTargetFormatToString(Existing->RenderTargetFormat.GetValue()));
        Data->SetStringField(TEXT("pixel_format"), GetPixelFormatString(Existing->GetFormat()));
        return mcp_common::MakeOkReply(Data);
    }

    FString CreateError;
    UTextureRenderTarget2D* NewRT = mcp_common::NewAssetInPackage<UTextureRenderTarget2D>(PackageName, Name,
        TEXT("Failed to create UTextureRenderTarget2D object"), CreateError);
    if (!NewRT)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // Record the requested render-target format on the asset, then init the
    // resource. InitCustomFormat takes a concrete EPixelFormat (derived from the
    // ETextureRenderTargetFormat). InitAutoFormat would derive from bHDR; we set
    // the explicit format so re-reads classify deterministically.
    NewRT->RenderTargetFormat = Format;
    NewRT->ClearColor = FLinearColor::Black;
    const EPixelFormat PixelFormat = GetPixelFormatFromRenderTargetFormat(Format);
    const bool bForceLinearGamma = (Format != RTF_RGBA8_SRGB);
    NewRT->InitCustomFormat((uint32)Width, (uint32)Height, PixelFormat, bForceLinearGamma);
    NewRT->UpdateResourceImmediate(true);

    FAssetRegistryModule::AssetCreated(NewRT);
    NewRT->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewRT->GetPackage(), NewRT, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_name"), NewRT->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    Data->SetNumberField(TEXT("width"), NewRT->SizeX);
    Data->SetNumberField(TEXT("height"), NewRT->SizeY);
    Data->SetStringField(TEXT("format"), RenderTargetFormatToString(NewRT->RenderTargetFormat.GetValue()));
    Data->SetStringField(TEXT("pixel_format"), GetPixelFormatString(NewRT->GetFormat()));
    if (bHasFormat && !bFormatRecognized)
    {
        Data->SetStringField(TEXT("format_warning"),
            FString::Printf(TEXT("Unrecognized format '%s'; defaulted to RTF_RGBA8"), *FormatStr));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_render_target_2d"), SaveError);
}

// ---------------------------------------------------------------------------
// get_render_target_info
// Reads SizeX/SizeY, the high-level render-target format, the resolved pixel
// format and sRGB flag from the asset.
// ---------------------------------------------------------------------------
// MCP:get_render_target_info - Read metadata for a UTextureRenderTarget2D asset
//   @param render_target_path - Asset path of the render target, e.g. /Game/RenderTargets/RT_Example
//   @returns { render_target_name: string, asset_path: string, width: int, height: int, format: string, pixel_format: string, is_srgb: bool }
TSharedPtr<FJsonObject> HandleGetRenderTargetInfo(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2D* RT = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadTarget(Params, TEXT("Render target"), RT))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_name"), RT->GetName());
    Data->SetStringField(TEXT("asset_path"), RT->GetPathName());
    Data->SetNumberField(TEXT("width"), RT->SizeX);
    Data->SetNumberField(TEXT("height"), RT->SizeY);
    Data->SetStringField(TEXT("format"), RenderTargetFormatToString(RT->RenderTargetFormat.GetValue()));
    Data->SetStringField(TEXT("pixel_format"), GetPixelFormatString(RT->GetFormat()));
    Data->SetBoolField(TEXT("is_srgb"), RT->IsSRGB());
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_render_target_2d_array_info - Read metadata for a UTextureRenderTarget2DArray asset (size, slice count, format). Metadata-only, no pixel readback.
//   @param render_target_path - Asset path of the render target, e.g. /Game/RenderTargets/RTA_Example
//   @returns { render_target_name: string, asset_path: string, width: int, height: int, slices: int, pixel_format: string, is_hdr: bool, is_srgb: bool }
TSharedPtr<FJsonObject> HandleGetRenderTarget2DArrayInfo(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2DArray* RT = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadTarget(Params, TEXT("Render target array"), RT))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_name"), RT->GetName());
    Data->SetStringField(TEXT("asset_path"), RT->GetPathName());
    Data->SetNumberField(TEXT("width"), RT->SizeX);
    Data->SetNumberField(TEXT("height"), RT->SizeY);
    Data->SetNumberField(TEXT("slices"), RT->Slices);
    Data->SetStringField(TEXT("pixel_format"), GetPixelFormatString(RT->GetFormat()));
    Data->SetBoolField(TEXT("is_hdr"), RT->bHDR != 0);
    Data->SetBoolField(TEXT("is_srgb"), RT->IsSRGB());
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_render_target_cube_info - Read metadata for a UTextureRenderTargetCube asset (face size, mip count, format). Metadata-only, no pixel readback.
//   @param render_target_path - Asset path of the render target, e.g. /Game/RenderTargets/RTC_Example
//   @returns { render_target_name: string, asset_path: string, size: int, num_mips: int, pixel_format: string, is_hdr: bool, is_srgb: bool, auto_generate_mips: bool }
TSharedPtr<FJsonObject> HandleGetRenderTargetCubeInfo(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTargetCube* RT = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadTarget(Params, TEXT("Render target cube"), RT))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_name"), RT->GetName());
    Data->SetStringField(TEXT("asset_path"), RT->GetPathName());
    Data->SetNumberField(TEXT("size"), RT->SizeX);
    Data->SetNumberField(TEXT("num_mips"), RT->GetNumMips());
    Data->SetStringField(TEXT("pixel_format"), GetPixelFormatString(RT->GetFormat()));
    Data->SetBoolField(TEXT("is_hdr"), RT->bHDR != 0);
    Data->SetBoolField(TEXT("is_srgb"), RT->IsSRGB());
    Data->SetBoolField(TEXT("auto_generate_mips"), RT->bAutoGenerateMips != 0);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// clear_render_target
// UKismetRenderingLibrary::ClearRenderTarget2D(World, RT, FLinearColor).
// ---------------------------------------------------------------------------
// MCP:clear_render_target - Clear a render target to a solid color via ClearRenderTarget2D
//   @param render_target_path - Asset path of the render target
//   @param color - Optional [r, g, b, a] clear color, defaults to opaque black
//   @returns { render_target_path: string, cleared: bool, clear_color: [number] }
TSharedPtr<FJsonObject> HandleClearRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2D* RT = nullptr;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadDrawTarget(Params, RT, World))
    {
        return Error;
    }

    const FLinearColor Color = ParseLinearColorWithDefault(Params, TEXT("color"), FLinearColor::Black);

    UKismetRenderingLibrary::ClearRenderTarget2D(World, RT, Color);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetBoolField(TEXT("cleared"), true);
    Data->SetArrayField(TEXT("clear_color"), mcp_common::ColorToJsonArray(Color));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// draw_material_to_render_target
// UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RT, Material).
// Renders a full-screen quad with the material into the render target.
// ---------------------------------------------------------------------------
// MCP:draw_material_to_render_target - Render a material full-screen quad into a render target
//   @param render_target_path - Asset path of the destination render target
//   @param material_path - Asset path of the UMaterialInterface to draw
//   @returns { render_target_path: string, material_path: string, drawn: bool }
TSharedPtr<FJsonObject> HandleDrawMaterialToRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2D* RT = nullptr;
    UMaterialInterface* Material = nullptr;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadMaterialDraw(Params, RT, Material, World))
    {
        return Error;
    }

    UKismetRenderingLibrary::DrawMaterialToRenderTarget(World, RT, Material);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetStringField(TEXT("material_path"), Material->GetPathName());
    Data->SetBoolField(TEXT("drawn"), true);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// export_render_target
// UKismetRenderingLibrary::ExportRenderTarget(World, RT, Dir, Name) -> writes a
// PNG (LDR formats) or HDR (float formats) file to disk. Verifies the file
// exists afterwards via IFileManager.
// ---------------------------------------------------------------------------
// MCP:export_render_target - Export a render target to an image file on disk
//   @param render_target_path - Asset path of the render target to export
//   @param output_directory - Absolute directory path to write into, created if missing
//   @param filename - Output file name including extension, e.g. RT_Example.png
//   @returns { render_target_path: string, output_directory: string, filename: string, file_path: string, exported: bool, file_exists: bool, file_size_bytes: number }
TSharedPtr<FJsonObject> HandleExportRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString RTPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("render_target_path"), RTPath)) { return Missing; }

    FString OutputDirectory;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_directory"), OutputDirectory)) { return Missing; }

    FString FileName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("filename"), FileName)) { return Missing; }

    UTextureRenderTarget2D* RT = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ResolveAsset(RTPath, TEXT("Render target"), RT))
    {
        return Error;
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    // Ensure the output directory exists (ExportRenderTarget itself does not
    // create it; a missing dir silently produces no file).
    IFileManager& FM = IFileManager::Get();
    FM.MakeDirectory(*OutputDirectory, /*Tree*/ true);

    UKismetRenderingLibrary::ExportRenderTarget(World, RT, OutputDirectory, FileName);

    // ExportRenderTarget appends no extension if one is present; it writes PNG for
    // LDR formats and HDR for float formats. Build the expected full path and
    // verify it exists.
    FString FullPath = FPaths::Combine(OutputDirectory, FileName);
    bool bExists = FM.FileExists(*FullPath);

    int64 FileSize = bExists ? FM.FileSize(*FullPath) : -1;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetStringField(TEXT("output_directory"), OutputDirectory);
    Data->SetStringField(TEXT("filename"), FileName);
    Data->SetStringField(TEXT("file_path"), FullPath);
    Data->SetBoolField(TEXT("exported"), true);
    Data->SetBoolField(TEXT("file_exists"), bExists);
    Data->SetNumberField(TEXT("file_size_bytes"), (double)FileSize);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Canvas batch drawing.
//
// Every command here uses the canonical batch path (RenderTarget_DrawCanvas):
//   UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(World, RT, OutCanvas,
//       OutSize, OutContext)   -> draw with the returned UCanvas (Canvas.h K2_Draw*) ->
//   UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(World, Context)
// Engine module only; headless-safe in the editor world.

// ---------------------------------------------------------------------------
// clear_and_fill_render_target
// Begin -> Canvas->K2_DrawTile(white texture, full rect, tint=color) -> End.
// Fills the whole render target with a solid color via the canvas (the point of
// this group is exercising the UCanvas batch API, not ClearRenderTarget2D).
// ---------------------------------------------------------------------------
// MCP:clear_and_fill_render_target - Fill an entire render target with a solid color via the canvas
//   @param render_target_path - Asset path of the destination render target, e.g. /Game/MyGame/RT_Example
//   @param color - Optional [r, g, b, a] fill color, defaults to opaque white
//   @returns { render_target_path: string, drawn: bool, canvas_width: number, canvas_height: number, fill_color: [number] }
TSharedPtr<FJsonObject> HandleClearAndFillRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2D* RT = nullptr;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadDrawTarget(Params, RT, World))
    {
        return Error;
    }

    const FLinearColor Color = ParseLinearColorWithDefault(Params, TEXT("color"), FLinearColor::White);

    FVector2D CanvasSize(0.0f, 0.0f);
    auto DrawOnCanvas = [&](UCanvas& Canvas, const FVector2D& DrawSize)
    {
        CanvasSize = DrawSize;
        // DrawTile of the engine white texture over the entire canvas, tinted to Color.
        // DrawColor is applied to DrawTile, so set it to the requested fill color.
        Canvas.SetLinearDrawColor(Color);
        UTexture* WhiteTex = Canvas.DefaultTexture; // engine default white texture
        Canvas.DrawTile(
            WhiteTex,
            0.0f, 0.0f,                           // X, Y
            (float)DrawSize.X, (float)DrawSize.Y, // XL, YL (full rect)
            0.0f, 0.0f, 1.0f, 1.0f,               // U, V, UL, VL
            BLEND_Opaque);
    };
    if (TSharedPtr<FJsonObject> Error = RenderTarget_DrawCanvas(World, RT, DrawOnCanvas))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetBoolField(TEXT("drawn"), true);
    Data->SetNumberField(TEXT("canvas_width"), CanvasSize.X);
    Data->SetNumberField(TEXT("canvas_height"), CanvasSize.Y);
    Data->SetArrayField(TEXT("fill_color"), mcp_common::ColorToJsonArray(Color));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// draw_text_to_render_target
// Begin -> Canvas->K2_DrawText(font, text, position, scale, color, ...) -> End.
// Font defaults to GEngine->GetMediumFont() (falls back to GetTinyFont()).
// ---------------------------------------------------------------------------
// MCP:draw_text_to_render_target - Draw a string of text into a render target via the canvas
//   @param render_target_path - Asset path of the destination render target
//   @param text - Text to draw
//   @param position - Optional [x, y] text position, default [0,0]
//   @param color - Optional [r, g, b, a] text color, defaults to opaque white
//   @param scale - Optional uniform number or [x, y] scale, default 1
//   @returns { render_target_path: string, text: string, drawn: bool, font_resolved: bool, position: [number] }
TSharedPtr<FJsonObject> HandleDrawTextToRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString RTPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("render_target_path"), RTPath)) { return Missing; }

    FString Text;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("text"), Text, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UTextureRenderTarget2D* RT = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ResolveAsset(RTPath, TEXT("Render target"), RT))
    {
        return Error;
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const FVector2D Position = ParseVector2D(Params, TEXT("position"), FVector2D(0.0f, 0.0f));
    const FLinearColor Color = ParseLinearColorWithDefault(Params, TEXT("color"), FLinearColor::White);

    // 'scale' may be a single number (uniform) or a [x,y] array.
    FVector2D Scale(1.0f, 1.0f);
    double UniformScale = 0.0;
    if (Params->TryGetNumberField(TEXT("scale"), UniformScale))
    {
        Scale = FVector2D(UniformScale, UniformScale);
    }
    else
    {
        Scale = ParseVector2D(Params, TEXT("scale"), FVector2D(1.0f, 1.0f));
    }

    // Resolve a font: GEngine medium font, fall back to tiny font.
    UFont* Font = GEngine ? GEngine->GetMediumFont() : nullptr;
    if (!Font && GEngine) { Font = GEngine->GetTinyFont(); }

    // K2_DrawText(RenderFont, RenderText, ScreenPosition, Scale, RenderColor, Kerning,
    //   ShadowColor, ShadowOffset, bCentreX, bCentreY, bOutlined, OutlineColor).
    // A null RenderFont causes K2_DrawText to use a default engine font internally.
    auto DrawOnCanvas = [&](UCanvas& Canvas, const FVector2D&)
    {
        Canvas.K2_DrawText(
            Font,
            Text,
            Position,
            Scale,
            Color);
    };
    if (TSharedPtr<FJsonObject> Error = RenderTarget_DrawCanvas(World, RT, DrawOnCanvas))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetStringField(TEXT("text"), Text);
    Data->SetBoolField(TEXT("drawn"), true);
    Data->SetBoolField(TEXT("font_resolved"), Font != nullptr);
    Data->SetArrayField(TEXT("position"), mcp_common::Vector2DToJsonArray(Position));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// draw_line_to_render_target
// Begin -> Canvas->K2_DrawLine(start, end, thickness, color) -> End.
// ---------------------------------------------------------------------------
// MCP:draw_line_to_render_target - Draw a line into a render target via the canvas
//   @param render_target_path - Asset path of the destination render target
//   @param start [string!] - Line start position [x, y] in screen-space pixels
//   @param end [string!] - Line end position [x, y] in screen-space pixels
//   @param color - Optional [r, g, b, a] line color, defaults to opaque white
//   @param thickness - Optional line thickness in pixels, default 1
//   @returns { render_target_path: string, drawn: bool, thickness: number, start: [number], end: [number] }
TSharedPtr<FJsonObject> HandleDrawLineToRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2D* RT = nullptr;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadDrawTarget(Params, RT, World))
    {
        return Error;
    }

    const FVector2D Start = ParseVector2D(Params, TEXT("start"), FVector2D(0.0f, 0.0f));
    const FVector2D End = ParseVector2D(Params, TEXT("end"), FVector2D(0.0f, 0.0f));
    const FLinearColor Color = ParseLinearColorWithDefault(Params, TEXT("color"), FLinearColor::White);

    float Thickness = 1.0f;
    double ThicknessTmp = 0.0;
    if (Params->TryGetNumberField(TEXT("thickness"), ThicknessTmp))
    {
        Thickness = (float)ThicknessTmp;
    }

    // K2_DrawLine(ScreenPositionA, ScreenPositionB, Thickness, RenderColor).
    auto DrawOnCanvas = [&](UCanvas& Canvas, const FVector2D&)
    {
        Canvas.K2_DrawLine(Start, End, Thickness, Color);
    };
    if (TSharedPtr<FJsonObject> Error = RenderTarget_DrawCanvas(World, RT, DrawOnCanvas))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetBoolField(TEXT("drawn"), true);
    Data->SetNumberField(TEXT("thickness"), Thickness);
    Data->SetArrayField(TEXT("start"), mcp_common::Vector2DToJsonArray(Start));
    Data->SetArrayField(TEXT("end"), mcp_common::Vector2DToJsonArray(End));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// draw_texture_to_render_target
// Begin -> Canvas->K2_DrawTexture(texture, position, size, ...) -> End.
// ---------------------------------------------------------------------------
// MCP:draw_texture_to_render_target - Draw a texture into a region of a render target via the canvas
//   @param render_target_path - Asset path of the destination render target
//   @param texture_path - Asset path of the UTexture to draw, e.g. /Engine/EngineResources/WhiteSquareTexture
//   @param position - Optional [x, y] destination position, default [0,0]
//   @param size - Optional [w, h] destination size, defaults to render target dimensions
//   @param color - Optional [r, g, b, a] tint color, defaults to opaque white
//   @returns { render_target_path: string, texture_path: string, drawn: bool, position: [number], size: [number] }
TSharedPtr<FJsonObject> HandleDrawTextureToRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString RTPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("render_target_path"), RTPath)) { return Missing; }

    FString TexturePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("texture_path"), TexturePath)) { return Missing; }

    UTextureRenderTarget2D* RT = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ResolveAsset(RTPath, TEXT("Render target"), RT))
    {
        return Error;
    }

    UTexture* Texture = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ResolveAsset(TexturePath, TEXT("Texture"), Texture))
    {
        return Error;
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const FVector2D Position = ParseVector2D(Params, TEXT("position"), FVector2D(0.0f, 0.0f));
    // Default size to the full render target if not supplied.
    const FVector2D Size = ParseVector2D(Params, TEXT("size"), FVector2D((float)RT->SizeX, (float)RT->SizeY));
    const FLinearColor Color = ParseLinearColorWithDefault(Params, TEXT("color"), FLinearColor::White);

    // K2_DrawTexture(RenderTexture, ScreenPosition, ScreenSize, CoordinatePosition,
    //   CoordinateSize=UnitVector, RenderColor=White, BlendMode=Translucent, Rotation, PivotPoint).
    auto DrawOnCanvas = [&](UCanvas& Canvas, const FVector2D&)
    {
        Canvas.K2_DrawTexture(
            Texture,
            Position,
            Size,
            FVector2D(0.0f, 0.0f),            // CoordinatePosition (UV origin)
            FVector2D(1.0f, 1.0f),            // CoordinateSize (full UV)
            Color,
            BLEND_Translucent);
    };
    if (TSharedPtr<FJsonObject> Error = RenderTarget_DrawCanvas(World, RT, DrawOnCanvas))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetStringField(TEXT("texture_path"), Texture->GetPathName());
    Data->SetBoolField(TEXT("drawn"), true);
    Data->SetArrayField(TEXT("position"), mcp_common::Vector2DToJsonArray(Position));
    Data->SetArrayField(TEXT("size"), mcp_common::Vector2DToJsonArray(Size));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// draw_material_to_render_target_region
// Begin -> Canvas->K2_DrawMaterial(material, position, size, ...) -> End.
// Draws the material into a sub-region of the render target (unlike
// DrawMaterialToRenderTarget which is always full-screen).
// ---------------------------------------------------------------------------
// MCP:draw_material_to_render_target_region - Draw a material into a region of a render target via the canvas
//   @param render_target_path - Asset path of the destination render target
//   @param material_path - Asset path of the UMaterialInterface to draw
//   @param position - Optional [x, y] destination position, default [0,0]
//   @param size - Optional [w, h] destination size, defaults to render target dimensions
//   @returns { render_target_path: string, material_path: string, drawn: bool, position: [number], size: [number] }
TSharedPtr<FJsonObject> HandleDrawMaterialToRenderTargetRegion(const TSharedPtr<FJsonObject>& Params)
{
    UTextureRenderTarget2D* RT = nullptr;
    UMaterialInterface* Material = nullptr;
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> Error = RenderTarget_ReadMaterialDraw(Params, RT, Material, World))
    {
        return Error;
    }

    const FVector2D Position = ParseVector2D(Params, TEXT("position"), FVector2D(0.0f, 0.0f));
    const FVector2D Size = ParseVector2D(Params, TEXT("size"), FVector2D((float)RT->SizeX, (float)RT->SizeY));

    // K2_DrawMaterial(RenderMaterial, ScreenPosition, ScreenSize, CoordinatePosition,
    //   CoordinateSize=UnitVector, Rotation, PivotPoint).
    auto DrawOnCanvas = [&](UCanvas& Canvas, const FVector2D&)
    {
        Canvas.K2_DrawMaterial(
            Material,
            Position,
            Size,
            FVector2D(0.0f, 0.0f),            // CoordinatePosition (UV origin)
            FVector2D(1.0f, 1.0f));           // CoordinateSize (full UV)
    };
    if (TSharedPtr<FJsonObject> Error = RenderTarget_DrawCanvas(World, RT, DrawOnCanvas))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetStringField(TEXT("material_path"), Material->GetPathName());
    Data->SetBoolField(TEXT("drawn"), true);
    Data->SetArrayField(TEXT("position"), mcp_common::Vector2DToJsonArray(Position));
    Data->SetArrayField(TEXT("size"), mcp_common::Vector2DToJsonArray(Size));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("clear_and_fill_render_target"), &HandleClearAndFillRenderTarget },
    { TEXT("clear_render_target"), &HandleClearRenderTarget },
    { TEXT("create_render_target_2d"), &HandleCreateRenderTarget2D },
    { TEXT("draw_line_to_render_target"), &HandleDrawLineToRenderTarget },
    { TEXT("draw_material_to_render_target"), &HandleDrawMaterialToRenderTarget },
    { TEXT("draw_material_to_render_target_region"), &HandleDrawMaterialToRenderTargetRegion },
    { TEXT("draw_text_to_render_target"), &HandleDrawTextToRenderTarget },
    { TEXT("draw_texture_to_render_target"), &HandleDrawTextureToRenderTarget },
    { TEXT("export_render_target"), &HandleExportRenderTarget },
    { TEXT("get_render_target_2d_array_info"), &HandleGetRenderTarget2DArrayInfo },
    { TEXT("get_render_target_cube_info"), &HandleGetRenderTargetCubeInfo },
    { TEXT("get_render_target_info"), &HandleGetRenderTargetInfo },
};
} // namespace UEAgentRenderTargetCommands
