#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/DateTime.h"

/**
 * Defensive backup utility: before a destructive operation, duplicate an asset into a timestamped
 * folder under /Game/MCP_Backups/ (the "backup_asset" command).
 */
namespace UEAgentBackupManager
{

// MCP-DOMAIN: Editor

/** Content root for all backups. */
FString GetBackupRoot()
{
    return TEXT("/Game/MCP_Backups");
}

/**
 * Duplicate the asset at AssetPath into /Game/MCP_Backups/<timestamp>/.
 * Returns the backup package path on success, or an empty string (with
 * OutError set) on failure. Accepts package paths or full object paths.
 */
FString BackupAsset(const FString& AssetPath, FString& OutError)
{
    OutError.Empty();

    // Accept a full object path (/Game/Dir/Name.Name) or a package path (/Game/Dir/Name).
    FString PackagePath = AssetPath;
    if (AssetPath.Contains(TEXT(".")))
    {
        PackagePath = FPackageName::ObjectPathToPackageName(AssetPath);
    }

    if (!UEditorAssetLibrary::DoesAssetExist(PackagePath))
    {
        OutError = FString::Printf(TEXT("Asset does not exist: %s"), *PackagePath);
        return FString();
    }

    const FString AssetName = FPackageName::GetShortName(PackagePath);
    const FString TimeStamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
    const FString DestPath = FString::Printf(TEXT("%s/%s/%s"), *GetBackupRoot(), *TimeStamp, *AssetName);

    UObject* Duplicated = UEditorAssetLibrary::DuplicateAsset(PackagePath, DestPath);
    if (!Duplicated)
    {
        OutError = FString::Printf(TEXT("Failed to duplicate %s -> %s"), *PackagePath, *DestPath);
        return FString();
    }

    UEditorAssetLibrary::SaveAsset(DestPath, /*bOnlyIfIsDirty=*/false);
    return DestPath;
}

// MCP:backup_asset - Duplicate an asset into a timestamped /Game/MCP_Backups/ folder.  Call this before destructive operations (delete, reparent, overwrite). Accepts a package path or a full object path.
//   @param path - Source asset path (package path like /Game/Foo/Foo or full object path .Foo)
//   @returns { source: string, backup_path: string }

TSharedPtr<FJsonObject> HandleBackupAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Error;
    const FString BackupPath = BackupAsset(AssetPath, Error);
    if (BackupPath.IsEmpty())
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source"), AssetPath);
    Result->SetStringField(TEXT("backup_path"), BackupPath);
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("backup_asset"), &HandleBackupAsset },
};
} // namespace UEAgentBackupManager
