#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Dom/JsonObject.h"
#include "LevelSequence.h"

// Level sequence lookups and reply openings shared by the media command families.
namespace LevelSequenceUtils
{
    // The level sequences stored under PackagePath, sub-folders included, straight from the asset registry:
    // metadata only, nothing gets loaded.
    inline TArray<FAssetData> ListUnder(const FName PackagePath)
    {
        FARFilter Query;
        Query.PackagePaths.Add(PackagePath);
        Query.bRecursivePaths = true;
        Query.ClassPaths.Add(ULevelSequence::StaticClass()->GetClassPathName());

        TArray<FAssetData> Found;
        mcp_common::AssetRegistry().GetAssets(Query, Found);
        return Found;
    }

    // The first level sequence under /Game whose asset name matches SequenceName (FString equality, so case is
    // ignored), or null. Only the match is loaded.
    inline ULevelSequence* FindByName(const FString& SequenceName)
    {
        for (const FAssetData& Candidate : ListUnder(FName(TEXT("/Game"))))
        {
            if (Candidate.AssetName.ToString() == SequenceName)
            {
                return Cast<ULevelSequence>(Candidate.GetAsset());
            }
        }
        return nullptr;
    }

    // A reply payload that starts with sequence_name.
    inline TSharedPtr<FJsonObject> NewNamePayload(const FString& SequenceName)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sequence_name"), SequenceName);
        return Payload;
    }

    // A reply payload that starts with the sequence's sequence_path.
    inline TSharedPtr<FJsonObject> NewPathPayload(const ULevelSequence* Sequence)
    {
        TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
        Payload->SetStringField(TEXT("sequence_path"), Sequence->GetPathName());
        return Payload;
    }
}
