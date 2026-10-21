// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"   // create_project_overview: NetModeLabel
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h" // Complete type for UEdGraph (Blueprint->FunctionGraphs/EventGraphs) — only forward-declared by Blueprint.h
#include "Blueprint/BlueprintSupport.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Editor.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"
#include "Sound/SoundBase.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Components/ActorComponent.h"
#include "Engine/DataTable.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UObjectIterator.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/DateTime.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"   // create_project_overview: FEngineVersion::Current()

namespace UEAgentDocumentationCommands
{

// MCP-DOMAIN: Documentation

// ===========================================================================
// Shared pieces of the documentation generators: the editor world they all
// require, the /Game registry queries, the per-Blueprint record and the count
// tables.
// ===========================================================================
namespace UEAgentDocumentationCommandsImpl
{
    TSharedPtr<FJsonObject> NoWorldError()
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }

    /**
     * Every asset under /Game (recursively). With a Class, only assets of that class -- and of its subclasses
     * when bWithSubclasses, which abstract bases such as UMaterialInterface or USoundBase need to match at all.
     */
    TArray<FAssetData> GameAssetsOf(IAssetRegistry& Registry, UClass* Class, bool bWithSubclasses)
    {
        FARFilter Scope;
        Scope.PackagePaths.Add(TEXT("/Game"));
        Scope.bRecursivePaths = true;
        if (Class != nullptr)
        {
            Scope.ClassPaths.Add(Class->GetClassPathName());
            Scope.bRecursiveClasses = bWithSubclasses;
        }
        TArray<FAssetData> Found;
        Registry.GetAssets(Scope, Found);
        return Found;
    }

    /** One { <KeyField>: key, count: n } entry per distinct key, in first-seen order. */
    TArray<TSharedPtr<FJsonValue>> CountsToJson(const TMap<FString, int32>& Counts, const TCHAR* KeyField)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (const TPair<FString, int32>& Count : Counts)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(KeyField, Count.Key);
            Entry->SetNumberField(TEXT("count"), Count.Value);
            Entries.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Entries;
    }

    /** A sub-object carrying one number, e.g. { "total_textures": 12 }. */
    TSharedPtr<FJsonObject> SingleCount(const TCHAR* Field, int32 Value)
    {
        TSharedPtr<FJsonObject> Holder = MakeShared<FJsonObject>();
        Holder->SetNumberField(Field, Value);
        return Holder;
    }

    /** The first Limit names of Names, as strings. */
    TArray<TSharedPtr<FJsonValue>> FirstNames(const TArray<FName>& Names, int32 Limit)
    {
        TArray<TSharedPtr<FJsonValue>> Listed;
        const int32 Count = FMath::Min(Names.Num(), Limit);
        for (int32 Index = 0; Index < Count; ++Index)
        {
            Listed.Add(MakeShared<FJsonValueString>(Names[Index].ToString()));
        }
        return Listed;
    }

    /** Category and usage blurb create_asset_documentation gives the main asset kinds; anything else is "Other". */
    void DescribeAssetKind(const FName& TypeName, FString& OutCategory, FString& OutUsage)
    {
        struct FKind
        {
            const TCHAR* Type;
            const TCHAR* Category;
            const TCHAR* Usage;
        };
        static const FKind Kinds[] = {
            { TEXT("StaticMesh"), TEXT("3D Model"),  TEXT("Static mesh for 3D objects") },
            { TEXT("Material"),   TEXT("Material"),  TEXT("Material for rendering surfaces") },
            { TEXT("Texture2D"),  TEXT("Texture"),   TEXT("2D texture for materials and UI") },
            { TEXT("Blueprint"),  TEXT("Blueprint"), TEXT("Blueprint for game logic and behavior") },
            { TEXT("SoundBase"),  TEXT("Audio"),     TEXT("Audio asset for sound effects and music") },
        };
        for (const FKind& Kind : Kinds)
        {
            if (TypeName == Kind.Type)
            {
                OutCategory = Kind.Category;
                OutUsage = Kind.Usage;
                return;
            }
        }
        OutCategory = TEXT("Other");
        OutUsage = TEXT("General asset");
    }

    /**
     * The generate_blueprint_docs record of one loaded Blueprint: identity, parent, a two-way type label
     * (anything that is not a normal Blueprint reads MacroLibrary), compile state, component templates,
     * the names of its function graphs followed by its compiled event graphs, and its own variables.
     */
    TSharedPtr<FJsonObject> DocumentBlueprint(UBlueprint* Blueprint, const FAssetData& Entry)
    {
        TSharedPtr<FJsonObject> Doc = MakeShared<FJsonObject>();
        Doc->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
        Doc->SetStringField(TEXT("blueprint_path"), Entry.PackageName.ToString());
        Doc->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());
        UClass* const Parent = Blueprint->ParentClass;
        Doc->SetStringField(TEXT("parent_class"), Parent ? Parent->GetName() : FString(TEXT("None")));
        Doc->SetStringField(TEXT("blueprint_type"), Blueprint->BlueprintType == BPTYPE_Normal ? TEXT("Normal") : TEXT("MacroLibrary"));
        Doc->SetStringField(TEXT("compilation_status"), Blueprint->Status == BS_UpToDate ? TEXT("UpToDate") : TEXT("NeedsCompilation"));

        TArray<TSharedPtr<FJsonValue>> Components;
        for (UActorComponent* Template : Blueprint->ComponentTemplates)
        {
            if (Template == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
            Info->SetStringField(TEXT("component_name"), Template->GetName());
            Info->SetStringField(TEXT("component_class"), Template->GetClass()->GetName());
            Info->SetBoolField(TEXT("is_editable"), Template->IsEditableWhenInherited());
            Components.Add(MakeShared<FJsonValueObject>(Info));
        }
        Doc->SetArrayField(TEXT("components"), Components);

        TArray<TSharedPtr<FJsonValue>> GraphNames;
        auto AddGraphNames = [&GraphNames](const auto& Graphs)
        {
            for (UEdGraph* Graph : Graphs)
            {
                if (Graph != nullptr)
                {
                    GraphNames.Add(MakeShared<FJsonValueString>(Graph->GetName()));
                }
            }
        };
        AddGraphNames(Blueprint->FunctionGraphs);
        AddGraphNames(Blueprint->EventGraphs);
        Doc->SetArrayField(TEXT("functions"), GraphNames);

        TArray<TSharedPtr<FJsonValue>> Variables;
        for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
        {
            TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
            Info->SetStringField(TEXT("name"), Variable.VarName.ToString());
            Info->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
            Variables.Add(MakeShared<FJsonValueObject>(Info));
        }
        Doc->SetArrayField(TEXT("variables"), Variables);
        return Doc;
    }
}

// MCP:generate_blueprint_docs - Documents every Blueprint asset under /Game in one reply, subclasses included (Widget, Anim and other Blueprints): per Blueprint its name, package path, class, parent class, type, compile state, component templates, the names of its function and event graphs, and its own variables with their pin category. Read-only despite its name: it loads every Blueprint to read it, but writes and saves nothing, and the result comes back in the reply only, never as a file. On a big project it can take long and return a very large reply; plugin and engine content are not included, and an editor world is required. For one Blueprint use get_blueprint_info (get_blueprint_components for its real component tree) or export_blueprint_data; for one asset of any kind, create_asset_documentation; for project-wide counts without loading, create_project_overview.
//   @hints read-only
//   @returns { blueprint_documentation: [{ blueprint_name, blueprint_path, blueprint_class, parent_class, blueprint_type, compilation_status, components, functions: [string], variables: [{name, type}] }], total_blueprints: int, generation_date: string, project_name: string } -- blueprint_type is Normal, or MacroLibrary for every other kind (interfaces and function libraries included); compilation_status is UpToDate or NeedsCompilation (errors included); components are the Blueprint's ComponentTemplates, which do not include the components of its Components panel

TSharedPtr<FJsonObject> HandleGenerateBlueprintDocs(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDocumentationCommandsImpl;

    // Like every generator here, this one refuses to run without an editor world.
    if (GEditor->GetEditorWorldContext().World() == nullptr)
    {
        return Impl::NoWorldError();
    }

    // Every Blueprint under /Game, subclasses included (WidgetBlueprint, AnimBlueprint, ...); each one is
    // loaded to be documented.
    const TArray<FAssetData> Candidates = Impl::GameAssetsOf(mcp_common::AssetRegistry(), UBlueprint::StaticClass(), /*bWithSubclasses*/ true);
    TArray<TSharedPtr<FJsonValue>> Documented;
    for (const FAssetData& Entry : Candidates)
    {
        if (UBlueprint* Blueprint = Cast<UBlueprint>(Entry.GetAsset()))
        {
            Documented.Add(MakeShared<FJsonValueObject>(Impl::DocumentBlueprint(Blueprint, Entry)));
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("blueprint_documentation"), Documented);
    Reply->SetNumberField(TEXT("total_blueprints"), Documented.Num());
    Reply->SetStringField(TEXT("generation_date"), FDateTime::Now().ToString());
    Reply->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    return mcp_common::MakeOkReply(Reply);
}

// MCP:create_asset_documentation - Builds a documentation record of one asset from the Asset Registry, without loading it: name, class, package and object path, .uasset size and timestamp, a coarse category and usage line chosen by exact class (StaticMesh, Material, Texture2D, Blueprint; every other class, sound waves and material instances included, is Other), and its direct package dependencies and referencers, 100 of each at most with truncation flags. Read-only despite its name: nothing is loaded, written or saved, and the record comes back in the reply only. An asset the registry does not know, or no editor world, is an error. For complete edge lists use get_asset_dependencies and get_asset_referencers (get_asset_dependencies_recursive beyond depth 1); for every Blueprint at once, generate_blueprint_docs; for project-wide counts, create_project_overview.
//   @hints read-only
//   @param asset_name - Package path ("/Game/Props/SM_Chair") or object path ("/Game/Props/SM_Chair.SM_Chair") of the asset; a bare asset name is not resolved (find_assets_by_name turns a name into a path).
//   @returns { asset_documentation: { asset_name, asset_path, asset_class, asset_type, package_name, object_path, file_name, file_size: int (bytes, 0 if file_exists=false), last_modified: string, file_exists: bool, asset_category, usage, dependencies: [string] (package names, depth-1, capped at 100), dependencies_truncated: bool, dependents: [string] (referencers, depth-1, capped at 100), dependents_truncated: bool }, generation_date: string, project_name: string }

TSharedPtr<FJsonObject> HandleCreateAssetDocumentation(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDocumentationCommandsImpl;

    FString RequestedPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), RequestedPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (GEditor->GetEditorWorldContext().World() == nullptr)
    {
        return Impl::NoWorldError();
    }

    // Resolved as an object path through the registry, never loaded.
    IAssetRegistry& Registry = mcp_common::AssetRegistry();
    const FAssetData Asset = mcp_common::FindAssetDataByPath(RequestedPath);
    if (!Asset.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset '%s' not found"), *RequestedPath));
    }

    const FString PackageName = Asset.PackageName.ToString();
    const FName TypeName = Asset.AssetClassPath.GetAssetName();

    TSharedPtr<FJsonObject> Doc = MakeShared<FJsonObject>();
    Doc->SetStringField(TEXT("asset_name"), Asset.AssetName.ToString());
    Doc->SetStringField(TEXT("asset_path"), PackageName);
    Doc->SetStringField(TEXT("package_name"), PackageName);
    Doc->SetStringField(TEXT("object_path"), Asset.GetObjectPathString());
    Doc->SetStringField(TEXT("asset_class"), Asset.AssetClassPath.ToString());
    Doc->SetStringField(TEXT("asset_type"), TypeName.ToString());
    Doc->SetStringField(TEXT("file_name"), PackageName + TEXT(".uasset"));

    // The real size and date of the .uasset on disk; 0 and "Unknown" when there is no such file.
    IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
    FString OnDisk;
    const bool bOnDisk = FPackageName::TryConvertLongPackageNameToFilename(PackageName, OnDisk, TEXT(".uasset"))
        && Files.FileExists(*OnDisk);
    Doc->SetBoolField(TEXT("file_exists"), bOnDisk);
    Doc->SetNumberField(TEXT("file_size"), bOnDisk ? Files.FileSize(*OnDisk) : 0);
    Doc->SetStringField(TEXT("last_modified"), bOnDisk ? Files.GetTimeStamp(*OnDisk).ToString() : FString(TEXT("Unknown")));

    FString Category;
    FString Usage;
    Impl::DescribeAssetKind(TypeName, Category, Usage);
    Doc->SetStringField(TEXT("asset_category"), Category);
    Doc->SetStringField(TEXT("usage"), Usage);

    // Package-level edges, depth 1, at most 100 each way -- this documents an asset, it does not audit it
    // (get_asset_dependencies / get_asset_referencers give the full lists).
    const int32 MaxListed = 100;
    TArray<FName> Needs;
    Registry.GetDependencies(Asset.PackageName, Needs, UE::AssetRegistry::EDependencyCategory::Package);
    Doc->SetArrayField(TEXT("dependencies"), Impl::FirstNames(Needs, MaxListed));
    Doc->SetBoolField(TEXT("dependencies_truncated"), Needs.Num() > MaxListed);
    TArray<FName> NeededBy;
    Registry.GetReferencers(Asset.PackageName, NeededBy, UE::AssetRegistry::EDependencyCategory::Package);
    Doc->SetArrayField(TEXT("dependents"), Impl::FirstNames(NeededBy, MaxListed));
    Doc->SetBoolField(TEXT("dependents_truncated"), NeededBy.Num() > MaxListed);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetObjectField(TEXT("asset_documentation"), Doc);
    Reply->SetStringField(TEXT("generation_date"), FDateTime::Now().ToString());
    Reply->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    return mcp_common::MakeOkReply(Reply);
}

// MCP:create_project_overview - Summarize the project in one read that writes nothing, neither a file nor an asset: project name, engine version and platform; /Game asset counts per class from the Asset Registry; the Blueprint total split into normal Blueprints and macro libraries, read from the registry's BlueprintType tag without loading any Blueprint; the name, world_type, net mode, actor count, level count and actors per class of one world, the PIE world while a play session runs and the editor world otherwise; and the totals of materials (any UMaterialInterface), Texture2D and sound assets. Without an editor world it is an error. Project metadata alone: get_project_info; one asset: create_asset_documentation; every Blueprint in detail: generate_blueprint_docs; the actors themselves: get_actors_in_level.
//   @returns { project_info: { project_name, engine_version, platform, generation_date, is_editor }, asset_types: [{ asset_type, count }], blueprint_info: { total_blueprints, normal_blueprints, macro_blueprints }, world_info: { world_name, world_type, net_mode, actor_count, level_count }, actor_types: [{ actor_type, count }], material_info: { total_materials }, texture_info: { total_textures }, audio_info: { total_audio_assets }, statistics: { total_assets, total_blueprints, total_materials, total_textures, total_audio_assets, total_actors } }
//   @hints read-only

TSharedPtr<FJsonObject> HandleCreateProjectOverview(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDocumentationCommandsImpl;

    // The world described: the PIE world while a play session runs, else the editor world.
    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr)
    {
        return Impl::NoWorldError();
    }
    UWorld* World = EditorWorld;
    if (GEditor->PlayWorld != nullptr)
    {
        World = GEditor->PlayWorld;
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();

    TSharedPtr<FJsonObject> ProjectSummary = MakeShared<FJsonObject>();
    ProjectSummary->SetStringField(TEXT("project_name"), FApp::GetProjectName());
    ProjectSummary->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
    ProjectSummary->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
    ProjectSummary->SetStringField(TEXT("generation_date"), FDateTime::Now().ToString());
    ProjectSummary->SetBoolField(TEXT("is_editor"), GIsEditor);
    Reply->SetObjectField(TEXT("project_info"), ProjectSummary);

    IAssetRegistry& Registry = mcp_common::AssetRegistry();

    // Every /Game asset, counted per class name.
    const TArray<FAssetData> Everything = Impl::GameAssetsOf(Registry, nullptr, /*bWithSubclasses*/ false);
    TMap<FString, int32> PerAssetClass;
    for (const FAssetData& Entry : Everything)
    {
        ++PerAssetClass.FindOrAdd(Entry.AssetClassPath.GetAssetName().ToString());
    }
    Reply->SetArrayField(TEXT("asset_types"), Impl::CountsToJson(PerAssetClass, TEXT("asset_type")));

    // Blueprints of every kind, split into normal Blueprints and macro libraries by the registry's BlueprintType
    // tag (UBlueprint::BlueprintType is AssetRegistrySearchable, exported as the enum name, "BPTYPE_Normal"),
    // so no Blueprint is loaded. FString comparison ignores case, as the engine's own readers of this tag do.
    const TArray<FAssetData> Blueprints = Impl::GameAssetsOf(Registry, UBlueprint::StaticClass(), /*bWithSubclasses*/ true);
    int32 NormalCount = 0;
    int32 MacroLibraryCount = 0;
    for (const FAssetData& Entry : Blueprints)
    {
        FString BlueprintTypeTag;
        if (!Entry.GetTagValue(FBlueprintTags::BlueprintType, BlueprintTypeTag))
        {
            continue;
        }
        if (BlueprintTypeTag == TEXT("BPTYPE_Normal"))
        {
            ++NormalCount;
        }
        else if (BlueprintTypeTag == TEXT("BPTYPE_MacroLibrary"))
        {
            ++MacroLibraryCount;
        }
    }
    TSharedPtr<FJsonObject> BlueprintSummary = MakeShared<FJsonObject>();
    BlueprintSummary->SetNumberField(TEXT("total_blueprints"), Blueprints.Num());
    BlueprintSummary->SetNumberField(TEXT("normal_blueprints"), NormalCount);
    BlueprintSummary->SetNumberField(TEXT("macro_blueprints"), MacroLibraryCount);
    Reply->SetObjectField(TEXT("blueprint_info"), BlueprintSummary);

    TSharedPtr<FJsonObject> WorldSummary = MakeShared<FJsonObject>();
    WorldSummary->SetStringField(TEXT("world_name"), World->GetName());
    WorldSummary->SetStringField(TEXT("world_type"), LexToString(World->WorldType.GetValue()));
    WorldSummary->SetStringField(TEXT("net_mode"), mcp_common::NetModeLabel(World->GetNetMode()));
    WorldSummary->SetNumberField(TEXT("actor_count"), World->GetActorCount());
    WorldSummary->SetNumberField(TEXT("level_count"), World->GetNumLevels());
    Reply->SetObjectField(TEXT("world_info"), WorldSummary);

    // Actors of that world, counted per class name.
    TMap<FString, int32> PerActorClass;
    for (TActorIterator<AActor> ActorIt(World); ActorIt; ++ActorIt)
    {
        ++PerActorClass.FindOrAdd(ActorIt->GetClass()->GetName());
    }
    Reply->SetArrayField(TEXT("actor_types"), Impl::CountsToJson(PerActorClass, TEXT("actor_type")));

    // UMaterialInterface and USoundBase are abstract, so only their subclasses can match; Texture2D is exact.
    const int32 MaterialTotal = Impl::GameAssetsOf(Registry, UMaterialInterface::StaticClass(), /*bWithSubclasses*/ true).Num();
    const int32 TextureTotal = Impl::GameAssetsOf(Registry, UTexture2D::StaticClass(), /*bWithSubclasses*/ false).Num();
    const int32 AudioTotal = Impl::GameAssetsOf(Registry, USoundBase::StaticClass(), /*bWithSubclasses*/ true).Num();
    Reply->SetObjectField(TEXT("material_info"), Impl::SingleCount(TEXT("total_materials"), MaterialTotal));
    Reply->SetObjectField(TEXT("texture_info"), Impl::SingleCount(TEXT("total_textures"), TextureTotal));
    Reply->SetObjectField(TEXT("audio_info"), Impl::SingleCount(TEXT("total_audio_assets"), AudioTotal));

    TSharedPtr<FJsonObject> Totals = MakeShared<FJsonObject>();
    Totals->SetNumberField(TEXT("total_assets"), Everything.Num());
    Totals->SetNumberField(TEXT("total_blueprints"), Blueprints.Num());
    Totals->SetNumberField(TEXT("total_materials"), MaterialTotal);
    Totals->SetNumberField(TEXT("total_textures"), TextureTotal);
    Totals->SetNumberField(TEXT("total_audio_assets"), AudioTotal);
    Totals->SetNumberField(TEXT("total_actors"), World->GetActorCount());
    Reply->SetObjectField(TEXT("statistics"), Totals);

    return mcp_common::MakeOkReply(Reply);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_asset_documentation"), &HandleCreateAssetDocumentation },
    { TEXT("create_project_overview"), &HandleCreateProjectOverview },
    { TEXT("generate_blueprint_docs"), &HandleGenerateBlueprintDocs },
};
} // namespace UEAgentDocumentationCommands
