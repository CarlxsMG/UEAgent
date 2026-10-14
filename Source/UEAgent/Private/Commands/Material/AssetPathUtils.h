#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

// Asset loading shared by the material commands.
namespace AssetPathUtils
{
	/** LoadObject on Path, else LoadObject again on Path as FSoftObjectPath writes it. */
	template <typename AssetType>
	AssetType* LoadOrLoadSoftPathString(const FString& Path)
	{
		if (AssetType* Loaded = LoadObject<AssetType>(nullptr, *Path))
		{
			return Loaded;
		}
		const FSoftObjectPath SoftPath(Path);
		return LoadObject<AssetType>(nullptr, *SoftPath.ToString());
	}
}
