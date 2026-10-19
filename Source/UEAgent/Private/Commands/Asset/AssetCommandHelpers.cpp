#include "Commands/Asset/AssetCommandHelpers.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"      // FPackageName::ObjectPathToPackageName / GetShortName
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"   // GetObjectsWithOuter
#include "UObject/UObjectGlobals.h"

namespace AssetCommandHelpers
{
    TSharedPtr<FJsonObject> EmptyParamError(const TCHAR* ParamName)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parameter '%s' cannot be empty"), ParamName));
    }

    TSharedPtr<FJsonObject> RangeError(const TCHAR* ParamName, int32 Value, int32 Lowest, int32 Highest)
    {
        if (Value >= Lowest && Value <= Highest)
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Parameter '%s' must be between %d and %d"), ParamName, Lowest, Highest));
    }

    TSharedPtr<FJsonObject> AssetNotFoundError(const FString& Name)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found: %s"), *Name));
    }

    bool FindAssetByExactName(const FString& Name, const UClass* ExactClass, FNamedAsset& Out)
    {
        IAssetRegistry& Registry = mcp_common::AssetRegistry();
        TArray<FAssetData> Candidates;
        Registry.GetAssetsByPackageName(FName(*Name), Candidates);
        if (Candidates.Num() == 0)
        {
            // A filtered scan of /Game, never GetAllAssets().
            FARFilter Scope;
            Scope.PackagePaths.Add(TEXT("/Game"));
            Scope.bRecursivePaths = true;
            if (ExactClass != nullptr)
            {
                Scope.ClassPaths.Add(ExactClass->GetClassPathName());
            }
            TArray<FAssetData> Scanned;
            Registry.GetAssets(Scope, Scanned);
            Candidates = Scanned.FilterByPredicate([&Name](const FAssetData& Candidate)
            {
                return Candidate.AssetName.ToString() == Name;
            });
            if (Candidates.Num() > 1)
            {
                for (const FAssetData& Candidate : Candidates)
                {
                    Out.AmbiguousMatches.Add(MakeShared<FJsonValueString>(Candidate.GetObjectPathString()));
                }
            }
        }
        if (Candidates.Num() == 0)
        {
            return false;
        }
        Out.Asset = Candidates[0];
        return true;
    }

    void AppendAmbiguity(const TSharedPtr<FJsonObject>& Reply, const TArray<TSharedPtr<FJsonValue>>& Candidates)
    {
        if (Candidates.Num() > 0)
        {
            Reply->SetArrayField(TEXT("ambiguous_matches"), Candidates);
        }
    }

    UObject* FindDottedPathInWorld(const FString& Path, UWorld* World)
    {
        if (!World)
        {
            return nullptr;
        }

        TArray<FString> Segments;
        Path.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
        if (Segments.Num() == 0)
        {
            return nullptr;
        }

        UObject* Resolved = mcp_common::FindActorByName(World, Segments[0]);
        for (int32 i = 1; Resolved && i < Segments.Num(); ++i)
        {
            UObject* Next = nullptr;
            if (AActor* OwnerActor = Cast<AActor>(Resolved))
            {
                TArray<UActorComponent*> Components;
                OwnerActor->GetComponents(Components);
                for (UActorComponent* Component : Components)
                {
                    if (Component && Component->GetName() == Segments[i])
                    {
                        Next = Component;
                        break;
                    }
                }
            }
            if (!Next)
            {
                Next = StaticFindObjectFast(UObject::StaticClass(), Resolved, *Segments[i]);
            }
            Resolved = Next;
        }
        return Resolved;
    }

    TArray<TSharedPtr<FJsonValue>> CompilerMessagesToJson(const FCompilerResultsLog& Results)
    {
        TArray<TSharedPtr<FJsonValue>> Messages;
        for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
        {
            const TCHAR* SeverityStr = TEXT("info");
            switch (Message->GetSeverity())
            {
                case EMessageSeverity::Error:              SeverityStr = TEXT("error"); break;
                case EMessageSeverity::PerformanceWarning:
                case EMessageSeverity::Warning:            SeverityStr = TEXT("warning"); break;
                default:                                   SeverityStr = TEXT("info"); break;
            }
            TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
            MsgObj->SetStringField(TEXT("severity"), SeverityStr);
            MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
            Messages.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
        return Messages;
    }

    UObject* NativeClassDefault(UClass* Class)
    {
        if (!Class || Class->ClassGeneratedBy != nullptr)
        {
            return nullptr;
        }
        if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            return nullptr;
        }
        UObject* CDO = Class->GetDefaultObject(/*bCreateIfNeeded=*/false);
        if (!CDO || !CDO->HasAllFlags(RF_ClassDefaultObject) || !IsValid(CDO))
        {
            return nullptr;
        }
        return CDO;
    }

    FString MapPathTooLongForExternalPackages(const FString& MapPackageName)
    {
        // The object path is at least two characters longer than the package name, so its margin covering the actor
        // packages' overhead means the world's object paths always reach the limit first.
        static_assert(mcp_common::ObjectPathSubobjectMargin + 2 > MapExternalPackageOverhead,
            "the object path limit must keep a map's actor and actor folder packages under NAME_SIZE");
        return mcp_common::InvalidPackageName(MapPackageName);
    }

    int32 DeepestSubobjectPathDepth(const UObject* Asset, FString* OutDeepestSuffix)
    {
        if (OutDeepestSuffix)
        {
            OutDeepestSuffix->Reset();
        }
        if (!Asset)
        {
            return 0;
        }
        const FString AssetPath = Asset->GetPathName();
        TArray<UObject*> Inner;
        GetObjectsWithOuter(Asset, Inner, EGetObjectsFlags::IncludeNestedObjects, RF_NoFlags, EInternalObjectFlags::Garbage);
        int32 Deepest = 0;
        for (const UObject* Object : Inner)
        {
            const FString Path = Object->GetPathName();
            if (Path.Len() - AssetPath.Len() > Deepest)
            {
                Deepest = Path.Len() - AssetPath.Len();
                if (OutDeepestSuffix)
                {
                    *OutDeepestSuffix = Path.RightChop(AssetPath.Len());
                }
            }
        }
        return Deepest;
    }

    FString CopiedObjectPathsTooLong(const UObject* Asset, const FString& NewObjectPath)
    {
        if (FString Invalid = mcp_common::InvalidPackageName(NewObjectPath); !Invalid.IsEmpty())
        {
            return Invalid;
        }
        if (!Asset)
        {
            return FString();
        }
        FString DeepestSuffix;
        const int32 Depth = DeepestSubobjectPathDepth(Asset, &DeepestSuffix);
        // The destination's object path: its package, a '.', and the object name given or else the last path segment.
        const FString PackageName = FPackageName::ObjectPathToPackageName(NewObjectPath);
        int32 Dot = INDEX_NONE;
        const FString AssetName = NewObjectPath.FindChar(TEXT('.'), Dot) ? NewObjectPath.Mid(Dot + 1)
            : FPackageName::GetShortName(PackageName);
        const int32 ObjectPathLength = PackageName.Len() + 1 + AssetName.Len();
        if (ObjectPathLength + Depth < NAME_SIZE)
        {
            return FString();
        }
        return FString::Printf(TEXT("Invalid destination '%s...': its object path (package.asset) is %d characters long, and the ")
            TEXT("deepest object inside '%s' reaches %d characters past the asset's object path ('%s...'), which the copy or rename ")
            TEXT("keeps under the new path: %d characters, and an FName holds at most %d (the engine aborts the editor on a longer ")
            TEXT("one). For this asset the longest destination object path accepted is %d. Use a shorter folder or asset name."),
            *NewObjectPath.Left(64), ObjectPathLength, *Asset->GetName(), Depth, *DeepestSuffix.Left(64), ObjectPathLength + Depth,
            NAME_SIZE - 1, NAME_SIZE - 1 - Depth);
    }
}
