#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Data/DataAuthoringUtils.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"     // create_composite_data_table + composite-aware get_data_table_info
#include "Kismet/DataTableFunctionLibrary.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/PropertyPortFlags.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
// DataTable row authoring + curve assets
#include "DataTableEditorUtils.h"          // FDataTableEditorUtils (UnrealEd)
#include "Curves/CurveBase.h"              // UCurveBase
#include "Curves/CurveFloat.h"             // UCurveFloat / FRichCurve
#include "Curves/CurveVector.h"            // UCurveVector (FloatCurves[3])
#include "Curves/CurveLinearColor.h"       // UCurveLinearColor (FloatCurves[4])
#include "Curves/RichCurve.h"              // FRichCurve / FRichCurveKey
#include "Curves/RealCurve.h"              // ERichCurveInterpMode (RCIM_*)
#include "Curves/KeyHandle.h"              // FKeyHandle
#include "JsonObjectConverter.h"           // FJsonObjectConverter::JsonValueToUProperty (JsonUtilities)
#include "Serialization/JsonWriter.h"      // TJsonWriter/TJsonWriterFactory for WriteRowAsJSON
#include "Serialization/JsonReader.h"      // parse row_json back into an FJsonObject
#include "Serialization/JsonSerializer.h"  // FJsonSerializer::Deserialize/Serialize
#include "Policies/PrettyJsonPrintPolicy.h"// WriteRowAsJSON is instantiated for TPrettyJsonPrintPolicy<TCHAR>
#include "Dom/JsonValue.h"                 // diff_data_tables: FJsonValue::CompareEqual + FJsonValueNull/Array/Object
#include "Dom/JsonObject.h"                // diff_data_tables: FJsonObject::Values / TryGetField
// External .uasset read via throwaway mount + collision-safe LoadPackage
#include "Misc/Paths.h"                    // FPaths::FileExists / GetPath / GetBaseFilename / ConvertRelativePathToFull
#include "Misc/FileHelper.h"               // FFileHelper::SaveStringToFile for export_data_table_as_{json,csv} file_path
#include "UObject/UnrealType.h"            // FMapProperty / FSoftObjectProperty / FStructProperty for get_data_table_info columns
#include "HAL/IConsoleManager.h"           // CVar guard (DataTableJSON.ExportUsingPropertyVisitor) in MCPExt_RowToNativeJsonObject
#include "Misc/Guid.h"                     // FGuid::NewGuid for a unique throwaway mount root
#include "UObject/UObjectHash.h"           // ForEachObjectWithPackage (find the DataTable + clear keep-flags)
#include "UObject/GarbageCollection.h"     // CollectGarbage / GARBAGE_COLLECTION_KEEPFLAGS after the throwaway load
// set_data_table_cell leaf write/read (array-of-structs element sub-field).
#include "DataTableUtils.h"                // DataTableUtils::AssignStringToPropertyDirect (enum-friendly-name-aware ImportText, PUBLIC Engine header)
#include "StructUtils/UserDefinedStruct.h" // UUserDefinedStruct (authored-name fallback for the array-element struct; CoreUObject/StructUtils, not Engine/)

/**
 * Game-data authoring and reading: UDataAsset instances, UDataTable / UCompositeDataTable assets with
 * their rows, cells and whole-table exports, and UCurveBase curve assets with their keys.
 */
namespace UEAgentDataAssetCommands
{

// MCP-DOMAIN: DataAsset

// Every row/struct-field value in this file goes through the typed
// mcp_common::PropertyValueToJson (numbers/bools/strings native, tags flat,
// object-refs as path strings) instead of a flat ExportText_Direct string for
// every kind, so get_data_table_rows returns typed JSON.

// ===========================================================================
// Shared plumbing of the DataAsset / DataTable handlers: required-parameter
// errors, loading a DataAsset or a DataTable with their standard refusals, the
// typed row and column JSON, the whole-table export and the steps the row
// writers share.
// ===========================================================================
namespace UEAgentDataAssetCommandsImpl
{

    /** "<Folder>/<Name>": the package a create_* command writes. */
    FString JoinPackagePath(const FString& Folder, const FString& AssetName)
    {
        return Folder + TEXT("/") + AssetName;
    }

    UDataAsset* LoadDataAssetAt(const FString& Path)
    {
        return Cast<UDataAsset>(StaticLoadObject(UDataAsset::StaticClass(), nullptr, *Path));
    }

    /**
     * The DataTable (plain or composite) at Path, provided it loads -- and, with bNeedRowStruct, also has a
     * row struct. Otherwise nullptr, with the error reply the table handlers send left in OutRefusal.
     */
    UDataTable* OpenTable(const FString& Path, bool bNeedRowStruct, TSharedPtr<FJsonObject>& OutRefusal)
    {
        UDataTable* Table = Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *Path));
        if (Table == nullptr)
        {
            OutRefusal = mcp_common::MakeErrorReply(TEXT("DataTable not found: ") + Path);
            return nullptr;
        }
        if (bNeedRowStruct && Table->RowStruct == nullptr)
        {
            OutRefusal = mcp_common::MakeErrorReply(TEXT("DataTable has no row struct"));
            return nullptr;
        }
        return Table;
    }

    /** Every column of one row as typed JSON (mcp_common::PropertyValueToJson: numbers, bools, flat tags, paths). */
    TSharedPtr<FJsonObject> RowColumnsToJson(const UScriptStruct* RowStruct, const void* RowMemory)
    {
        TSharedPtr<FJsonObject> Columns = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> Field(RowStruct); Field; ++Field)
        {
            FProperty* Column = *Field;
            if (Column != nullptr)
            {
                Columns->SetField(Column->GetName(),
                    mcp_common::PropertyValueToJson(Column, Column->ContainerPtrToValuePtr<void>(RowMemory)));
            }
        }
        return Columns;
    }

    /**
     * Column metadata of a row struct: name and C++ type, plus the three shapes the reflection readers treat
     * specially -- TMaps (TMap<Enum,SoftClass> collapses there), soft references (FSoftClassProperty derives
     * from FSoftObjectProperty, so one test covers both) and GameplayTag / GameplayTagContainer structs.
     */
    TArray<TSharedPtr<FJsonValue>> DescribeColumns(const UScriptStruct* RowStruct)
    {
        TArray<TSharedPtr<FJsonValue>> Columns;
        if (RowStruct == nullptr)
        {
            return Columns;
        }
        for (TFieldIterator<FProperty> Field(RowStruct); Field; ++Field)
        {
            const FProperty* Column = *Field;
            if (Column == nullptr)
            {
                continue;
            }
            bool bTag = false;
            if (const FStructProperty* AsStruct = CastField<FStructProperty>(Column))
            {
                const FString StructName = AsStruct->Struct ? AsStruct->Struct->GetName() : FString();
                bTag = (StructName == TEXT("GameplayTag") || StructName == TEXT("GameplayTagContainer"));
            }
            TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
            Info->SetStringField(TEXT("name"), Column->GetName());
            Info->SetStringField(TEXT("type"), Column->GetCPPType());
            Info->SetBoolField(TEXT("is_map"), CastField<FMapProperty>(Column) != nullptr);
            Info->SetBoolField(TEXT("is_soft"), CastField<FSoftObjectProperty>(Column) != nullptr);
            Info->SetBoolField(TEXT("is_tag"), bTag);
            Columns.Add(MakeShared<FJsonValueObject>(Info));
        }
        return Columns;
    }

    /** Name and FProperty kind of every reflected property of a DataAsset class (inherited ones included). */
    // Every reflected property of the asset: name, property type and its CURRENT value on this
    // asset as typed JSON (mcp_common::PropertyValueToJson).
    TArray<TSharedPtr<FJsonValue>> DescribeAssetProperties(UObject* Asset)
    {
        TArray<TSharedPtr<FJsonValue>> Listed;
        for (TFieldIterator<FProperty> Field(Asset->GetClass()); Field; ++Field)
        {
            FProperty* Property = *Field;
            if (Property == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("name"), Property->GetName());
            Entry->SetStringField(TEXT("type"), Property->GetClass()->GetName());
            Entry->SetField(TEXT("value"), mcp_common::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Asset)));
            Listed.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Listed;
    }

    /**
     * A JSON value as the text token ImportText understands, the same normalization WriteDataTableRow uses:
     * numbers through SanitizeFloat, bools as true/false, anything else by its string form.
     */
    FString JsonValueAsImportText(const TSharedPtr<FJsonValue>& Value)
    {
        if (!Value.IsValid())
        {
            return FString();
        }
        switch (Value->Type)
        {
            case EJson::Number:  return FString::SanitizeFloat(Value->AsNumber());
            case EJson::Boolean: return Value->AsBool() ? FString(TEXT("true")) : FString(TEXT("false"));
            default:             return Value->AsString();
        }
    }

    /**
     * export_data_table_as_json / export_data_table_as_csv: the WHOLE table through the engine's own
     * exporter (GetTableAsJSON / GetTableAsCSV -- there is no WriteToJSONString / WriteToCSVString), so
     * TMap<Enum,SoftClass>, soft references and tags survive, unlike the reflection row readers. Returned as
     * a string and, when file_path is given, also written there.
     */
    TSharedPtr<FJsonObject> ExportWholeTable(const TSharedPtr<FJsonObject>& Params, bool bAsCsv)
    {
        FString TablePath;
        if (!Params->TryGetStringField(TEXT("data_table"), TablePath) &&
            !Params->TryGetStringField(TEXT("table_path"), TablePath))
        {
            return mcp_common::MissingParameterReply(TEXT("table_path"));
        }
        TSharedPtr<FJsonObject> Refusal;
        UDataTable* Table = UEAgentDataAssetCommandsImpl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
        if (Table == nullptr)
        {
            return Refusal;
        }

        const FString Exported = bAsCsv
            ? Table->GetTableAsCSV(EDataTableExportFlags::None)
            : Table->GetTableAsJSON(EDataTableExportFlags::None);

        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("table_path"), TablePath);
        Reply->SetStringField(TEXT("row_struct"), Table->RowStruct->GetName());
        Reply->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());
        Reply->SetStringField(bAsCsv ? TEXT("csv") : TEXT("json"), Exported);

        FString TargetFile;
        if (Params->TryGetStringField(TEXT("file_path"), TargetFile) && !TargetFile.IsEmpty())
        {
            // ForceUTF8: AutoDetect switches to UTF-16LE with a BOM as soon as one cell holds non-ASCII text
            // (accented names, say), and that breaks the round-trip tooling.
            if (!FFileHelper::SaveStringToFile(Exported, *TargetFile, FFileHelper::EEncodingOptions::ForceUTF8))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Failed to write %s to file: %s"), bAsCsv ? TEXT("CSV") : TEXT("JSON"), *TargetFile));
            }
            Reply->SetStringField(TEXT("written_to"), TargetFile);
        }
        return mcp_common::MakeOkReply(Reply);
    }

    /** Reads the required "table_path" and "row_name" of a single-row command; the error reply when either is missing. */
    TSharedPtr<FJsonObject> ReadTableAndRow(const TSharedPtr<FJsonObject>& Params, FString& OutTablePath, FString& OutRowName)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), OutTablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), OutRowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        return nullptr;
    }

    /** The refusal for a row key that Table (which has a row struct) does not hold. */
    TSharedPtr<FJsonObject> RowNotFoundError(const UDataTable* Table, const FString& TablePath, const FString& RowName)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Row not found: '%s' is not a row in DataTable '%s' (row struct '%s')"),
            *RowName, *TablePath, *Table->RowStruct->GetName()));
    }

    /** OpenTable (with a row struct), then its row RowName into OutRow; the error reply when either is missing. */
    TSharedPtr<FJsonObject> OpenRow(const FString& TablePath, const FString& RowName, UDataTable*& OutTable, const uint8*& OutRow)
    {
        TSharedPtr<FJsonObject> Refusal;
        OutTable = OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
        if (OutTable == nullptr)
        {
            return Refusal;
        }
        OutRow = OutTable->FindRowUnchecked(FName(*RowName));
        if (OutRow == nullptr)
        {
            return RowNotFoundError(OutTable, TablePath, RowName);
        }
        return nullptr;
    }

    /** The row-struct field named Column: by property name, else by the authored (display) name the table knows it by. */
    FProperty* FindColumn(const UDataTable* Table, const FString& Column)
    {
        FProperty* Property = Table->RowStruct->FindPropertyByName(FName(*Column));
        if (Property == nullptr)
        {
            Property = Table->FindTableProperty(FName(*Column));
        }
        return Property;
    }

    /**
     * The memory of Scope (a row of Table's row struct), seeded with the live row RowName when bExisted so the
     * columns a write leaves alone keep their values; otherwise it keeps the struct defaults.
     */
    uint8* SeedWithLiveRow(FStructOnScope& Scope, const UDataTable* Table, FName RowName, bool bExisted)
    {
        uint8* Memory = Scope.GetStructMemory();
        if (Memory && bExisted)
        {
            const uint8* Current = Table->FindRowUnchecked(RowName);
            if (Current)
            {
                Table->RowStruct->CopyScriptStruct(Memory, Current);
            }
        }
        return Memory;
    }

    /** Adds row RowName from RowMemory (AddRow replaces an existing row) after Modify(), inside the editor's change broadcasts. */
    void AddRowWithEditorNotify(UDataTable* Table, FName RowName, const uint8* RowMemory, UScriptStruct* RowStruct)
    {
        Table->Modify();
        FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
        Table->AddRow(RowName, RowMemory, RowStruct);
        FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
    }

    /**
     * Rows (complete row objects, keyed through the import key field) through UE's own DataTable JSON importer, into
     * a transient table with Table's row struct and key field: the path that round-trips nested structs,
     * FGameplayTag(Container), soft references and TMap<Enum,SoftClass> exactly. OutProblems receives what the
     * importer reported.
     */
    UDataTable* ImportRowsIntoTransientTable(const UDataTable* Table, const TArray<TSharedPtr<FJsonValue>>& Rows, TArray<FString>& OutProblems)
    {
        FString RowsJson;
        {
            TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&RowsJson);
            FJsonSerializer::Serialize(Rows, Writer);
            Writer->Close();
        }
        UDataTable* Transient = NewObject<UDataTable>(GetTransientPackage(), NAME_None, RF_Transient);
        Transient->RowStruct = Table->RowStruct;
        Transient->ImportKeyField = Table->ImportKeyField;
        OutProblems = Transient->CreateTableFromJSONString(RowsJson);
        return Transient;
    }

    /** Problems without the importer's "Row 'X' is missing an entry for 'Y'." lines (that phrase: a bare "missing" also hits real errors). */
    TArray<FString> DropMissingEntryProblems(const TArray<FString>& Problems)
    {
        TArray<FString> Kept;
        for (const FString& Problem : Problems)
        {
            if (!Problem.Contains(TEXT("is missing an entry for")))
            {
                Kept.Add(Problem);
            }
        }
        return Kept;
    }

    /** The refusal when the native import yields no row RowName, with the importer's Problems appended. */
    TSharedPtr<FJsonObject> NoImportedRowError(const FString& RowName, const TArray<FString>& Problems)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Native JSON import produced no row '%s'%s"), *RowName,
            Problems.Num() > 0 ? *(FString(TEXT(": ")) + FString::Join(Problems, TEXT("; "))) : TEXT("")));
    }

    /** One parent_row_struct_mismatch entry: the parent at Index, whose row struct is not CompositeRowStruct. */
    TSharedPtr<FJsonValue> RowStructMismatch(int32 Index, const UDataTable* Parent, const UScriptStruct* CompositeRowStruct)
    {
        TSharedPtr<FJsonObject> MismatchObj = MakeShared<FJsonObject>();
        MismatchObj->SetNumberField(TEXT("index"), Index);
        MismatchObj->SetStringField(TEXT("path"), Parent->GetPathName());
        MismatchObj->SetStringField(TEXT("parent_row_struct"), Parent->RowStruct ? Parent->RowStruct->GetName() : TEXT("None"));
        MismatchObj->SetStringField(TEXT("composite_row_struct"), CompositeRowStruct ? CompositeRowStruct->GetName() : TEXT("None"));
        return MakeShared<FJsonValueObject>(MismatchObj);
    }

    /** The reply of create_curve_asset before its outcome: the request, then the curve asset the path holds. */
    TSharedPtr<FJsonObject> CreatedCurveReply(const FString& Name, const FString& PackagePath, const FString& CurveType,
        const UObject* Curve, int32 NumChannels)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("curve_type"), CurveType);
        Reply->SetStringField(TEXT("asset_path"), Curve->GetPathName());
        Reply->SetStringField(TEXT("curve_class"), Curve->GetClass()->GetName());
        Reply->SetNumberField(TEXT("num_channels"), NumChannels);
        return Reply;
    }
}

// Single-record data assets: create one, read it back, write one property.

// MCP:create_data_asset - Create a DataAsset instance of a given parent class -- the single-record alternative to a DataTable row, for configuration that wants its own asset and its own reference (a biome definition, a lock profile, a weapon config). Pick a DataAsset when things point AT it individually; pick a DataTable when you want many rows edited together. Fill it with set_data_asset_property and read it back with get_data_asset_info.
//   @param asset_name - Name of the new DataAsset to create.
//   @param package_path - Target content package path where the asset is created (check the body for the default)
//   @param parent_class - DataAsset parent class name (defaults to DataAsset)
//   @returns { asset_name, package_path, parent_class, asset_path, success, saved, message }
TSharedPtr<FJsonObject> HandleCreateDataAsset(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Both of these fall back to a default when the caller leaves them out.
    FString Folder;
    if (!Params->TryGetStringField(TEXT("package_path"), Folder)) { Folder = TEXT("/Game/DataAssets"); }
    FString ClassName;
    if (!Params->TryGetStringField(TEXT("parent_class"), ClassName)) { ClassName = TEXT("DataAsset"); }

    UClass* AssetClass = FindFirstObject<UClass>(*ClassName);
    if (AssetClass == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Parent class not found: ") + ClassName);
    }
    if (!AssetClass->IsChildOf(UDataAsset::StaticClass()))
    {
        return mcp_common::MakeErrorReply(TEXT("Parent class is not a DataAsset: ") + ClassName);
    }
    // An abstract class (UDataAsset itself, for one) cannot back a real asset: NewObject yields a degenerate
    // instance that the linker nulls out on save. Refused honestly -- a concrete subclass is required.
    if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Cannot create DataAsset from abstract class '%s' -- provide a concrete subclass"), *ClassName));
    }

    const FString TargetPath = Impl::JoinPackagePath(Folder, NewName);
    // The registry is asked first; the load is only attempted when it knows nothing at that path.
    if (UEditorAssetLibrary::DoesAssetExist(TargetPath) || Impl::LoadDataAssetAt(TargetPath) != nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("DataAsset already exists: ") + TargetPath);
    }

    // A real package at the target path (the same create flow as create_material / create_blueprint), so
    // the asset persists to disk and reloads like any other.
    FString CreateError;
    UDataAsset* Created = static_cast<UDataAsset*>(mcp_common::NewAssetInPackage(
        AssetClass, TargetPath, NewName, TEXT("Failed to create DataAsset"), CreateError, RF_Public | RF_Standalone));
    if (Created == nullptr)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }
    FAssetRegistryModule::AssetCreated(Created);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Created->GetOutermost(), Created, SaveError);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("asset_name"), NewName);
    Reply->SetStringField(TEXT("package_path"), Folder);
    Reply->SetStringField(TEXT("parent_class"), ClassName);
    Reply->SetStringField(TEXT("asset_path"), Created->GetPathName());
    Reply->SetBoolField(TEXT("saved"), bSaved);
    Reply->SetBoolField(TEXT("success"), bSaved);
    Reply->SetStringField(TEXT("message"), bSaved
        ? FString(TEXT("DataAsset created and saved to disk"))
        : TEXT("DataAsset created but save failed: ") + SaveError);

    // The save verdict measured above is the verdict emitted; a hardcoded success:true from
    // MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(Reply, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_data_asset"), TEXT("bSaved")));
}

// MCP:get_data_asset_info - Read a DataAsset's class and ALL of its reflected properties with their current values -- the read-back oracle for set_data_asset_property, and better than inspecting it from Python, where StructBase.to_dict() on a user-defined struct returns an empty dict and a GameplayTag prints as {} whether it holds a tag or not, both of which read as "nothing was written" when the values are actually there.
//   @param asset_path - The full asset path to the DataAsset.
//   @returns { asset_path, asset_name, asset_class, asset_path_name, properties: [{ name, type, value }] }
TSharedPtr<FJsonObject> HandleGetDataAssetInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UDataAsset* Asset = Impl::LoadDataAssetAt(RequestedPath);
    if (Asset == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("DataAsset not found: ") + RequestedPath);
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("asset_path"), RequestedPath);
    Reply->SetStringField(TEXT("asset_path_name"), Asset->GetPathName());
    Reply->SetStringField(TEXT("asset_name"), Asset->GetName());
    Reply->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
    Reply->SetArrayField(TEXT("properties"), Impl::DescribeAssetProperties(Asset));
    return mcp_common::MakeOkReply(Reply);
}

// MCP:set_data_asset_property - Write ONE reflected property on a DataAsset from a text value and verify it by reading back, returning old_value and new_value so the reply proves the change instead of merely claiming it. Values use UE text form: structs as "(X=1,Y=2,Z=3)", object references as a full asset path, GameplayTags as (TagName="A.B"). One property per call, so loop for several; get_data_asset_info dumps them all.
//   @param data_asset_path - Full asset path of the DataAsset.
//   @param property_name - Name of the UPROPERTY to set (class property name).
//   @param value - New value as a string (numbers/bools accepted); structs use UE text form e.g. "(X=1,Y=2,Z=3)"; object refs use a full asset path.
//   @returns { data_asset_path: string, property_name: string, property_type: string, old_value: string, new_value: string, applied: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetDataAssetProperty(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_asset_path"), RequestedPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Any JSON type is accepted for value; it is normalized to text further down.
    if (!Params->HasField(TEXT("value")))
    {
        return mcp_common::MissingParameterReply(TEXT("value"));
    }

    UDataAsset* Asset = Impl::LoadDataAssetAt(RequestedPath);
    if (Asset == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("DataAsset not found: ") + RequestedPath);
    }
    // Looked up by its class (C++) name.
    FProperty* Property = Asset->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (Property == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown property '%s'"), *PropertyName));
    }

    // The value before the write, read through reflection, so the reply can prove the change.
    const TSharedPtr<FJsonValue> Before = mcp_common::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Asset));
    const FString ValueText = Impl::JsonValueAsImportText(Params->TryGetField(TEXT("value")));

    Asset->PreEditChange(Property);
    // Imported into the live asset: the container is the DataAsset object itself.
    if (Property->ImportText_InContainer(*ValueText, Asset, Asset, PPF_None) == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Could not import value '%s' for property '%s' (type %s)"),
            *ValueText, *PropertyName, *Property->GetClass()->GetName()));
    }
    FPropertyChangedEvent ChangeEvent(Property);
    Asset->PostEditChangeProperty(ChangeEvent);
    Asset->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Asset->GetOutermost(), Asset, SaveError);

    // Verification by readback: the value as the object holds it now.
    const TSharedPtr<FJsonValue> After = mcp_common::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Asset));

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("data_asset_path"), RequestedPath);
    Reply->SetStringField(TEXT("property_name"), PropertyName);
    Reply->SetStringField(TEXT("property_type"), Property->GetClass()->GetName());
    Reply->SetField(TEXT("old_value"), Before);
    Reply->SetField(TEXT("new_value"), After);
    Reply->SetBoolField(TEXT("applied"), true);

    return mcp_common::MakeSavedReply(Reply, bSaved, TEXT("set_data_asset_property"), SaveError);
}

// ========================================================================== //
// Composite DataTable support -- shared by create_composite_data_table and
// by the composite-aware half of get_data_table_info. Declared HERE (above both
// handlers) on purpose; the other anonymous-namespace block further down is the
// same namespace in this TU, so names are prefixed MCPExt_ to avoid collisions.
//
// A UCompositeDataTable is a UDataTable whose row map is the MERGE of its parent
// tables; CompositeDataTable.h documents the precedence as "Tables with higher
// indices override data in tables with lower indices". Three engine facts drive
// everything below (all verified against the Engine sources):
//
//  * UCompositeDataTable::UpdateCachedRowMap() opens an FMessageDialog -- a MODAL
//    that blocks the game thread and would kill the live MCP session -- when it
//    finds a cyclic parent chain (CompositeDataTable.cpp) or a parent whose
//    RowStruct differs from the composite's (CompositeDataTable.cpp). Both
//    AppendParentTables() and RemoveParentTables() reach it with
//    bWarnOnInvalidChildren=true (they pass EPropertyChangeType::ValueSet). So
//    BOTH conditions are validated here BEFORE any parent is ever assigned.
//  * UCompositeDataTable::GetRowState() is WITH_EDITORONLY_DATA and carries no
//    ENGINE_API, and FindLoops()/UpdateCachedRowMap()/OnParentTablesUpdated() are
//    protected without ENGINE_API: none of them link from a plugin. Provenance
//    and cycle detection are therefore RE-COMPUTED from the public
//    GetParentTables()/GetRowMap(), never read out of the engine's own caches.
//  * UDataTable::EmptyTable() destroys existing rows with the CURRENT RowStruct
//    (DataTable.cpp GetEmptyUsingStruct), so re-pointing RowStruct on a
//    composite that still holds merged rows would free them with the wrong
//    layout. That is why changing the row struct of an EXISTING composite is
//    refused below instead of "handled".
// ========================================================================== //
namespace
{
    // Load a DataTable (plain or composite) by /Game path. UCompositeDataTable derives from
    // UDataTable, so resolving on the base class finds both; Cast<> narrows after. Goes through
    // mcp_common::ResolveObjectByPath (not a bare StaticLoadObject) BECAUSE a bogus parent path is
    // an expected caller mistake here, and StaticLoadObject leaves a phantom UPackage behind on a
    // miss -- which would then break the CreatePackage of the composite itself.
    UDataTable* MCPExt_LoadAnyDataTable(const FString& Path)
    {
        return Cast<UDataTable>(mcp_common::ResolveObjectByPath(Path, UDataTable::StaticClass()));
    }

    // Plugin-side re-implementation of UCompositeDataTable::FindLoops (protected in the
    // engine). Root may be nullptr for a composite that does not exist yet -- in that case only
    // pre-existing loops among the prospective parents are reachable, which is exactly what the
    // engine would find from a brand-new root. Returns the first table that depends on itself.
    const UCompositeDataTable* MCPExt_FindCompositeLoop(
        const UCompositeDataTable* Root,
        const TArray<UDataTable*>& RootParents,
        TArray<const UCompositeDataTable*> AlreadySeen)
    {
        if (Root)
        {
            AlreadySeen.Add(Root);
        }

        for (const UDataTable* Parent : RootParents)
        {
            // Only composites can close a loop; a plain DataTable terminates the chain.
            const UCompositeDataTable* CompositeParent = Cast<UCompositeDataTable>(Parent);
            if (!CompositeParent)
            {
                continue;
            }
            if (AlreadySeen.Contains(CompositeParent))
            {
                return CompositeParent;
            }
            if (const UCompositeDataTable* Found = MCPExt_FindCompositeLoop(
                    CompositeParent, CompositeParent->GetParentTables(), AlreadySeen))
            {
                return Found;
            }
        }

        return nullptr;
    }

    // One merged row name and every parent index that supplies it, ascending. The LAST index
    // is the winner (engine precedence: higher index overrides lower).
    struct FMCPExtCompositeRowSource
    {
        FName RowName;
        TArray<int32> ParentIndices;
    };

    // Rebuild, from the parents' own row maps, the same merge UpdateCachedRowMap performs.
    // Only parents whose RowStruct matches contribute (the engine skips the rest), so the caller
    // must pass the already-filtered list.
    void MCPExt_ComputeCompositeProvenance(
        const TArray<UDataTable*>& ContributingParents,
        TArray<FMCPExtCompositeRowSource>& OutSources)
    {
        OutSources.Reset();
        TMap<FName, int32> SlotByRow;

        for (int32 ParentIdx = 0; ParentIdx < ContributingParents.Num(); ++ParentIdx)
        {
            const UDataTable* Parent = ContributingParents[ParentIdx];
            if (!Parent)
            {
                continue;
            }
            for (const TPair<FName, uint8*>& RowPair : Parent->GetRowMap())
            {
                if (int32* ExistingSlot = SlotByRow.Find(RowPair.Key))
                {
                    OutSources[*ExistingSlot].ParentIndices.Add(ParentIdx);
                }
                else
                {
                    FMCPExtCompositeRowSource NewSource;
                    NewSource.RowName = RowPair.Key;
                    NewSource.ParentIndices.Add(ParentIdx);
                    SlotByRow.Add(RowPair.Key, OutSources.Add(MoveTemp(NewSource)));
                }
            }
        }
    }

    // Fill Out with the composite-only fields: parent_tables[], merged_row_count vs
    // sum_parent_row_counts, shadowed_rows[] (with winner), parent_row_struct_mismatch[] and,
    // behind bIncludeRowSources, the per-row source_table (one entry per merged row -- expensive).
    // OutMergedMatchesParents receives the real consistency check: does the live merged row map
    // hold exactly the distinct row names the parents can supply?
    void MCPExt_AddCompositeAnalysis(
        UCompositeDataTable* Composite,
        bool bIncludeRowSources,
        const TSharedPtr<FJsonObject>& Out,
        bool* OutMergedMatchesParents = nullptr)
    {
        if (!Composite || !Out.IsValid())
        {
            if (OutMergedMatchesParents)
            {
                *OutMergedMatchesParents = false;
            }
            return;
        }

        const TArray<UDataTable*>& Parents = Composite->GetParentTables();
        UScriptStruct* CompositeRowStruct = Composite->RowStruct;

        TArray<UDataTable*> Contributing;
        TArray<FString> ContributingPaths;
        TArray<TSharedPtr<FJsonValue>> ParentsArr;
        TArray<TSharedPtr<FJsonValue>> MismatchArr;
        int32 SumParentRows = 0;

        for (int32 Index = 0; Index < Parents.Num(); ++Index)
        {
            UDataTable* Parent = Parents[Index];

            TSharedPtr<FJsonObject> ParentObj = MakeShared<FJsonObject>();
            ParentObj->SetNumberField(TEXT("index"), Index);

            if (!Parent)
            {
                // A None entry in the array is real state, not an error -- report it instead of
                // quietly compacting the list (the indices are the precedence order).
                ParentObj->SetStringField(TEXT("path"), TEXT("None"));
                ParentObj->SetBoolField(TEXT("null_entry"), true);
                ParentObj->SetBoolField(TEXT("contributes"), false);
                ParentsArr.Add(MakeShared<FJsonValueObject>(ParentObj));
                continue;
            }

            const bool bRowStructMatches = (Parent->RowStruct == CompositeRowStruct);
            const int32 ParentRowCount = Parent->GetRowMap().Num();

            ParentObj->SetStringField(TEXT("path"), Parent->GetPathName());
            ParentObj->SetStringField(TEXT("name"), Parent->GetName());
            ParentObj->SetStringField(TEXT("table_class"), Parent->GetClass()->GetName());
            ParentObj->SetStringField(TEXT("row_struct"), Parent->RowStruct ? Parent->RowStruct->GetName() : TEXT("None"));
            ParentObj->SetNumberField(TEXT("row_count"), ParentRowCount);
            ParentObj->SetBoolField(TEXT("row_struct_matches"), bRowStructMatches);
            ParentObj->SetBoolField(TEXT("contributes"), bRowStructMatches);
            ParentObj->SetBoolField(TEXT("null_entry"), false);
            ParentsArr.Add(MakeShared<FJsonValueObject>(ParentObj));

            if (bRowStructMatches)
            {
                Contributing.Add(Parent);
                ContributingPaths.Add(Parent->GetPathName());
                SumParentRows += ParentRowCount;
            }
            else
            {
                MismatchArr.Add(UEAgentDataAssetCommandsImpl::RowStructMismatch(Index, Parent, CompositeRowStruct));
            }
        }

        TArray<FMCPExtCompositeRowSource> Sources;
        MCPExt_ComputeCompositeProvenance(Contributing, Sources);

        TArray<TSharedPtr<FJsonValue>> ShadowedArr;
        TArray<TSharedPtr<FJsonValue>> RowSourcesArr;
        for (const FMCPExtCompositeRowSource& Source : Sources)
        {
            const int32 WinnerSlot = Source.ParentIndices.Num() > 0 ? Source.ParentIndices.Last() : INDEX_NONE;
            const FString WinnerPath = ContributingPaths.IsValidIndex(WinnerSlot) ? ContributingPaths[WinnerSlot] : FString(TEXT("None"));

            if (bIncludeRowSources)
            {
                TSharedPtr<FJsonObject> RowSrcObj = MakeShared<FJsonObject>();
                RowSrcObj->SetStringField(TEXT("row_name"), Source.RowName.ToString());
                RowSrcObj->SetStringField(TEXT("source_table"), WinnerPath);
                RowSrcObj->SetNumberField(TEXT("source_parent_slot"), WinnerSlot);
                RowSrcObj->SetBoolField(TEXT("shadowed"), Source.ParentIndices.Num() > 1);
                RowSourcesArr.Add(MakeShared<FJsonValueObject>(RowSrcObj));
            }

            if (Source.ParentIndices.Num() <= 1)
            {
                continue;
            }

            TArray<TSharedPtr<FJsonValue>> LosersArr;
            for (int32 SlotIdx = 0; SlotIdx < Source.ParentIndices.Num() - 1; ++SlotIdx)
            {
                const int32 LoserSlot = Source.ParentIndices[SlotIdx];
                TSharedPtr<FJsonObject> LoserObj = MakeShared<FJsonObject>();
                LoserObj->SetNumberField(TEXT("parent_slot"), LoserSlot);
                LoserObj->SetStringField(TEXT("path"), ContributingPaths.IsValidIndex(LoserSlot) ? ContributingPaths[LoserSlot] : FString(TEXT("None")));
                LosersArr.Add(MakeShared<FJsonValueObject>(LoserObj));
            }

            TSharedPtr<FJsonObject> ShadowObj = MakeShared<FJsonObject>();
            ShadowObj->SetStringField(TEXT("row_name"), Source.RowName.ToString());
            ShadowObj->SetStringField(TEXT("winner"), WinnerPath);
            ShadowObj->SetNumberField(TEXT("winner_parent_slot"), WinnerSlot);
            ShadowObj->SetArrayField(TEXT("shadowed_by_winner"), LosersArr);
            ShadowedArr.Add(MakeShared<FJsonValueObject>(ShadowObj));
        }

        const int32 MergedRowCount = Composite->GetRowMap().Num();
        const int32 ExpectedMergedRowCount = Sources.Num();
        const bool bMergedMatches = (MergedRowCount == ExpectedMergedRowCount);

        Out->SetArrayField(TEXT("parent_tables"), ParentsArr);
        Out->SetNumberField(TEXT("parent_count"), Parents.Num());
        Out->SetNumberField(TEXT("contributing_parent_count"), Contributing.Num());
        Out->SetNumberField(TEXT("merged_row_count"), MergedRowCount);
        Out->SetNumberField(TEXT("sum_parent_row_counts"), SumParentRows);
        Out->SetNumberField(TEXT("expected_merged_row_count"), ExpectedMergedRowCount);
        Out->SetBoolField(TEXT("merged_row_count_matches_parents"), bMergedMatches);
        Out->SetArrayField(TEXT("parent_row_struct_mismatch"), MismatchArr);
        Out->SetArrayField(TEXT("shadowed_rows"), ShadowedArr);
        Out->SetNumberField(TEXT("shadowed_row_count"), ShadowedArr.Num());
        Out->SetBoolField(TEXT("row_sources_included"), bIncludeRowSources);
        if (bIncludeRowSources)
        {
            Out->SetArrayField(TEXT("row_sources"), RowSourcesArr);
        }

        if (OutMergedMatchesParents)
        {
            *OutMergedMatchesParents = bMergedMatches;
        }
    }
}

// MCP:create_data_table - Create a DataTable asset bound to a row struct -- the standard container for game data (items, recipes, loot, quests, dialogue) that designers edit without touching Blueprints. The row struct must already exist (create_struct) and cannot be changed afterwards without losing data, so settle its columns first. Idempotent: re-running with the same row_struct returns the existing table with already_existed instead of erroring. Rows go in with add_data_table_row or import_data_table_csv; create_composite_data_table is the variant that merges several parent tables.
//   @param table_name - Name of the new DataTable.
//   @param row_struct - Name of the UScriptStruct that defines the row structure.
//   @param package_path - Target content package path where the asset is created (check the body for the default)
//   @returns { table_name, package_path, row_struct, table_path, success, saved, already_existed, message }
TSharedPtr<FJsonObject> HandleCreateDataTable(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Falls back to /Game/DataTables when absent.
    FString Folder;
    if (!Params->TryGetStringField(TEXT("package_path"), Folder)) { Folder = TEXT("/Game/DataTables"); }
    FString StructName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_struct"), StructName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UScriptStruct* RowStruct = FindFirstObject<UScriptStruct>(*StructName);
    if (RowStruct == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Row struct not found: ") + StructName);
    }

    const FString TargetPath = Impl::JoinPackagePath(Folder, NewName);

    // Idempotent: a table already at this path with the SAME row struct is this very call re-run, so it
    // succeeds with already_existed=true. A different row struct is a real conflict and still fails.
    const bool bRegistered = UEditorAssetLibrary::DoesAssetExist(TargetPath);
    UDataTable* Existing = Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *TargetPath));
    if (bRegistered || Existing != nullptr)
    {
        if (Existing == nullptr)
        {
            return mcp_common::MakeErrorReply(
                TEXT("DataTable already exists but could not be loaded to verify its row struct: ") + TargetPath);
        }
        if (Existing->RowStruct != RowStruct)
        {
            const FString ExistingStruct = Existing->RowStruct ? Existing->RowStruct->GetName() : FString(TEXT("None"));
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("DataTable already exists with a different row struct: %s (existing: %s, requested: %s)"),
                *TargetPath, *ExistingStruct, *StructName));
        }

        TSharedPtr<FJsonObject> Unchanged = MakeShared<FJsonObject>();
        Unchanged->SetStringField(TEXT("table_name"), NewName);
        Unchanged->SetStringField(TEXT("package_path"), Folder);
        Unchanged->SetStringField(TEXT("row_struct"), StructName);
        Unchanged->SetStringField(TEXT("table_path"), Existing->GetPathName());
        Unchanged->SetBoolField(TEXT("already_existed"), true);
        Unchanged->SetBoolField(TEXT("saved"), true);
        Unchanged->SetBoolField(TEXT("success"), true);
        Unchanged->SetStringField(TEXT("message"), TEXT("DataTable already exists with matching row struct"));
        return mcp_common::MakeOkReply(Unchanged);
    }

    // A real package at the target path, so the table persists to disk.
    FString CreateError;
    UDataTable* Created = mcp_common::NewAssetInPackage<UDataTable>(
        TargetPath, NewName, TEXT("Failed to create DataTable"), CreateError, RF_Public | RF_Standalone);
    if (Created == nullptr)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }
    Created->RowStruct = RowStruct;
    FAssetRegistryModule::AssetCreated(Created);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Created->GetOutermost(), Created, SaveError);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_name"), NewName);
    Reply->SetStringField(TEXT("package_path"), Folder);
    Reply->SetStringField(TEXT("row_struct"), StructName);
    Reply->SetStringField(TEXT("table_path"), Created->GetPathName());
    Reply->SetBoolField(TEXT("already_existed"), false);
    Reply->SetBoolField(TEXT("saved"), bSaved);
    Reply->SetBoolField(TEXT("success"), bSaved);
    Reply->SetStringField(TEXT("message"), bSaved
        ? FString(TEXT("DataTable created and saved to disk"))
        : TEXT("DataTable created but save failed: ") + SaveError);

    // The save verdict measured above is the verdict emitted; a hardcoded success:true from
    // MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(Reply, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_data_table"), TEXT("bSaved")));
}

// MCP:create_composite_data_table - Create (or re-converge) a UCompositeDataTable whose row map is the merge of its parent DataTables, so a fork can EXTEND an upstream table instead of replacing it. The parent list is SET, not appended (re-running with a different list rewrites it), the row struct of every parent is validated BEFORE assignment (a mismatch or a cyclic parent chain would make the engine open a modal dialog and hang the editor), and the merge is verified by re-reading the live row map.
//   @param table_name - Name of the composite DataTable asset to create or converge.
//   @param package_path - Target content package path where the asset lives (check the body for the default).
//   @param parent_tables - Array of full asset paths of the parent DataTables, LOW precedence first: a row present in several parents is won by the one with the HIGHEST index.
//   @param row_struct - Optional UScriptStruct name for the composite rows. Defaults to the row struct of the first resolved parent (or, on an existing composite, to the one it already has -- which cannot be changed in place).
//   @param include_row_sources - Optional bool (default false): also return the winning parent for EVERY merged row. Expensive on large tables.
//   @returns { table_name, package_path, table_path, table_class, row_struct, created, already_existed, requested_parent_tables, missing_parents, parents_applied, saved, save_error, parent_tables, parent_count, contributing_parent_count, merged_row_count, sum_parent_row_counts, expected_merged_row_count, merged_row_count_matches_parents, parent_row_struct_mismatch, shadowed_rows, shadowed_row_count, row_sources_included, row_sources? }
TSharedPtr<FJsonObject> HandleCreateCompositeDataTable(const TSharedPtr<FJsonObject>& Params)
{
    FString TableName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_name"), TableName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // The composite lands under /Game/DataTables unless a folder is given.
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath)) { PackagePath = TEXT("/Game/DataTables"); }
    bool bIncludeRowSources = false;
    Params->TryGetBoolField(TEXT("include_row_sources"), bIncludeRowSources);
    const FString FullPath = UEAgentDataAssetCommandsImpl::JoinPackagePath(PackagePath, TableName);

    // ---------------------------------------------------------------------- //
    // STEP 1 -- resolve the requested parents. Nothing is assigned yet.
    // ---------------------------------------------------------------------- //
    const TArray<TSharedPtr<FJsonValue>>* ParentArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("parent_tables"), ParentArray) || !ParentArray)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: parent_tables (array of DataTable asset paths, low precedence first)"));
    }

    TArray<UDataTable*> DesiredParents;
    TArray<FString> RequestedParentPaths;
    TArray<FString> MissingParents;
    for (const TSharedPtr<FJsonValue>& Value : *ParentArray)
    {
        FString ParentPath;
        if (!Value.IsValid() || !Value->TryGetString(ParentPath) || ParentPath.IsEmpty())
        {
            continue;
        }
        RequestedParentPaths.Add(ParentPath);

        UDataTable* Parent = MCPExt_LoadAnyDataTable(ParentPath);
        if (!Parent)
        {
            MissingParents.Add(ParentPath);
            continue;
        }
        // AddUnique: the same table twice in the list is a no-op for the merge but would double
        // the parent array and confuse the precedence report.
        DesiredParents.AddUnique(Parent);
    }

    if (MissingParents.Num() > 0)
    {
        // Hard-fail instead of building a composite that silently misses rows: a partial merge
        // that still answers success:true is exactly the dishonesty this tool exists to avoid.
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("table_name"), TableName);
        FailObj->SetStringField(TEXT("package_path"), PackagePath);
        FailObj->SetArrayField(TEXT("missing_parents"), mcp_common::StringsToJson(MissingParents));
        FailObj->SetBoolField(TEXT("created"), false);
        FailObj->SetBoolField(TEXT("parents_applied"), false);
        return mcp_common::MakeOutcomeReply(FailObj, false, FString::Printf(
            TEXT("%d parent table(s) could not be loaded; nothing was created or modified"), MissingParents.Num()));
    }

    // ---------------------------------------------------------------------- //
    // Resolve the target asset (it may already exist) BEFORE deciding the row
    // struct, because an existing composite's row struct wins.
    // ---------------------------------------------------------------------- //
    UCompositeDataTable* Composite = nullptr;
    bool bAlreadyExists = false;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UDataTable* ExistingAny = MCPExt_LoadAnyDataTable(FullPath);
        Composite = Cast<UCompositeDataTable>(ExistingAny);
        if (!Composite)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Asset already exists at %s but is not a UCompositeDataTable (class: %s) -- refusing to replace it"),
                *FullPath,
                ExistingAny ? *ExistingAny->GetClass()->GetName() : TEXT("could not be loaded")));
        }
        bAlreadyExists = true;
    }

    // Row struct precedence: explicit param > existing composite's > first resolved parent's.
    FString RequestedRowStructName;
    UScriptStruct* RequestedRowStruct = nullptr;
    if (Params->TryGetStringField(TEXT("row_struct"), RequestedRowStructName) && !RequestedRowStructName.IsEmpty())
    {
        RequestedRowStruct = FindFirstObject<UScriptStruct>(*RequestedRowStructName);
        if (!RequestedRowStruct)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Row struct not found: %s"), *RequestedRowStructName));
        }
    }

    UScriptStruct* ExistingRowStruct = nullptr;
    if (bAlreadyExists)
    {
        ExistingRowStruct = Composite->RowStruct;
    }
    UScriptStruct* RowStruct = RequestedRowStruct;
    if (!RowStruct)
    {
        RowStruct = ExistingRowStruct;
    }
    if (!RowStruct && DesiredParents.Num() > 0)
    {
        RowStruct = DesiredParents[0]->RowStruct;
    }
    if (!RowStruct)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Could not determine a row struct: pass row_struct explicitly, or give at least one parent table that has one"));
    }

    // Re-pointing the row struct of a composite that already holds merged rows would make
    // UDataTable::EmptyTable() free those rows with the WRONG layout (it destroys with the
    // current RowStruct). Refuse honestly rather than "handle" it.
    if (bAlreadyExists && ExistingRowStruct && ExistingRowStruct != RowStruct)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Composite %s already uses row struct '%s'; changing it in place is unsafe (rows would be destroyed with the wrong layout). Delete the asset and recreate it to move to '%s'."),
            *FullPath, *ExistingRowStruct->GetName(), *RowStruct->GetName()));
    }
    if (bAlreadyExists && !ExistingRowStruct && Composite->GetRowMap().Num() > 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Composite %s has no row struct but still holds %d rows -- refusing to touch it (the engine would free them with an undefined layout)"),
            *FullPath, Composite->GetRowMap().Num()));
    }

    // ---------------------------------------------------------------------- //
    // STEP 2 -- validate BEFORE assigning. Both checks below are exactly the two
    // conditions that make UpdateCachedRowMap() pop an FMessageDialog, which
    // would block the game thread and kill the caller's MCP session.
    // ---------------------------------------------------------------------- //
    TArray<TSharedPtr<FJsonValue>> PreflightMismatchArr;
    for (int32 Index = 0; Index < DesiredParents.Num(); ++Index)
    {
        UDataTable* Parent = DesiredParents[Index];
        if (Parent->RowStruct == RowStruct)
        {
            continue;
        }
        PreflightMismatchArr.Add(UEAgentDataAssetCommandsImpl::RowStructMismatch(Index, Parent, RowStruct));
    }
    if (PreflightMismatchArr.Num() > 0)
    {
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("table_name"), TableName);
        FailObj->SetStringField(TEXT("package_path"), PackagePath);
        FailObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
        FailObj->SetArrayField(TEXT("parent_row_struct_mismatch"), PreflightMismatchArr);
        FailObj->SetBoolField(TEXT("created"), false);
        FailObj->SetBoolField(TEXT("already_existed"), bAlreadyExists);
        FailObj->SetBoolField(TEXT("parents_applied"), false);
        return mcp_common::MakeOutcomeReply(FailObj, false, FString::Printf(
            TEXT("%d parent table(s) do not use row struct '%s'; a composite must share the row struct of every parent. Nothing was created or modified."),
            PreflightMismatchArr.Num(), *RowStruct->GetName()));
    }

    for (UDataTable* Parent : DesiredParents)
    {
        if (Parent == Composite)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Cyclic parent chain: %s cannot be its own parent"), *FullPath));
        }
    }

    if (const UCompositeDataTable* LoopTable = MCPExt_FindCompositeLoop(
            Composite, DesiredParents, TArray<const UCompositeDataTable*>()))
    {
        TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
        FailObj->SetStringField(TEXT("table_name"), TableName);
        FailObj->SetStringField(TEXT("package_path"), PackagePath);
        FailObj->SetBoolField(TEXT("cycle_detected"), true);
        FailObj->SetStringField(TEXT("cycle_table"), LoopTable->GetPathName());
        FailObj->SetBoolField(TEXT("created"), false);
        FailObj->SetBoolField(TEXT("already_existed"), bAlreadyExists);
        FailObj->SetBoolField(TEXT("parents_applied"), false);
        return mcp_common::MakeOutcomeReply(FailObj, false, FString::Printf(
            TEXT("Cyclic parent chain through composite %s; nothing was created or modified"), *LoopTable->GetPathName()));
    }

    // ---------------------------------------------------------------------- //
    // STEP 3 -- create the asset (only once the inputs are known-safe).
    // ---------------------------------------------------------------------- //
    if (!Composite)
    {
        FString CreateError;
        Composite = mcp_common::NewAssetInPackage<UCompositeDataTable>(FullPath, TableName, TEXT("Failed to create UCompositeDataTable"), CreateError);
        if (!Composite)
        {
            return mcp_common::MakeErrorReply(CreateError);
        }

        FAssetRegistryModule::AssetCreated(Composite);
    }
    UPackage* Package = Composite->GetOutermost();

    Composite->Modify();

    // ---------------------------------------------------------------------- //
    // STEP 4 -- row struct FIRST. UpdateCachedRowMap() compares every parent
    // against Composite->RowStruct; leaving it null here would make every parent
    // "mismatch" and open the modal.
    // ---------------------------------------------------------------------- //
    Composite->RowStruct = RowStruct;

    // ---------------------------------------------------------------------- //
    // STEP 5 -- converge the parent list: SET semantics, not append. The engine's
    // AppendParentTables() appends raw (no dedupe), so an idempotent re-run would
    // otherwise grow the array forever -- and it is the ONLY public entry point
    // that drives OnParentTablesUpdated()->UpdateCachedRowMap(), i.e. the merge a
    // hand-written execute_python assignment to ParentTables never performs.
    // Removing everything first (rather than diffing) is what guarantees the
    // resulting ORDER matches the requested precedence, and it is also the safe
    // order: with an empty parent array neither the loop check nor the row-struct
    // check inside UpdateCachedRowMap can fire.
    // ---------------------------------------------------------------------- //
    TArray<UDataTable*> CurrentParents = Composite->GetParentTables(); // copy: RemoveParentTables mutates the live array
    const bool bParentListDiffers = (CurrentParents != DesiredParents);
    if (bParentListDiffers)
    {
        if (CurrentParents.Num() > 0)
        {
            Composite->RemoveParentTables(CurrentParents);
        }
        if (DesiredParents.Num() > 0)
        {
            Composite->AppendParentTables(DesiredParents);
        }
    }

    Composite->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, Composite, SaveError);

    // ---------------------------------------------------------------------- //
    // Verification: re-read the LIVE parent array and the LIVE merged row map.
    // ---------------------------------------------------------------------- //
    const TArray<UDataTable*>& AppliedParents = Composite->GetParentTables();
    bool bParentsApplied = (AppliedParents.Num() == DesiredParents.Num());
    if (bParentsApplied)
    {
        for (int32 Index = 0; Index < DesiredParents.Num(); ++Index)
        {
            if (AppliedParents[Index] != DesiredParents[Index])
            {
                bParentsApplied = false;
                break;
            }
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_name"), TableName);
    ResultObj->SetStringField(TEXT("package_path"), PackagePath);
    ResultObj->SetStringField(TEXT("table_path"), Composite->GetPathName());
    ResultObj->SetStringField(TEXT("table_class"), Composite->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("row_struct"), Composite->RowStruct ? Composite->RowStruct->GetName() : TEXT("None"));
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExists);
    ResultObj->SetBoolField(TEXT("parent_list_rewritten"), bParentListDiffers);
    ResultObj->SetBoolField(TEXT("parents_applied"), bParentsApplied);
    mcp_common::AddSaveOutcome(ResultObj, bSaved, SaveError);

    ResultObj->SetArrayField(TEXT("requested_parent_tables"), mcp_common::StringsToJson(RequestedParentPaths));
    ResultObj->SetArrayField(TEXT("missing_parents"), TArray<TSharedPtr<FJsonValue>>());

    bool bMergedMatchesParents = false;
    MCPExt_AddCompositeAnalysis(Composite, bIncludeRowSources, ResultObj, &bMergedMatchesParents);

    FString OutcomeError;
    if (!bParentsApplied)
    {
        OutcomeError = TEXT("The live ParentTables array does not match the requested list after the update");
    }
    else if (!bMergedMatchesParents)
    {
        OutcomeError = TEXT("The merged row map does not hold the distinct row names the parents supply -- the cached row map did not converge");
    }
    else if (!bSaved)
    {
        OutcomeError = FString::Printf(TEXT("Composite converged in memory but the package did not save: %s"), *SaveError);
    }

    return mcp_common::MakeOutcomeReply(ResultObj, OutcomeError.IsEmpty(), OutcomeError);
}

// MCP:get_data_table_row - Read ONE row of a DataTable by key and get every column as JSON . Reads the row memory untyped (FindRowUnchecked), so it works on Blueprint / user-defined row structs that are not FTableRowBase children, where the typed lookup reports "Row not found" for rows that exist; a miss here really is a missing key. Serializes TMap, nested struct and GameplayTag columns. When you need the byte-exact ExportText of a column (to copy a vendor row, or to tell an empty enum from an unwritten one) use read_data_table_row_complex instead, and get_data_table_rows when you want the enum DISPLAY names.
//   @param table_path - The full asset path to the DataTable.
//   @param row_name - The name of the row to retrieve.
//   @returns { table_path, row_name, row_data } where row_data is the fully serialized row struct (TMap / nested struct / FGameplayTag columns included)
TSharedPtr<FJsonObject> HandleGetDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedTable;
    FString RequestedRow;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTableAndRow(Params, RequestedTable, RequestedRow))
    {
        return Missing;
    }

    // The row memory is looked up untyped (FindRowUnchecked, as add/set/delete_data_table_row do).
    // The typed FindRow<FTableRowBase> goes through FindRowInternal, which returns nullptr for ANY row struct
    // that is not a child of FTableRowBase BEFORE it even looks at the row map -- so every row of a table
    // keyed by a Blueprint / user-defined struct would read as missing although get_data_table_info and
    // get_data_table_rows list it. With no type gate, a null here can only be a key that is not there.
    UDataTable* Table = nullptr;
    const uint8* Row = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::OpenRow(RequestedTable, RequestedRow, Table, Row))
    {
        return Failure;
    }

    // TMap, nested struct and GameplayTag(Container) columns all go through the shared typed exporter.
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_path"), RequestedTable);
    Reply->SetStringField(TEXT("row_name"), RequestedRow);
    Reply->SetObjectField(TEXT("row_data"), Impl::RowColumnsToJson(Table->RowStruct, Row));
    return mcp_common::MakeOkReply(Reply);
}

// MCP:get_data_table_rows - Dump EVERY row of a DataTable as an array, with the row count -- for browsing or diffing a whole table of items, recipes, quests or loot. The reader to pick when a column is a user-defined ENUM: it emits the enumerator's DISPLAY NAME, whereas the ExportText readers return "" for the index-0 enumerator, which is indistinguishable from a column that was never written. Whole-table sized, so for one row use get_data_table_row and for one column across rows use get_data_table_column.
//   @param table_path - The full asset path to the DataTable.
//   @returns { table_path, row_name, rows, row_count }
TSharedPtr<FJsonObject> HandleGetDataTableRows(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedTable;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), RequestedTable, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    TSharedPtr<FJsonObject> Refusal;
    UDataTable* Table = Impl::OpenTable(RequestedTable, /*bNeedRowStruct*/ true, Refusal);
    if (Table == nullptr)
    {
        return Refusal;
    }

    // Row map order, every row as { row_name, row_data } with typed columns.
    const TMap<FName, uint8*>& RowMap = Table->GetRowMap();
    TArray<TSharedPtr<FJsonValue>> AllRows;
    AllRows.Reserve(RowMap.Num());
    for (const TPair<FName, uint8*>& Entry : RowMap)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("row_name"), Entry.Key.ToString());
        Row->SetObjectField(TEXT("row_data"), Impl::RowColumnsToJson(Table->RowStruct, Entry.Value));
        AllRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_path"), RequestedTable);
    Reply->SetNumberField(TEXT("row_count"), AllRows.Num());
    Reply->SetArrayField(TEXT("rows"), AllRows);
    return mcp_common::MakeOkReply(Reply);
}

// MCP:get_data_table_info - Retrieves detailed information about a DataTable, including all row names. When the asset is a UCompositeDataTable (see table_class) the reported row_count is the ALREADY-MERGED map, so the response additionally explains where those rows come from: parent_tables[] in precedence order, merged_row_count vs sum_parent_row_counts, shadowed_rows[] with the winning parent, and parent_row_struct_mismatch[] for parents the engine silently skips.
//   @param table_path - The full asset path to the DataTable.
//   @param include_row_sources - Composite tables only. Optional bool (default false): also return the winning parent for EVERY merged row. Expensive on large tables.
//   @returns { table_path, table_name, table_class, row_struct, row_count, row_names, columns:[{ name, type, is_map, is_soft, is_tag }] } plus, for a UCompositeDataTable: { parent_tables:[{ index, path, name, table_class, row_struct, row_count, row_struct_matches, contributes, null_entry }], parent_count, contributing_parent_count, merged_row_count, sum_parent_row_counts, expected_merged_row_count, merged_row_count_matches_parents, parent_row_struct_mismatch:[], shadowed_rows:[{ row_name, winner, winner_parent_slot, shadowed_by_winner:[] }], shadowed_row_count, row_sources_included, row_sources? }
TSharedPtr<FJsonObject> HandleGetDataTableInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedTable;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), RequestedTable, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // A table without a row struct is still described here (row_struct reads "None").
    TSharedPtr<FJsonObject> Refusal;
    UDataTable* Table = Impl::OpenTable(RequestedTable, /*bNeedRowStruct*/ false, Refusal);
    if (Table == nullptr)
    {
        return Refusal;
    }

    const UScriptStruct* RowStruct = Table->RowStruct;
    const TMap<FName, uint8*>& RowMap = Table->GetRowMap();

    TArray<TSharedPtr<FJsonValue>> Keys;
    Keys.Reserve(RowMap.Num());
    for (const TPair<FName, uint8*>& Entry : RowMap)
    {
        Keys.Add(MakeShared<FJsonValueString>(Entry.Key.ToString()));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_path"), RequestedTable);
    Reply->SetStringField(TEXT("table_name"), Table->GetName());
    Reply->SetStringField(TEXT("table_class"), Table->GetClass()->GetName());
    Reply->SetStringField(TEXT("row_struct"), RowStruct ? RowStruct->GetName() : FString(TEXT("None")));
    Reply->SetNumberField(TEXT("row_count"), RowMap.Num());
    Reply->SetArrayField(TEXT("row_names"), Keys);
    Reply->SetArrayField(TEXT("columns"), Impl::DescribeColumns(RowStruct));

    // Composite-aware tail, additive: for a plain table every field above is all there is. A
    // UCompositeDataTable's row map is ALREADY the merge of its parents, so row_count says "how many" but
    // never "from where", and a row one parent shadows in another stays invisible. GetRowState() would tell,
    // but it is WITH_EDITORONLY_DATA without ENGINE_API and does not link from a plugin, so the provenance is
    // re-computed from the public GetParentTables()/GetRowMap() with the engine's own precedence rule
    // (CompositeDataTable.h: a higher parent index overrides a lower one). No is_composite flag on purpose:
    // table_class already says CompositeDataTable.
    if (UCompositeDataTable* Composite = Cast<UCompositeDataTable>(Table))
    {
        bool bIncludeRowSources = false;
        Params->TryGetBoolField(TEXT("include_row_sources"), bIncludeRowSources);
        MCPExt_AddCompositeAnalysis(Composite, bIncludeRowSources, Reply);
    }

    return mcp_common::MakeOkReply(Reply);
}

// ========================================================================== //
// DataTable row authoring + Curve asset authoring
// ========================================================================== //

namespace
{
    // Load a UDataTable from a /Game path. Returns nullptr on failure.
    UDataTable* LoadDataTableAt(const FString& TablePath)
    {
        return Cast<UDataTable>(StaticLoadObject(UDataTable::StaticClass(), nullptr, *TablePath));
    }

    // Serialize one row instance (RowStruct-typed memory) into a JSON object,
    // exporting every column via reflection (matches HandleGetDataTableRow).
    void RowToJson(const UScriptStruct* RowStruct, const void* RowData, TSharedPtr<FJsonObject>& OutObj)
    {
        for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property)
            {
                continue;
            }
            const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RowData);
            OutObj->SetField(Property->GetName(), mcp_common::PropertyValueToJson(Property, ValuePtr));
        }
    }

    // Resolve a curve asset by /Game path. The exact UCurve* subclass is detected
    // by the caller via Cast<>. Returns the base UObject (or nullptr).
    UObject* LoadCurveObjectAt(const FString& CurvePath)
    {
        // UCurveBase is the shared base; resolve generically then Cast<> downstream.
        return mcp_common::ResolveObjectByPath(CurvePath);
    }

    // Map a string interp mode -> ERichCurveInterpMode. Defaults to Linear.
    ERichCurveInterpMode ParseInterpMode(const FString& In)
    {
        const FString Low = In.ToLower();
        if (Low == TEXT("constant")) { return RCIM_Constant; }
        if (Low == TEXT("cubic"))    { return RCIM_Cubic; }
        if (Low == TEXT("linear"))   { return RCIM_Linear; }
        return RCIM_Linear;
    }

    const TCHAR* InterpModeToString(ERichCurveInterpMode Mode)
    {
        switch (Mode)
        {
            case RCIM_Constant: return TEXT("Constant");
            case RCIM_Cubic:    return TEXT("Cubic");
            case RCIM_Linear:   return TEXT("Linear");
            default:            return TEXT("None");
        }
    }
}

// Shared implementation for add_data_table_row / set_data_table_row. Both are an
// upsert via UDataTable::AddRow(FName, const uint8*, const UScriptStruct*) which
// internally removes any existing row and copies in the new struct memory.
// We build the row in an FStructOnScope (allocated + initialized for RowStruct),
// import each provided column via reflection (FProperty::ImportText_InContainer),
// then copy it in. We then read the row BACK to confirm persistence and return it.
static TSharedPtr<FJsonObject> WriteDataTableRow(const TSharedPtr<FJsonObject>& Params, bool bRequireExisting)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString TablePath;
    FString RowName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTableAndRow(Params, TablePath, RowName))
    {
        return Missing;
    }
    // row_data must be a JSON object of { column: value }.
    const TSharedPtr<FJsonObject>* RowDataObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("row_data"), RowDataObj) || !RowDataObj || !(*RowDataObj).IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: row_data (object of {column:value})"));
    }

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = Impl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }

    const FName RowFName(*RowName);
    const bool bExisted = (DataTable->FindRowUnchecked(RowFName) != nullptr);
    if (bRequireExisting && !bExisted)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Row not found (set requires existing row): %s"), *RowName));
    }

    // The exact struct AddRow expects: must equal &GetEmptyUsingStruct() (==RowStruct).
    // GetEmptyUsingStruct() is protected; the public RowStruct member is the same
    // pointer when set (verified non-null above), so AddRow's checkf still holds.
    UScriptStruct* RowStruct = DataTable->RowStruct;

    // Allocate + default-initialize a temporary row instance for RowStruct; when updating an
    // existing row it is seeded from the current row, so columns not present in row_data are preserved.
    FStructOnScope RowScope(RowStruct);
    uint8* RowMemory = Impl::SeedWithLiveRow(RowScope, DataTable, RowFName, bExisted);
    if (!RowMemory)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate temporary row memory"));
    }

    // Import each provided column value into the temp row via reflection.
    TArray<FString> UnknownColumns;
    TArray<FString> FailedColumns;
    int32 ColumnsWritten = 0;
    for (const auto& Pair : (*RowDataObj)->Values)
    {
        const FString ColumnName = *Pair.Key;
        FProperty* Property = Impl::FindColumn(DataTable, ColumnName);
        if (!Property)
        {
            UnknownColumns.Add(ColumnName);
            continue;
        }

        // Write the JSON value into the property via reflection. JsonValueToUProperty
        // handles scalars (number/string/bool/enum-by-name) AND object->struct (FVector
        // etc.) AND array->TArray uniformly, so scalar, nested struct and array columns
        // all work. Use the VALUE ptr (offset already applied
        // here), since JsonValueToUProperty does NOT resolve the container offset itself.
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RowMemory);
        FText FailReason;
        const bool bOk = FJsonObjectConverter::JsonValueToUProperty(
            Pair.Value, Property, ValuePtr, /*CheckFlags*/ 0, /*SkipFlags*/ 0, /*bStrictMode*/ false, &FailReason);
        if (bOk)
        {
            ++ColumnsWritten;
        }
        else
        {
            const FString Reason = FailReason.IsEmpty() ? FString() : (TEXT(": ") + FailReason.ToString());
            FailedColumns.Add(ColumnName + Reason);
        }
    }

    // Honest gate: a column that fails to import would otherwise persist
    // silently with a default/stale value (e.g. an unrecognized GameplayTag collapsing
    // to NAME_None) -- fail the whole write instead of committing partial data.
    if (FailedColumns.Num() > 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%d column(s) failed to import, row not written: %s"),
            FailedColumns.Num(), *FString::Join(FailedColumns, TEXT("; "))));
    }

    // Upsert the row (AddRow removes the existing row first, then copies in).
    Impl::AddRowWithEditorNotify(DataTable, RowFName, RowMemory, RowStruct);
    DataTable->MarkPackageDirty();

    // Persist to disk so subsequent loads (get_data_table_row) see the change.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    // Read the row back to confirm it persisted (verification-by-readback).
    const uint8* Written = DataTable->FindRowUnchecked(RowFName);
    const bool bRowExists = (Written != nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_name"), RowName);
    ResultObj->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetName());
    ResultObj->SetBoolField(TEXT("success"), bRowExists);
    ResultObj->SetBoolField(TEXT("updated_existing"), bExisted);
    ResultObj->SetNumberField(TEXT("columns_written"), ColumnsWritten);
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());
    ResultObj->SetBoolField(TEXT("saved"), bSaved);

    if (UnknownColumns.Num() > 0)
    {
        ResultObj->SetArrayField(TEXT("unknown_columns"), mcp_common::StringsToJson(UnknownColumns));
    }

    // Echo the persisted row contents.
    if (bRowExists)
    {
        TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
        RowToJson(DataTable->RowStruct, Written, RowJson);
        ResultObj->SetObjectField(TEXT("row_data"), RowJson);
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:add_data_table_row - Create a new row in a DataTable (upsert -- it overwrites if the key exists), passing columns as a plain JSON object. Values go through reflection, so scalars, enums by name, nested structs and arrays all work. It fails the WHOLE write when any column cannot be imported, rather than committing a half-filled row with silent defaults, and lists anything it did not recognise in unknown_columns -- read that array, a typo'd column name is dropped, not an error. GameplayTag and GameplayTagContainer columns are the ones that reject a bare string: pass them in ExportText form, e.g. (TagName="A.B"). Use set_data_table_row to require the row to exist, or set_data_table_row_fields for a partial per-column update with before/after evidence.
//   @param table_path - Full asset path of the DataTable (e.g. /Game/MyGame/DT_Example)
//   @param row_name - Name (key) of the row to write
//   @param row_data - Object of {column_name: value}; values may be string/number/bool
//   @returns success or error payload
TSharedPtr<FJsonObject> HandleAddDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
    return WriteDataTableRow(Params, /*bRequireExisting*/ false);
}

// MCP:set_data_table_row - Update an EXISTING DataTable row, erroring if the key is absent -- that guard is the only difference from add_data_table_row, and it is what stops a typo'd row name from quietly creating a second row. Columns you omit keep their current values; the write fails as a whole if any provided column cannot be imported, and unrecognised names come back in unknown_columns. Pass GameplayTag columns in ExportText form, e.g. (TagName="A.B"). For per-column before/after evidence use set_data_table_row_fields; for one cell use set_data_table_cell.
//   @param table_path - Full asset path of the DataTable
//   @param row_name - Name (key) of the existing row to update
//   @param row_data - Object of {column_name: value} to apply over the current row
//   @returns success or error payload
TSharedPtr<FJsonObject> HandleSetDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
    return WriteDataTableRow(Params, /*bRequireExisting*/ true);
}

// MCP:delete_data_table_row - Remove one row from a DataTable by key, saving the asset and VERIFYING the removal by re-reading (it reports existed, removed, still_exists and the new row_count, so the reply is its own oracle rather than a bare success). Check existed to tell "deleted it" from "there was never such a key". Anything still referencing that row name by handle now resolves to nothing, silently -- audit with find_blueprint_node_references before deleting a live row.
//   @param table_path - Full asset path of the DataTable
//   @param row_name - Name (key) of the row to remove
//   @returns { table_path, row_name, existed, removed, still_exists, row_count, saved }
TSharedPtr<FJsonObject> HandleDeleteDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedTable;
    FString RequestedRow;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTableAndRow(Params, RequestedTable, RequestedRow))
    {
        return Missing;
    }
    TSharedPtr<FJsonObject> Refusal;
    UDataTable* Table = Impl::OpenTable(RequestedTable, /*bNeedRowStruct*/ false, Refusal);
    if (Table == nullptr)
    {
        return Refusal;
    }

    const FName RowKey(*RequestedRow);
    const bool bExisted = Table->FindRowUnchecked(RowKey) != nullptr;

    // A key that is not there is answered (existed=false) without touching the asset; nothing to save then.
    bool bSaved = true;
    if (bExisted)
    {
        // FDataTableEditorUtils::RemoveRow opens its own FScopedTransaction, calls Modify(), frees the row
        // memory and broadcasts the editor pre/post change itself.
        FDataTableEditorUtils::RemoveRow(Table, RowKey);
        Table->MarkPackageDirty();
        FString SaveError;
        bSaved = mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, SaveError);
    }

    // Verification by readback: the key must be gone now.
    const bool bStillThere = Table->FindRowUnchecked(RowKey) != nullptr;

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_path"), RequestedTable);
    Reply->SetStringField(TEXT("row_name"), RequestedRow);
    Reply->SetBoolField(TEXT("existed"), bExisted);
    Reply->SetBoolField(TEXT("still_exists"), bStillThere);
    Reply->SetBoolField(TEXT("removed"), bExisted && !bStillThere);
    Reply->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());

    return mcp_common::MakeSavedReply(Reply, bSaved, TEXT("delete_data_table_row"));
}

// MCP:import_data_table_csv - Bulk-load a DataTable from a CSV string -- the fast path for authoring or restoring many rows at once. DESTRUCTIVE: it REPLACES the table's rows, so any row not in the CSV is gone; export first (export_data_table_as_csv) if you meant to merge. Always read the problems array and problem_count: the native importer reports per-cell failures there while still succeeding overall, and enum values whose display name contains spaces are cut at the first space and silently land on the index-0 enumerator. For a handful of rows prefer set_data_table_rows_from_json, which reports per-row results.
//   @param table_path - Full asset path of the DataTable
//   @param csv_string - CSV content (header row + one row per data row)
//   @returns { table_path, row_struct, rows_imported, row_count, problem_count, saved, problems, row_names }
TSharedPtr<FJsonObject> HandleImportDataTableCSV(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    FString CSVString;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("csv_string"), CSVString, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = UEAgentDataAssetCommandsImpl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }

    DataTable->Modify();
    FDataTableEditorUtils::BroadcastPreChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);

    // CreateTableFromCSVString empties the table (unless bPreserveExistingValues)
    // and parses the CSV against RowStruct. Returns a list of problems.
    const TArray<FString> Problems = DataTable->CreateTableFromCSVString(CSVString);

    FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowList);
    DataTable->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    // Verify by readback: the number of rows now present.
    const int32 RowsImported = DataTable->GetRowMap().Num();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetName());
    ResultObj->SetNumberField(TEXT("rows_imported"), RowsImported);
    ResultObj->SetNumberField(TEXT("row_count"), RowsImported);
    ResultObj->SetNumberField(TEXT("problem_count"), Problems.Num());
    ResultObj->SetArrayField(TEXT("problems"), mcp_common::StringsToJson(Problems));

    TArray<TSharedPtr<FJsonValue>> RowNamesArr;
    for (const auto& RowPair : DataTable->GetRowMap())
    {
        RowNamesArr.Add(MakeShared<FJsonValueString>(RowPair.Key.ToString()));
    }
    ResultObj->SetArrayField(TEXT("row_names"), RowNamesArr);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("import_data_table_csv"), SaveError);
}

// ========================================================================== //
// Per-row native-JSON round-trip
// get/set_data_table_row use REFLECTION field-by-field. That path cannot round-trip a
// TMap<Enum, SoftClass> column (the exported wrapper form does not re-import; the value
// silently collapses to () while the write still "succeeds"). get_data_table_row_as_json and
// set_data_table_row_from_json use UE's OWN DataTable JSON exporter/importer (WriteRowAsJSON /
// CreateTableFromJSONString) so nested structs / FGameplayTag(Container) / soft refs / TMap
// columns round-trip exactly.
// ========================================================================== //

namespace
{
    // Serialize one row via UE's native DataTable JSON writer (the exact <TCHAR,TPrettyJsonPrintPolicy>
    // instantiation is the ENGINE_API-exported one). WriteRowAsJSON only emits the key/value pairs, so
    // the object braces go around it here. Returns the "{col:value,...}" object string.
    FString MCPExt_WriteRowAsNativeJsonString(UDataTable* Table, const void* RowData,
        EDataTableExportFlags Flags = EDataTableExportFlags::None)
    {
        FString RowJsonStr;
        TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
            TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&RowJsonStr);
        Writer->WriteObjectStart();
        Table->WriteRowAsJSON(Writer, RowData, Flags);
        Writer->WriteObjectEnd();
        Writer->Close();
        return RowJsonStr;
    }

    // Parse a JSON object string; returns an empty object (never null) on parse failure.
    TSharedPtr<FJsonObject> MCPExt_ParseJsonObject(const FString& JsonStr)
    {
        TSharedPtr<FJsonObject> Obj;
        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid())
        {
            Obj = MakeShared<FJsonObject>();
        }
        return Obj;
    }
}

// MCP:get_data_table_row_as_json - Export ONE DataTable row through UE's own DataTable JSON writer, returning both the verbatim native JSON string and a parsed object -- the exact format set_data_table_row_from_json consumes, so it is the read half of a copy/duplicate/round-trip. Use it over get_data_table_row when the value has to go back INTO the engine (it emits GameplayTagContainers in the ExportText form the importer accepts). The round trip is not lossless for multi-word user-defined enums, so re-read what you wrote.
//   @param table_path - Full asset path of the DataTable
//   @param row_name - Name (key) of the row
//   @returns { table_path, row_name, row_struct, row_json (verbatim native JSON string), row_data (parsed) }
TSharedPtr<FJsonObject> HandleGetDataTableRowAsJson(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedTable;
    FString RequestedRow;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTableAndRow(Params, RequestedTable, RequestedRow))
    {
        return Missing;
    }
    UDataTable* Table = nullptr;
    const uint8* Row = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::OpenRow(RequestedTable, RequestedRow, Table, Row))
    {
        return Failure;
    }

    const FString NativeJson = MCPExt_WriteRowAsNativeJsonString(Table, Row);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_path"), RequestedTable);
    Reply->SetStringField(TEXT("row_name"), RequestedRow);
    Reply->SetStringField(TEXT("row_struct"), Table->RowStruct->GetName());
    // Verbatim native JSON: this string goes straight back into set_data_table_row_from_json.
    Reply->SetStringField(TEXT("row_json"), NativeJson);
    // The same row as a parsed, typed object, for convenience.
    Reply->SetObjectField(TEXT("row_data"), Impl::RowColumnsToJson(Table->RowStruct, Row));
    return mcp_common::MakeOkReply(Reply);
}

// MCP:set_data_table_row_from_json - Write a whole DataTable row from a native-format JSON object (the shape get_data_table_row_as_json emits), with merge=true to overwrite only the columns you supply. The route for copying a row between tables or authoring a complex row in one call. ALWAYS read the problems array: success:true routinely coexists with columns that never landed -- a GameplayTagContainer passed as a JSON array is rejected and left empty (give it as the string "(GameplayTags=((TagName=\"A.B\")))"), a sub-field whose internal name differs from the display name you used stays at its struct default, and a user-defined enum whose display name has spaces is cut at the first space and lands on the index-0 enumerator (write the internal NewEnumeratorN name instead). Confirm with read_data_table_row_complex.
//   @param table_path - Full asset path of the DataTable
//   @param row_name - Name (key) of the row to write
//   @param row_json - A JSON object of {column:value} (as produced by get_data_table_row_as_json)
//   @param merge - When true and the row exists, only the caller-provided columns are overwritten
//   @returns success or error payload
TSharedPtr<FJsonObject> HandleSetDataTableRowFromJson(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString TablePath;
    FString RowName;
    if (TSharedPtr<FJsonObject> Missing = Impl::ReadTableAndRow(Params, TablePath, RowName))
    {
        return Missing;
    }
    // row_json may arrive as a JSON OBJECT (the tool schema's declared type, and the natural
    // form when a caller parses get_data_table_row_as_json's row_json string) OR as a JSON STRING (that
    // verbatim native JSON text). Accept EITHER: a string-only read (TryGetStringField) fails object
    // payloads with "Missing required parameter: row_json".
    FString RowJsonStr;
    TSharedPtr<FJsonObject> RowJsonObj;
    const TSharedPtr<FJsonObject>* RowJsonObjPtr = nullptr;
    if (Params->TryGetObjectField(TEXT("row_json"), RowJsonObjPtr) && RowJsonObjPtr && RowJsonObjPtr->IsValid())
    {
        RowJsonObj = *RowJsonObjPtr;
    }
    else if (Params->TryGetStringField(TEXT("row_json"), RowJsonStr))
    {
        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(RowJsonStr);
        if (!FJsonSerializer::Deserialize(Reader, RowJsonObj) || !RowJsonObj.IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("row_json string is not valid JSON"));
        }
    }
    else
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: row_json (a JSON object of {column:value}, or that JSON as a string)"));
    }

    bool bMerge = false;
    Params->TryGetBoolField(TEXT("merge"), bMerge);

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = Impl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }

    UScriptStruct* RowStruct = DataTable->RowStruct;
    const FName RowFName(*RowName);

    // Remember which columns the caller actually provided (for the merge overlay).
    // NOTE: FJsonObject::Values keys are UE::FSharedString; deref (*Key) -> TCHAR*.
    TArray<FString> ProvidedColumns;
    for (const auto& Pair : RowJsonObj->Values) { ProvidedColumns.Add(FString(*Pair.Key)); }

    // Force the key field so the native importer keys the row on row_name.
    const FString KeyField = DataTable->ImportKeyField.IsEmpty() ? FString(TEXT("Name")) : DataTable->ImportKeyField;
    RowJsonObj->SetStringField(KeyField, RowName);

    // Import through a TRANSIENT table with the same RowStruct, as a 1-element array (CreateTableFromJSONString
    // expects an array of rows): UE's own importer round-trips what the reflection field-by-field path does not.
    TArray<TSharedPtr<FJsonValue>> Rows;
    Rows.Add(MakeShared<FJsonValueObject>(RowJsonObj));
    TArray<FString> Problems;
    UDataTable* Temp = Impl::ImportRowsIntoTransientTable(DataTable, Rows, Problems);

    const uint8* ImportedRow = Temp->FindRowUnchecked(RowFName);
    if (!ImportedRow)
    {
        return Impl::NoImportedRowError(RowName, Problems);
    }

    const bool bExisted = (DataTable->FindRowUnchecked(RowFName) != nullptr);

    // Assemble the final row.
    FStructOnScope FinalScope(RowStruct);
    uint8* FinalMemory = FinalScope.GetStructMemory();
    if (!FinalMemory)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate temporary row memory"));
    }

    if (bMerge && bExisted)
    {
        // Seed from the LIVE row (byte-preserves untouched columns -> dodges the
        // TMap<Enum,SoftClass> re-export caveat for columns the caller did not touch),
        // then overlay ONLY the caller-provided columns from the freshly imported row.
        const uint8* Current = DataTable->FindRowUnchecked(RowFName);
        RowStruct->CopyScriptStruct(FinalMemory, Current);
        for (const FString& Col : ProvidedColumns)
        {
            FProperty* Property = Impl::FindColumn(DataTable, Col);
            if (!Property) { continue; }
            Property->CopyCompleteValue(
                Property->ContainerPtrToValuePtr<void>(FinalMemory),
                Property->ContainerPtrToValuePtr<void>(ImportedRow));
        }
    }
    else
    {
        RowStruct->CopyScriptStruct(FinalMemory, ImportedRow);
    }

    // Upsert (AddRow removes any existing row first, then copies in the new struct memory).
    Impl::AddRowWithEditorNotify(DataTable, RowFName, FinalMemory, RowStruct);
    DataTable->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    const uint8* Written = DataTable->FindRowUnchecked(RowFName);
    const bool bRowExists = (Written != nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_name"), RowName);
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
    ResultObj->SetBoolField(TEXT("success"), bRowExists);
    ResultObj->SetBoolField(TEXT("updated_existing"), bExisted);
    ResultObj->SetBoolField(TEXT("merged"), bMerge && bExisted);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());

    // Surface any importer problems (a null AddRow-source would already have errored).
    // On a MERGE, suppress the FALSE "Row 'X' is missing an entry for column 'Y'" problems
    // that a PARTIAL payload always emits from CreateTableFromJSONString: on merge we
    // deliberately supply only the caller's columns and preserve every untouched column from
    // the live row above, so the transient table's missing-column defaults are irrelevant.
    // Any OTHER problem (a genuinely bad value) is kept. On a FULL write (no merge) every
    // column IS expected, so a "missing" problem there is real and stays. Mirrors the filter
    // in set_data_table_row_fields.
    ResultObj->SetArrayField(TEXT("problems"),
        mcp_common::StringsToJson(bMerge && bExisted ? Impl::DropMissingEntryProblems(Problems) : Problems));

    // Echo the persisted row (typed) for verification-by-readback.
    if (bRowExists)
    {
        TSharedPtr<FJsonObject> RowJson = MakeShared<FJsonObject>();
        RowToJson(RowStruct, Written, RowJson);
        ResultObj->SetObjectField(TEXT("row_data"), RowJson);
    }

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    // The verdict is BOTH things: the operation succeeded AND it persisted.
    // If the file cannot be written (read-only, or locked by source control) the
    // save fails and the change stays in memory only; that is not a success.
    return mcp_common::MakeOutcomeReply(ResultObj, (bRowExists) && bSaved,
        ((bRowExists) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("set_data_table_row_from_json")));
}

// ========================================================================== //
// Non-destructive partial-cell write + ExportText complex read
//
// set_data_table_row_fields writes ONLY the caller-named cells: it byte-seeds the
// final row from the LIVE row (so untouched columns are preserved verbatim -- this is
// what dodges the TMap<Enum,SoftClass> re-export caveat for columns nobody touched),
// then overlays just the provided columns, which it obtains by round-tripping the
// provided `fields` through UE's OWN DataTable JSON importer in a transient table (the
// only path that reimports a TMap<Enum,SoftClass> correctly -- with each map value as a
// BARE path string, never the reflection ExportText wrapper). read_data_table_row_complex
// echoes each cell via FProperty::ExportText so TMaps / nested arrays / soft refs, which
// get_data_table_row serializes to {} for TMaps, actually appear.
// ========================================================================== //

namespace
{
    // Export ONE column of a row (RowStruct-typed memory) to its UE ExportText string.
    // Data is the row CONTAINER base ptr; ExportText_InContainer applies the offset itself.
    FString ColumnExportText(const FProperty* Property, const void* RowData)
    {
        FString Exported;
        if (Property && RowData)
        {
            Property->ExportText_InContainer(0, Exported, RowData, /*Delta*/ nullptr, /*Parent*/ nullptr, PPF_None);
        }
        return Exported;
    }

    // Serialize every column of a row to its ExportText string form (the
    // get_data_table_column_as_string style) into OutObj as {column: exporttext}.
    void RowToExportTextObject(const UScriptStruct* RowStruct, const void* RowData, TSharedPtr<FJsonObject>& OutObj)
    {
        if (!RowStruct || !RowData) { return; }
        for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
        {
            FProperty* Property = *It;
            if (!Property) { continue; }
            OutObj->SetStringField(Property->GetName(), ColumnExportText(Property, RowData));
        }
    }

    // Serialize one row via UE's native DataTable JSON writer AS A RECURSABLE JSON OBJECT for the
    // deep-merge. This is NOT byte-identical to the get_data_table_row_as_json path:
    //   (1) EDataTableExportFlags::UseJsonObjectsForStructs (that path uses None) so nested struct
    //       members export as JSON objects the deep-merge can recurse into, not one opaque ExportText
    //       string; AND
    //   (2) it FORCES the legacy field-iterator writer (CVar DataTableJSON.ExportUsingPropertyVisitor=0)
    //       for the duration of the call. The DEFAULT visitor writer serializes a TMap<Enum,SoftClass>
    //       value as a WRAPPED object {"<ValuePropName>":"<class path>"}, which
    //       CreateTableFromJSONString CANNOT reimport (a soft-class map value must be a bare string ->
    //       "Expected String, got Object"), silently dropping untouched sibling map keys.
    //       Field-iterator + the flag = object-form structs AND bare-string map values = the only
    //       combination that round-trips. WARNING (maintenance): the field-iterator writer path is documented as
    //       possibly-removed in a future engine; if it goes, switch to a property-level merge (no JSON
    //       round-trip). A runtime round-trip smoke test on a TMap<Enum,SoftClass> table guards this.
    TSharedPtr<FJsonObject> MCPExt_RowToNativeJsonObject(UDataTable* Table, const void* RowData)
    {
        if (!Table || !RowData) { return MakeShared<FJsonObject>(); }

        IConsoleVariable* CVarVisitor = IConsoleManager::Get().FindConsoleVariable(
            TEXT("DataTableJSON.ExportUsingPropertyVisitor"));
        const bool bPrevVisitor = CVarVisitor ? CVarVisitor->GetBool() : false;
        if (CVarVisitor) { CVarVisitor->Set(TEXT("0"), ECVF_SetByCode); }

        const FString RowJsonStr = MCPExt_WriteRowAsNativeJsonString(Table, RowData, EDataTableExportFlags::UseJsonObjectsForStructs);

        if (CVarVisitor) { CVarVisitor->Set(bPrevVisitor ? TEXT("1") : TEXT("0"), ECVF_SetByCode); }

        return MCPExt_ParseJsonObject(RowJsonStr);
    }

    // Deep-merge Overlay INTO Base (mutates Base): for each key in Overlay, if BOTH Base[key] and
    // Overlay[key] are JSON OBJECTS, recurse (preserving Base's untouched sub-fields); otherwise
    // Overlay[key] REPLACES Base[key] wholesale (scalars / strings / arrays / type-mismatch / new key).
    // This is the rule that turns a provided struct-member sub-field into a deep-merge, not a clobber.
    // A TMap<Enum,SoftClass> column exports (field-iterator) as {EnumName:"<bare path>"}, so a provided
    // map is KEY-merged (add/override provided keys, keep the rest) -- REQUIRED so setting one key
    // does not wipe the others (the engine importer EmptyValues() a map, so only a pre-merged complete
    // map round-trips). Merge cannot express DELETING a map key (acceptable for cell edits).
    void MCPExt_DeepMergeJsonObjectInto(const TSharedPtr<FJsonObject>& Base, const TSharedPtr<FJsonObject>& Overlay)
    {
        if (!Base.IsValid() || !Overlay.IsValid()) { return; }
        for (const auto& Pair : Overlay->Values)
        {
            const FString Key = FString(*Pair.Key);
            const TSharedPtr<FJsonValue>& OverlayVal = Pair.Value;
            const TSharedPtr<FJsonObject>* BaseSubPtr = nullptr;
            if (OverlayVal.IsValid() && OverlayVal->Type == EJson::Object &&
                Base->TryGetObjectField(Key, BaseSubPtr) && BaseSubPtr && (*BaseSubPtr).IsValid())
            {
                MCPExt_DeepMergeJsonObjectInto(*BaseSubPtr, OverlayVal->AsObject());
            }
            else
            {
                Base->SetField(Key, OverlayVal);
            }
        }
    }
}

namespace UEAgentDataAssetCommandsImpl
{
    /**
     * Splits the requested Columns by their ExportText on WrittenRow: into OutChanged when it moved from
     * BeforeExport, into OutUnchanged when it did not (a write that silently did not take).
     */
    void ClassifyColumnChanges(const TArray<TPair<FString, FProperty*>>& Columns, const TMap<FString, FString>& BeforeExport,
        const void* WrittenRow, TArray<FString>& OutChanged, TArray<FString>& OutUnchanged)
    {
        for (const TPair<FString, FProperty*>& KV : Columns)
        {
            const FString AfterVal = ColumnExportText(KV.Value, WrittenRow);
            const FString* BeforeVal = BeforeExport.Find(KV.Key);
            if (!BeforeVal || *BeforeVal != AfterVal)
            {
                OutChanged.Add(KV.Key);
            }
            else
            {
                OutUnchanged.Add(KV.Key);
            }
        }
    }
}

// MCP:set_data_table_row_fields - Write only the named cells of one DataTable row via the native JSON importer, deep-merged onto the current row so untouched columns and struct sub-fields survive, then save. Returns changed_columns, unchanged_despite_request, unknown_columns (skipped) and the row as ExportText. For many rows use set_data_table_rows_from_json.
//   @param data_table - Full asset path of the DataTable (table_path also accepted)
//   @param row_name - Name (key) of the row to write
//   @param fields - Object of {column_name: value} to overlay onto the current row (only these cells are written); a TMap<Enum,SoftClass> value must be an object of {EnumName: bare-class-path} (e.g. {"MyEnumValue":"/Game/...ABP_x_C"}) -- passed through verbatim, never wrapped
//   @param create_if_missing - When true, create the row (from defaults) if it does not exist; when false (default) a missing row is an error
//   @param table_path - Alias of data_table (the DataTable asset path, e.g. "/Game/Data/DT_Items"); data_table wins when both are given
//   @returns { table_path, row_name, row_struct, success, saved, created, updated_existing, written_fields:[..], changed_columns:[..], unchanged_despite_request:[..], unknown_columns?:[..], problems:[..], row_after_exporttext:{column:exporttext} }
TSharedPtr<FJsonObject> HandleSetDataTableRowFields(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString TablePath;
    if (!Params->TryGetStringField(TEXT("data_table"), TablePath) &&
        !Params->TryGetStringField(TEXT("table_path"), TablePath))
    {
        return mcp_common::MissingParameterReply(TEXT("data_table"));
    }
    FString RowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const TSharedPtr<FJsonObject>* FieldsObjPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("fields"), FieldsObjPtr) || !FieldsObjPtr || !(*FieldsObjPtr).IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: fields (object of {column:value})"));
    }
    bool bCreateIfMissing = false;
    Params->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = Impl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }

    UScriptStruct* RowStruct = DataTable->RowStruct;
    const FName RowFName(*RowName);
    const bool bExisted = (DataTable->FindRowUnchecked(RowFName) != nullptr);
    if (!bExisted && !bCreateIfMissing)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Row not found: '%s' (pass create_if_missing=true to create it)"), *RowName));
    }

    // Resolve each provided column to a property; classify unknowns. WrittenFields is the
    // set of columns that actually resolved and will be overlaid.
    TArray<FString> WrittenFields;
    TArray<FString> UnknownColumns;
    TArray<TPair<FString, FProperty*>> ColProps;
    for (const auto& Pair : (*FieldsObjPtr)->Values)
    {
        const FString Col = FString(*Pair.Key);
        FProperty* Property = Impl::FindColumn(DataTable, Col);
        if (!Property) { UnknownColumns.Add(Col); continue; }
        ColProps.Add(TPair<FString, FProperty*>(Col, Property));
        WrittenFields.Add(Col);
    }

    // Build the COMPLETE target row JSON = deep-merge(current-row native JSON, provided fields):
    // export the current row -- or a default-initialized row for create_if_missing -- via UE's
    // native DataTable JSON writer, then deep-merge the caller's `fields` INTO it. A provided value
    // that is a JSON OBJECT is deep-merged into the existing member (untouched sibling sub-fields are
    // preserved); scalars/strings/arrays REPLACE.
    // Importing the COMPLETE row (never a partial overlay) is what stops the transient importer from
    // resetting un-provided struct-member siblings to defaults. Then run it through UE's native
    // DataTable JSON importer in a TRANSIENT table (same RowStruct) -- the one path that reimports
    // TMap<Enum,SoftClass> / nested arrays / soft refs exactly (each TMap value a bare path string).
    TSharedPtr<FJsonObject> OverlayObj;
    {
        FStructOnScope BaseScope(RowStruct);
        // BaseMem is the live row (bExisted) or a default-initialized row (create_if_missing).
        uint8* BaseMem = Impl::SeedWithLiveRow(BaseScope, DataTable, RowFName, bExisted);
        OverlayObj = MCPExt_RowToNativeJsonObject(DataTable, BaseMem);
        MCPExt_DeepMergeJsonObjectInto(OverlayObj, *FieldsObjPtr);
    }
    const FString KeyField = DataTable->ImportKeyField.IsEmpty() ? FString(TEXT("Name")) : DataTable->ImportKeyField;
    OverlayObj->SetStringField(KeyField, RowName);

    TArray<TSharedPtr<FJsonValue>> Rows;
    Rows.Add(MakeShared<FJsonValueObject>(OverlayObj));
    TArray<FString> RawProblems;
    UDataTable* Temp = Impl::ImportRowsIntoTransientTable(DataTable, Rows, RawProblems);

    // Suppress the FALSE "Row 'X' is missing an entry for column 'Y'" problems that a PARTIAL
    // overlay always emits: we deliberately supply only the caller-named columns; every other
    // column is preserved from the live row below, so the transient's missing-column defaults
    // are irrelevant. Any OTHER problem (a genuinely bad value) is kept for the caller.
    const TArray<FString> Problems = Impl::DropMissingEntryProblems(RawProblems);

    const uint8* ImportedRow = Temp->FindRowUnchecked(RowFName);
    if (!ImportedRow)
    {
        return Impl::NoImportedRowError(RowName, Problems);
    }

    // Capture the BEFORE ExportText of each provided column (from the live row if it exists,
    // else the default-initialized row) so we can report changed_columns after the write.
    TMap<FString, FString> BeforeExport;
    {
        FStructOnScope BeforeScope(RowStruct);
        uint8* BeforeMem = Impl::SeedWithLiveRow(BeforeScope, DataTable, RowFName, bExisted);
        for (const TPair<FString, FProperty*>& KV : ColProps)
        {
            BeforeExport.Add(KV.Key, ColumnExportText(KV.Value, BeforeMem));
        }
    }

    // ImportedRow is now the COMPLETE target row (deep-merge of the current row + provided fields,
    // run through UE's native importer so TMap<Enum,SoftClass> / soft refs / nested arrays are
    // correct). Write it WHOLESALE: every column -- including untouched struct-member siblings --
    // is already correct, so there is no per-column overlay and no sibling clobber. (A per-column
    // copy would replace a touched struct column with the transient row's value, whose un-provided
    // siblings are struct defaults -> silent sibling data loss.)
    FStructOnScope FinalScope(RowStruct);
    uint8* FinalMemory = FinalScope.GetStructMemory();
    if (!FinalMemory)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate temporary row memory"));
    }
    RowStruct->CopyScriptStruct(FinalMemory, ImportedRow);

    // Upsert (AddRow removes any existing row first, then copies in the new struct memory).
    Impl::AddRowWithEditorNotify(DataTable, RowFName, FinalMemory, RowStruct);
    DataTable->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    const uint8* Written = DataTable->FindRowUnchecked(RowFName);
    const bool bRowExists = (Written != nullptr);

    // changed_columns = provided columns whose ExportText differs before vs after.
    // unchanged_despite_request = provided columns that resolved but whose post-write ExportText did
    // NOT move (a silent no-op -- e.g. a TMap<Enum,SoftClass> that stayed empty, or a value that did
    // not take, while success=true). It is written_fields minus changed_columns. Also echo the whole
    // row via ExportText for verification-by-readback.
    TArray<FString> ChangedColumns;
    TArray<FString> UnchangedDespiteRequest;
    TSharedPtr<FJsonObject> RowAfterExport = MakeShared<FJsonObject>();
    if (bRowExists)
    {
        RowToExportTextObject(RowStruct, Written, RowAfterExport);
        Impl::ClassifyColumnChanges(ColProps, BeforeExport, Written, ChangedColumns, UnchangedDespiteRequest);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_name"), RowName);
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
    ResultObj->SetBoolField(TEXT("success"), bRowExists);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetBoolField(TEXT("created"), !bExisted && bRowExists);
    ResultObj->SetBoolField(TEXT("updated_existing"), bExisted);
    ResultObj->SetArrayField(TEXT("written_fields"), mcp_common::StringsToJson(WrittenFields));
    ResultObj->SetArrayField(TEXT("changed_columns"), mcp_common::StringsToJson(ChangedColumns));
    ResultObj->SetArrayField(TEXT("unchanged_despite_request"), mcp_common::StringsToJson(UnchangedDespiteRequest));
    if (UnknownColumns.Num() > 0)
    {
        ResultObj->SetArrayField(TEXT("unknown_columns"), mcp_common::StringsToJson(UnknownColumns));
    }
    ResultObj->SetArrayField(TEXT("problems"), mcp_common::StringsToJson(Problems));
    ResultObj->SetObjectField(TEXT("row_after_exporttext"), RowAfterExport);

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    // The verdict is BOTH things: the operation succeeded AND it persisted.
    // If the file cannot be written (read-only, or locked by source control) the
    // save fails and the change stays in memory only; that is not a success.
    return mcp_common::MakeOutcomeReply(ResultObj, (bRowExists) && bSaved,
        ((bRowExists) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("set_data_table_row_fields")));
}

// MCP:read_data_table_row_complex - Read one DataTable row with every cell as its ExportText string, so TMap<Enum,SoftClass>, nested struct arrays and soft references appear in full (get_data_table_row shows TMaps as {}); read-only. Returns row_struct and fields as {column: text}; a missing row is an error.
//   @param data_table - Full asset path of the DataTable (table_path also accepted)
//   @param row_name - Name (key) of the row to read
//   @param table_path - Alias of data_table (the DataTable asset path, e.g. "/Game/Data/DT_Items"); data_table wins when both are given
//   @returns { table_path, row_name, row_struct, fields:{column: exporttext-serialized value} }
TSharedPtr<FJsonObject> HandleReadDataTableRowComplex(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    if (!Params->TryGetStringField(TEXT("data_table"), TablePath) &&
        !Params->TryGetStringField(TEXT("table_path"), TablePath))
    {
        return mcp_common::MissingParameterReply(TEXT("data_table"));
    }
    FString RowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UDataTable* DataTable = nullptr;
    const uint8* RowData = nullptr;
    if (TSharedPtr<FJsonObject> Failure = UEAgentDataAssetCommandsImpl::OpenRow(TablePath, RowName, DataTable, RowData))
    {
        return Failure;
    }

    // Every cell as its ExportText string, so TMap / nested struct arrays / soft refs come out whole.
    TSharedPtr<FJsonObject> ExportedCells = MakeShared<FJsonObject>();
    RowToExportTextObject(DataTable->RowStruct, RowData, ExportedCells);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetObjectField(TEXT("fields"), ExportedCells);
    Reply->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetName());
    Reply->SetStringField(TEXT("row_name"), RowName);
    Reply->SetStringField(TEXT("table_path"), TablePath);
    return mcp_common::MakeOkReply(Reply);
}

// ==========================================================================
// Bulk sibling-safe multi-row upsert + atomic row-key rename.
// Both rely on the helpers above (MCPExt_RowToNativeJsonObject /
// MCPExt_DeepMergeJsonObjectInto / ColumnExportText / UEAgentDataAssetCommandsImpl).
// ==========================================================================

// MCP:set_data_table_rows_from_json - Upsert many DataTable rows with one native JSON import and one save, each deep-merged onto its current values like set_data_table_row_fields; missing rows are created from defaults. Returns per-row results and a summary. A failed row does not fail the call: check summary.failed and saved.
//   @param table_path - Full asset path of the DataTable (data_table also accepted)
//   @param rows - Object of { row_name: {column:value} }, or that object as a JSON string; each fields object is overlaid non-destructively (a value that is a JSON object deep-merges into the existing struct member, scalars/arrays replace)
//   @param save - When true (default) save the package once after all rows are written; when false leave it dirty in memory
//   @param data_table - Alias of table_path (the DataTable asset path, e.g. "/Game/Data/DT_Items"); table_path wins when both are given
//   @returns { table_path, row_struct, results:{ row_name:{ success, created, updated_existing, changed_columns:[..], unchanged_despite_request:[..], unknown_columns?:[..], error? } }, summary:{ total, created, updated, failed }, saved, row_count, problems:[..] }
TSharedPtr<FJsonObject> HandleSetDataTableRowsFromJson(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString TablePath;
    if (!Params->TryGetStringField(TEXT("table_path"), TablePath) &&
        !Params->TryGetStringField(TEXT("data_table"), TablePath))
    {
        return mcp_common::MissingParameterReply(TEXT("table_path"));
    }

    // rows: an object { row_name: {column:value} }. Accept a stringified JSON object too.
    TSharedPtr<FJsonObject> RowsObj;
    const TSharedPtr<FJsonObject>* RowsObjPtr = nullptr;
    if (Params->TryGetObjectField(TEXT("rows"), RowsObjPtr) && RowsObjPtr && (*RowsObjPtr).IsValid())
    {
        RowsObj = *RowsObjPtr;
    }
    else
    {
        FString RowsStr;
        if (Params->TryGetStringField(TEXT("rows"), RowsStr))
        {
            TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(RowsStr);
            if (!FJsonSerializer::Deserialize(Reader, RowsObj) || !RowsObj.IsValid())
            {
                return mcp_common::MakeErrorReply(TEXT("rows string is not a valid JSON object of {row_name:{column:value}}"));
            }
        }
        else
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required parameter: rows (object of {row_name:{column:value}})"));
        }
    }

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = Impl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }

    UScriptStruct* RowStruct = DataTable->RowStruct;
    const FString KeyField = DataTable->ImportKeyField.IsEmpty() ? FString(TEXT("Name")) : DataTable->ImportKeyField;

    // Per-row plan captured BEFORE any write (BeforeExport reflects the pre-batch live state).
    struct FRowPlan
    {
        FString RowName;
        bool bExisted = false;
        bool bBadShape = false;
        TArray<TPair<FString, FProperty*>> ColProps;
        TArray<FString> UnknownColumns;
        TMap<FString, FString> BeforeExport;
    };
    TArray<FRowPlan> Plans;
    TArray<TSharedPtr<FJsonValue>> MergedRows; // complete, key-forced row objects for ONE bulk import

    for (const auto& RowPair : RowsObj->Values)
    {
        FRowPlan Plan;
        Plan.RowName = FString(*RowPair.Key);
        const FName RowFName(*Plan.RowName);
        Plan.bExisted = (DataTable->FindRowUnchecked(RowFName) != nullptr);

        if (!RowPair.Value.IsValid() || RowPair.Value->Type != EJson::Object)
        {
            Plan.bBadShape = true;
            Plans.Add(MoveTemp(Plan));
            continue;
        }
        const TSharedPtr<FJsonObject> FieldsObj = RowPair.Value->AsObject();

        // Resolve provided columns -> properties (for change reporting + unknown-column classification).
        for (const auto& FPair : FieldsObj->Values)
        {
            const FString Col = FString(*FPair.Key);
            FProperty* Property = Impl::FindColumn(DataTable, Col);
            if (!Property) { Plan.UnknownColumns.Add(Col); continue; }
            Plan.ColProps.Add(TPair<FString, FProperty*>(Col, Property));
        }

        // Build the COMPLETE target row = deep-merge(current-or-default native JSON, fields), and
        // capture the BEFORE ExportText of each provided column from that same base memory.
        TSharedPtr<FJsonObject> MergedObj;
        {
            FStructOnScope BaseScope(RowStruct);
            // BaseMem is the live row (bExisted) or a default-initialized row (create-on-upsert).
            uint8* BaseMem = Impl::SeedWithLiveRow(BaseScope, DataTable, RowFName, Plan.bExisted);
            MergedObj = MCPExt_RowToNativeJsonObject(DataTable, BaseMem);
            MCPExt_DeepMergeJsonObjectInto(MergedObj, FieldsObj);
            for (const TPair<FString, FProperty*>& KV : Plan.ColProps)
            {
                Plan.BeforeExport.Add(KV.Key, ColumnExportText(KV.Value, BaseMem));
            }
        }
        MergedObj->SetStringField(KeyField, Plan.RowName);
        MergedRows.Add(MakeShared<FJsonValueObject>(MergedObj));
        Plans.Add(MoveTemp(Plan));
    }

    // ONE bulk native import of all complete rows through a transient table (same RowStruct):
    // UE's own importer, so TMap<Enum,SoftClass> / soft refs / nested arrays round-trip exactly.
    TArray<FString> RawProblems;
    UDataTable* Temp = Impl::ImportRowsIntoTransientTable(DataTable, MergedRows, RawProblems);

    // Apply every imported row in ONE editor change window.
    DataTable->Modify();
    FDataTableEditorUtils::BroadcastPreChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowData);

    int32 CreatedCount = 0, UpdatedCount = 0, FailedCount = 0;
    TSharedPtr<FJsonObject> ResultsObj = MakeShared<FJsonObject>();
    // A row that cannot be written is reported under its name as { success:false, error }.
    auto RecordFailure = [&ResultsObj, &FailedCount](const FString& RowName, const TCHAR* Error)
    {
        TSharedPtr<FJsonObject> RowResult = MakeShared<FJsonObject>();
        RowResult->SetBoolField(TEXT("success"), false);
        RowResult->SetStringField(TEXT("error"), Error);
        ResultsObj->SetObjectField(RowName, RowResult);
        ++FailedCount;
    };

    for (const FRowPlan& Plan : Plans)
    {
        if (Plan.bBadShape)
        {
            RecordFailure(Plan.RowName, TEXT("value must be an object of {column:value}"));
            continue;
        }

        const FName RowFName(*Plan.RowName);
        const uint8* ImportedRow = Temp->FindRowUnchecked(RowFName);
        if (!ImportedRow)
        {
            RecordFailure(Plan.RowName, TEXT("native JSON import produced no row for this key"));
            continue;
        }

        FStructOnScope FinalScope(RowStruct);
        uint8* FinalMemory = FinalScope.GetStructMemory();
        if (!FinalMemory)
        {
            RecordFailure(Plan.RowName, TEXT("failed to allocate row memory"));
            continue;
        }

        // The imported row is COMPLETE (deep-merged) -> write it wholesale (no sibling clobber).
        RowStruct->CopyScriptStruct(FinalMemory, ImportedRow);
        DataTable->AddRow(RowFName, FinalMemory, RowStruct);

        // changed_columns / unchanged_despite_request via before-vs-after ExportText readback.
        const uint8* Written = DataTable->FindRowUnchecked(RowFName);
        TArray<FString> ChangedColumns;
        TArray<FString> UnchangedDespiteRequest;
        if (Written)
        {
            Impl::ClassifyColumnChanges(Plan.ColProps, Plan.BeforeExport, Written, ChangedColumns, UnchangedDespiteRequest);
        }

        TSharedPtr<FJsonObject> RowResult = MakeShared<FJsonObject>();
        RowResult->SetBoolField(TEXT("success"), true);
        RowResult->SetBoolField(TEXT("created"), !Plan.bExisted);
        RowResult->SetBoolField(TEXT("updated_existing"), Plan.bExisted);
        RowResult->SetArrayField(TEXT("changed_columns"), mcp_common::StringsToJson(ChangedColumns));
        RowResult->SetArrayField(TEXT("unchanged_despite_request"), mcp_common::StringsToJson(UnchangedDespiteRequest));
        if (Plan.UnknownColumns.Num() > 0)
        {
            RowResult->SetArrayField(TEXT("unknown_columns"), mcp_common::StringsToJson(Plan.UnknownColumns));
        }
        ResultsObj->SetObjectField(Plan.RowName, RowResult);

        if (Plan.bExisted) { ++UpdatedCount; } else { ++CreatedCount; }
    }

    FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
    DataTable->MarkPackageDirty();

    FString SaveError;
    bool bSaved = false;
    if (bSave)
    {
        bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
    ResultObj->SetObjectField(TEXT("results"), ResultsObj);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());

    // Surface non-"missing entry" importer problems (deep-merged rows are complete, so a genuine
    // problem here is a bad value -- e.g. an unresolvable soft ref -- not a partial-overlay artifact).
    ResultObj->SetArrayField(TEXT("problems"), mcp_common::StringsToJson(Impl::DropMissingEntryProblems(RawProblems)));

    TSharedPtr<FJsonObject> SummaryObj = MakeShared<FJsonObject>();
    SummaryObj->SetNumberField(TEXT("total"), Plans.Num());
    SummaryObj->SetNumberField(TEXT("created"), CreatedCount);
    SummaryObj->SetNumberField(TEXT("updated"), UpdatedCount);
    SummaryObj->SetNumberField(TEXT("failed"), FailedCount);
    ResultObj->SetObjectField(TEXT("summary"), SummaryObj);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:rename_data_table_row - Rename a DataTable row key, copying its value byte-exact (safer than delete plus re-add, which can drop GameplayTags), then save and verify the swap (new_exists, old_removed). Refuses a case-only rename, and an existing new_name unless overwrite=true. Row handles that reference the old name are not updated.
//   @param table_path - Full asset path of the DataTable (data_table also accepted)
//   @param old_name - Existing row key to rename (error if it does not exist)
//   @param new_name - New row key (error if it already exists unless overwrite=true)
//   @param overwrite - When true, allow new_name to replace an existing row; when false (default) a pre-existing new_name is an error
//   @param data_table - Alias of table_path (the DataTable asset path, e.g. "/Game/Data/DT_Items"); table_path wins when both are given
//   @returns { table_path, old_name, new_name, row_struct, success, overwrote_existing, new_exists, old_removed, saved, row_count }
TSharedPtr<FJsonObject> HandleRenameDataTableRow(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString TablePath;
    if (!Params->TryGetStringField(TEXT("table_path"), TablePath) &&
        !Params->TryGetStringField(TEXT("data_table"), TablePath))
    {
        return mcp_common::MissingParameterReply(TEXT("table_path"));
    }
    FString OldName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (OldName == NewName)
    {
        return mcp_common::MakeErrorReply(TEXT("old_name and new_name are identical -- nothing to rename"));
    }
    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = Impl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }

    UScriptStruct* RowStruct = DataTable->RowStruct;
    const FName OldFName(*OldName);
    const FName NewFName(*NewName);

    // The row map is keyed by FName (case-insensitive). A case-only rename (torch->Torch) would
    // AddRow then RemoveRow the SAME key = destroy the row. Guard on the resolved keys, not FStrings.
    if (OldFName == NewFName)
    {
        return mcp_common::MakeErrorReply(TEXT("old_name and new_name resolve to the same row key (case-insensitive) -- nothing to rename"));
    }

    const uint8* OldRowData = DataTable->FindRowUnchecked(OldFName);
    if (!OldRowData)
    {
        return Impl::RowNotFoundError(DataTable, TablePath, OldName);
    }
    const bool bNewExistedBefore = (DataTable->FindRowUnchecked(NewFName) != nullptr);
    if (bNewExistedBefore && !bOverwrite)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Target row already exists: '%s' (pass overwrite=true to replace it)"), *NewName));
    }

    // Copy old_name's struct memory BYTE-EXACT into an owned buffer, then re-key it. A byte copy
    // preserves TMap<Enum,SoftClass> / soft refs / nested arrays perfectly -- it never re-serializes,
    // so it fully sidesteps the JSON re-import caveat this file exists for. AddRow copies FinalMemory
    // in before RemoveRow frees the old_name row, so the value survives the swap.
    FStructOnScope Scope(RowStruct);
    uint8* Mem = Scope.GetStructMemory();
    if (!Mem)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to allocate temporary row memory"));
    }
    RowStruct->CopyScriptStruct(Mem, OldRowData);

    Impl::AddRowWithEditorNotify(DataTable, NewFName, Mem, RowStruct);
    // Drop old_name (FDataTableEditorUtils::RemoveRow runs its own transaction + pre/post broadcast).
    FDataTableEditorUtils::RemoveRow(DataTable, OldFName);
    DataTable->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    // Verify by readback: new key present, old key gone.
    const bool bNewExists = (DataTable->FindRowUnchecked(NewFName) != nullptr);
    const bool bOldGone = (DataTable->FindRowUnchecked(OldFName) == nullptr);
    const bool bSuccess = bNewExists && bOldGone;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("old_name"), OldName);
    ResultObj->SetStringField(TEXT("new_name"), NewName);
    ResultObj->SetStringField(TEXT("row_struct"), RowStruct->GetName());
    ResultObj->SetBoolField(TEXT("success"), bSuccess);
    ResultObj->SetBoolField(TEXT("overwrote_existing"), bNewExistedBefore);
    ResultObj->SetBoolField(TEXT("new_exists"), bNewExists);
    ResultObj->SetBoolField(TEXT("old_removed"), bOldGone);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    // The verdict is BOTH things: the operation succeeded AND it persisted.
    // If the file cannot be written (read-only, or locked by source control) the
    // save fails and the change stays in memory only; that is not a success.
    return mcp_common::MakeOutcomeReply(ResultObj, (bSuccess) && bSaved,
        ((bSuccess) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("rename_data_table_row")));
}

// ----------------------------------------------------------------------------
// Curve asset authoring
// ----------------------------------------------------------------------------

// MCP:create_curve_asset - Create a curve asset (CurveFloat, CurveVector or CurveLinearColor) -- a reusable value-over-time or value-over-input ramp for damage falloff, stat growth, fade timing or colour gradients, referenced by assets instead of hard-coded numbers. Channel count follows the type (1 / 3 / 4), and add_curve_key fills it. Different thing from a CurveTable (create_curve_table), which holds many named curves as rows.
//   @param name - Asset name to create
//   @param package_path - Target content package path where the asset is created (check the body for the default)
//   @param curve_type - Curve type to create (read the body for the allowed values)
//   @returns { name, package_path, curve_type, asset_path, curve_class, num_channels, created, already_exists, saved, success, message }
TSharedPtr<FJsonObject> HandleCreateCurveAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    FString AssetName;
    FString CurveType;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/Curves");
    }
    if (!Params->TryGetStringField(TEXT("curve_type"), CurveType))
    {
        CurveType = TEXT("Float");
    }

    // Resolve the concrete UCurve* class from the requested type.
    UClass* CurveClass = nullptr;
    const FString TypeLow = CurveType.ToLower();
    int32 NumChannels = 0;
    if (TypeLow == TEXT("float"))
    {
        CurveClass = UCurveFloat::StaticClass();
        NumChannels = 1;
    }
    else if (TypeLow == TEXT("vector"))
    {
        CurveClass = UCurveVector::StaticClass();
        NumChannels = 3;
    }
    else if (TypeLow == TEXT("linearcolor") || TypeLow == TEXT("color"))
    {
        CurveClass = UCurveLinearColor::StaticClass();
        NumChannels = 4;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown curve_type '%s' (expected Float|Vector|LinearColor)"), *CurveType));
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load and return it.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UObject* Existing = StaticLoadObject(CurveClass, nullptr, *FullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ExistObj = UEAgentDataAssetCommandsImpl::CreatedCurveReply(AssetName, PackagePath, CurveType, Existing, NumChannels);
            DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj);
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Curve already exists at %s but could not be loaded as %s"), *FullPath, *CurveClass->GetName()));
    }

    // Create a real /Game package + curve object inside it, then save (mirrors the
    // create_data_table flow). Curve assets have no inner objects, so direct
    // NewObject is correct and avoids the curve factory's modal type picker.
    FString CreateError;
    UCurveBase* NewCurve = Cast<UCurveBase>(mcp_common::NewAssetInPackage(
        CurveClass, FullPath, AssetName, TEXT("Failed to create curve asset"), CreateError, RF_Public | RF_Standalone));
    if (!NewCurve)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewCurve);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewCurve->GetOutermost(), NewCurve, SaveError);

    TSharedPtr<FJsonObject> ResultObj = UEAgentDataAssetCommandsImpl::CreatedCurveReply(AssetName, PackagePath, CurveType, NewCurve, NumChannels);
    DataAuthoringUtils::AddCreatedOutcome(ResultObj, bSaved, TEXT("Curve asset"), SaveError);

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_curve_asset"), TEXT("bSaved")));
}

// Resolve the FRichCurve* for a given channel on a loaded curve object.
// Returns nullptr if the channel index is out of range for the curve type.
static FRichCurve* GetRichCurveForChannel(UObject* CurveObj, int32 Channel, int32& OutNumChannels)
{
    if (UCurveFloat* CF = Cast<UCurveFloat>(CurveObj))
    {
        OutNumChannels = 1;
        return (Channel == 0) ? &CF->FloatCurve : nullptr;
    }
    if (UCurveVector* CV = Cast<UCurveVector>(CurveObj))
    {
        OutNumChannels = 3;
        return (Channel >= 0 && Channel < 3) ? &CV->FloatCurves[Channel] : nullptr;
    }
    if (UCurveLinearColor* CC = Cast<UCurveLinearColor>(CurveObj))
    {
        OutNumChannels = 4;
        return (Channel >= 0 && Channel < 4) ? &CC->FloatCurves[Channel] : nullptr;
    }
    OutNumChannels = 0;
    return nullptr;
}

namespace UEAgentDataAssetCommandsImpl
{
    /**
     * The curve asset at CurvePath and its channel Channel; the error reply when the asset is not a curve or has
     * no such channel. OutNumChannels is the curve's channel count.
     */
    TSharedPtr<FJsonObject> OpenCurveChannel(const FString& CurvePath, int32 Channel,
        UObject*& OutCurveObj, FRichCurve*& OutCurve, int32& OutNumChannels)
    {
        OutCurveObj = LoadCurveObjectAt(CurvePath);
        if (!OutCurveObj || !OutCurveObj->IsA(UCurveBase::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Curve asset not found: %s"), *CurvePath));
        }
        OutCurve = GetRichCurveForChannel(OutCurveObj, Channel, OutNumChannels);
        if (!OutCurve)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid channel %d for curve (curve has %d channel(s))"), Channel, OutNumChannels));
        }
        return nullptr;
    }
}

// MCP:add_curve_key - Add or update a keyframe (time, value) on one channel of a CurveFloat, CurveVector or CurveLinearColor asset, set its interpolation and save the asset; a key already at that time is replaced. Returns the read-back time, value, interp and key_count. Inverse: remove_curve_key; list keys with get_curve_keys.
//   @param curve_path - Full asset path of the curve
//   @param time - Key time on the X axis of the curve, in the units the curve uses (often seconds); an existing key at this time is replaced
//   @param value - Key value (Y axis) stored at that time on the chosen channel, as a float
//   @param channel - Curve channel index, default 0: 0 for CurveFloat, 0-2 (X, Y, Z) for CurveVector, 0-3 (R, G, B, A) for CurveLinearColor; out of range is an error
//   @param interp - Key interpolation mode (constant|cubic|linear, default linear); any other value silently falls back to linear
//   @returns { curve_path, curve_class, channel, num_channels, time, value, interp, key_added, key_count, saved }
TSharedPtr<FJsonObject> HandleAddCurveKey(const TSharedPtr<FJsonObject>& Params)
{
    FString CurvePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_path"), CurvePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double TimeD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time"), TimeD)) { return Missing; }
    double ValueD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("value"), ValueD)) { return Missing; }

    int32 Channel = 0;
    double ChannelD = 0.0;
    if (Params->TryGetNumberField(TEXT("channel"), ChannelD))
    {
        Channel = static_cast<int32>(ChannelD);
    }

    FString InterpStr;
    Params->TryGetStringField(TEXT("interp"), InterpStr);
    const ERichCurveInterpMode InterpMode = ParseInterpMode(InterpStr);

    UObject* CurveObj = nullptr;
    FRichCurve* Curve = nullptr;
    int32 NumChannels = 0;
    if (TSharedPtr<FJsonObject> Failure = UEAgentDataAssetCommandsImpl::OpenCurveChannel(CurvePath, Channel, CurveObj, Curve, NumChannels))
    {
        return Failure;
    }

    CurveObj->Modify();

    // UpdateOrAddKey replaces a key at the same time (within tolerance) or adds one.
    const FKeyHandle Handle = Curve->UpdateOrAddKey(static_cast<float>(TimeD), static_cast<float>(ValueD));
    Curve->SetKeyInterpMode(Handle, InterpMode);

    CurveObj->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(CurveObj->GetOutermost(), CurveObj, SaveError);

    // Verify by readback: re-read the key time/value from the curve via the handle.
    const float ReadTime = Curve->GetKeyTime(Handle);
    const float ReadValue = Curve->GetKeyValue(Handle);
    const ERichCurveInterpMode ReadInterp = Curve->GetKeyInterpMode(Handle);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("curve_path"), CurvePath);
    ResultObj->SetStringField(TEXT("curve_class"), CurveObj->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("channel"), Channel);
    ResultObj->SetNumberField(TEXT("num_channels"), NumChannels);
    ResultObj->SetNumberField(TEXT("time"), ReadTime);
    ResultObj->SetNumberField(TEXT("value"), ReadValue);
    ResultObj->SetStringField(TEXT("interp"), InterpModeToString(ReadInterp));
    ResultObj->SetBoolField(TEXT("key_added"), true);
    ResultObj->SetNumberField(TEXT("key_count"), Curve->GetNumKeys());

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_curve_key"), SaveError);
}

// MCP:remove_curve_key - Delete one keyframe from a curve channel, addressed either by key_index or by time -- the inverse of add_curve_key. Indices shift after a removal, so prefer addressing by time, or work from the highest index down. The reply's existed flag distinguishes "removed it" from "there was no key there"; get_curve_keys gives the indices and times.
//   @param curve_path - Full asset path of the curve
//   @param channel - Curve channel index (defaults to 0)
//   @param key_index - Index of the key to remove (0 <= index < key_count); provide this OR time
//   @param time - Time of the key to remove; provide this OR key_index
//   @returns { curve_path, curve_class, channel, num_channels, removed, existed, key_count, saved }
TSharedPtr<FJsonObject> HandleRemoveCurveKey(const TSharedPtr<FJsonObject>& Params)
{
    FString CurvePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_path"), CurvePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    int32 Channel = 0;
    double ChannelD = 0.0;
    if (Params->TryGetNumberField(TEXT("channel"), ChannelD))
    {
        Channel = static_cast<int32>(ChannelD);
    }

    // Exactly one of key_index / time selects the key to remove.
    int32 KeyIndex = 0;
    double TimeD = 0.0;
    const bool bHasKeyIndex = Params->TryGetNumberField(TEXT("key_index"), KeyIndex);
    const bool bHasTime = Params->TryGetNumberField(TEXT("time"), TimeD);
    if (bHasKeyIndex == bHasTime)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide exactly one of: key_index (int) or time (number)"));
    }

    UObject* CurveObj = nullptr;
    FRichCurve* Curve = nullptr;
    int32 NumChannels = 0;
    if (TSharedPtr<FJsonObject> Failure = UEAgentDataAssetCommandsImpl::OpenCurveChannel(CurvePath, Channel, CurveObj, Curve, NumChannels))
    {
        return Failure;
    }

    FKeyHandle Handle;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::FindCurveKey(*Curve, bHasKeyIndex, KeyIndex, TimeD, TEXT("curve"), Handle))
    {
        return Failure;
    }

    // DeleteKey does not validate the handle and asserts on an invalid one, so gate
    // on IsKeyHandleValid. An unresolved key is an honest no-op (existed=false).
    const bool bExisted = Curve->IsKeyHandleValid(Handle);
    bool bSaved = false;
    FString SaveError;
    if (bExisted)
    {
        CurveObj->Modify();
        Curve->DeleteKey(Handle);
        CurveObj->MarkPackageDirty();
        bSaved = mcp_common::SaveAssetPackage(CurveObj->GetOutermost(), CurveObj, SaveError);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("curve_path"), CurvePath);
    ResultObj->SetStringField(TEXT("curve_class"), CurveObj->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("channel"), Channel);
    ResultObj->SetNumberField(TEXT("num_channels"), NumChannels);
    ResultObj->SetBoolField(TEXT("removed"), bExisted);
    ResultObj->SetBoolField(TEXT("existed"), bExisted);
    ResultObj->SetNumberField(TEXT("key_count"), Curve->GetNumKeys());

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_curve_key"));
}

// MCP:get_curve_keys - Read every keyframe of a curve asset -- time, value and interpolation mode, grouped per channel, with per-channel and total counts. The read-back oracle for add_curve_key and the way to see the actual shape of a damage-falloff or growth curve before tuning it. Returns the authored KEYS; to sample the interpolated value at an arbitrary time use evaluate_curve_at_time instead.
//   @param curve_path - Full asset path of the curve
//   @returns { curve_path, curve_class, num_channels, channel, time, value, interp, keys, key_count, channels, total_key_count }
TSharedPtr<FJsonObject> HandleGetCurveKeys(const TSharedPtr<FJsonObject>& Params)
{
    FString CurvePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_path"), CurvePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UObject* CurveObj = LoadCurveObjectAt(CurvePath);
    if (!CurveObj || !CurveObj->IsA(UCurveBase::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Curve asset not found: %s"), *CurvePath));
    }

    // Determine channel count from the concrete type.
    int32 NumChannels = 0;
    if (Cast<UCurveFloat>(CurveObj)) { NumChannels = 1; }
    else if (Cast<UCurveVector>(CurveObj)) { NumChannels = 3; }
    else if (Cast<UCurveLinearColor>(CurveObj)) { NumChannels = 4; }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("curve_path"), CurvePath);
    ResultObj->SetStringField(TEXT("curve_class"), CurveObj->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("num_channels"), NumChannels);

    auto KeyToJson = [](const FRichCurveKey& Key) -> TSharedPtr<FJsonValue>
    {
        TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
        KeyObj->SetNumberField(TEXT("time"), Key.Time);
        KeyObj->SetNumberField(TEXT("value"), Key.Value);
        KeyObj->SetStringField(TEXT("interp"), InterpModeToString(Key.InterpMode.GetValue()));
        return MakeShared<FJsonValueObject>(KeyObj);
    };

    // channels: array of { channel, keys: [ {time, value, interp} ] }
    TArray<TSharedPtr<FJsonValue>> ChannelsArr;
    int32 TotalKeys = 0;
    for (int32 Ch = 0; Ch < NumChannels; ++Ch)
    {
        int32 NC = 0;
        FRichCurve* Curve = GetRichCurveForChannel(CurveObj, Ch, NC);
        if (!Curve)
        {
            continue;
        }

        TSharedPtr<FJsonObject> ChanObj = MakeShared<FJsonObject>();
        ChanObj->SetNumberField(TEXT("channel"), Ch);

        TArray<TSharedPtr<FJsonValue>> KeysArr;
        const TArray<FRichCurveKey>& Keys = Curve->GetConstRefOfKeys();
        for (const FRichCurveKey& Key : Keys)
        {
            KeysArr.Add(KeyToJson(Key));
            ++TotalKeys;
        }
        ChanObj->SetArrayField(TEXT("keys"), KeysArr);
        ChanObj->SetNumberField(TEXT("key_count"), KeysArr.Num());
        ChannelsArr.Add(MakeShared<FJsonValueObject>(ChanObj));
    }

    ResultObj->SetArrayField(TEXT("channels"), ChannelsArr);
    ResultObj->SetNumberField(TEXT("total_key_count"), TotalKeys);

    // Convenience flat key list for single-channel (float) curves so callers can
    // read keys[] directly without indexing channels.
    if (NumChannels == 1)
    {
        int32 NC = 0;
        FRichCurve* Curve = GetRichCurveForChannel(CurveObj, 0, NC);
        TArray<TSharedPtr<FJsonValue>> FlatKeys;
        if (Curve)
        {
            for (const FRichCurveKey& Key : Curve->GetConstRefOfKeys())
            {
                FlatKeys.Add(KeyToJson(Key));
            }
        }
        ResultObj->SetArrayField(TEXT("keys"), FlatKeys);
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// read_datatable_from_package_file -- read a DataTable from a RAW EXTERNAL .uasset file (collision-safe)
// diff_data_tables                 -- per-row/per-cell DataTable diff (two assets, or before/after JSON)
//
// read_datatable_from_package_file: the file declares a /Game/... package name shared with the LIVE project, so a
// plain LoadPackage(nullptr, "/Game/...", ...) would collide with the in-memory copy.
// We dodge that by registering a THROWAWAY, uniquely-named mount point over the file's
// directory and loading it under that private root (/MCPExtRead_<guid>/<Asset>), so the
// live /Game/... object is never rebound or touched. Rows are serialized with the SAME
// native UDataTable::WriteRowAsJSON path as get_data_table_row_as_json. The mount is
// unregistered + the throwaway load's keep-flags cleared + GC'd afterward.
//
// diff_data_tables: builds rowName->row-object maps for both sides (via that same native writer for
// assets, or by normalizing before/after JSON dumps) and reports added/removed/changed
// rows with per-cell (column, a, b) detail -- the "did only that row change?" round-trip guard.
// ========================================================================== //

namespace
{
    // A param that may arrive as a JSON object/array, or as a stringified JSON blob.
    // Returns the resolved value (parsing the string form), or nullptr when absent.
    TSharedPtr<FJsonValue> MCPExt_ResolveJsonValue(const TSharedPtr<FJsonValue>& Field)
    {
        if (!Field.IsValid() || Field->Type == EJson::Null)
        {
            return nullptr;
        }
        if (Field->Type == EJson::String)
        {
            const FString S = Field->AsString();
            TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(S);
            TSharedPtr<FJsonValue> Parsed;
            if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
            {
                return Parsed;
            }
            return Field; // leave as string -> the normalizer reports the shape error
        }
        return Field;
    }

    // Build a rowName -> row-object map from a live UDataTable using the native JSON writer.
    void MCPExt_BuildRowMapFromTable(UDataTable* Table, TMap<FString, TSharedPtr<FJsonObject>>& Out)
    {
        if (!Table || !Table->RowStruct)
        {
            return;
        }
        for (const auto& Pair : Table->GetRowMap())
        {
            Out.Add(Pair.Key.ToString(),
                MCPExt_ParseJsonObject(MCPExt_WriteRowAsNativeJsonString(Table, Pair.Value)));
        }
    }

    // Normalize the many shapes a "table as JSON" can arrive in into rowName -> row-object:
    //   * { "RowA": {col:val,...}, "RowB": {...} }                (native table map form)
    //   * { "rows": [ {row_name, row_data:{...}}, ... ] }         (get_data_table_rows payload)
    //   * [ {row_name, row_data:{...}}, ... ]                     (that payload's rows array)
    //   * [ {"Name":"RowA", ...cols}, ... ]                       (UE GetTableAsJSON array form)
    bool MCPExt_NormalizeTableJson(const TSharedPtr<FJsonValue>& In,
        TMap<FString, TSharedPtr<FJsonObject>>& Out, FString& OutErr)
    {
        if (!In.IsValid())
        {
            OutErr = TEXT("value is null");
            return false;
        }

        if (In->Type == EJson::Array)
        {
            int32 Index = 0;
            for (const TSharedPtr<FJsonValue>& El : In->AsArray())
            {
                if (!El.IsValid() || El->Type != EJson::Object)
                {
                    ++Index;
                    continue;
                }
                const TSharedPtr<FJsonObject> Elem = El->AsObject();
                FString RowName;
                TSharedPtr<FJsonObject> RowData;

                const TSharedPtr<FJsonObject>* RowDataPtr = nullptr;
                if (Elem->TryGetObjectField(TEXT("row_data"), RowDataPtr) && RowDataPtr)
                {
                    // { row_name, row_data:{...} } wrapper element
                    if (!Elem->TryGetStringField(TEXT("row_name"), RowName))
                    {
                        Elem->TryGetStringField(TEXT("Name"), RowName);
                    }
                    RowData = *RowDataPtr;
                }
                else
                {
                    // Native array element: a name key inline, everything else is a column.
                    for (const TCHAR* NameKey : { TEXT("Name"), TEXT("RowName"), TEXT("row_name") })
                    {
                        if (Elem->TryGetStringField(NameKey, RowName))
                        {
                            break;
                        }
                    }
                    RowData = MakeShared<FJsonObject>(*Elem);
                    RowData->RemoveField(TEXT("Name"));
                    RowData->RemoveField(TEXT("RowName"));
                    RowData->RemoveField(TEXT("row_name"));
                }

                if (RowName.IsEmpty())
                {
                    RowName = FString::Printf(TEXT("[%d]"), Index);
                }
                if (!RowData.IsValid())
                {
                    RowData = MakeShared<FJsonObject>();
                }
                Out.Add(RowName, RowData);
                ++Index;
            }
            return true;
        }

        if (In->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> RootObj = In->AsObject();

            // Wrapper: { rows:[...] } -> recurse on the array.
            const TArray<TSharedPtr<FJsonValue>>* RowsArr = nullptr;
            if (RootObj->TryGetArrayField(TEXT("rows"), RowsArr) && RowsArr)
            {
                return MCPExt_NormalizeTableJson(MakeShared<FJsonValueArray>(*RowsArr), Out, OutErr);
            }

            // Map form: each object-valued field is a row keyed by its field name.
            // (FString key type matches the codebase's ->Values iteration convention.)
            for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : RootObj->Values)
            {
                if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
                {
                    Out.Add(Pair.Key, Pair.Value->AsObject());
                }
            }
            return true;
        }

        OutErr = TEXT("expected a JSON object or array");
        return false;
    }

    // Cell-level diff of two row objects. Emits [{column, a, b}] for every column whose
    // value differs (missing on one side -> null). Column order is sorted for determinism.
    void MCPExt_DiffRowObjects(const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B,
        TArray<TSharedPtr<FJsonValue>>& OutCells)
    {
        TSet<FString> KeySet;
        if (A.IsValid()) { for (const TPair<FString, TSharedPtr<FJsonValue>>& P : A->Values) { KeySet.Add(P.Key); } }
        if (B.IsValid()) { for (const TPair<FString, TSharedPtr<FJsonValue>>& P : B->Values) { KeySet.Add(P.Key); } }
        TArray<FString> Keys = KeySet.Array();
        Keys.Sort();

        for (const FString& Key : Keys)
        {
            const TSharedPtr<FJsonValue> Av = A.IsValid() ? A->TryGetField(Key) : nullptr;
            const TSharedPtr<FJsonValue> Bv = B.IsValid() ? B->TryGetField(Key) : nullptr;

            bool bEqual;
            if (Av.IsValid() && Bv.IsValid())
            {
                bEqual = FJsonValue::CompareEqual(*Av, *Bv);
            }
            else
            {
                bEqual = (!Av.IsValid() && !Bv.IsValid());
            }
            if (bEqual)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Cell = MakeShared<FJsonObject>();
            Cell->SetStringField(TEXT("column"), Key);
            Cell->SetField(TEXT("a"), Av.IsValid() ? Av : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
            Cell->SetField(TEXT("b"), Bv.IsValid() ? Bv : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
            OutCells.Add(MakeShared<FJsonValueObject>(Cell));
        }
    }

    // Full diff of two rowName -> row-object maps. A = "before"/a, B = "after"/b.
    TSharedPtr<FJsonObject> MCPExt_DiffRowMaps(
        const TMap<FString, TSharedPtr<FJsonObject>>& A,
        const TMap<FString, TSharedPtr<FJsonObject>>& B)
    {
        TArray<TSharedPtr<FJsonValue>> AddedRows, RemovedRows, ChangedRows;
        int32 UnchangedCount = 0;

        // Deterministic ordering: walk A's keys sorted, then B's added keys sorted.
        TArray<FString> AKeys; A.GetKeys(AKeys); AKeys.Sort();
        for (const FString& Key : AKeys)
        {
            const TSharedPtr<FJsonObject>* BRow = B.Find(Key);
            if (!BRow)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("row_name"), Key);
                Entry->SetObjectField(TEXT("row_data"), A[Key]);
                RemovedRows.Add(MakeShared<FJsonValueObject>(Entry));
                continue;
            }

            TArray<TSharedPtr<FJsonValue>> ChangedCells;
            MCPExt_DiffRowObjects(A[Key], *BRow, ChangedCells);
            if (ChangedCells.Num() > 0)
            {
                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("row_name"), Key);
                Entry->SetArrayField(TEXT("changed_cells"), ChangedCells);
                Entry->SetNumberField(TEXT("changed_cell_count"), ChangedCells.Num());
                ChangedRows.Add(MakeShared<FJsonValueObject>(Entry));
            }
            else
            {
                ++UnchangedCount;
            }
        }

        TArray<FString> BKeys; B.GetKeys(BKeys); BKeys.Sort();
        for (const FString& Key : BKeys)
        {
            if (A.Contains(Key))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("row_name"), Key);
            Entry->SetObjectField(TEXT("row_data"), B[Key]);
            AddedRows.Add(MakeShared<FJsonValueObject>(Entry));
        }

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("added_rows"), AddedRows);
        Result->SetArrayField(TEXT("removed_rows"), RemovedRows);
        Result->SetArrayField(TEXT("changed_rows"), ChangedRows);
        Result->SetNumberField(TEXT("added_count"), AddedRows.Num());
        Result->SetNumberField(TEXT("removed_count"), RemovedRows.Num());
        Result->SetNumberField(TEXT("changed_count"), ChangedRows.Num());
        Result->SetNumberField(TEXT("unchanged_count"), UnchangedCount);
        Result->SetNumberField(TEXT("a_row_count"), A.Num());
        Result->SetNumberField(TEXT("b_row_count"), B.Num());
        Result->SetBoolField(TEXT("identical"),
            AddedRows.Num() == 0 && RemovedRows.Num() == 0 && ChangedRows.Num() == 0);
        return Result;
    }
} // namespace

// MCP:read_datatable_from_package_file - Read a DataTable from any .uasset file on disk (e.g. a backup) under a throwaway mount, so a live asset with the same /Game name is untouched; then unmounts and, by default, garbage-collects. Returns table_name, row_struct, row_count and rows as native JSON, or one row via row_name.
//   @param package_file - Absolute filesystem path to the external .uasset (alias: file_path)
//   @param object_name - Optional name of the DataTable object inside the package (defaults to the first UDataTable / the package's primary asset)
//   @param row_name - Optional single row to return; when omitted every row is returned
//   @param collect_garbage - When true (default) force a GC purge of the throwaway load after reading
//   @param file_path - Alias of package_file (OS path to the .uasset, e.g. "D:/Backup/DT_Items.uasset"); package_file wins when both are given
//   @returns { package_file, mount_root, package_name, table_name, row_struct, row_count, rows:[{row_name,row_data}] } (or, for a single row_name, { ..., row_name, row_json, row_data })
TSharedPtr<FJsonObject> HandleReadDataTableFromPackageFile(const TSharedPtr<FJsonObject>& Params)
{
    FString PackageFile;
    if (!Params->TryGetStringField(TEXT("package_file"), PackageFile) &&
        !Params->TryGetStringField(TEXT("file_path"), PackageFile))
    {
        return mcp_common::MissingParameterReply(TEXT("package_file"));
    }

    PackageFile = FPaths::ConvertRelativePathToFull(PackageFile);
    if (!FPaths::FileExists(PackageFile))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("File not found: %s"), *PackageFile));
    }

    FString ObjectName;
    Params->TryGetStringField(TEXT("object_name"), ObjectName);
    FString RowName;
    const bool bSingleRow = Params->TryGetStringField(TEXT("row_name"), RowName) && !RowName.IsEmpty();
    bool bCollectGarbage = true;
    Params->TryGetBoolField(TEXT("collect_garbage"), bCollectGarbage);

    const FString ContentDir = FPaths::GetPath(PackageFile);
    const FString BaseName   = FPaths::GetBaseFilename(PackageFile);

    // Unique throwaway root so the load never shadows/rebinds the live /Game/... package.
    const FString MountRoot = FString::Printf(TEXT("/MCPExtRead_%s/"),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FString LongPackageName = MountRoot + BaseName;

    FPackageName::RegisterMountPoint(MountRoot, ContentDir);

    TSharedPtr<FJsonObject> Response;
    UPackage* LoadedPkg = nullptr;

    // LOAD_ForDiff: this is a read-only diff-style load; keep it off the live/savable path.
    // DisableCompileOnLoad/EngineVersionChecks tolerate BP row-structs and cross-version files.
    LoadedPkg = LoadPackage(nullptr, *LongPackageName,
        LOAD_ForDiff | LOAD_DisableCompileOnLoad | LOAD_DisableEngineVersionChecks);

    if (!LoadedPkg)
    {
        Response = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Failed to load package from file: %s"), *PackageFile));
    }
    else
    {
        UDataTable* DataTable = nullptr;
        if (!ObjectName.IsEmpty())
        {
            DataTable = FindObject<UDataTable>(LoadedPkg, *ObjectName);
        }
        if (!DataTable)
        {
            ForEachObjectWithPackage(LoadedPkg, [&DataTable](UObject* Obj)
            {
                if (UDataTable* DT = Cast<UDataTable>(Obj))
                {
                    DataTable = DT;
                    return false; // stop
                }
                return true;
            }, EGetObjectsFlags::IncludeNestedObjects);
        }
        if (!DataTable)
        {
            if (UObject* Asset = LoadedPkg->FindAssetInPackage())
            {
                DataTable = Cast<UDataTable>(Asset);
            }
        }

        if (!DataTable)
        {
            Response = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("No UDataTable found in package file: %s"), *PackageFile));
        }
        else if (!DataTable->RowStruct)
        {
            Response = mcp_common::MakeErrorReply(FString::Printf(
                TEXT("DataTable '%s' loaded from file has no resolvable row struct"), *DataTable->GetName()));
        }
        else
        {
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("package_file"), PackageFile);
            ResultObj->SetStringField(TEXT("mount_root"), MountRoot);
            ResultObj->SetStringField(TEXT("package_name"), LongPackageName);
            ResultObj->SetStringField(TEXT("table_name"), DataTable->GetName());
            ResultObj->SetStringField(TEXT("row_struct"), DataTable->RowStruct->GetName());
            ResultObj->SetNumberField(TEXT("row_count"), DataTable->GetRowMap().Num());

            if (bSingleRow)
            {
                const uint8* RowData = DataTable->FindRowUnchecked(FName(*RowName));
                if (!RowData)
                {
                    Response = mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("Row not found: '%s' is not a row in DataTable '%s' (from file)"),
                        *RowName, *DataTable->GetName()));
                }
                else
                {
                    const FString RowJson = MCPExt_WriteRowAsNativeJsonString(DataTable, RowData);
                    ResultObj->SetStringField(TEXT("row_name"), RowName);
                    ResultObj->SetStringField(TEXT("row_json"), RowJson);
                    ResultObj->SetObjectField(TEXT("row_data"), MCPExt_ParseJsonObject(RowJson));
                    Response = mcp_common::MakeOkReply(ResultObj);
                }
            }
            else
            {
                // Every row through the same native writer as the single-row branch, parsed back to JSON.
                TArray<TSharedPtr<FJsonValue>> AllRows;
                AllRows.Reserve(DataTable->GetRowMap().Num());
                for (const TPair<FName, uint8*>& Entry : DataTable->GetRowMap())
                {
                    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
                    Row->SetStringField(TEXT("row_name"), Entry.Key.ToString());
                    Row->SetObjectField(TEXT("row_data"),
                        MCPExt_ParseJsonObject(MCPExt_WriteRowAsNativeJsonString(DataTable, Entry.Value)));
                    AllRows.Add(MakeShared<FJsonValueObject>(Row));
                }
                ResultObj->SetArrayField(TEXT("rows"), AllRows);
                Response = mcp_common::MakeOkReply(ResultObj);
            }
        }
    }

    // Cleanup: clear the keep-flags GC honors in-editor (RF_Standalone) so the throwaway
    // load is reclaimable, drop the private mount, then optionally purge it.
    if (LoadedPkg)
    {
        LoadedPkg->ClearFlags(RF_Standalone | RF_Public);
        ForEachObjectWithPackage(LoadedPkg, [](UObject* Obj)
        {
            Obj->ClearFlags(RF_Standalone | RF_Public);
            return true;
        }, EGetObjectsFlags::IncludeNestedObjects);
        LoadedPkg->ClearDirtyFlag();
    }
    FPackageName::UnRegisterMountPoint(MountRoot, ContentDir);
    if (bCollectGarbage)
    {
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    return Response;
}

// MCP:diff_data_tables - Diff two DataTables row by row and cell by cell without modifying either: two asset paths (a, b), or two JSON dumps (before_json, after) such as get_data_table_rows or export_data_table_as_json output, which win when present. Returns identical, added/removed/changed rows with counts and changed cells as {column, a, b}.
//   @param a - Full asset path of the first DataTable (alias: table_a); asset mode
//   @param b - Full asset path of the second DataTable (alias: table_b); asset mode
//   @param before_json - The 'before'/A table dump as a JSON object/array or stringified JSON (alias: before); JSON mode
//   @param after - The 'after'/B table dump as a JSON object/array or stringified JSON (alias: after_json); JSON mode
//   @param table_a - Alias of a: asset path of the first DataTable, e.g. "/Game/Data/DT_Items"; a wins when both are given
//   @param table_b - Alias of b: asset path of the second DataTable, e.g. "/Game/Data/DT_Items_Old"; b wins when both are given
//   @param before - Alias of before_json: the first (before) table dump; before_json wins when both are given
//   @param after_json - Alias of after: the second (after) table dump; after wins when both are given
//   @returns { mode, identical, added_count, removed_count, changed_count, unchanged_count, a_row_count, b_row_count, added_rows:[{row_name,row_data}], removed_rows:[...], changed_rows:[{row_name,changed_cells:[{column,a,b}]}] }
TSharedPtr<FJsonObject> HandleDiffDataTables(const TSharedPtr<FJsonObject>& Params)
{
    // JSON before/after mode takes precedence when those params are present.
    TSharedPtr<FJsonValue> BeforeField = Params->TryGetField(TEXT("before_json"));
    if (!BeforeField.IsValid()) { BeforeField = Params->TryGetField(TEXT("before")); }
    TSharedPtr<FJsonValue> AfterField = Params->TryGetField(TEXT("after"));
    if (!AfterField.IsValid()) { AfterField = Params->TryGetField(TEXT("after_json")); }

    const bool bHasBefore = BeforeField.IsValid() && BeforeField->Type != EJson::Null;
    const bool bHasAfter  = AfterField.IsValid()  && AfterField->Type  != EJson::Null;

    if (bHasBefore || bHasAfter)
    {
        if (!bHasBefore || !bHasAfter)
        {
            return mcp_common::MakeErrorReply(
                TEXT("JSON diff mode requires BOTH 'before_json' and 'after'"));
        }

        TMap<FString, TSharedPtr<FJsonObject>> MapA, MapB;
        FString ErrA, ErrB;
        if (!MCPExt_NormalizeTableJson(MCPExt_ResolveJsonValue(BeforeField), MapA, ErrA))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("before_json: %s"), *ErrA));
        }
        if (!MCPExt_NormalizeTableJson(MCPExt_ResolveJsonValue(AfterField), MapB, ErrB))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("after: %s"), *ErrB));
        }

        TSharedPtr<FJsonObject> Diff = MCPExt_DiffRowMaps(MapA, MapB);
        Diff->SetStringField(TEXT("mode"), TEXT("json"));
        return mcp_common::MakeOkReply(Diff);
    }

    // Asset mode: two /Game DataTable paths.
    FString PathA, PathB;
    if (!Params->TryGetStringField(TEXT("a"), PathA))
    {
        Params->TryGetStringField(TEXT("table_a"), PathA);
    }
    if (!Params->TryGetStringField(TEXT("b"), PathB))
    {
        Params->TryGetStringField(TEXT("table_b"), PathB);
    }

    if (PathA.IsEmpty() || PathB.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide two DataTable asset paths (a/table_a, b/table_b) or two JSON dumps (before_json, after)"));
    }

    UDataTable* TableA = LoadDataTableAt(PathA);
    if (!TableA)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("DataTable not found: %s"), *PathA));
    }
    UDataTable* TableB = LoadDataTableAt(PathB);
    if (!TableB)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("DataTable not found: %s"), *PathB));
    }
    if (!TableA->RowStruct)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("DataTable has no row struct: %s"), *PathA));
    }
    if (!TableB->RowStruct)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("DataTable has no row struct: %s"), *PathB));
    }

    TMap<FString, TSharedPtr<FJsonObject>> MapA, MapB;
    MCPExt_BuildRowMapFromTable(TableA, MapA);
    MCPExt_BuildRowMapFromTable(TableB, MapB);

    TSharedPtr<FJsonObject> Diff = MCPExt_DiffRowMaps(MapA, MapB);
    Diff->SetStringField(TEXT("mode"), TEXT("assets"));
    Diff->SetStringField(TEXT("table_a"), PathA);
    Diff->SetStringField(TEXT("table_b"), PathB);
    Diff->SetStringField(TEXT("row_struct_a"), TableA->RowStruct->GetName());
    Diff->SetStringField(TEXT("row_struct_b"), TableB->RowStruct->GetName());
    Diff->SetBoolField(TEXT("row_struct_mismatch"), TableA->RowStruct != TableB->RowStruct);
    return mcp_common::MakeOkReply(Diff);
}

// ==========================================================================
// Whole-column read (ColumnExportText per row) + native whole-table JSON/CSV
// export (UEAgentDataAssetCommandsImpl::ExportWholeTable).
// ==========================================================================

// MCP:get_data_table_column - Read one column across every row of a DataTable as ExportText strings, so TMaps, soft references and tags come out flat and intact; read-only. Returns row_struct, row_count and values as {row_name: text}. Use it to verify cell edits; an unknown column is an error.
//   @param table_path - Full asset path of the DataTable.
//   @param data_table - Alias of table_path: pass one of the two.
//   @param column - Name of the column (a field of the row struct, by property or display name) to read across every row
//   @returns { table_path, column, row_struct, row_count, values:{ rowName: exporttext-string } }
TSharedPtr<FJsonObject> HandleGetDataTableColumn(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString RequestedTable;
    if (!Params->TryGetStringField(TEXT("data_table"), RequestedTable) &&
        !Params->TryGetStringField(TEXT("table_path"), RequestedTable))
    {
        return mcp_common::MissingParameterReply(TEXT("table_path"));
    }
    FString Column;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("column"), Column, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    TSharedPtr<FJsonObject> Refusal;
    UDataTable* Table = Impl::OpenTable(RequestedTable, /*bNeedRowStruct*/ true, Refusal);
    if (Table == nullptr)
    {
        return Refusal;
    }

    // The column has to be a field of the row struct, found the way the field writers find it. A miss is
    // an error, never a silently empty column.
    FProperty* Field = Impl::FindColumn(Table, Column);
    if (Field == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Column not found: '%s' is not a field of row struct '%s' in DataTable '%s'"),
            *Column, *Table->RowStruct->GetName(), *RequestedTable));
    }

    // One ExportText string per row. The row map holds row CONTAINER pointers: ExportText_InContainer
    // (inside ColumnExportText) applies the property offset itself.
    const TMap<FName, uint8*>& RowMap = Table->GetRowMap();
    TSharedPtr<FJsonObject> Cells = MakeShared<FJsonObject>();
    for (const TPair<FName, uint8*>& Entry : RowMap)
    {
        Cells->SetStringField(Entry.Key.ToString(), ColumnExportText(Field, Entry.Value));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("table_path"), RequestedTable);
    Reply->SetStringField(TEXT("column"), Column);
    Reply->SetStringField(TEXT("row_struct"), Table->RowStruct->GetName());
    Reply->SetNumberField(TEXT("row_count"), RowMap.Num());
    Reply->SetObjectField(TEXT("values"), Cells);
    return mcp_common::MakeOkReply(Reply);
}

// MCP:export_data_table_as_json - Export a whole DataTable to JSON with the native exporter (read-only), so TMap<Enum,SoftClass>, soft references and GameplayTags survive, unlike get_data_table_rows. Returns row_struct, row_count and the json string (an array of row objects), optionally also written to a UTF-8 file.
//   @param table_path - Full asset path of the DataTable.
//   @param data_table - Alias of table_path: pass one of the two.
//   @param file_path - Optional OS path to also write the JSON string to (echoed back as written_to)
//   @returns { table_path, row_struct, row_count, json, written_to? }
TSharedPtr<FJsonObject> HandleExportDataTableAsJson(const TSharedPtr<FJsonObject>& Params)
{
    using UEAgentDataAssetCommandsImpl::ExportWholeTable;
    // GetTableAsJSON (array-of-objects form), the whole-table counterpart of the per-row WriteRowAsJSON.
    return ExportWholeTable(Params, /*bAsCsv*/ false);
}

// MCP:export_data_table_as_csv - Export a whole DataTable to CSV with the native exporter (read-only), returning row_struct, row_count and the csv string, optionally also written to a UTF-8 file. Use it as the backup before import_data_table_csv; nested structs and TMaps do not survive CSV cleanly, so export_data_table_as_json is the faithful copy.
//   @param table_path - Full asset path of the DataTable.
//   @param data_table - Alias of table_path: pass one of the two.
//   @param file_path - Optional OS path to also write the CSV string to (echoed back as written_to)
//   @returns { table_path, row_struct, row_count, csv, written_to? }
TSharedPtr<FJsonObject> HandleExportDataTableAsCsv(const TSharedPtr<FJsonObject>& Params)
{
    using UEAgentDataAssetCommandsImpl::ExportWholeTable;
    // GetTableAsCSV: flat by nature, the spreadsheet view and the backup before import_data_table_csv.
    return ExportWholeTable(Params, /*bAsCsv*/ true);
}

// ==========================================================================
// set_data_table_cell: direct in-place FProperty write of ONE sub-field of ONE
// element of an array-of-structs column. The struct-field lookup below extends
// UDataTable::FindTableProperty's authored-name fallback to the element struct.
// ==========================================================================

namespace
{
    // Generalization of UDataTable::FindTableProperty's UserDefinedStruct authored-name fallback
    // (DataTable.cpp) to an ARBITRARY UStruct -- needed because a row struct's ARRAY
    // ELEMENT struct is often itself a
    // Blueprint UUserDefinedStruct with GUID-suffixed internal property names, so a bare
    // FindPropertyByName on the element struct would reproduce the same "friendly name" problem
    // one level deeper.
    FProperty* FindStructPropertyByNameOrDisplayName(UStruct* Struct, FName PropertyName)
    {
        if (!Struct)
        {
            return nullptr;
        }
        FProperty* Property = Struct->FindPropertyByName(PropertyName);
        if (!Property && Struct->IsA<UUserDefinedStruct>())
        {
            const FString PropertyNameStr = PropertyName.ToString();
            for (TFieldIterator<FProperty> It(Struct); It; ++It)
            {
                if (PropertyNameStr == Struct->GetAuthoredNameForField(*It))
                {
                    Property = *It;
                    break;
                }
            }
        }
        return Property;
    }
}

// MCP:set_data_table_cell - Set one element of an array column in a DataTable row (a field of a struct element, or a scalar element) by direct ImportText, bypassing the JSON/CSV importer, and save. Returns before_value, after_value, write_verified; an unregistered GameplayTag still becomes None (write_verified:false) and is saved anyway.
//   @param table_path - Full asset path of the DataTable (data_table also accepted)
//   @param row_name - Name (key) of the row to edit
//   @param column_name - Name of the row-struct field holding the array (display name or authored/internal name both accepted, same fallback as get_data_table_column)
//   @param row_index - Index into that array to edit (0-based; errors honestly if out of range, never a silent OOB read/write)
//   @param cell_property - Name of the sub-field on the array element STRUCT to set (display or authored name); omit/empty for a scalar array (e.g. TArray<FGameplayTag>/TArray<FName>) to edit the element itself
//   @param value - ExportText/ImportText string to write (e.g. "Material.Bone" for a bare FGameplayTag value, or an enum's internal OR display name)
//   @param data_table - Alias of table_path (the DataTable asset path, e.g. "/Game/Data/DT_Items"); table_path wins when both are given
//   @returns { table_path, row_name, column_name, row_index, cell_property, resolved_property, array_length, before_value, after_value, changed, already_set, import_succeeded, write_verified, success, warning?, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetDataTableCell(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentDataAssetCommandsImpl;

    FString TablePath;
    if (!Params->TryGetStringField(TEXT("table_path"), TablePath) &&
        !Params->TryGetStringField(TEXT("data_table"), TablePath))
    {
        return mcp_common::MissingParameterReply(TEXT("table_path"));
    }
    FString RowName, ColumnName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("column_name"), ColumnName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    double RowIndexD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("row_index"), RowIndexD)) { return Missing; }
    const int32 RowIndex = static_cast<int32>(RowIndexD);
    // Optional: empty means the array element itself is the value (a scalar array).
    FString CellProperty;
    Params->TryGetStringField(TEXT("cell_property"), CellProperty);
    FString Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    TSharedPtr<FJsonObject> Refusal;
    UDataTable* DataTable = Impl::OpenTable(TablePath, /*bNeedRowStruct*/ true, Refusal);
    if (DataTable == nullptr)
    {
        return Refusal;
    }
    const FName RowFName(*RowName);
    uint8* RowMem = DataTable->FindRowUnchecked(RowFName);
    if (RowMem == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Row not found: ") + RowName);
    }

    FProperty* ColProp = Impl::FindColumn(DataTable, ColumnName);
    if (!ColProp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Column not found: '%s' is not a field of row struct '%s' in DataTable '%s'"),
            *ColumnName, *DataTable->RowStruct->GetName(), *TablePath));
    }
    FArrayProperty* ArrayProp = CastField<FArrayProperty>(ColProp);
    if (!ArrayProp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Column '%s' is not an array (actual type: %s); set_data_table_cell only edits array-of-struct(s) columns"),
            *ColumnName, *ColProp->GetCPPType(nullptr, 0)));
    }

    // Bounds-check BEFORE calling FScriptArrayHelper::GetRawPtr -- it does NOT bounds-check
    // internally, so an unchecked index would read/write out-of-bounds memory.
    void* ArrayValuePtr = ArrayProp->ContainerPtrToValuePtr<void>(RowMem);
    FScriptArrayHelper ArrayHelper(ArrayProp, ArrayValuePtr);
    const int32 ArrayLength = ArrayHelper.Num();
    if (RowIndex < 0 || RowIndex >= ArrayLength)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("row_index %d out of range (column '%s' has %d element(s) on row '%s')"),
            RowIndex, *ColumnName, ArrayLength, *RowName));
    }
    uint8* ElementPtr = ArrayHelper.GetRawPtr(RowIndex);

    FProperty* TargetProperty = nullptr;
    void* TargetValuePtr = nullptr;
    FString ResolvedPropertyName;
    if (CellProperty.IsEmpty())
    {
        // Scalar array element (e.g. TArray<FGameplayTag>, TArray<FName>): the element IS the value.
        TargetProperty = ArrayProp->Inner;
        TargetValuePtr = ElementPtr;
        ResolvedPropertyName = TargetProperty ? TargetProperty->GetName() : FString();
    }
    else
    {
        FStructProperty* InnerStruct = CastField<FStructProperty>(ArrayProp->Inner);
        if (!InnerStruct || !InnerStruct->Struct)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("cell_property ('%s') given but column '%s' elements are not a struct (Inner type: %s); omit cell_property for a scalar array"),
                *CellProperty, *ColumnName, *ArrayProp->Inner->GetCPPType(nullptr, 0)));
        }
        TargetProperty = FindStructPropertyByNameOrDisplayName(InnerStruct->Struct, FName(*CellProperty));
        if (!TargetProperty)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("cell_property not found: '%s' is not a field of struct '%s' (column '%s', row_index %d)"),
                *CellProperty, *InnerStruct->Struct->GetName(), *ColumnName, RowIndex));
        }
        TargetValuePtr = TargetProperty->ContainerPtrToValuePtr<void>(ElementPtr);
        ResolvedPropertyName = TargetProperty->GetName();
    }

    // Capture BEFORE via ExportText on the bare value ptr (NOT ExportText_InContainer -- there is
    // no enclosing container here, TargetValuePtr already IS the value).
    FString BeforeValue;
    TargetProperty->ExportText_Direct(BeforeValue, TargetValuePtr, TargetValuePtr, nullptr, PPF_None);

    // Mutate in-place. Modify() + BroadcastPreChange BEFORE the write (mirrors
    // HandleSetDataTableRowFields' persistence bracket) so undo/editor-refresh see the
    // pre-mutation state, not a half-written one.
    DataTable->Modify();
    FDataTableEditorUtils::BroadcastPreChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowData);

    // DataTableUtils::AssignStringToPropertyDirect (NOT a raw FProperty::ImportText_Direct call):
    // for enum/byte properties it tries the enum's DISPLAY name before falling back to the raw
    // internal entry identifier (UUserDefinedEnum entries are "NewEnumeratorN" internally); for
    // everything else -- including FGameplayTag, which is a plain FStructProperty -- it falls
    // straight through to the property's own ImportText_Direct, so tags/numbers/strings/soft refs
    // behave identically to a raw ImportText_Direct call. This never goes through the CSV/JSON
    // import factory, so it cannot trigger that path's editor modal-dialog risk either.
    const FString ImportErrorText = DataTableUtils::AssignStringToPropertyDirect(Value, TargetProperty, static_cast<uint8*>(TargetValuePtr));
    const bool bImportSucceeded = ImportErrorText.IsEmpty();

    FDataTableEditorUtils::BroadcastPostChange(DataTable, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
    DataTable->MarkPackageDirty();

    FString AfterValue;
    TargetProperty->ExportText_Direct(AfterValue, TargetValuePtr, TargetValuePtr, nullptr, PPF_None);

    const bool bChanged = (BeforeValue != AfterValue);

    // Honest post-condition check -- the whole reason this tool exists: UGameplayTagsManager::
    // ImportSingleGameplayTag silently resets TagName to NAME_None (no error text at all) when
    // bImportFromSerialize is false (true for every DataTable-import code path AND for this direct
    // call, since we deliberately do not pass PPF_SerializedAsImportText) and the tag string does
    // not resolve via FindTagNode. A blanket "ImportError.IsEmpty() => success" would silently
    // re-introduce exactly the silent reset this tool exists to catch. Instead: verify the AFTER value
    // actually reflects what was asked for (loose, property-type-agnostic normalized comparison --
    // strips the FStructProperty parenthesized-form wrapper and quotes so "Material.Bone"
    // matches an ExportText of (TagName="Material.Bone") either way).
    auto NormalizeForCompare = [](FString S) -> FString
    {
        S.TrimStartAndEndInline();
        if (S.Len() >= 2 && S.StartsWith(TEXT("(")) && S.EndsWith(TEXT(")")))
        {
            S = S.Mid(1, S.Len() - 2);
        }
        S = S.Replace(TEXT("\""), TEXT(""));
        return S;
    };
    const FString NormAfter = NormalizeForCompare(AfterValue);
    const FString NormValue = NormalizeForCompare(Value);
    bool bWriteVerified;
    if (NormValue.IsEmpty() || NormValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
    {
        bWriteVerified = NormAfter.IsEmpty() || NormAfter.Equals(TEXT("None"), ESearchCase::IgnoreCase);
    }
    else
    {
        bWriteVerified = NormAfter.Contains(NormValue, ESearchCase::IgnoreCase) || NormValue.Contains(NormAfter, ESearchCase::IgnoreCase);
    }

    const bool bAlreadySet = !bChanged && bWriteVerified;
    const bool bSuccess = bImportSucceeded && bWriteVerified;

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(DataTable->GetOutermost(), DataTable, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("table_path"), TablePath);
    ResultObj->SetStringField(TEXT("row_name"), RowName);
    ResultObj->SetStringField(TEXT("column_name"), ColumnName);
    ResultObj->SetNumberField(TEXT("row_index"), RowIndex);
    ResultObj->SetStringField(TEXT("cell_property"), CellProperty);
    ResultObj->SetStringField(TEXT("resolved_property"), ResolvedPropertyName);
    ResultObj->SetNumberField(TEXT("array_length"), ArrayLength);
    ResultObj->SetStringField(TEXT("before_value"), BeforeValue);
    ResultObj->SetStringField(TEXT("after_value"), AfterValue);
    ResultObj->SetBoolField(TEXT("changed"), bChanged);
    ResultObj->SetBoolField(TEXT("already_set"), bAlreadySet);
    ResultObj->SetBoolField(TEXT("import_succeeded"), bImportSucceeded);
    ResultObj->SetBoolField(TEXT("write_verified"), bWriteVerified);
    ResultObj->SetBoolField(TEXT("success"), bSuccess);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    if (!ImportErrorText.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("import_error"), ImportErrorText);
    }
    if (!bWriteVerified)
    {
        ResultObj->SetStringField(TEXT("warning"), FString::Printf(
            TEXT("Requested value '%s' does not appear in the resulting after_value ('%s') despite import_succeeded=%s. This is the silent-reset signature this tool guards against (e.g. an unregistered/malformed FGameplayTag reset to None with zero engine diagnostics) -- treat this write as FAILED regardless of import_succeeded."),
            *Value, *AfterValue, bImportSucceeded ? TEXT("true") : TEXT("false")));
    }
    if (!bSaved && !SaveError.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("save_error"), SaveError);
    }

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    // The verdict is BOTH things: the operation succeeded AND it persisted.
    // If the file cannot be written (read-only, or locked by source control) the
    // save fails and the change stays in memory only; that is not a success.
    return mcp_common::MakeOutcomeReply(ResultObj, (bSuccess) && bSaved,
        ((bSuccess) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("set_data_table_cell")));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_curve_key"), &HandleAddCurveKey },
    { TEXT("add_data_table_row"), &HandleAddDataTableRow },
    { TEXT("create_composite_data_table"), &HandleCreateCompositeDataTable },
    { TEXT("create_curve_asset"), &HandleCreateCurveAsset },
    { TEXT("create_data_asset"), &HandleCreateDataAsset },
    { TEXT("create_data_table"), &HandleCreateDataTable },
    { TEXT("delete_data_table_row"), &HandleDeleteDataTableRow },
    { TEXT("diff_data_tables"), &HandleDiffDataTables },
    { TEXT("export_data_table_as_csv"), &HandleExportDataTableAsCsv },
    { TEXT("export_data_table_as_json"), &HandleExportDataTableAsJson },
    { TEXT("get_curve_keys"), &HandleGetCurveKeys },
    { TEXT("get_data_asset_info"), &HandleGetDataAssetInfo },
    { TEXT("get_data_table_column"), &HandleGetDataTableColumn },
    { TEXT("get_data_table_info"), &HandleGetDataTableInfo },
    { TEXT("get_data_table_row"), &HandleGetDataTableRow },
    { TEXT("get_data_table_row_as_json"), &HandleGetDataTableRowAsJson },
    { TEXT("get_data_table_rows"), &HandleGetDataTableRows },
    { TEXT("import_data_table_csv"), &HandleImportDataTableCSV },
    { TEXT("read_data_table_row_complex"), &HandleReadDataTableRowComplex },
    { TEXT("read_datatable_from_package_file"), &HandleReadDataTableFromPackageFile },
    { TEXT("remove_curve_key"), &HandleRemoveCurveKey },
    { TEXT("rename_data_table_row"), &HandleRenameDataTableRow },
    { TEXT("set_data_asset_property"), &HandleSetDataAssetProperty },
    { TEXT("set_data_table_cell"), &HandleSetDataTableCell },
    { TEXT("set_data_table_row"), &HandleSetDataTableRow },
    { TEXT("set_data_table_row_fields"), &HandleSetDataTableRowFields },
    { TEXT("set_data_table_row_from_json"), &HandleSetDataTableRowFromJson },
    { TEXT("set_data_table_rows_from_json"), &HandleSetDataTableRowsFromJson },
};
} // namespace UEAgentDataAssetCommands
