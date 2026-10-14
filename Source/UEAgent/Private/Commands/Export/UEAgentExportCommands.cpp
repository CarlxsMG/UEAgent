#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
// export_asset_as_text: canonical stable text/JSON/T3D dump.
#include "Engine/DataTable.h"                 // UDataTable::GetTableAsJSON (native DataTable-JSON writer)
#include "DataTableUtils.h"                    // EDataTableExportFlags
#include "Exporters/Exporter.h"               // UExporter::ExportToOutputDevice (T3D)
#include "UnrealExporter.h"                    // FExportObjectInnerContext full definition (T3D)
#include "Misc/StringOutputDevice.h"          // FStringOutputDevice (T3D capture)
#include "Misc/SecureHash.h"                  // FMD5 (stable content hash)
#include "Policies/PrettyJsonPrintPolicy.h"   // deterministic pretty JSON

namespace UEAgentExportCommands
{

// MCP-DOMAIN: Export

// ---------------------------------------------------------------------------
// Plumbing shared by the commands that write a file under Saved/ and answer with
// where it went (export_asset_data, export_blueprint_data, export_level_section,
// generate_asset_report).
// ---------------------------------------------------------------------------
namespace UEAgentExportImpl
{
    /** A property value as the details panel prints it. Empty when either input is null. */
    FString PropertyDisplayText(const FProperty* Prop, const void* OwnerMemory)
    {
        FString Rendered;
        if (Prop != nullptr && OwnerMemory != nullptr)
        {
            const void* ValueMemory = Prop->ContainerPtrToValuePtr<void>(OwnerMemory);
            Prop->ExportTextItem_Direct(Rendered, ValueMemory, nullptr, nullptr, PPF_PropertyWindow);
        }
        return Rendered;
    }

    /** (name, value text) of every property an editor user or a Blueprint can see, in reflection order. */
    TArray<TPair<FString, FString>> VisiblePropertyTexts(UObject* Subject)
    {
        TArray<TPair<FString, FString>> Pairs;
        for (TFieldIterator<FProperty> Field(Subject->GetClass()); Field; ++Field)
        {
            const FProperty* Prop = *Field;
            if (Prop == nullptr || !Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
            {
                continue;
            }
            Pairs.Emplace(Prop->GetName(), PropertyDisplayText(Prop, Subject));
        }
        return Pairs;
    }

    /** The registry entry of RequestedAsset and the asset it loads. Null on success, else the error reply. */
    TSharedPtr<FJsonObject> LoadAssetEntry(const FString& RequestedAsset, FAssetData& OutEntry, UObject*& OutSubject)
    {
        OutEntry = mcp_common::FindAssetDataByPath(RequestedAsset);
        if (!OutEntry.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found: %s"), *RequestedAsset));
        }
        OutSubject = OutEntry.GetAsset();
        if (OutSubject == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *RequestedAsset));
        }
        return nullptr;
    }

    /** Which writer a (lower-cased) format token selects. */
    enum class EDumpFormat : uint8
    {
        Json,
        Text,
        Unsupported
    };

    EDumpFormat DumpFormatFromToken(const FString& LowerToken)
    {
        if (LowerToken == TEXT("json"))
        {
            return EDumpFormat::Json;
        }
        if (LowerToken == TEXT("txt"))
        {
            return EDumpFormat::Text;
        }
        return EDumpFormat::Unsupported;
    }

    /** One generated file: where it goes, whether it was written, and the status sentence. */
    struct FDumpFile
    {
        FString Folder;
        FString Name;
        FString FullPath;
        bool bWritten = false;
        FString Status;
        int64 Bytes = 0;
    };

    /**
     * Picks <Saved>/<Area>/<Kind>/<Stem>_<yyyymmdd_hhmmss>.<Extension>. The folder is created right
     * away, even when nothing ends up written. Status starts as the given failure sentence.
     */
    FDumpFile PlanDumpFile(IPlatformFile& Disk, const TCHAR* Area, const TCHAR* Kind,
        const FString& Stem, const FString& Extension, const TCHAR* FailureStatus)
    {
        FDumpFile Plan;
        Plan.Folder = FPaths::ProjectSavedDir() / Area / Kind;
        if (!Disk.DirectoryExists(*Plan.Folder))
        {
            Disk.CreateDirectoryTree(*Plan.Folder);
        }
        const FString Stamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
        Plan.Name = FString::Printf(TEXT("%s_%s.%s"), *Stem, *Stamp, *Extension);
        Plan.FullPath = Plan.Folder / Plan.Name;
        Plan.Status = FailureStatus;
        return Plan;
    }

    /** Writes Contents to the planned path; only a successful write flips the status. */
    void CommitText(FDumpFile& File, const FString& Contents, const TCHAR* SuccessStatus)
    {
        if (!FFileHelper::SaveStringToFile(Contents, *File.FullPath))
        {
            return;
        }
        File.bWritten = true;
        File.Status = SuccessStatus;
    }

    /** Same as CommitText, for a JSON document serialized with the default writer policy. */
    void CommitJson(FDumpFile& File, const TSharedPtr<FJsonObject>& Document, const TCHAR* SuccessStatus)
    {
        FString Serialized;
        TSharedRef<TJsonWriter<>> Sink = TJsonWriterFactory<>::Create(&Serialized);
        FJsonSerializer::Serialize(Document.ToSharedRef(), Sink);
        CommitText(File, Serialized, SuccessStatus);
    }

    /** Records the size of the file only when this call wrote it. */
    void MeasureDumpFile(IPlatformFile& Disk, FDumpFile& File)
    {
        File.Bytes = (File.bWritten && Disk.FileExists(*File.FullPath)) ? Disk.FileSize(*File.FullPath) : 0;
    }

    /**
     * Size of the package file behind an asset (.uasset or .umap), or 0 when it is not on disk.
     */
    int64 SourcePackageBytes(IPlatformFile& Disk, const FAssetData& Entry)
    {
        FString PackageFile;
        if (mcp_common::FindPackageFileOnDisk(Entry.PackageName.ToString(), PackageFile))
        {
            return FMath::Max<int64>(0, Disk.FileSize(*PackageFile));
        }
        return 0;
    }

    double InMegabytes(const int64 Bytes)
    {
        return Bytes / (1024.0 * 1024.0);
    }

    /** The export_* fields that describe the written file. */
    void PutExportFileFields(const TSharedPtr<FJsonObject>& Answer, const FDumpFile& File)
    {
        Answer->SetBoolField(TEXT("export_success"), File.bWritten);
        Answer->SetStringField(TEXT("export_message"), File.Status);
        Answer->SetStringField(TEXT("export_file_path"), File.FullPath);
        Answer->SetStringField(TEXT("export_file_name"), File.Name);
        Answer->SetNumberField(TEXT("export_file_size_bytes"), File.Bytes);
        Answer->SetNumberField(TEXT("export_file_size_mb"), InMegabytes(File.Bytes));
    }

    /** The closing export_* fields: answer time, outcome word and target folder. */
    void PutExportTrailer(const TSharedPtr<FJsonObject>& Answer, const FDumpFile& File)
    {
        Answer->SetStringField(TEXT("export_timestamp"), FDateTime::Now().ToString());
        Answer->SetStringField(TEXT("export_status"), File.bWritten ? TEXT("Completed") : TEXT("Failed"));
        Answer->SetStringField(TEXT("export_directory"), File.Folder);
    }

    // -- Blueprint facts --------------------------------------------------------

    const TCHAR* BlueprintStatusName(const UBlueprint& Blueprint)
    {
        switch (Blueprint.Status.GetValue())
        {
        case BS_UpToDate:             return TEXT("UpToDate");
        case BS_Error:                return TEXT("Error");
        case BS_UpToDateWithWarnings: return TEXT("UpToDateWithWarnings");
        default:                      break;
        }
        return TEXT("Unknown");
    }

    FString ParentClassName(const UBlueprint& Blueprint)
    {
        const UClass* Parent = Blueprint.ParentClass.Get();
        return Parent != nullptr ? Parent->GetName() : FString(TEXT("None"));
    }

    /** Names of the non-null function graphs (the reported function count still includes null slots). */
    TArray<FString> FunctionGraphNames(const UBlueprint& Blueprint)
    {
        TArray<FString> Names;
        for (UEdGraph* Graph : Blueprint.FunctionGraphs)
        {
            if (Graph != nullptr)
            {
                Names.Add(Graph->GetName());
            }
        }
        return Names;
    }

    struct FMemberVariableRow
    {
        FString Name;
        FString PinCategory;
        FString Category;
        bool bBlueprintVisible = false;
        bool bEditable = false;
    };

    TArray<FMemberVariableRow> MemberVariableRows(const UBlueprint& Blueprint)
    {
        TArray<FMemberVariableRow> Rows;
        Rows.Reserve(Blueprint.NewVariables.Num());
        for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
        {
            FMemberVariableRow& Row = Rows.AddDefaulted_GetRef();
            Row.Name = Variable.VarName.ToString();
            Row.PinCategory = Variable.VarType.PinCategory.ToString();
            Row.Category = Variable.Category.ToString();
            Row.bBlueprintVisible = (Variable.PropertyFlags & CPF_BlueprintVisible) != 0;
            Row.bEditable = (Variable.PropertyFlags & CPF_Edit) != 0;
        }
        return Rows;
    }

    // -- Level facts --------------------------------------------------------------

    /** World of the engine's first world context; no editor/PIE distinction is made. */
    UWorld* FirstContextWorld()
    {
        return mcp_common::GetFirstContextWorld();
    }

    struct FActorRow
    {
        FString Name;
        FString ClassName;
        FVector Location;
        FRotator Rotation;
        FVector Scale;
    };

    /**
     * The actors a section names: "*" is the whole world; otherwise a loaded level whose package
     * short name matches (the persistent level or a streaming sub-level); otherwise an outliner
     * folder -- the actors in it and in its subfolders. OutScope is "world", "level" or "folder",
     * and empty when nothing matches (the list is then empty too).
     */
    TArray<AActor*> ActorsInSection(UWorld* World, const FString& Section, FString& OutScope, FString& OutLevelName)
    {
        TArray<AActor*> Actors;
        OutScope.Reset();
        OutLevelName = World->GetName();
        if (Section == TEXT("*"))
        {
            OutScope = TEXT("world");
            for (TActorIterator<AActor> Cursor(World); Cursor; ++Cursor)
            {
                Actors.Add(*Cursor);
            }
            return Actors;
        }
        for (ULevel* Level : World->GetLevels())
        {
            if (Level != nullptr && FPackageName::GetShortName(Level->GetOutermost()->GetName()).Equals(Section, ESearchCase::IgnoreCase))
            {
                OutScope = TEXT("level");
                OutLevelName = FPackageName::GetShortName(Level->GetOutermost()->GetName());
                for (AActor* Actor : Level->Actors)
                {
                    if (Actor != nullptr)
                    {
                        Actors.Add(Actor);
                    }
                }
                return Actors;
            }
        }
        for (TActorIterator<AActor> Cursor(World); Cursor; ++Cursor)
        {
            const FString Folder = Cursor->GetFolderPath().ToString();
            if (Folder.Equals(Section, ESearchCase::IgnoreCase) || Folder.StartsWith(Section + TEXT("/"), ESearchCase::IgnoreCase))
            {
                Actors.Add(*Cursor);
            }
        }
        if (Actors.Num() > 0)
        {
            OutScope = TEXT("folder");
        }
        return Actors;
    }

    TArray<FActorRow> CollectActorRows(const TArray<AActor*>& Actors)
    {
        TArray<FActorRow> Rows;
        for (AActor* Actor : Actors)
        {
            if (Actor == nullptr)
            {
                continue;
            }
            FActorRow& Row = Rows.AddDefaulted_GetRef();
            Row.ClassName = Actor->GetClass()->GetName();
            Row.Name = Actor->GetName();
            Row.Location = Actor->GetActorLocation();
            Row.Rotation = Actor->GetActorRotation();
            Row.Scale = Actor->GetActorScale();
        }
        return Rows;
    }

    // -- Property census for generate_asset_report ---------------------------------

    struct FPropertyCensus
    {
        TArray<FString> Names;
        TArray<FString> TypeNames;
        TArray<FString> Texts;
        int32 EditableCount = 0;
        int32 BlueprintVisibleCount = 0;
    };

    /** Every reflected property of the subject's class, unfiltered, with its type and value text. */
    FPropertyCensus TakePropertyCensus(UObject* Subject)
    {
        FPropertyCensus Census;
        for (TFieldIterator<FProperty> Field(Subject->GetClass()); Field; ++Field)
        {
            const FProperty* Prop = *Field;
            if (Prop == nullptr)
            {
                continue;
            }
            Census.Names.Add(Prop->GetName());
            Census.TypeNames.Add(Prop->GetClass()->GetName());
            Census.Texts.Add(PropertyDisplayText(Prop, Subject));
            Census.EditableCount += Prop->HasAnyPropertyFlags(CPF_Edit) ? 1 : 0;
            Census.BlueprintVisibleCount += Prop->HasAnyPropertyFlags(CPF_BlueprintVisible) ? 1 : 0;
        }
        return Census;
    }
}

// MCP:export_asset_data - Exports asset data to JSON or TXT format.
//   @param asset_path - Full path to the asset (e.g., '/Game/MyAsset').
//   @param export_format - Export format: 'json' or 'txt'.
//   @returns { asset_path: string, export_format: string, export_success: bool, export_message: string, export_file_path: string, export_file_name: string, export_file_size_bytes: int, export_file_size_mb: number, asset_class: string, package_name: string, original_file_size_bytes: int, original_file_size_mb: number, export_timestamp: string, export_status: string, export_directory: string }

TSharedPtr<FJsonObject> HandleExportAssetData(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentExportImpl;

    FString RequestedAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString FormatToken;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("export_format"), FormatToken, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetData Entry;
    UObject* Subject = nullptr;
    if (TSharedPtr<FJsonObject> Error = Impl::LoadAssetEntry(RequestedAsset, Entry, Subject)) { return Error; }

    // The file name always uses the lower-cased token as extension, supported or not.
    IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
    const FString LowerToken = FormatToken.ToLower();
    const FString AssetName = FPaths::GetBaseFilename(RequestedAsset);
    Impl::FDumpFile File = Impl::PlanDumpFile(Disk, TEXT("Exports"), TEXT("Assets"), AssetName, LowerToken, TEXT("Export failed"));

    const FString ClassPath = Entry.AssetClassPath.ToString();
    const FString PackagePath = Entry.PackageName.ToString();
    switch (Impl::DumpFormatFromToken(LowerToken))
    {
    case Impl::EDumpFormat::Json:
    {
        TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
        Document->SetStringField(TEXT("asset_path"), RequestedAsset);
        Document->SetStringField(TEXT("asset_class"), ClassPath);
        Document->SetStringField(TEXT("package_name"), PackagePath);
        Document->SetStringField(TEXT("asset_name"), AssetName);
        Document->SetStringField(TEXT("export_timestamp"), FDateTime::Now().ToString());

        TSharedPtr<FJsonObject> ValueMap = MakeShared<FJsonObject>();
        for (const TPair<FString, FString>& Item : Impl::VisiblePropertyTexts(Subject))
        {
            ValueMap->SetStringField(Item.Key, Item.Value);
        }
        Document->SetObjectField(TEXT("properties"), ValueMap);
        Impl::CommitJson(File, Document, TEXT("Asset data exported to JSON successfully"));
        break;
    }
    case Impl::EDumpFormat::Text:
    {
        FString Body;
        Body += TEXT("Asset Export Report\n");
        Body += TEXT("==================\n");
        Body.Appendf(TEXT("Asset Path: %s\n"), *RequestedAsset);
        Body.Appendf(TEXT("Asset Class: %s\n"), *ClassPath);
        Body.Appendf(TEXT("Package Name: %s\n"), *PackagePath);
        Body.Appendf(TEXT("Asset Name: %s\n"), *AssetName);
        Body.Appendf(TEXT("Export Time: %s\n"), *FDateTime::Now().ToString());
        Body += TEXT("\nProperties:\n");
        for (const TPair<FString, FString>& Item : Impl::VisiblePropertyTexts(Subject))
        {
            Body.Appendf(TEXT("  %s: %s\n"), *Item.Key, *Item.Value);
        }
        Impl::CommitText(File, Body, TEXT("Asset data exported to text successfully"));
        break;
    }
    default:
        File.Status = TEXT("Unsupported export format");
        break;
    }

    const int64 PackageBytes = Impl::SourcePackageBytes(Disk, Entry);
    Impl::MeasureDumpFile(Disk, File);

    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("asset_path"), RequestedAsset);
    Answer->SetStringField(TEXT("export_format"), FormatToken);
    Impl::PutExportFileFields(Answer, File);
    Answer->SetStringField(TEXT("asset_class"), ClassPath);
    Answer->SetStringField(TEXT("package_name"), PackagePath);
    Answer->SetNumberField(TEXT("original_file_size_bytes"), PackageBytes);
    Answer->SetNumberField(TEXT("original_file_size_mb"), Impl::InMegabytes(PackageBytes));
    Impl::PutExportTrailer(Answer, File);
    return Answer;
}

// MCP:export_blueprint_data - Exports blueprint structure and metadata to JSON or TXT format.
//   @param blueprint_path - Full path to the blueprint asset (e.g., '/Game/MyBlueprint').
//   @param export_format - Export format: 'json' or 'txt'.
//   @returns { blueprint_path: string, export_format: string, export_success: bool, export_message: string, export_file_path: string, export_file_name: string, export_file_size_bytes: int, export_file_size_mb: number, blueprint_class: string, package_name: string, original_file_size_bytes: int, original_file_size_mb: number, export_timestamp: string, export_status: string, export_directory: string }

TSharedPtr<FJsonObject> HandleExportBlueprintData(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentExportImpl;

    FString RequestedBlueprint;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_path"), RequestedBlueprint, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString FormatToken;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("export_format"), FormatToken, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const FAssetData Entry = mcp_common::FindAssetDataByPath(RequestedBlueprint);
    if (!Entry.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Blueprint not found: %s"), *RequestedBlueprint));
    }
    // Anything that is not a UBlueprint counts as a failed load.
    UBlueprint* Blueprint = Cast<UBlueprint>(Entry.GetAsset());
    if (Blueprint == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load blueprint: %s"), *RequestedBlueprint));
    }

    IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
    const FString LowerToken = FormatToken.ToLower();
    const FString BlueprintName = FPaths::GetBaseFilename(RequestedBlueprint);
    Impl::FDumpFile File = Impl::PlanDumpFile(Disk, TEXT("Exports"), TEXT("Blueprints"), BlueprintName, LowerToken, TEXT("Export failed"));

    const FString ClassPath = Entry.AssetClassPath.ToString();
    const FString PackagePath = Entry.PackageName.ToString();
    const FString ParentName = Impl::ParentClassName(*Blueprint);
    const TCHAR* StatusName = Impl::BlueprintStatusName(*Blueprint);
    const int32 FunctionSlots = Blueprint->FunctionGraphs.Num();
    const int32 EventSlots = Blueprint->EventGraphs.Num();
    const int32 VariableSlots = Blueprint->NewVariables.Num();

    switch (Impl::DumpFormatFromToken(LowerToken))
    {
    case Impl::EDumpFormat::Json:
    {
        TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
        Layout->SetNumberField(TEXT("function_count"), FunctionSlots);
        Layout->SetNumberField(TEXT("event_count"), EventSlots);
        Layout->SetNumberField(TEXT("variable_count"), VariableSlots);
        Layout->SetStringField(TEXT("parent_class"), ParentName);
        Layout->SetStringField(TEXT("compilation_status"), StatusName);
        Layout->SetArrayField(TEXT("function_names"), mcp_common::StringsToJson(Impl::FunctionGraphNames(*Blueprint)));

        TArray<TSharedPtr<FJsonValue>> VariableValues;
        for (const Impl::FMemberVariableRow& Row : Impl::MemberVariableRows(*Blueprint))
        {
            TSharedPtr<FJsonObject> VariableJson = MakeShared<FJsonObject>();
            VariableJson->SetStringField(TEXT("name"), Row.Name);
            VariableJson->SetStringField(TEXT("type"), Row.PinCategory);
            VariableJson->SetStringField(TEXT("category"), Row.Category);
            VariableJson->SetBoolField(TEXT("is_public"), Row.bBlueprintVisible);
            VariableJson->SetBoolField(TEXT("is_editable"), Row.bEditable);
            VariableValues.Add(MakeShared<FJsonValueObject>(VariableJson));
        }
        Layout->SetArrayField(TEXT("variables"), VariableValues);

        TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
        Document->SetStringField(TEXT("blueprint_path"), RequestedBlueprint);
        Document->SetStringField(TEXT("blueprint_class"), ClassPath);
        Document->SetStringField(TEXT("package_name"), PackagePath);
        Document->SetStringField(TEXT("blueprint_name"), BlueprintName);
        Document->SetStringField(TEXT("export_timestamp"), FDateTime::Now().ToString());
        Document->SetBoolField(TEXT("is_blueprint"), true);
        Document->SetStringField(TEXT("blueprint_type"), TEXT("Blueprint"));
        Document->SetObjectField(TEXT("blueprint_info"), Layout);
        Impl::CommitJson(File, Document, TEXT("Blueprint data exported to JSON successfully"));
        break;
    }
    case Impl::EDumpFormat::Text:
    {
        FString Body;
        Body += TEXT("Blueprint Export Report\n");
        Body += TEXT("=====================\n");
        Body.Appendf(TEXT("Blueprint Path: %s\n"), *RequestedBlueprint);
        Body.Appendf(TEXT("Blueprint Class: %s\n"), *ClassPath);
        Body.Appendf(TEXT("Package Name: %s\n"), *PackagePath);
        Body.Appendf(TEXT("Blueprint Name: %s\n"), *BlueprintName);
        Body.Appendf(TEXT("Export Time: %s\n"), *FDateTime::Now().ToString());
        Body.Appendf(TEXT("Parent Class: %s\n"), *ParentName);
        Body.Appendf(TEXT("Compilation Status: %s\n"), StatusName);

        Body += TEXT("\nBlueprint Structure:\n");
        Body.Appendf(TEXT("  Functions: %d\n"), FunctionSlots);
        Body.Appendf(TEXT("  Events: %d\n"), EventSlots);
        Body.Appendf(TEXT("  Variables: %d\n"), VariableSlots);

        Body += TEXT("\nFunctions:\n");
        for (const FString& GraphName : Impl::FunctionGraphNames(*Blueprint))
        {
            Body.Appendf(TEXT("  - %s\n"), *GraphName);
        }
        Body += TEXT("\nVariables:\n");
        for (const Impl::FMemberVariableRow& Row : Impl::MemberVariableRows(*Blueprint))
        {
            Body.Appendf(TEXT("  - %s (%s): %s\n"), *Row.Name, *Row.PinCategory, *Row.Category);
        }
        Impl::CommitText(File, Body, TEXT("Blueprint data exported to text successfully"));
        break;
    }
    default:
        File.Status = TEXT("Unsupported export format");
        break;
    }

    const int64 PackageBytes = Impl::SourcePackageBytes(Disk, Entry);
    Impl::MeasureDumpFile(Disk, File);

    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("blueprint_path"), RequestedBlueprint);
    Answer->SetStringField(TEXT("export_format"), FormatToken);
    Impl::PutExportFileFields(Answer, File);
    Answer->SetStringField(TEXT("blueprint_class"), ClassPath);
    Answer->SetStringField(TEXT("package_name"), PackagePath);
    Answer->SetBoolField(TEXT("is_blueprint"), true);
    Answer->SetStringField(TEXT("blueprint_type"), TEXT("Blueprint"));
    Answer->SetNumberField(TEXT("original_file_size_bytes"), PackageBytes);
    Answer->SetNumberField(TEXT("original_file_size_mb"), Impl::InMegabytes(PackageBytes));
    Impl::PutExportTrailer(Answer, File);
    return Answer;
}

// MCP:export_level_section - Exports the actors of one section of the editor world (name, class, transform) to JSON or TXT. A section is a loaded level (the persistent level or a streaming sub-level, by package name), else an outliner folder (with its subfolders); '*' exports the whole world. An unknown section is an error that lists the loaded levels.
//   @param section_name - Level package name (e.g. 'L_City_Interiors'), outliner folder path (e.g. 'Lighting' or 'Props/Trees'), or '*' for the whole world.
//   @param export_format - Export format: 'json' or 'txt'.
//   @returns { section_name: string, export_format: string, export_success: bool, export_message: string, export_file_path: string, export_file_name: string, export_file_size_bytes: int, export_file_size_mb: number, level_name: string, world_name: string, section_scope: string ('world' | 'level' | 'folder'), actor_count: int, export_timestamp: string, export_status: string, export_directory: string }

TSharedPtr<FJsonObject> HandleExportLevelSection(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentExportImpl;

    FString RequestedSection;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("section_name"), RequestedSection, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString FormatToken;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("export_format"), FormatToken, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* SourceWorld = Impl::FirstContextWorld();
    if (SourceWorld == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("No world found"));
    }

    FString Scope;
    FString LevelName;
    const TArray<AActor*> SectionActors = Impl::ActorsInSection(SourceWorld, RequestedSection, Scope, LevelName);
    if (Scope.IsEmpty())
    {
        FString Levels;
        for (const ULevel* Level : SourceWorld->GetLevels())
        {
            if (Level != nullptr)
            {
                Levels += (Levels.IsEmpty() ? TEXT("") : TEXT(", ")) + FPackageName::GetShortName(Level->GetOutermost()->GetName());
            }
        }
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No loaded level or outliner folder named '%s'. Loaded levels: %s. Use '*' for the whole world."),
            *RequestedSection, *Levels));
    }

    IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
    const FString LowerToken = FormatToken.ToLower();
    const FString WorldName = SourceWorld->GetName();
    const FString FileStem = FString::Printf(TEXT("%s_%s"), *RequestedSection, *WorldName);
    Impl::FDumpFile File = Impl::PlanDumpFile(Disk, TEXT("Exports"), TEXT("Levels"), FileStem, LowerToken, TEXT("Export failed"));

    const TArray<Impl::FActorRow> Rows = Impl::CollectActorRows(SectionActors);

    switch (Impl::DumpFormatFromToken(LowerToken))
    {
    case Impl::EDumpFormat::Json:
    {
        TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
        Document->SetStringField(TEXT("section_name"), RequestedSection);
        Document->SetStringField(TEXT("section_scope"), Scope);
        Document->SetStringField(TEXT("level_name"), LevelName);
        Document->SetStringField(TEXT("world_name"), WorldName);
        Document->SetStringField(TEXT("export_timestamp"), FDateTime::Now().ToString());
        Document->SetNumberField(TEXT("actor_count"), Rows.Num());

        TArray<TSharedPtr<FJsonValue>> ActorValues;
        ActorValues.Reserve(Rows.Num());
        for (const Impl::FActorRow& Row : Rows)
        {
            TSharedPtr<FJsonObject> ActorJson = MakeShared<FJsonObject>();
            ActorJson->SetStringField(TEXT("name"), Row.Name);
            ActorJson->SetStringField(TEXT("class"), Row.ClassName);
            ActorJson->SetNumberField(TEXT("location_x"), Row.Location.X);
            ActorJson->SetNumberField(TEXT("location_y"), Row.Location.Y);
            ActorJson->SetNumberField(TEXT("location_z"), Row.Location.Z);
            ActorJson->SetNumberField(TEXT("rotation_pitch"), Row.Rotation.Pitch);
            ActorJson->SetNumberField(TEXT("rotation_yaw"), Row.Rotation.Yaw);
            ActorJson->SetNumberField(TEXT("rotation_roll"), Row.Rotation.Roll);
            ActorJson->SetNumberField(TEXT("scale_x"), Row.Scale.X);
            ActorJson->SetNumberField(TEXT("scale_y"), Row.Scale.Y);
            ActorJson->SetNumberField(TEXT("scale_z"), Row.Scale.Z);
            ActorValues.Add(MakeShared<FJsonValueObject>(ActorJson));
        }
        Document->SetArrayField(TEXT("actors"), ActorValues);
        Impl::CommitJson(File, Document, TEXT("Level section exported to JSON successfully"));
        break;
    }
    case Impl::EDumpFormat::Text:
    {
        FString Body;
        Body += TEXT("Level Section Export Report\n");
        Body += TEXT("==========================\n");
        Body.Appendf(TEXT("Section Name: %s (%s)\n"), *RequestedSection, *Scope);
        Body.Appendf(TEXT("Level Name: %s\n"), *LevelName);
        Body.Appendf(TEXT("World Name: %s\n"), *WorldName);
        Body.Appendf(TEXT("Export Time: %s\n"), *FDateTime::Now().ToString());
        Body.Appendf(TEXT("Actor Count: %d\n"), Rows.Num());

        Body += TEXT("\nActors:\n");
        int32 Ordinal = 0;
        for (const Impl::FActorRow& Row : Rows)
        {
            ++Ordinal;
            Body.Appendf(TEXT("  %d. %s (%s)\n"), Ordinal, *Row.Name, *Row.ClassName);
            Body.Appendf(TEXT("     Location: (%.2f, %.2f, %.2f)\n"), Row.Location.X, Row.Location.Y, Row.Location.Z);
            Body.Appendf(TEXT("     Rotation: (%.2f, %.2f, %.2f)\n"), Row.Rotation.Pitch, Row.Rotation.Yaw, Row.Rotation.Roll);
            Body.Appendf(TEXT("     Scale: (%.2f, %.2f, %.2f)\n"), Row.Scale.X, Row.Scale.Y, Row.Scale.Z);
        }
        Impl::CommitText(File, Body, TEXT("Level section exported to text successfully"));
        break;
    }
    default:
        File.Status = TEXT("Unsupported export format");
        break;
    }

    Impl::MeasureDumpFile(Disk, File);

    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("section_name"), RequestedSection);
    Answer->SetStringField(TEXT("export_format"), FormatToken);
    Impl::PutExportFileFields(Answer, File);
    Answer->SetStringField(TEXT("level_name"), LevelName);
    Answer->SetStringField(TEXT("world_name"), WorldName);
    Answer->SetStringField(TEXT("section_scope"), Scope);
    Answer->SetNumberField(TEXT("actor_count"), Rows.Num());
    Impl::PutExportTrailer(Answer, File);
    return Answer;
}

// MCP:generate_asset_report - Generates comprehensive asset analysis report with property statistics to JSON or TXT format.
//   @param asset_path - Full path to the asset to analyze (e.g., '/Game/MyAsset').
//   @param report_format - Report format: 'json' or 'txt'.
//   @returns { asset_path: string, report_format: string, report_success: bool, report_message: string, report_file_path: string, report_file_name: string, report_file_size_bytes: int, report_file_size_mb: number, asset_class: string, package_name: string, report_timestamp: string, report_status: string, report_directory: string, total_properties: int, editable_properties: int, blueprint_visible_properties: int, file_size_bytes: int, file_size_mb: number }

TSharedPtr<FJsonObject> HandleGenerateAssetReport(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentExportImpl;

    FString RequestedAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString FormatToken;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("report_format"), FormatToken, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FAssetData Entry;
    UObject* Subject = nullptr;
    if (TSharedPtr<FJsonObject> Error = Impl::LoadAssetEntry(RequestedAsset, Entry, Subject)) { return Error; }

    IPlatformFile& Disk = FPlatformFileManager::Get().GetPlatformFile();
    const FString LowerToken = FormatToken.ToLower();
    const FString AssetName = FPaths::GetBaseFilename(RequestedAsset);
    Impl::FDumpFile File = Impl::PlanDumpFile(Disk, TEXT("Reports"), TEXT("Assets"), AssetName + TEXT("_Report"), LowerToken,
        TEXT("Report generation failed"));

    const FString ClassPath = Entry.AssetClassPath.ToString();
    const FString PackagePath = Entry.PackageName.ToString();
    const TCHAR* ReportKind = TEXT("Asset Analysis");

    // The property statistics only go into the written file; the answer carries the file facts.
    switch (Impl::DumpFormatFromToken(LowerToken))
    {
    case Impl::EDumpFormat::Json:
    {
        TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
        Document->SetStringField(TEXT("asset_path"), RequestedAsset);
        Document->SetStringField(TEXT("asset_class"), ClassPath);
        Document->SetStringField(TEXT("package_name"), PackagePath);
        Document->SetStringField(TEXT("asset_name"), AssetName);
        Document->SetStringField(TEXT("report_timestamp"), FDateTime::Now().ToString());
        Document->SetStringField(TEXT("report_type"), ReportKind);

        const Impl::FPropertyCensus Census = Impl::TakePropertyCensus(Subject);
        TSharedPtr<FJsonObject> Statistics = MakeShared<FJsonObject>();
        Statistics->SetNumberField(TEXT("total_properties"), Census.Names.Num());
        Statistics->SetNumberField(TEXT("editable_properties"), Census.EditableCount);
        Statistics->SetNumberField(TEXT("blueprint_visible_properties"), Census.BlueprintVisibleCount);
        Statistics->SetArrayField(TEXT("property_names"), mcp_common::StringsToJson(Census.Names));
        Statistics->SetArrayField(TEXT("property_types"), mcp_common::StringsToJson(Census.TypeNames));
        Statistics->SetArrayField(TEXT("property_values"), mcp_common::StringsToJson(Census.Texts));
        Document->SetObjectField(TEXT("properties"), Statistics);

        const int64 PackageBytes = Impl::SourcePackageBytes(Disk, Entry);
        Document->SetNumberField(TEXT("file_size_bytes"), PackageBytes);
        Document->SetNumberField(TEXT("file_size_mb"), Impl::InMegabytes(PackageBytes));
        Impl::CommitJson(File, Document, TEXT("Asset report generated to JSON successfully"));
        break;
    }
    case Impl::EDumpFormat::Text:
    {
        FString Body;
        Body += TEXT("Asset Analysis Report\n");
        Body += TEXT("====================\n");
        Body.Appendf(TEXT("Asset Path: %s\n"), *RequestedAsset);
        Body.Appendf(TEXT("Asset Class: %s\n"), *ClassPath);
        Body.Appendf(TEXT("Package Name: %s\n"), *PackagePath);
        Body.Appendf(TEXT("Asset Name: %s\n"), *AssetName);
        Body.Appendf(TEXT("Report Time: %s\n"), *FDateTime::Now().ToString());
        Body += TEXT("Report Type: Asset Analysis\n");

        const int64 PackageBytes = Impl::SourcePackageBytes(Disk, Entry);
        Body.Appendf(TEXT("File Size: %lld bytes (%.2f MB)\n"), PackageBytes, Impl::InMegabytes(PackageBytes));

        Body += TEXT("\nProperties Analysis:\n");
        const Impl::FPropertyCensus Census = Impl::TakePropertyCensus(Subject);
        for (int32 Index = 0; Index < Census.Names.Num(); ++Index)
        {
            Body.Appendf(TEXT("  %s (%s): %s\n"), *Census.Names[Index], *Census.TypeNames[Index], *Census.Texts[Index]);
        }

        Body += TEXT("\nProperty Summary:\n");
        Body.Appendf(TEXT("  Total Properties: %d\n"), Census.Names.Num());
        Body.Appendf(TEXT("  Editable Properties: %d\n"), Census.EditableCount);
        Body.Appendf(TEXT("  Blueprint Visible Properties: %d\n"), Census.BlueprintVisibleCount);
        Impl::CommitText(File, Body, TEXT("Asset report generated to text successfully"));
        break;
    }
    default:
        File.Status = TEXT("Unsupported report format");
        break;
    }

    Impl::MeasureDumpFile(Disk, File);

    TSharedPtr<FJsonObject> Answer = MakeShared<FJsonObject>();
    Answer->SetStringField(TEXT("asset_path"), RequestedAsset);
    Answer->SetStringField(TEXT("report_format"), FormatToken);
    Answer->SetBoolField(TEXT("report_success"), File.bWritten);
    Answer->SetStringField(TEXT("report_message"), File.Status);
    Answer->SetStringField(TEXT("report_file_path"), File.FullPath);
    Answer->SetStringField(TEXT("report_file_name"), File.Name);
    Answer->SetNumberField(TEXT("report_file_size_bytes"), File.Bytes);
    Answer->SetNumberField(TEXT("report_file_size_mb"), Impl::InMegabytes(File.Bytes));
    Answer->SetStringField(TEXT("asset_class"), ClassPath);
    Answer->SetStringField(TEXT("package_name"), PackagePath);
    Answer->SetStringField(TEXT("report_timestamp"), FDateTime::Now().ToString());
    Answer->SetStringField(TEXT("report_status"), File.bWritten ? TEXT("Completed") : TEXT("Failed"));
    Answer->SetStringField(TEXT("report_type"), ReportKind);
    Answer->SetStringField(TEXT("report_directory"), File.Folder);
    return Answer;
}

// ==========================================================================
// export_asset_as_text
// ==========================================================================
namespace
{
    // Deterministic JSON dump: a fixed-order header + a `properties` object whose
    // keys are inserted in SORTED order (FJsonObject preserves insertion order, so
    // this serializes stably) and whose values are typed via the canonical reader.
    // Transient (runtime-only) properties are stripped so nothing volatile leaks in.
    FString BuildCanonicalAssetJson_Export(UObject* Asset)
    {
        TMap<FString, TSharedPtr<FJsonValue>> Fields;
        for (TFieldIterator<FProperty> It(Asset->GetClass()); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property) { continue; }
            if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) { continue; }
            TSharedPtr<FJsonValue> Value = mcp_common::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Asset));
            if (Value.IsValid()) { Fields.Add(Property->GetName(), Value); }
        }

        TArray<FString> Keys;
        Fields.GetKeys(Keys);
        Keys.Sort();

        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        for (const FString& K : Keys) { Props->SetField(K, Fields[K]); }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("object"), Asset->GetPathName());
        Root->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
        Root->SetObjectField(TEXT("properties"), Props);

        FString Out;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
        FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
        return Out;
    }

    FString Md5OfText_Export(const FString& Text)
    {
        FTCHARToUTF8 Utf8(*Text);
        FMD5 Md5Gen;
        Md5Gen.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        uint8 Digest[16];
        Md5Gen.Final(Digest);
        return BytesToHex(Digest, 16);
    }
}

// MCP:export_asset_as_text - Produce a CANONICAL, STABLE text dump of ANY asset (Blueprint, DataTable, Material, DataAsset, ...) for external diff / VCS snapshot / review, without the volatile bits of the binary .uasset. format='json' (default) gives a deterministic sorted UPROPERTY dump (a UDataTable uses its native GetTableAsJSON writer); format='t3d' gives the engine's UExporter T3D text. Returns the text inline (capped), a whole-text MD5 content hash for change detection, and writes the full dump to Saved/Exports/Text. This is the primitive under the diff tools.
//   @param asset_path - Object path or /Game package path of the asset to dump.
//   @param format - 'json' (default; deterministic reflection / DataTable-JSON) or 't3d' (UExporter T3D).
//   @param write_file - Optional bool (default true). Also write the full dump under Saved/Exports/Text.
//   @returns { asset_path: string, object_path: string, asset_class: string, format: string, text: string, text_length: int, truncated: bool, content_hash: string, export_file_path: string, written: bool, message: string }
TSharedPtr<FJsonObject> HandleExportAssetAsText(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }
    FString Format = TEXT("json");
    Params->TryGetStringField(TEXT("format"), Format);
    Format = Format.ToLower();
    if (Format != TEXT("json") && Format != TEXT("t3d"))
    {
        return mcp_common::MakeErrorReply(TEXT("format must be 'json' or 't3d'"));
    }
    bool bWriteFile = true;
    Params->TryGetBoolField(TEXT("write_file"), bWriteFile);

    // Resolve to an object path (append the leaf name for a bare package path).
    FString ObjectPath = AssetPath;
    if (!ObjectPath.Contains(TEXT(".")))
    {
        const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
        const FString LeafName = FPackageName::GetLongPackageAssetName(PackageName);
        ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *LeafName);
    }
    UObject* Asset = mcp_common::ResolveObjectByPath(ObjectPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found / could not be loaded: %s"), *ObjectPath));
    }

    FString Text;
    FString UsedFormat = Format;
    if (Format == TEXT("t3d"))
    {
        FStringOutputDevice Ar;
        FExportObjectInnerContext Context;
        UExporter::ExportToOutputDevice(&Context, Asset, /*InExporter=*/nullptr, Ar, TEXT("T3D"), /*Indent=*/0,
            PPF_ExportsNotFullyQualified | PPF_Copy, /*bInSelectedOnly=*/false, /*ExportRootScope=*/nullptr);
        Text = Ar;
        if (Text.IsEmpty())
        {
            // No T3D exporter for this asset type -> honestly fall back to the canonical JSON dump.
            Text = BuildCanonicalAssetJson_Export(Asset);
            UsedFormat = TEXT("json (no T3D exporter for this type)");
        }
    }
    else if (UDataTable* DataTable = Cast<UDataTable>(Asset))
    {
        // Native DataTable-JSON writer (rows serialized field-by-field).
        Text = DataTable->GetTableAsJSON(EDataTableExportFlags::None);
        UsedFormat = TEXT("json (DataTable)");
    }
    else
    {
        Text = BuildCanonicalAssetJson_Export(Asset);
    }

    const FString ContentHash = Md5OfText_Export(Text);

    // Persist the FULL dump; return a capped tail inline.
    FString ExportFilePath;
    bool bWritten = false;
    if (bWriteFile)
    {
        const FString ExportDir = FPaths::ProjectSavedDir() / TEXT("Exports") / TEXT("Text");
        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        PlatformFile.CreateDirectoryTree(*ExportDir);
        const FString Ext = Format == TEXT("t3d") ? TEXT("t3d") : TEXT("json");
        const FString FileName = FString::Printf(TEXT("%s_%s.%s"),
            *FPaths::GetBaseFilename(Asset->GetName()), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), *Ext);
        ExportFilePath = ExportDir / FileName;
        bWritten = FFileHelper::SaveStringToFile(Text, *ExportFilePath);
    }

    const int32 MaxInline = 60000;
    const bool bTruncated = Text.Len() > MaxInline;
    const FString InlineText = bTruncated ? Text.Left(MaxInline) : Text;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("object_path"), Asset->GetPathName());
    Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
    Result->SetStringField(TEXT("format"), UsedFormat);
    Result->SetStringField(TEXT("text"), InlineText);
    Result->SetNumberField(TEXT("text_length"), Text.Len());
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetStringField(TEXT("content_hash"), ContentHash);
    Result->SetStringField(TEXT("export_file_path"), ExportFilePath);
    Result->SetBoolField(TEXT("written"), bWritten);
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("Exported %s as %s (%d chars, md5 %s)%s."),
        *Asset->GetName(), *UsedFormat, Text.Len(), *ContentHash,
        bWritten ? *FString::Printf(TEXT(" -> %s"), *ExportFilePath) : TEXT("")));
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("export_asset_as_text"), &HandleExportAssetAsText },
    { TEXT("export_asset_data"), &HandleExportAssetData },
    { TEXT("export_blueprint_data"), &HandleExportBlueprintData },
    { TEXT("export_level_section"), &HandleExportLevelSection },
    { TEXT("generate_asset_report"), &HandleGenerateAssetReport },
};
} // namespace UEAgentExportCommands
