#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/UI/UMGWidgetUtils.h"

#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Engine/CollisionProfile.h"
// UMG audits: walk WidgetTrees and resolve dotted style paths.
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "EditorAssetLibrary.h"
#include "Internationalization/Text.h"                    // FTextInspector::ShouldGatherForLocalization
#include "Commands/Common/McpJsonPropertyApply.h"         // ResolvePropertyPath
#include "Commands/Common/UEAgentGraphUtils.h"      // ResolveClass for target_class
// Accessibility: the widget types that are inspected, and their layout.
#include "Components/TextBlock.h"
#include "Components/Border.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SlateWrapperTypes.h"   // ESlateAccessibleBehavior
#include "Engine/DataTable.h"
#include "Engine/DataAsset.h"
#include "Styling/SlateColor.h"   // IsColorSpecified: an FSlateColor may carry no color of its own

#include "UObject/UnrealType.h"
#include "UObject/Script.h"
// UClass::TryFindTypeSlow (Class.h) + FindFirstObject/LoadObject/EFindFirstObjectOptions
// (UObjectGlobals.h) back the capture_pie_state class_filter resolver below. Both are the
// idiom already used by this plugin's own class resolver
// (Private/Commands/Asset/UEAgentAssetCommands.cpp ResolveClassFilterToUClass);
// included explicitly here rather than relying on transitive pulls.
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"

/**
 * Project-wide, read-only audit commands.
 *
 * The Blueprint, collision, reference and replication audits and capture_pie_state write their full JSON
 * report to Saved/MCPAudits/ and return only {path, summary}, so the LLM client isn't forced to ingest a
 * potentially large report just to know "did this pass"; read_mcp_audit_file(path) reads a report back on
 * demand. The UMG audits return their entries inline.
 */
namespace UEAgentAuditCommands
{

// MCP-DOMAIN: Audit

namespace
{
    // Writes Report to Saved/MCPAudits/<Prefix>_<UTC timestamp>.json and puts in OutPath the full absolute
    // path written (what read_mcp_audit_file expects): the error reply when the file cannot be written.
    TSharedPtr<FJsonObject> AU_WriteReport(const FString& Prefix, const TSharedPtr<FJsonObject>& Report, FString& OutPath)
    {
        const FString AuditDir = FPaths::ProjectSavedDir() / TEXT("MCPAudits");
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        if (!PlatformFile.DirectoryExists(*AuditDir))
        {
            PlatformFile.CreateDirectoryTree(*AuditDir);
        }
        const FString FileName = FString::Printf(TEXT("%s_%s.json"), *Prefix, *FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S")));
        const FString FilePath = AuditDir / FileName;

        FString JsonString;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonString);
        FJsonSerializer::Serialize(Report.ToSharedRef(), Writer);
        if (!FFileHelper::SaveStringToFile(JsonString, *FilePath))
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to write audit report to Saved/MCPAudits/"));
        }
        OutPath = FPaths::ConvertRelativePathToFull(FilePath);
        return nullptr;
    }

    // A report opened with its audit_type and the UTC time it was taken.
    TSharedPtr<FJsonObject> AU_StartReport(const TCHAR* AuditType)
    {
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        Report->SetStringField(TEXT("audit_type"), AuditType);
        Report->SetStringField(TEXT("timestamp_utc"), FDateTime::UtcNow().ToString());
        return Report;
    }

    int32 AU_GetMaxResults(const TSharedPtr<FJsonObject>& Params, int32 DefaultValue)
    {
        double D = DefaultValue;
        Params->TryGetNumberField(TEXT("max_results"), D);
        return FMath::Clamp(static_cast<int32>(D), 1, 100000);
    }

    FString AU_GetFolderPath(const TSharedPtr<FJsonObject>& Params)
    {
        FString FolderPath = TEXT("/Game");
        Params->TryGetStringField(TEXT("folder_path"), FolderPath);
        return FolderPath;
    }

    bool AU_GetRecursive(const TSharedPtr<FJsonObject>& Params)
    {
        bool bRecursive = true;
        Params->TryGetBoolField(TEXT("recursive"), bRecursive);
        return bRecursive;
    }

    // Reads the optional "dependency_type" param ("hard"|"soft"|"all", case-insensitive) and
    // normalizes it to lowercase. Defaults to "all" -- unknown values also fall back to "all"
    // rather than erroring, since this only scopes an audit (not a mutating call).
    FString AU_GetDependencyType(const TSharedPtr<FJsonObject>& Params)
    {
        FString DependencyType = TEXT("all");
        Params->TryGetStringField(TEXT("dependency_type"), DependencyType);
        if (DependencyType.Equals(TEXT("hard"), ESearchCase::IgnoreCase)) { return TEXT("hard"); }
        if (DependencyType.Equals(TEXT("soft"), ESearchCase::IgnoreCase)) { return TEXT("soft"); }
        return TEXT("all");
    }

    // Builds the FDependencyQuery matching a normalized AU_GetDependencyType() result.
    // "all" (the default) is the default overload's behavior exactly: a
    // default-constructed FDependencyQuery (NoRequirements) applies no Hard/Soft filter at all.
    UE::AssetRegistry::FDependencyQuery AU_BuildDependencyQuery(const FString& DependencyType)
    {
        if (DependencyType == TEXT("hard"))
        {
            return UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
        }
        if (DependencyType == TEXT("soft"))
        {
            return UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Soft);
        }
        return UE::AssetRegistry::FDependencyQuery();
    }

    // Resolves one capture_pie_state `class_filter` entry to a UClass. Mirrors this
    // plugin's existing short-name->UClass idiom (ResolveClassFilterToUClass in
    // UEAgentAssetCommands.cpp): object/script path via LoadObject first, then
    // UClass::TryFindTypeSlow, then FindFirstObject as a last resort.
    // Extra step for PIE: a Blueprint's runtime class is "<Name>_C", so a caller who
    // passes the Blueprint asset name ("BP_MyCharacter") also gets a hit. Returns nullptr
    // when nothing resolves -- the caller MUST error, never silently drop the filter
    // (a dropped filter would return the whole world, the opposite of the ask).
    UClass* AU_ResolveActorClass(const FString& ClassName)
    {
        if (ClassName.IsEmpty())
        {
            return nullptr;
        }

        if (ClassName.Contains(TEXT(".")) || ClassName.StartsWith(TEXT("/Script/")))
        {
            // FindObject BEFORE LoadObject. This runs while PIE is live, and any class that
            // actually has instances in the PIE world is already loaded -- so the already-loaded
            // lookup is both sufficient and free of a synchronous package load mid-session.
            // (See UEAgentBlueprintCommands.cpp: FindObject<UClass>(nullptr, ...)
            // only resolves FULL paths, which is exactly this branch's input.)
            if (UClass* AlreadyLoaded = FindObject<UClass>(nullptr, *ClassName))
            {
                return AlreadyLoaded;
            }
            if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassName))
            {
                return Loaded;
            }
        }

        if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ClassName))
        {
            return Found;
        }

        const FString GeneratedName = ClassName + TEXT("_C");
        if (UClass* FoundGenerated = UClass::TryFindTypeSlow<UClass>(GeneratedName))
        {
            return FoundGenerated;
        }

        if (UClass* First = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::None))
        {
            return First;
        }
        return FindFirstObject<UClass>(*GeneratedName, EFindFirstObjectOptions::None);
    }
}

// MCP:audit_replication - Project-wide replication audit: for every Blueprint under folder_path, which variables are Replicated (CPF_Net), whether the class itself replicates (AActor::GetIsReplicated on the GeneratedClass CDO), and which UFUNCTIONs are RPCs (FUNC_NetFuncFlags). It returns only a SUMMARY plus a report path -- the full per-Blueprint detail is written to Saved/MCPAudits/ and must be read back with read_mcp_audit_file. For one actor rather than a sweep, use get_replication_info / get_actor_replication_info.
//   @param folder_path [string] - Content-relative folder to scan. Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param max_results [number] - Cap on Blueprints processed (1-100000). Defaults to 1000.
//   @returns { path: string, summary: { blueprint_count: int, replicated_class_count: int, replicated_variable_count: int, rpc_count: int } }
TSharedPtr<FJsonObject> HandleAuditReplication(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 1000);

    IAssetRegistry& AR = mcp_common::AssetRegistry();

    // Enumerate ALL assets under FolderPath, then filter to UBlueprint via AssetClassPath.
    // Avoid GetAssetsByClass(UBlueprint, bSearchSubClasses=true), which can hang on a large registry
    // (same guardrail already used by get_blueprint_children in UEAgentAnalysisCommands.cpp).
    TArray<FAssetData> AllAssets;
    AR.GetAssetsByPath(FName(*FolderPath), AllAssets, bRecursive);
    const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();

    TArray<TSharedPtr<FJsonValue>> BlueprintEntries;
    int32 ProcessedCount = 0, ReplicatedClassCount = 0, ReplicatedVarCount = 0, RpcCount = 0;

    for (const FAssetData& AD : AllAssets)
    {
        if (AD.AssetClassPath != BlueprintClassPath) { continue; }
        if (ProcessedCount >= MaxResults) { break; }
        UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
        if (!BP) { continue; }
        ++ProcessedCount;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blueprint_path"), BP->GetPathName());

        TArray<TSharedPtr<FJsonValue>> RepVarArr;
        for (const FBPVariableDescription& Var : BP->NewVariables)
        {
            if (Var.PropertyFlags & CPF_Net)
            {
                RepVarArr.Add(MakeShared<FJsonValueString>(Var.VarName.ToString()));
                ++ReplicatedVarCount;
            }
        }
        Entry->SetArrayField(TEXT("replicated_variables"), RepVarArr);

        bool bClassReplicates = false;
        TArray<TSharedPtr<FJsonValue>> RpcArr;
        if (UClass* GenClass = BP->GeneratedClass)
        {
            if (AActor* CDO = Cast<AActor>(GenClass->GetDefaultObject(false)))
            {
                bClassReplicates = CDO->GetIsReplicated();
            }
            for (TFieldIterator<UFunction> FuncIt(GenClass, EFieldIteratorFlags::ExcludeSuper); FuncIt; ++FuncIt)
            {
                UFunction* Fn = *FuncIt;
                if (Fn && (Fn->FunctionFlags & FUNC_NetFuncFlags))
                {
                    RpcArr.Add(MakeShared<FJsonValueString>(Fn->GetName()));
                    ++RpcCount;
                }
            }
        }
        Entry->SetBoolField(TEXT("class_replicates"), bClassReplicates);
        Entry->SetArrayField(TEXT("rpcs"), RpcArr);
        if (bClassReplicates) { ++ReplicatedClassCount; }

        BlueprintEntries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Report = AU_StartReport(TEXT("replication"));
    Report->SetStringField(TEXT("folder_path"), FolderPath);
    Report->SetArrayField(TEXT("blueprints"), BlueprintEntries);

    FString Path;
    if (TSharedPtr<FJsonObject> Failure = AU_WriteReport(TEXT("audit_replication"), Report, Path))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("blueprint_count"), ProcessedCount);
    Summary->SetNumberField(TEXT("replicated_class_count"), ReplicatedClassCount);
    Summary->SetNumberField(TEXT("replicated_variable_count"), ReplicatedVarCount);
    Summary->SetNumberField(TEXT("rpc_count"), RpcCount);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetObjectField(TEXT("summary"), Summary);
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_blueprint_health - Project-wide compile-sweep: RECOMPILES every Blueprint under folder_path and records error/warning counts. By far the heaviest audit here (it compiles, it does not just read metadata) and it runs synchronously in the editor -- ALWAYS scope it with folder_path on a large project, and lean on max_results, which is a hard cap. It returns only a summary plus a report path; read the detail back with read_mcp_audit_file. To find already-broken Blueprints WITHOUT recompiling anything, use find_erroring_blueprints instead.
//   @param folder_path [string] - Content-relative folder to scan. Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param max_results [number] - Cap on Blueprints compiled (1-100000). Defaults to 200 (lighter than the other audits -- compiling is expensive).
//   @returns { path: string, summary: { blueprint_count: int, clean_count: int, error_count: int, warning_count: int } }
//   NOTE (impl): Epic's own UCompileAllBlueprintsCommandlet does the same sweep out-of-process, not as a
//   synchronous in-editor call, for exactly this reason; we trim engine memory every 10 Blueprints, the same
//   mitigation the commandlet uses.
TSharedPtr<FJsonObject> HandleAuditBlueprintHealth(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 200);

    IAssetRegistry& AR = mcp_common::AssetRegistry();
    // Avoid partial results if the registry is still background-scanning (same guard
    // UCompileAllBlueprintsCommandlet uses before it lists assets).
    AR.SearchAllAssets(/*bSynchronousSearch=*/true);

    TArray<FAssetData> AllAssets;
    AR.GetAssetsByPath(FName(*FolderPath), AllAssets, bRecursive);
    const FTopLevelAssetPath BlueprintClassPath = UBlueprint::StaticClass()->GetClassPathName();

    TArray<TSharedPtr<FJsonValue>> Entries;
    int32 ProcessedCount = 0, CleanCount = 0, ErrorCount = 0, WarningCount = 0, SinceTrim = 0;

    for (const FAssetData& AD : AllAssets)
    {
        if (AD.AssetClassPath != BlueprintClassPath) { continue; }
        if (ProcessedCount >= MaxResults) { break; }
        UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
        if (!BP) { continue; }
        ++ProcessedCount;

        FCompilerResultsLog ResultsLog;
        FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &ResultsLog);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("blueprint_path"), BP->GetPathName());
        Entry->SetNumberField(TEXT("error_count"), ResultsLog.NumErrors);
        Entry->SetNumberField(TEXT("warning_count"), ResultsLog.NumWarnings);
        const bool bClean = (ResultsLog.NumErrors == 0 && ResultsLog.NumWarnings == 0);
        Entry->SetBoolField(TEXT("clean"), bClean);
        if (bClean) { ++CleanCount; }
        if (ResultsLog.NumErrors > 0) { ++ErrorCount; }
        if (ResultsLog.NumWarnings > 0) { ++WarningCount; }
        Entries.Add(MakeShared<FJsonValueObject>(Entry));

        if (++SinceTrim >= 10)
        {
            if (GEngine) { GEngine->TrimMemory(); }
            SinceTrim = 0;
        }
    }

    TSharedPtr<FJsonObject> Report = AU_StartReport(TEXT("blueprint_health"));
    Report->SetStringField(TEXT("folder_path"), FolderPath);
    Report->SetArrayField(TEXT("blueprints"), Entries);
    Report->SetStringField(TEXT("note"), TEXT("Recompiling leaves packages dirty in memory; this audit never calls SavePackage."));

    FString Path;
    if (TSharedPtr<FJsonObject> Failure = AU_WriteReport(TEXT("audit_blueprint_health"), Report, Path))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("blueprint_count"), ProcessedCount);
    Summary->SetNumberField(TEXT("clean_count"), CleanCount);
    Summary->SetNumberField(TEXT("error_count"), ErrorCount);
    Summary->SetNumberField(TEXT("warning_count"), WarningCount);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetObjectField(TEXT("summary"), Summary);
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_collision_profiles - Aggregate report of every project collision profile AND every collision channel (built-in + custom), including which custom ECC_GameTraceChannelN slots are already taken -- the question to ask before add_collision_channel. Cheap: it reads the collision settings in memory, with no AssetRegistry scan and no asset loading, so unlike the other audit_* sweeps it takes no folder_path and no max_results. Returns a summary + a report path (read the detail with read_mcp_audit_file).
//   @returns { path: string, summary: { profile_count: int, channel_count: int, custom_channels_in_use: int } }
//   NOTE (impl): pure public API -- UCollisionProfile::GetNumOfProfiles / GetProfileByIndex / ReturnChannelNameFromContainerIndex.
TSharedPtr<FJsonObject> HandleAuditCollisionProfiles(const TSharedPtr<FJsonObject>& /*Params*/)
{
    UCollisionProfile* Profile = UCollisionProfile::Get();
    if (!Profile)
    {
        return mcp_common::MakeErrorReply(TEXT("UCollisionProfile::Get() returned null"));
    }

    TArray<TSharedPtr<FJsonValue>> ProfileArr;
    const int32 NumProfiles = Profile->GetNumOfProfiles();
    for (int32 i = 0; i < NumProfiles; ++i)
    {
        const FCollisionResponseTemplate* T = Profile->GetProfileByIndex(i);
        if (!T) { continue; }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), T->Name.ToString());
        Entry->SetStringField(TEXT("object_type_name"), T->ObjectTypeName.ToString());
        Entry->SetBoolField(TEXT("can_modify"), T->bCanModify);
        Entry->SetNumberField(TEXT("custom_response_count"), T->CustomResponses.Num());
        ProfileArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TArray<TSharedPtr<FJsonValue>> ChannelArr;
    int32 InUseCount = 0;
    UEnum* ChannelEnum = StaticEnum<ECollisionChannel>();
    if (ChannelEnum)
    {
        for (int32 Idx = 0; Idx < ChannelEnum->NumEnums() - 1; ++Idx) // skip implicit _MAX
        {
            const int64 Value = ChannelEnum->GetValueByIndex(Idx);
            if (Value < 0 || Value > 63) { continue; }
            const FString RawName = ChannelEnum->GetNameStringByIndex(Idx);
            FString RawShortName = RawName;
            RawShortName.RemoveFromStart(TEXT("ECC_"));
            const FName Friendly = Profile->ReturnChannelNameFromContainerIndex(static_cast<int32>(Value));
            const bool bIsGameSlot = RawName.StartsWith(TEXT("ECC_GameTraceChannel"));
            const bool bInUse = bIsGameSlot && !Friendly.ToString().Equals(RawShortName, ESearchCase::IgnoreCase);
            if (bInUse) { ++InUseCount; }

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("channel"), RawName);
            Entry->SetStringField(TEXT("friendly_name"), Friendly.ToString());
            Entry->SetBoolField(TEXT("is_custom_game_slot"), bIsGameSlot);
            ChannelArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> Report = AU_StartReport(TEXT("collision_profiles"));
    Report->SetArrayField(TEXT("profiles"), ProfileArr);
    Report->SetArrayField(TEXT("channels"), ChannelArr);

    FString Path;
    if (TSharedPtr<FJsonObject> Failure = AU_WriteReport(TEXT("audit_collision_profiles"), Report, Path))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("profile_count"), NumProfiles);
    Summary->SetNumberField(TEXT("channel_count"), ChannelArr.Num());
    Summary->SetNumberField(TEXT("custom_channels_in_use"), InUseCount);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetObjectField(TEXT("summary"), Summary);
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_asset_references - Project-wide reference audit under folder_path: assets with zero referencers ("orphans") and dependencies pointing at a package no longer on disk ("broken"). Metadata-only through the AssetRegistry -- it never LOADS the assets, so it scales to a large project without spiking memory. It returns only a summary plus a report path; read the detail back with read_mcp_audit_file. For one asset rather than a sweep, use get_asset_referencers / get_asset_dependencies; for unused assets only, find_unused_assets.
//   @param folder_path [string] - Content-relative folder to scan. Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param max_results [number] - Cap on assets processed (1-100000). Defaults to 5000.
//   @param dependency_type [string] - "hard"|"soft"|"all" (case-insensitive), default "all". Scopes BOTH the referencer check (orphan detection) and the dependency check (broken-reference detection) to that dependency kind. Use "hard" to find only the broken references that actually break loading -- a broken SOFT reference (TSoftObjectPtr/FSoftObjectPath) does not. "all" applies no Hard/Soft requirement at all.
//   @returns { path: string, summary: { asset_count: int, orphan_count: int, broken_reference_count: int, dependency_type: string } }
//   NOTE (impl): IAssetRegistry GetReferencers / GetDependencies / DoesPackageExistOnDisk.
TSharedPtr<FJsonObject> HandleAuditAssetReferences(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 5000);
    const FString DependencyType = AU_GetDependencyType(Params);
    const UE::AssetRegistry::FDependencyQuery Query = AU_BuildDependencyQuery(DependencyType);

    IAssetRegistry& AR = mcp_common::AssetRegistry();

    TArray<FAssetData> AllAssets;
    AR.GetAssetsByPath(FName(*FolderPath), AllAssets, bRecursive);

    TArray<TSharedPtr<FJsonValue>> OrphanArr;
    TArray<TSharedPtr<FJsonValue>> BrokenArr;
    int32 ProcessedCount = 0;

    for (const FAssetData& AD : AllAssets)
    {
        if (ProcessedCount >= MaxResults) { break; }
        ++ProcessedCount;
        const FName PackageName = AD.PackageName;

        TArray<FName> Referencers;
        AR.GetReferencers(PackageName, Referencers, UE::AssetRegistry::EDependencyCategory::Package, Query);
        if (Referencers.Num() == 0)
        {
            OrphanArr.Add(MakeShared<FJsonValueString>(PackageName.ToString()));
        }

        TArray<FName> Dependencies;
        AR.GetDependencies(PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, Query);
        for (const FName& Dep : Dependencies)
        {
            const FString DepStr = Dep.ToString();
            if (DepStr.StartsWith(TEXT("/Script/"))) { continue; } // C++ module reference, not an asset
            if (!AR.DoesPackageExistOnDisk(Dep))
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("asset"), PackageName.ToString());
                Entry->SetStringField(TEXT("missing_dependency"), DepStr);
                BrokenArr.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    TSharedPtr<FJsonObject> Report = AU_StartReport(TEXT("asset_references"));
    Report->SetStringField(TEXT("folder_path"), FolderPath);
    Report->SetStringField(TEXT("dependency_type"), DependencyType);
    Report->SetArrayField(TEXT("orphan_assets"), OrphanArr);
    Report->SetArrayField(TEXT("broken_references"), BrokenArr);

    FString Path;
    if (TSharedPtr<FJsonObject> Failure = AU_WriteReport(TEXT("audit_asset_references"), Report, Path))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("asset_count"), ProcessedCount);
    Summary->SetNumberField(TEXT("orphan_count"), OrphanArr.Num());
    Summary->SetNumberField(TEXT("broken_reference_count"), BrokenArr.Num());
    Summary->SetStringField(TEXT("dependency_type"), DependencyType);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetObjectField(TEXT("summary"), Summary);
    return mcp_common::MakeOkReply(Result);
}

// capture_pie_state -- DELIBERATELY has no `include_components` param, and must not grow one.
//   A PIE component COUNT is not a structural measure: two structurally identical actors can
//   differ (e.g. 54 vs 53) purely because some system spawns its own components (Niagara, etc.)
//   at runtime. Exposing that count here would invite exactly that false positive. The correct
//   structural comparison is on the CDO/SCS (verify_reparent_success, diff_blueprint_components,
//   get_blueprint_scs_tree).
//
// The CPF_SaveGame half of the property filter below is NOT dead, and a 0 in
//   savegame_property_count is a result to look at, not a foregone conclusion.
//   The flag is not only a NATIVE-UPROPERTY question. It also lives on BLUEPRINT variables
//   -- the "SaveGame" checkbox of the variable Details panel writes CPF_SaveGame onto
//   FBPVariableDescription::PropertyFlags, and KismetCompiler.cpp copies it onto the generated
//   FProperty, which is the FProperty this filter iterates. A grep over native C++ alone cannot
//   see those.
//   Also true: no NATIVE UPROPERTY of this plugin, nor of the stock engine actor classes used
//   here, sets the flag -- and the filter never silently empties the response anyway, because the
//   CPF_Net half of the same OR still matches. So the design holds: MEASURE per run rather than assume.
//   savegame_property_count + savegame_filter_status ("dead_in_this_run" when the flag matched nothing)
//   keep a 0 visibly "the flag matched nothing here", never "we lied" -- but on a snapshot of actors
//   that own SaveGame Blueprint variables, "dead_in_this_run" is a finding worth chasing.
//
// MCP:capture_pie_state - Snapshot of every actor in the ACTIVE PIE world, with every property flagged Replicated or SaveGame (generic -- no judgment call about what is "relevant"). It requires a live PIE session: with none running for pie_client_index it errors cleanly instead of silently snapshotting the EDITOR world, which is what makes it usable as co-op evidence -- point it at a client index to prove a value really replicated. On a large world ALWAYS pass class_filter and/or max_actors. Returns a summary plus a report path; read the detail with read_mcp_audit_file.
//   @param pie_client_index [number] - Which PIE client's world to snapshot (0 = first/server/standalone). Defaults to 0.
//   @param class_filter - Optional actor-class guardrail. Accepts a single class name OR an array of them (the hint is deliberately absent: the type-hint grammar has no union, and `[string|array]` would make this whole @param unparseable, leaving it an EMPTY description in the catalog). Detail: one class name or an array of them, each resolved to a UClass (object/script path, short name, or the Blueprint asset name -- "<Name>_C" is tried too) and matched with IsA, so subclasses match. An entry that does not resolve, or that resolves to a non-AActor class, is a hard ERROR rather than being dropped: dropping it would silently snapshot the whole World Partition world, keeping it would silently return an empty snapshot. Defaults to no filter (every actor).
//   @param max_actors [number] - Optional cap on how many actors get their properties reflected into the report; 0 (the default) means UNLIMITED, and negative values are clamped to 0. Actors past the cap are still COUNTED (cheap) but not reflected (the expensive part), so total_matching_actors and truncated are exact, not estimated.
//   @returns { path: string, truncated: bool, summary: { actor_count: int, actors_seen: int, total_matching_actors: int, max_actors: int, truncated: bool, class_filter: [string], replicated_property_count: int, savegame_property_count: int, savegame_filter_status: "active"|"dead_in_this_run" }, report_size_bytes: int, verification: {...}, verification_checks: [...] } -- class_filter lists the resolved class paths; success is false when the report is missing or empty on disk.
//   NOTE (impl): the PIE world comes from GEditor->GetPIEWorldContext directly, NOT mcp_common::GetActiveWorld,
//   which silently falls back to the editor world -- that fallback would defeat the point of a PIE-specific snapshot.
TSharedPtr<FJsonObject> HandleCapturePieState(const TSharedPtr<FJsonObject>& Params)
{
    int32 PieClientIndex = 0;
    double D = 0.0;
    if (Params->TryGetNumberField(TEXT("pie_client_index"), D)) { PieClientIndex = static_cast<int32>(D); }

    // max_actors -- size guardrail. Default 0 == unlimited, so omitting it returns the
    // full response (rule: an optional param never changes the default response).
    int32 MaxActors = 0;
    double MaxActorsD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_actors"), MaxActorsD))
    {
        MaxActors = FMath::Max(0, static_cast<int32>(MaxActorsD));
    }

    // class_filter -- accepts a bare string OR an array of strings. TryGetStringField fails on
    // an array value, so the array branch is only tried after the string branch declines.
    TArray<FString> ClassFilterNames;
    FString SingleClassFilter;
    if (Params->TryGetStringField(TEXT("class_filter"), SingleClassFilter))
    {
        if (!SingleClassFilter.IsEmpty()) { ClassFilterNames.Add(SingleClassFilter); }
    }
    else
    {
        const TArray<TSharedPtr<FJsonValue>>* ClassFilterArr = nullptr;
        if (Params->TryGetArrayField(TEXT("class_filter"), ClassFilterArr) && ClassFilterArr)
        {
            for (const TSharedPtr<FJsonValue>& Value : *ClassFilterArr)
            {
                FString Entry;
                if (Value.IsValid() && Value->TryGetString(Entry) && !Entry.IsEmpty())
                {
                    ClassFilterNames.Add(Entry);
                }
            }
        }
    }

    // PIE gate FIRST -- cheaper failure, and it avoids any class lookup (which can touch the
    // package loader) when there is nothing to snapshot anyway.
    if (!GEditor)
    {
        return mcp_common::MakeErrorReply(TEXT("No editor available"));
    }
    FWorldContext* WC = GEditor->GetPIEWorldContext(PieClientIndex);
    if (!WC || !WC->World())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No active PIE session for pie_client_index %d"), PieClientIndex));
    }
    UWorld* PieWorld = WC->World();

    TArray<UClass*> FilterClasses;
    TArray<TSharedPtr<FJsonValue>> ResolvedFilterEcho;
    for (const FString& FilterName : ClassFilterNames)
    {
        UClass* Resolved = AU_ResolveActorClass(FilterName);
        if (!Resolved)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("class_filter '%s' did not resolve to a loaded UClass. Pass a /Script/ path, a native class short name, or a Blueprint's generated class (e.g. 'BP_Foo_C'). Refusing to drop the filter, which would snapshot the entire world."),
                *FilterName));
        }
        if (!Resolved->IsChildOf(AActor::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("class_filter '%s' resolved to %s, which is not an AActor subclass -- it would match zero actors and return a silently empty snapshot."),
                *FilterName, *Resolved->GetPathName()));
        }
        FilterClasses.AddUnique(Resolved);
        ResolvedFilterEcho.Add(MakeShared<FJsonValueString>(Resolved->GetPathName()));
    }

    TArray<TSharedPtr<FJsonValue>> ActorArr;
    int32 ActorsSeen = 0;           // every non-null actor the iterator visited, before class_filter
    int32 MatchingActors = 0;       // actors that passed class_filter (counted even past max_actors)
    int32 ReplicatedPropCount = 0;
    int32 SaveGamePropCount = 0;
    for (TActorIterator<AActor> It(PieWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor) { continue; }
        ++ActorsSeen;

        if (FilterClasses.Num() > 0)
        {
            bool bMatchesFilter = false;
            for (UClass* FilterClass : FilterClasses)
            {
                if (Actor->IsA(FilterClass)) { bMatchesFilter = true; break; }
            }
            if (!bMatchesFilter) { continue; }
        }
        ++MatchingActors;

        // Past the cap we keep COUNTING (one IsA per actor, cheap) but stop reflecting
        // properties (TFieldIterator + PropertyValueToJson, the expensive part). That is what
        // makes `truncated` and `total_matching_actors` exact instead of guessed.
        if (MaxActors > 0 && ActorArr.Num() >= MaxActors) { continue; }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actor_name"), Actor->GetName());
        Entry->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
        Entry->SetBoolField(TEXT("replicates"), Actor->GetIsReplicated());

        TArray<TSharedPtr<FJsonValue>> PropArr;
        for (TFieldIterator<FProperty> PropIt(Actor->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop) { continue; }
            const bool bReplicated = Prop->HasAnyPropertyFlags(CPF_Net);
            const bool bSaveGame = Prop->HasAnyPropertyFlags(CPF_SaveGame);
            if (!bReplicated && !bSaveGame) { continue; }
            if (bReplicated) { ++ReplicatedPropCount; }
            if (bSaveGame) { ++SaveGamePropCount; }

            TSharedPtr<FJsonObject> PropEntry = MakeShared<FJsonObject>();
            PropEntry->SetStringField(TEXT("name"), Prop->GetName());
            PropEntry->SetBoolField(TEXT("replicated"), bReplicated);
            PropEntry->SetBoolField(TEXT("savegame"), bSaveGame);
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Actor);
            PropEntry->SetField(TEXT("value"), mcp_common::PropertyValueToJson(Prop, ValuePtr));
            PropArr.Add(MakeShared<FJsonValueObject>(PropEntry));
        }
        Entry->SetArrayField(TEXT("tracked_properties"), PropArr);
        ActorArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    const int32 ActorCount = ActorArr.Num(); // actors actually written into the report
    const bool bTruncated = (MaxActors > 0) && (MatchingActors > ActorCount);
    // Measured, not asserted -- "dead_in_this_run" means the CPF_SaveGame half of the filter
    // matched zero properties in the actors that were actually reflected.
    const FString SaveGameFilterStatus = (SaveGamePropCount > 0) ? TEXT("active") : TEXT("dead_in_this_run");

    TSharedPtr<FJsonObject> Report = AU_StartReport(TEXT("pie_state"));
    Report->SetNumberField(TEXT("pie_client_index"), PieClientIndex);
    Report->SetArrayField(TEXT("class_filter"), ResolvedFilterEcho);
    Report->SetNumberField(TEXT("max_actors"), MaxActors);
    Report->SetNumberField(TEXT("actors_seen"), ActorsSeen);
    Report->SetNumberField(TEXT("total_matching_actors"), MatchingActors);
    Report->SetNumberField(TEXT("actor_count"), ActorCount);
    Report->SetBoolField(TEXT("truncated"), bTruncated);
    Report->SetNumberField(TEXT("replicated_property_count"), ReplicatedPropCount);
    Report->SetNumberField(TEXT("savegame_property_count"), SaveGamePropCount);
    Report->SetStringField(TEXT("savegame_filter_status"), SaveGameFilterStatus);
    Report->SetStringField(TEXT("savegame_filter_note"),
        TEXT("Properties are included when CPF_Net OR CPF_SaveGame is set. CPF_SaveGame is no longer expected to be 0: no NATIVE UPROPERTY here sets it, but the \"SaveGame\" checkbox of a Blueprint variable does, and the compiler copies that onto the generated FProperty this filter iterates. savegame_property_count is a measurement either way -- a 0 on actors whose Blueprints DO tick that box means the flag did not survive the compile, which is a finding, not a quirk of the capture."));
    Report->SetStringField(TEXT("components_note"),
        TEXT("No component data by design: PIE component counts are not a structural measure (runtime-spawned components, e.g. Niagara, make structurally identical actors differ). Use verify_reparent_success / diff_blueprint_components / get_blueprint_scs_tree for structure."));
    Report->SetArrayField(TEXT("actors"), ActorArr);

    FString Path;
    if (TSharedPtr<FJsonObject> Failure = AU_WriteReport(TEXT("capture_pie_state"), Report, Path))
    {
        return Failure;
    }

    TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
    Summary->SetNumberField(TEXT("actor_count"), ActorCount);
    Summary->SetNumberField(TEXT("actors_seen"), ActorsSeen);
    Summary->SetNumberField(TEXT("total_matching_actors"), MatchingActors);
    Summary->SetNumberField(TEXT("max_actors"), MaxActors);
    Summary->SetBoolField(TEXT("truncated"), bTruncated);
    Summary->SetArrayField(TEXT("class_filter"), ResolvedFilterEcho);
    Summary->SetNumberField(TEXT("replicated_property_count"), ReplicatedPropCount);
    Summary->SetNumberField(TEXT("savegame_property_count"), SaveGamePropCount);
    Summary->SetStringField(TEXT("savegame_filter_status"), SaveGameFilterStatus);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), Path);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetObjectField(TEXT("summary"), Summary);

    // Honesty -- AU_WriteReport only reports what SaveStringToFile returned. Re-stat the file
    // so the caller gets evidence the report really landed on disk (and how big it is) instead of
    // a hardcoded success.
    const int64 WrittenBytes = FPlatformFileManager::Get().GetPlatformFile().FileSize(*Path);
    const bool bOnDisk = (WrittenBytes > 0);
    Result->SetNumberField(TEXT("report_size_bytes"), static_cast<double>(WrittenBytes));
    return mcp_common::MakeVerifiedReply(
        Result,
        bOnDisk,
        bOnDisk ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("IPlatformFile::FileSize on the written report"),
        FString::Printf(TEXT("%s -> %lld bytes"), *Path, WrittenBytes),
        bOnDisk ? FString() : FString::Printf(TEXT("Report file is missing or empty on disk after write: %s"), *Path));
}

// MCP:read_mcp_audit_file - Read back the FULL report that an audit_* or capture_pie_state command wrote to disk. Those commands return only a summary plus a path, so this is the second half of every one of them -- without it you never see the per-Blueprint / per-asset / per-actor detail. Restricted to Saved/MCPAudits/: it is not a general file reader.
//   @param path [string!] - Path returned by a prior audit command
//   @returns the full parsed JSON report object
TSharedPtr<FJsonObject> HandleReadMcpAuditFile(const TSharedPtr<FJsonObject>& Params)
{
    FString Path;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("path"), Path)) { return Missing; }

    const FString AuditDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MCPAudits"));
    const FString FullPath = FPaths::ConvertRelativePathToFull(Path);
    if (!FullPath.StartsWith(AuditDir))
    {
        return mcp_common::MakeErrorReply(TEXT("path must be a file under Saved/MCPAudits/"));
    }

    FString Contents;
    if (!FFileHelper::LoadFileToString(Contents, *FullPath))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not read file: %s"), *FullPath));
    }

    TSharedPtr<FJsonObject> Parsed;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Contents);
    if (!FJsonSerializer::Deserialize(Reader, Parsed) || !Parsed.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("File is not valid JSON"));
    }

    return mcp_common::MakeOkReply(Parsed);
}

// ---------------------------------------------------------------------------
// UI AUDITS -- the two questions that scale badly by hand over dozens of screens.
//
// The other audits look at Blueprints, collisions, references and replication, not
// at UMG. These two answer "which text in my UI cannot be translated?" and "which
// widgets have drifted from the style?".
// ---------------------------------------------------------------------------

namespace
{
    /**
     * Every Widget Blueprint under a folder, already loaded, up to MaxAssets. bOutTruncated says
     * whether another one was left out (found in the registry, never loaded) -- not merely that the
     * list holds exactly MaxAssets.
     */
    TArray<UWidgetBlueprint*> AU_CollectWidgetBlueprints(const FString& FolderPath, bool bRecursive,
                                                         int32 MaxAssets, bool& bOutTruncated)
    {
        bOutTruncated = false;
        TArray<UWidgetBlueprint*> Out;
        IAssetRegistry& AR = mcp_common::AssetRegistry();
        AR.SearchAllAssets(/*bSynchronousSearch=*/ true);

        TArray<FAssetData> AllAssets;
        AR.GetAssetsByPath(FName(*FolderPath), AllAssets, bRecursive);

        const FTopLevelAssetPath WidgetBPClassPath = UWidgetBlueprint::StaticClass()->GetClassPathName();
        for (const FAssetData& AD : AllAssets)
        {
            if (AD.AssetClassPath != WidgetBPClassPath) { continue; }
            if (Out.Num() >= MaxAssets) { bOutTruncated = true; break; }
            if (UWidgetBlueprint* WBP = Cast<UWidgetBlueprint>(AD.GetAsset()))
            {
                Out.Add(WBP);
            }
        }
        return Out;
    }

    /** Exported text of a leaf property, to compare values of different types without fuss. */
    FString AU_ExportLeaf(FProperty* Prop, const void* ValuePtr)
    {
        FString Out;
        if (Prop && ValuePtr)
        {
            Prop->ExportText_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None, nullptr);
        }
        return Out;
    }
}

namespace
{
    /**
     * Recursively walks the FTexts of an object or struct and calls Visit for each non-empty one.
     * Needed because a large game's text does not live in widgets: it lives in DataTable rows and
     * DataAssets (item names, quest descriptions, tooltips), and there the FTexts are
     * NESTED inside structs, not hanging off the class.
     */
    void ForEachTextInStruct_Audit(const UStruct* Struct, const void* Container, const FString& PathPrefix,
                                   int32 Depth, TFunctionRef<void(const FString&, const FText&)> Visit)
    {
        if (!Struct || !Container || Depth > 6)   // cap: some structs reference each other cyclically
        {
            return;
        }
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            FProperty* Prop = *It;
            if (!Prop) { continue; }
            const FString Path = PathPrefix.IsEmpty() ? Prop->GetName()
                                                      : PathPrefix + TEXT(".") + Prop->GetName();

            if (const FTextProperty* TextProp = CastField<FTextProperty>(Prop))
            {
                const FText Value = TextProp->GetPropertyValue_InContainer(Container);
                if (!Value.IsEmpty()) { Visit(Path, Value); }
            }
            else if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
            {
                ForEachTextInStruct_Audit(StructProp->Struct,
                    StructProp->ContainerPtrToValuePtr<void>(Container), Path, Depth + 1, Visit);
            }
            else if (const FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop))
            {
                if (const FStructProperty* Inner = CastField<FStructProperty>(ArrayProp->Inner))
                {
                    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
                    for (int32 i = 0; i < Helper.Num(); ++i)
                    {
                        ForEachTextInStruct_Audit(Inner->Struct, Helper.GetRawPtr(i),
                            FString::Printf(TEXT("%s[%d]"), *Path, i), Depth + 1, Visit);
                    }
                }
                else if (CastField<FTextProperty>(ArrayProp->Inner))
                {
                    FScriptArrayHelper Helper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(Container));
                    for (int32 i = 0; i < Helper.Num(); ++i)
                    {
                        const FText& Value = *reinterpret_cast<const FText*>(Helper.GetRawPtr(i));
                        if (!Value.IsEmpty()) { Visit(FString::Printf(TEXT("%s[%d]"), *Path, i), Value); }
                    }
                }
            }
        }
    }
}

// MCP:audit_widget_text - Sweep Widget Blueprints and report which player-visible texts are typed INLINE rather than coming from a String Table -- the localization-maintainability question. NOTE: this corrects the intuition most people start with: a text written as a bare string is NOT lost to localization. Unreal assigns it a stable key when the package is saved, and FTextInspector::ShouldGatherForLocalization returns true, so it DOES get collected and translated. What it lacks is a MEANINGFUL identity: the namespace comes out empty and the key is an opaque GUID like 971E3B3B436AEE91028BA1ADB97B3046. That still costs -- translators see no context, and re-authoring the text churns the key, orphaning the translation. So this reports identity_kind per text: string_table (best), explicit (an author-given namespace/key), auto_key (typed inline, key generated on save) and none (never saved, no identity yet). Default output is everything that is NOT string_table or explicit.
//   @param folder_path - Content folder to sweep (default /Game). Every UWidgetBlueprint under it is loaded and walked.
//   @param recursive [bool] - Recurse into subfolders (default true).
//   @param include_all [bool] - Also report the texts that already have a good identity, string_table and explicit ones included (default false).
//   @param include_data_assets [bool] - Also sweep DataTables and DataAssets under the same folder (default false). This is where an AAA game actually keeps its text -- item names, quest descriptions, tooltips -- and it is text the widget walk cannot reach, because there the FTexts sit in nested struct fields rather than on a widget class. Nested structs and arrays of structs are walked too.
//   @param max_results - Max Widget Blueprints to load (default 500). Loading a WBP is not free; a whole-project sweep is a heavy call.
//   @returns { folder_path, widget_blueprints_scanned, texts_examined, counts_by_identity: {string_table, explicit, auto_key, none}, not_gathered_count, entries: [{ widget_bp_path, widget_name, widget_class, property_name, text, identity_kind, will_be_gathered, namespace, key, string_table }], truncated } -- not_gathered_count is the engine's own ShouldGatherForLocalization saying a text will NOT be collected at all; in a saved asset that is normally 0, and anything above it is a real alarm.

TSharedPtr<FJsonObject> HandleAuditWidgetText(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 500);

    bool bIncludeAll = false;
    Params->TryGetBoolField(TEXT("include_all"), bIncludeAll);

    bool bWidgetListTruncated = false;
    TArray<UWidgetBlueprint*> WidgetBPs = AU_CollectWidgetBlueprints(FolderPath, bRecursive, MaxResults, bWidgetListTruncated);

    TArray<TSharedPtr<FJsonValue>> Entries;
    int32 TextsExamined = 0, NotGathered = 0;
    int32 CountTable = 0, CountExplicit = 0, CountAuto = 0, CountNone = 0;

    // Counts one text under its localization identity and lists it unless that identity is good (string_table or
    // explicit) and include_all is off. Owner is the asset that holds the text; Holder is the widget, or the asset
    // itself for text read off data.
    auto VisitText = [&](const UObject* Owner, const UObject* Holder, const FString& HolderName,
                         const FString& PropertyPath, const FText& Value, bool bFromData)
    {
        ++TextsExamined;

        const bool bWillGather = FTextInspector::ShouldGatherForLocalization(Value);
        if (!bWillGather) { ++NotGathered; }

        const TOptional<FString> Namespace = FTextInspector::GetNamespace(Value);
        const TOptional<FString> Key = FTextInspector::GetKey(Value);
        FName TableId;
        FString TableKey;
        const bool bFromTable = FTextInspector::GetTableIdAndKey(Value, TableId, TableKey);

        // The classification is what is useful. MEASURED: a bare string ends up with a key (UE
        // assigns it when the package is saved) but with an EMPTY namespace -- that is why the
        // namespace separates "the author gave it an identity" from "the engine made one up".
        const FString HasNamespace = Namespace.IsSet() ? *Namespace : FString();
        const FString HasKey = Key.IsSet() ? *Key : FString();

        FString IdentityKind;
        if (bFromTable)                   { IdentityKind = TEXT("string_table"); ++CountTable; }
        else if (!HasNamespace.IsEmpty()) { IdentityKind = TEXT("explicit");     ++CountExplicit; }
        else if (!HasKey.IsEmpty())       { IdentityKind = TEXT("auto_key");     ++CountAuto; }
        else                              { IdentityKind = TEXT("none");         ++CountNone; }

        const bool bIsGoodIdentity = bFromTable || !HasNamespace.IsEmpty();
        if (bIsGoodIdentity && !bIncludeAll)
        {
            return;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("widget_bp_path"), Owner->GetPathName());
        Entry->SetStringField(TEXT("widget_name"), HolderName);
        Entry->SetStringField(TEXT("widget_class"), Holder->GetClass()->GetName());
        Entry->SetStringField(TEXT("property_name"), PropertyPath);
        Entry->SetStringField(TEXT("text"), Value.ToString().Left(200));
        Entry->SetStringField(TEXT("identity_kind"), IdentityKind);
        Entry->SetBoolField(TEXT("will_be_gathered"), bWillGather);
        Entry->SetStringField(TEXT("namespace"), HasNamespace);
        Entry->SetStringField(TEXT("key"), HasKey);
        Entry->SetStringField(TEXT("string_table"), bFromTable ? TableId.ToString() : FString());
        if (bFromData)
        {
            Entry->SetStringField(TEXT("source_kind"), TEXT("data"));
        }
        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    };

    for (UWidgetBlueprint* WBP : WidgetBPs)
    {
        if (!WBP || !WBP->WidgetTree) { continue; }

        TArray<UWidget*> AllWidgets;
        WBP->WidgetTree->GetAllWidgets(AllWidgets);

        for (UWidget* Widget : AllWidgets)
        {
            if (!Widget) { continue; }

            for (TFieldIterator<FTextProperty> It(Widget->GetClass()); It; ++It)
            {
                FTextProperty* TextProp = *It;
                if (!TextProp || !TextProp->HasAnyPropertyFlags(CPF_Edit)) { continue; }

                const FText Value = TextProp->GetPropertyValue_InContainer(Widget);
                // An empty text is not a localization problem: there is simply no text.
                if (Value.IsEmpty()) { continue; }

                VisitText(WBP, Widget, Widget->GetName(), TextProp->GetName(), Value, /*bFromData=*/ false);
            }
        }
    }

    // --- Optional sweep of DataTables and DataAssets: a real game's text lives there.
    bool bIncludeDataAssets = false;
    Params->TryGetBoolField(TEXT("include_data_assets"), bIncludeDataAssets);
    int32 DataAssetsScanned = 0;
    if (bIncludeDataAssets)
    {
        IAssetRegistry& AR2 = mcp_common::AssetRegistry();
        TArray<FAssetData> DataAssets;
        AR2.GetAssetsByPath(FName(*FolderPath), DataAssets, bRecursive);

        for (const FAssetData& AD : DataAssets)
        {
            UObject* Asset = AD.GetAsset();
            if (!Asset) { continue; }

            if (UDataTable* Table = Cast<UDataTable>(Asset))
            {
                ++DataAssetsScanned;
                const UScriptStruct* RowStruct = Table->GetRowStruct();
                if (!RowStruct) { continue; }
                for (const TPair<FName, uint8*>& Row : Table->GetRowMap())
                {
                    if (!Row.Value) { continue; }
                    const FString RowLabel = Row.Key.ToString();
                    ForEachTextInStruct_Audit(RowStruct, Row.Value, FString(), 0,
                        [&VisitText, Asset, &RowLabel](const FString& P, const FText& V) { VisitText(Asset, Asset, RowLabel, P, V, /*bFromData=*/ true); });
                }
            }
            else if (Asset->IsA<UDataAsset>())
            {
                ++DataAssetsScanned;
                const FString AssetLabel = Asset->GetName();
                ForEachTextInStruct_Audit(Asset->GetClass(), Asset, FString(), 0,
                    [&VisitText, Asset, &AssetLabel](const FString& P, const FText& V) { VisitText(Asset, Asset, AssetLabel, P, V, /*bFromData=*/ true); });
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("folder_path"), FolderPath);
    Result->SetNumberField(TEXT("widget_blueprints_scanned"), WidgetBPs.Num());
    Result->SetNumberField(TEXT("data_assets_scanned"), DataAssetsScanned);
    Result->SetNumberField(TEXT("texts_examined"), TextsExamined);
    Result->SetNumberField(TEXT("not_gathered_count"), NotGathered);

    TSharedPtr<FJsonObject> ByIdentity = MakeShared<FJsonObject>();
    ByIdentity->SetNumberField(TEXT("string_table"), CountTable);
    ByIdentity->SetNumberField(TEXT("explicit"), CountExplicit);
    ByIdentity->SetNumberField(TEXT("auto_key"), CountAuto);
    ByIdentity->SetNumberField(TEXT("none"), CountNone);
    Result->SetObjectField(TEXT("counts_by_identity"), ByIdentity);
    Result->SetArrayField(TEXT("entries"), Entries);
    Result->SetBoolField(TEXT("truncated"), bWidgetListTruncated);
    return Result;
}

// MCP:audit_widget_style - Sweep Widget Blueprints and report every widget whose style DEVIATES from a reference widget -- the "who is off-theme" question, which is the one that scales badly by hand once there are more than a couple of screens. You designate one widget as the truth (reference_bp_path + reference_widget_name), list the property paths that define the style (dotted paths work: "WidgetStyle.Normal.TintColor"), and this reports, per matching widget, which of those paths differ and what each side holds. Comparison is on the property's exported text, so it is type-agnostic and reports a real difference rather than a formatting one. Pair it with apply_widget_style_preset, which can copy those same paths off that same reference widget to FIX what this finds.
//   @param reference_bp_path [string!] - /Game path of the Widget Blueprint holding the reference widget (the style source of truth).
//   @param reference_widget_name [string!] - Name of the reference widget inside that Widget Blueprint.
//   @param property_paths [array!] - The paths that define the style, e.g. ["WidgetStyle.Normal.TintColor","WidgetStyle.NormalPadding"]. A path that does not resolve on the REFERENCE is an error (the reference must define the style); one that does not resolve on a target is reported as unresolved, not as a deviation.
//   @param folder_path - Content folder to sweep for targets (default /Game).
//   @param recursive [bool] - Recurse into subfolders (default true).
//   @param target_class - Only compare widgets of this class, matched with IsA (defaults to the reference widget's own class, which is almost always what you want).
//   @param include_matching [bool] - Also report the widgets that already match (default false).
//   @param max_results - Max Widget Blueprints to load (default 500).
//   @returns { reference, property_paths, widget_blueprints_scanned, widgets_compared, deviating_count, matching_count, entries: [{ widget_bp_path, widget_name, widget_class, deviations: [{path, actual, expected}], unresolved_paths: [string], matches: bool }], truncated }

TSharedPtr<FJsonObject> HandleAuditWidgetStyle(const TSharedPtr<FJsonObject>& Params)
{
    FString RefBPPath, RefWidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("reference_bp_path"), RefBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("reference_widget_name"), RefWidgetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("property_paths"), PathsArr) || !PathsArr || PathsArr->Num() == 0)
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: property_paths (a non-empty array of property paths)"));

    TArray<FString> Paths;
    for (const TSharedPtr<FJsonValue>& E : *PathsArr)
    {
        const FString P = E.IsValid() ? E->AsString() : FString();
        if (!P.IsEmpty()) { Paths.Add(P); }
    }

    UWidgetBlueprint* RefBP = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(RefBPPath));
    if (!RefBP || !RefBP->WidgetTree)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("reference_bp_path '%s' is not a loadable Widget Blueprint"), *RefBPPath));

    UWidget* RefWidget = RefBP->WidgetTree->FindWidget(FName(*RefWidgetName));
    if (!RefWidget)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("reference widget '%s' not found in %s"), *RefWidgetName, *RefBPPath));

    // The reference values are read ONCE. If one does not resolve on the reference it is an error
    // of the call, not a finding: without an expected value there is nothing to compare against.
    TMap<FString, FString> Expected;
    for (const FString& Path : Paths)
    {
        FProperty* LeafProp = nullptr;
        void* LeafPtr = nullptr;
        FString ResolveErr;
        if (!mcp_common::ResolvePropertyPath(RefWidget->GetClass(), RefWidget, Path, LeafProp, LeafPtr, ResolveErr))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("property_path '%s' does not resolve on the reference widget '%s' (%s): %s"),
                *Path, *RefWidgetName, *RefWidget->GetClass()->GetName(), *ResolveErr));
        }
        Expected.Add(Path, AU_ExportLeaf(LeafProp, LeafPtr));
    }

    UClass* FilterClass = RefWidget->GetClass();
    FString TargetClassName;
    if (Params->TryGetStringField(TEXT("target_class"), TargetClassName) && !TargetClassName.IsEmpty())
    {
        if (UClass* Resolved = FUEAgentGraphUtils::ResolveClass(TargetClassName))
        {
            FilterClass = Resolved;
        }
    }

    const FString FolderPath = AU_GetFolderPath(Params);
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 500);
    bool bIncludeMatching = false;
    Params->TryGetBoolField(TEXT("include_matching"), bIncludeMatching);

    bool bWidgetListTruncated = false;
    TArray<UWidgetBlueprint*> WidgetBPs = AU_CollectWidgetBlueprints(FolderPath, bRecursive, MaxResults, bWidgetListTruncated);

    TArray<TSharedPtr<FJsonValue>> Entries;
    int32 Compared = 0, Deviating = 0, Matching = 0;

    for (UWidgetBlueprint* WBP : WidgetBPs)
    {
        if (!WBP || !WBP->WidgetTree) { continue; }

        TArray<UWidget*> AllWidgets;
        WBP->WidgetTree->GetAllWidgets(AllWidgets);

        for (UWidget* Widget : AllWidgets)
        {
            if (!Widget || !Widget->IsA(FilterClass)) { continue; }
            // The reference is not compared with itself: it always "matches" and only clutters the report.
            if (Widget == RefWidget) { continue; }

            ++Compared;

            TArray<TSharedPtr<FJsonValue>> Deviations;
            TArray<TSharedPtr<FJsonValue>> Unresolved;

            for (const FString& Path : Paths)
            {
                FProperty* LeafProp = nullptr;
                void* LeafPtr = nullptr;
                FString ResolveErr;
                if (!mcp_common::ResolvePropertyPath(Widget->GetClass(), Widget, Path, LeafProp, LeafPtr, ResolveErr))
                {
                    // A path missing on a widget of another class is normal, not a deviation.
                    Unresolved.Add(MakeShared<FJsonValueString>(Path));
                    continue;
                }
                const FString Actual = AU_ExportLeaf(LeafProp, LeafPtr);
                if (!Actual.Equals(Expected[Path], ESearchCase::CaseSensitive))
                {
                    TSharedPtr<FJsonObject> Dev = MakeShared<FJsonObject>();
                    Dev->SetStringField(TEXT("path"), Path);
                    Dev->SetStringField(TEXT("actual"), Actual.Left(300));
                    Dev->SetStringField(TEXT("expected"), Expected[Path].Left(300));
                    Deviations.Add(MakeShared<FJsonValueObject>(Dev));
                }
            }

            const bool bMatches = Deviations.Num() == 0;
            if (bMatches) { ++Matching; } else { ++Deviating; }

            if (!bMatches || bIncludeMatching)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("widget_bp_path"), WBP->GetPathName());
                Entry->SetStringField(TEXT("widget_name"), Widget->GetName());
                Entry->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetName());
                Entry->SetArrayField(TEXT("deviations"), Deviations);
                Entry->SetArrayField(TEXT("unresolved_paths"), Unresolved);
                Entry->SetBoolField(TEXT("matches"), bMatches);
                Entries.Add(MakeShared<FJsonValueObject>(Entry));
            }
        }
    }

    TSharedPtr<FJsonObject> Reference = MakeShared<FJsonObject>();
    Reference->SetStringField(TEXT("widget_bp_path"), RefBPPath);
    Reference->SetStringField(TEXT("widget_name"), RefWidgetName);
    Reference->SetStringField(TEXT("widget_class"), RefWidget->GetClass()->GetName());
    Reference->SetStringField(TEXT("compared_as_class"), FilterClass->GetName());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetObjectField(TEXT("reference"), Reference);
    Result->SetArrayField(TEXT("property_paths"), mcp_common::StringsToJson(Paths));
    Result->SetNumberField(TEXT("widget_blueprints_scanned"), WidgetBPs.Num());
    Result->SetNumberField(TEXT("widgets_compared"), Compared);
    Result->SetNumberField(TEXT("deviating_count"), Deviating);
    Result->SetNumberField(TEXT("matching_count"), Matching);
    Result->SetArrayField(TEXT("entries"), Entries);
    Result->SetBoolField(TEXT("truncated"), bWidgetListTruncated);
    return Result;
}

// ---------------------------------------------------------------------------
// ACCESSIBILITY AUDIT
//
// Why it exists. On console this is a CERTIFICATION requirement, not an optional
// improvement.
//
// WARNING: HOW FAR IT REACHES, stated before anyone trusts it too much. This measures what is
// AUTHORED in the Widget Blueprint, which is the only thing measurable without playing:
//   - Contrast is computed against the FIRST ancestor with a solid background color found
//     walking up the tree: a UBorder, a UButton (via its WidgetStyle.Normal) or a flat
//     UImage. If the real background comes from a texture, a material, a video or a sibling
//     widget drawn underneath, THIS DOES NOT SEE IT -- and then it reports
//     background_source:"none" instead of passing by default. A "not found" is not a pass,
//     and that distinction is the whole value of the tool.
//   - The UButton branch is NOT a detail: without it a button's background -- where
//     almost all of a menu's text lives -- would be invisible, and UMG's default gray under white
//     text (1.92:1) would read "contrast_unknown", hiding most real issues. If this tool tells
//     you there are no contrast failures, ALSO check that it is not saying so because it did
//     not look where it should.
//   - A color coming from a CommonUI style or a property binding is not seen either:
//     what is authored is what there is.
// What IS exact: the formula. It is WCAG 2.x -- relative luminance
// 0.2126R + 0.7152G + 0.0722B over LINEAR values (which is exactly what an FLinearColor is,
// so nothing needs linearizing) and ratio (Llighter + 0.05) / (Ldarker + 0.05).
// ---------------------------------------------------------------------------

namespace
{
    /**
     * An FSlateBrush counts as a SOLID BACKGROUND only if it paints a flat color UNDER the text.
     * The three conditions, and why each one:
     *   - no ResourceObject -- with a texture or material the color is a TINT and the real
     *     background is the image, which cannot be averaged here without lying (same criterion
     *     as for UImage);
     *   - a DrawAs that FILLS the center. WARNING: ESlateBrushDrawType::Border does NOT count,
     *     and it is the enum's trap: it draws the sides and leaves the CENTER EMPTY -- exactly
     *     where the text falls. NoDrawType paints nothing either;
     *   - TintColor with its OWN color. An FSlateColor with UseColor_Foreground inherits it from
     *     the style at runtime, and GetSpecifiedColor() would return a value nobody will see --
     *     the same reasoning the text-color reader already applies further down.
     */
    bool BrushAsSolidBackground_A11y(const FSlateBrush& Brush, FLinearColor& OutColor)
    {
        if (Brush.GetResourceObject() != nullptr) { return false; }

        const ESlateBrushDrawType::Type Draw = Brush.GetDrawType();
        if (Draw != ESlateBrushDrawType::Box
            && Draw != ESlateBrushDrawType::Image
            && Draw != ESlateBrushDrawType::RoundedBox)
        {
            return false;
        }

        if (!Brush.TintColor.IsColorSpecified()) { return false; }

        const FLinearColor C = Brush.TintColor.GetSpecifiedColor();
        if (C.A <= 0.01f) { return false; }

        OutColor = C;
        return true;
    }

    /**
     * Walks up the tree looking for the first solid background. Returns false if it finds none
     * -- which is a result, not a failure: it means "I cannot judge the contrast here".
     */
    bool FindBackgroundColor_A11y(UWidget* Widget, FLinearColor& OutColor, FString& OutSource)
    {
        UWidget* Cursor = Widget ? Widget->GetParent() : nullptr;
        while (Cursor)
        {
            if (UBorder* Border = Cast<UBorder>(Cursor))
            {
                // GetBrushColor() alone returns the tint EVEN IF the brush carries a texture or does
                // not paint the center => it would judge contrasts it cannot see.
                FLinearColor C;
                if (BrushAsSolidBackground_A11y(Border->Background, C))
                {
                    OutColor = C;
                    OutSource = FString::Printf(TEXT("Border:%s"), *Border->GetName());
                    return true;
                }
            }
            else if (UButton* Button = Cast<UButton>(Cursor))
            {
                // A button's background is not a UBorder nor a UImage -- it is its OWN
                // WidgetStyle.Normal. Without this branch, white text over UMG's default gray
                // (0.4955 linear => 1.92:1, a hard fail against the 3.0 floor for large text)
                // would read "contrast_unknown", hiding most of a menu's real issues: the tool
                // would report "no contrast failures" on an unreadable menu. Normal is used
                // because it is the resting state, the one the player sees while reading;
                // Hovered/Pressed are transient and are not audited as background.
                FLinearColor C;
                if (BrushAsSolidBackground_A11y(Button->GetStyle().Normal, C))
                {
                    OutColor = C;
                    OutSource = FString::Printf(TEXT("Button:%s(WidgetStyle.Normal)"), *Button->GetName());
                    return true;
                }
            }
            else if (UImage* Image = Cast<UImage>(Cursor))
            {
                // Only counts if it is a flat color: with a texture or material the color is a TINT
                // and the real background is the image, which cannot be averaged here without lying.
                if (!Image->GetBrush().GetResourceObject())
                {
                    const FLinearColor C = Image->GetColorAndOpacity();
                    if (C.A > 0.01f)
                    {
                        OutColor = C;
                        OutSource = FString::Printf(TEXT("Image:%s"), *Image->GetName());
                        return true;
                    }
                }
            }
            Cursor = Cursor->GetParent();
        }
        return false;
    }
}

// MCP:audit_widget_accessibility - Sweep Widget Blueprints for the accessibility problems that are measurable from what was AUTHORED: text/background contrast against the WCAG 2.x threshold, text smaller than a legible minimum, interactive widgets with no accessible label for a screen reader, and buttons below a minimum touch/focus size. On console this is a CERTIFICATION requirement, not a nicety. READ THE LIMITS BEFORE TRUSTING A PASS: contrast is computed against the FIRST ancestor holding a solid colour -- a UBorder's brush, a UButton's WidgetStyle.Normal, or a flat UImage -- so a background painted by a texture, a material, a video or a sibling drawn underneath is INVISIBLE to this, and those cases come back as background_source:"none" rather than quietly passing, because "could not judge" and "fine" are different answers. A brush only counts when it has no ResourceObject, its TintColor is UseColor_Specified, and its DrawAs actually FILLS the middle: Box, Image or RoundedBox. DrawAs=Border is excluded on purpose (it tiles the sides and leaves the centre empty, which is exactly where the text sits). Colours coming from a CommonUI style asset or a property binding are equally invisible: only authored values are read. What IS exact is the maths -- relative luminance 0.2126R+0.7152G+0.0722B over LINEAR values (which is what an FLinearColor already is) and ratio (Llight+0.05)/(Ldark+0.05), straight from WCAG.
//   @param folder_path - Content folder to sweep (default /Game).
//   @param recursive [bool] - Recurse into subfolders (default true).
//   @param min_contrast_ratio [number] - Contrast floor for normal-size text (default 4.5, the WCAG AA rule). Text at or above large_text_size only needs 3.0, applied automatically.
//   @param large_text_size [number] - Font size from which text counts as "large" for the contrast rule (default 18.0, per WCAG).
//   @param min_font_size [number] - Smallest legible font size; anything under it is reported (default 12.0).
//   @param min_touch_size [number] - Minimum width/height in pixels for an interactive widget's authored slot size (default 44.0, the common touch-target guideline). Only checkable when the slot actually carries a size -- an auto-sized slot reports size_known:false instead of a false pass.
//   @param max_results - Max Widget Blueprints to load (default 500).
//   @returns { folder_path, widget_blueprints_scanned, widgets_examined, issue_count, issues_by_kind: {low_contrast, contrast_unknown, small_text, missing_accessible_text, small_touch_target}, entries: [{ widget_bp_path, widget_name, widget_class, kind, detail, contrast_ratio?, required_ratio?, text_color?, background_color?, background_source?, font_size?, width?, height? }], truncated }

TSharedPtr<FJsonObject> HandleAuditWidgetAccessibility(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 500);

    double MinContrast = 4.5;   Params->TryGetNumberField(TEXT("min_contrast_ratio"), MinContrast);
    double LargeTextSize = 18.0; Params->TryGetNumberField(TEXT("large_text_size"), LargeTextSize);
    double MinFontSize = 12.0;  Params->TryGetNumberField(TEXT("min_font_size"), MinFontSize);
    double MinTouchSize = 44.0; Params->TryGetNumberField(TEXT("min_touch_size"), MinTouchSize);

    bool bWidgetListTruncated = false;
    TArray<UWidgetBlueprint*> WidgetBPs = AU_CollectWidgetBlueprints(FolderPath, bRecursive, MaxResults, bWidgetListTruncated);

    TArray<TSharedPtr<FJsonValue>> Entries;
    int32 Examined = 0;
    int32 CountLowContrast = 0, CountUnknownContrast = 0, CountSmallText = 0,
          CountNoLabel = 0, CountSmallTouch = 0;

    auto AddIssue = [&Entries](UWidgetBlueprint* WBP, UWidget* W, const TCHAR* Kind, const FString& Detail)
        -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
        E->SetStringField(TEXT("widget_bp_path"), WBP->GetPathName());
        E->SetStringField(TEXT("widget_name"), W->GetName());
        E->SetStringField(TEXT("widget_class"), W->GetClass()->GetName());
        E->SetStringField(TEXT("kind"), Kind);
        E->SetStringField(TEXT("detail"), Detail);
        Entries.Add(MakeShared<FJsonValueObject>(E));
        return E;
    };

    for (UWidgetBlueprint* WBP : WidgetBPs)
    {
        if (!WBP || !WBP->WidgetTree) { continue; }

        TArray<UWidget*> AllWidgets;
        WBP->WidgetTree->GetAllWidgets(AllWidgets);

        for (UWidget* W : AllWidgets)
        {
            if (!W) { continue; }
            ++Examined;

            // --- Text: contrast and size.
            UTextBlock* AsText = Cast<UTextBlock>(W);
            if (AsText)
            {
                const FSlateFontInfo Font = AsText->GetFont();
                const double FontSize = Font.Size;

                if (FontSize > 0.0 && FontSize < MinFontSize)
                {
                    ++CountSmallText;
                    TSharedPtr<FJsonObject> E = AddIssue(WBP, W, TEXT("small_text"), FString::Printf(
                        TEXT("font size %.1f is below the %.1f minimum"), FontSize, MinFontSize));
                    E->SetNumberField(TEXT("font_size"), FontSize);
                }

                // WARNING: an FSlateColor does NOT always carry its own color: with ColorUseRule ==
                // UseColor_Foreground it inherits it from the style at runtime, and GetSpecifiedColor()
                // would return a value nobody will see. Asking first is the difference
                // between measuring and making up a value.
                const FSlateColor TextSlateColor = AsText->GetColorAndOpacity();
                if (!TextSlateColor.IsColorSpecified())
                {
                    ++CountUnknownContrast;
                    TSharedPtr<FJsonObject> E = AddIssue(WBP, W, TEXT("contrast_unknown"),
                        TEXT("the text colour is inherited from the style at runtime "
                             "(FSlateColor is not UseColor_Specified): it cannot be judged statically"));
                    E->SetStringField(TEXT("background_source"), TEXT("n/a"));
                    continue;
                }
                const FLinearColor TextColor = TextSlateColor.GetSpecifiedColor();
                FLinearColor BgColor;
                FString BgSource;
                if (FindBackgroundColor_A11y(W, BgColor, BgSource))
                {
                    const double Ratio = UMGWidgetUtils::WcagContrast(TextColor, BgColor);
                    const double Required = (FontSize >= LargeTextSize) ? 3.0 : MinContrast;
                    if (Ratio < Required)
                    {
                        ++CountLowContrast;
                        TSharedPtr<FJsonObject> E = AddIssue(WBP, W, TEXT("low_contrast"), FString::Printf(
                            TEXT("contrast %.2f:1 against %s is below the %.1f:1 required at size %.1f"),
                            Ratio, *BgSource, Required, FontSize));
                        E->SetNumberField(TEXT("contrast_ratio"), Ratio);
                        E->SetNumberField(TEXT("required_ratio"), Required);
                        E->SetNumberField(TEXT("font_size"), FontSize);
                        E->SetObjectField(TEXT("text_color"), UMGWidgetUtils::ColorToJsonWithHex(TextColor));
                        E->SetObjectField(TEXT("background_color"), UMGWidgetUtils::ColorToJsonWithHex(BgColor));
                        E->SetStringField(TEXT("background_source"), BgSource);
                    }
                }
                else
                {
                    // This is NOT a pass: it is a "cannot judge it", and it says so.
                    ++CountUnknownContrast;
                    TSharedPtr<FJsonObject> E = AddIssue(WBP, W, TEXT("contrast_unknown"), FString::Printf(
                        TEXT("no solid background found above this text: contrast could NOT be judged "
                             "(the background may be a texture, a material or a sibling widget)")));
                    E->SetObjectField(TEXT("text_color"), UMGWidgetUtils::ColorToJsonWithHex(TextColor));
                    E->SetStringField(TEXT("background_source"), TEXT("none"));
                }
            }

            // --- Interactive widgets: accessible label and target size.
            const bool bInteractive = W->IsA<UButton>() || W->IsA<USlider>() || W->IsA<UCheckBox>()
                                   || W->IsA<UComboBoxString>() || W->IsA<UEditableTextBox>();
            if (bInteractive)
            {
                const bool bHasLabel =
                    (W->AccessibleBehavior != ESlateAccessibleBehavior::NotAccessible)
                    && !(W->AccessibleBehavior == ESlateAccessibleBehavior::Custom && W->AccessibleText.IsEmpty());
                if (!bHasLabel)
                {
                    ++CountNoLabel;
                    AddIssue(WBP, W, TEXT("missing_accessible_text"), FString::Printf(
                        TEXT("interactive widget with AccessibleBehavior=%s and no usable accessible text: "
                             "a screen reader has nothing to announce"),
                        *StaticEnum<ESlateAccessibleBehavior>()->GetNameStringByValue(
                            static_cast<int64>(W->AccessibleBehavior))));
                }

                if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(W->Slot))
                {
                    const FVector2D Size = CanvasSlot->GetSize();
                    const bool bAutoSized = CanvasSlot->GetAutoSize();
                    if (!bAutoSized && (Size.X > 0.0 || Size.Y > 0.0))
                    {
                        if (Size.X < MinTouchSize || Size.Y < MinTouchSize)
                        {
                            ++CountSmallTouch;
                            TSharedPtr<FJsonObject> E = AddIssue(WBP, W, TEXT("small_touch_target"),
                                FString::Printf(TEXT("authored size %.0fx%.0f is under the %.0f minimum"),
                                    Size.X, Size.Y, MinTouchSize));
                            E->SetNumberField(TEXT("width"), Size.X);
                            E->SetNumberField(TEXT("height"), Size.Y);
                            E->SetBoolField(TEXT("size_known"), true);
                        }
                    }
                }
            }
        }
    }

    TSharedPtr<FJsonObject> ByKind = MakeShared<FJsonObject>();
    ByKind->SetNumberField(TEXT("low_contrast"), CountLowContrast);
    ByKind->SetNumberField(TEXT("contrast_unknown"), CountUnknownContrast);
    ByKind->SetNumberField(TEXT("small_text"), CountSmallText);
    ByKind->SetNumberField(TEXT("missing_accessible_text"), CountNoLabel);
    ByKind->SetNumberField(TEXT("small_touch_target"), CountSmallTouch);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("folder_path"), FolderPath);
    Result->SetNumberField(TEXT("widget_blueprints_scanned"), WidgetBPs.Num());
    Result->SetNumberField(TEXT("widgets_examined"), Examined);
    Result->SetNumberField(TEXT("issue_count"), Entries.Num());
    Result->SetObjectField(TEXT("issues_by_kind"), ByKind);
    Result->SetArrayField(TEXT("entries"), Entries);
    Result->SetBoolField(TEXT("truncated"), bWidgetListTruncated);
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("audit_asset_references"), &HandleAuditAssetReferences },
    { TEXT("audit_blueprint_health"), &HandleAuditBlueprintHealth },
    { TEXT("audit_collision_profiles"), &HandleAuditCollisionProfiles },
    { TEXT("audit_replication"), &HandleAuditReplication },
    { TEXT("audit_widget_accessibility"), &HandleAuditWidgetAccessibility },
    { TEXT("audit_widget_style"), &HandleAuditWidgetStyle },
    { TEXT("audit_widget_text"), &HandleAuditWidgetText },
    { TEXT("capture_pie_state"), &HandleCapturePieState },
    { TEXT("read_mcp_audit_file"), &HandleReadMcpAuditFile },
};
} // namespace UEAgentAuditCommands
