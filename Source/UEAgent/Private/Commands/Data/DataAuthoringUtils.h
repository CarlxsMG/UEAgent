#pragma once

#include "CoreMinimal.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/AssetData.h"                        // FAssetData (mcp_common::FindAssetDataByPath)
#include "Curves/RichCurve.h"                               // FRichCurve / FKeyHandle
#include "Engine/World.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"        // UDataLayerAsset
#include "WorldPartition/DataLayer/DataLayerManager.h"      // UDataLayerManager::GetDataLayerManager
#include "WorldPartition/DataLayer/DataLayerType.h"         // EDataLayerType

// Reply pieces and lookups shared by the asset authoring, data layer and World Partition command families.
namespace DataAuthoringUtils
{
    /**
     * created=false, already_exists=true, saved=false and the message "<AssetNoun> already exists; ...": a create
     * command that found its asset already there and returns it unchanged. Nothing was written, so nothing is saved.
     */
    inline void AddAlreadyExistsFlags(const TSharedPtr<FJsonObject>& Reply, const TCHAR* AssetNoun)
    {
        Reply->SetBoolField(TEXT("created"), false);
        Reply->SetBoolField(TEXT("already_exists"), true);
        Reply->SetBoolField(TEXT("saved"), false);
        Reply->SetStringField(TEXT("message"), FString::Printf(
            TEXT("%s already exists; returned the existing asset unchanged, nothing saved"), AssetNoun));
    }

    /**
     * The error of a create command whose target path already holds an asset that is not exactly of class Wanted
     * (another class, or a subclass with its own behavior, such as a composite table): it is not the asset asked
     * for, so it is neither returned as already existing nor replaced. Found is that asset when it loaded as a
     * Wanted; otherwise its class is read from the asset registry.
     */
    inline TSharedPtr<FJsonObject> OtherClassAtPathReply(const FString& Path, const UObject* Found, const UClass* Wanted)
    {
        FString FoundClass;
        if (Found != nullptr)
        {
            FoundClass = Found->GetClass()->GetName();
        }
        else
        {
            const FAssetData OnDisk = mcp_common::FindAssetDataByPath(Path);
            if (!OnDisk.IsValid() || OnDisk.AssetClassPath == Wanted->GetClassPathName())
            {
                // The registry knows no other class there: the asset is there but does not load.
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("An asset already exists at '%s' but does not load as a %s; nothing was created or saved."),
                    *Path, *Wanted->GetName()));
            }
            FoundClass = OnDisk.AssetClassPath.GetAssetName().ToString();
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' already exists as a %s, so a %s cannot be created there; nothing was created or saved. ")
            TEXT("Choose another name, or delete or rename the existing asset first."),
            *Path, *FoundClass, *Wanted->GetName()));
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
