// Copyright (c) 2026 CarlxsMG.

// Asset-path checks and asset loads the animation command families share.
#pragma once

#include "CoreMinimal.h"
#include "Commands/Common/McpCommonUtils.h"
#include "EditorAssetLibrary.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"

namespace AnimAssetUtils
{
    /** Path without its trailing slashes. */
    inline FString StripTrailingSlashes(FString Path)
    {
        while (Path.EndsWith(TEXT("/")))
        {
            Path = Path.LeftChop(1);
        }
        return Path;
    }

    /** The reply refusing PackagePath when it is not a long package path ("/Game/..."), else null. */
    inline TSharedPtr<FJsonObject> LongPackagePathError(const FString& PackagePath)
    {
        if (!PackagePath.StartsWith(TEXT("/")))
        {
            return mcp_common::MakeErrorReply(TEXT("package_path must be a UE long path, e.g. /Game/Foo"));
        }
        return nullptr;
    }

    /**
     * OutFolder is PackagePath without one trailing slash and OutFullPath is OutFolder/Name: the reply refusing
     * OutFullPath when it cannot name an asset, else null.
     */
    inline TSharedPtr<FJsonObject> JoinAssetPath(const FString& PackagePath, const FString& Name, FString& OutFolder,
        FString& OutFullPath)
    {
        OutFolder = PackagePath;
        OutFolder.RemoveFromEnd(TEXT("/"));
        OutFullPath = FString::Printf(TEXT("%s/%s"), *OutFolder, *Name);
        return mcp_common::InvalidPackageNameReply(OutFullPath);
    }

    /** The T at the object path Path; null when Path is empty or names no T. */
    template <typename T>
    T* LoadAsset(const FString& Path)
    {
        if (Path.IsEmpty())
        {
            return nullptr;
        }
        return Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *Path));
    }

    /** LoadAsset<T>(Path) into OutAsset: null when it loads, else the "<TypeLabel> not found: <Path>" reply. */
    template <typename T>
    TSharedPtr<FJsonObject> LoadOrError(const FString& Path, const TCHAR* TypeLabel, T*& OutAsset)
    {
        OutAsset = AnimAssetUtils::LoadAsset<T>(Path);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s not found: %s"), TypeLabel, *Path));
        }
        return nullptr;
    }

    /**
     * mcp_common::LoadAssetOrSoftPath<T>(Path) into OutAsset: null when it loads, else the "Could not load <Label>: <Path>"
     * reply.
     */
    template <typename T>
    TSharedPtr<FJsonObject> LoadFlexibleOrError(const FString& Path, const TCHAR* Label, T*& OutAsset)
    {
        OutAsset = mcp_common::LoadAssetOrSoftPath<T>(Path);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load %s: %s"), Label, *Path));
        }
        return nullptr;
    }

    /** The T already on disk at FullPath, or null. */
    template <typename T>
    T* FindExisting(const FString& FullPath)
    {
        return UEditorAssetLibrary::DoesAssetExist(FullPath)
            ? Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *FullPath))
            : nullptr;
    }
}
