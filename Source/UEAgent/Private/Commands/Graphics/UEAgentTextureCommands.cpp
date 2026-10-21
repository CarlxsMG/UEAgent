// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Texture
#include "Engine/Texture.h"                                 // UTexture, FTextureSource, TextureCompressionSettings, TextureGroup
#include "Engine/Texture2D.h"                               // UTexture2D, GetSizeX/GetSizeY/GetPixelFormat
#include "Engine/TextureDefines.h"                          // ETextureSourceFormat (TSF_BGRA8), TC_*, TEXTUREGROUP_*, TMGS_*
#include "Engine/TextureRenderTarget2D.h"                   // UTextureRenderTarget2D
#include "Engine/Texture2DArray.h"                          // UTexture2DArray, SourceTextures, UpdateSourceFromSourceTextures
#include "Engine/TextureRenderTarget2DArray.h"              // UTextureRenderTarget2DArray, Init(SizeX,SizeY,Slices,Format)
#include "Engine/TextureRenderTargetCube.h"                 // UTextureRenderTargetCube, Init(SizeX,Format) / InitAutoFormat
#include "Kismet/KismetRenderingLibrary.h"                  // UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly
#include "PixelFormat.h"                                    // GetPixelFormatString, EPixelFormat

// Engine / editor / packaging
#include "Editor.h"                                         // GEditor
#include "EditorAssetLibrary.h"                             // UEditorAssetLibrary::DoesAssetExist
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"                                  // UEnum reverse lookup
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

/**
 * Procedural texture authoring commands.
 *
 * They author and inspect persisted texture assets entirely headless, with no external file import (the import
 * commands cover that). Engine APIs used, all under Runtime/Engine/Classes:
 *   - UTexture2D (Engine/Texture2D.h): GetSizeX() / GetSizeY() / GetPixelFormat()
 *   - UTexture (Engine/Texture.h): Source (FTextureSource), SRGB, CompressionSettings, LODGroup,
 *     UpdateResource(), PostEditChange(), GetTextureGroupString()
 *   - FTextureSource (Engine/Texture.h): Init / LockMip / UnlockMip / GetSizeX / GetSizeY / GetFormat /
 *     IsValid
 *   - Engine/TextureDefines.h: ETextureSourceFormat (TSF_BGRA8), TextureCompressionSettings (TC_*),
 *     TextureGroup (TEXTUREGROUP_*)
 *   - UTexture2DArray (Engine/Texture2DArray.h): SourceTextures, UpdateSourceFromSourceTextures(),
 *     GetArraySize()
 *   - UTextureRenderTarget2DArray::Init and UTextureRenderTargetCube::Init / InitAutoFormat
 *     (Engine/TextureRenderTarget2DArray.h, Engine/TextureRenderTargetCube.h)
 *   - UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly (Kismet/KismetRenderingLibrary.h)
 *
 * Modules: Engine, UnrealEd, RenderCore. All operations run in the editor and save real /Game assets.
 */
namespace UEAgentTextureCommands
{

// MCP-DOMAIN: Asset

namespace
{
    // Map a member name of ETextureSourceFormat to a readable string for the payload.
    const TCHAR* TextureSourceFormatToString(ETextureSourceFormat InFormat)
    {
        switch (InFormat)
        {
        case TSF_Invalid:   return TEXT("TSF_Invalid");
        case TSF_G8:        return TEXT("TSF_G8");
        case TSF_BGRA8:     return TEXT("TSF_BGRA8");
        case TSF_BGRE8:     return TEXT("TSF_BGRE8");
        case TSF_RGBA16:    return TEXT("TSF_RGBA16");
        case TSF_RGBA16F:   return TEXT("TSF_RGBA16F");
        case TSF_G16:       return TEXT("TSF_G16");
        case TSF_RGBA32F:   return TEXT("TSF_RGBA32F");
        case TSF_R16F:      return TEXT("TSF_R16F");
        case TSF_R32F:      return TEXT("TSF_R32F");
        default:            return TEXT("Unknown");
        }
    }

    // Map a member name of TextureCompressionSettings to a readable string.
    const TCHAR* CompressionSettingsToString(TextureCompressionSettings InSettings)
    {
        if (const UEnum* EnumPtr = StaticEnum<TextureCompressionSettings>())
        {
            // GetNameStringByValue returns the C++ identifier (e.g. "TC_Default").
            const FString Name = EnumPtr->GetNameStringByValue((int64)InSettings);
            if (!Name.IsEmpty())
            {
                // Statically cache nothing; copy into a transient is fine for JSON.
                static thread_local FString Buf;
                Buf = Name;
                return *Buf;
            }
        }
        return TEXT("Unknown");
    }

    // Parse a compression-settings string (with or without the TC_ prefix, and
    // accepting the documented short names "Default"/"Normalmap"/"Masks"/
    // "VectorDisplacementmap"). Returns TC_Default on no match.
    TextureCompressionSettings ParseCompressionSettings(const FString& InStr, bool& bOutRecognized)
    {
        bOutRecognized = true;
        const FString S = InStr.TrimStartAndEnd();
        FString Candidate = S;
        if (!Candidate.StartsWith(TEXT("TC_")))
        {
            Candidate = FString::Printf(TEXT("TC_%s"), *Candidate);
        }
        if (const UEnum* EnumPtr = StaticEnum<TextureCompressionSettings>())
        {
            const int64 Value = EnumPtr->GetValueByNameString(Candidate);
            if (Value != INDEX_NONE)
            {
                return (TextureCompressionSettings)Value;
            }
        }
        bOutRecognized = false;
        return TC_Default;
    }

    // Parse a texture-group / LOD-group string (with or without the TEXTUREGROUP_
    // prefix). Returns TEXTUREGROUP_World on no match.
    TextureGroup ParseTextureGroup(const FString& InStr, bool& bOutRecognized)
    {
        bOutRecognized = true;
        const FString S = InStr.TrimStartAndEnd();
        FString Candidate = S;
        if (!Candidate.StartsWith(TEXT("TEXTUREGROUP_")))
        {
            Candidate = FString::Printf(TEXT("TEXTUREGROUP_%s"), *Candidate);
        }
        if (const UEnum* EnumPtr = StaticEnum<TextureGroup>())
        {
            const int64 Value = EnumPtr->GetValueByNameString(Candidate);
            if (Value != INDEX_NONE)
            {
                return (TextureGroup)Value;
            }
        }
        bOutRecognized = false;
        return TEXTUREGROUP_World;
    }

    // Parse a render-target EPixelFormat string. Accepts the common BGRA8 / RGBA16F /
    // RGBA32F / float aliases (with or without the PF_ prefix). Returns the default on
    // no match. UTextureRenderTarget2DArray / Cube take an EPixelFormat directly.
    EPixelFormat ParseRenderTargetPixelFormat(const FString& InFormat, EPixelFormat Default, bool& bOutRecognized)
    {
        bOutRecognized = true;
        const FString F = InFormat.TrimStartAndEnd();
        auto Eq = [&F](const TCHAR* A, const TCHAR* B) -> bool
        {
            return F.Equals(A, ESearchCase::IgnoreCase) || F.Equals(B, ESearchCase::IgnoreCase);
        };
        if (Eq(TEXT("PF_B8G8R8A8"), TEXT("B8G8R8A8")) || Eq(TEXT("PF_BGRA8"), TEXT("BGRA8")) || Eq(TEXT("RGBA8"), TEXT("RGBA8"))) { return PF_B8G8R8A8; }
        if (Eq(TEXT("PF_R8G8B8A8"), TEXT("R8G8B8A8"))) { return PF_R8G8B8A8; }
        if (Eq(TEXT("PF_FloatRGBA"), TEXT("FloatRGBA")) || Eq(TEXT("PF_RGBA16F"), TEXT("RGBA16F"))) { return PF_FloatRGBA; }
        if (Eq(TEXT("PF_A32B32G32R32F"), TEXT("A32B32G32R32F")) || Eq(TEXT("PF_RGBA32F"), TEXT("RGBA32F"))) { return PF_A32B32G32R32F; }
        if (Eq(TEXT("PF_G8"), TEXT("G8")) || Eq(TEXT("PF_R8"), TEXT("R8"))) { return PF_G8; }
        if (Eq(TEXT("PF_R16F"), TEXT("R16F"))) { return PF_R16F; }
        if (Eq(TEXT("PF_FloatR11G11B10"), TEXT("FloatR11G11B10"))) { return PF_FloatR11G11B10; }
        bOutRecognized = false;
        return Default;
    }

    // Allocates Texture's editor source (one slice, one mip, BGRA8) and fills every pixel with R, G, B, A; false when
    // the mip cannot be locked for writing.
    bool Texture_FillSolidSource(UTexture2D* Texture, int32 Width, int32 Height, uint8 R, uint8 G, uint8 B, uint8 A)
    {
        Texture->Source.Init(Width, Height, /*NumSlices*/ 1, /*NumMips*/ 1, TSF_BGRA8);
        uint8* MipData = Texture->Source.LockMip(0);
        if (!MipData)
        {
            return false;
        }
        // BGRA8 byte order is B, G, R, A.
        const int64 NumPixels = (int64)Width * (int64)Height;
        for (int64 P = 0; P < NumPixels; ++P)
        {
            const int64 Offset = P * 4;
            MipData[Offset + 0] = B;
            MipData[Offset + 1] = G;
            MipData[Offset + 2] = R;
            MipData[Offset + 3] = A;
        }
        Texture->Source.UnlockMip(0);
        return true;
    }

    // Create a small solid-color UTexture2D slice with a valid editor Source (so it can
    // feed UTexture2DArray::UpdateSourceFromSourceTextures, which requires Source.IsValid()).
    // Mirrors the create_texture_2d_from_data path. NOT saved on its own — these are
    // intermediate slices owned by the array's transient package.
    UTexture2D* CreateSolidColorSlice(UObject* Outer, const FName SliceName, int32 Size,
                                      uint8 R, uint8 G, uint8 B, uint8 A)
    {
        // RF_Public (referenceable across packages) but NOT RF_Standalone: these are
        // intermediate slices owned by / referenced from the array, not independent
        // top-level content-browser assets. UpdateSourceFromSourceTextures copies their
        // pixel data into the array's own Source.
        UTexture2D* Slice = NewObject<UTexture2D>(
            Outer, UTexture2D::StaticClass(), SliceName,
            RF_Public | RF_Transactional);
        if (!Slice)
        {
            return nullptr;
        }
        if (!Texture_FillSolidSource(Slice, Size, Size, R, G, B, A))
        {
            return nullptr;
        }
        Slice->SRGB = true;
        Slice->CompressionSettings = TC_Default;
        Slice->MipGenSettings = TMGS_NoMipmaps;
        Slice->UpdateResource();
        Slice->PostEditChange();
        return Slice;
    }

    // Fill a populated metadata payload for a UTexture2D.
    void FillTextureInfo(TSharedPtr<FJsonObject>& Data, UTexture2D* Tex)
    {
        Data->SetStringField(TEXT("texture_name"), Tex->GetName());
        Data->SetStringField(TEXT("asset_path"), Tex->GetPathName());
        Data->SetNumberField(TEXT("width"), Tex->GetSizeX());
        Data->SetNumberField(TEXT("height"), Tex->GetSizeY());
        Data->SetStringField(TEXT("format"), GetPixelFormatString(Tex->GetPixelFormat()));
        Data->SetBoolField(TEXT("srgb"), Tex->SRGB != 0);
        Data->SetStringField(TEXT("compression"),
            CompressionSettingsToString(Tex->CompressionSettings.GetValue()));
        Data->SetStringField(TEXT("lod_group"),
            UTexture::GetTextureGroupString(Tex->LODGroup.GetValue()));
        // Sampler/LOD settings (short enum names, prefix stripped).
        auto EnumShort = [](UEnum* E, int64 Value, const TCHAR* Prefix) -> FString
        {
            if (!E) { return FString(); }
            FString N = E->GetNameStringByValue(Value);
            N.RemoveFromStart(Prefix);
            return N;
        };
        Data->SetStringField(TEXT("address_x"), EnumShort(StaticEnum<TextureAddress>(), Tex->AddressX.GetValue(), TEXT("TA_")));
        Data->SetStringField(TEXT("address_y"), EnumShort(StaticEnum<TextureAddress>(), Tex->AddressY.GetValue(), TEXT("TA_")));
        Data->SetStringField(TEXT("filter"), EnumShort(StaticEnum<TextureFilter>(), Tex->Filter.GetValue(), TEXT("TF_")));
        Data->SetStringField(TEXT("mip_gen_settings"), EnumShort(StaticEnum<TextureMipGenSettings>(), Tex->MipGenSettings.GetValue(), TEXT("TMGS_")));
        Data->SetNumberField(TEXT("lod_bias"), Tex->LODBias);
        Data->SetNumberField(TEXT("max_texture_size"), Tex->MaxTextureSize);
        Data->SetBoolField(TEXT("flip_green"), Tex->bFlipGreenChannel != 0);
        const bool bHasSource = Tex->Source.IsValid();
        Data->SetBoolField(TEXT("has_source"), bHasSource);
        if (bHasSource)
        {
            Data->SetNumberField(TEXT("source_width"), (double)Tex->Source.GetSizeX());
            Data->SetNumberField(TEXT("source_height"), (double)Tex->Source.GetSizeY());
            Data->SetStringField(TEXT("source_format"),
                TextureSourceFormatToString(Tex->Source.GetFormat()));
        }
    }

    // Reads the required "texture_path" and resolves it to a UTexture2D; null on success, else the error reply.
    TSharedPtr<FJsonObject> Texture_ReadTexture2D(const TSharedPtr<FJsonObject>& Params, UTexture2D*& OutTexture)
    {
        FString TexPath;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("texture_path"), TexPath))
        {
            return Missing;
        }
        OutTexture = mcp_common::ResolveAsset<UTexture2D>(TexPath);
        if (!OutTexture)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Texture not found: %s"), *TexPath));
        }
        return nullptr;
    }

    // Reads the optional "format" param into OutFormat (ParseRenderTargetPixelFormat, PF_B8G8R8A8 when not
    // recognized, with OutWarning saying so); true when the caller gives a format.
    bool Texture_ReadPixelFormat(const TSharedPtr<FJsonObject>& Params, EPixelFormat& OutFormat, FString& OutWarning)
    {
        FString FormatStr;
        if (Params->TryGetStringField(TEXT("format"), FormatStr) && !FormatStr.IsEmpty())
        {
            bool bRecognized = true;
            OutFormat = ParseRenderTargetPixelFormat(FormatStr, PF_B8G8R8A8, bRecognized);
            if (!bRecognized)
            {
                OutWarning = FString::Printf(TEXT("Unrecognized format '%s'; defaulted to PF_B8G8R8A8"), *FormatStr);
            }
            return true;
        }
        return false;
    }

    // Deletes the asset at PackagePath/Name if there is one, so the command rebuilds it deterministically (NewObject
    // never lands on an occupied name and the payload stays stable), then creates its package and a new AssetType in
    // it; null on success, else the error reply (FailureMessage when the object cannot be created).
    template <typename AssetType>
    TSharedPtr<FJsonObject> Texture_CreateFreshAsset(const FString& PackagePath, const FString& Name, const TCHAR* FailureMessage,
        FString& OutObjectPath, UPackage*& OutPackage, AssetType*& OutAsset)
    {
        const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
        OutObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);
        // Before the lookup and its delete: the folder and the name each fit an FName and can still join past one,
        // which the lookup aborts the editor on.
        if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
        if (UEditorAssetLibrary::DoesAssetExist(OutObjectPath))
        {
            UEditorAssetLibrary::DeleteAsset(OutObjectPath);
        }

        FString CreateError;
        OutAsset = mcp_common::NewAssetInPackage<AssetType>(PackageName, Name, FailureMessage, CreateError);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(CreateError);
        }
        OutPackage = OutAsset->GetPackage();
        return nullptr;
    }

    // Registers the new Asset with the asset registry, dirties its package and saves it; false, with OutSaveError,
    // when the save fails.
    bool Texture_RegisterAndSave(UPackage* Package, UObject* Asset, FString& OutSaveError)
    {
        FAssetRegistryModule::AssetCreated(Asset);
        Asset->MarkPackageDirty();
        return mcp_common::SaveAssetPackage(Package, Asset, OutSaveError);
    }
}

// ---------------------------------------------------------------------------
// create_texture_2d_from_data
// Creates a real persisted UTexture2D. Allocates FTextureSource via Init(W,H,1,1,
// TSF_BGRA8), fills every pixel with fill_color (RGBA 0-255), UpdateResource(),
// AssetRegistry::AssetCreated, save. Verified by reading width/height/format back.
// Idempotent: if the asset exists, it is deleted+rebuilt so the payload is stable.
// ---------------------------------------------------------------------------
// MCP:create_texture_2d_from_data - Creates a persisted UTexture2D filled with a solid color (no external file).  Allocates the editor source (FTextureSource::Init, BGRA8), fills every pixel with ``fill_color``, calls UpdateResource, registers the asset and saves it to disk. Re-running deletes and rebuilds the asset deterministically.  fill_color is an [R, G, B, A] array of 0-255 byte values (default = opaque white).
//   @param name - Name of the texture asset to create
//   @param package_path - Content folder; defaults to /Game/Textures
//   @param width - Optional pixel width; clamped 1..8192; defaults to 64
//   @param height - Optional pixel height; clamped 1..8192; defaults to 64
//   @param fill_color - Optional [R, G, B, A] (0-255 each); defaults to opaque white
//   @returns { texture_name: string, asset_path: string, created: bool, saved: bool, width: int, height: int, format: string, has_source: bool, fill_color: [int, int, int, int], save_error?: string }

TSharedPtr<FJsonObject> HandleCreateTexture2DFromData(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/Textures");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    int32 Width = 64;
    int32 Height = 64;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("width"), Tmp))  { Width = FMath::Clamp((int32)Tmp, 1, 8192); }
        if (Params->TryGetNumberField(TEXT("height"), Tmp)) { Height = FMath::Clamp((int32)Tmp, 1, 8192); }
    }

    // fill_color is [R,G,B,A] in 0-255; default opaque white.
    uint8 R = 255, G = 255, B = 255, A = 255;
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (Params->TryGetArrayField(TEXT("fill_color"), Arr) && Arr)
        {
            auto Clamp8 = [](double V) -> uint8 { return (uint8)FMath::Clamp((int32)FMath::RoundToInt(V), 0, 255); };
            if (Arr->Num() > 0) { R = Clamp8((*Arr)[0]->AsNumber()); }
            if (Arr->Num() > 1) { G = Clamp8((*Arr)[1]->AsNumber()); }
            if (Arr->Num() > 2) { B = Clamp8((*Arr)[2]->AsNumber()); }
            if (Arr->Num() > 3) { A = Clamp8((*Arr)[3]->AsNumber()); }
        }
    }

    FString ObjectPath;
    UPackage* Package = nullptr;
    UTexture2D* NewTex = nullptr;
    if (TSharedPtr<FJsonObject> Error = Texture_CreateFreshAsset(PackagePath, Name, TEXT("Failed to create UTexture2D object"),
        ObjectPath, Package, NewTex))
    {
        return Error;
    }

    if (!Texture_FillSolidSource(NewTex, Width, Height, R, G, B, A))
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to lock texture source mip for writing"));
    }

    // Reasonable defaults for a hand-authored color texture.
    NewTex->SRGB = true;
    NewTex->CompressionSettings = TC_Default;
    NewTex->MipGenSettings = TMGS_FromTextureGroup;

    NewTex->UpdateResource();
    NewTex->PostEditChange();

    FString SaveError;
    const bool bSaved = Texture_RegisterAndSave(Package, NewTex, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("texture_name"), NewTex->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    // Read-back verification straight off the asset.
    Data->SetNumberField(TEXT("width"), NewTex->GetSizeX());
    Data->SetNumberField(TEXT("height"), NewTex->GetSizeY());
    Data->SetStringField(TEXT("format"), GetPixelFormatString(NewTex->GetPixelFormat()));
    Data->SetBoolField(TEXT("has_source"), NewTex->Source.IsValid());
    Data->SetArrayField(TEXT("fill_color"), mcp_common::ColorToJsonArray(FColor(R, G, B, A)));
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_texture_2d_from_data"), SaveError);
}

// ---------------------------------------------------------------------------
// create_static_texture_from_render_target
// UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly(RT, FullPath,
// CompressionSettings, MipSettings). The full /Game path is passed so the asset
// lands exactly where requested. The Kismet helper uses CreateUniqueAssetName, so
// any existing asset is deleted first for idempotency; we then save to disk.
// ---------------------------------------------------------------------------
// MCP:create_static_texture_from_render_target - Bakes a UTexture2D from an existing UTextureRenderTarget2D (editor only).  Wraps UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly and saves the resulting static texture to disk. Re-running deletes and rebuilds the asset deterministically.  render_target_path accepts either short "/Game/MyGame/RT_Example" or fully-qualified "...RT_Example.RT_Example" asset paths.
//   @param render_target_path - Asset path of the source render target (e.g. "/Game/MyGame/RT_Example" or fully-qualified "...RT_Example.RT_Example")
//   @param name - Name of the static texture asset to create
//   @param package_path - Content folder; defaults to /Game/Textures
//   @returns { texture_name: string, asset_path: string, render_target_path: string, created: bool, saved: bool, width: int, height: int, format: string, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateStaticTextureFromRenderTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString RTPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("render_target_path"), RTPath)) { return Missing; }

    FString PackagePath = TEXT("/Game/Textures");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    UTextureRenderTarget2D* RT = mcp_common::ResolveAsset<UTextureRenderTarget2D>(RTPath);
    if (!RT)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Render target not found: %s"), *RTPath));
    }

    const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    const FString ObjectPath  = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }

    // Idempotent: delete any existing asset at the destination so the Kismet
    // CreateUniqueAssetName path produces our exact requested name (no _1 suffix).
    if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
    {
        UEditorAssetLibrary::DeleteAsset(ObjectPath);
    }

    // Pass the full long package path so the helper places the asset deterministically.
    UTexture2D* NewTex = UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly(
        RT, PackageName, TC_Default, TMGS_FromTextureGroup);
    if (!NewTex)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create static texture from render target: %s"), *RTPath));
    }

    // The helper marks dirty + AssetCreated but does NOT save; persist it now.
    UPackage* Package = NewTex->GetOutermost();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, NewTex, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("texture_name"), NewTex->GetName());
    Data->SetStringField(TEXT("asset_path"), NewTex->GetPathName());
    Data->SetStringField(TEXT("render_target_path"), RT->GetPathName());
    Data->SetBoolField(TEXT("created"), true);
    Data->SetNumberField(TEXT("width"), NewTex->GetSizeX());
    Data->SetNumberField(TEXT("height"), NewTex->GetSizeY());
    Data->SetStringField(TEXT("format"), GetPixelFormatString(NewTex->GetPixelFormat()));
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_static_texture_from_render_target"), SaveError);
}

// ---------------------------------------------------------------------------
// set_texture_settings
// Sets UTexture::SRGB / CompressionSettings / LODGroup on an existing UTexture2D,
// then PostEditChange + UpdateResource + save. Reads the resulting values back.
// ---------------------------------------------------------------------------
// MCP:set_texture_settings - Sets the given sRGB, compression, LOD group and sampler/LOD fields (address X/Y, filter, mip-gen settings, LOD bias, max size, flip-green) of a UTexture2D asset, rebuilds its resource when one changed (PostEditChange + UpdateResource, no shader recompile) and saves it in every case, returning every setting read back. An unknown compression or LOD group name is still applied, as TC_Default or TEXTUREGROUP_World, with a compression_warning or lod_group_warning; an unknown address, filter or mip-gen name is skipped with an address_x_warning, address_y_warning, filter_warning or mip_gen_settings_warning. Enum params accept the name with or without its prefix.
//   @param texture_path - Asset path of the texture to modify
//   @param srgb - True treats the texture as sRGB color, false as linear data such as normal or mask maps; omit to keep the current value
//   @param compression_settings - TextureCompressionSettings name with or without the TC_ prefix, e.g. "Default", "Normalmap", "Masks", "VectorDisplacementmap", "BC7", "HDR"; omit to keep the current value
//   @param lod_group - TextureGroup name with or without the TEXTUREGROUP_ prefix, e.g. "World", "UI", "Character", "Effects"; omit to keep the current value
//   @param address_x - Optional TextureAddress name, with or without TA_: Wrap, Clamp or Mirror; omit to keep the current value
//   @param address_y - Optional TextureAddress name, with or without TA_: Wrap, Clamp or Mirror; omit to keep the current value
//   @param filter - Optional TextureFilter name, with or without TF_: Nearest, Bilinear, Trilinear or Default; omit to keep the current value
//   @param mip_gen_settings - Optional TextureMipGenSettings name, with or without TMGS_: FromTextureGroup, SimpleAverage, NoMipmaps, LeaveExistingMips, Sharpen0..10, Blur1..5, Unfiltered, Angular; omit to keep the current value
//   @param lod_bias - Optional int LOD bias; omit to keep the current value
//   @param max_texture_size - Optional int max texture size (0 = no limit, negatives clamp to 0); omit to keep the current value
//   @param flip_green - Optional bool, flips the green channel; omit to keep the current value
//   @returns { texture_name: string, asset_path: string, changed: bool, saved: bool, width: int, height: int, format: string, srgb: bool, compression: string, lod_group: string, address_x: string, address_y: string, filter: string, mip_gen_settings: string, lod_bias: int, max_texture_size: int, flip_green: bool, has_source: bool, source_width?: int, source_height?: int, source_format?: string, compression_warning?: string, lod_group_warning?: string, address_x_warning?: string, address_y_warning?: string, filter_warning?: string, mip_gen_settings_warning?: string, save_error?: string }

TSharedPtr<FJsonObject> HandleSetTextureSettings(const TSharedPtr<FJsonObject>& Params)
{
    UTexture2D* Tex = nullptr;
    if (TSharedPtr<FJsonObject> Error = Texture_ReadTexture2D(Params, Tex))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    bool bChanged = false;

    bool bSRGB = false;
    if (Params->TryGetBoolField(TEXT("srgb"), bSRGB))
    {
        Tex->SRGB = bSRGB;
        bChanged = true;
    }

    FString CompStr;
    if (Params->TryGetStringField(TEXT("compression_settings"), CompStr) && !CompStr.IsEmpty())
    {
        bool bRecognized = true;
        const TextureCompressionSettings Comp = ParseCompressionSettings(CompStr, bRecognized);
        if (!bRecognized)
        {
            Data->SetStringField(TEXT("compression_warning"),
                FString::Printf(TEXT("Unrecognized compression_settings '%s'; defaulted to TC_Default"), *CompStr));
        }
        Tex->CompressionSettings = Comp;
        bChanged = true;
    }

    FString GroupStr;
    if (Params->TryGetStringField(TEXT("lod_group"), GroupStr) && !GroupStr.IsEmpty())
    {
        bool bRecognized = true;
        const TextureGroup Group = ParseTextureGroup(GroupStr, bRecognized);
        if (!bRecognized)
        {
            Data->SetStringField(TEXT("lod_group_warning"),
                FString::Printf(TEXT("Unrecognized lod_group '%s'; defaulted to TEXTUREGROUP_World"), *GroupStr));
        }
        Tex->LODGroup = Group;
        bChanged = true;
    }

    // Sampler/LOD enums by name: accept the value with or without its TA_/TF_/TMGS_ prefix.
    auto ParseEnumByName = [](UEnum* E, const TCHAR* Prefix, const FString& In, int64& OutValue) -> bool
    {
        if (!E || In.IsEmpty()) { return false; }
        FString Name = In;
        if (!Name.StartsWith(Prefix)) { Name = FString(Prefix) + Name; }
        const int64 V = E->GetValueByNameString(Name);
        if (V == INDEX_NONE) { return false; }
        OutValue = V;
        return true;
    };

    FString S;
    int64 V = 0;
    if (Params->TryGetStringField(TEXT("address_x"), S) && !S.IsEmpty())
    {
        if (ParseEnumByName(StaticEnum<TextureAddress>(), TEXT("TA_"), S, V)) { Tex->AddressX = (TextureAddress)V; bChanged = true; }
        else { Data->SetStringField(TEXT("address_x_warning"), FString::Printf(TEXT("Unrecognized address_x '%s'"), *S)); }
    }
    if (Params->TryGetStringField(TEXT("address_y"), S) && !S.IsEmpty())
    {
        if (ParseEnumByName(StaticEnum<TextureAddress>(), TEXT("TA_"), S, V)) { Tex->AddressY = (TextureAddress)V; bChanged = true; }
        else { Data->SetStringField(TEXT("address_y_warning"), FString::Printf(TEXT("Unrecognized address_y '%s'"), *S)); }
    }
    if (Params->TryGetStringField(TEXT("filter"), S) && !S.IsEmpty())
    {
        if (ParseEnumByName(StaticEnum<TextureFilter>(), TEXT("TF_"), S, V)) { Tex->Filter = (TextureFilter)V; bChanged = true; }
        else { Data->SetStringField(TEXT("filter_warning"), FString::Printf(TEXT("Unrecognized filter '%s'"), *S)); }
    }
    if (Params->TryGetStringField(TEXT("mip_gen_settings"), S) && !S.IsEmpty())
    {
        if (ParseEnumByName(StaticEnum<TextureMipGenSettings>(), TEXT("TMGS_"), S, V)) { Tex->MipGenSettings = (TextureMipGenSettings)V; bChanged = true; }
        else { Data->SetStringField(TEXT("mip_gen_settings_warning"), FString::Printf(TEXT("Unrecognized mip_gen_settings '%s'"), *S)); }
    }

    double Num = 0.0;
    if (Params->TryGetNumberField(TEXT("lod_bias"), Num)) { Tex->LODBias = (int32)Num; bChanged = true; }
    if (Params->TryGetNumberField(TEXT("max_texture_size"), Num)) { Tex->MaxTextureSize = FMath::Max(0, (int32)Num); bChanged = true; }

    bool bFlip = false;
    if (Params->TryGetBoolField(TEXT("flip_green"), bFlip)) { Tex->bFlipGreenChannel = bFlip ? 1 : 0; bChanged = true; }

    if (bChanged)
    {
        Tex->PostEditChange();
        Tex->UpdateResource();
        Tex->MarkPackageDirty();
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Tex->GetOutermost(), Tex, SaveError);

    // Read-back verification of every setting, the sampler fields included.
    FillTextureInfo(Data, Tex);
    Data->SetBoolField(TEXT("changed"), bChanged);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_texture_settings"), SaveError);
}

// ---------------------------------------------------------------------------
// get_texture_info
// A UTexture2D: width/height/format/srgb/compression/lod_group, the sampler
// fields and the source. A UTexture2DArray: its size, slices and source textures.
// ---------------------------------------------------------------------------
// MCP:get_texture_info - Reads the metadata of a UTexture2D or UTexture2DArray asset without changing it; texture_class says which. A UTexture2D reports its size, pixel format, sRGB, compression, LOD group, sampler/LOD settings (address, filter, mip-gen, LOD bias, max size, flip-green), mip count and source; a UTexture2DArray reports its slice size as width/height, num_slices, format, num_mips and source_texture_count. texture_path accepts short or fully-qualified "/Game/MyGame/T_Example.T_Example" asset paths.
//   @param texture_path - Asset path of the texture or texture array (e.g. "/Game/MyGame/T_Example" or fully-qualified "/Game/MyGame/T_Example.T_Example")
//   @returns { texture_name: string, asset_path: string, texture_class: string, width: int, height: int, format: string, num_mips: int, has_source: bool, srgb?: bool, compression?: string, lod_group?: string, address_x?: string, address_y?: string, filter?: string, mip_gen_settings?: string, lod_bias?: int, max_texture_size?: int, flip_green?: bool, source_width?: int, source_height?: int, source_format?: string, num_slices?: int, source_texture_count?: int }

TSharedPtr<FJsonObject> HandleGetTextureInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString TexPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("texture_path"), TexPath)) { return Missing; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (UTexture2D* Tex = mcp_common::ResolveAsset<UTexture2D>(TexPath))
    {
        FillTextureInfo(Data, Tex);
        Data->SetStringField(TEXT("texture_class"), Tex->GetClass()->GetName());
        Data->SetNumberField(TEXT("num_mips"), Tex->GetNumMips());
        return mcp_common::MakeOkReply(Data);
    }

    UTexture2DArray* Arr = mcp_common::ResolveAsset<UTexture2DArray>(TexPath);
    if (!Arr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Texture not found (neither a UTexture2D nor a UTexture2DArray): %s"), *TexPath));
    }

    // A freshly built array may report no platform slices yet; its source textures are the slices then.
    int32 NumSlices = Arr->GetArraySize();
    if (NumSlices <= 0 && Arr->SourceTextures.Num() > 0)
    {
        NumSlices = Arr->SourceTextures.Num();
    }
    Data->SetStringField(TEXT("texture_name"), Arr->GetName());
    Data->SetStringField(TEXT("asset_path"), Arr->GetPathName());
    Data->SetStringField(TEXT("texture_class"), Arr->GetClass()->GetName());
    Data->SetNumberField(TEXT("width"), Arr->GetSizeX());
    Data->SetNumberField(TEXT("height"), Arr->GetSizeY());
    Data->SetNumberField(TEXT("num_slices"), NumSlices);
    Data->SetStringField(TEXT("format"), GetPixelFormatString(Arr->GetPixelFormat()));
    Data->SetNumberField(TEXT("num_mips"), Arr->GetNumMips());
    Data->SetBoolField(TEXT("has_source"), Arr->Source.IsValid());
    Data->SetNumberField(TEXT("source_texture_count"), Arr->SourceTextures.Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// create_texture_2d_array
// Creates a real persisted UTexture2DArray. Either loads the given
// source_texture_paths into SourceTextures, or generates slice_count small
// solid-color UTexture2D slices (alternating hues) into the array's package as
// intermediates. In both cases it then calls UpdateSourceFromSourceTextures(true)
// which builds the array Source (Source.Init(W,H,ArraySize,1,Format)) and
// UpdateResource(). Verified by reading GetSizeX/GetSizeY/GetArraySize back, and
// the cov test loads the saved asset back to confirm num_slices persisted.
// Idempotent: an existing asset is deleted + rebuilt so the payload is stable.
// ---------------------------------------------------------------------------
// MCP:create_texture_2d_array - Creates a persisted UTexture2DArray from slices (no external file import).  If ``source_texture_paths`` is given, those UTexture2D assets are loaded into the array's SourceTextures; otherwise ``slice_count`` small solid-color slices of ``size`` x ``size`` are generated. The array Source is then built via UTexture2DArray::UpdateSourceFromSourceTextures(true) and the asset is saved.  source_texture_paths is an optional array of UTexture2D asset paths used as the array's slices. Each must have a valid editor source. If omitted, slices are generated.  size is the per-slice pixel size (clamped 1..2048).
//   @param name - Name of the texture-array asset to create
//   @param package_path - Content folder; defaults to /Game/Textures
//   @param slice_count - Optional number of generated slices when source_texture_paths is omitted; clamped 1..64; defaults to 2
//   @param size - Optional per-slice pixel size; clamped 1..2048; defaults to 64
//   @param source_texture_paths - Optional array of UTexture2D asset paths used as slices
//   @returns { texture_array_name: string, asset_path: string, width: int, height: int, num_slices: int, format: string, created: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateTexture2DArray(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/Textures");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    int32 SliceCount = 2;
    int32 Size = 64;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("slice_count"), Tmp)) { SliceCount = FMath::Clamp((int32)Tmp, 1, 64); }
        if (Params->TryGetNumberField(TEXT("size"), Tmp))        { Size = FMath::Clamp((int32)Tmp, 1, 2048); }
    }

    const TArray<TSharedPtr<FJsonValue>>* SourcePathArr = nullptr;
    const bool bHaveSources = Params->TryGetArrayField(TEXT("source_texture_paths"), SourcePathArr)
                              && SourcePathArr && SourcePathArr->Num() > 0;

    FString ObjectPath;
    UPackage* Package = nullptr;
    UTexture2DArray* NewArray = nullptr;
    if (TSharedPtr<FJsonObject> Error = Texture_CreateFreshAsset(PackagePath, Name, TEXT("Failed to create UTexture2DArray object"),
        ObjectPath, Package, NewArray))
    {
        return Error;
    }

    // Resolve / generate the per-slice source textures.
    TArray<TObjectPtr<UTexture2D>> Slices;
    if (bHaveSources)
    {
        for (const TSharedPtr<FJsonValue>& V : *SourcePathArr)
        {
            if (!V.IsValid()) { continue; }
            const FString SlicePath = V->AsString();
            UTexture2D* SrcTex = mcp_common::ResolveAsset<UTexture2D>(SlicePath);
            if (!SrcTex)
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Source texture not found: %s"), *SlicePath));
            }
            if (!SrcTex->Source.IsValid())
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Source texture has no valid editor source (cannot build array slice): %s"), *SlicePath));
            }
            Slices.Add(SrcTex);
        }
    }
    else
    {
        // Generate SliceCount small solid-color slices with valid editor source.
        static const uint8 Palette[][3] =
        {
            {255,   0,   0}, {  0, 255,   0}, {  0,   0, 255}, {255, 255,   0},
            {255,   0, 255}, {  0, 255, 255}, {255, 255, 255}, {128, 128, 128},
        };
        const int32 PaletteNum = (int32)UE_ARRAY_COUNT(Palette);
        for (int32 i = 0; i < SliceCount; ++i)
        {
            const int32 PaletteIdx = i % PaletteNum;
            const FName SliceName = MakeUniqueObjectName(
                Package, UTexture2D::StaticClass(),
                *FString::Printf(TEXT("%s_Slice%d"), *Name, i));
            UTexture2D* Slice = CreateSolidColorSlice(
                Package, SliceName, Size,
                Palette[PaletteIdx][0], Palette[PaletteIdx][1], Palette[PaletteIdx][2], 255);
            if (!Slice)
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Failed to generate slice %d for texture array"), i));
            }
            Slices.Add(Slice);
        }
    }

    if (Slices.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No slices to build texture array from"));
    }

    NewArray->SourceTextures = Slices;

    // Build the array Source from the slices (single-mip). bCreatingNewTexture=true so
    // compression / mip / SRGB settings are taken from the first slice.
    const bool bBuilt = NewArray->UpdateSourceFromSourceTextures(/*bCreatingNewTexture*/ true);
    if (!bBuilt)
    {
        return mcp_common::MakeErrorReply(
            TEXT("UpdateSourceFromSourceTextures failed (slices not compatible / missing valid source)"));
    }

    NewArray->PostEditChange();
    NewArray->UpdateResource();

    FString SaveError;
    const bool bSaved = Texture_RegisterAndSave(Package, NewArray, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), NewArray->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    // Read-back verification straight off the built array.
    Data->SetNumberField(TEXT("slice_count"), NewArray->GetArraySize());
    Data->SetNumberField(TEXT("size_x"), NewArray->GetSizeX());
    Data->SetNumberField(TEXT("size_y"), NewArray->GetSizeY());
    Data->SetStringField(TEXT("format"), GetPixelFormatString(NewArray->GetPixelFormat()));
    Data->SetNumberField(TEXT("source_texture_count"), NewArray->SourceTextures.Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_texture_2d_array"), SaveError);
}

// ---------------------------------------------------------------------------
// create_render_target_2d_array
// Creates a real persisted UTextureRenderTarget2DArray via
// Init(SizeX, SizeY, Slices, Format). Verified by reading SizeX/SizeY/Slices back.
// Idempotent: an existing asset is deleted + rebuilt so the payload is stable.
// ---------------------------------------------------------------------------
// MCP:create_render_target_2d_array - Creates a persisted UTextureRenderTarget2DArray.  Wraps UTextureRenderTarget2DArray::Init(SizeX, SizeY, Slices, Format) and saves the asset to disk. Re-running deletes and rebuilds the asset deterministically.  format accepts one of "B8G8R8A8"/"BGRA8" (default), "FloatRGBA"/"RGBA16F", "RGBA32F", "R16F".
//   @param name - Name of the render-target-array asset to create
//   @param package_path - Content folder; defaults to /Game/Textures
//   @param size - Optional square edge in pixels; clamped 1..8192
//   @param slices - Optional slice count; clamped 1..512
//   @param format - Optional EPixelFormat name; defaults to BGRA8
//   @returns { render_target_name: string, asset_path: string, size_x: int, size_y: int, slices: int, format: string, created: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateRenderTarget2DArray(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/Textures");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    int32 SizeVal = 256;
    int32 SliceVal = 4;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("size"), Tmp))   { SizeVal = FMath::Clamp((int32)Tmp, 1, 8192); }
        if (Params->TryGetNumberField(TEXT("slices"), Tmp)) { SliceVal = FMath::Clamp((int32)Tmp, 1, 512); }
    }

    EPixelFormat Format = PF_B8G8R8A8;
    FString FormatWarning;
    Texture_ReadPixelFormat(Params, Format, FormatWarning);

    FString ObjectPath;
    UPackage* Package = nullptr;
    UTextureRenderTarget2DArray* NewRT = nullptr;
    if (TSharedPtr<FJsonObject> Error = Texture_CreateFreshAsset(PackagePath, Name, TEXT("Failed to create UTextureRenderTarget2DArray object"),
        ObjectPath, Package, NewRT))
    {
        return Error;
    }

    NewRT->Init((uint32)SizeVal, (uint32)SizeVal, (uint32)SliceVal, Format);

    FString SaveError;
    const bool bSaved = Texture_RegisterAndSave(Package, NewRT, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), NewRT->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    // Read-back verification straight off the asset.
    Data->SetNumberField(TEXT("size"), NewRT->SizeX);
    Data->SetNumberField(TEXT("size_x"), NewRT->SizeX);
    Data->SetNumberField(TEXT("size_y"), NewRT->SizeY);
    Data->SetNumberField(TEXT("slices"), NewRT->Slices);
    Data->SetStringField(TEXT("format"), GetPixelFormatString(NewRT->GetFormat()));
    if (!FormatWarning.IsEmpty())
    {
        Data->SetStringField(TEXT("format_warning"), FormatWarning);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_render_target_2d_array"), SaveError);
}

// ---------------------------------------------------------------------------
// create_render_target_cube
// Creates a real persisted UTextureRenderTargetCube. If a format is given uses
// Init(SizeX, Format); otherwise InitAutoFormat(SizeX). Verified by reading SizeX
// back. Idempotent: an existing asset is deleted + rebuilt so the payload is stable.
// ---------------------------------------------------------------------------
// MCP:create_render_target_cube - Creates a persisted UTextureRenderTargetCube (6-face cube render target).  Wraps UTextureRenderTargetCube::Init(SizeX, Format) when a format is given, or InitAutoFormat(SizeX) otherwise, and saves the asset to disk. Re-running deletes and rebuilds the asset deterministically.  format accepts one of "B8G8R8A8"/"BGRA8", "FloatRGBA"/"RGBA16F", "RGBA32F". When omitted the format is auto-derived (InitAutoFormat).
//   @param name - Name of the cube render-target asset to create
//   @param package_path - Content folder; defaults to /Game/Textures
//   @param size - Optional cube edge in pixels; clamped 1..8192
//   @param format - Optional EPixelFormat name; defaults to auto-derived
//   @returns { render_target_name: string, asset_path: string, size_x: int, format: string, created: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateRenderTargetCube(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/Textures");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    int32 SizeVal = 256;
    {
        double Tmp = 0.0;
        if (Params->TryGetNumberField(TEXT("size"), Tmp)) { SizeVal = FMath::Clamp((int32)Tmp, 1, 8192); }
    }

    EPixelFormat Format = PF_B8G8R8A8;
    FString FormatWarning;
    const bool bHaveFormat = Texture_ReadPixelFormat(Params, Format, FormatWarning);

    FString ObjectPath;
    UPackage* Package = nullptr;
    UTextureRenderTargetCube* NewRT = nullptr;
    if (TSharedPtr<FJsonObject> Error = Texture_CreateFreshAsset(PackagePath, Name, TEXT("Failed to create UTextureRenderTargetCube object"),
        ObjectPath, Package, NewRT))
    {
        return Error;
    }

    if (bHaveFormat)
    {
        NewRT->Init((uint32)SizeVal, Format);
    }
    else
    {
        NewRT->InitAutoFormat((uint32)SizeVal);
    }

    FString SaveError;
    const bool bSaved = Texture_RegisterAndSave(Package, NewRT, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("name"), NewRT->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    // Read-back verification straight off the asset (cube width == SizeX, 6 faces).
    Data->SetNumberField(TEXT("size"), NewRT->SizeX);
    Data->SetStringField(TEXT("format"), GetPixelFormatString(NewRT->GetFormat()));
    if (!FormatWarning.IsEmpty())
    {
        Data->SetStringField(TEXT("format_warning"), FormatWarning);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_render_target_cube"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_render_target_2d_array"), &HandleCreateRenderTarget2DArray },
    { TEXT("create_render_target_cube"), &HandleCreateRenderTargetCube },
    { TEXT("create_static_texture_from_render_target"), &HandleCreateStaticTextureFromRenderTarget },
    { TEXT("create_texture_2d_array"), &HandleCreateTexture2DArray },
    { TEXT("create_texture_2d_from_data"), &HandleCreateTexture2DFromData },
    { TEXT("get_texture_info"), &HandleGetTextureInfo },
    { TEXT("set_texture_settings"), &HandleSetTextureSettings },
};
} // namespace UEAgentTextureCommands
