#pragma once

#include "CoreMinimal.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Curves/RichCurve.h"                               // FRichCurve / FKeyHandle
#include "Engine/World.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"        // UDataLayerAsset
#include "WorldPartition/DataLayer/DataLayerManager.h"      // UDataLayerManager::GetDataLayerManager
#include "WorldPartition/DataLayer/DataLayerType.h"         // EDataLayerType

// Reply pieces and lookups shared by the asset authoring, data layer and World Partition command families.
namespace DataAuthoringUtils
{
    /** created=false, already_exists=true, saved=true: a create command that found its asset already there. */
    inline void AddAlreadyExistsFlags(const TSharedPtr<FJsonObject>& Reply)
    {
        Reply->SetBoolField(TEXT("created"), false);
        Reply->SetBoolField(TEXT("already_exists"), true);
        Reply->SetBoolField(TEXT("saved"), true);
    }

    /** created, already_exists=false, success and saved = bSaved, message "<AssetNoun> created and saved to disk" or "... but save failed: <SaveError>". */
    inline void AddCreatedOutcome(const TSharedPtr<FJsonObject>& Reply, bool bSaved, const TCHAR* AssetNoun, const FString& SaveError)
    {
        Reply->SetBoolField(TEXT("created"), true);
        Reply->SetBoolField(TEXT("already_exists"), false);
        Reply->SetBoolField(TEXT("success"), bSaved);
        Reply->SetBoolField(TEXT("saved"), bSaved);
        Reply->SetStringField(TEXT("message"), bSaved
            ? FString::Printf(TEXT("%s created and saved to disk"), AssetNoun)
            : FString::Printf(TEXT("%s created but save failed: %s"), AssetNoun, *SaveError));
    }

    /** The key KeyIndex (bByIndex) or KeyTime picks, invalid when none sits at KeyTime; an error naming KeyHolder for a bad index. */
    inline TSharedPtr<FJsonObject> FindCurveKey(const FRichCurve& Curve, bool bByIndex, int32 KeyIndex, double KeyTime,
        const TCHAR* KeyHolder, FKeyHandle& OutHandle)
    {
        if (!bByIndex)
        {
            OutHandle = Curve.FindKey(static_cast<float>(KeyTime));
            return nullptr;
        }
        if (KeyIndex < 0 || KeyIndex >= Curve.GetNumKeys())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("key_index %d out of range (%s has %d key(s))"), KeyIndex, KeyHolder, Curve.GetNumKeys()));
        }
        // FIndexedCurve::GetKeyHandle(int32) is protected: the index goes to the key's time, the time to its handle.
        OutHandle = Curve.FindKey(Curve.GetConstRefOfKeys()[KeyIndex].Time);
        return nullptr;
    }

    /** EDataLayerType by its enumerator name; "Unknown" for anything else. */
    inline const TCHAR* DataLayerTypeToString(EDataLayerType InType)
    {
        switch (InType)
        {
            case EDataLayerType::Runtime: return TEXT("Runtime");
            case EDataLayerType::Editor:  return TEXT("Editor");
            case EDataLayerType::Unknown: return TEXT("Unknown");
            default:                      return TEXT("Unknown");
        }
    }

    /** The UDataLayerAsset at AssetPath, or null. */
    inline UDataLayerAsset* LoadDataLayerAsset(const FString& AssetPath)
    {
        return Cast<UDataLayerAsset>(StaticLoadObject(UDataLayerAsset::StaticClass(), nullptr, *AssetPath));
    }

    /** The data layer asset at AssetPath; the error reply when none loads there. */
    inline TSharedPtr<FJsonObject> OpenDataLayerAsset(const FString& AssetPath, UDataLayerAsset*& OutAsset)
    {
        OutAsset = LoadDataLayerAsset(AssetPath);
        if (OutAsset == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Data layer asset not found: %s"), *AssetPath));
        }
        return nullptr;
    }

    /** The editor world and its data layer manager; an error without an editor world, NoManagerError without World Partition. */
    inline TSharedPtr<FJsonObject> OpenEditorDataLayerManager(const TCHAR* NoManagerError, UWorld*& OutWorld, UDataLayerManager*& OutManager)
    {
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(OutWorld))
        {
            return NoWorld;
        }
        OutManager = UDataLayerManager::GetDataLayerManager(OutWorld);
        if (OutManager == nullptr)
        {
            return mcp_common::MakeErrorReply(NoManagerError);
        }
        return nullptr;
    }

    /** { world, is_world_partition, <NotRunFlag>: false, success: true }: a World Partition build reported on, not run. */
    inline TSharedPtr<FJsonObject> WorldPartitionGateReply(const UWorld& World, const TCHAR* NotRunFlag)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("world"), World.GetName());
        Reply->SetBoolField(TEXT("is_world_partition"), World.IsPartitionedWorld());
        Reply->SetBoolField(NotRunFlag, false);
        Reply->SetBoolField(TEXT("success"), true);
        return Reply;
    }
}
