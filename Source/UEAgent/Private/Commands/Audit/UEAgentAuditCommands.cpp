// Copyright (c) 2026 CarlxsMG.

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

// Content and level audits: texture and mesh settings read from tags (or the loaded asset), and placed actors.
#include "AssetRegistry/ARFilter.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "PhysicsEngine/BodySetup.h"
#include "RenderUtils.h"                                  // IsStaticLightingAllowed
#include "UObject/ObjectRedirector.h"
#include "GameFramework/Info.h"
#include "GameFramework/Controller.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/StaticMeshActor.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/MeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"

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
 * potentially large report just to know "did this pass"; get_mcp_audit_report(path) reads a report back on
 * demand. The UMG audits return their entries inline.
 */
namespace UEAgentAuditCommands
{

// MCP-DOMAIN: Audit

namespace
{
    // Writes Report to Saved/MCPAudits/<Prefix>_<UTC timestamp>.json and puts in OutPath the full absolute
    // path written (what get_mcp_audit_report expects): the error reply when the file cannot be written.
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

// MCP:audit_replication - Project-wide replication audit: for every Blueprint under folder_path, which variables are Replicated (CPF_Net), whether the class itself replicates (AActor::GetIsReplicated on the GeneratedClass CDO), and which UFUNCTIONs are RPCs (FUNC_NetFuncFlags). It returns only a SUMMARY plus a report path -- the full per-Blueprint detail is written to Saved/MCPAudits/ and must be read back with get_mcp_audit_report. For one actor rather than a sweep, use get_actor_replication_info.
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
    // (same guardrail list_derived_classes uses).
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

// MCP:audit_blueprint_health - Project-wide compile-sweep: RECOMPILES every Blueprint under folder_path and records error/warning counts. By far the heaviest audit here (it compiles, it does not just read metadata) and it runs synchronously in the editor -- ALWAYS scope it with folder_path on a large project, and lean on max_results, which is a hard cap. It returns only a summary plus a report path; read the detail back with get_mcp_audit_report. To find already-broken Blueprints WITHOUT recompiling anything, use find_erroring_blueprints instead.
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

// MCP:audit_collision_profiles - Aggregate report of every project collision profile AND every collision channel (built-in + custom), including which custom ECC_GameTraceChannelN slots are already taken -- the question to ask before add_collision_channel. Cheap: it reads the collision settings in memory, with no AssetRegistry scan and no asset loading, so unlike the other audit_* sweeps it takes no folder_path and no max_results. Returns a summary + a report path (read the detail with get_mcp_audit_report).
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

// MCP:audit_asset_references - Sweeps the assets under folder_path for two reference problems: orphans (assets no package references, by Asset Registry metadata) and broken references (a dependency on a package that is no longer on disk; /Script/ C++ modules are skipped). Metadata-only: it never loads, changes or saves an asset, so it scales to large projects. Not read-only, though: the full lists go to a new JSON report under <Project>/Saved/MCPAudits/ (a call within the same second replaces the previous one), and the reply carries only the counts and the report path; read the lists with get_mcp_audit_report. Orphans are candidates, not proof: levels and assets loaded by name or from config have no referencers either. For one asset use get_asset_referencers or get_asset_dependencies; for unused assets only, find_unused_assets; delete with delete_asset and clean redirectors with fixup_redirectors.
//   @param folder_path [string] - Content folder to scan, e.g. "/Game/Props" or a plugin mount such as "/MyPlugin". Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param max_results [number] - Cap on assets processed, clamped to 1-100000; defaults to 5000. The summary has no truncation flag: asset_count equal to the cap means the sweep was cut.
//   @param dependency_type [string] - "hard"|"soft"|"all" (case-insensitive), default "all"; any other value is treated as "all". Scopes BOTH the referencer check (orphan detection) and the dependency check (broken-reference detection) to that dependency kind. Use "hard" to find only the broken references that actually break loading -- a broken SOFT reference (TSoftObjectPtr/FSoftObjectPath) does not. "all" applies no Hard/Soft requirement at all.
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
//   get_blueprint_components).
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
// MCP:capture_pie_state - Snapshot of every actor in the ACTIVE PIE world, with every property flagged Replicated or SaveGame (generic -- no judgment call about what is "relevant"). It requires a live PIE session: with none running for pie_client_index it errors cleanly instead of silently snapshotting the EDITOR world, which is what makes it usable as co-op evidence -- point it at a client index to prove a value really replicated. On a large world ALWAYS pass class_filter and/or max_actors. Returns a summary plus a report path; read the detail with get_mcp_audit_report.
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
        TEXT("No component data by design: PIE component counts are not a structural measure (runtime-spawned components, e.g. Niagara, make structurally identical actors differ). Use verify_reparent_success / diff_blueprint_components / get_blueprint_components for structure."));
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

// MCP:get_mcp_audit_report - Read back the FULL report that an audit_* or capture_pie_state command wrote to disk. Those commands return only a summary plus a path, so this is the second half of every one of them -- without it you never see the per-Blueprint / per-asset / per-actor detail. Restricted to Saved/MCPAudits/: it is not a general file reader.
//   @renamed-from read_mcp_audit_file
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

// ---------------------------------------------------------------------------
// CONTENT AND LEVEL AUDITS -- textures, static meshes, asset names and placed actors.
//
// These four answer inline with one shape: findings [{severity, rule, asset|actor, message,
// suggested_fix, ...}], a summary counted over every finding (not only the returned page), and
// offset/limit paging. The asset audits read Asset Registry tags, so they scale without loading;
// an asset is loaded only when the tags a rule needs are missing (a package saved before the
// engine wrote them), and max_loads caps that.
// ---------------------------------------------------------------------------

namespace
{
    enum class EAU_Severity : uint8 { Error, Warning, Info };

    const TCHAR* AU_SeverityName(EAU_Severity Severity)
    {
        switch (Severity)
        {
            case EAU_Severity::Error:   return TEXT("error");
            case EAU_Severity::Warning: return TEXT("warning");
            default:                    return TEXT("info");
        }
    }

    struct FAU_Finding
    {
        EAU_Severity Severity = EAU_Severity::Info;
        FString SortKey;
        FString Rule;
        TSharedPtr<FJsonObject> Json;
    };

    /**
     * The findings of one audit call: which rules run, every finding, and the counts. Rules are
     * checked against the audit's own list so that a misspelled rule is an error instead of a
     * silently empty report.
     */
    class FAU_FindingSet
    {
    public:
        // Reads the optional "rules" array against KnownRules. Returns the error reply when a rule is unknown.
        TSharedPtr<FJsonObject> Init(const TSharedPtr<FJsonObject>& Params, const TArray<FString>& KnownRules)
        {
            Known = KnownRules;
            const TArray<TSharedPtr<FJsonValue>>* RulesArr = nullptr;
            if (Params->TryGetArrayField(TEXT("rules"), RulesArr) && RulesArr)
            {
                for (const TSharedPtr<FJsonValue>& Value : *RulesArr)
                {
                    FString Rule;
                    if (!Value.IsValid() || !Value->TryGetString(Rule) || Rule.IsEmpty()) { continue; }
                    if (!Known.Contains(Rule))
                    {
                        return mcp_common::MakeErrorReply(FString::Printf(
                            TEXT("Unknown rule '%s'. Known rules: %s"), *Rule, *FString::Join(Known, TEXT(", "))));
                    }
                    Enabled.Add(Rule);
                }
            }
            return nullptr;
        }

        bool IsEnabled(const TCHAR* Rule) const
        {
            return Enabled.Num() == 0 || Enabled.Contains(Rule);
        }

        TArray<FString> EnabledRules() const
        {
            TArray<FString> Out;
            for (const FString& Rule : Known)
            {
                if (IsEnabled(*Rule)) { Out.Add(Rule); }
            }
            return Out;
        }

        // Adds a finding on TargetField (e.g. "asset" or "actor") and returns its object, for extra detail fields.
        TSharedPtr<FJsonObject> Add(EAU_Severity Severity, const TCHAR* Rule, const TCHAR* TargetField,
                                    const FString& Target, const FString& Message, const FString& SuggestedFix)
        {
            TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
            Json->SetStringField(TEXT("severity"), AU_SeverityName(Severity));
            Json->SetStringField(TEXT("rule"), Rule);
            Json->SetStringField(TargetField, Target);
            Json->SetStringField(TEXT("message"), Message);
            Json->SetStringField(TEXT("suggested_fix"), SuggestedFix);

            FAU_Finding& Finding = Items.AddDefaulted_GetRef();
            Finding.Severity = Severity;
            Finding.SortKey = Target;
            Finding.Rule = Rule;
            Finding.Json = Json;
            return Json;
        }

        // Sorts (severity first, then target and rule, so a page is stable across calls) and writes
        // summary, findings and paging into Result.
        void WriteTo(const TSharedPtr<FJsonObject>& Result, int32 Offset, int32 Limit)
        {
            Items.StableSort([](const FAU_Finding& A, const FAU_Finding& B)
            {
                if (A.Severity != B.Severity) { return static_cast<uint8>(A.Severity) < static_cast<uint8>(B.Severity); }
                const int32 ByTarget = A.SortKey.Compare(B.SortKey, ESearchCase::IgnoreCase);
                if (ByTarget != 0) { return ByTarget < 0; }
                return A.Rule < B.Rule;
            });

            int32 Errors = 0, Warnings = 0, Infos = 0;
            TSharedPtr<FJsonObject> ByRule = MakeShared<FJsonObject>();
            for (const FString& Rule : EnabledRules()) { ByRule->SetNumberField(Rule, 0); }
            TMap<FString, int32> RuleCounts;
            for (const FAU_Finding& Finding : Items)
            {
                if (Finding.Severity == EAU_Severity::Error) { ++Errors; }
                else if (Finding.Severity == EAU_Severity::Warning) { ++Warnings; }
                else { ++Infos; }
                ++RuleCounts.FindOrAdd(Finding.Rule);
            }
            for (const TPair<FString, int32>& Pair : RuleCounts) { ByRule->SetNumberField(Pair.Key, Pair.Value); }

            TSharedPtr<FJsonObject> BySeverity = MakeShared<FJsonObject>();
            BySeverity->SetNumberField(TEXT("error"), Errors);
            BySeverity->SetNumberField(TEXT("warning"), Warnings);
            BySeverity->SetNumberField(TEXT("info"), Infos);

            TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
            Summary->SetNumberField(TEXT("finding_count"), Items.Num());
            Summary->SetObjectField(TEXT("by_severity"), BySeverity);
            Summary->SetObjectField(TEXT("by_rule"), ByRule);
            Result->SetObjectField(TEXT("summary"), Summary);
            Result->SetArrayField(TEXT("rules"), mcp_common::StringsToJson(EnabledRules()));

            TArray<TSharedPtr<FJsonValue>> Page;
            const int32 End = FMath::Min(Items.Num(), Offset + Limit);
            for (int32 i = Offset; i < End; ++i)
            {
                Page.Add(MakeShared<FJsonValueObject>(Items[i].Json));
            }
            Result->SetArrayField(TEXT("findings"), Page);
            Result->SetNumberField(TEXT("offset"), Offset);
            Result->SetNumberField(TEXT("limit"), Limit);
            Result->SetNumberField(TEXT("returned"), Page.Num());
            if (End < Items.Num())
            {
                Result->SetNumberField(TEXT("next_offset"), End);
            }
            else
            {
                Result->SetField(TEXT("next_offset"), MakeShared<FJsonValueNull>());
            }
        }

    private:
        TArray<FString> Known;
        TSet<FString> Enabled;
        TArray<FAU_Finding> Items;
    };

    // offset (default 0, at least 0) and limit (default 200, clamped 1-5000) of the findings page.
    void AU_GetPaging(const TSharedPtr<FJsonObject>& Params, int32& OutOffset, int32& OutLimit)
    {
        double OffsetD = 0.0, LimitD = 200.0;
        Params->TryGetNumberField(TEXT("offset"), OffsetD);
        Params->TryGetNumberField(TEXT("limit"), LimitD);
        OutOffset = FMath::Max(0, static_cast<int32>(OffsetD));
        OutLimit = FMath::Clamp(static_cast<int32>(LimitD), 1, 5000);
    }

    int32 AU_GetIntParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, int32 DefaultValue, int32 Min, int32 Max)
    {
        double D = DefaultValue;
        Params->TryGetNumberField(Key, D);
        return FMath::Clamp(static_cast<int32>(D), Min, Max);
    }

    double AU_GetDoubleParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, double DefaultValue)
    {
        double D = DefaultValue;
        Params->TryGetNumberField(Key, D);
        return D;
    }

    // folder_path must be a mounted content path ("/Game/...", "/MyPlugin/..."): anything else would
    // silently match nothing and read as a clean project.
    TSharedPtr<FJsonObject> AU_ValidateFolderPath(const FString& FolderPath)
    {
        if (!FolderPath.StartsWith(TEXT("/")) || FolderPath.Contains(TEXT("\\")))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("folder_path '%s' must be a content path such as /Game or /Game/Props"), *FolderPath));
        }
        return nullptr;
    }

    // The assets of exactly ClassPath (subclasses excluded) under FolderPath, sorted by package name
    // so that paging is stable. bOutTruncated is set when more than MaxAssets matched.
    TArray<FAssetData> AU_CollectAssetsOfClass(const FString& FolderPath, bool bRecursive,
                                               const FTopLevelAssetPath& ClassPath, int32 MaxAssets, bool& bOutTruncated)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*FolderPath));
        Filter.bRecursivePaths = bRecursive;
        if (!ClassPath.IsNull())
        {
            Filter.ClassPaths.Add(ClassPath);
        }
        Filter.bRecursiveClasses = false;

        TArray<FAssetData> Assets;
        mcp_common::AssetRegistry().GetAssets(Filter, Assets);
        // World Partition's one-file-per-actor packages are not authored assets.
        Assets.RemoveAll([](const FAssetData& AD)
        {
            return AD.PackagePath.ToString().Contains(TEXT("/__External"));
        });
        Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.Compare(B.PackageName) < 0; });

        bOutTruncated = Assets.Num() > MaxAssets;
        if (bOutTruncated)
        {
            Assets.SetNum(MaxAssets);
        }
        return Assets;
    }

    // "TC_Normalmap", "TextureCompressionSettings::TC_Normalmap" and "tc_normalmap" all compare equal.
    bool AU_EnumIs(const FString& Value, const TCHAR* Name)
    {
        int32 Sep = INDEX_NONE;
        FString Short = Value;
        if (Value.FindLastChar(TEXT(':'), Sep))
        {
            Short = Value.Mid(Sep + 1);
        }
        return Short.Equals(Name, ESearchCase::IgnoreCase);
    }

    bool AU_TagBool(const FAssetData& AD, const TCHAR* Tag, bool& OutValue)
    {
        FString Value;
        if (!AD.GetTagValue(FName(Tag), Value) || Value.IsEmpty()) { return false; }
        OutValue = Value.Equals(TEXT("True"), ESearchCase::IgnoreCase) || Value == TEXT("1");
        return true;
    }

    bool AU_TagInt(const FAssetData& AD, const TCHAR* Tag, int64& OutValue)
    {
        FString Value;
        if (!AD.GetTagValue(FName(Tag), Value) || Value.IsEmpty() || !Value.IsNumeric()) { return false; }
        OutValue = FCString::Atoi64(*Value);
        return true;
    }

    // The last "_" token of an asset name, which carries the texture's role by convention (T_Rock_N, T_Rock_ORM).
    FString AU_NameSuffix(const FString& AssetName)
    {
        int32 Sep = INDEX_NONE;
        return AssetName.FindLastChar(TEXT('_'), Sep) ? AssetName.Mid(Sep + 1) : FString();
    }

    bool AU_SuffixIn(const FString& Suffix, std::initializer_list<const TCHAR*> Candidates)
    {
        for (const TCHAR* Candidate : Candidates)
        {
            if (Suffix.Equals(Candidate, ESearchCase::IgnoreCase)) { return true; }
        }
        return false;
    }

    /** What audit_textures reads of one texture, from tags or, when they are missing, from the loaded texture. */
    struct FAU_TextureInfo
    {
        int32 Width = 0;
        int32 Height = 0;
        FString Compression;
        FString LODGroup;
        FString MipGen;        // editor-only data: empty when neither tag nor object has it
        FString PowerOfTwo;
        int32 MaxTextureSize = 0;
        bool bSRGB = false;
        bool bNeverStream = false;
        bool bVirtualTexture = false;
        bool bComplete = false;
    };

    FAU_TextureInfo AU_TextureInfoFromTags(const FAssetData& AD)
    {
        FAU_TextureInfo Info;
        FString Dimensions;
        bool bHasDims = false;
        if (AD.GetTagValue(FName(TEXT("Dimensions")), Dimensions))
        {
            FString W, H;
            if (Dimensions.Split(TEXT("x"), &W, &H))
            {
                Info.Width = FCString::Atoi(*W);
                Info.Height = FCString::Atoi(*H);
                bHasDims = Info.Width > 0 && Info.Height > 0;
            }
        }
        const bool bHasCompression = AD.GetTagValue(FName(TEXT("CompressionSettings")), Info.Compression) && !Info.Compression.IsEmpty();
        const bool bHasGroup = AD.GetTagValue(FName(TEXT("LODGroup")), Info.LODGroup) && !Info.LODGroup.IsEmpty();
        const bool bHasSRGB = AU_TagBool(AD, TEXT("SRGB"), Info.bSRGB);
        AD.GetTagValue(FName(TEXT("MipGenSettings")), Info.MipGen);
        AD.GetTagValue(FName(TEXT("PowerOfTwoMode")), Info.PowerOfTwo);
        int64 MaxSize = 0;
        if (AU_TagInt(AD, TEXT("MaxTextureSize"), MaxSize)) { Info.MaxTextureSize = static_cast<int32>(MaxSize); }
        const bool bHasNeverStream = AU_TagBool(AD, TEXT("NeverStream"), Info.bNeverStream);
        AU_TagBool(AD, TEXT("VirtualTextureStreaming"), Info.bVirtualTexture);
        Info.bComplete = bHasDims && bHasCompression && bHasGroup && bHasSRGB && bHasNeverStream;
        return Info;
    }

    FAU_TextureInfo AU_TextureInfoFromObject(const UTexture2D* Tex)
    {
        FAU_TextureInfo Info;
        const FIntPoint Size = Tex->GetImportedSize();
        Info.Width = Size.X;
        Info.Height = Size.Y;
        Info.Compression = StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(Tex->CompressionSettings.GetValue()));
        Info.LODGroup = StaticEnum<TextureGroup>()->GetNameStringByValue(static_cast<int64>(Tex->LODGroup.GetValue()));
#if WITH_EDITORONLY_DATA
        Info.MipGen = StaticEnum<TextureMipGenSettings>()->GetNameStringByValue(static_cast<int64>(Tex->MipGenSettings.GetValue()));
        Info.PowerOfTwo = StaticEnum<ETexturePowerOfTwoSetting::Type>()->GetNameStringByValue(static_cast<int64>(Tex->PowerOfTwoMode.GetValue()));
        Info.MaxTextureSize = Tex->MaxTextureSize;
#endif
        Info.bSRGB = Tex->SRGB != 0;
        Info.bNeverStream = Tex->NeverStream != 0;
        Info.bVirtualTexture = Tex->VirtualTextureStreaming != 0;
        Info.bComplete = Info.Width > 0 && Info.Height > 0;
        return Info;
    }

    /** What audit_static_meshes reads of one mesh, from tags or, when they are missing, from the loaded mesh. */
    struct FAU_MeshInfo
    {
        int64 Triangles = 0;
        int64 LODs = 0;
        int64 CollisionPrims = 0;
        int64 Materials = 0;
        int64 UVChannels = 0;
        int32 LightMapCoordinateIndex = INDEX_NONE;   // only known when the mesh was loaded
        bool bNanite = false;
        FString CollisionComplexity;
        FString DefaultCollision;
        FVector Size = FVector::ZeroVector;
        bool bComplete = false;
    };

    FAU_MeshInfo AU_MeshInfoFromTags(const FAssetData& AD)
    {
        FAU_MeshInfo Info;
        const bool bTris = AU_TagInt(AD, TEXT("Triangles"), Info.Triangles);
        const bool bLODs = AU_TagInt(AD, TEXT("LODs"), Info.LODs);
        const bool bPrims = AU_TagInt(AD, TEXT("CollisionPrims"), Info.CollisionPrims);
        const bool bMats = AU_TagInt(AD, TEXT("Materials"), Info.Materials);
        const bool bUVs = AU_TagInt(AD, TEXT("UVChannels"), Info.UVChannels);
        AU_TagBool(AD, TEXT("NaniteEnabled"), Info.bNanite);
        AD.GetTagValue(FName(TEXT("CollisionComplexity")), Info.CollisionComplexity);
        AD.GetTagValue(FName(TEXT("DefaultCollision")), Info.DefaultCollision);

        bool bSize = false;
        FString Approx;
        if (AD.GetTagValue(FName(TEXT("ApproxSize")), Approx))
        {
            TArray<FString> Parts;
            Approx.ParseIntoArray(Parts, TEXT("x"));
            if (Parts.Num() == 3)
            {
                Info.Size = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
                bSize = true;
            }
        }
        Info.bComplete = bTris && bLODs && bPrims && bMats && bUVs && bSize;
        return Info;
    }

    FAU_MeshInfo AU_MeshInfoFromObject(const UStaticMesh* Mesh)
    {
        FAU_MeshInfo Info;
        Info.LODs = Mesh->GetNumLODs();
        Info.Triangles = Info.LODs > 0 ? Mesh->GetNumTriangles(0) : 0;
        Info.UVChannels = Info.LODs > 0 ? Mesh->GetNumUVChannels(0) : 0;
        Info.LightMapCoordinateIndex = Mesh->GetLightMapCoordinateIndex();
        Info.bNanite = Mesh->IsNaniteEnabled();
        Info.Materials = Mesh->GetStaticMaterials().Num();
        if (const UBodySetup* Body = Mesh->GetBodySetup())
        {
            Info.CollisionPrims = Body->AggGeom.GetElementCount();
            Info.CollisionComplexity = LexToString(Body->GetCollisionTraceFlag().GetValue());
            Info.DefaultCollision = Body->DefaultInstance.GetCollisionProfileName().ToString();
        }
        Info.Size = Mesh->GetBounds().BoxExtent * 2.0;
        Info.bComplete = true;
        return Info;
    }
}

// MCP:audit_textures - Sweep the Texture2D assets under folder_path for the settings that cost memory or look wrong: non-power-of-two size, oversized versus a threshold, no mips, normal maps without Normalmap compression or with sRGB on, sRGB on mask textures (ORM/roughness/AO...), a normal map or UI texture left in a generic texture group, and streaming disabled. Reads Asset Registry tags, so nothing is loaded; a texture whose package predates those tags is loaded to read them, up to max_loads. Returns findings inline, paged with offset/limit; the summary counts every finding. Read-only.
//   @param folder_path [string] - Content folder to scan. Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param max_texture_size [number] - Largest acceptable dimension in pixels after Maximum Texture Size is applied. Defaults to 4096.
//   @param rules [array] - Rule ids to run, default all: texture_non_power_of_two, texture_oversized, texture_no_mips, texture_normal_compression, texture_normal_srgb, texture_mask_srgb, texture_lod_group, texture_streaming_disabled. An unknown id is an error.
//   @param max_results [number] - Cap on textures examined (1-100000). Defaults to 20000.
//   @param max_loads [number] - Cap on textures loaded because their tags are missing (0-5000; 0 never loads). Defaults to 200.
//   @param offset [number] - Index of the first finding returned. Defaults to 0.
//   @param limit [number] - Findings returned per call (1-5000). Defaults to 200.
//   @returns { folder_path, scanned: int, scan_truncated: bool, loaded_count: int, skipped_no_tags: int, rules: [string], summary: { finding_count, by_severity: {error, warning, info}, by_rule: {rule: int} }, findings: [{ severity, rule, asset, message, suggested_fix, width?, height?, compression?, lod_group? }], offset, limit, returned, next_offset: int|null }
//   Normal/mask roles come from the name's last "_" token (N, Normal, NRM / M, Mask, ORM, ARM, R, AO...) and from CompressionSettings.
TSharedPtr<FJsonObject> HandleAuditTextures(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    if (TSharedPtr<FJsonObject> Bad = AU_ValidateFolderPath(FolderPath)) { return Bad; }
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 20000);
    const int32 MaxLoads = AU_GetIntParam(Params, TEXT("max_loads"), 200, 0, 5000);
    const int32 MaxTextureSize = AU_GetIntParam(Params, TEXT("max_texture_size"), 4096, 1, 1 << 16);
    int32 Offset = 0, Limit = 200;
    AU_GetPaging(Params, Offset, Limit);

    FAU_FindingSet Findings;
    if (TSharedPtr<FJsonObject> Bad = Findings.Init(Params, {
            TEXT("texture_non_power_of_two"), TEXT("texture_oversized"), TEXT("texture_no_mips"),
            TEXT("texture_normal_compression"), TEXT("texture_normal_srgb"), TEXT("texture_mask_srgb"),
            TEXT("texture_lod_group"), TEXT("texture_streaming_disabled") }))
    {
        return Bad;
    }

    bool bTruncated = false;
    const TArray<FAssetData> Assets = AU_CollectAssetsOfClass(FolderPath, bRecursive,
        UTexture2D::StaticClass()->GetClassPathName(), MaxResults, bTruncated);

    int32 LoadedCount = 0, SkippedNoTags = 0;
    for (const FAssetData& AD : Assets)
    {
        FAU_TextureInfo Info = AU_TextureInfoFromTags(AD);
        if (!Info.bComplete)
        {
            UTexture2D* Tex = nullptr;
            if (LoadedCount < MaxLoads)
            {
                Tex = Cast<UTexture2D>(AD.GetAsset());
                ++LoadedCount;
            }
            if (!Tex) { ++SkippedNoTags; continue; }
            Info = AU_TextureInfoFromObject(Tex);
            if (!Info.bComplete) { ++SkippedNoTags; continue; }
        }

        const FString Asset = AD.GetSoftObjectPath().ToString();
        const FString Suffix = AU_NameSuffix(AD.AssetName.ToString());
        const bool bUIGroup = AU_EnumIs(Info.LODGroup, TEXT("TEXTUREGROUP_UI")) || AU_EnumIs(Info.LODGroup, TEXT("TEXTUREGROUP_Pixels2D"));
        const bool bUICompression = AU_EnumIs(Info.Compression, TEXT("TC_EditorIcon"));
        const bool bLookupTable = AU_EnumIs(Info.LODGroup, TEXT("TEXTUREGROUP_ColorLookupTable"));
        const bool bNormalCompression = AU_EnumIs(Info.Compression, TEXT("TC_Normalmap"));
        const bool bMaskCompression = AU_EnumIs(Info.Compression, TEXT("TC_Masks"));
        const bool bNormalByName = AU_SuffixIn(Suffix, { TEXT("N"), TEXT("NRM"), TEXT("Normal"), TEXT("Normals"), TEXT("Norm"), TEXT("NM") });
        const bool bMaskByName = AU_SuffixIn(Suffix, { TEXT("M"), TEXT("Mask"), TEXT("ORM"), TEXT("ARM"), TEXT("RMA"), TEXT("MRA"),
            TEXT("AO"), TEXT("R"), TEXT("Rough"), TEXT("Roughness"), TEXT("Metal"), TEXT("Metallic"), TEXT("H"), TEXT("Height") });
        const int32 MaxDim = FMath::Max(Info.Width, Info.Height);

        auto AddTex = [&](EAU_Severity Severity, const TCHAR* Rule, const FString& Message, const FString& Fix)
        {
            TSharedPtr<FJsonObject> F = Findings.Add(Severity, Rule, TEXT("asset"), Asset, Message, Fix);
            F->SetNumberField(TEXT("width"), Info.Width);
            F->SetNumberField(TEXT("height"), Info.Height);
            F->SetStringField(TEXT("compression"), Info.Compression);
            F->SetStringField(TEXT("lod_group"), Info.LODGroup);
            return F;
        };

        if (Findings.IsEnabled(TEXT("texture_non_power_of_two"))
            && (!FMath::IsPowerOfTwo(Info.Width) || !FMath::IsPowerOfTwo(Info.Height))
            && (Info.PowerOfTwo.IsEmpty() || AU_EnumIs(Info.PowerOfTwo, TEXT("None"))))
        {
            AddTex(bUIGroup || bUICompression ? EAU_Severity::Info : EAU_Severity::Warning, TEXT("texture_non_power_of_two"),
                FString::Printf(TEXT("%dx%d is not a power of two: it gets no mips and does not stream"), Info.Width, Info.Height),
                TEXT("Resize the source to power-of-two dimensions, or set Power Of Two Mode to Pad To Power Of Two"));
        }

        const int32 EffectiveDim = (Info.MaxTextureSize > 0) ? FMath::Min(MaxDim, Info.MaxTextureSize) : MaxDim;
        if (Findings.IsEnabled(TEXT("texture_oversized")) && EffectiveDim > MaxTextureSize)
        {
            AddTex(EAU_Severity::Warning, TEXT("texture_oversized"),
                FString::Printf(TEXT("%d px exceeds the %d px threshold"), EffectiveDim, MaxTextureSize),
                FString::Printf(TEXT("Set Maximum Texture Size to %d (or raise LOD Bias), or downscale the source"), MaxTextureSize));
        }

        if (Findings.IsEnabled(TEXT("texture_no_mips")) && AU_EnumIs(Info.MipGen, TEXT("TMGS_NoMipmaps"))
            && !bUIGroup && !bUICompression && !bLookupTable)
        {
            AddTex(EAU_Severity::Warning, TEXT("texture_no_mips"),
                TEXT("Mip Gen Settings is NoMipmaps on a world texture: it aliases at distance and keeps full resolution resident"),
                TEXT("Set Mip Gen Settings to FromTextureGroup"));
        }

        if (Findings.IsEnabled(TEXT("texture_normal_compression")) && bNormalByName && !bNormalCompression)
        {
            AddTex(EAU_Severity::Error, TEXT("texture_normal_compression"),
                FString::Printf(TEXT("named as a normal map but compressed as %s%s"), *Info.Compression, Info.bSRGB ? TEXT(" with sRGB on") : TEXT("")),
                TEXT("Set Compression Settings to Normalmap (this also turns sRGB off)"));
        }

        if (Findings.IsEnabled(TEXT("texture_normal_srgb")) && bNormalCompression && Info.bSRGB)
        {
            AddTex(EAU_Severity::Error, TEXT("texture_normal_srgb"),
                TEXT("Normalmap compression with sRGB on: the vectors are gamma-decoded and the shading is wrong"),
                TEXT("Turn sRGB off and resave the texture"));
        }

        if (Findings.IsEnabled(TEXT("texture_mask_srgb")) && Info.bSRGB && !bNormalCompression && (bMaskByName || bMaskCompression))
        {
            AddTex(EAU_Severity::Warning, TEXT("texture_mask_srgb"),
                FString::Printf(TEXT("mask/data texture (suffix '%s', %s) is sampled as sRGB: its linear values are distorted"), *Suffix, *Info.Compression),
                TEXT("Set Compression Settings to Masks, or turn sRGB off"));
        }

        if (Findings.IsEnabled(TEXT("texture_lod_group")))
        {
            if (bNormalCompression && !Info.LODGroup.Contains(TEXT("NormalMap")))
            {
                AddTex(EAU_Severity::Info, TEXT("texture_lod_group"),
                    FString::Printf(TEXT("normal map in texture group %s"), *Info.LODGroup),
                    TEXT("Set Texture Group to WorldNormalMap (or the matching Character/Weapon/Vehicle NormalMap group)"));
            }
            else if (bUICompression && !bUIGroup)
            {
                AddTex(EAU_Severity::Info, TEXT("texture_lod_group"),
                    FString::Printf(TEXT("UserInterface2D texture in texture group %s"), *Info.LODGroup),
                    TEXT("Set Texture Group to UI"));
            }
        }

        if (Findings.IsEnabled(TEXT("texture_streaming_disabled")) && Info.bNeverStream && !Info.bVirtualTexture
            && !bUIGroup && !bUICompression && !bLookupTable && MaxDim > 256)
        {
            AddTex(EAU_Severity::Warning, TEXT("texture_streaming_disabled"),
                FString::Printf(TEXT("Never Stream is on for a %d px world texture: every mip stays resident"), MaxDim),
                TEXT("Turn Never Stream off unless the texture must always be fully resident"));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("folder_path"), FolderPath);
    Result->SetNumberField(TEXT("scanned"), Assets.Num());
    Result->SetBoolField(TEXT("scan_truncated"), bTruncated);
    Result->SetNumberField(TEXT("loaded_count"), LoadedCount);
    Result->SetNumberField(TEXT("skipped_no_tags"), SkippedNoTags);
    Findings.WriteTo(Result, Offset, Limit);
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_static_meshes - Sweep the StaticMesh assets under folder_path for: no simple collision (and not complex-as-simple, and not set to NoCollision), a high triangle count with no LODs and no Nanite, a missing lightmap UV channel when the project allows static lighting, too many material slots, and degenerate bounds (empty mesh, zero size on two axes, or absurdly large). Reads Asset Registry tags, so nothing is loaded; a mesh whose package predates those tags is loaded to read them, up to max_loads. Returns findings inline, paged with offset/limit; the summary counts every finding. Read-only.
//   @param folder_path [string] - Content folder to scan. Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param max_triangles_without_lod [number] - LOD0 triangle count from which a mesh with a single LOD and no Nanite is reported. Defaults to 10000.
//   @param max_material_slots [number] - Largest acceptable number of material slots. Defaults to 8.
//   @param max_bounds_size [number] - Largest acceptable bounds size on any axis, in cm. Defaults to 1000000 (10 km).
//   @param rules [array] - Rule ids to run, default all: mesh_no_collision, mesh_high_tris_no_lod, mesh_missing_lightmap_uv, mesh_too_many_materials, mesh_degenerate_bounds. An unknown id is an error.
//   @param max_results [number] - Cap on meshes examined (1-100000). Defaults to 20000.
//   @param max_loads [number] - Cap on meshes loaded because their tags are missing (0-5000; 0 never loads). Defaults to 200.
//   @param offset [number] - Index of the first finding returned. Defaults to 0.
//   @param limit [number] - Findings returned per call (1-5000). Defaults to 200.
//   @returns { folder_path, scanned: int, scan_truncated: bool, loaded_count: int, skipped_no_tags: int, static_lighting_allowed: bool, rules: [string], summary: { finding_count, by_severity: {error, warning, info}, by_rule: {rule: int} }, findings: [{ severity, rule, asset, message, suggested_fix, triangles?, lods?, nanite?, size?: [x,y,z] }], offset, limit, returned, next_offset: int|null }
//   The ApproxSize tag is rounded to whole cm, so a mesh thinner than 0.5 cm reads as 0 on that axis.
TSharedPtr<FJsonObject> HandleAuditStaticMeshes(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    if (TSharedPtr<FJsonObject> Bad = AU_ValidateFolderPath(FolderPath)) { return Bad; }
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 20000);
    const int32 MaxLoads = AU_GetIntParam(Params, TEXT("max_loads"), 200, 0, 5000);
    const int32 MaxTrisNoLod = AU_GetIntParam(Params, TEXT("max_triangles_without_lod"), 10000, 1, MAX_int32);
    const int32 MaxMaterialSlots = AU_GetIntParam(Params, TEXT("max_material_slots"), 8, 1, 1000);
    const double MaxBoundsSize = FMath::Max(1.0, AU_GetDoubleParam(Params, TEXT("max_bounds_size"), 1000000.0));
    int32 Offset = 0, Limit = 200;
    AU_GetPaging(Params, Offset, Limit);

    FAU_FindingSet Findings;
    if (TSharedPtr<FJsonObject> Bad = Findings.Init(Params, {
            TEXT("mesh_no_collision"), TEXT("mesh_high_tris_no_lod"), TEXT("mesh_missing_lightmap_uv"),
            TEXT("mesh_too_many_materials"), TEXT("mesh_degenerate_bounds") }))
    {
        return Bad;
    }

    const bool bStaticLighting = IsStaticLightingAllowed();

    bool bTruncated = false;
    const TArray<FAssetData> Assets = AU_CollectAssetsOfClass(FolderPath, bRecursive,
        UStaticMesh::StaticClass()->GetClassPathName(), MaxResults, bTruncated);

    int32 LoadedCount = 0, SkippedNoTags = 0;
    for (const FAssetData& AD : Assets)
    {
        FAU_MeshInfo Info = AU_MeshInfoFromTags(AD);
        if (!Info.bComplete)
        {
            UStaticMesh* Mesh = nullptr;
            if (LoadedCount < MaxLoads)
            {
                Mesh = Cast<UStaticMesh>(AD.GetAsset());
                ++LoadedCount;
            }
            if (!Mesh) { ++SkippedNoTags; continue; }
            Info = AU_MeshInfoFromObject(Mesh);
        }

        const FString Asset = AD.GetSoftObjectPath().ToString();
        auto AddMesh = [&](EAU_Severity Severity, const TCHAR* Rule, const FString& Message, const FString& Fix)
        {
            TSharedPtr<FJsonObject> F = Findings.Add(Severity, Rule, TEXT("asset"), Asset, Message, Fix);
            F->SetNumberField(TEXT("triangles"), static_cast<double>(Info.Triangles));
            F->SetNumberField(TEXT("lods"), static_cast<double>(Info.LODs));
            F->SetBoolField(TEXT("nanite"), Info.bNanite);
            F->SetArrayField(TEXT("size"), mcp_common::VectorToJsonArray(Info.Size));
            return F;
        };

        const bool bComplexAsSimple = Info.CollisionComplexity.Equals(TEXT("CTF_UseComplexAsSimple"), ESearchCase::IgnoreCase);
        const bool bNoCollisionProfile = Info.DefaultCollision.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase);
        if (Findings.IsEnabled(TEXT("mesh_no_collision")) && Info.CollisionPrims == 0 && !bComplexAsSimple && !bNoCollisionProfile)
        {
            AddMesh(EAU_Severity::Warning, TEXT("mesh_no_collision"),
                TEXT("no simple collision primitives and not Use Complex As Simple: placed instances block nothing"),
                TEXT("Add simple collision (Static Mesh Editor > Collision, or add_static_mesh_simple_collision), or set the collision preset to NoCollision if that is intended"));
        }

        if (Findings.IsEnabled(TEXT("mesh_high_tris_no_lod")) && Info.Triangles >= MaxTrisNoLod && Info.LODs <= 1 && !Info.bNanite)
        {
            AddMesh(EAU_Severity::Warning, TEXT("mesh_high_tris_no_lod"),
                FString::Printf(TEXT("%lld triangles with a single LOD and Nanite off"), Info.Triangles),
                TEXT("Enable Nanite, or generate LODs (LOD Group or Number of LODs in the Static Mesh Editor)"));
        }

        if (Findings.IsEnabled(TEXT("mesh_missing_lightmap_uv")) && bStaticLighting)
        {
            if (Info.LightMapCoordinateIndex != INDEX_NONE && Info.LightMapCoordinateIndex >= Info.UVChannels)
            {
                AddMesh(EAU_Severity::Error, TEXT("mesh_missing_lightmap_uv"),
                    FString::Printf(TEXT("Light Map Coordinate Index %d points past the %lld UV channel(s)"), Info.LightMapCoordinateIndex, Info.UVChannels),
                    TEXT("Enable Generate Lightmap UVs in the LOD0 build settings, or set Light Map Coordinate Index to an existing channel"));
            }
            else if (Info.UVChannels < 2)
            {
                AddMesh(EAU_Severity::Warning, TEXT("mesh_missing_lightmap_uv"),
                    FString::Printf(TEXT("%lld UV channel(s) while static lighting is allowed: lightmaps share UV0, which usually overlaps"), Info.UVChannels),
                    TEXT("Enable Generate Lightmap UVs in the LOD0 build settings, or author a non-overlapping UV1"));
            }
        }

        if (Findings.IsEnabled(TEXT("mesh_too_many_materials")) && Info.Materials > MaxMaterialSlots)
        {
            TSharedPtr<FJsonObject> F = AddMesh(EAU_Severity::Warning, TEXT("mesh_too_many_materials"),
                FString::Printf(TEXT("%lld material slots exceed %d: each slot is a separate draw call"), Info.Materials, MaxMaterialSlots),
                TEXT("Merge material slots (atlas the textures or combine sections in the DCC tool)"));
            F->SetNumberField(TEXT("material_slots"), static_cast<double>(Info.Materials));
        }

        if (Findings.IsEnabled(TEXT("mesh_degenerate_bounds")))
        {
            const int32 ZeroAxes = (Info.Size.X <= 0.0 ? 1 : 0) + (Info.Size.Y <= 0.0 ? 1 : 0) + (Info.Size.Z <= 0.0 ? 1 : 0);
            if (Info.Triangles == 0)
            {
                AddMesh(EAU_Severity::Error, TEXT("mesh_degenerate_bounds"), TEXT("the mesh has no triangles"),
                    TEXT("Reimport the source or delete the asset"));
            }
            else if (ZeroAxes >= 2)
            {
                AddMesh(EAU_Severity::Error, TEXT("mesh_degenerate_bounds"),
                    FString::Printf(TEXT("bounds %.0fx%.0fx%.0f cm are zero on %d axes"), Info.Size.X, Info.Size.Y, Info.Size.Z, ZeroAxes),
                    TEXT("Check the import scale and the source geometry, then reimport"));
            }
            else if (Info.Size.GetMax() > MaxBoundsSize)
            {
                AddMesh(EAU_Severity::Warning, TEXT("mesh_degenerate_bounds"),
                    FString::Printf(TEXT("bounds %.0fx%.0fx%.0f cm exceed %.0f cm on an axis"), Info.Size.X, Info.Size.Y, Info.Size.Z, MaxBoundsSize),
                    TEXT("Check the import scale (a units mismatch multiplies size by 100) or stray vertices far from the mesh"));
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("folder_path"), FolderPath);
    Result->SetNumberField(TEXT("scanned"), Assets.Num());
    Result->SetBoolField(TEXT("scan_truncated"), bTruncated);
    Result->SetNumberField(TEXT("loaded_count"), LoadedCount);
    Result->SetNumberField(TEXT("skipped_no_tags"), SkippedNoTags);
    Result->SetBoolField(TEXT("static_lighting_allowed"), bStaticLighting);
    Findings.WriteTo(Result, Offset, Limit);
    return mcp_common::MakeOkReply(Result);
}

namespace
{
    // The prefixes a class's assets should start with, keyed by the class's short name. Blueprints are
    // split by their BlueprintType tag into Blueprint, BlueprintInterface, BlueprintFunctionLibrary and
    // BlueprintMacroLibrary. Taken from Epic's recommended asset naming conventions; a second entry is
    // a widespread alternative.
    TMap<FString, TArray<FString>> AU_DefaultNamingPrefixes()
    {
        TMap<FString, TArray<FString>> Map;
        auto P = [&Map](const TCHAR* ClassName, std::initializer_list<const TCHAR*> Accepted)
        {
            TArray<FString>& List = Map.Add(ClassName);
            for (const TCHAR* Prefix : Accepted) { List.Add(Prefix); }
        };
        P(TEXT("Blueprint"), { TEXT("BP_") });
        P(TEXT("BlueprintInterface"), { TEXT("BPI_") });
        P(TEXT("BlueprintFunctionLibrary"), { TEXT("BPFL_") });
        P(TEXT("BlueprintMacroLibrary"), { TEXT("BPML_") });
        P(TEXT("WidgetBlueprint"), { TEXT("WBP_"), TEXT("W_") });
        P(TEXT("EditorUtilityWidgetBlueprint"), { TEXT("EUW_") });
        P(TEXT("AnimBlueprint"), { TEXT("ABP_") });
        P(TEXT("ControlRigBlueprint"), { TEXT("CR_") });
        P(TEXT("Material"), { TEXT("M_") });
        P(TEXT("MaterialInstanceConstant"), { TEXT("MI_") });
        P(TEXT("MaterialFunction"), { TEXT("MF_") });
        P(TEXT("MaterialParameterCollection"), { TEXT("MPC_") });
        P(TEXT("PhysicalMaterial"), { TEXT("PM_") });
        P(TEXT("Texture2D"), { TEXT("T_") });
        P(TEXT("TextureCube"), { TEXT("T_"), TEXT("HDR_") });
        P(TEXT("TextureRenderTarget2D"), { TEXT("RT_") });
        P(TEXT("StaticMesh"), { TEXT("SM_") });
        P(TEXT("SkeletalMesh"), { TEXT("SK_"), TEXT("SKM_") });
        P(TEXT("Skeleton"), { TEXT("SKEL_") });
        P(TEXT("PhysicsAsset"), { TEXT("PHYS_"), TEXT("PA_") });
        P(TEXT("AnimSequence"), { TEXT("AS_"), TEXT("A_") });
        P(TEXT("AnimMontage"), { TEXT("AM_") });
        P(TEXT("BlendSpace"), { TEXT("BS_") });
        P(TEXT("BlendSpace1D"), { TEXT("BS_") });
        P(TEXT("AimOffsetBlendSpace"), { TEXT("AO_") });
        P(TEXT("IKRigDefinition"), { TEXT("IK_") });
        P(TEXT("IKRetargeter"), { TEXT("RTG_") });
        P(TEXT("NiagaraSystem"), { TEXT("NS_") });
        P(TEXT("NiagaraEmitter"), { TEXT("NE_") });
        P(TEXT("ParticleSystem"), { TEXT("P_"), TEXT("PS_") });
        P(TEXT("SoundWave"), { TEXT("S_"), TEXT("SW_") });
        P(TEXT("SoundCue"), { TEXT("SC_") });
        P(TEXT("SoundAttenuation"), { TEXT("ATT_") });
        P(TEXT("SoundConcurrency"), { TEXT("SCON_") });
        P(TEXT("MetaSoundSource"), { TEXT("MS_") });
        P(TEXT("MetaSoundPatch"), { TEXT("MSP_") });
        P(TEXT("DataTable"), { TEXT("DT_") });
        P(TEXT("CurveTable"), { TEXT("CT_") });
        P(TEXT("CurveFloat"), { TEXT("Curve_"), TEXT("C_") });
        P(TEXT("CurveVector"), { TEXT("Curve_"), TEXT("C_") });
        P(TEXT("CurveLinearColor"), { TEXT("Curve_"), TEXT("C_") });
        P(TEXT("UserDefinedEnum"), { TEXT("E_"), TEXT("E") });
        P(TEXT("UserDefinedStruct"), { TEXT("F_"), TEXT("S_") });
        P(TEXT("StringTable"), { TEXT("ST_") });
        P(TEXT("LevelSequence"), { TEXT("LS_") });
        P(TEXT("BehaviorTree"), { TEXT("BT_") });
        P(TEXT("BlackboardData"), { TEXT("BB_") });
        P(TEXT("EnvQuery"), { TEXT("EQS_") });
        P(TEXT("InputAction"), { TEXT("IA_") });
        P(TEXT("InputMappingContext"), { TEXT("IMC_") });
        return Map;
    }

    // The class key an asset's prefix is looked up by: its class's short name, with Blueprints split by type.
    FString AU_NamingClassKey(const FAssetData& AD)
    {
        const FString ClassName = AD.AssetClassPath.GetAssetName().ToString();
        if (ClassName == TEXT("Blueprint"))
        {
            FString BlueprintType;
            if (AD.GetTagValue(FName(TEXT("BlueprintType")), BlueprintType))
            {
                if (AU_EnumIs(BlueprintType, TEXT("BPTYPE_Interface"))) { return TEXT("BlueprintInterface"); }
                if (AU_EnumIs(BlueprintType, TEXT("BPTYPE_FunctionLibrary"))) { return TEXT("BlueprintFunctionLibrary"); }
                if (AU_EnumIs(BlueprintType, TEXT("BPTYPE_MacroLibrary"))) { return TEXT("BlueprintMacroLibrary"); }
            }
        }
        return ClassName;
    }

    bool AU_IsNameChar(TCHAR C)
    {
        return (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('a') && C <= TEXT('z'))
            || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('_');
    }
}

// MCP:audit_asset_naming - Sweep the assets under folder_path for naming-convention problems: a name without the prefix its class takes (BP_, BPI_, WBP_, ABP_, M_, MI_, MF_, T_, SM_, SK_, SKEL_, AS_, AM_, NS_, S_, SC_, DT_, IA_, IMC_... after Epic's recommended conventions), spaces, and characters other than letters, digits and underscore. Asset Registry only: nothing is loaded. Classes with no prefix rule (levels, data assets of project classes...) are counted in unchecked_count, not reported. Each finding carries a suggested_name. Returns findings inline, paged with offset/limit; the summary counts every finding. Read-only: it renames nothing (rename_asset does).
//   @param folder_path [string] - Content folder to scan. Defaults to "/Game".
//   @param recursive [bool] - Recurse into subfolders. Defaults to true.
//   @param prefixes [object] - Prefix rules by class short name, merged over the defaults: {"StaticMesh": "SM_", "Texture2D": ["T_", "TX_"], "World": "L_", "Blueprint": ""}. A string or an array of accepted prefixes (case-sensitive); "" or [] turns the class's rule off. Blueprints are keyed Blueprint, BlueprintInterface, BlueprintFunctionLibrary and BlueprintMacroLibrary.
//   @param replace_default_prefixes [bool] - Use only the prefixes param, without the defaults. Defaults to false.
//   @param rules [array] - Rule ids to run, default all: naming_prefix, naming_space, naming_invalid_char. An unknown id is an error.
//   @param max_results [number] - Cap on assets examined (1-100000). Defaults to 50000.
//   @param offset [number] - Index of the first finding returned. Defaults to 0.
//   @param limit [number] - Findings returned per call (1-5000). Defaults to 200.
//   @returns { folder_path, scanned: int, scan_truncated: bool, unchecked_count: int, prefix_rules: {class: [string]}, rules: [string], summary: { finding_count, by_severity: {error, warning, info}, by_rule: {rule: int} }, findings: [{ severity, rule, asset, asset_class, message, suggested_fix, suggested_name, expected_prefixes? }], offset, limit, returned, next_offset: int|null }
TSharedPtr<FJsonObject> HandleAuditAssetNaming(const TSharedPtr<FJsonObject>& Params)
{
    const FString FolderPath = AU_GetFolderPath(Params);
    if (TSharedPtr<FJsonObject> Bad = AU_ValidateFolderPath(FolderPath)) { return Bad; }
    const bool bRecursive = AU_GetRecursive(Params);
    const int32 MaxResults = AU_GetMaxResults(Params, 50000);
    int32 Offset = 0, Limit = 200;
    AU_GetPaging(Params, Offset, Limit);

    FAU_FindingSet Findings;
    if (TSharedPtr<FJsonObject> Bad = Findings.Init(Params, { TEXT("naming_prefix"), TEXT("naming_space"), TEXT("naming_invalid_char") }))
    {
        return Bad;
    }

    bool bReplaceDefaults = false;
    Params->TryGetBoolField(TEXT("replace_default_prefixes"), bReplaceDefaults);
    TMap<FString, TArray<FString>> Prefixes = bReplaceDefaults ? TMap<FString, TArray<FString>>() : AU_DefaultNamingPrefixes();

    const TSharedPtr<FJsonObject>* Overrides = nullptr;
    if (Params->TryGetObjectField(TEXT("prefixes"), Overrides) && Overrides && Overrides->IsValid())
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Overrides)->Values)
        {
            TArray<FString> Accepted;
            FString Single;
            const TArray<TSharedPtr<FJsonValue>>* Many = nullptr;
            if (Pair.Value.IsValid() && Pair.Value->TryGetString(Single))
            {
                if (!Single.IsEmpty()) { Accepted.Add(Single); }
            }
            else if (Pair.Value.IsValid() && Pair.Value->TryGetArray(Many) && Many)
            {
                for (const TSharedPtr<FJsonValue>& V : *Many)
                {
                    FString S;
                    if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty()) { Accepted.Add(S); }
                }
            }
            else
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("prefixes.%s must be a string or an array of strings"), *Pair.Key));
            }
            Prefixes.Add(Pair.Key, Accepted);
        }
    }

    // Every prefix in use, longest first, to strip a wrong one when suggesting a name (T_Rock as a material -> M_Rock).
    TArray<FString> AllPrefixes;
    for (const TPair<FString, TArray<FString>>& Pair : Prefixes)
    {
        for (const FString& P : Pair.Value) { AllPrefixes.AddUnique(P); }
    }
    AllPrefixes.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });

    bool bTruncated = false;
    TArray<FAssetData> Assets = AU_CollectAssetsOfClass(FolderPath, bRecursive, FTopLevelAssetPath(), MaxResults, bTruncated);
    const FTopLevelAssetPath RedirectorPath = UObjectRedirector::StaticClass()->GetClassPathName();

    int32 Scanned = 0, Unchecked = 0;
    for (const FAssetData& AD : Assets)
    {
        if (AD.AssetClassPath == RedirectorPath) { continue; }
        ++Scanned;

        const FString Name = AD.AssetName.ToString();
        const FString Asset = AD.GetSoftObjectPath().ToString();
        const FString ClassKey = AU_NamingClassKey(AD);

        // The name with spaces turned into underscores and other characters dropped.
        FString CleanName;
        bool bHasSpace = false;
        TArray<TCHAR> BadChars;
        for (TCHAR C : Name)
        {
            if (C == TEXT(' ')) { bHasSpace = true; CleanName.AppendChar(TEXT('_')); }
            else if (AU_IsNameChar(C)) { CleanName.AppendChar(C); }
            else { BadChars.AddUnique(C); }
        }

        const TArray<FString>* Expected = Prefixes.Find(ClassKey);
        const bool bHasRule = Expected && Expected->Num() > 0;
        if (!bHasRule) { ++Unchecked; }
        bool bPrefixOk = !bHasRule;
        if (bHasRule)
        {
            for (const FString& P : *Expected)
            {
                if (Name.StartsWith(P, ESearchCase::CaseSensitive)) { bPrefixOk = true; break; }
            }
        }

        FString SuggestedName = CleanName;
        if (!bPrefixOk)
        {
            FString Stem = CleanName;
            for (const FString& P : AllPrefixes)
            {
                if (Stem.StartsWith(P, ESearchCase::CaseSensitive) && Stem.Len() > P.Len())
                {
                    Stem.RightChopInline(P.Len());
                    break;
                }
            }
            SuggestedName = (*Expected)[0] + Stem;
        }

        auto AddName = [&](EAU_Severity Severity, const TCHAR* Rule, const FString& Message, const FString& Fix)
        {
            TSharedPtr<FJsonObject> F = Findings.Add(Severity, Rule, TEXT("asset"), Asset, Message, Fix);
            F->SetStringField(TEXT("asset_class"), ClassKey);
            F->SetStringField(TEXT("suggested_name"), SuggestedName);
            return F;
        };

        if (Findings.IsEnabled(TEXT("naming_prefix")) && !bPrefixOk)
        {
            TSharedPtr<FJsonObject> F = AddName(EAU_Severity::Warning, TEXT("naming_prefix"),
                FString::Printf(TEXT("%s '%s' does not start with %s"), *ClassKey, *Name, *FString::Join(*Expected, TEXT(" or "))),
                FString::Printf(TEXT("Rename to %s (rename_asset fixes up referencers)"), *SuggestedName));
            F->SetArrayField(TEXT("expected_prefixes"), mcp_common::StringsToJson(*Expected));
        }

        if (Findings.IsEnabled(TEXT("naming_space")) && bHasSpace)
        {
            AddName(EAU_Severity::Error, TEXT("naming_space"),
                FString::Printf(TEXT("'%s' contains spaces, which break console commands, command-line paths and some source-control tools"), *Name),
                FString::Printf(TEXT("Rename to %s"), *SuggestedName));
        }

        if (Findings.IsEnabled(TEXT("naming_invalid_char")) && BadChars.Num() > 0)
        {
            FString Chars;
            for (TCHAR C : BadChars) { Chars.AppendChar(C); }
            AddName(EAU_Severity::Warning, TEXT("naming_invalid_char"),
                FString::Printf(TEXT("'%s' contains characters other than letters, digits and underscore: \"%s\""), *Name, *Chars),
                FString::Printf(TEXT("Rename to %s"), *SuggestedName));
        }
    }

    TSharedPtr<FJsonObject> Rules = MakeShared<FJsonObject>();
    for (const TPair<FString, TArray<FString>>& Pair : Prefixes)
    {
        if (Pair.Value.Num() > 0) { Rules->SetArrayField(Pair.Key, mcp_common::StringsToJson(Pair.Value)); }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("folder_path"), FolderPath);
    Result->SetNumberField(TEXT("scanned"), Scanned);
    Result->SetBoolField(TEXT("scan_truncated"), bTruncated);
    Result->SetNumberField(TEXT("unchecked_count"), Unchecked);
    Result->SetObjectField(TEXT("prefix_rules"), Rules);
    Findings.WriteTo(Result, Offset, Limit);
    return mcp_common::MakeOkReply(Result);
}

// MCP:audit_level_actors - Sweep the actors of a world for placement problems: actors with no components, bare AActor instances with nothing visible, duplicates stacked at the same transform with the same meshes, mesh components with no mesh or with empty material slots, actors far from the origin or below the world's KillZ, and meshes hidden in game that still collide. Defaults to the editor world; pass world to audit a PIE world. In a World Partition map only the loaded actors are seen. Returns findings inline, paged with offset/limit; the summary counts every finding. Read-only.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @param max_distance [number] - Distance from the origin, in cm, beyond which an actor is reported. Defaults to 1048576 (the classic half world size, about 10.5 km).
//   @param rules [array] - Rule ids to run, default all: actor_no_components, actor_no_visual, actor_duplicate, actor_missing_mesh, actor_missing_material, actor_out_of_bounds, actor_below_kill_z, actor_hidden_collidable. An unknown id is an error.
//   @param max_results [number] - Cap on actors examined (1-100000). Defaults to 50000.
//   @param offset [number] - Index of the first finding returned. Defaults to 0.
//   @param limit [number] - Findings returned per call (1-5000). Defaults to 200.
//   @returns { world: string, world_name, world_package, world_type, net_mode, pie_instance, scanned: int, scan_truncated: bool, rules: [string], summary: { finding_count, by_severity: {error, warning, info}, by_rule: {rule: int} }, findings: [{ severity, rule, actor, actor_name, actor_class, message, suggested_fix, component?, location?, duplicate_of? }], offset, limit, returned, next_offset: int|null }
TSharedPtr<FJsonObject> HandleAuditLevelActors(const TSharedPtr<FJsonObject>& Params)
{
    FString WorldSelector, WorldError;
    UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), WorldSelector, WorldError);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty() ? TEXT("No world could be resolved for audit_level_actors") : WorldError);
    }

    const int32 MaxResults = AU_GetMaxResults(Params, 50000);
    const double MaxDistance = FMath::Max(1.0, AU_GetDoubleParam(Params, TEXT("max_distance"), 1048576.0));
    int32 Offset = 0, Limit = 200;
    AU_GetPaging(Params, Offset, Limit);

    FAU_FindingSet Findings;
    if (TSharedPtr<FJsonObject> Bad = Findings.Init(Params, {
            TEXT("actor_no_components"), TEXT("actor_no_visual"), TEXT("actor_duplicate"), TEXT("actor_missing_mesh"),
            TEXT("actor_missing_material"), TEXT("actor_out_of_bounds"), TEXT("actor_below_kill_z"), TEXT("actor_hidden_collidable") }))
    {
        return Bad;
    }

    const AWorldSettings* Settings = World->GetWorldSettings();
    const bool bKillZ = Settings && Settings->AreWorldBoundsChecksEnabled();
    const double KillZ = Settings ? static_cast<double>(Settings->KillZ) : 0.0;

    // Key -> label of the first actor seen with it, for actor_duplicate.
    TMap<FString, FString> SeenTransforms;
    int32 Scanned = 0;
    bool bTruncated = false;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor) || Actor->IsTemplate()) { continue; }
        if (Scanned >= MaxResults) { bTruncated = true; break; }
        ++Scanned;

        // Infos (world settings, game mode, game state...), controllers and the level script hold logic, not placement.
        const bool bLogicActor = Actor->IsA<AInfo>() || Actor->IsA<AController>() || Actor->IsA<ALevelScriptActor>();
        const FString Label = Actor->GetActorLabel();
        const FString Target = Label.IsEmpty() ? Actor->GetName() : Label;

        auto AddActor = [&](EAU_Severity Severity, const TCHAR* Rule, const FString& Message, const FString& Fix)
        {
            TSharedPtr<FJsonObject> F = Findings.Add(Severity, Rule, TEXT("actor"), Target, Message, Fix);
            F->SetStringField(TEXT("actor_name"), Actor->GetName());
            F->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
            return F;
        };

        TInlineComponentArray<UActorComponent*> Components(Actor);
        if (Findings.IsEnabled(TEXT("actor_no_components")) && Components.Num() == 0 && !bLogicActor)
        {
            AddActor(EAU_Severity::Info, TEXT("actor_no_components"),
                TEXT("the actor has no components: it has no transform and draws nothing"),
                TEXT("Delete it if it is a leftover (delete_actor), or give it a root component"));
        }

        TInlineComponentArray<UPrimitiveComponent*> Primitives(Actor);
        if (Findings.IsEnabled(TEXT("actor_no_visual")) && Components.Num() > 0 && Primitives.Num() == 0
            && Actor->GetClass() == AActor::StaticClass())
        {
            bool bOnlySceneComponents = true;
            for (const UActorComponent* C : Components)
            {
                if (C && !C->IsA<USceneComponent>()) { bOnlySceneComponents = false; break; }
            }
            if (bOnlySceneComponents)
            {
                AddActor(EAU_Severity::Info, TEXT("actor_no_visual"),
                    TEXT("a bare Actor holding only scene components: nothing is drawn and nothing collides"),
                    TEXT("Delete it if it is a leftover, or keep it only as an intentional pivot/marker"));
            }
        }

        const FVector Location = Actor->GetActorLocation();
        if (Findings.IsEnabled(TEXT("actor_out_of_bounds")) && Actor->GetRootComponent() && Location.Size() > MaxDistance)
        {
            TSharedPtr<FJsonObject> F = AddActor(EAU_Severity::Warning, TEXT("actor_out_of_bounds"),
                FString::Printf(TEXT("%.0f cm from the origin, beyond %.0f cm"), Location.Size(), MaxDistance),
                TEXT("Move it back into the playable area or delete it"));
            F->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Location));
        }
        if (Findings.IsEnabled(TEXT("actor_below_kill_z")) && bKillZ && Actor->GetRootComponent() && !bLogicActor && Location.Z < KillZ)
        {
            TSharedPtr<FJsonObject> F = AddActor(EAU_Severity::Warning, TEXT("actor_below_kill_z"),
                FString::Printf(TEXT("Z %.0f is below the world's KillZ %.0f: the actor is destroyed when play starts if it can fall out of the world"), Location.Z, KillZ),
                TEXT("Move it above KillZ, or lower KillZ in World Settings"));
            F->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Location));
        }

        TArray<FString> MeshPaths;
        for (UPrimitiveComponent* Prim : Primitives)
        {
            if (!Prim) { continue; }
            UMeshComponent* MeshComp = Cast<UMeshComponent>(Prim);
            if (!MeshComp) { continue; }

            bool bHasMesh = true;
            if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(MeshComp))
            {
                if (UStaticMesh* Mesh = SMC->GetStaticMesh()) { MeshPaths.Add(Mesh->GetPathName()); }
                else { bHasMesh = false; }
            }
            else if (USkinnedMeshComponent* SkC = Cast<USkinnedMeshComponent>(MeshComp))
            {
                bHasMesh = SkC->GetSkinnedAsset() != nullptr;
            }
            else
            {
                continue;   // procedural, spline, dynamic... meshes have no asset to be missing
            }

            if (!bHasMesh)
            {
                if (Findings.IsEnabled(TEXT("actor_missing_mesh")))
                {
                    // A placed mesh actor with no mesh is certainly broken; a Blueprint component may be filled at runtime.
                    const bool bMeshActor = Actor->IsA<AStaticMeshActor>() || Actor->IsA<ASkeletalMeshActor>();
                    TSharedPtr<FJsonObject> F = AddActor(bMeshActor ? EAU_Severity::Error : EAU_Severity::Warning, TEXT("actor_missing_mesh"),
                        FString::Printf(TEXT("component %s has no mesh assigned"), *MeshComp->GetName()),
                        TEXT("Assign a mesh (set_component_property on a placed actor, set_static_mesh_properties on a Blueprint component) or delete the component or actor"));
                    F->SetStringField(TEXT("component"), MeshComp->GetName());
                }
                continue;
            }

            if (Findings.IsEnabled(TEXT("actor_missing_material")))
            {
                TArray<FString> EmptySlots;
                for (int32 Slot = 0; Slot < MeshComp->GetNumMaterials(); ++Slot)
                {
                    if (!MeshComp->GetMaterial(Slot)) { EmptySlots.Add(FString::FromInt(Slot)); }
                }
                if (EmptySlots.Num() > 0)
                {
                    TSharedPtr<FJsonObject> F = AddActor(EAU_Severity::Warning, TEXT("actor_missing_material"),
                        FString::Printf(TEXT("component %s has no material in slot(s) %s: it renders with the default material"),
                            *MeshComp->GetName(), *FString::Join(EmptySlots, TEXT(", "))),
                        TEXT("Assign materials on the mesh asset or as overrides on the component"));
                    F->SetStringField(TEXT("component"), MeshComp->GetName());
                }
            }

            const bool bHiddenInGame = Actor->IsHidden() || MeshComp->bHiddenInGame;
            if (Findings.IsEnabled(TEXT("actor_hidden_collidable")) && bHiddenInGame && MeshComp->IsCollisionEnabled())
            {
                TSharedPtr<FJsonObject> F = AddActor(EAU_Severity::Warning, TEXT("actor_hidden_collidable"),
                    FString::Printf(TEXT("mesh component %s is hidden in game but its collision is %s: players hit something invisible"),
                        *MeshComp->GetName(), *StaticEnum<ECollisionEnabled::Type>()->GetNameStringByValue(static_cast<int64>(MeshComp->GetCollisionEnabled()))),
                    TEXT("Set its collision to NoCollision, or replace it with a Blocking Volume if the invisible wall is intended"));
                F->SetStringField(TEXT("component"), MeshComp->GetName());
            }
        }

        if (Findings.IsEnabled(TEXT("actor_duplicate")) && MeshPaths.Num() > 0 && Actor->GetRootComponent())
        {
            MeshPaths.Sort();
            const FTransform& T = Actor->GetActorTransform();
            const FVector L = T.GetLocation();
            const FRotator R = T.Rotator().GetNormalized();
            const FVector S = T.GetScale3D();
            const FString Key = FString::Printf(TEXT("%s|%.1f,%.1f,%.1f|%.2f,%.2f,%.2f|%.3f,%.3f,%.3f|%s"),
                *Actor->GetClass()->GetPathName(), L.X, L.Y, L.Z, R.Pitch, R.Yaw, R.Roll, S.X, S.Y, S.Z,
                *FString::Join(MeshPaths, TEXT(";")));
            if (const FString* First = SeenTransforms.Find(Key))
            {
                TSharedPtr<FJsonObject> F = AddActor(EAU_Severity::Warning, TEXT("actor_duplicate"),
                    FString::Printf(TEXT("same class, meshes and transform as %s: a stacked duplicate (double draw cost, z-fighting)"), **First),
                    TEXT("Delete the duplicate (delete_actor)"));
                F->SetStringField(TEXT("duplicate_of"), *First);
                F->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(L));
            }
            else
            {
                SeenTransforms.Add(Key, Target);
            }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Result, World);
    Result->SetNumberField(TEXT("scanned"), Scanned);
    Result->SetBoolField(TEXT("scan_truncated"), bTruncated);
    Findings.WriteTo(Result, Offset, Limit);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("audit_asset_references"), &HandleAuditAssetReferences },
    { TEXT("audit_asset_naming"), &HandleAuditAssetNaming },
    { TEXT("audit_blueprint_health"), &HandleAuditBlueprintHealth },
    { TEXT("audit_collision_profiles"), &HandleAuditCollisionProfiles },
    { TEXT("audit_level_actors"), &HandleAuditLevelActors },
    { TEXT("audit_replication"), &HandleAuditReplication },
    { TEXT("audit_static_meshes"), &HandleAuditStaticMeshes },
    { TEXT("audit_textures"), &HandleAuditTextures },
    { TEXT("audit_widget_accessibility"), &HandleAuditWidgetAccessibility },
    { TEXT("audit_widget_style"), &HandleAuditWidgetStyle },
    { TEXT("audit_widget_text"), &HandleAuditWidgetText },
    { TEXT("capture_pie_state"), &HandleCapturePieState },
    { TEXT("get_mcp_audit_report"), &HandleReadMcpAuditFile },
};
} // namespace UEAgentAuditCommands
