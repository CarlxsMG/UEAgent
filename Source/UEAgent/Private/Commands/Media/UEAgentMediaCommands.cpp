#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Media framework (module: MediaAssets, runtime)
#include "MediaPlayer.h"        // UMediaPlayer: PlayOnOpen, Loop, IsLooping/SetLooping
#include "MediaTexture.h"       // UMediaTexture: SetMediaPlayer/GetMediaPlayer, AutoClear
#include "FileMediaSource.h"    // UFileMediaSource: SetFilePath/GetFilePath/FilePath

// Engine / editor / packaging
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

#define LOCTEXT_NAMESPACE "UEAgentMedia"

/**
 * Media framework asset commands: create, link and inspect persisted /Game Media assets -- UMediaPlayer
 * (PlayOnOpen / Loop), UMediaTexture (renders a player's video track) and UFileMediaSource (references a
 * movie file path).
 *
 * Creation is idempotent (an existing asset is loaded and returned) and headless-safe: no modal factory
 * picker, and no real movie file is required. Mutations run in an FScopedTransaction after Modify() and are
 * saved through mcp_common::SaveAssetPackage.
 */
namespace UEAgentMediaCommands
{

// MCP-DOMAIN: Editor

namespace UEAgentMediaImpl
{
    // Where a create command puts its asset: the required name, under package_path (default /Game/Media).
    struct FMediaAssetTarget
    {
        FString Name;
        FString PackagePath = TEXT("/Game/Media");
        FString PackageName;
        FString ObjectPath;
    };

    // Reads package_path and the required name into Out; the missing-parameter reply when name is absent.
    TSharedPtr<FJsonObject> ReadAssetTarget(const TSharedPtr<FJsonObject>& Params, FMediaAssetTarget& Out)
    {
        Params->TryGetStringField(TEXT("package_path"), Out.PackagePath);
        Out.PackagePath.RemoveFromEnd(TEXT("/"));
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Out.Name))
        {
            return Missing;
        }
        Out.PackageName = FString::Printf(TEXT("%s/%s"), *Out.PackagePath, *Out.Name);
        Out.ObjectPath = FString::Printf(TEXT("%s.%s"), *Out.PackageName, *Out.Name);
        return nullptr;
    }

    // Registers a newly created asset and saves its package; false, with OutSaveError, when the save fails.
    bool RegisterAndSave(UPackage* Package, UObject* Asset, FString& OutSaveError)
    {
        FAssetRegistryModule::AssetCreated(Asset);
        Asset->MarkPackageDirty();
        return mcp_common::SaveAssetPackage(Package, Asset, OutSaveError);
    }

    // A reply payload that starts with the asset's name under NameField, its asset_path and whether this call
    // created it (an asset that already existed also reports already_exists).
    TSharedPtr<FJsonObject> NewAssetPayload(const TCHAR* NameField, const UObject* Asset, const FString& ObjectPath, bool bCreated)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(NameField, Asset->GetName());
        Data->SetStringField(TEXT("asset_path"), ObjectPath);
        Data->SetBoolField(TEXT("created"), bCreated);
        if (!bCreated)
        {
            Data->SetBoolField(TEXT("already_exists"), true);
        }
        return Data;
    }
}

// ---------------------------------------------------------------------------
// create_media_player
// NewObject<UMediaPlayer> -> save. Optionally also creates a sibling
// UMediaTexture ("<name>_T") and links it via UMediaTexture::SetMediaPlayer.
// PlayOnOpen / Loop are configurable; Loop is set via the public SetLooping().
// ---------------------------------------------------------------------------
// MCP:create_media_player - Creates (or loads, idempotently) a persisted UMediaPlayer asset
//   @param name - Name of the media player asset to create.
//   @param package_path - Target content package path where the asset is created (default /Game/Media)
//   @param create_output_texture - Also create + link a sibling media texture (default true)
//   @param play_on_open - Set the player's PlayOnOpen flag (default true)
//   @param loop - Set the player to loop via SetLooping (default false)
//   @returns { media_player_name, asset_path, created, already_exists, play_on_open, loop, texture_created, media_texture_path, saved, texture_saved, save_error, texture_save_error }
TSharedPtr<FJsonObject> HandleCreateMediaPlayer(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentMediaImpl::FMediaAssetTarget Target;
    if (TSharedPtr<FJsonObject> Missing = UEAgentMediaImpl::ReadAssetTarget(Params, Target)) { return Missing; }

    bool bCreateOutputTexture = true;
    Params->TryGetBoolField(TEXT("create_output_texture"), bCreateOutputTexture);

    bool bPlayOnOpen = true;
    Params->TryGetBoolField(TEXT("play_on_open"), bPlayOnOpen);

    bool bLoop = false;
    Params->TryGetBoolField(TEXT("loop"), bLoop);

    const FString TexName = FString::Printf(TEXT("%s_T"), *Target.Name);
    const FString TexPackageName = FString::Printf(TEXT("%s/%s"), *Target.PackagePath, *TexName);
    const FString TexObjectPath = FString::Printf(TEXT("%s.%s"), *TexPackageName, *TexName);

    // Idempotent: load+return a stable payload if the player already exists.
    if (UMediaPlayer* Existing = mcp_common::ResolveAsset<UMediaPlayer>(Target.ObjectPath))
    {
        // Determine whether the expected output texture exists and is linked.
        UMediaTexture* ExistingTex = mcp_common::ResolveAsset<UMediaTexture>(TexObjectPath);

        TSharedPtr<FJsonObject> Data = UEAgentMediaImpl::NewAssetPayload(TEXT("media_player_name"), Existing, Target.ObjectPath, false);
        Data->SetBoolField(TEXT("play_on_open"), Existing->PlayOnOpen);
        Data->SetBoolField(TEXT("loop"), Existing->IsLooping());
        Data->SetBoolField(TEXT("texture_created"), ExistingTex != nullptr);
        if (ExistingTex)
        {
            Data->SetStringField(TEXT("media_texture_path"), TexObjectPath);
        }
        return mcp_common::MakeOkReply(Data);
    }

    FString CreateError;
    UMediaPlayer* NewPlayer = mcp_common::NewAssetInPackage<UMediaPlayer>(Target.PackageName, Target.Name,
        TEXT("Failed to create UMediaPlayer object"), CreateError);
    if (!NewPlayer)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    {
        FScopedTransaction Transaction(LOCTEXT("CreateMediaPlayer", "Create Media Player"));
        NewPlayer->Modify();
        NewPlayer->PlayOnOpen = bPlayOnOpen;
        // Loop is protected; SetLooping is the public setter (returns false on
        // some players, so we do not rely on the return value).
        NewPlayer->SetLooping(bLoop);
    }

    FString SaveError;
    const bool bSaved = UEAgentMediaImpl::RegisterAndSave(NewPlayer->GetPackage(), NewPlayer, SaveError);

    // Optionally create + link a media texture so the player has a render sink.
    bool bTextureCreated = false;
    bool bTextureSaved = false;
    FString TexSaveError;
    if (bCreateOutputTexture)
    {
        // Idempotent on the texture too.
        UMediaTexture* Tex = mcp_common::ResolveAsset<UMediaTexture>(TexObjectPath);
        if (!Tex)
        {
            if (const FString AssetConflict = mcp_common::AssetNameConflict(*TexPackageName, *TexName, UMediaTexture::StaticClass()); !AssetConflict.IsEmpty())
            {
                return mcp_common::MakeErrorReply(AssetConflict);
            }
            UPackage* TexPackage = CreatePackage(*TexPackageName);
            if (TexPackage)
            {
                TexPackage->FullyLoad();
                Tex = NewObject<UMediaTexture>(
                    TexPackage, UMediaTexture::StaticClass(), *TexName,
                    RF_Public | RF_Standalone | RF_Transactional);
                if (Tex)
                {
                    {
                        FScopedTransaction Transaction(LOCTEXT("CreateMediaTextureForPlayer", "Create Media Texture"));
                        Tex->Modify();
                        Tex->AutoClear = true;
                        Tex->SetMediaPlayer(NewPlayer);
                        Tex->UpdateResource();
                    }
                    bTextureSaved = UEAgentMediaImpl::RegisterAndSave(TexPackage, Tex, TexSaveError);
                    bTextureCreated = true;
                }
            }
        }
        else
        {
            bTextureCreated = true; // already exists
        }
    }

    TSharedPtr<FJsonObject> Data = UEAgentMediaImpl::NewAssetPayload(TEXT("media_player_name"), NewPlayer, Target.ObjectPath, true);
    Data->SetBoolField(TEXT("play_on_open"), NewPlayer->PlayOnOpen);
    Data->SetBoolField(TEXT("loop"), NewPlayer->IsLooping());
    Data->SetBoolField(TEXT("texture_created"), bTextureCreated);
    if (bTextureCreated)
    {
        Data->SetStringField(TEXT("media_texture_path"), TexObjectPath);
        Data->SetBoolField(TEXT("texture_saved"), bTextureSaved);
    }
    if (bCreateOutputTexture && bTextureCreated && !bTextureSaved && !TexSaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("texture_save_error"), TexSaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_media_player"), SaveError);
}

// ---------------------------------------------------------------------------
// create_file_media_source
// NewObject<UFileMediaSource> -> SetFilePath(file_path) -> save. No real movie
// file is required; the asset simply references a path string.
// ---------------------------------------------------------------------------
// MCP:create_file_media_source - Creates (or loads, idempotently) a persisted UFileMediaSource asset
//   @param name - Name of the file media source asset to create.
//   @param file_path - The movie file path the source references (e.g.
//   @param package_path - Target content package path where the asset is created (default /Game/Media)
//   @returns { media_source_name, asset_path, created, already_exists, file_path, saved, save_error }
TSharedPtr<FJsonObject> HandleCreateFileMediaSource(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentMediaImpl::FMediaAssetTarget Target;
    if (TSharedPtr<FJsonObject> Missing = UEAgentMediaImpl::ReadAssetTarget(Params, Target)) { return Missing; }

    FString FilePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("file_path"), FilePath)) { return Missing; }

    // Idempotent: load+return the existing source's actual file path.
    if (UFileMediaSource* Existing = mcp_common::ResolveAsset<UFileMediaSource>(Target.ObjectPath))
    {
        TSharedPtr<FJsonObject> Data = UEAgentMediaImpl::NewAssetPayload(TEXT("media_source_name"), Existing, Target.ObjectPath, false);
        Data->SetStringField(TEXT("file_path"), Existing->GetFilePath());
        return mcp_common::MakeOkReply(Data);
    }

    FString CreateError;
    UFileMediaSource* NewSource = mcp_common::NewAssetInPackage<UFileMediaSource>(Target.PackageName, Target.Name,
        TEXT("Failed to create UFileMediaSource object"), CreateError);
    if (!NewSource)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    {
        FScopedTransaction Transaction(LOCTEXT("CreateFileMediaSource", "Create File Media Source"));
        NewSource->Modify();
        // SetFilePath auto-relativizes paths under /Content/Movies; otherwise
        // stores the path verbatim.
        NewSource->SetFilePath(FilePath);
    }

    FString SaveError;
    const bool bSaved = UEAgentMediaImpl::RegisterAndSave(NewSource->GetPackage(), NewSource, SaveError);

    TSharedPtr<FJsonObject> Data = UEAgentMediaImpl::NewAssetPayload(TEXT("media_source_name"), NewSource, Target.ObjectPath, true);
    // Read back the stored path (may differ from input if relativized).
    Data->SetStringField(TEXT("file_path"), NewSource->GetFilePath());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_file_media_source"), SaveError);
}

// ---------------------------------------------------------------------------
// create_media_texture
// NewObject<UMediaTexture> -> UpdateResource -> save. Not linked to any player.
// ---------------------------------------------------------------------------
// MCP:create_media_texture - Creates (or loads, idempotently) a persisted UMediaTexture asset
//   @param name - Name of the media texture asset to create.
//   @param package_path - Target content package path where the asset is created (default /Game/Media)
//   @returns { media_texture_name, asset_path, created, already_exists, has_media_player, media_player_path, saved, save_error }
TSharedPtr<FJsonObject> HandleCreateMediaTexture(const TSharedPtr<FJsonObject>& Params)
{
    UEAgentMediaImpl::FMediaAssetTarget Target;
    if (TSharedPtr<FJsonObject> Missing = UEAgentMediaImpl::ReadAssetTarget(Params, Target)) { return Missing; }

    // Idempotent.
    if (UMediaTexture* Existing = mcp_common::ResolveAsset<UMediaTexture>(Target.ObjectPath))
    {
        UMediaPlayer* LinkedPlayer = Existing->GetMediaPlayer();

        TSharedPtr<FJsonObject> Data = UEAgentMediaImpl::NewAssetPayload(TEXT("media_texture_name"), Existing, Target.ObjectPath, false);
        Data->SetBoolField(TEXT("has_media_player"), LinkedPlayer != nullptr);
        if (LinkedPlayer)
        {
            Data->SetStringField(TEXT("media_player_path"), LinkedPlayer->GetPathName());
        }
        return mcp_common::MakeOkReply(Data);
    }

    FString CreateError;
    UMediaTexture* NewTex = mcp_common::NewAssetInPackage<UMediaTexture>(Target.PackageName, Target.Name,
        TEXT("Failed to create UMediaTexture object"), CreateError);
    if (!NewTex)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    {
        FScopedTransaction Transaction(LOCTEXT("CreateMediaTexture", "Create Media Texture"));
        NewTex->Modify();
        NewTex->AutoClear = true;
        NewTex->UpdateResource();
    }

    FString SaveError;
    const bool bSaved = UEAgentMediaImpl::RegisterAndSave(NewTex->GetPackage(), NewTex, SaveError);

    TSharedPtr<FJsonObject> Data = UEAgentMediaImpl::NewAssetPayload(TEXT("media_texture_name"), NewTex, Target.ObjectPath, true);
    Data->SetBoolField(TEXT("has_media_player"), false);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_media_texture"), SaveError);
}

// ---------------------------------------------------------------------------
// link_media_texture_to_player
// UMediaTexture::SetMediaPlayer(player), verified by reading GetMediaPlayer back.
// Re-saves the texture package so the link persists.
// ---------------------------------------------------------------------------
// MCP:link_media_texture_to_player - Links a media texture to a media player via SetMediaPlayer
//   @param media_texture_path - Asset path of the UMediaTexture (e.g.
//   @param media_player_path - Asset path of the UMediaPlayer to bind.
//   @returns { media_texture_path, media_player_path, linked, saved, bound_player_path, save_error }
TSharedPtr<FJsonObject> HandleLinkMediaTextureToPlayer(const TSharedPtr<FJsonObject>& Params)
{
    FString TexPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("media_texture_path"), TexPath)) { return Missing; }

    FString PlayerPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("media_player_path"), PlayerPath)) { return Missing; }

    UMediaTexture* Tex = mcp_common::ResolveAsset<UMediaTexture>(TexPath);
    if (!Tex)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Media texture not found: %s"), *TexPath));
    }

    UMediaPlayer* Player = mcp_common::ResolveAsset<UMediaPlayer>(PlayerPath);
    if (!Player)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Media player not found: %s"), *PlayerPath));
    }

    {
        FScopedTransaction Transaction(LOCTEXT("LinkMediaTextureToPlayer", "Link Media Texture To Player"));
        Tex->Modify();
        Tex->SetMediaPlayer(Player);
        Tex->UpdateResource();
    }

    // Verify by readback.
    UMediaPlayer* Bound = Tex->GetMediaPlayer();
    const bool bLinked = (Bound == Player);

    // Persist the texture asset so the link survives.
    Tex->MarkPackageDirty();
    FString SaveError;
    bool bSaved = false;
    if (UPackage* Package = Tex->GetOutermost())
    {
        bSaved = mcp_common::SaveAssetPackage(Package, Tex, SaveError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("media_texture_path"), Tex->GetPathName());
    Data->SetStringField(TEXT("media_player_path"), Player->GetPathName());
    Data->SetBoolField(TEXT("linked"), bLinked);
    if (Bound)
    {
        Data->SetStringField(TEXT("bound_player_path"), Bound->GetPathName());
    }
    if (!bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("link_media_texture_to_player"));
}

// ---------------------------------------------------------------------------
// get_media_info  (read-only)
// Loads the asset, reports its class + type-specific fields:
//   UMediaPlayer       -> play_on_open, loop
//   UMediaTexture      -> has_media_player, media_player_path
//   UFileMediaSource   -> file_path
// ---------------------------------------------------------------------------
// MCP:get_media_info - Reads metadata for a Media asset (read-only)
//   @param asset_path - Asset path of the media asset (player, texture, or file
//   @returns { asset_path, asset_name, class, media_type, play_on_open, loop, has_media_player, media_player_path, auto_clear, file_path }
TSharedPtr<FJsonObject> HandleGetMediaInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UObject* Asset = mcp_common::ResolveAsset<UObject>(AssetPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Media asset not found: %s"), *AssetPath));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Data->SetStringField(TEXT("asset_name"), Asset->GetName());
    Data->SetStringField(TEXT("class"), Asset->GetClass()->GetName());

    if (UMediaPlayer* Player = Cast<UMediaPlayer>(Asset))
    {
        Data->SetStringField(TEXT("media_type"), TEXT("MediaPlayer"));
        Data->SetBoolField(TEXT("play_on_open"), Player->PlayOnOpen);
        Data->SetBoolField(TEXT("loop"), Player->IsLooping());
    }
    else if (UMediaTexture* Tex = Cast<UMediaTexture>(Asset))
    {
        Data->SetStringField(TEXT("media_type"), TEXT("MediaTexture"));
        UMediaPlayer* LinkedPlayer = Tex->GetMediaPlayer();
        Data->SetBoolField(TEXT("has_media_player"), LinkedPlayer != nullptr);
        if (LinkedPlayer)
        {
            Data->SetStringField(TEXT("media_player_path"), LinkedPlayer->GetPathName());
        }
        Data->SetBoolField(TEXT("auto_clear"), Tex->AutoClear);
    }
    else if (UFileMediaSource* Source = Cast<UFileMediaSource>(Asset))
    {
        Data->SetStringField(TEXT("media_type"), TEXT("FileMediaSource"));
        Data->SetStringField(TEXT("file_path"), Source->GetFilePath());
    }
    else
    {
        Data->SetStringField(TEXT("media_type"), TEXT("Other"));
    }

    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_file_media_source"), &HandleCreateFileMediaSource },
    { TEXT("create_media_player"), &HandleCreateMediaPlayer },
    { TEXT("create_media_texture"), &HandleCreateMediaTexture },
    { TEXT("get_media_info"), &HandleGetMediaInfo },
    { TEXT("link_media_texture_to_player"), &HandleLinkMediaTextureToPlayer },
};
} // namespace UEAgentMediaCommands

#undef LOCTEXT_NAMESPACE
