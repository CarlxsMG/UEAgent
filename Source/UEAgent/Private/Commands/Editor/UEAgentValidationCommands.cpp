// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Editor/McpMessageLog.h"
#include "Commands/Common/McpCommonUtils.h"

// Editor + world
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"   // ULevel::LoadAllExternalObjectsTag (validate_asset loads the asset as IsAssetValid does)

// Asset registry (enumerate assets / resolve a single asset path)
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/SoftObjectPath.h"

// DataValidation plugin (module: DataValidation, plugin: DataValidation)
// Reference: Engine/Plugins/Editor/DataValidation/Source/DataValidation/Public/EditorValidatorSubsystem.h
#include "EditorValidatorSubsystem.h"
#include "EditorValidatorBase.h"
// EDataValidationResult lives in CoreUObject UObjectGlobals; EDataValidationUsecase in Misc/DataValidation.h
#include "Misc/DataValidation.h"

// Map Check message log (module: MessageLog)
// Reference: Engine/Source/Developer/MessageLog/Public/{MessageLogModule,IMessageLogListing}.h
#include "MessageLogModule.h"
#include "IMessageLogListing.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/StringOutputDevice.h"

// Message-log token payloads (FUObjectToken lives in CoreUObject; FActorToken and
// the IMessageToken base live in Core/Logging/TokenizedMessage.h, included above).
#include "Misc/UObjectToken.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "UObject/Object.h"
#include "UObject/Class.h"

// Shared Message Log helpers (McpMessageLog.h), also used by get_message_log_entries.
namespace mcp_message_log
{

TArray<FString> ParseBaseLibraries(const TSharedPtr<FJsonObject>& Params)
{
    TArray<FString> BaseLibraries;

    const TArray<TSharedPtr<FJsonValue>>* BaseLibrariesJson = nullptr;
    if (Params.IsValid() && Params->TryGetArrayField(TEXT("base_libraries"), BaseLibrariesJson) && BaseLibrariesJson)
    {
        for (const TSharedPtr<FJsonValue>& Value : *BaseLibrariesJson)
        {
            FString Root;
            if (Value.IsValid() && Value->TryGetString(Root) && !Root.IsEmpty())
            {
                while (Root.RemoveFromEnd(TEXT("/")))
                {
                    // Trim EVERY trailing slash so "/Game/VendorPack//" still matches.
                }
                if (!Root.IsEmpty())
                {
                    BaseLibraries.AddUnique(Root);
                }
            }
        }
        // An EXPLICIT [] means "classify against nothing"; do not substitute
        // the default here, that would silently ignore the caller's opt-out.
        return BaseLibraries;
    }

    return mcp_common::GetDefaultReadonlyRoots();
}

FString ComputePathRoot(const FString& ObjectOrPackagePath, const TArray<FString>& BaseLibraries, bool& bOutIsBaseLibrary)
{
    bOutIsBaseLibrary = false;
    if (ObjectOrPackagePath.IsEmpty())
    {
        return FString();
    }

    // Object paths are "<package>.<object>" (and "<package>.<object>:<sub>"); the
    // root only ever lives in the package part.
    FString PackagePath = ObjectOrPackagePath;
    int32 DotIndex = INDEX_NONE;
    if (PackagePath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
    {
        PackagePath = PackagePath.Left(DotIndex);
    }

    // Longest matching base library wins ("/Game/VendorPack/Combat" beats "/Game/VendorPack").
    FString BestMatch;
    for (const FString& Root : BaseLibraries)
    {
        if (Root.IsEmpty())
        {
            continue;
        }
        FString Trimmed = Root;
        while (Trimmed.RemoveFromEnd(TEXT("/")))
        {
        }
        if (Trimmed.IsEmpty())
        {
            continue;
        }
        const bool bMatches = PackagePath.Equals(Trimmed, ESearchCase::IgnoreCase)
            || PackagePath.StartsWith(Trimmed + TEXT("/"), ESearchCase::IgnoreCase);
        if (bMatches && Trimmed.Len() > BestMatch.Len())
        {
            BestMatch = Trimmed;
        }
    }

    if (!BestMatch.IsEmpty())
    {
        bOutIsBaseLibrary = true;
        return BestMatch;
    }

    // No base library matched -> report the mount point ("/Game", "/Script",
    // "/Engine", "/MyPlugin"). Still DATA, never a verdict.
    if (PackagePath.StartsWith(TEXT("/")))
    {
        const int32 SecondSlash = PackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
        if (SecondSlash != INDEX_NONE)
        {
            return PackagePath.Left(SecondSlash);
        }
    }
    return PackagePath;
}

namespace
{
    // Fills the class_* triple for whichever object we decided describes the
    // message (the owning actor when there is one, else the token object itself).
    void AddClassFields_MessageLog(const UObject* Object, const TArray<FString>& BaseLibraries, const TSharedPtr<FJsonObject>& OutEntry)
    {
        if (!Object)
        {
            return;
        }
        const UClass* ObjectClass = Object->GetClass();
        if (!ObjectClass)
        {
            return;
        }
        const FString ClassAssetPath = ObjectClass->GetPathName();
        OutEntry->SetStringField(TEXT("class_asset_path"), ClassAssetPath);

        bool bIsBaseLibrary = false;
        const FString ClassRoot = ComputePathRoot(ClassAssetPath, BaseLibraries, bIsBaseLibrary);
        if (!ClassRoot.IsEmpty())
        {
            OutEntry->SetStringField(TEXT("class_root"), ClassRoot);
        }
        OutEntry->SetBoolField(TEXT("class_root_is_base_library"), bIsBaseLibrary);
    }
}

void TokenizedMessageToJson(const TSharedRef<FTokenizedMessage>& Message, const TArray<FString>& BaseLibraries, const TSharedPtr<FJsonObject>& OutEntry)
{
    if (!OutEntry.IsValid())
    {
        return;
    }

    // The engine attaches the subject of a Map Check / message-log entry as a
    // token: FUObjectToken for a live UObject (actor or component), FActorToken
    // for an actor referenced by path+GUID (World Partition, possibly unloaded).
    // The subject can also hang off the message LINK instead of the token list.
    FUObjectToken* ObjectToken = nullptr;
    FActorToken* ActorPathToken = nullptr;

    auto ConsiderToken = [&ObjectToken, &ActorPathToken](const TSharedPtr<IMessageToken>& Token)
    {
        if (!Token.IsValid())
        {
            return;
        }
        if (!ObjectToken && Token->GetType() == EMessageToken::Object)
        {
            ObjectToken = static_cast<FUObjectToken*>(Token.Get());
        }
        else if (!ActorPathToken && Token->GetType() == EMessageToken::Actor)
        {
            ActorPathToken = static_cast<FActorToken*>(Token.Get());
        }
    };

    for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
    {
        ConsiderToken(Token);
    }
    ConsiderToken(Message->GetMessageLink());

    if (ObjectToken)
    {
        OutEntry->SetStringField(TEXT("token_source"), TEXT("uobject_token"));

        // The token holds a WEAK pointer: the object may already be gone (GC, level
        // unloaded). The original path name is recorded at message-creation time and
        // survives that, so it is the honest fallback.
        UObject* TokenObject = ObjectToken->GetObject().Get();
        OutEntry->SetBoolField(TEXT("object_resolved"), TokenObject != nullptr);

        const FString ObjectPath = TokenObject
            ? TokenObject->GetPathName()
            : ObjectToken->GetOriginalObjectPathName();
        if (!ObjectPath.IsEmpty())
        {
            OutEntry->SetStringField(TEXT("object_path"), ObjectPath);
        }

        AActor* Actor = Cast<AActor>(TokenObject);
        if (UActorComponent* Component = Cast<UActorComponent>(TokenObject))
        {
            OutEntry->SetStringField(TEXT("component_name"), Component->GetName());
            Actor = Component->GetOwner();
        }

        if (Actor)
        {
            OutEntry->SetStringField(TEXT("actor_name"), Actor->GetName());
            OutEntry->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
            OutEntry->SetStringField(TEXT("actor_path"), Actor->GetPathName());
            if (const UClass* ActorClass = Actor->GetClass())
            {
                OutEntry->SetStringField(TEXT("actor_class"), ActorClass->GetName());
            }
            // class_* describes the ACTOR (the owner, for a component token): that is
            // what answers "which library does this warning come from".
            AddClassFields_MessageLog(Actor, BaseLibraries, OutEntry);
        }
        else if (TokenObject)
        {
            // Not an actor and not owned by one (an asset, a subsystem...): report its
            // own class rather than inventing actor_* fields.
            AddClassFields_MessageLog(TokenObject, BaseLibraries, OutEntry);
        }
        // else: stale weak pointer. object_path still carries the original path and
        // object_resolved is false -- we do NOT guess a class from a dead pointer.
        return;
    }

    if (ActorPathToken)
    {
        // Path + GUID only: no live object, so no class information exists to report.
        OutEntry->SetStringField(TEXT("token_source"), TEXT("actor_token"));
        OutEntry->SetBoolField(TEXT("object_resolved"), false);

        const FString ActorPath = ActorPathToken->GetActorPath();
        if (!ActorPath.IsEmpty())
        {
            OutEntry->SetStringField(TEXT("actor_path"), ActorPath);
            OutEntry->SetStringField(TEXT("object_path"), ActorPath);

            // Last segment of "<map>.<map>:PersistentLevel.<ActorName>" is the name.
            FString ActorName;
            if (ActorPath.Split(TEXT("."), nullptr, &ActorName, ESearchCase::IgnoreCase, ESearchDir::FromEnd)
                && !ActorName.IsEmpty())
            {
                OutEntry->SetStringField(TEXT("actor_name"), ActorName);
            }
        }
        OutEntry->SetStringField(TEXT("actor_guid"), ActorPathToken->GetActorGuid().ToString());
        return;
    }

    OutEntry->SetStringField(TEXT("token_source"), TEXT("none"));
    OutEntry->SetBoolField(TEXT("object_resolved"), false);
}

} // namespace mcp_message_log

/**
 * Asset validation and level audit commands.
 *
 * Wraps the engine DataValidation plugin (UEditorValidatorSubsystem) for asset-level validation, and
 * the editor's Map Check (run through GEditor->Exec) for the level audit.
 *
 * All commands run headless against the live editor. Where a registered validator genuinely cannot
 * run (e.g. no DataValidation subsystem in this context) they return an honest error rather than a
 * fake success.
 */
namespace UEAgentValidationCommands
{

// MCP-DOMAIN: Testing

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    // Resolve the DataValidation editor subsystem (UEditorSubsystem -> via GEditor).
    UEditorValidatorSubsystem* GetValidatorSubsystem(FString& OutError)
    {
        if (!GEditor)
        {
            OutError = TEXT("GEditor is not available (validation requires the editor)");
            return nullptr;
        }
        UEditorValidatorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorValidatorSubsystem>();
        if (!Subsystem)
        {
            OutError = TEXT("UEditorValidatorSubsystem is not available (is the DataValidation plugin enabled?)");
            return nullptr;
        }
        return Subsystem;
    }

    void TextArrayToJson(const TArray<FText>& In, TArray<TSharedPtr<FJsonValue>>& Out)
    {
        for (const FText& T : In)
        {
            Out.Add(MakeShared<FJsonValueString>(T.ToString()));
        }
    }

    const TCHAR* DataValidationResultToString(EDataValidationResult Result)
    {
        switch (Result)
        {
        case EDataValidationResult::Valid:        return TEXT("Valid");
        case EDataValidationResult::Invalid:      return TEXT("Invalid");
        case EDataValidationResult::NotValidated: return TEXT("NotValidated");
        default:                                  return TEXT("Unknown");
        }
    }

    /**
     * The subsystem's own per-asset validation, ValidateObjectInternal, which IsAssetValid runs without asking it for
     * statistics: with them it records, per validator class, how many assets that validator actually validated (a
     * result other than NotValidated). It is protected, so it is called through a pointer to member formed in this
     * derived type, which C++ allows for a protected member; the type is never instantiated.
     */
    struct FValidatorSubsystemAccess : public UEditorValidatorSubsystem
    {
        static EDataValidationResult ValidateWithStatistics(const UEditorValidatorSubsystem& Subsystem,
            const FAssetData& AssetData, UObject* Asset, FDataValidationContext& Context,
            TMap<FTopLevelAssetPath, FValidatorStatistics>& OutStatistics)
        {
            return (Subsystem.*(&FValidatorSubsystemAccess::ValidateObjectInternal))(AssetData, Asset, Context, &OutStatistics);
        }
    };

    // Resolve an FAssetData for an object path like "/Game/MyGame/M_Example" or
    // "/Game/MyGame/M_Example.M_Example". Returns whether a valid asset was found.
    bool ResolveAssetData(const FString& AssetPath, FAssetData& OutAssetData)
    {
        // A bare package path names its primary asset (mcp_common::FindAssetDataByPath).
        OutAssetData = mcp_common::FindAssetDataByPath(AssetPath);
        return OutAssetData.IsValid();
    }
}

// ---------------------------------------------------------------------------
// validate_asset
// ---------------------------------------------------------------------------

// MCP:validate_asset - Run the editor's enabled data validators (the Data Validation plugin, in its Script use case) on one asset and report the verdict, without changing or saving it (the asset gets loaded): validation_result is Valid, Invalid or NotValidated (no validator applies to that asset), is_valid is false only for Invalid, and errors and warnings carry the validators' messages. Use it to check one asset after editing it, e.g. before a commit or a cook; for a whole folder use validate_assets_in_path, to see which validators exist list_validators, and for a level's actors run_map_check. validators_run counts the validators that actually validated this asset (returned a result other than NotValidated), validators_applied names their classes, and validators_enabled counts every enabled validator; none run when the asset's own class-level check already finds it Invalid. An asset the Asset Registry cannot find, or the Data Validation plugin being off, is an error.
//   @param asset_path - Asset to validate, as an object path ("/Game/MyGame/M_Example.M_Example") or a package path ("/Game/MyGame/M_Example", which names its primary asset).
//   @returns { asset_path: string, resolved_object_path: string, validation_result: string, is_valid: bool, errors: [string], warnings: [string], error_count: number, warning_count: number, validators_run: number, validators_applied: [string], validators_enabled: number }

TSharedPtr<FJsonObject> HandleValidateAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    FString SubsysError;
    UEditorValidatorSubsystem* Subsystem = GetValidatorSubsystem(SubsysError);
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(SubsysError);
    }

    FAssetData AssetData;
    if (!ResolveAssetData(AssetPath, AssetData))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
    }

    // What IsAssetValid does -- load the asset, the class-level IsDataValid, then every enabled validator, collecting
    // errors and warnings without touching any FMessageLog tab -- plus the per-validator statistics, from which
    // validators_run counts the validators that actually validated this asset. An asset that does not load is
    // NotValidated, as IsAssetValid answers.
    TArray<FText> Errors;
    TArray<FText> Warnings;
    TMap<FTopLevelAssetPath, FValidatorStatistics> ValidatorStatistics;
    EDataValidationResult Result = EDataValidationResult::NotValidated;
    if (UObject* Asset = AssetData.GetAsset({ ULevel::LoadAllExternalObjectsTag }))
    {
        FDataValidationContext Context(false, EDataValidationUsecase::Script, {});
        Result = FValidatorSubsystemAccess::ValidateWithStatistics(*Subsystem, AssetData, Asset, Context, ValidatorStatistics);
        Context.SplitIssues(Warnings, Errors);
    }
    TArray<FString> ValidatorsApplied;
    for (const TPair<FTopLevelAssetPath, FValidatorStatistics>& Stat : ValidatorStatistics)
    {
        if (Stat.Value.AssetsValidated > 0)
        {
            ValidatorsApplied.Add(Stat.Key.GetAssetName().ToString());
        }
    }
    ValidatorsApplied.Sort();

    // How many validators are enabled, applicable to this asset or not.
    int32 ValidatorsEnabled = 0;
    Subsystem->ForEachEnabledValidator([&ValidatorsEnabled](UEditorValidatorBase* Validator) -> bool
    {
        if (Validator)
        {
            ++ValidatorsEnabled;
        }
        return true; // continue iteration
    });

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("asset_path"), AssetPath);
    Response->SetStringField(TEXT("resolved_object_path"), AssetData.GetObjectPathString());
    Response->SetStringField(TEXT("validation_result"), DataValidationResultToString(Result));
    // is_valid: NotValidated (no validator applied) is treated as valid (not invalid).
    Response->SetBoolField(TEXT("is_valid"), Result != EDataValidationResult::Invalid);

    TArray<TSharedPtr<FJsonValue>> ErrorsJson;
    TextArrayToJson(Errors, ErrorsJson);
    Response->SetArrayField(TEXT("errors"), ErrorsJson);

    TArray<TSharedPtr<FJsonValue>> WarningsJson;
    TextArrayToJson(Warnings, WarningsJson);
    Response->SetArrayField(TEXT("warnings"), WarningsJson);

    Response->SetNumberField(TEXT("error_count"), Errors.Num());
    Response->SetNumberField(TEXT("warning_count"), Warnings.Num());
    Response->SetNumberField(TEXT("validators_run"), ValidatorsApplied.Num());
    TArray<TSharedPtr<FJsonValue>> AppliedJson;
    for (const FString& Name : ValidatorsApplied)
    {
        AppliedJson.Add(MakeShared<FJsonValueString>(Name));
    }
    Response->SetArrayField(TEXT("validators_applied"), AppliedJson);
    Response->SetNumberField(TEXT("validators_enabled"), ValidatorsEnabled);

    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// validate_assets_in_path
// ---------------------------------------------------------------------------

// MCP:validate_assets_in_path - Run the enabled editor data validators (Script use case, as validate_asset does) on every asset under a content path, recursively by default, up to max_results assets; truncated tells when more were found. Loads each asset but changes and saves nothing. Per asset: validation_result (Valid, Invalid or NotValidated), is_valid (NotValidated counts as valid), errors and warnings; totals in num_valid, num_invalid and num_not_validated. A path with no assets returns zero counts, not an error. Use validate_asset for one asset, list_validators to see which validators run.
//   @param path - Package path to scan, e.g. "/Game/MyGame".
//   @param recursive - Whether to scan sub-paths (optional, default true)
//   @param max_results - Maximum number of assets to validate (optional, default 1000, valid 1-100000)
//   @returns { path: string, recursive: bool, num_assets: number, num_valid: number, num_invalid: number, num_not_validated: number, total_found: number, truncated: bool, results: [{ asset_path: string, asset_name: string, asset_class: string, validation_result: string, is_valid: bool, error_count: number, warning_count: number, errors: [string], warnings: [string] }] }

TSharedPtr<FJsonObject> HandleValidateAssetsInPath(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("path"), Path)) { return Missing; }

    bool bRecursive = true;
    Params->TryGetBoolField(TEXT("recursive"), bRecursive);

    int32 MaxResults = 1000;
    {
        double MaxResultsD = 0.0;
        if (Params->TryGetNumberField(TEXT("max_results"), MaxResultsD))
        {
            MaxResults = static_cast<int32>(MaxResultsD);
        }
    }
    if (MaxResults <= 0 || MaxResults > 100000)
    {
        return mcp_common::MakeErrorReply(TEXT("max_results must be between 1 and 100000"));
    }

    FString SubsysError;
    UEditorValidatorSubsystem* Subsystem = GetValidatorSubsystem(SubsysError);
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(SubsysError);
    }

    // Enumerate assets under the package path.
    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    // Normalize: strip trailing slash.
    FString PackagePath = Path;
    PackagePath.RemoveFromEnd(TEXT("/"));

    TArray<FAssetData> Assets;
    AssetRegistry.GetAssetsByPath(FName(*PackagePath), Assets, bRecursive, /*bIncludeOnlyOnDiskAssets=*/false);

    int32 NumValid = 0;
    int32 NumInvalid = 0;
    int32 NumNotValidated = 0;
    int32 NumChecked = 0;

    TArray<TSharedPtr<FJsonValue>> ResultsJson;
    for (const FAssetData& AssetData : Assets)
    {
        if (NumChecked >= MaxResults)
        {
            break;
        }
        // Skip redirectors / world-external actor stubs that are not validatable assets here.
        if (!AssetData.IsValid())
        {
            continue;
        }

        TArray<FText> Errors;
        TArray<FText> Warnings;
        const EDataValidationResult Result =
            Subsystem->IsAssetValid(AssetData, Errors, Warnings, EDataValidationUsecase::Script);

        ++NumChecked;
        if (Result == EDataValidationResult::Invalid)
        {
            ++NumInvalid;
        }
        else if (Result == EDataValidationResult::Valid)
        {
            ++NumValid;
        }
        else
        {
            ++NumNotValidated;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("asset_path"), AssetData.GetObjectPathString());
        Entry->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
        Entry->SetStringField(TEXT("asset_class"), AssetData.AssetClassPath.ToString());
        Entry->SetStringField(TEXT("validation_result"), DataValidationResultToString(Result));
        Entry->SetBoolField(TEXT("is_valid"), Result != EDataValidationResult::Invalid);
        Entry->SetNumberField(TEXT("error_count"), Errors.Num());
        Entry->SetNumberField(TEXT("warning_count"), Warnings.Num());

        TArray<TSharedPtr<FJsonValue>> ErrorsJson;
        TextArrayToJson(Errors, ErrorsJson);
        Entry->SetArrayField(TEXT("errors"), ErrorsJson);
        TArray<TSharedPtr<FJsonValue>> WarningsJson;
        TextArrayToJson(Warnings, WarningsJson);
        Entry->SetArrayField(TEXT("warnings"), WarningsJson);

        ResultsJson.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("path"), PackagePath);
    Response->SetBoolField(TEXT("recursive"), bRecursive);
    Response->SetNumberField(TEXT("num_assets"), NumChecked);
    Response->SetNumberField(TEXT("num_valid"), NumValid);
    Response->SetNumberField(TEXT("num_invalid"), NumInvalid);
    Response->SetNumberField(TEXT("num_not_validated"), NumNotValidated);
    Response->SetNumberField(TEXT("total_found"), Assets.Num());
    Response->SetBoolField(TEXT("truncated"), Assets.Num() > NumChecked);
    Response->SetArrayField(TEXT("results"), ResultsJson);

    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// list_validators
// ---------------------------------------------------------------------------

// MCP:list_validators - List the editor data validators the DataValidation subsystem has enabled (ForEachEnabledValidator): class_name, class_path and enabled for each, plus validator_count. Read-only; errors when the DataValidation plugin subsystem is unavailable. validate_asset runs them on one asset.
//   @returns { validators: [{ class_name: string, class_path: string, enabled: bool }], validator_count: number }

TSharedPtr<FJsonObject> HandleListValidators(const TSharedPtr<FJsonObject>& Params)
{
    FString SubsysError;
    UEditorValidatorSubsystem* Subsystem = GetValidatorSubsystem(SubsysError);
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(SubsysError);
    }

    TArray<TSharedPtr<FJsonValue>> ValidatorsJson;
    int32 Count = 0;
    Subsystem->ForEachEnabledValidator([&](UEditorValidatorBase* Validator) -> bool
    {
        if (Validator)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("class_name"), Validator->GetClass()->GetName());
            Entry->SetStringField(TEXT("class_path"), Validator->GetClass()->GetPathName());
            Entry->SetBoolField(TEXT("enabled"), Validator->IsEnabled());
            ValidatorsJson.Add(MakeShared<FJsonValueObject>(Entry));
            ++Count;
        }
        return true; // continue iteration
    });

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetArrayField(TEXT("validators"), ValidatorsJson);
    Response->SetNumberField(TEXT("validator_count"), Count);

    return mcp_common::MakeOkReply(Response);
}

// ---------------------------------------------------------------------------
// run_map_check
// ---------------------------------------------------------------------------

// MCP:run_map_check - Run the editor's Map Check (the MAP CHECK console command) on the level open in the editor and report error, warning and info counts plus up to 200 messages, each resolved back to the actor or component the engine attached to it. Use it to find level problems such as missing meshes, actors with invalid settings or lighting that needs a rebuild; for assets use validate_asset or validate_assets_in_path, for Blueprints check_blueprint_errors. It changes no actor or asset and saves nothing, but it replaces the contents of the editor's Map Check message log (no window opens); get_message_log_entries rereads it. Severity numbers: 1 error, 2 performance warning, 3 warning, 4 info. The counts cover every message and messages_truncated says when the list was capped. Editor world only, not a PIE session; success is false when the editor did not run the command, and the counts are then stale.
//   @param check_deprecated_only - When true, runs MAP CHECKDEP: only the deprecated-class checks (optional, default false)
//   @param base_libraries [array] - Package roots, e.g. ["/Game/Marketplace"], to classify each message's class against (optional, default: the project's Read Only Content Roots). An explicit [] disables the classification. Reported as data in class_root/class_root_is_base_library; the tool never judges which root is "upstream".
//   @returns { world: string, map_check_ran: bool, check_deprecated_only: bool, base_libraries: [string], error_count: number, warning_count: number, info_count: number, total_messages: number, messages_truncated: bool, messages: [{ severity: number, text: string, token_source: string, object_path: string, object_resolved: bool, actor_name: string, actor_label: string, actor_path: string, actor_class: string, actor_guid: string, component_name: string, class_asset_path: string, class_root: string, class_root_is_base_library: bool }] }

TSharedPtr<FJsonObject> HandleRunMapCheck(const TSharedPtr<FJsonObject>& Params)
{
    if (!GEditor)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor is not available (Map Check requires the editor)"));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available for Map Check"))) { return NoWorld; }

    bool bCheckDeprecatedOnly = false;
    Params->TryGetBoolField(TEXT("check_deprecated_only"), bCheckDeprecatedOnly);

    // Optional package roots the per-message class path is classified against.
    const TArray<FString> BaseLibraries = mcp_message_log::ParseBaseLibraries(Params);

    // UEditorEngine::Map_Check is private, but the public Exec interface routes
    // "MAP CHECK" / "MAP CHECKDEP" to it (see EditorServer.cpp HandleMapCommand).
    // "DONTDISPLAYDIALOG" maps to EMapCheckNotification::DontDisplayResults so we
    // don't pop a window during headless operation; the log is cleared by default
    // so we read only this run's results. Results land in the "MapCheck"
    // FMessageLog listing, which we read below for per-severity counts.
    const TCHAR* MapCheckCmd = bCheckDeprecatedOnly
        ? TEXT("MAP CHECKDEP DONTDISPLAYDIALOG")
        : TEXT("MAP CHECK DONTDISPLAYDIALOG");

    FStringOutputDevice OutputCapture;
    const bool bExecOk = GEditor->Exec(World, MapCheckCmd, OutputCapture);

    // Read the MapCheck message log listing for accurate per-severity counts.
    int32 ErrorCount = 0;
    int32 WarningCount = 0;
    int32 InfoCount = 0;
    int32 TotalMessages = 0;
    TArray<TSharedPtr<FJsonValue>> MessagesJson;

    FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
    TSharedRef<IMessageLogListing> Listing = MessageLogModule.GetLogListing(TEXT("MapCheck"));
    const TArray<TSharedRef<FTokenizedMessage>>& Messages = Listing->GetFilteredMessages();

    for (const TSharedRef<FTokenizedMessage>& Message : Messages)
    {
        ++TotalMessages;
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        // EMessageSeverity: Error=1, PerformanceWarning=2, Warning=3, Info=4
        if (Severity == EMessageSeverity::Error)
        {
            ++ErrorCount;
        }
        else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
        {
            ++WarningCount;
        }
        else
        {
            ++InfoCount;
        }

        // Capture the first 200 message texts for context.
        if (MessagesJson.Num() < 200)
        {
            TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
            MsgObj->SetNumberField(TEXT("severity"), static_cast<int32>(Severity));
            MsgObj->SetStringField(TEXT("text"), Message->ToText().ToString());
            // The engine attaches the offending actor/component to the message as
            // an FUObjectToken; it is reported, so a warning says where it came from.
            mcp_message_log::TokenizedMessageToJson(Message, BaseLibraries, MsgObj);
            MessagesJson.Add(MakeShared<FJsonValueObject>(MsgObj));
        }
    }

    TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("world"), World->GetName());
    Response->SetBoolField(TEXT("map_check_ran"), bExecOk);
    Response->SetBoolField(TEXT("check_deprecated_only"), bCheckDeprecatedOnly);
    Response->SetArrayField(TEXT("base_libraries"), mcp_common::StringsToJson(BaseLibraries));
    Response->SetNumberField(TEXT("error_count"), ErrorCount);
    Response->SetNumberField(TEXT("warning_count"), WarningCount);
    Response->SetNumberField(TEXT("info_count"), InfoCount);
    Response->SetNumberField(TEXT("total_messages"), TotalMessages);
    // `messages` is capped at 200 entries while the *_count fields cover the
    // whole listing; say so instead of letting the caller assume messages.Num() is all.
    Response->SetBoolField(TEXT("messages_truncated"), TotalMessages > MessagesJson.Num());
    Response->SetArrayField(TEXT("messages"), MessagesJson);

    // Honesty -- the counts above are a read-back of the MapCheck listing after
    // the Exec, but if the Exec was NOT handled the listing still holds whatever was
    // there before and reporting success would be a lie. bExecOk is the real outcome.
    mcp_common::AddVerificationEvidence(
        Response,
        bExecOk ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("message_log_listing_readback"),
        FString::Printf(TEXT("GEditor->Exec(\"%s\") returned %s; %d message(s) read back from the 'MapCheck' listing"),
            MapCheckCmd, bExecOk ? TEXT("true") : TEXT("false"), TotalMessages));

    return mcp_common::MakeOutcomeReply(
        Response,
        bExecOk,
        bExecOk ? FString() : FString::Printf(
            TEXT("GEditor->Exec did not handle '%s' -- the MapCheck listing was NOT refreshed by this call; the counts below are stale"), MapCheckCmd));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("list_validators"), &HandleListValidators },
    { TEXT("run_map_check"), &HandleRunMapCheck },
    { TEXT("validate_asset"), &HandleValidateAsset },
    { TEXT("validate_assets_in_path"), &HandleValidateAssetsInPath },
};
} // namespace UEAgentValidationCommands
