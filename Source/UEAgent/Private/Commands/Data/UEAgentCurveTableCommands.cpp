#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Data/DataAuthoringUtils.h"

#include "Engine/CurveTable.h"            // UCurveTable, ECurveTableMode
#include "Engine/CompositeCurveTable.h"   // UCompositeCurveTable
#include "Curves/RichCurve.h"             // FRichCurve / FRichCurveKey
#include "Curves/RealCurve.h"             // FRealCurve / ERichCurveInterpMode

#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"           // FArrayProperty / FObjectProperty (reflection)
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

/**
 * Curve table commands.
 *
 * Authors UCurveTable assets (a table of named FRichCurve rows) and UCompositeCurveTable assets that stack
 * parent UCurveTables. Distinct from create_curve_asset, which makes a standalone UCurveFloat /
 * UCurveVector / UCurveLinearColor.
 *
 * Engine APIs (Engine module):
 *   - UCurveTable: GetRowMap() -> const TMap<FName, FRealCurve*>& in any mode, GetRichCurveRowMap()
 *     only when the mode is not SimpleCurves, AddRichCurve(FName) -> FRichCurve&, GetCurveTableMode().
 *   - UCompositeCurveTable::AppendParentTables(); ParentTables is protected, so it is read via reflection.
 *   - FRichCurve::UpdateOrAddKey(Time, Value) -> FKeyHandle.
 */
namespace UEAgentCurveTableCommands
{

// MCP-DOMAIN: DataAsset

namespace
{
    UCurveTable* LoadCurveTableAt(const FString& Path)
    {
        return Cast<UCurveTable>(StaticLoadObject(UCurveTable::StaticClass(), nullptr, *Path));
    }

    const TCHAR* CurveTableModeToString(ECurveTableMode Mode)
    {
        switch (Mode)
        {
            case ECurveTableMode::Empty:        return TEXT("Empty");
            case ECurveTableMode::SimpleCurves: return TEXT("SimpleCurves");
            case ECurveTableMode::RichCurves:   return TEXT("RichCurves");
            default:                            return TEXT("Unknown");
        }
    }

    // Number of keys in a row's curve. RowMap stores FRealCurve* whose GetNumKeys()
    // is virtual (FRichCurve overrides it), so this works for both modes.
    int32 RowKeyCount(const FRealCurve* Curve)
    {
        return Curve ? Curve->GetNumKeys() : 0;
    }

    // Read the protected UCompositeCurveTable::ParentTables array via reflection so
    // we can report a verified parent_count without linking against the member.
    int32 ReadParentTableCount(UCompositeCurveTable* Composite)
    {
        if (!Composite)
        {
            return 0;
        }
        FArrayProperty* ArrayProp = FindFProperty<FArrayProperty>(
            UCompositeCurveTable::StaticClass(), TEXT("ParentTables"));
        if (!ArrayProp)
        {
            return 0;
        }
        const void* ArrayPtr = ArrayProp->ContainerPtrToValuePtr<void>(Composite);
        FScriptArrayHelper Helper(ArrayProp, ArrayPtr);
        return Helper.Num();
    }
}

// Steps the curve table handlers share.
namespace UEAgentCurveTableCommandsImpl
{
    TSharedPtr<FJsonObject> CurveTableNotFound(const FString& Path)
    {
        return mcp_common::MakeErrorReply(TEXT("CurveTable not found: ") + Path);
    }

    // The table at Path, refused for RefusedEdit when it holds simple curves: AddRichCurve check()s that mode, so a table
    // imported as simple curves cannot take rich-curve rows or keys.
    TSharedPtr<FJsonObject> OpenRichCurveTable(const FString& Path, const TCHAR* RefusedEdit, UCurveTable*& OutTable)
    {
        OutTable = LoadCurveTableAt(Path);
        if (OutTable == nullptr)
        {
            return CurveTableNotFound(Path);
        }
        if (OutTable->GetCurveTableMode() == ECurveTableMode::SimpleCurves)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("CurveTable is in SimpleCurves mode; cannot %s"), RefusedEdit));
        }
        return nullptr;
    }

    // Tells the editor Table changed (broadcast + PostEditChange), then dirties and saves it; false when the save fails.
    bool SaveTableChange(UCurveTable* Table, FString& OutSaveError)
    {
        Table->OnCurveTableChanged().Broadcast();
        Table->PostEditChange();
        Table->MarkPackageDirty();
        return mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, OutSaveError);
    }

    TSharedPtr<FJsonObject> RowReply(const FString& TablePath, const FString& RowName)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("curve_table_path"), TablePath);
        Reply->SetStringField(TEXT("row_name"), RowName);
        return Reply;
    }

    void AddRowCountAndMode(const TSharedPtr<FJsonObject>& Reply, const UCurveTable& Table)
    {
        Reply->SetNumberField(TEXT("row_count"), Table.GetRowMap().Num());
        Reply->SetStringField(TEXT("curve_table_mode"), CurveTableModeToString(Table.GetCurveTableMode()));
    }

    // The reply of a create command before its outcome: the request, then the table the path holds.
    TSharedPtr<FJsonObject> CreatedTableReply(const FString& Name, const FString& PackagePath, const UCurveTable& Table,
        TOptional<int32> ParentCount = TOptional<int32>())
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("asset_path"), Table.GetPathName());
        Reply->SetStringField(TEXT("curve_table_path"), Table.GetPathName());
        Reply->SetStringField(TEXT("curve_class"), Table.GetClass()->GetName());
        if (ParentCount.IsSet())
        {
            Reply->SetNumberField(TEXT("parent_count"), ParentCount.GetValue());
        }
        AddRowCountAndMode(Reply, Table);
        return Reply;
    }
}

// --------------------------------------------------------------------------- //
// create_curve_table
// --------------------------------------------------------------------------- //
// MCP:create_curve_table - Create and save a new UCurveTable asset (a table of named rich-curve rows).
//   @param name - Name of the new CurveTable asset.
//   @param package_path - Content folder for the asset; defaults to /Game/CurveTables
//   @returns { name: string, package_path: string, asset_path: string, curve_table_path: string, curve_class: string, row_count: int, curve_table_mode: string, created: bool, already_exists: bool, saved: bool, message: string }

TSharedPtr<FJsonObject> HandleCreateCurveTable(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentCurveTableCommandsImpl;

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/CurveTables");
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load it and return a STABLE payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UCurveTable* Existing = LoadCurveTableAt(FullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ExistObj = Impl::CreatedTableReply(Name, PackagePath, *Existing);
            DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj);
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset already exists at %s but could not be loaded as UCurveTable"), *FullPath));
    }

    FString CreateError;
    UCurveTable* NewTable = mcp_common::NewAssetInPackage<UCurveTable>(FullPath, Name, TEXT("Failed to create UCurveTable"), CreateError);
    if (!NewTable)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewTable);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewTable->GetOutermost(), NewTable, SaveError);

    TSharedPtr<FJsonObject> ResultObj = Impl::CreatedTableReply(Name, PackagePath, *NewTable);
    DataAuthoringUtils::AddCreatedOutcome(ResultObj, bSaved, TEXT("CurveTable"), SaveError);

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_curve_table"), TEXT("bSaved")));
}

// --------------------------------------------------------------------------- //
// add_curve_table_row
// --------------------------------------------------------------------------- //
// MCP:add_curve_table_row - Add (or replace) a named FRichCurve row in a UCurveTable, optionally seeding keys.
//   @param curve_table_path - Full asset path to the UCurveTable.
//   @param row_name - Name of the row (rich curve) to add.
//   @param keys - Optional [[time, value], ...] array of seed keys
//   @returns { curve_table_path: string, row_name: string, row_added: bool, keys_added: int, key_count: int, row_count: int, curve_table_mode: string, saved: bool, keys: [{ time, value }] }

TSharedPtr<FJsonObject> HandleAddCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentCurveTableCommandsImpl;

    FString TablePath, RowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UCurveTable* Table = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::OpenRichCurveTable(TablePath, TEXT("add a rich-curve row"), Table))
    {
        return Failure;
    }

    const FName RowFName(*RowName);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddCurveTableRow", "Add Curve Table Row"));
    Table->Modify();

    // AddRichCurve adds (or replaces) the named row and returns the live FRichCurve&.
    FRichCurve& Curve = Table->AddRichCurve(RowFName);

    // Optional initial keys: [[time, value], ...].
    int32 KeysAdded = 0;
    const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
    if (Params->TryGetArrayField(TEXT("keys"), KeysArray) && KeysArray)
    {
        for (const TSharedPtr<FJsonValue>& KeyVal : *KeysArray)
        {
            const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
            if (KeyVal.IsValid() && KeyVal->TryGetArray(Pair) && Pair && Pair->Num() >= 2)
            {
                const float Time = static_cast<float>((*Pair)[0]->AsNumber());
                const float Value = static_cast<float>((*Pair)[1]->AsNumber());
                Curve.UpdateOrAddKey(Time, Value);
                ++KeysAdded;
            }
        }
    }

    FString SaveError;
    const bool bSaved = Impl::SaveTableChange(Table, SaveError);

    // Verify by readback: row is present in the RowMap and its key count matches.
    const FRealCurve* WrittenCurve = Table->FindCurveUnchecked(RowFName);
    const bool bRowExists = (WrittenCurve != nullptr);
    const int32 ReadKeyCount = RowKeyCount(WrittenCurve);

    TSharedPtr<FJsonObject> ResultObj = Impl::RowReply(TablePath, RowName);
    ResultObj->SetBoolField(TEXT("row_added"), bRowExists);
    ResultObj->SetNumberField(TEXT("keys_added"), KeysAdded);
    ResultObj->SetNumberField(TEXT("key_count"), ReadKeyCount);
    Impl::AddRowCountAndMode(ResultObj, *Table);

    // Echo the keys we can read back so callers can verify directly.
    TArray<TSharedPtr<FJsonValue>> KeysOut;
    if (const FRichCurve* RichWritten = static_cast<const FRichCurve*>(WrittenCurve))
    {
        if (bRowExists)
        {
            for (const FRichCurveKey& Key : RichWritten->GetConstRefOfKeys())
            {
                TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
                KeyObj->SetNumberField(TEXT("time"), Key.Time);
                KeyObj->SetNumberField(TEXT("value"), Key.Value);
                KeysOut.Add(MakeShared<FJsonValueObject>(KeyObj));
            }
        }
    }
    ResultObj->SetArrayField(TEXT("keys"), KeysOut);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_curve_table_row"), SaveError);
}

// --------------------------------------------------------------------------- //
// add_curve_table_key
// --------------------------------------------------------------------------- //
// MCP:add_curve_table_key - Add or update a single key on an existing UCurveTable row's rich curve.  Creates the row on demand if it does not yet exist.
//   @param curve_table_path - Full asset path to the UCurveTable.
//   @param row_name - Name of the row (rich curve) to key.
//   @param time - Key time (X).
//   @param value - Key value (Y).
//   @returns { curve_table_path: string, row_name: string, time: number, value: number, key_added: bool, created_row: bool, key_count: int, row_count: int, saved: bool }

TSharedPtr<FJsonObject> HandleAddCurveTableKey(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentCurveTableCommandsImpl;

    FString TablePath, RowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // The key itself: both coordinates are required numbers.
    double TimeD = 0.0;
    double ValueD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time"), TimeD)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("value"), ValueD)) { return Missing; }

    UCurveTable* Table = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::OpenRichCurveTable(TablePath, TEXT("add a rich-curve key"), Table))
    {
        return Failure;
    }

    const FName RowFName(*RowName);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddCurveTableKey", "Add Curve Table Key"));
    Table->Modify();

    // Resolve the existing rich curve, or create the row on demand so this command
    // is usable standalone. AddRichCurve replaces, so only call it when missing.
    FRichCurve* Curve = Table->FindRichCurve(RowFName, TEXT("add_curve_table_key"), /*bWarnIfNotFound*/ false);
    const bool bCreatedRow = (Curve == nullptr);
    if (!Curve)
    {
        Curve = &Table->AddRichCurve(RowFName);
    }

    const FKeyHandle Handle = Curve->UpdateOrAddKey(static_cast<float>(TimeD), static_cast<float>(ValueD));

    FString SaveError;
    const bool bSaved = Impl::SaveTableChange(Table, SaveError);

    // Verify by readback from the live curve via the returned handle.
    const float ReadTime = Curve->GetKeyTime(Handle);
    const float ReadValue = Curve->GetKeyValue(Handle);

    TSharedPtr<FJsonObject> ResultObj = Impl::RowReply(TablePath, RowName);
    ResultObj->SetNumberField(TEXT("time"), ReadTime);
    ResultObj->SetNumberField(TEXT("value"), ReadValue);
    ResultObj->SetBoolField(TEXT("key_added"), true);
    ResultObj->SetBoolField(TEXT("created_row"), bCreatedRow);
    ResultObj->SetNumberField(TEXT("key_count"), Curve->GetNumKeys());
    ResultObj->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_curve_table_key"), SaveError);
}

// --------------------------------------------------------------------------- //
// remove_curve_table_key
// --------------------------------------------------------------------------- //
// MCP:remove_curve_table_key - Remove a single key from an existing UCurveTable row's rich curve by index or time, without deleting the row (inverse of add_curve_table_key).
//   @param curve_table_path - Full asset path to the UCurveTable.
//   @param row_name - Name of the row (rich curve) to edit.
//   @param key_index - Index of the key to remove (0 <= index < key_count); provide this OR time
//   @param time - Time of the key to remove; provide this OR key_index
//   @returns { curve_table_path: string, row_name: string, row_existed: bool, key_existed: bool, removed: bool, key_count: int, row_count: int, curve_table_mode: string, saved: bool }

TSharedPtr<FJsonObject> HandleRemoveCurveTableKey(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentCurveTableCommandsImpl;

    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString RowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

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

    UCurveTable* Table = nullptr;
    if (TSharedPtr<FJsonObject> Failure = Impl::OpenRichCurveTable(TablePath, TEXT("remove a rich-curve key"), Table))
    {
        return Failure;
    }

    const FName RowFName(*RowName);

    // Missing row is an honest no-op (row_existed=false), not an error.
    FRichCurve* Curve = Table->FindRichCurve(RowFName, TEXT("remove_curve_table_key"), /*bWarnIfNotFound*/ false);
    if (!Curve)
    {
        TSharedPtr<FJsonObject> NoRow = Impl::RowReply(TablePath, RowName);
        NoRow->SetBoolField(TEXT("row_existed"), false);
        NoRow->SetBoolField(TEXT("key_existed"), false);
        NoRow->SetBoolField(TEXT("removed"), false);
        NoRow->SetNumberField(TEXT("key_count"), 0);
        Impl::AddRowCountAndMode(NoRow, *Table);
        NoRow->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(NoRow);
    }

    FKeyHandle Handle;
    if (TSharedPtr<FJsonObject> Failure = DataAuthoringUtils::FindCurveKey(*Curve, bHasKeyIndex, KeyIndex, TimeD, TEXT("row"), Handle))
    {
        return Failure;
    }

    // DeleteKey does not validate the handle and asserts on an invalid one, so gate
    // on IsKeyHandleValid. An unresolved key is an honest no-op (key_existed=false).
    const bool bKeyExisted = Curve->IsKeyHandleValid(Handle);
    bool bSaved = false;
    if (bKeyExisted)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveCurveTableKey", "Remove Curve Table Key"));
        Table->Modify();

        Curve->DeleteKey(Handle);

        FString SaveError;
        bSaved = Impl::SaveTableChange(Table, SaveError);
    }

    TSharedPtr<FJsonObject> ResultObj = Impl::RowReply(TablePath, RowName);
    ResultObj->SetBoolField(TEXT("row_existed"), true);
    ResultObj->SetBoolField(TEXT("key_existed"), bKeyExisted);
    ResultObj->SetBoolField(TEXT("removed"), bKeyExisted);
    ResultObj->SetNumberField(TEXT("key_count"), Curve->GetNumKeys());
    Impl::AddRowCountAndMode(ResultObj, *Table);

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_curve_table_key"));
}

// --------------------------------------------------------------------------- //
// create_composite_curve_table
// --------------------------------------------------------------------------- //
// MCP:create_composite_curve_table - Creates a UCompositeCurveTable at package_path/name that stacks the given UCurveTables as parents, and saves it. An existing composite there is reused and only gets parents if it has none; parents that fail to load are skipped and listed in missing_parents. Returns parent_count, row_count, created, saved.
//   @param name - Name of the new CompositeCurveTable asset.
//   @param package_path - Content folder; defaults to /Game/CurveTables
//   @param parent_table_paths - Optional list of UCurveTable asset paths to stack as parents
//   @returns { name: string, package_path: string, asset_path: string, curve_table_path: string, curve_class: string, parent_count: int, row_count: int, curve_table_mode: string, created: bool, already_exists: bool, saved: bool, missing_parents?: [string] }

TSharedPtr<FJsonObject> HandleCreateCompositeCurveTable(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/CurveTables");
    }

    // Resolve parent UCurveTable assets up front.
    TArray<UCurveTable*> ParentTables;
    TArray<FString> MissingParents;
    const TArray<TSharedPtr<FJsonValue>>* ParentArray = nullptr;
    if (Params->TryGetArrayField(TEXT("parent_table_paths"), ParentArray) && ParentArray)
    {
        for (const TSharedPtr<FJsonValue>& V : *ParentArray)
        {
            FString ParentPath;
            if (V.IsValid() && V->TryGetString(ParentPath) && !ParentPath.IsEmpty())
            {
                if (UCurveTable* Parent = LoadCurveTableAt(ParentPath))
                {
                    ParentTables.AddUnique(Parent);
                }
                else
                {
                    MissingParents.Add(ParentPath);
                }
            }
        }
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    UCompositeCurveTable* Composite = nullptr;
    bool bAlreadyExists = false;

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        Composite = Cast<UCompositeCurveTable>(
            StaticLoadObject(UCompositeCurveTable::StaticClass(), nullptr, *FullPath));
        if (!Composite)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Asset already exists at %s but is not a UCompositeCurveTable"), *FullPath));
        }
        bAlreadyExists = true;
    }

    if (!Composite)
    {
        FString CreateError;
        Composite = mcp_common::NewAssetInPackage<UCompositeCurveTable>(FullPath, Name, TEXT("Failed to create UCompositeCurveTable"), CreateError);
        if (!Composite)
        {
            return mcp_common::MakeErrorReply(CreateError);
        }

        FAssetRegistryModule::AssetCreated(Composite);
    }
    UPackage* Package = Composite->GetOutermost();

    // Append the requested parents. AppendParentTables() updates the cached row map
    // and wires change delegates. On an idempotent re-run this appends again, so we
    // guard: only append parents not already referenced.
    const int32 ExistingParentCount = ReadParentTableCount(Composite);
    if (ParentTables.Num() > 0)
    {
        Composite->Modify();
        // On a brand-new asset there are no parents yet; on re-run, AddUnique-style
        // dedupe is handled by AppendParentTables appending raw, so only append when
        // the asset was freshly created to avoid duplicate entries.
        if (!bAlreadyExists || ExistingParentCount == 0)
        {
            Composite->AppendParentTables(ParentTables);
        }
    }

    Composite->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, Composite, SaveError);

    // Verify parent_count by reflection on the live asset.
    const int32 ParentCount = ReadParentTableCount(Composite);

    TSharedPtr<FJsonObject> ResultObj = UEAgentCurveTableCommandsImpl::CreatedTableReply(Name, PackagePath, *Composite, ParentCount);
    ResultObj->SetBoolField(TEXT("created"), !bAlreadyExists);
    ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);

    if (MissingParents.Num() > 0)
    {
        ResultObj->SetArrayField(TEXT("missing_parents"), mcp_common::StringsToJson(MissingParents));
    }

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_composite_curve_table"), SaveError);
}

// --------------------------------------------------------------------------- //
// get_curve_table_info
// --------------------------------------------------------------------------- //
// MCP:get_curve_table_info - Get info about a UCurveTable: each row's name + key count, plus mode.
//   @param curve_table_path - Full asset path to the UCurveTable (or UCompositeCurveTable).
//   @returns { curve_table_path: string, curve_table_name: string, curve_class: string, curve_table_mode: string, rows: [{ name, key_count }], row_count: int, is_composite: bool, parent_count?: int }

TSharedPtr<FJsonObject> HandleGetCurveTableInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UCurveTable* Table = LoadCurveTableAt(TablePath);
    if (!Table)
    {
        return UEAgentCurveTableCommandsImpl::CurveTableNotFound(TablePath);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("curve_table_path"), TablePath);
    ResultObj->SetStringField(TEXT("curve_table_name"), Table->GetName());
    ResultObj->SetStringField(TEXT("curve_class"), Table->GetClass()->GetName());
    ResultObj->SetStringField(TEXT("curve_table_mode"), CurveTableModeToString(Table->GetCurveTableMode()));

    TArray<TSharedPtr<FJsonValue>> RowsArr;
    for (const TPair<FName, FRealCurve*>& RowPair : Table->GetRowMap())
    {
        TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
        RowObj->SetStringField(TEXT("name"), RowPair.Key.ToString());
        RowObj->SetNumberField(TEXT("key_count"), RowKeyCount(RowPair.Value));
        RowsArr.Add(MakeShared<FJsonValueObject>(RowObj));
    }
    ResultObj->SetArrayField(TEXT("rows"), RowsArr);
    ResultObj->SetNumberField(TEXT("row_count"), Table->GetRowMap().Num());

    // If this is a composite, also report the parent count.
    if (UCompositeCurveTable* Composite = Cast<UCompositeCurveTable>(Table))
    {
        ResultObj->SetNumberField(TEXT("parent_count"), ReadParentTableCount(Composite));
        ResultObj->SetBoolField(TEXT("is_composite"), true);
    }
    else
    {
        ResultObj->SetBoolField(TEXT("is_composite"), false);
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// --------------------------------------------------------------------------- //
// delete_curve_table_row / rename_curve_table_row
// UCurveTable::DeleteRow/RenameRow are raw (no Modify()/broadcast) — wrap them in
// a transaction + broadcast/PostEditChange/save exactly like HandleAddCurveTableRow.
// Both are mode-agnostic (RowMap is FRealCurve*), so no SimpleCurves guard needed.
// --------------------------------------------------------------------------- //
// MCP:delete_curve_table_row - Delete a named row (curve) from a UCurveTable and verify it is gone.  Idempotent: deleting a missing row reports existed=false/removed=false without error.
//   @param curve_table_path - Full asset path to the UCurveTable.
//   @param row_name - Name of the row (curve) to remove.
//   @returns { curve_table_path: string, row_name: string, existed: bool, removed: bool, still_exists: bool, row_count: int, curve_table_mode: string, saved: bool }
TSharedPtr<FJsonObject> HandleDeleteCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentCurveTableCommandsImpl;

    FString TablePath, RowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UCurveTable* Table = LoadCurveTableAt(TablePath);
    if (Table == nullptr)
    {
        return Impl::CurveTableNotFound(TablePath);
    }

    // Idempotent: a missing row is reported (existed=false), not refused; there is nothing to save then.
    const FName RowFName(*RowName);
    const bool bExisted = (Table->FindCurveUnchecked(RowFName) != nullptr);

    bool bSaved = true;
    if (bExisted)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "DeleteCurveTableRow", "Delete Curve Table Row"));
        Table->Modify();
        Table->DeleteRow(RowFName);

        FString SaveError;
        bSaved = Impl::SaveTableChange(Table, SaveError);
    }

    const bool bStillExists = (Table->FindCurveUnchecked(RowFName) != nullptr);

    TSharedPtr<FJsonObject> ResultObj = Impl::RowReply(TablePath, RowName);
    ResultObj->SetBoolField(TEXT("existed"), bExisted);
    ResultObj->SetBoolField(TEXT("removed"), bExisted && !bStillExists);
    ResultObj->SetBoolField(TEXT("still_exists"), bStillExists);
    Impl::AddRowCountAndMode(ResultObj, *Table);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("delete_curve_table_row"));
}

// MCP:rename_curve_table_row - Rename a row (curve) in a UCurveTable.  UCurveTable::RenameRow silently no-ops if the source is missing or the target name already exists, so those cases are reported via the boolean flags (not errors).
//   @param curve_table_path - Full asset path to the UCurveTable.
//   @param row_name - Current name of the row to rename.
//   @param new_row_name - New name for the row.
//   @returns { curve_table_path: string, row_name: string, new_row_name: string, source_existed: bool, target_existed: bool, renamed: bool, old_still_exists: bool, new_exists: bool, row_count: int, curve_table_mode: string, saved: bool }
TSharedPtr<FJsonObject> HandleRenameCurveTableRow(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentCurveTableCommandsImpl;

    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_table_path"), TablePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString RowName, NewRowName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("row_name"), RowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_row_name"), NewRowName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UCurveTable* Table = LoadCurveTableAt(TablePath);
    if (!Table)
    {
        return Impl::CurveTableNotFound(TablePath);
    }

    const FName OldFName(*RowName);
    const FName NewFName(*NewRowName);
    const bool bSourceExisted = (Table->FindCurveUnchecked(OldFName) != nullptr);
    const bool bTargetExisted = (Table->FindCurveUnchecked(NewFName) != nullptr);

    // RenameRow refuses (no-ops) if source missing or target already present.
    const bool bWillRename = bSourceExisted && !bTargetExisted && OldFName != NewFName;

    bool bSaved = true;
    if (bWillRename)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RenameCurveTableRow", "Rename Curve Table Row"));
        Table->Modify();
        Table->RenameRow(OldFName, NewFName);

        FString SaveError;
        bSaved = Impl::SaveTableChange(Table, SaveError);
    }

    const bool bOldStillExists = (Table->FindCurveUnchecked(OldFName) != nullptr);
    const bool bNewExists = (Table->FindCurveUnchecked(NewFName) != nullptr);

    TSharedPtr<FJsonObject> ResultObj = Impl::RowReply(TablePath, RowName);
    ResultObj->SetStringField(TEXT("new_row_name"), NewRowName);
    ResultObj->SetBoolField(TEXT("source_existed"), bSourceExisted);
    ResultObj->SetBoolField(TEXT("target_existed"), bTargetExisted);
    ResultObj->SetBoolField(TEXT("renamed"), bWillRename && bNewExists && !bOldStillExists);
    ResultObj->SetBoolField(TEXT("old_still_exists"), bOldStillExists);
    ResultObj->SetBoolField(TEXT("new_exists"), bNewExists);
    Impl::AddRowCountAndMode(ResultObj, *Table);
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("rename_curve_table_row"));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_curve_table_key"), &HandleAddCurveTableKey },
    { TEXT("add_curve_table_row"), &HandleAddCurveTableRow },
    { TEXT("create_composite_curve_table"), &HandleCreateCompositeCurveTable },
    { TEXT("create_curve_table"), &HandleCreateCurveTable },
    { TEXT("delete_curve_table_row"), &HandleDeleteCurveTableRow },
    { TEXT("get_curve_table_info"), &HandleGetCurveTableInfo },
    { TEXT("remove_curve_table_key"), &HandleRemoveCurveTableKey },
    { TEXT("rename_curve_table_row"), &HandleRenameCurveTableRow },
};
} // namespace UEAgentCurveTableCommands
