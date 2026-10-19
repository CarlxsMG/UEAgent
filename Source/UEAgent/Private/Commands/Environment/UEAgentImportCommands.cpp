#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorReimportHandler.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "Sound/SoundWave.h"
#include "Modules/ModuleManager.h"
#include "Factories/Factory.h"
#include "Factories/TextureFactory.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

/**
 * External-file asset import commands: import a texture or a WAV file, reimport an asset from its
 * recorded source file, and read an asset's UAssetImportData.
 *
 * All imports use the LEGACY (non-Interchange) factory path, synchronous on the calling thread, so the
 * import does NOT re-enter the game-thread TaskGraph from inside the bridge's game-thread task (an
 * Interchange sync wait there would assert). RunLegacyImport below says why IAssetTools is bypassed too.
 */
namespace UEAgentImportCommands
{

// MCP-DOMAIN: Asset

namespace
{
    /**
     * Drive a legacy import via UFactory::FactoryCreateBinary (no IAssetTools).
     *
     * Why: IAssetTools::ImportAssetTasks pumps the game-thread task graph, which
     * trips a RecursionGuard assertion when called from inside the bridge's
     * FTSTicker callback (the risk the old generic importer hit at UE 5.7).
     * FactoryCreateBinary is inline on the calling thread, no task-graph
     * dependency.
     *
     * Returns the imported object path (e.g. /Game/Foo/Bar.Bar) or empty on failure.
     */
    FString RunLegacyImport(const FString& SourceFilePath, const FString& DestinationPath,
                            const FString& DestinationName, UFactory* Factory, UClass* TargetClass)
    {
        FString Error;
        const FString ImportedPath = mcp_common::FactoryCreateBinaryImport(
            SourceFilePath, DestinationPath, DestinationName, Factory, TargetClass, Error);

        if (ImportedPath.IsEmpty())
        {
            UE_LOG(LogTemp, Warning, TEXT("UEAgentImportCommands: import failed for '%s': %s"),
                   *SourceFilePath, *Error);
        }
        return ImportedPath;
    }

    // The params both file imports read, with their checks: null, or the error reply.
    TSharedPtr<FJsonObject> Import_ReadSourceAndDestination(const TSharedPtr<FJsonObject>& Params, FString& OutFilePath,
        FString& OutPackagePath, FString& OutAssetName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("file_path"), OutFilePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), OutPackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (!OutPackagePath.StartsWith(TEXT("/")))
        {
            return mcp_common::MakeErrorReply(TEXT("package_path must be a UE long path, e.g. /Game/Foo"));
        }
        if (!FPaths::FileExists(OutFilePath))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Source file does not exist: %s"), *OutFilePath));
        }

        Params->TryGetStringField(TEXT("asset_name"), OutAssetName);

        // replace_existing=false refuses before anything is touched when the destination already holds an
        // asset; the factory import itself always overwrites, so this is the only place the flag can act.
        bool bReplaceExisting = true;
        Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);
        if (!bReplaceExisting)
        {
            const FString FinalName = OutAssetName.IsEmpty() ? FPaths::GetBaseFilename(OutFilePath) : OutAssetName;
            const FString DestinationPackage = OutPackagePath / FinalName;
            if (UEditorAssetLibrary::DoesAssetExist(DestinationPackage))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("An asset already exists at %s and replace_existing is false; pass replace_existing=true to overwrite it or choose another asset_name."),
                    *DestinationPackage));
            }
        }
        return nullptr;
    }

    // Image extensions the legacy UTextureFactory path imports; anything else is refused before the factory runs.
    bool Import_IsTextureExtension(const FString& Extension)
    {
        static const TSet<FString> TextureExtensions = {
            TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("bmp"), TEXT("tga"),
            TEXT("dds"), TEXT("exr"), TEXT("hdr"), TEXT("tif"), TEXT("tiff"),
            TEXT("psd"), TEXT("pcx")
        };
        return TextureExtensions.Contains(Extension.ToLower());
    }
}

// MCP:import_texture_file - Import an external image file into a UTexture2D asset.  Uses the legacy UTextureFactory path inline (no Interchange, no IAssetTools::ImportAssets, which asserts when re-entered from the engine tick). Images only -- png/jpg/jpeg/bmp/tga/dds/exr/hdr/tif/tiff/psd/pcx: any other extension is refused before anything is touched (import_sound_file for .wav, import_landscape_heightmap for terrain, import_interchange_asset for FBX/glTF meshes). The new texture is saved to disk and verified by loading it back; the response reports the resulting asset_path, width and height.
//   @param file_path - Absolute path to the source image on disk; its existence is checked before any work starts.
//   @param package_path - Destination content folder, e.g. /Game/Imported. Must start with '/'.
//   @param asset_name - Optional destination asset name (default = source file stem).
//   @param replace_existing - Optional bool, default true: overwrite an asset already at the destination. false refuses with an error when the destination already holds an asset.
//   @returns { file_path: string, package_path: string, asset_path: string, asset_name: string, width: int, height: int, imported: bool }

TSharedPtr<FJsonObject> HandleImportTextureFile(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath, PackagePath, AssetName;
    if (TSharedPtr<FJsonObject> Error = Import_ReadSourceAndDestination(Params, FilePath, PackagePath, AssetName)) { return Error; }

    const FString Extension = FPaths::GetExtension(FilePath);
    if (!Import_IsTextureExtension(Extension))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("import_texture_file only imports images (png/jpg/jpeg/bmp/tga/dds/exr/hdr/tif/tiff/psd/pcx), got '.%s'. "
                 "Use import_sound_file for .wav, import_landscape_heightmap for terrain, import_interchange_asset for FBX/glTF meshes."),
            *Extension));
    }

    // Legacy texture factory: forces the inline import path (no Interchange re-entry).
    // FactoryCreateBinaryImport owns the AddToRoot / RemoveFromRoot lifecycle.
    UTextureFactory* TexFactory = NewObject<UTextureFactory>();
    TexFactory->bEditorImport = true;

    const FString ImportedPath = RunLegacyImport(FilePath, PackagePath, AssetName, TexFactory, UTexture2D::StaticClass());

    if (ImportedPath.IsEmpty())
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Texture import produced no asset from: %s"), *FilePath));

    // Verify by loading the resulting UTexture2D and reading its dimensions.
    UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ImportedPath);
    if (!Texture)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Imported object is not a UTexture2D: %s"), *ImportedPath));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("file_path"), FilePath);
    Data->SetStringField(TEXT("package_path"), PackagePath);
    Data->SetStringField(TEXT("asset_path"), ImportedPath);
    Data->SetStringField(TEXT("asset_name"), Texture->GetName());
    Data->SetNumberField(TEXT("width"), Texture->GetSizeX());
    Data->SetNumberField(TEXT("height"), Texture->GetSizeY());
    Data->SetBoolField(TEXT("imported"), true);
    return mcp_common::MakeOkReply(Data);
}

// MCP:import_sound_file - Import an external WAV file into a USoundWave asset.  Uses the legacy USoundFactory (AudioEditor module) path, no Interchange. The new sound wave is saved to disk and verified by loading it back; the response reports the resulting asset_path, duration (seconds) and num_channels.
//   @param file_path - Absolute path to the source .wav on disk.
//   @param package_path - Destination content folder, e.g. /Game/Imported.
//   @param asset_name - Optional destination asset name (default = source file stem).
//   @param replace_existing - Optional bool, default true: overwrite an asset already at the destination. false refuses with an error when the destination already holds an asset.
//   @returns { file_path: string, package_path: string, asset_path: string, asset_name: string, duration: float, num_channels: int, imported: bool }

TSharedPtr<FJsonObject> HandleImportSoundFile(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath, PackagePath, AssetName;
    if (TSharedPtr<FJsonObject> Error = Import_ReadSourceAndDestination(Params, FilePath, PackagePath, AssetName)) { return Error; }

    // The legacy WAV factory is USoundFactory in the 'AudioEditor' module
    // (UCLASS(MinimalAPI)). AudioEditor is always loaded in the editor (it is a
    // dependency of UnrealEd), so we resolve the class by reflected path at runtime
    // and instantiate via NewObject(Outer, Class) instead of linking AudioEditor.
    UClass* SoundFactoryClass = UClass::TryFindTypeSlow<UClass>(TEXT("/Script/AudioEditor.SoundFactory"));
    if (!SoundFactoryClass)
    {
        SoundFactoryClass = LoadClass<UFactory>(nullptr, TEXT("/Script/AudioEditor.SoundFactory"));
    }
    if (!SoundFactoryClass)
        return mcp_common::MakeErrorReply(TEXT("USoundFactory (AudioEditor) is unavailable in this editor build"));

    UFactory* SoundFactory = NewObject<UFactory>(GetTransientPackage(), SoundFactoryClass);
    if (!SoundFactory)
        return mcp_common::MakeErrorReply(TEXT("Failed to instantiate USoundFactory"));
    SoundFactory->bEditorImport = true;

    const FString ImportedPath = RunLegacyImport(FilePath, PackagePath, AssetName, SoundFactory, USoundWave::StaticClass());

    if (ImportedPath.IsEmpty())
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Sound import produced no asset from: %s"), *FilePath));

    // Verify by loading the resulting USoundWave and reading duration / channels.
    USoundWave* SoundWave = LoadObject<USoundWave>(nullptr, *ImportedPath);
    if (!SoundWave)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Imported object is not a USoundWave: %s"), *ImportedPath));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("file_path"), FilePath);
    Data->SetStringField(TEXT("package_path"), PackagePath);
    Data->SetStringField(TEXT("asset_path"), ImportedPath);
    Data->SetStringField(TEXT("asset_name"), SoundWave->GetName());
    Data->SetNumberField(TEXT("duration"), SoundWave->GetDuration());
    Data->SetNumberField(TEXT("num_channels"), SoundWave->NumChannels);
    Data->SetBoolField(TEXT("imported"), true);
    return mcp_common::MakeOkReply(Data);
}

// MCP:reimport_asset - Reimport an asset from its recorded source file via FReimportManager (legacy).  Works on any asset that carries UAssetImportData (e.g. a texture or sound that was imported from a file on disk). Runs fully automated (no dialogs). Returns can_reimport, reimported, and the recorded source_files.
//   @param asset_path - Object path of the asset, e.g. /Game/Imported/MyTex.MyTex.
//   @returns { asset_path: string, can_reimport: bool, reimported: bool, source_files: [string] }

TSharedPtr<FJsonObject> HandleReimportAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

    FReimportManager* ReimportManager = FReimportManager::Instance();
    if (!ReimportManager)
        return mcp_common::MakeErrorReply(TEXT("FReimportManager unavailable"));

    TArray<FString> SourceFilenames;
    const bool bCanReimport = ReimportManager->CanReimport(Asset, &SourceFilenames);

    // Legacy reimport: bAskForNewFileIfMissing=false, bShowNotification=false,
    // bAutomated=true so no dialogs ever appear headless. This routes through the
    // registered legacy reimport handlers (no Interchange sync wait).
    bool bReimported = false;
    if (bCanReimport)
    {
        bReimported = ReimportManager->Reimport(
            Asset,
            /*bAskForNewFileIfMissing=*/false,
            /*bShowNotification=*/false,
            /*PreferredReimportFile=*/TEXT(""),
            /*SpecifiedReimportHandler=*/nullptr,
            /*SourceFileIndex=*/INDEX_NONE,
            /*bForceNewFile=*/false,
            /*bAutomated=*/true);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("can_reimport"), bCanReimport);
    Data->SetBoolField(TEXT("reimported"), bReimported);
    Data->SetArrayField(TEXT("source_files"), mcp_common::StringsToJson(SourceFilenames));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_asset_import_data - Read the source-file record (UAssetImportData) of an asset loaded by path: the file(s) it was imported from, as stored and resolved to absolute paths, with each file's import timestamp. Works for textures, sound waves and any class with an AssetImportData property (static and skeletal meshes, animation sequences). An asset that has none is a success with has_import_data=false, empty source_file and source_files, source_file_count 0 and no import_time. source_file is the first resolved file and import_time the first file's timestamp. Read-only: the asset is loaded if needed, not changed. An asset that does not load is an error. Interchange pipelines and node data: get_interchange_asset_import_data; reimport from the recorded file: reimport_asset or reimport_interchange_asset.
//   @param asset_path - Asset path in object form, e.g. "/Game/Imported/T_Rock.T_Rock", or package form "/Game/Imported/T_Rock".
//   @returns { asset_path: string, asset_class: string, has_import_data: bool, source_file: string, source_files: [{relative_filename, resolved_filename, import_time}], source_file_count: int, import_time?: string }

TSharedPtr<FJsonObject> HandleGetAssetImportData(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found: %s"), *AssetPath));

    // Different asset classes expose AssetImportData as a typed UPROPERTY member.
    // Resolve it generically by reading the 'AssetImportData' object property so we
    // support textures, sound waves, static/skeletal meshes, etc.
    UAssetImportData* ImportData = nullptr;
    if (UTexture* Texture = Cast<UTexture>(Asset))
    {
        ImportData = Texture->AssetImportData;
    }
    else if (USoundWave* SoundWave = Cast<USoundWave>(Asset))
    {
        ImportData = SoundWave->AssetImportData;
    }
    else if (FObjectProperty* Prop = FindFProperty<FObjectProperty>(Asset->GetClass(), TEXT("AssetImportData")))
    {
        ImportData = Cast<UAssetImportData>(Prop->GetObjectPropertyValue_InContainer(Asset));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

    if (!ImportData)
    {
        Data->SetBoolField(TEXT("has_import_data"), false);
        Data->SetStringField(TEXT("source_file"), TEXT(""));
        Data->SetArrayField(TEXT("source_files"), TArray<TSharedPtr<FJsonValue>>());
        Data->SetNumberField(TEXT("source_file_count"), 0);
        return mcp_common::MakeOkReply(Data);
    }

    const FString FirstFilename = ImportData->GetFirstFilename();

    TArray<FString> Filenames;
    ImportData->ExtractFilenames(Filenames);

    TArray<TSharedPtr<FJsonValue>> SourceArray;
    const FAssetImportInfo& SourceInfo = ImportData->GetSourceData();
    for (int32 Index = 0; Index < SourceInfo.SourceFiles.Num(); ++Index)
    {
        const FAssetImportInfo::FSourceFile& SourceFile = SourceInfo.SourceFiles[Index];
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("relative_filename"), SourceFile.RelativeFilename);
        Entry->SetStringField(TEXT("resolved_filename"),
                              Filenames.IsValidIndex(Index) ? Filenames[Index] : SourceFile.RelativeFilename);
        Entry->SetStringField(TEXT("import_time"), SourceFile.Timestamp.ToString());
        SourceArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Data->SetBoolField(TEXT("has_import_data"), true);
    Data->SetStringField(TEXT("source_file"), FirstFilename);
    Data->SetArrayField(TEXT("source_files"), SourceArray);
    Data->SetNumberField(TEXT("source_file_count"), SourceInfo.SourceFiles.Num());
    if (SourceInfo.SourceFiles.Num() > 0)
    {
        Data->SetStringField(TEXT("import_time"), SourceInfo.SourceFiles[0].Timestamp.ToString());
    }
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("get_asset_import_data"), &HandleGetAssetImportData },
    { TEXT("import_sound_file"), &HandleImportSoundFile },
    { TEXT("import_texture_file"), &HandleImportTextureFile },
    { TEXT("reimport_asset"), &HandleReimportAsset },
};
} // namespace UEAgentImportCommands
