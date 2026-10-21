// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h" // UE_VERSION_OLDER_THAN: the UE 5.7 paths

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"                  // set_chooser_row_outcome: a Blueprint named as a Class result
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"                 // FScopedTransaction: every table edit is one undo step
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

// Chooser plugin (Runtime) — UChooserTable + EObjectChooserResultType + columns + outcomes.
#include "Chooser.h"                       // UChooserTable, FInstancedStruct ColumnsStructs / ResultsStructs
#include "ChooserFunctionLibrary.h"       // UChooserFunctionLibrary::EvaluateChooser
#include "ChooserSignature.h"             // UChooserSignature (ResultType, OutputObjectType, ContextData)
#include "IHasContext.h"                   // EObjectChooserResultType
#include "IChooserColumn.h"                // FChooserColumnBase
#include "FloatRangeColumn.h"              // FFloatRangeColumn / FChooserFloatRangeRowData
#include "BoolColumn.h"                    // FBoolColumn / EBoolColumnCellValue
#include "EnumColumn.h"                    // FEnumColumn / FChooserEnumRowData
#include "ObjectColumn.h"                  // FObjectColumn / FChooserObjectRowData
#include "OutputObjectColumn.h"            // FOutputObjectColumn / FChooserOutputObjectRowData
#include "ObjectChooser_Asset.h"           // FAssetChooser (TObjectPtr<UObject> Asset)
#include "ObjectChooser_Class.h"           // FClassChooser (TObjectPtr<UClass> Class) -- set_chooser_row_outcome on a Class table
#include "OutputFloatColumn.h"              // FOutputFloatColumn
#include "OutputBoolColumn.h"               // FOutputBoolColumn
#if !UE_VERSION_OLDER_THAN(5, 8, 0) // UE 5.7: the Chooser plugin has no Name or OutputName column (no NameColumn.h / OutputNameColumn.h)
#include "NameColumn.h"                     // FChooserNameColumn / FNameContextProperty / ENameColumnCellValueComparison
#include "OutputNameColumn.h"               // FChooserOutputNameColumn
#endif
#include "ObjectClassColumn.h"              // FObjectClassColumn / EObjectClassColumnCellValueComparison
#include "OutputEnumColumn.h"               // FOutputEnumColumn
#include "MultiEnumColumn.h"                // FMultiEnumColumn
#include "RandomizeColumn.h"                // FRandomizeColumn

/**
 * Chooser commands.
 *
 * Wraps the engine's Chooser plugin (UChooserTable + columns + outcomes) so an LLM can author chooser-table
 * assets -- create the table, append typed columns, add implicit rows, set the asset outcome on a row via
 * FAssetChooser -- and then read the table back and evaluate it. All commands are headless-safe: authoring
 * edits the UChooserTable asset directly, and evaluation goes through UChooserFunctionLibrary, a pure runtime
 * API that needs no PIE.
 */
namespace UEAgentChooserCommands
{

// MCP-DOMAIN: Chooser

namespace
{
	/** column_type name -> short name of the column USTRUCT it creates. */
	const TMap<FString, FString>& Chooser_ColumnStructNames()
	{
		static const TMap<FString, FString> KnownTypes =
		{
			{ TEXT("FloatRange"),   TEXT("FloatRangeColumn") },
			{ TEXT("Bool"),         TEXT("BoolColumn") },
			{ TEXT("Enum"),         TEXT("EnumColumn") },
			{ TEXT("Object"),       TEXT("ObjectColumn") },
			{ TEXT("OutputObject"), TEXT("OutputObjectColumn") },
			{ TEXT("OutputFloat"),  TEXT("OutputFloatColumn") },
			{ TEXT("OutputBool"),   TEXT("OutputBoolColumn") },
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
			{ TEXT("OutputName"),   TEXT("ChooserOutputNameColumn") },
#endif
			{ TEXT("ObjectClass"),  TEXT("ObjectClassColumn") },
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
			{ TEXT("Name"),         TEXT("ChooserNameColumn") },
#endif
			{ TEXT("OutputEnum"),   TEXT("OutputEnumColumn") },
			{ TEXT("MultiEnum"),    TEXT("MultiEnumColumn") },
			{ TEXT("Randomize"),    TEXT("RandomizeColumn") },
		};
		return KnownTypes;
	}

	/**
	 * Resolve a column USTRUCT by short name ("FloatRange" / "Bool" / "Enum" / "Object"
	 * / "OutputObject"). Returns the script struct that callers will hand to
	 * FInstancedStruct::InitializeAs. Tries FindFirstObject by short name first, then
	 * strips the leading F, then falls back to /Script/Chooser.<Name>Column via
	 * LoadObject<UScriptStruct>.
	 */
	UScriptStruct* ResolveChooserColumnStruct(const FString& ColumnTypeName)
	{
		const FString* Found = Chooser_ColumnStructNames().Find(ColumnTypeName);
		if (!Found)
		{
			return nullptr;
		}
		const FString StructName = *Found;

		// 1) Try by short name (reflection strips leading F, so try with and without).
		if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*StructName, EFindFirstObjectOptions::NativeFirst))
		{
			return S;
		}
		if (StructName.StartsWith(TEXT("F")) && StructName.Len() > 1)
		{
			if (UScriptStruct* S = FindFirstObject<UScriptStruct>(*StructName.RightChop(1), EFindFirstObjectOptions::NativeFirst))
			{
				return S;
			}
		}

		// 2) Fallback: explicit /Script/Chooser.<Name>Column path.
		const FString FullPath = FString::Printf(TEXT("/Script/Chooser.%s"), *StructName);
		if (UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *FullPath))
		{
			return S;
		}
		return nullptr;
	}

	/**
	 * Best-effort column-type check by name (case-insensitive, like FString ==). Used to reject
	 * obvious type mismatches in add_chooser_column when the caller passes a column_type
	 * that doesn't match the column struct.
	 */
	bool IsKnownColumnType(const FString& ColumnTypeName)
	{
		return Chooser_ColumnStructNames().Contains(ColumnTypeName);
	}

	/** Stringify an EObjectChooserResultType for inspect responses. */
	FString ResultTypeToString(EObjectChooserResultType R)
	{
		switch (R)
		{
		case EObjectChooserResultType::ObjectResult:     return TEXT("Object");
		case EObjectChooserResultType::ClassResult:      return TEXT("Class");
		case EObjectChooserResultType::NoPrimaryResult:  return TEXT("NoPrimaryResult");
		default:                                         return TEXT("Unknown");
		}
	}

	/** The EObjectChooserResultType named S (Object, Class or NoPrimaryResult, any case) into Out; false for any other name. */
	bool ResultTypeFromString(const FString& S, EObjectChooserResultType& Out)
	{
		if (S.Equals(TEXT("Object"), ESearchCase::IgnoreCase))          { Out = EObjectChooserResultType::ObjectResult; return true; }
		if (S.Equals(TEXT("Class"), ESearchCase::IgnoreCase))           { Out = EObjectChooserResultType::ClassResult; return true; }
		if (S.Equals(TEXT("NoPrimaryResult"), ESearchCase::IgnoreCase)) { Out = EObjectChooserResultType::NoPrimaryResult; return true; }
		return false;
	}

	/** The typed column a column struct holds, or null when it holds another type. */
	template <typename ColumnType>
	ColumnType* Chooser_ColumnPtr(FInstancedStruct& Col)
	{
		return Col.GetMutablePtr<ColumnType>();
	}

	template <typename ColumnType>
	const ColumnType* Chooser_ColumnPtr(const FInstancedStruct& Col)
	{
		return Col.GetPtr<ColumnType>();
	}

	/** The row array of a column: RowValuesWithAny for a Bool column, RowValues for every other type. */
	template <typename ColumnType>
	auto& Chooser_RowArray(ColumnType& Column)
	{
		if constexpr (std::is_same_v<std::remove_const_t<ColumnType>, FBoolColumn>)
		{
			return Column.RowValuesWithAny;
		}
		else
		{
			return Column.RowValues;
		}
	}

	/** Visit(Column) when Col holds a ColumnType column; false when it holds another type. */
	template <typename ColumnType, typename InstancedStructType, typename VisitorType>
	bool Chooser_VisitAs(InstancedStructType& Col, VisitorType& Visit)
	{
		if (Col.GetScriptStruct() != ColumnType::StaticStruct())
		{
			return false;
		}
		if (auto* Column = Chooser_ColumnPtr<ColumnType>(Col))
		{
			Visit(*Column);
		}
		return true;
	}

	/**
	 * Calls Visit with the typed column Col holds when it is one of the column types these commands
	 * support; false for any other column type.
	 */
	template <typename InstancedStructType, typename VisitorType>
	bool Chooser_VisitColumn(InstancedStructType& Col, VisitorType&& Visit)
	{
		return Chooser_VisitAs<FFloatRangeColumn>(Col, Visit)
			|| Chooser_VisitAs<FBoolColumn>(Col, Visit)
			|| Chooser_VisitAs<FEnumColumn>(Col, Visit)
			|| Chooser_VisitAs<FObjectColumn>(Col, Visit)
			|| Chooser_VisitAs<FOutputObjectColumn>(Col, Visit)
			|| Chooser_VisitAs<FOutputFloatColumn>(Col, Visit)
			|| Chooser_VisitAs<FOutputBoolColumn>(Col, Visit)
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
			|| Chooser_VisitAs<FChooserOutputNameColumn>(Col, Visit)
#endif
			|| Chooser_VisitAs<FObjectClassColumn>(Col, Visit)
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
			|| Chooser_VisitAs<FChooserNameColumn>(Col, Visit)
#endif
			|| Chooser_VisitAs<FOutputEnumColumn>(Col, Visit)
			|| Chooser_VisitAs<FMultiEnumColumn>(Col, Visit)
			|| Chooser_VisitAs<FRandomizeColumn>(Col, Visit);
	}

	/**
	 * The *ContextProperty struct the editor's new-column button binds as a new column's InputValue
	 * (per CHOOSER_COLUMN_BOILERPLATE the base struct is /Script/Chooser.ChooserParameterXBase).
	 * Null for a Randomize column: its binding is optional (only cross-chooser repeat-avoidance
	 * through a RandomizationContext uses it) and FRandomizeColumn::Compile skips it when unbound,
	 * so binding one would need a real FChooserRandomizationContext for no functional benefit.
	 */
	UScriptStruct* Chooser_DefaultInputStruct(const UScriptStruct* ColumnStruct)
	{
		if (ColumnStruct == FBoolColumn::StaticStruct() || ColumnStruct == FOutputBoolColumn::StaticStruct())
		{
			return FBoolContextProperty::StaticStruct();
		}
		if (ColumnStruct == FFloatRangeColumn::StaticStruct() || ColumnStruct == FOutputFloatColumn::StaticStruct())
		{
			return FFloatContextProperty::StaticStruct();
		}
		if (ColumnStruct == FEnumColumn::StaticStruct() || ColumnStruct == FOutputEnumColumn::StaticStruct()
			|| ColumnStruct == FMultiEnumColumn::StaticStruct())
		{
			return FEnumContextProperty::StaticStruct();
		}
		if (ColumnStruct == FObjectColumn::StaticStruct() || ColumnStruct == FOutputObjectColumn::StaticStruct()
			|| ColumnStruct == FObjectClassColumn::StaticStruct())
		{
			return FObjectContextProperty::StaticStruct();
		}
#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		if (ColumnStruct == FChooserOutputNameColumn::StaticStruct() || ColumnStruct == FChooserNameColumn::StaticStruct())
		{
			return FNameContextProperty::StaticStruct();
		}
#endif
		return nullptr;
	}

	/** The name of a Bool column cell value: "MatchFalse", "MatchTrue", "MatchAny", or "Unknown". */
	const TCHAR* Chooser_BoolCellName(EBoolColumnCellValue Value)
	{
		switch (Value)
		{
		case EBoolColumnCellValue::MatchFalse: return TEXT("MatchFalse");
		case EBoolColumnCellValue::MatchTrue:  return TEXT("MatchTrue");
		case EBoolColumnCellValue::MatchAny:   return TEXT("MatchAny");
		default:                               return TEXT("Unknown");
		}
	}

	/** The name of an Object column comparison: "MatchEqual", "MatchNotEqual", "MatchAny", or else "Modulus". */
	const TCHAR* Chooser_ObjectComparisonName(EObjectColumnCellValueComparison Comparison)
	{
		return Comparison == EObjectColumnCellValueComparison::MatchEqual ? TEXT("MatchEqual")
			: Comparison == EObjectColumnCellValueComparison::MatchNotEqual ? TEXT("MatchNotEqual")
			: Comparison == EObjectColumnCellValueComparison::MatchAny ? TEXT("MatchAny")
			: TEXT("Modulus");
	}

	// One cell of a column -- a row value or the column's DefaultRowValue -- as typed JSON fields, one
	// overload per column type:
	//   FloatRange   : min, max, b_no_min, b_no_max
	//   Bool         : cell_value ("MatchFalse"|"MatchTrue"|"MatchAny")
	//   Enum         : comparison, value (int)
	//   Object       : comparison, asset_path (soft object path string)
	//   OutputObject : asset_path (FAssetChooser.Asset path, or "")
	//   OutputFloat / OutputBool / OutputName / Randomize : default_value
	//   ObjectClass  : class_path, comparison
	//   Name         : value, comparison
	//   OutputEnum / MultiEnum : value (int)
	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FFloatRangeColumn&, const FChooserFloatRangeRowData& Cell)
	{
		Obj->SetNumberField(TEXT("min"), Cell.Min);
		Obj->SetNumberField(TEXT("max"), Cell.Max);
		Obj->SetBoolField(TEXT("b_no_min"), Cell.bNoMin);
		Obj->SetBoolField(TEXT("b_no_max"), Cell.bNoMax);
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FBoolColumn&, EBoolColumnCellValue Cell)
	{
		Obj->SetStringField(TEXT("cell_value"), Chooser_BoolCellName(Cell));
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FEnumColumn&, const FChooserEnumRowData& Cell)
	{
		switch (Cell.Comparison)
		{
		case EEnumColumnCellValueComparison::MatchEqual:    Obj->SetStringField(TEXT("comparison"), TEXT("MatchEqual")); break;
		case EEnumColumnCellValueComparison::MatchNotEqual: Obj->SetStringField(TEXT("comparison"), TEXT("MatchNotEqual")); break;
		case EEnumColumnCellValueComparison::MatchAny:      Obj->SetStringField(TEXT("comparison"), TEXT("MatchAny")); break;
		case EEnumColumnCellValueComparison::Modulus:       Obj->SetStringField(TEXT("comparison"), TEXT("Modulus")); break;
		default: Obj->SetStringField(TEXT("comparison"), TEXT("Unknown")); break;
		}
		Obj->SetNumberField(TEXT("value"), static_cast<int32>(Cell.Value));
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FObjectColumn&, const FChooserObjectRowData& Cell)
	{
		Obj->SetStringField(TEXT("comparison"), Chooser_ObjectComparisonName(Cell.Comparison));
		Obj->SetStringField(TEXT("asset_path"), Cell.Value.ToSoftObjectPath().ToString());
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FOutputObjectColumn&, const FChooserOutputObjectRowData& Cell)
	{
		FString AssetPathStr;
		if (Cell.Value.IsValid() && Cell.Value.GetScriptStruct() == FAssetChooser::StaticStruct())
		{
			const FAssetChooser* AssetChooser = Cell.Value.GetPtr<FAssetChooser>();
			if (AssetChooser && AssetChooser->Asset)
			{
				AssetPathStr = AssetChooser->Asset->GetPathName();
			}
		}
		Obj->SetStringField(TEXT("asset_path"), AssetPathStr);
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FOutputFloatColumn&, double Cell)
	{
		Obj->SetNumberField(TEXT("default_value"), Cell);
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FOutputBoolColumn&, bool Cell)
	{
		Obj->SetBoolField(TEXT("default_value"), Cell);
	}

#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FChooserOutputNameColumn&, const FName& Cell)
	{
		Obj->SetStringField(TEXT("default_value"), Cell.ToString());
	}
#endif

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FObjectClassColumn&, const FChooserObjectClassRowData& Cell)
	{
		Obj->SetStringField(TEXT("class_path"), Cell.Value ? Cell.Value->GetPathName() : FString());
		Obj->SetStringField(TEXT("comparison"),
			Cell.Comparison == EObjectClassColumnCellValueComparison::Equal ? TEXT("Equal")
			: Cell.Comparison == EObjectClassColumnCellValueComparison::NotEqual ? TEXT("NotEqual")
			: Cell.Comparison == EObjectClassColumnCellValueComparison::SubClassOf ? TEXT("SubClassOf")
			: Cell.Comparison == EObjectClassColumnCellValueComparison::NotSubClassOf ? TEXT("NotSubClassOf")
			: TEXT("Any"));
	}

#if !UE_VERSION_OLDER_THAN(5, 8, 0)
	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FChooserNameColumn&, const FChooserNameRowData& Cell)
	{
		Obj->SetStringField(TEXT("value"), Cell.Value.ToString());
		switch (Cell.Comparison)
		{
		case ENameColumnCellValueComparison::MatchEqual:    Obj->SetStringField(TEXT("comparison"), TEXT("MatchEqual")); break;
		case ENameColumnCellValueComparison::MatchNotEqual: Obj->SetStringField(TEXT("comparison"), TEXT("MatchNotEqual")); break;
		case ENameColumnCellValueComparison::MatchAny:      Obj->SetStringField(TEXT("comparison"), TEXT("MatchAny")); break;
		case ENameColumnCellValueComparison::Modulus:       Obj->SetStringField(TEXT("comparison"), TEXT("Modulus")); break;
		default: Obj->SetStringField(TEXT("comparison"), TEXT("Unknown")); break;
		}
	}
#endif

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FOutputEnumColumn&, const FChooserOutputEnumRowData& Cell)
	{
		Obj->SetNumberField(TEXT("value"), static_cast<int32>(Cell.Value));
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FMultiEnumColumn&, const FChooserMultiEnumRowData& Cell)
	{
		Obj->SetNumberField(TEXT("value"), static_cast<int64>(Cell.Value));
	}

	void Chooser_WriteCell(const TSharedPtr<FJsonObject>& Obj, const FRandomizeColumn&, float Cell)
	{
		Obj->SetNumberField(TEXT("default_value"), Cell);
	}

	/**
	 * Effective row count for a chooser table. ResultsStructs is the source of truth;
	 * column row arrays should match its length. We use the max across ColumnsStructs
	 * to handle partially-built tables gracefully.
	 */
	int32 EffectiveRowCount(const UChooserTable* Table)
	{
		if (!Table) { return 0; }
		int32 Max = Table->ResultsStructs.Num();
		for (const FInstancedStruct& Col : Table->ColumnsStructs)
		{
			if (!Col.IsValid()) { continue; }
			Chooser_VisitColumn(Col, [&Max](const auto& Column) { Max = FMath::Max(Max, Chooser_RowArray(Column).Num()); });
		}
		return Max;
	}

	/**
	 * Reads the required table_path. The reply to return when Params is missing or table_path is
	 * empty ("<Command>: params are required" / "<Command>: 'table_path' is required"), else null.
	 */
	TSharedPtr<FJsonObject> Chooser_ReadTablePath(const TSharedPtr<FJsonObject>& Params, const TCHAR* Command, FString& OutTablePath)
	{
		if (!Params.IsValid())
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: params are required"), Command));
		}
		if (!Params->TryGetStringField(TEXT("table_path"), OutTablePath) || OutTablePath.IsEmpty())
		{
			return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: 'table_path' is required"), Command));
		}
		return nullptr;
	}

	/**
	 * The UChooserTable at the long package path TablePath (e.g. /Game/MyGame/MyChooser); null, with
	 * OutError "<Command>: could not load UChooserTable at <TablePath>", when none loads.
	 */
	UChooserTable* Chooser_LoadTable(const FString& TablePath, const TCHAR* Command, TSharedPtr<FJsonObject>& OutError)
	{
		UChooserTable* Table = LoadObject<UChooserTable>(nullptr, *TablePath);
		if (!Table)
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s: could not load UChooserTable at %s"), Command, *TablePath));
		}
		return Table;
	}

	/** Chooser_ReadTablePath then Chooser_LoadTable: the table the request names, or null with OutError. */
	UChooserTable* Chooser_LoadTableParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Command, TSharedPtr<FJsonObject>& OutError)
	{
		FString TablePath;
		OutError = Chooser_ReadTablePath(Params, Command, TablePath);
		if (OutError.IsValid())
		{
			return nullptr;
		}
		return Chooser_LoadTable(TablePath, Command, OutError);
	}

	/**
	 * The column the table_path and column_index params name, with its table and index; null, with OutError naming
	 * Command, when a param is missing, the table does not load, the index is out of range or the column has no struct.
	 */
	FInstancedStruct* Chooser_LoadColumnParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Command,
		UChooserTable*& OutTable, int32& OutColumnIndex, TSharedPtr<FJsonObject>& OutError)
	{
		FString TablePath;
		OutError = Chooser_ReadTablePath(Params, Command, TablePath);
		if (OutError.IsValid())
		{
			return nullptr;
		}
		OutColumnIndex = INDEX_NONE;
		if (!Params->TryGetNumberField(TEXT("column_index"), OutColumnIndex))
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: 'column_index' is required (int)"), Command));
			return nullptr;
		}
		OutTable = Chooser_LoadTable(TablePath, Command, OutError);
		if (!OutTable)
		{
			return nullptr;
		}
		if (!OutTable->ColumnsStructs.IsValidIndex(OutColumnIndex))
		{
			OutError = mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s: column_index %d out of range [0, %d)"),
					Command, OutColumnIndex, OutTable->ColumnsStructs.Num()));
			return nullptr;
		}
		FInstancedStruct& Col = OutTable->ColumnsStructs[OutColumnIndex];
		if (!Col.GetScriptStruct())
		{
			OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: target column has no script struct"), Command));
			return nullptr;
		}
		return &Col;
	}

	/** Compiles the table and saves its package; true when the package reached disk, else OutSaveError says why. */
	bool Chooser_CompileAndSave(UChooserTable* Table, FString& OutSaveError)
	{
		Table->Compile(true);
		return mcp_common::SaveAssetPackage(Table->GetOutermost(), Table, OutSaveError);
	}

	/**
	 * The optional inputs of both evaluate commands: context_object_path (OutContextObject, null when
	 * absent) and object_class (OutResultClass, UObject when absent and never null, since
	 * EvaluateChooserMulti's IsA(nullptr) post-filter would crash). False, with OutError naming
	 * Command, when a given path does not load.
	 */
	bool Chooser_ReadEvaluationInputs(const TSharedPtr<FJsonObject>& Params, const TCHAR* Command,
		UObject*& OutContextObject, UClass*& OutResultClass, TSharedPtr<FJsonObject>& OutError)
	{
		OutContextObject = nullptr;
		FString ContextPath;
		if (Params->TryGetStringField(TEXT("context_object_path"), ContextPath) && !ContextPath.IsEmpty())
		{
			OutContextObject = LoadObject<UObject>(nullptr, *ContextPath);
			if (!OutContextObject)
			{
				OutError = mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s: could not load context UObject at %s"), Command, *ContextPath));
				return false;
			}
		}

		OutResultClass = UObject::StaticClass();
		FString ResultClassPath;
		if (Params->TryGetStringField(TEXT("object_class"), ResultClassPath) && !ResultClassPath.IsEmpty())
		{
			UClass* Cls = LoadObject<UClass>(nullptr, *ResultClassPath);
			if (!Cls)
			{
				OutError = mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s: could not load UClass at %s"), Command, *ResultClassPath));
				return false;
			}
			OutResultClass = Cls;
		}
		return true;
	}
}

// MCP:create_chooser_table - Create (or load if it already exists) a UChooserTable asset under the given package path. A new table is compiled and saved. On an existing table only the result_type and output_object_class the call gives are applied, as one undo step, and the table is compiled and saved only when that changes it (saved false otherwise). success is false, with save_error, when the save fails. An unknown result_type, or an output_object_class that does not load as a UClass, is an error checked before the asset is created or loaded.
//   @param name - Asset short name (e.g. "MyChooser"); becomes the package short name.
//   @param package_path - Long package root (default "/Game/MCP"); full asset path = <package_path>/<name>.
//   @param result_type - EObjectChooserResultType: "Object" (default), "Class", or "NoPrimaryResult".
//   @param output_object_class - UClass path used as OutputObjectType (default UObject), loaded via LoadObject<UClass>; a path that does not load is an error.
//   @returns { asset_path: string, already_existed: bool, result_type: string, output_object_class: string|null, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCreateChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return mcp_common::MakeErrorReply(TEXT("create_chooser_table: params are required"));
	}
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("create_chooser_table: 'name' is required"));
	}
	FString PackagePath = mcp_common::DefaultAssetFolder();
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (PackagePath.IsEmpty()) { PackagePath = mcp_common::DefaultAssetFolder(); }

	const FString FullPath = PackagePath / Name;

	// Both settings are validated before the asset is created or loaded, so a refused call touches nothing.
	FString ResultTypeStr;
	const bool bHasResultType = Params->TryGetStringField(TEXT("result_type"), ResultTypeStr) && !ResultTypeStr.IsEmpty();
	EObjectChooserResultType ResultType = EObjectChooserResultType::ObjectResult;
	if (bHasResultType && !ResultTypeFromString(ResultTypeStr, ResultType))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("create_chooser_table: unknown result_type '%s' (expected Object|Class|NoPrimaryResult); nothing was created or changed"),
			*ResultTypeStr));
	}

	FString OutputObjectClassPath;
	Params->TryGetStringField(TEXT("output_object_class"), OutputObjectClassPath);
	UClass* RequestedClass = nullptr;
	if (!OutputObjectClassPath.IsEmpty())
	{
		RequestedClass = LoadObject<UClass>(nullptr, *OutputObjectClassPath);
		if (!RequestedClass)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("create_chooser_table: could not load UClass at output_object_class '%s'; nothing was created or changed"),
				*OutputObjectClassPath));
		}
	}

	FString CreateError;
	bool bCreated = false;
	UChooserTable* Chooser = mcp_common::CreateOrLoadAsset<UChooserTable>(FullPath, CreateError, &bCreated);
	if (!Chooser)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("Failed to create UChooserTable: %s"), *CreateError));
	}

	// The reply echoes the settings the table holds, read back after any write.
	auto MakeReplyData = [Chooser, bCreated]()
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
		Data->SetBoolField(TEXT("already_existed"), !bCreated);
		Data->SetStringField(TEXT("result_type"), ResultTypeToString(Chooser->ResultType));
		Data->SetStringField(TEXT("output_object_class"),
			Chooser->OutputObjectType ? Chooser->OutputObjectType->GetPathName() : FString());
		return Data;
	};

	if (!bCreated)
	{
		// An existing table is loaded and returned. Only the settings the request names are applied to it, so a
		// call without them leaves it as it is; a change is one undo step and is saved.
		const bool bChangeResultType = bHasResultType && Chooser->ResultType != ResultType;
		const bool bChangeOutputClass = RequestedClass != nullptr && Chooser->OutputObjectType != RequestedClass;
		if (!bChangeResultType && !bChangeOutputClass)
		{
			TSharedPtr<FJsonObject> Data = MakeReplyData();
			Data->SetBoolField(TEXT("saved"), false);
			return mcp_common::MakeOkReply(Data);
		}

		FString SaveError;
		bool bSaved = false;
		{
			const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetChooserTableSettings", "Set Chooser Table Settings (MCP)"));
			Chooser->Modify();
			if (bChangeResultType)
			{
				Chooser->ResultType = ResultType;
			}
			if (bChangeOutputClass)
			{
				Chooser->OutputObjectType = RequestedClass;
			}
			bSaved = Chooser_CompileAndSave(Chooser, SaveError);
		}
		return mcp_common::MakeSavedReply(MakeReplyData(), bSaved, TEXT("create_chooser_table"), SaveError);
	}

	// A new asset: its creation is not an undoable edit, so its first settings are written directly.
	Chooser->ResultType = ResultType;
	if (RequestedClass)
	{
		Chooser->OutputObjectType = RequestedClass;
	}
	FString SaveError;
	const bool bSaved = Chooser_CompileAndSave(Chooser, SaveError);
	return mcp_common::MakeSavedReply(MakeReplyData(), bSaved, TEXT("create_chooser_table"), SaveError);
}

// MCP:add_chooser_column - Add a column of column_type, its input left unbound, to a UChooserTable asset as one undo step, then compile and save the table; success is false, with save_error, when the save fails. Each call adds a new column, even of a type the table already has, except Randomize: a table holds at most one, so an existing Randomize column is returned with already_existed=true and nothing changes. The column goes where the Chooser editor puts it: a filter column before the first output or Randomize column, so filters stay ahead of them; an output column at the end but before a trailing Randomize column; a Randomize column last. The columns after it move up one index; column_index is where the new column went. The new column gets one cell, its DefaultRowValue, per row already in the table, so columns and rows stay in step whichever is added first. Besides the types listed under column_type it accepts OutputFloat, OutputBool, OutputName, OutputEnum, ObjectClass, Name, MultiEnum and Randomize.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param column_type - Column struct to add: "FloatRange", "Bool", "Enum", "Object", or "OutputObject". Name and OutputName need UE 5.8.
//   @param column_name - Label echoed back in the reply only; it is not stored on the column.
//   @returns { table_path: string, column_index: int, column_name: string, column_type: string, already_existed: bool, row_count?: int, saved?: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleAddChooserColumnToTable(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (TSharedPtr<FJsonObject> Missing = Chooser_ReadTablePath(Params, TEXT("add_chooser_column"), TablePath))
	{
		return Missing;
	}
	FString ColumnType;
	if (!Params->TryGetStringField(TEXT("column_type"), ColumnType) || ColumnType.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("add_chooser_column: 'column_type' is required"));
	}
	FString ColumnName;
	if (!Params->TryGetStringField(TEXT("column_name"), ColumnName) || ColumnName.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("add_chooser_column: 'column_name' is required"));
	}

#if UE_VERSION_OLDER_THAN(5, 8, 0)
	// UE 5.7's Chooser plugin has no FChooserNameColumn / FChooserOutputNameColumn (both arrive in 5.8).
	if (ColumnType == TEXT("Name") || ColumnType == TEXT("OutputName"))
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("add_chooser_column: column_type '%s' needs Unreal Engine 5.8 or later: the 5.7 Chooser plugin has no Name or OutputName column; nothing was changed"),
			*ColumnType));
	}
#endif
	if (!IsKnownColumnType(ColumnType))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("add_chooser_column: unknown column_type '%s' (expected FloatRange|Bool|Enum|Object|OutputObject|OutputFloat|OutputBool|OutputName|ObjectClass|Name|OutputEnum|MultiEnum|Randomize)"), *ColumnType));
	}

	TSharedPtr<FJsonObject> LoadError;
	UChooserTable* Table = Chooser_LoadTable(TablePath, TEXT("add_chooser_column"), LoadError);
	if (!Table)
	{
		return LoadError;
	}

	UScriptStruct* ColumnStruct = ResolveChooserColumnStruct(ColumnType);
	if (!ColumnStruct)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("add_chooser_column: could not resolve script struct for column_type '%s'"), *ColumnType));
	}

	// A table holds any number of columns of one type (each bound to its own input), so a new column is
	// always added. The exception is Randomize: a table has at most one, as the Chooser editor's own
	// AddColumn enforces, so an existing one is returned instead (already_existed=true).
	if (ColumnStruct == FRandomizeColumn::StaticStruct())
	{
		for (int32 i = 0; i < Table->ColumnsStructs.Num(); ++i)
		{
			if (Table->ColumnsStructs[i].GetScriptStruct() == ColumnStruct)
			{
				TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetStringField(TEXT("table_path"), Table->GetPathName());
				Data->SetNumberField(TEXT("column_index"), i);
				Data->SetStringField(TEXT("column_name"), ColumnName);
				Data->SetStringField(TEXT("column_type"), ColumnType);
				Data->SetBoolField(TEXT("already_existed"), true);
				return mcp_common::MakeOkReply(Data);
			}
		}
	}

	// Build a fresh InstancedStruct, initialized as the requested column type, with
	// matching default-constructed InputValue where applicable. Its row array gets one
	// DefaultRowValue cell per existing row (ResultsStructs, as the Chooser editor sizes
	// columns), so the new column stays in step with the rows already in the table.
	const int32 RowCount = Table->ResultsStructs.Num();
	FInstancedStruct NewCol;
	NewCol.InitializeAs(ColumnStruct);
	UScriptStruct* InputStruct = Chooser_DefaultInputStruct(NewCol.GetScriptStruct());
	Chooser_VisitColumn(NewCol, [InputStruct, RowCount](auto& Column)
	{
		if (InputStruct)
		{
			Column.InputValue.InitializeAs(InputStruct);
		}
		auto& Rows = Chooser_RowArray(Column);
		while (Rows.Num() < RowCount)
		{
			Rows.Add(Column.DefaultRowValue);
		}
	});

	// The column goes where the Chooser editor's FChooserTableViewModel::AddColumn puts it, by the same tests: a
	// Randomize column at the end (it picks one of the rows the columns before it left); an output column at the
	// end too, but before a trailing Randomize column; any other column, a filter, before the first output or
	// Randomize column, so the filters stay ahead of them. Each column owns its row cells, so the insert leaves
	// every row in step.
	int32 NewIndex = Table->ColumnsStructs.Num();
	if (const FChooserColumnBase* NewColumn = NewCol.GetPtr<FChooserColumnBase>(); NewColumn && !NewColumn->IsRandomizeColumn())
	{
		if (NewColumn->HasOutputs())
		{
			const FChooserColumnBase* LastColumn = NewIndex > 0 ? Table->ColumnsStructs.Last().GetPtr<FChooserColumnBase>() : nullptr;
			if (LastColumn && LastColumn->IsRandomizeColumn())
			{
				--NewIndex;
			}
		}
		else
		{
			NewIndex = 0;
			while (NewIndex < Table->ColumnsStructs.Num())
			{
				const FChooserColumnBase* Column = Table->ColumnsStructs[NewIndex].GetPtr<FChooserColumnBase>();
				if (Column && (Column->HasOutputs() || Column->IsRandomizeColumn()))
				{
					break;
				}
				++NewIndex;
			}
		}
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddChooserColumn", "Add Chooser Column (MCP)"));
	Table->Modify();
	Table->ColumnsStructs.Insert(MoveTemp(NewCol), NewIndex);
	FString SaveError;
	const bool bSaved = Chooser_CompileAndSave(Table, SaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), Table->GetPathName());
	Data->SetNumberField(TEXT("column_index"), NewIndex);
	Data->SetStringField(TEXT("column_name"), ColumnName);
	Data->SetStringField(TEXT("column_type"), ColumnType);
	Data->SetBoolField(TEXT("already_existed"), false);
	int32 ColumnRowCount = 0;
	Chooser_VisitColumn(Table->ColumnsStructs[NewIndex], [&ColumnRowCount](const auto& Column) { ColumnRowCount = Chooser_RowArray(Column).Num(); });
	Data->SetNumberField(TEXT("row_count"), ColumnRowCount);
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_chooser_column"), SaveError);
}

// MCP:add_chooser_row - Add one implicit row to a UChooserTable. Grows every column's RowValues by one (using each column's DefaultRowValue where available) and grows ResultsStructs by one, as one undo step, then compiles and saves the table; success is false, with save_error, when the save fails.
//   @param table_path - Long package path of an existing UChooserTable.
//   @returns { table_path: string, row_index: int, row_count: int, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleAddChooserRow(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UChooserTable* Table = Chooser_LoadTableParam(Params, TEXT("add_chooser_row"), Error);
	if (!Table)
	{
		return Error;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddChooserRow", "Add Chooser Row (MCP)"));
	Table->Modify();

	// Grow every column's row-array by one, using the column's DefaultRowValue when defined
	// and a default-constructed entry otherwise.
	for (FInstancedStruct& Col : Table->ColumnsStructs)
	{
		if (!Col.IsValid()) { continue; }
		Chooser_VisitColumn(Col, [](auto& Column)
		{
			Chooser_RowArray(Column).Add(Column.DefaultRowValue);
		});
	}

	Table->ResultsStructs.AddDefaulted();
	const int32 NewIndex = Table->ResultsStructs.Num() - 1;

	FString SaveError;
	const bool bSaved = Chooser_CompileAndSave(Table, SaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), Table->GetPathName());
	Data->SetNumberField(TEXT("row_index"), NewIndex);
	Data->SetNumberField(TEXT("row_count"), EffectiveRowCount(Table));
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_chooser_row"), SaveError);
}

// MCP:set_chooser_row_outcome - Sets the result a Chooser Table row returns -- the row's entry in the table's results, which evaluate_chooser_table returns when that row is chosen: an Asset result holding asset_path on an Object table, or a Class result holding the class on a Class table (asset_path may then name a Blueprint, whose generated class is used). One undo step, then the table is compiled and saved; success is false, with save_error, when the save fails. Idempotent: a row already returning that asset or class is left as it is (already_existed true, saved false). Refused with nothing changed when row_index is out of range of the table's results, the asset does not load, the table returns no primary result (NoPrimaryResult), or the asset or class is not of the table's output_object_class. The row's filter and output-column cells are not touched: set_chooser_cell writes those (an OutputObject column included). Read the result back with evaluate_chooser_table; add rows with add_chooser_row; get_chooser_table_info gives the row count and result type.
//   @param table_path - Long package path of an existing UChooserTable (e.g. "/Game/Choosers/CT_Locomotion"), as list_chooser_tables lists it.
//   @param row_index - 0-based row index, below the table's result count (results_count in get_chooser_table_info).
//   @param asset_path - Long package path (e.g. "/Game/Anims/AS_Run") or object path of the asset the row returns; on a Class table a class path or a Blueprint. It must load, or the call is refused.
//   @returns { table_path: string, row_index: int, asset_path: string, result_type: "Object"|"Class", result_struct: string, already_existed: bool, saved: bool, save_error?: string } -- asset_path is what the row now returns, read back from its result entry
TSharedPtr<FJsonObject> HandleSetChooserRowOutcome(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (TSharedPtr<FJsonObject> Missing = Chooser_ReadTablePath(Params, TEXT("set_chooser_row_outcome"), TablePath))
	{
		return Missing;
	}
	int32 RowIndex = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("row_index"), RowIndex))
	{
		return mcp_common::MakeErrorReply(TEXT("set_chooser_row_outcome: 'row_index' is required (int)"));
	}
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("set_chooser_row_outcome: 'asset_path' is required"));
	}

	TSharedPtr<FJsonObject> LoadError;
	UChooserTable* Table = Chooser_LoadTable(TablePath, TEXT("set_chooser_row_outcome"), LoadError);
	if (!Table)
	{
		return LoadError;
	}

	// A row's result is its entry in ResultsStructs: what EvaluateChooser returns when the row is chosen.
	if (Table->ResultType == EObjectChooserResultType::NoPrimaryResult)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("set_chooser_row_outcome: table %s returns no primary result (result_type NoPrimaryResult), so its rows have no result to set; ")
			TEXT("write its output columns with set_chooser_cell, or change result_type with create_chooser_table. Nothing was changed."), *TablePath));
	}
	if (RowIndex < 0 || RowIndex >= Table->ResultsStructs.Num())
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("set_chooser_row_outcome: row_index %d out of range [0,%d) of the table's results"),
				RowIndex, Table->ResultsStructs.Num()));
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("set_chooser_row_outcome: could not load UObject at %s"), *AssetPath));
	}

	// The result entry the row gets: an Asset result on an Object table, a Class result on a Class table, of the
	// table's output class (OutputObjectType: the object's class, or the parent of the returned classes).
	const bool bClassResult = Table->ResultType == EObjectChooserResultType::ClassResult;
	UClass* const OutputType = Table->OutputObjectType;
	FInstancedStruct NewResult;
	if (bClassResult)
	{
		UClass* ResultClass = Cast<UClass>(Asset);
		if (!ResultClass)
		{
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
			{
				ResultClass = Blueprint->GeneratedClass;
			}
		}
		if (!ResultClass)
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("set_chooser_row_outcome: table %s returns classes (result_type Class) and %s is a %s, not a class or a Blueprint; nothing was changed"),
				*TablePath, *AssetPath, *Asset->GetClass()->GetName()));
		}
		if (OutputType && !ResultClass->IsChildOf(OutputType))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("set_chooser_row_outcome: class %s is not a subclass of the table's output_object_class %s; nothing was changed"),
				*ResultClass->GetPathName(), *OutputType->GetPathName()));
		}
		FClassChooser ClassResult;
		ClassResult.Class = ResultClass;
		NewResult = FInstancedStruct::Make<FClassChooser>(ClassResult);
	}
	else
	{
		if (OutputType && !Asset->IsA(OutputType))
		{
			return mcp_common::MakeErrorReply(FString::Printf(
				TEXT("set_chooser_row_outcome: %s is a %s, not of the table's output_object_class %s; nothing was changed"),
				*Asset->GetPathName(), *Asset->GetClass()->GetName(), *OutputType->GetPathName()));
		}
		FAssetChooser AssetResult;
		AssetResult.Asset = Asset;
		NewResult = FInstancedStruct::Make<FAssetChooser>(AssetResult);
	}

	// What a result entry returns, for the idempotency check and the read-back.
	auto ResultTarget = [](const FInstancedStruct& Result) -> const UObject*
	{
		if (const FAssetChooser* AssetResult = Result.GetPtr<FAssetChooser>())
		{
			return AssetResult->Asset.Get();
		}
		if (const FClassChooser* ClassResult = Result.GetPtr<FClassChooser>())
		{
			return ClassResult->Class.Get();
		}
		return nullptr;
	};
	const UObject* Wanted = ResultTarget(NewResult);
	const bool bAlreadyExisted = Table->ResultsStructs[RowIndex].GetScriptStruct() == NewResult.GetScriptStruct()
		&& ResultTarget(Table->ResultsStructs[RowIndex]) == Wanted;

	bool bSaved = false;
	FString SaveError;
	if (!bAlreadyExisted)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetChooserRowOutcome", "Set Chooser Row Outcome (MCP)"));
		Table->Modify();
		Table->ResultsStructs[RowIndex] = NewResult;
		bSaved = Chooser_CompileAndSave(Table, SaveError);
	}

	// Read back from the row's result entry.
	const FInstancedStruct& RowResult = Table->ResultsStructs[RowIndex];
	const UObject* Returned = ResultTarget(RowResult);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), Table->GetPathName());
	Data->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetStringField(TEXT("asset_path"), Returned ? Returned->GetPathName() : FString());
	Data->SetStringField(TEXT("result_type"), ResultTypeToString(Table->ResultType));
	Data->SetStringField(TEXT("result_struct"), RowResult.GetScriptStruct() ? RowResult.GetScriptStruct()->GetName() : FString());
	Data->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
	if (bAlreadyExisted)
	{
		// Nothing was written, so nothing was saved.
		Data->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Data);
	}
	if (Returned != Wanted)
	{
		Data->SetBoolField(TEXT("saved"), bSaved);
		return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
			TEXT("set_chooser_row_outcome: row %d's result does not read back as %s after the write."), RowIndex, *Wanted->GetPathName()));
	}
	return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_chooser_row_outcome"), SaveError);
}

// MCP:get_chooser_table_info - Read the shape of one Chooser Table asset without changing it: its result type, output object class, columns (index and column struct type), row count, result count and context-data count. Use it first, to learn the column indices and row count that get_chooser_column_values (one column across all rows), get_chooser_row_values (one row across all columns), set_chooser_cell and remove_chooser_row take; to see which row a context would pick, use evaluate_chooser_table; to find tables, list_chooser_tables. row_count is the largest of the result count and every column's row array, so a half-built table still reports all its rows. A path that does not load a Chooser Table is an error and nothing is read.
//   @renamed-from inspect_chooser_table
//   @param table_path - Long package path of the Chooser Table, without the object suffix, e.g. "/Game/Animation/CHT_Locomotion", as list_chooser_tables returns it.
//   @returns { table_path: string, result_type: "Object"|"Class"|"NoPrimaryResult", output_object_class: string (empty when none), column_count: int, columns: [{index, type}], row_count: int, results_count: int, context_data_count: int }
TSharedPtr<FJsonObject> HandleInspectChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UChooserTable* Table = Chooser_LoadTableParam(Params, TEXT("get_chooser_table_info"), Error);
	if (!Table)
	{
		return Error;
	}

	TArray<TSharedPtr<FJsonValue>> ColumnsArr;
	for (int32 i = 0; i < Table->ColumnsStructs.Num(); ++i)
	{
		const FInstancedStruct& Col = Table->ColumnsStructs[i];
		TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
		ColObj->SetNumberField(TEXT("index"), i);
		ColObj->SetStringField(TEXT("type"), Col.GetScriptStruct() ? Col.GetScriptStruct()->GetName() : TEXT(""));
		ColumnsArr.Add(MakeShared<FJsonValueObject>(ColObj));
	}

	int32 ResultsCount = 0;
	ResultsCount = Table->ResultsStructs.Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), Table->GetPathName());
	Data->SetStringField(TEXT("result_type"), ResultTypeToString(Table->ResultType));
	Data->SetStringField(TEXT("output_object_class"),
		Table->OutputObjectType ? Table->OutputObjectType->GetPathName() : FString());
	Data->SetNumberField(TEXT("column_count"), Table->ColumnsStructs.Num());
	Data->SetArrayField(TEXT("columns"), ColumnsArr);
	Data->SetNumberField(TEXT("row_count"), EffectiveRowCount(Table));
	Data->SetNumberField(TEXT("results_count"), ResultsCount);
	Data->SetNumberField(TEXT("context_data_count"), Table->ContextData.Num());
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Typed delete + row removal + table enumeration.
// ---------------------------------------------------------------------------

// MCP:delete_chooser_table - Destroy a UChooserTable asset at a /Game path (mirror of create_chooser_table). Typed-delete: it loads the asset and REFUSES to delete it if it is not a UChooserTable, so it cannot wipe the wrong asset the way the generic delete_asset can; that refusal, and a delete the engine does not carry out, make success false with deleted false. Idempotent on missing assets.
//   @param table_path - Full /Game/.../Chooser asset path.
//   @returns { table_path, deleted, already_existed, asset_class?, note? }
//   NOTE (impl): delegates the actual delete to UEditorAssetLibrary::DeleteAsset once the type check passes.

TSharedPtr<FJsonObject> HandleDeleteChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), TablePath)) { return Missing; }

	if (!UEditorAssetLibrary::DoesAssetExist(TablePath))
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("table_path"), TablePath);
		Resp->SetBoolField(TEXT("deleted"), false);
		Resp->SetBoolField(TEXT("already_existed"), false);
		Resp->SetStringField(TEXT("note"), TEXT("table_path does not exist; nothing to remove"));
		return mcp_common::MakeOkReply(Resp);
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *TablePath);
	if (!Asset)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("delete_chooser_table: could not load asset at %s"), *TablePath));
	}

	UChooserTable* Table = Cast<UChooserTable>(Asset);
	if (!Table)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("table_path"), TablePath);
		Resp->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());
		Resp->SetBoolField(TEXT("deleted"), false);
		const FString Note = FString::Printf(TEXT("asset is not a UChooserTable (got %s) - wrong type, refusing to delete"),
			*Asset->GetClass()->GetName());
		Resp->SetStringField(TEXT("note"), Note);
		return mcp_common::MakeOutcomeReply(Resp, false, FString::Printf(TEXT("delete_chooser_table: %s"), *Note));
	}

	// The class name is read before the delete: ForceDeleteObjects can collect garbage, which frees the table.
	const FString AssetClassName = Table->GetClass()->GetName();
	const bool bDeleted = UEditorAssetLibrary::DeleteAsset(TablePath);

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("table_path"), TablePath);
	Resp->SetStringField(TEXT("asset_class"), AssetClassName);
	Resp->SetBoolField(TEXT("deleted"), bDeleted);
	Resp->SetBoolField(TEXT("already_existed"), true);
	return mcp_common::MakeOutcomeReply(Resp, bDeleted, bDeleted ? FString()
		: FString::Printf(TEXT("delete_chooser_table: the engine did not delete %s (see the editor log for why)"), *TablePath));
}

// MCP:remove_chooser_row - Remove one implicit row from a UChooserTable (inverse of add_chooser_row). A chooser row is not a real object: this shrinks EVERY column's RowValues array by one -- at row_index if given, else the last row -- and shrinks ResultsStructs to match, as one undo step, then compiles and saves the table like add_chooser_row; success is false, with save_error, when the save fails. A table with no rows is left as it is (note, saved false). Use remove_chooser_column to drop a column instead.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param row_index - Optional 0-based row index to remove (default: last row).
//   @returns { table_path, removed_row_index, previous_row_count, new_row_count, saved, save_error?, note? }

TSharedPtr<FJsonObject> HandleRemoveChooserRow(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("table_path"), TablePath)) { return Missing; }

	TSharedPtr<FJsonObject> LoadError;
	UChooserTable* Table = Chooser_LoadTable(TablePath, TEXT("remove_chooser_row"), LoadError);
	if (!Table)
	{
		return LoadError;
	}

	const int32 RowCountBefore = EffectiveRowCount(Table);
	if (RowCountBefore == 0)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("table_path"), TablePath);
		Resp->SetNumberField(TEXT("removed_row_index"), INDEX_NONE);
		Resp->SetNumberField(TEXT("previous_row_count"), 0);
		Resp->SetNumberField(TEXT("new_row_count"), 0);
		Resp->SetBoolField(TEXT("saved"), false);
		Resp->SetStringField(TEXT("note"), TEXT("table has zero rows; nothing to remove"));
		return mcp_common::MakeOkReply(Resp);
	}

	int32 RowIndex = -1;
	if (!Params->TryGetNumberField(TEXT("row_index"), RowIndex))
	{
		RowIndex = RowCountBefore - 1; // default: last row
	}
	if (RowIndex < 0 || RowIndex >= RowCountBefore)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("row_index %d out of range [0, %d)"), RowIndex, RowCountBefore));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveChooserRow", "Remove Chooser Row (MCP)"));
	Table->Modify();

	// Shrink every column's row-array at RowIndex (parallel arrays must stay in sync).
	for (FInstancedStruct& Col : Table->ColumnsStructs)
	{
		if (!Col.IsValid()) { continue; }
		Chooser_VisitColumn(Col, [RowIndex](auto& Column)
		{
			auto& Rows = Chooser_RowArray(Column);
			if (Rows.IsValidIndex(RowIndex)) { Rows.RemoveAt(RowIndex); }
		});
	}

	if (Table->ResultsStructs.IsValidIndex(RowIndex))
	{
		Table->ResultsStructs.RemoveAt(RowIndex);
	}

	FString SaveError;
	const bool bSaved = Chooser_CompileAndSave(Table, SaveError);

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("table_path"), Table->GetPathName());
	Resp->SetNumberField(TEXT("removed_row_index"), RowIndex);
	Resp->SetNumberField(TEXT("previous_row_count"), RowCountBefore);
	Resp->SetNumberField(TEXT("new_row_count"), EffectiveRowCount(Table));
	return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_chooser_row"), SaveError);
}

// MCP:list_chooser_tables - Lists the Chooser Table assets under a content folder and all its sub-folders, found through the Asset Registry, each with its name, object path, class and row and column counts. Each match is loaded to count its rows and columns; nothing is changed or saved. A folder without Chooser Tables returns total 0, not an error, and only assets of exactly the UChooserTable class are listed. Use it to find the table_path the other chooser tools take; then get_chooser_table_info for one table's columns and result type, evaluate_chooser_table to run one, create_chooser_table to make a new one.
//   @param package_path - Optional content folder to scan, sub-folders included (default "/Game"; an empty string also means /Game), e.g. "/Game/Animation/Choosers"; plugin content lives under its mount point, such as "/MyPlugin".
//   @returns { package_path, total, tables:[{ asset_name, asset_path, asset_class, row_count, column_count }] }

TSharedPtr<FJsonObject> HandleListChooserTables(const TSharedPtr<FJsonObject>& Params)
{
	FString PackagePath;
	Params->TryGetStringField(TEXT("package_path"), PackagePath);
	if (PackagePath.IsEmpty()) { PackagePath = TEXT("/Game"); }

	IAssetRegistry& AR = mcp_common::AssetRegistry();

	// Recursive scan of the package path.
	TArray<FAssetData> Assets;
	AR.GetAssetsByPath(FName(*PackagePath), Assets, /*bRecursive*/ true);

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FAssetData& AD : Assets)
	{
		if (AD.AssetClassPath != UChooserTable::StaticClass()->GetClassPathName())
		{
			continue;
		}
		// Load (lightweight) to read row/column count.
		UChooserTable* Table = Cast<UChooserTable>(AD.GetAsset());
		if (!Table) { continue; }
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("asset_name"), Table->GetName());
		E->SetStringField(TEXT("asset_path"), Table->GetPathName());
		E->SetStringField(TEXT("asset_class"), Table->GetClass()->GetName());
		E->SetNumberField(TEXT("row_count"), EffectiveRowCount(Table));
		E->SetNumberField(TEXT("column_count"), Table->ColumnsStructs.Num());
		Entries.Add(MakeShared<FJsonValueObject>(E));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("package_path"), PackagePath);
	Data->SetNumberField(TEXT("total"), Entries.Num());
	Data->SetArrayField(TEXT("tables"), Entries);
	return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Column removal + per-column default row value
// + runtime evaluation. All three are headless-safe authoring/runtime calls
// against the same UChooserTable pipeline; no PIE, no render.
// ---------------------------------------------------------------------------

// MCP:remove_chooser_column - Remove a column from a UChooserTable by index (inverse of add_chooser_column). The column's per-row cells go with it -- a column owns its own RowValues, so nothing needs re-syncing across the other columns and the row COUNT is unchanged (use remove_chooser_row for that). The removal is one undo step; the table is then compiled and saved, and success is false, with save_error, when the save fails. A table with no columns is left as it is (note, saved false). Omit column_index (or pass -1) to remove the LAST column, matching remove_chooser_row's default.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param column_index - Optional 0-based column index (default -1 = last column).
//   @returns { table_path, removed_column_index, previous_column_count, new_column_count, column_type, saved, save_error?, note? }
//   NOTE (impl): drops the FInstancedStruct at Table->ColumnsStructs[column_index]; the parallel RowValues live inside it.

TSharedPtr<FJsonObject> HandleRemoveChooserColumn(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (!Params.IsValid() || !Params->TryGetStringField(TEXT("table_path"), TablePath) || TablePath.IsEmpty())
	{
		return mcp_common::MakeErrorReply(TEXT("Missing required parameter: table_path"));
	}

	TSharedPtr<FJsonObject> LoadError;
	UChooserTable* Table = Chooser_LoadTable(TablePath, TEXT("remove_chooser_column"), LoadError);
	if (!Table)
	{
		return LoadError;
	}

	const int32 PreviousCount = Table->ColumnsStructs.Num();
	if (PreviousCount == 0)
	{
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("table_path"), Table->GetPathName());
		Resp->SetNumberField(TEXT("removed_column_index"), INDEX_NONE);
		Resp->SetNumberField(TEXT("previous_column_count"), 0);
		Resp->SetNumberField(TEXT("new_column_count"), 0);
		Resp->SetBoolField(TEXT("saved"), false);
		Resp->SetStringField(TEXT("note"), TEXT("table has zero columns; nothing to remove"));
		return mcp_common::MakeOkReply(Resp);
	}

	int32 ColumnIndex = -1;
	if (Params.IsValid())
	{
		Params->TryGetNumberField(TEXT("column_index"), ColumnIndex);
	}
	if (ColumnIndex < 0 || ColumnIndex >= PreviousCount)
	{
		if (ColumnIndex == -1)
		{
			ColumnIndex = PreviousCount - 1;
		}
		else
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("remove_chooser_column: column_index %d out of range [0, %d)"), ColumnIndex, PreviousCount));
		}
	}

	// Capture the column's struct type BEFORE RemoveAt for the response payload.
	const FString RemovedType = Table->ColumnsStructs[ColumnIndex].GetScriptStruct()
		? Table->ColumnsStructs[ColumnIndex].GetScriptStruct()->GetName()
		: FString();

	const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveChooserColumn", "Remove Chooser Column (MCP)"));
	Table->Modify();
	Table->ColumnsStructs.RemoveAt(ColumnIndex);
	FString SaveError;
	const bool bSaved = Chooser_CompileAndSave(Table, SaveError);

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("table_path"), Table->GetPathName());
	Resp->SetNumberField(TEXT("removed_column_index"), ColumnIndex);
	Resp->SetNumberField(TEXT("previous_column_count"), PreviousCount);
	Resp->SetNumberField(TEXT("new_column_count"), Table->ColumnsStructs.Num());
	Resp->SetStringField(TEXT("column_type"), RemovedType);
	return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_chooser_column"), SaveError);
}

namespace
{
	/** The cell CellRef writes: the column's DefaultRowValue for INDEX_NONE, else the cell of row RowIndex. */
	template <typename ColumnType>
	auto& Chooser_CellRef(ColumnType& Column, int32 RowIndex)
	{
		return RowIndex == INDEX_NONE ? Column.DefaultRowValue : Chooser_RowArray(Column)[RowIndex];
	}

	/**
	 * Writes the typed cell value the params give (min/max/b_no_min/b_no_max, default_value, comparison, value,
	 * asset_path, class_path: the fields that apply to the column type) into a cell of the column copy Col: its
	 * DefaultRowValue for RowIndex INDEX_NONE (set_chooser_column_default), else the cell of row RowIndex, which the
	 * caller has range-checked (set_chooser_cell). bOutAnyChange is whether the cell changed, bOutAlreadyExisted
	 * whether it already held the value. The error reply, prefixed with Command, for a value that does not fit or a
	 * column type no command writes; null otherwise.
	 */
	TSharedPtr<FJsonObject> Chooser_ApplyCellParams(const TSharedPtr<FJsonObject>& Params, const TCHAR* Command,
		FInstancedStruct& Col, int32 RowIndex, bool& bOutAnyChange, bool& bOutAlreadyExisted)
	{
		const UScriptStruct* ColStruct = Col.GetScriptStruct();
		const FString ColumnType = ColStruct->GetName();
		bool bAlreadyExisted = false;
		bool bAnyChange = false;

		if (ColStruct == FFloatRangeColumn::StaticStruct())
		{
			FFloatRangeColumn& C = Col.GetMutable<FFloatRangeColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			bool bSetSomething = false;
			if (Params->HasField(TEXT("min")))
			{
				double V = 0.0; Params->TryGetNumberField(TEXT("min"), V);
				const float NewMin = static_cast<float>(V);
				if (!bAnyChange && Cell.Min != NewMin) { bAnyChange = true; }
				Cell.Min = NewMin;
				bSetSomething = true;
			}
			if (Params->HasField(TEXT("max")))
			{
				double V = 0.0; Params->TryGetNumberField(TEXT("max"), V);
				const float NewMax = static_cast<float>(V);
				if (!bAnyChange && Cell.Max != NewMax) { bAnyChange = true; }
				Cell.Max = NewMax;
				bSetSomething = true;
			}
			if (Params->HasField(TEXT("b_no_min")))
			{
				bool V = false; Params->TryGetBoolField(TEXT("b_no_min"), V);
				if (!bAnyChange && Cell.bNoMin != V) { bAnyChange = true; }
				Cell.bNoMin = V;
				bSetSomething = true;
			}
			if (Params->HasField(TEXT("b_no_max")))
			{
				bool V = false; Params->TryGetBoolField(TEXT("b_no_max"), V);
				if (!bAnyChange && Cell.bNoMax != V) { bAnyChange = true; }
				Cell.bNoMax = V;
				bSetSomething = true;
			}
			if (!bSetSomething)
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(FloatRange): provide at least one of min/max/b_no_min/b_no_max"), Command));
			}
			bAlreadyExisted = !bAnyChange;
		}
		else if (ColStruct == FBoolColumn::StaticStruct())
		{
			FBoolColumn& C = Col.GetMutable<FBoolColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			FString DV;
			Params->TryGetStringField(TEXT("default_value"), DV);
			if (DV.IsEmpty())
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Bool): 'default_value' is required (MatchFalse|MatchTrue|MatchAny)"), Command));
			}
			EBoolColumnCellValue NewVal = EBoolColumnCellValue::MatchAny;
			if (DV.Equals(TEXT("MatchFalse"), ESearchCase::IgnoreCase))        { NewVal = EBoolColumnCellValue::MatchFalse; }
			else if (DV.Equals(TEXT("MatchTrue"), ESearchCase::IgnoreCase))    { NewVal = EBoolColumnCellValue::MatchTrue; }
			else if (DV.Equals(TEXT("MatchAny"), ESearchCase::IgnoreCase))     { NewVal = EBoolColumnCellValue::MatchAny; }
			else
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Bool): unknown default_value '%s' (expected MatchFalse|MatchTrue|MatchAny)"), Command, *DV));
			}
			bAlreadyExisted = (Cell == NewVal);
			Cell = NewVal;
			bAnyChange = !bAlreadyExisted;
		}
		else if (ColStruct == FEnumColumn::StaticStruct())
		{
			FEnumColumn& C = Col.GetMutable<FEnumColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			bool bSetSomething = false;

			FString ComparisonStr;
			if (Params->TryGetStringField(TEXT("comparison"), ComparisonStr) && !ComparisonStr.IsEmpty())
			{
				EEnumColumnCellValueComparison NewCmp = EEnumColumnCellValueComparison::MatchEqual;
				if (ComparisonStr.Equals(TEXT("MatchEqual"), ESearchCase::IgnoreCase))       { NewCmp = EEnumColumnCellValueComparison::MatchEqual; }
				else if (ComparisonStr.Equals(TEXT("MatchNotEqual"), ESearchCase::IgnoreCase)) { NewCmp = EEnumColumnCellValueComparison::MatchNotEqual; }
				else if (ComparisonStr.Equals(TEXT("MatchAny"), ESearchCase::IgnoreCase))    { NewCmp = EEnumColumnCellValueComparison::MatchAny; }
				else if (ComparisonStr.Equals(TEXT("Modulus"), ESearchCase::IgnoreCase))     { NewCmp = EEnumColumnCellValueComparison::Modulus; }
				else
				{
					return mcp_common::MakeErrorReply(
						FString::Printf(TEXT("%s(Enum): unknown comparison '%s' (expected MatchEqual|MatchNotEqual|MatchAny|Modulus)"), Command, *ComparisonStr));
				}
				if (!bAnyChange && Cell.Comparison != NewCmp) { bAnyChange = true; }
				Cell.Comparison = NewCmp;
				bSetSomething = true;
			}
			if (Params->HasField(TEXT("value")))
			{
				int32 V = 0; Params->TryGetNumberField(TEXT("value"), V);
				const uint8 NewVal = static_cast<uint8>(FMath::Clamp(V, 0, 255));
				if (!bAnyChange && Cell.Value != NewVal) { bAnyChange = true; }
				Cell.Value = NewVal;
				bSetSomething = true;
			}
			if (!bSetSomething)
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Enum): provide at least one of comparison/value"), Command));
			}
			bAlreadyExisted = !bAnyChange;
		}
		else if (ColStruct == FObjectColumn::StaticStruct())
		{
			FObjectColumn& C = Col.GetMutable<FObjectColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			FString AssetPath;
			Params->TryGetStringField(TEXT("asset_path"), AssetPath);
			if (AssetPath.IsEmpty())
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Object): 'asset_path' is required"), Command));
			}
			UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
			if (!Asset)
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Object): could not load UObject at %s"), Command, *AssetPath));
			}
			const FSoftObjectPath NewSoftPath(Asset);
			bAlreadyExisted = (Cell.Comparison == EObjectColumnCellValueComparison::MatchEqual)
				&& (Cell.Value.ToSoftObjectPath() == NewSoftPath);
			Cell.Comparison = EObjectColumnCellValueComparison::MatchEqual;
			Cell.Value = NewSoftPath;
			bAnyChange = !bAlreadyExisted;
		}
		else if (ColStruct == FOutputObjectColumn::StaticStruct())
		{
			FOutputObjectColumn& C = Col.GetMutable<FOutputObjectColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			FString AssetPath;
			Params->TryGetStringField(TEXT("asset_path"), AssetPath);
			if (AssetPath.IsEmpty())
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(OutputObject): 'asset_path' is required"), Command));
			}
			UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
			if (!Asset)
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(OutputObject): could not load UObject at %s"), Command, *AssetPath));
			}
			if (!Cell.Value.IsValid() || Cell.Value.GetScriptStruct() != FAssetChooser::StaticStruct())
			{
				Cell.Value.InitializeAs(FAssetChooser::StaticStruct());
			}
			FAssetChooser& AssetChooser = Cell.Value.GetMutable<FAssetChooser>();
			bAlreadyExisted = (AssetChooser.Asset == Asset);
			AssetChooser.Asset = Asset;
			bAnyChange = !bAlreadyExisted;
		}
		else if (ColStruct == FOutputFloatColumn::StaticStruct())
		{
			FOutputFloatColumn& C = Col.GetMutable<FOutputFloatColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			if (!Params->HasField(TEXT("default_value")))
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(OutputFloat): 'default_value' is required (number)"), Command));
			}
			double V = 0.0; Params->TryGetNumberField(TEXT("default_value"), V);
			bAlreadyExisted = (Cell == V);
			Cell = V;
			bAnyChange = !bAlreadyExisted;
		}
		else if (ColStruct == FOutputBoolColumn::StaticStruct())
		{
			FOutputBoolColumn& C = Col.GetMutable<FOutputBoolColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			if (!Params->HasField(TEXT("default_value")))
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(OutputBool): 'default_value' is required (bool)"), Command));
			}
			bool V = false; Params->TryGetBoolField(TEXT("default_value"), V);
			bAlreadyExisted = (Cell == V);
			Cell = V;
			bAnyChange = !bAlreadyExisted;
		}
	#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		else if (ColStruct == FChooserOutputNameColumn::StaticStruct())
		{
			FChooserOutputNameColumn& C = Col.GetMutable<FChooserOutputNameColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			// Read, then tested, like the Bool column: a negated read would publish default_value as
			// required for every column type, while most of them never read it.
			FString DV;
			Params->TryGetStringField(TEXT("default_value"), DV);
			if (DV.IsEmpty())
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(OutputName): 'default_value' is required (string)"), Command));
			}
			const FName NewVal(*DV);
			bAlreadyExisted = (Cell == NewVal);
			Cell = NewVal;
			bAnyChange = !bAlreadyExisted;
		}
	#endif
		else if (ColStruct == FObjectClassColumn::StaticStruct())
		{
			FObjectClassColumn& C = Col.GetMutable<FObjectClassColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			FString ClassPath;
			Params->TryGetStringField(TEXT("class_path"), ClassPath);
			if (ClassPath.IsEmpty())
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(ObjectClass): 'class_path' is required"), Command));
			}
			UClass* NewClass = LoadObject<UClass>(nullptr, *ClassPath);
			if (!NewClass)
			{
				NewClass = LoadClass<UObject>(nullptr, *ClassPath);
			}
			if (!NewClass)
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(ObjectClass): could not load UClass at %s"), Command, *ClassPath));
			}
			EObjectClassColumnCellValueComparison NewCmp = EObjectClassColumnCellValueComparison::SubClassOf;
			FString ComparisonStr;
			if (Params->TryGetStringField(TEXT("comparison"), ComparisonStr) && !ComparisonStr.IsEmpty())
			{
				if (ComparisonStr.Equals(TEXT("Equal"), ESearchCase::IgnoreCase))            { NewCmp = EObjectClassColumnCellValueComparison::Equal; }
				else if (ComparisonStr.Equals(TEXT("NotEqual"), ESearchCase::IgnoreCase))    { NewCmp = EObjectClassColumnCellValueComparison::NotEqual; }
				else if (ComparisonStr.Equals(TEXT("SubClassOf"), ESearchCase::IgnoreCase))  { NewCmp = EObjectClassColumnCellValueComparison::SubClassOf; }
				else if (ComparisonStr.Equals(TEXT("NotSubClassOf"), ESearchCase::IgnoreCase)) { NewCmp = EObjectClassColumnCellValueComparison::NotSubClassOf; }
				else if (ComparisonStr.Equals(TEXT("Any"), ESearchCase::IgnoreCase))         { NewCmp = EObjectClassColumnCellValueComparison::Any; }
				else
				{
					return mcp_common::MakeErrorReply(
						FString::Printf(TEXT("%s(ObjectClass): unknown comparison '%s' (expected Equal|NotEqual|SubClassOf|NotSubClassOf|Any)"), Command, *ComparisonStr));
				}
			}
			bAlreadyExisted = (Cell.Comparison == NewCmp) && (Cell.Value == NewClass);
			Cell.Comparison = NewCmp;
			Cell.Value = NewClass;
			bAnyChange = !bAlreadyExisted;
		}
	#if !UE_VERSION_OLDER_THAN(5, 8, 0)
		else if (ColStruct == FChooserNameColumn::StaticStruct())
		{
			FChooserNameColumn& C = Col.GetMutable<FChooserNameColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			bool bSetSomething = false;

			FString ComparisonStr;
			if (Params->TryGetStringField(TEXT("comparison"), ComparisonStr) && !ComparisonStr.IsEmpty())
			{
				ENameColumnCellValueComparison NewCmp = ENameColumnCellValueComparison::MatchEqual;
				if (ComparisonStr.Equals(TEXT("MatchEqual"), ESearchCase::IgnoreCase))       { NewCmp = ENameColumnCellValueComparison::MatchEqual; }
				else if (ComparisonStr.Equals(TEXT("MatchNotEqual"), ESearchCase::IgnoreCase)) { NewCmp = ENameColumnCellValueComparison::MatchNotEqual; }
				else if (ComparisonStr.Equals(TEXT("MatchAny"), ESearchCase::IgnoreCase))    { NewCmp = ENameColumnCellValueComparison::MatchAny; }
				else if (ComparisonStr.Equals(TEXT("Modulus"), ESearchCase::IgnoreCase))     { NewCmp = ENameColumnCellValueComparison::Modulus; }
				else
				{
					return mcp_common::MakeErrorReply(
						FString::Printf(TEXT("%s(Name): unknown comparison '%s' (expected MatchEqual|MatchNotEqual|MatchAny|Modulus)"), Command, *ComparisonStr));
				}
				if (!bAnyChange && Cell.Comparison != NewCmp) { bAnyChange = true; }
				Cell.Comparison = NewCmp;
				bSetSomething = true;
			}
			FString ValueStr;
			if (Params->TryGetStringField(TEXT("value"), ValueStr) && !ValueStr.IsEmpty())
			{
				const FName NewVal(*ValueStr);
				if (!bAnyChange && Cell.Value != NewVal) { bAnyChange = true; }
				Cell.Value = NewVal;
				bSetSomething = true;
			}
			if (!bSetSomething)
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Name): provide at least one of comparison/value"), Command));
			}
			bAlreadyExisted = !bAnyChange;
		}
	#endif
		else if (ColStruct == FOutputEnumColumn::StaticStruct())
		{
			FOutputEnumColumn& C = Col.GetMutable<FOutputEnumColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			if (!Params->HasField(TEXT("value")))
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(OutputEnum): 'value' is required (int, 0-255)"), Command));
			}
			int32 V = 0; Params->TryGetNumberField(TEXT("value"), V);
			const uint8 NewVal = static_cast<uint8>(FMath::Clamp(V, 0, 255));
			bAlreadyExisted = (Cell.Value == NewVal);
			Cell.Value = NewVal;
			bAnyChange = !bAlreadyExisted;
		}
		else if (ColStruct == FMultiEnumColumn::StaticStruct())
		{
			FMultiEnumColumn& C = Col.GetMutable<FMultiEnumColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			if (!Params->HasField(TEXT("value")))
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(MultiEnum): 'value' is required (int, bitmask)"), Command));
			}
			int64 V = 0; Params->TryGetNumberField(TEXT("value"), V);
			const uint32 NewVal = static_cast<uint32>(V);
			bAlreadyExisted = (Cell.Value == NewVal);
			Cell.Value = NewVal;
			bAnyChange = !bAlreadyExisted;
		}
		else if (ColStruct == FRandomizeColumn::StaticStruct())
		{
			FRandomizeColumn& C = Col.GetMutable<FRandomizeColumn>();
			auto& Cell = Chooser_CellRef(C, RowIndex);
			if (!Params->HasField(TEXT("default_value")))
			{
				return mcp_common::MakeErrorReply(
					FString::Printf(TEXT("%s(Randomize): 'default_value' is required (number, probability weight)"), Command));
			}
			double V = 0.0; Params->TryGetNumberField(TEXT("default_value"), V);
			const float NewVal = static_cast<float>(V);
			bAlreadyExisted = FMath::IsNearlyEqual(Cell, NewVal);
			Cell = NewVal;
			bAnyChange = !bAlreadyExisted;
		}
		else
		{
			return mcp_common::MakeErrorReply(
				FString::Printf(TEXT("%s: column type '%s' is not supported (FloatRange|Bool|Enum|Object|OutputObject|OutputFloat|OutputBool|OutputName|ObjectClass|Name|OutputEnum|MultiEnum|Randomize only)"), Command, *ColumnType));
		}

		bOutAnyChange = bAnyChange;
		bOutAlreadyExisted = bAlreadyExisted;
		return nullptr;
	}
}

// MCP:set_chooser_column_default - Set the DefaultRowValue of one UChooserTable column, the cell value rows appended by add_chooser_row inherit; set it before adding rows (a row already there: set_chooser_cell). The fields that apply depend on the column type (see the params); others are ignored. A change is one undo step and is compiled and saved, and success is false, with save_error, when the save fails; a value the column already holds changes nothing (already_existed true, saved false).
//   @param table_path - Long package path of an existing UChooserTable.
//   @param column_index - 0-based column index to update.
//   @param min - FloatRange column: default lower bound of the row's float range.
//   @param max - FloatRange column: default upper bound of the row's float range.
//   @param b_no_min - FloatRange column: when true, disables the lower bound (row matches any value <= max).
//   @param b_no_max - FloatRange column: when true, disables the upper bound (row matches any value >= min).
//   @param default_value - Bool column: "MatchFalse", "MatchTrue" or "MatchAny"; OutputFloat: a number; OutputBool: a bool; OutputName: a name string; Randomize: a probability weight. Ignored by other column types.
//   @param comparison - Enum and Name columns: "MatchEqual", "MatchNotEqual", "MatchAny" or "Modulus"; ObjectClass column: "Equal", "NotEqual", "SubClassOf" (default), "NotSubClassOf" or "Any". Case-insensitive.
//   @param value - Enum and OutputEnum columns: enum ordinal (clamped to 0-255); Name column: the name string to compare; MultiEnum column: the bitmask as an integer.
//   @param asset_path - Object/OutputObject column: long package path of the UObject bound as the default row value.
//   @param class_path - ObjectClass column: path of the UClass to match (e.g. "/Script/Engine.Pawn" or "/Game/BP_Enemy.BP_Enemy_C"), used with comparison (default SubClassOf).
//   @returns { table_path, column_index, column_type, default_value?, asset_path?, comparison?, already_existed, saved, save_error? }

TSharedPtr<FJsonObject> HandleSetChooserColumnDefault(const TSharedPtr<FJsonObject>& Params)
{
	UChooserTable* Table = nullptr;
	int32 ColumnIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> LoadError;
	FInstancedStruct* ColPtr = Chooser_LoadColumnParam(Params, TEXT("set_chooser_column_default"), Table, ColumnIndex, LoadError);
	if (!ColPtr)
	{
		return LoadError;
	}
	// The new default is written into a copy of the column. The table takes it only when it differs, inside a
	// transaction opened before that write, so a refused or no-op request leaves the table, its undo history and
	// its package untouched.
	FInstancedStruct Col = *ColPtr;
	const UScriptStruct* ColStruct = Col.GetScriptStruct();
	const FString ColumnType = ColStruct->GetName();

	bool bAlreadyExisted = false;
	bool bAnyChange = false;

	if (TSharedPtr<FJsonObject> Refused = Chooser_ApplyCellParams(Params, TEXT("set_chooser_column_default"), Col, INDEX_NONE, bAnyChange, bAlreadyExisted))
	{
		return Refused;
	}

	bool bSaved = false;
	FString SaveError;
	if (bAnyChange)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetChooserColumnDefault", "Set Chooser Column Default (MCP)"));
		Table->Modify();
		*ColPtr = Col;
		bSaved = Chooser_CompileAndSave(Table, SaveError);
	}

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("table_path"), Table->GetPathName());
	Resp->SetNumberField(TEXT("column_index"), ColumnIndex);
	Resp->SetStringField(TEXT("column_type"), ColumnType);
	Resp->SetBoolField(TEXT("already_existed"), bAlreadyExisted);

	// Echo the actual set value so a caller can verify it landed: a Bool or Object column in its own
	// shape, every other type in the cell shape of get_chooser_row_values.
	if (ColStruct == FBoolColumn::StaticStruct())
	{
		Resp->SetStringField(TEXT("default_value"), Chooser_BoolCellName(Col.GetPtr<FBoolColumn>()->DefaultRowValue));
	}
	else if (ColStruct == FObjectColumn::StaticStruct())
	{
		const FObjectColumn& C = *Col.GetPtr<FObjectColumn>();
		Resp->SetStringField(TEXT("asset_path"),
			C.DefaultRowValue.Value.IsValid() ? C.DefaultRowValue.Value.ToString() : FString());
		Resp->SetStringField(TEXT("comparison"), Chooser_ObjectComparisonName(C.DefaultRowValue.Comparison));
	}
	else
	{
		Chooser_VisitColumn(Col, [&Resp](const auto& Column) { Chooser_WriteCell(Resp, Column, Column.DefaultRowValue); });
	}

	if (!bAnyChange)
	{
		// Nothing was written, so nothing was saved.
		Resp->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Resp);
	}
	return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_chooser_column_default"), SaveError);
}

// MCP:set_chooser_cell - Set the cell of one existing row in one column of a UChooserTable -- a filter cell (FloatRange range, Bool match, Enum or Name comparison and value, Object or ObjectClass match) or an output cell -- as one undo step, then compile and save the table. It takes the same typed fields as set_chooser_column_default, which sets the value rows added LATER inherit; this one edits a row already there (set_chooser_row_outcome sets a row's result object). The fields that apply depend on the column type; default_value carries the cell of a Bool (MatchFalse|MatchTrue|MatchAny), OutputFloat, OutputBool, OutputName or Randomize column. A row or column index out of range, a missing or ill-typed field, or a column type no tool writes is an error and nothing changes; a cell already holding the value changes nothing (already_existed true, saved false). The reply re-reads the cell in the shape get_chooser_row_values gives; a failed save makes success false with save_error.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param row_index [int] - 0-based row whose cell to set (get_chooser_table_info gives the row count).
//   @param column_index - 0-based column of the cell.
//   @param min - FloatRange column: lower bound of the row's float range.
//   @param max - FloatRange column: upper bound of the row's float range.
//   @param b_no_min - FloatRange column: when true, the range has no lower bound.
//   @param b_no_max - FloatRange column: when true, the range has no upper bound.
//   @param default_value - The cell value of a Bool column ("MatchFalse", "MatchTrue" or "MatchAny"), OutputFloat (a number), OutputBool (a bool), OutputName (a name string) or Randomize column (a probability weight).
//   @param comparison - Enum and Name columns: "MatchEqual", "MatchNotEqual", "MatchAny" or "Modulus"; ObjectClass column: "Equal", "NotEqual", "SubClassOf" (default), "NotSubClassOf" or "Any". Case-insensitive.
//   @param value - Enum and OutputEnum columns: enum ordinal (clamped to 0-255); Name column: the name string to compare; MultiEnum column: the bitmask as an integer.
//   @param asset_path - Object/OutputObject column: long package path of the UObject the cell holds.
//   @param class_path - ObjectClass column: path of the UClass to match, used with comparison (default SubClassOf).
//   @returns { table_path, row_index, column_index, column_type, <typed cell fields as get_chooser_row_values>, already_existed, saved, save_error? }

TSharedPtr<FJsonObject> HandleSetChooserCell(const TSharedPtr<FJsonObject>& Params)
{
	int32 RowIndex = INDEX_NONE;
	if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("row_index"), RowIndex)) { return Missing; }
	UChooserTable* Table = nullptr;
	int32 ColumnIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> LoadError;
	FInstancedStruct* ColPtr = Chooser_LoadColumnParam(Params, TEXT("set_chooser_cell"), Table, ColumnIndex, LoadError);
	if (!ColPtr)
	{
		return LoadError;
	}
	const UScriptStruct* ColStruct = ColPtr->GetScriptStruct();
	const FString ColumnType = ColStruct->GetName();

	// The row has to be there in this column (its own row array: RowValuesWithAny for a Bool column).
	int32 RowCount = INDEX_NONE;
	Chooser_VisitColumn(*ColPtr, [&RowCount](const auto& Column) { RowCount = Chooser_RowArray(Column).Num(); });
	if (RowCount == INDEX_NONE)
	{
		return mcp_common::MakeErrorReply(FString::Printf(TEXT("set_chooser_cell: column type '%s' is not supported; nothing was changed"), *ColumnType));
	}
	if (RowIndex < 0 || RowIndex >= RowCount)
	{
		return mcp_common::MakeErrorReply(FString::Printf(
			TEXT("set_chooser_cell: row_index %d out of range [0, %d) for column %d; nothing was changed"), RowIndex, RowCount, ColumnIndex));
	}

	// Written into a copy of the column; the table takes it only when the cell changes, as set_chooser_column_default does.
	FInstancedStruct Col = *ColPtr;
	bool bAlreadyExisted = false;
	bool bAnyChange = false;
	if (TSharedPtr<FJsonObject> Refused = Chooser_ApplyCellParams(Params, TEXT("set_chooser_cell"), Col, RowIndex, bAnyChange, bAlreadyExisted))
	{
		return Refused;
	}

	bool bSaved = false;
	FString SaveError;
	if (bAnyChange)
	{
		const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetChooserCell", "Set Chooser Cell (MCP)"));
		Table->Modify();
		*ColPtr = Col;
		bSaved = Chooser_CompileAndSave(Table, SaveError);
	}

	// The re-read: the cell as the table holds it now.
	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("table_path"), Table->GetPathName());
	Resp->SetNumberField(TEXT("row_index"), RowIndex);
	Resp->SetNumberField(TEXT("column_index"), ColumnIndex);
	Resp->SetStringField(TEXT("column_type"), ColumnType);
	Chooser_VisitColumn(Table->ColumnsStructs[ColumnIndex], [&Resp, RowIndex](const auto& Column)
	{
		Chooser_WriteCell(Resp, Column, Chooser_RowArray(Column)[RowIndex]);
	});
	Resp->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
	if (!bAnyChange)
	{
		Resp->SetBoolField(TEXT("saved"), false);
		return mcp_common::MakeOkReply(Resp);
	}
	return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_chooser_cell"), SaveError);
}

// MCP:evaluate_chooser_table - Actually RUN a UChooserTable and get the SINGLE chosen result, or with all_results EVERY matching result (for a chooser meant to select a set) -- the way to test a chooser you just authored without opening the editor. Headless-safe: it is a pure runtime API, so it needs no PIE and no render. Pass context_object_path (any UObject in the editor world) to seed the evaluation context. A table with no columns, or one where no context property matches, falls through to the table's FallbackResult (reported as fallback_used); a table that matches nothing and has no fallback returns null (an empty results list with all_results). With all_results, null entries the engine interleaves in its array are skipped, so result_count is the count of REAL results, and result_object_path is the first of them.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param context_object_path - Optional UObject path used to seed the evaluation context.
//   @param object_class - Optional UClass path for the expected result type (default = /Script/CoreUObject.Object); results are filtered with IsA.
//   @param all_results - Optional bool, default false: true evaluates with EvaluateChooserMulti and returns every matching result in results.
//   @returns { table_path, evaluated, result_object_path, result_object_class, fallback_used, column_count, result_count?: int, results?: [{ result_object_path, result_object_class }], note? }
//   NOTE (impl): wraps UChooserFunctionLibrary::EvaluateChooser, or EvaluateChooserMulti with all_results (both BlueprintThreadSafe).

TSharedPtr<FJsonObject> HandleEvaluateChooserTable(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Error;
	UChooserTable* Table = Chooser_LoadTableParam(Params, TEXT("evaluate_chooser_table"), Error);
	if (!Table)
	{
		return Error;
	}

	UObject* ContextObject = nullptr;
	UClass* ResultClass = nullptr;
	if (!Chooser_ReadEvaluationInputs(Params, TEXT("evaluate_chooser_table"), ContextObject, ResultClass, Error))
	{
		return Error;
	}

	bool bAllResults = false;
	Params->TryGetBoolField(TEXT("all_results"), bAllResults);

	// Snapshot FallbackResult presence BEFORE evaluation so we can detect a fallback path.
	const bool bFallbackPresent = Table->FallbackResult.IsValid()
		&& Table->FallbackResult.GetScriptStruct() != nullptr;

	UObject* Result = nullptr;
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	if (bAllResults)
	{
		// The engine can interleave nullptr entries; skip them when building the payload.
		for (UObject* R : UChooserFunctionLibrary::EvaluateChooserMulti(ContextObject, Table, TSubclassOf<UObject>(ResultClass)))
		{
			if (!R) { continue; }
			if (!Result) { Result = R; }
			TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
			RObj->SetStringField(TEXT("result_object_path"), R->GetPathName());
			RObj->SetStringField(TEXT("result_object_class"), R->GetClass()->GetPathName());
			ResultsArr.Add(MakeShared<FJsonValueObject>(RObj));
		}
	}
	else
	{
		Result = UChooserFunctionLibrary::EvaluateChooser(ContextObject, Table, TSubclassOf<UObject>(ResultClass));
	}
	const bool bResultValid = (Result != nullptr);

	// Heuristic for fallback_used: no context was provided AND the table has no columns
	// (so no filter could have matched), and a FallbackResult is configured. With no
	// context properties bound, this is the only way the eval can return a non-null
	// UObject — i.e. it came from FallbackResult.
	const bool bNoContextAndNoColumns = (ContextObject == nullptr) && (Table->ColumnsStructs.Num() == 0);
	const bool bFallbackUsed = bResultValid && bNoContextAndNoColumns && bFallbackPresent;

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	if (bAllResults)
	{
		Resp->SetNumberField(TEXT("result_count"), ResultsArr.Num());
		Resp->SetArrayField(TEXT("results"), ResultsArr);
	}
	Resp->SetStringField(TEXT("table_path"), Table->GetPathName());
	Resp->SetBoolField(TEXT("evaluated"), true);
	Resp->SetStringField(TEXT("result_object_path"),
		bResultValid ? Result->GetPathName() : FString());
	Resp->SetStringField(TEXT("result_object_class"),
		bResultValid ? Result->GetClass()->GetPathName() : FString());
	Resp->SetBoolField(TEXT("fallback_used"), bFallbackUsed);
	Resp->SetNumberField(TEXT("column_count"), Table->ColumnsStructs.Num());
	if (!bResultValid && !bFallbackPresent)
	{
		Resp->SetStringField(TEXT("note"),
			TEXT("no row matched and FallbackResult is unset; result is null"));
	}
	return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// Per-row / per-column readers. The readers walk the
// ungated RowValues arrays (RowValuesWithAny for Bool) and write each cell with
// Chooser_WriteCell.
// ---------------------------------------------------------------------------

namespace
{
	/** Short struct name of a column ("FloatRangeColumn" / "BoolColumn" / ...). */
	FString ChooserColumnTypeName(const FInstancedStruct& Col)
	{
		return Col.GetScriptStruct() ? Col.GetScriptStruct()->GetName() : FString();
	}

	/**
	 * Write the typed cell fields for row RowIndex of a single chooser column into Obj
	 * (no column_type/column_index — callers own those). Returns true if the row index
	 * was in range for the column's array; false means out-of-bounds or an unsupported
	 * column type (caller decides how to signal it).
	 */
	bool WriteChooserRowCellFields(const TSharedPtr<FJsonObject>& Obj, const FInstancedStruct& Col, int32 RowIndex)
	{
		bool bWritten = false;
		Chooser_VisitColumn(Col, [&Obj, RowIndex, &bWritten](const auto& Column)
		{
			const auto& Rows = Chooser_RowArray(Column);
			if (Rows.IsValidIndex(RowIndex))
			{
				Chooser_WriteCell(Obj, Column, Rows[RowIndex]);
				bWritten = true;
			}
		});
		return bWritten;
	}
}

// MCP:get_chooser_row_values - Read ONE ROW ACROSS ALL COLUMNS of a UChooserTable, as typed cells -- the row-wise counterpart of get_chooser_column_values (one column across all rows) and finer-grained than get_chooser_table_info. Cell shapes by column type: FloatRange {min,max,b_no_min,b_no_max}; Bool {cell_value}; Enum {comparison,value}; Object {comparison,asset_path}; OutputObject {asset_path}. row_index out of range is an honest error. Read-only -- no compile, no save -- and it works headless, since row data is not editor-only. A cell of a row is written with set_chooser_cell.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param row_index - 0-based row index to read across all columns.
//   @returns { table_path, row_index, row_count, cells: [{ column_index, column_type, <typed fields> }] }
//   NOTE (impl): walks ColumnsStructs and reads RowValues[row_index] (RowValuesWithAny for Bool) per supported column type.
TSharedPtr<FJsonObject> HandleGetChooserRowValues(const TSharedPtr<FJsonObject>& Params)
{
	FString TablePath;
	if (TSharedPtr<FJsonObject> Missing = Chooser_ReadTablePath(Params, TEXT("get_chooser_row_values"), TablePath))
	{
		return Missing;
	}
	int32 RowIndex = INDEX_NONE;
	if (!Params->TryGetNumberField(TEXT("row_index"), RowIndex))
	{
		return mcp_common::MakeErrorReply(TEXT("get_chooser_row_values: 'row_index' is required (int)"));
	}

	TSharedPtr<FJsonObject> LoadError;
	UChooserTable* Table = Chooser_LoadTable(TablePath, TEXT("get_chooser_row_values"), LoadError);
	if (!Table)
	{
		return LoadError;
	}

	const int32 RowCount = EffectiveRowCount(Table);
	if (RowIndex < 0 || RowIndex >= RowCount)
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("get_chooser_row_values: row_index %d out of range [0, %d)"), RowIndex, RowCount));
	}

	TArray<TSharedPtr<FJsonValue>> Cells;
	for (int32 i = 0; i < Table->ColumnsStructs.Num(); ++i)
	{
		const FInstancedStruct& Col = Table->ColumnsStructs[i];
		const FString ColType = ChooserColumnTypeName(Col);

		TSharedPtr<FJsonObject> CellObj = MakeShared<FJsonObject>();
		CellObj->SetNumberField(TEXT("column_index"), i);
		CellObj->SetStringField(TEXT("column_type"), ColType);
		if (!WriteChooserRowCellFields(CellObj, Col, RowIndex))
		{
			// Column type unsupported for typed read, or its row array is shorter than RowIndex.
			CellObj->SetStringField(TEXT("note"),
				TEXT("no typed cell (unsupported column type or row not present in this column)"));
		}
		Cells.Add(MakeShared<FJsonValueObject>(CellObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), Table->GetPathName());
	Data->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetNumberField(TEXT("row_count"), RowCount);
	Data->SetArrayField(TEXT("cells"), Cells);
	return mcp_common::MakeOkReply(Data);
}

// MCP:get_chooser_column_values - Read ONE COLUMN ACROSS ALL ROWS of a UChooserTable, plus that column's DefaultRowValue -- the column-wise counterpart of get_chooser_row_values (one row across all columns), with the same typed cell shapes. The per-column default is EDITOR-ONLY: outside an editor build it comes back null with a note, while the row values themselves always resolve. column_index out of range is an honest error. Read-only: no compile, no save.
//   @param table_path - Long package path of an existing UChooserTable.
//   @param column_index - 0-based column index to read.
//   @returns { table_path, column_index, column_type, row_count, default: {<typed>}|null, row_values: [{<typed>}] }
TSharedPtr<FJsonObject> HandleGetChooserColumnValues(const TSharedPtr<FJsonObject>& Params)
{
	UChooserTable* Table = nullptr;
	int32 ColumnIndex = INDEX_NONE;
	TSharedPtr<FJsonObject> LoadError;
	const FInstancedStruct* ColPtr = Chooser_LoadColumnParam(Params, TEXT("get_chooser_column_values"), Table, ColumnIndex, LoadError);
	if (!ColPtr)
	{
		return LoadError;
	}
	const FInstancedStruct& Col = *ColPtr;
	const FString ColumnType = Col.GetScriptStruct()->GetName();

	// Per-column row array length (Bool uses RowValuesWithAny), for the supported types only.
	int32 ColRowCount = 0;
	if (!Chooser_VisitColumn(Col, [&ColRowCount](const auto& Column) { ColRowCount = Chooser_RowArray(Column).Num(); }))
	{
		return mcp_common::MakeErrorReply(
			FString::Printf(TEXT("get_chooser_column_values: column type '%s' is not supported (FloatRange|Bool|Enum|Object|OutputObject|OutputFloat|OutputBool|OutputName|ObjectClass|Name|OutputEnum|MultiEnum|Randomize only)"), *ColumnType));
	}

	TArray<TSharedPtr<FJsonValue>> RowValuesArr;
	for (int32 r = 0; r < ColRowCount; ++r)
	{
		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		WriteChooserRowCellFields(RowObj, Col, r);
		RowValuesArr.Add(MakeShared<FJsonValueObject>(RowObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), Table->GetPathName());
	Data->SetNumberField(TEXT("column_index"), ColumnIndex);
	Data->SetStringField(TEXT("column_type"), ColumnType);
	Data->SetNumberField(TEXT("row_count"), ColRowCount);
	Data->SetArrayField(TEXT("row_values"), RowValuesArr);

	// DefaultRowValue is WITH_EDITORONLY_DATA on every column struct.
	TSharedPtr<FJsonObject> DefaultObj = MakeShared<FJsonObject>();
	Chooser_VisitColumn(Col, [&DefaultObj](const auto& Column) { Chooser_WriteCell(DefaultObj, Column, Column.DefaultRowValue); });
	Data->SetObjectField(TEXT("default"), DefaultObj);

	return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
	{ TEXT("add_chooser_column"), &HandleAddChooserColumnToTable },
	{ TEXT("add_chooser_row"), &HandleAddChooserRow },
	{ TEXT("create_chooser_table"), &HandleCreateChooserTable },
	{ TEXT("delete_chooser_table"), &HandleDeleteChooserTable },
	{ TEXT("evaluate_chooser_table"), &HandleEvaluateChooserTable },
	{ TEXT("get_chooser_column_values"), &HandleGetChooserColumnValues },
	{ TEXT("get_chooser_row_values"), &HandleGetChooserRowValues },
	{ TEXT("get_chooser_table_info"), &HandleInspectChooserTable },
	{ TEXT("list_chooser_tables"), &HandleListChooserTables },
	{ TEXT("remove_chooser_column"), &HandleRemoveChooserColumn },
	{ TEXT("remove_chooser_row"), &HandleRemoveChooserRow },
	{ TEXT("set_chooser_cell"), &HandleSetChooserCell },
	{ TEXT("set_chooser_column_default"), &HandleSetChooserColumnDefault },
	{ TEXT("set_chooser_row_outcome"), &HandleSetChooserRowOutcome },
};
} // namespace UEAgentChooserCommands
