#include "Commands/Asset/AssetCommandHelpers.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/ActorComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Package.h"
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

    /** Fills Data with { asset_path, asset_class, deleted: false } and returns the note refusing to delete Asset. */
    FString RefuseWrongType(const TSharedPtr<FJsonObject>& Data, const FString& AssetPath, const UObject& Asset,
        const FString& ExpectedClassName)
    {
        Data->SetStringField(TEXT("asset_path"), AssetPath);
        Data->SetStringField(TEXT("asset_class"), Asset.GetClass()->GetName());
        Data->SetBoolField(TEXT("deleted"), false);
        return FString::Printf(TEXT("asset is not a %s (got %s) - wrong type, refusing to delete"),
            *ExpectedClassName, *Asset.GetClass()->GetName());
    }

    TSharedPtr<FJsonObject> WrongTypeRefusal(const TCHAR* Command, const FString& AssetPath, const UObject& Asset,
        const FString& ExpectedClassName)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        const FString Note = RefuseWrongType(Data, AssetPath, Asset, ExpectedClassName);
        Data->SetStringField(TEXT("note"), Note);
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(TEXT("%s: %s at %s"), Command, *Note, *AssetPath));
    }

    TSharedPtr<FJsonObject> DeleteOutcomeReply(const TCHAR* Command, const TSharedPtr<FJsonObject>& Data, bool bDeleted,
        const FString& AssetPath)
    {
        return mcp_common::MakeOutcomeReply(Data, bDeleted, bDeleted ? FString() : FString::Printf(
            TEXT("%s: the asset at %s was not deleted (the editor's delete returned false: a Play-in-Editor session ")
            TEXT("running, an asset the Asset Registry does not list, or a file that cannot be deleted; the Output Log ")
            TEXT("names which)"), Command, *AssetPath));
    }

    TSharedPtr<FJsonObject> DeleteTypedAsset(const TCHAR* Command, const FString& AssetPath, const UClass* ExpectedClass)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
        if (!Asset)
        {
            // Nothing there to delete: the idempotent no-op.
            Data->SetStringField(TEXT("asset_path"), AssetPath);
            Data->SetStringField(TEXT("asset_class"), TEXT(""));
            Data->SetBoolField(TEXT("deleted"), false);
            Data->SetBoolField(TEXT("already_existed"), false);
            Data->SetStringField(TEXT("note"), TEXT("asset_path did not resolve to a loadable UObject; nothing to remove"));
            return mcp_common::MakeOkReply(Data);
        }
        if (!Asset->IsA(ExpectedClass))
        {
            return WrongTypeRefusal(Command, AssetPath, *Asset, ExpectedClass->GetName());
        }

        // Read before the delete: ForceDeleteObjects can collect garbage, which frees the asset.
        const FString AssetClassName = Asset->GetClass()->GetName();
        const FString PackageName = Asset->GetOutermost()->GetName();
        const bool bDeleted = UEditorAssetLibrary::DeleteAsset(AssetPath);

        Data->SetStringField(TEXT("asset_path"), AssetPath);
        Data->SetStringField(TEXT("asset_class"), AssetClassName);
        Data->SetStringField(TEXT("package_name"), PackageName);
        Data->SetBoolField(TEXT("deleted"), bDeleted);
        Data->SetBoolField(TEXT("already_existed"), true);
        return DeleteOutcomeReply(Command, Data, bDeleted, AssetPath);
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
}
