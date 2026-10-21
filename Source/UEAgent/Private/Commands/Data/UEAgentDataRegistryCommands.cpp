// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Misc/EngineVersionComparison.h"
#include "Commands/Data/DataAuthoringUtils.h"       // OtherClassAtPathReply

// DataRegistry plugin (DataRegistry module — Build.cs dep; plugin must be enabled)
#include "DataRegistry.h"                    // UDataRegistry
#include "DataRegistrySource.h"              // UDataRegistrySource
#include "DataRegistrySource_DataTable.h"    // UDataRegistrySource_DataTable, FDataRegistrySource_DataTableRules
#include "DataRegistrySubsystem.h"           // UDataRegistrySubsystem
#include "DataRegistryId.h"                  // FDataRegistryId, FDataRegistryType
#include "DataRegistryTypes.h"               // FDataRegistryCacheGetResult, FDataRegistryIdFormat

// Engine
#include "Engine/DataTable.h"                // UDataTable
#include "UObject/UnrealType.h"              // FArrayProperty, FObjectPropertyBase, FNameProperty
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

/**
 * Data Registry authoring and runtime commands.
 *
 * Wraps the DataRegistry plugin to author UDataRegistry assets, attach and remove DataTable sources,
 * register/initialize registries through the UDataRegistrySubsystem, inspect their metadata, and perform
 * synchronous cached item lookups.
 *
 * Engine APIs (DataRegistry plugin, all DATAREGISTRY_API):
 *   - UDataRegistry: the authoring fields RegistryType/IdFormat/ItemStruct/DataSources are protected, so
 *     they are set via reflection; the public getters GetRegistryType()/GetItemStruct()/GetIdFormat()/
 *     IsInitialized()/GetItemNames() read them back.
 *   - UDataRegistrySource_DataTable::SetSourceTable. The class is Within=DataRegistry: its Outer MUST be
 *     the registry.
 *   - UDataRegistrySubsystem::Get()/LoadRegistryPath()/GetAllRegistries()/GetRegistryForType()/
 *     GetCachedItemRaw().
 */
namespace UEAgentDataRegistryCommands
{

// MCP-DOMAIN: DataAsset

namespace
{
    // Resolve a UScriptStruct from a short or full name (mirrors create_data_table's lookup).
    UScriptStruct* ResolveItemStruct(const FString& StructName)
    {
        if (StructName.IsEmpty())
        {
            return nullptr;
        }
        if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*StructName))
        {
            return Found;
        }
        if (UScriptStruct* Found = UClass::TryFindTypeSlow<UScriptStruct>(StructName))
        {
            return Found;
        }
        // Allow a full object path (e.g. /Script/Engine.MirrorTableRow).
        if (StructName.Contains(TEXT(".")) || StructName.StartsWith(TEXT("/")))
        {
            if (UScriptStruct* Loaded = LoadObject<UScriptStruct>(nullptr, *StructName))
            {
                return Loaded;
            }
        }
        return nullptr;
    }

    // Append a source into the registry's protected DataSources array via reflection.
    // Returns true on success and fills OutError otherwise.
    bool AppendDataSourceReflective(UDataRegistry* Registry, UDataRegistrySource* Source, int32& OutCount, FString& OutError)
    {
        FArrayProperty* SourcesProp = FindFProperty<FArrayProperty>(
            UDataRegistry::StaticClass(), TEXT("DataSources"));
        if (!SourcesProp)
        {
            OutError = TEXT("Reflection failed: UDataRegistry.DataSources property not found");
            return false;
        }

        void* ArrayPtr = SourcesProp->ContainerPtrToValuePtr<void>(Registry);
        FScriptArrayHelper Helper(SourcesProp, ArrayPtr);

        FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(SourcesProp->Inner);
        if (!InnerObjProp)
        {
            OutError = TEXT("Reflection failed: DataSources inner is not an object property");
            return false;
        }

        const int32 NewIndex = Helper.AddValue();
        void* ElemPtr = Helper.GetRawPtr(NewIndex);
        InnerObjProp->SetObjectPropertyValue(ElemPtr, Source);

        OutCount = Helper.Num();
        return true;
    }

    // Remove a single entry from the registry's protected DataSources array via
    // reflection. This is the true inverse of AppendDataSourceReflective: it mutates
    // the same protected TArray<TObjectPtr<UDataRegistrySource>> in place (the array
    // is protected, so FScriptArrayHelper is the only headless-safe write path).
    // Caller guarantees Index is in-bounds; fills OutCount with the resulting length.
    bool RemoveDataSourceAtReflective(UDataRegistry* Registry, int32 Index, int32& OutCount, FString& OutError)
    {
        FArrayProperty* SourcesProp = FindFProperty<FArrayProperty>(
            UDataRegistry::StaticClass(), TEXT("DataSources"));
        if (!SourcesProp)
        {
            OutError = TEXT("Reflection failed: UDataRegistry.DataSources property not found");
            return false;
        }

        void* ArrayPtr = SourcesProp->ContainerPtrToValuePtr<void>(Registry);
        FScriptArrayHelper Helper(SourcesProp, ArrayPtr);

        if (!Helper.IsValidIndex(Index))
        {
            OutError = FString::Printf(
                TEXT("Reflection failed: DataSources index %d out of bounds (num=%d)"),
                Index, Helper.Num());
            return false;
        }

        Helper.RemoveValues(Index, 1);
        OutCount = Helper.Num();
        return true;
    }

    // Read the registry's protected DataSources array via reflection (for info reporting). Every slot is kept,
    // a null one as nullptr, so an index into OutSources is the DataSources[] slot index.
    void ReadDataSourcesReflective(UDataRegistry* Registry, TArray<UDataRegistrySource*>& OutSources)
    {
        FArrayProperty* SourcesProp = FindFProperty<FArrayProperty>(
            UDataRegistry::StaticClass(), TEXT("DataSources"));
        if (!SourcesProp)
        {
            return;
        }
        void* ArrayPtr = SourcesProp->ContainerPtrToValuePtr<void>(Registry);
        FScriptArrayHelper Helper(SourcesProp, ArrayPtr);
        FObjectPropertyBase* InnerObjProp = CastField<FObjectPropertyBase>(SourcesProp->Inner);
        if (!InnerObjProp)
        {
            return;
        }
        for (int32 i = 0; i < Helper.Num(); ++i)
        {
            OutSources.Add(Cast<UDataRegistrySource>(InnerObjProp->GetObjectPropertyValue(Helper.GetRawPtr(i))));
        }
    }

    // The path of a source's SourceTable property, read reflectively whatever the source class (a hard or soft
    // object property; the DataTable and CurveTable sources both have one). False when the class has none.
    bool ReadSourceTableReflective(const UDataRegistrySource* Src, FString& OutPath)
    {
        FProperty* TableProp = Src ? Src->GetClass()->FindPropertyByName(TEXT("SourceTable")) : nullptr;
        if (!TableProp)
        {
            return false;
        }
        const void* ValuePtr = TableProp->ContainerPtrToValuePtr<void>(Src);
        if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(TableProp))
        {
            OutPath = SoftProp->GetPropertyValue(ValuePtr).ToString();
            return true;
        }
        if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(TableProp))
        {
            const UObject* TableObj = ObjProp->GetObjectPropertyValue(ValuePtr);
            OutPath = TableObj ? TableObj->GetPathName() : FString();
            return true;
        }
        return false;
    }

    // Count the DataSources array entries without copying them (avoids allocating
    // a TArray<UDataRegistrySource*> just to read the length).
    int32 CountDataSourcesReflective(UDataRegistry* Registry)
    {
        FArrayProperty* SourcesProp = FindFProperty<FArrayProperty>(
            UDataRegistry::StaticClass(), TEXT("DataSources"));
        if (!SourcesProp)
        {
            return 0;
        }
        void* ArrayPtr = SourcesProp->ContainerPtrToValuePtr<void>(Registry);
        FScriptArrayHelper Helper(SourcesProp, ArrayPtr);
        return Helper.Num();
    }

    // Empty the DataSources array in-place via reflection. The TArray itself
    // is protected, so this is the only headless-safe way.
    void ClearDataSourcesReflective(UDataRegistry* Registry)
    {
        FArrayProperty* SourcesProp = FindFProperty<FArrayProperty>(
            UDataRegistry::StaticClass(), TEXT("DataSources"));
        if (!SourcesProp)
        {
            return;
        }
        void* ArrayPtr = SourcesProp->ContainerPtrToValuePtr<void>(Registry);
        FScriptArrayHelper Helper(SourcesProp, ArrayPtr);
        Helper.EmptyValues();
    }

    // Set the protected RegistryType FName via reflection.
    bool SetRegistryTypeReflective(UDataRegistry* Registry, const FName& TypeName, FString& OutError)
    {
        FNameProperty* TypeProp = FindFProperty<FNameProperty>(
            UDataRegistry::StaticClass(), TEXT("RegistryType"));
        if (!TypeProp)
        {
            OutError = TEXT("Reflection failed: UDataRegistry.RegistryType property not found");
            return false;
        }
        TypeProp->SetPropertyValue_InContainer(Registry, TypeName);
        return true;
    }

    // Set the protected ItemStruct TObjectPtr<const UScriptStruct> via reflection.
    bool SetItemStructReflective(UDataRegistry* Registry, UScriptStruct* ItemStruct, FString& OutError)
    {
        FObjectPropertyBase* StructProp = FindFProperty<FObjectPropertyBase>(
            UDataRegistry::StaticClass(), TEXT("ItemStruct"));
        if (!StructProp)
        {
            OutError = TEXT("Reflection failed: UDataRegistry.ItemStruct property not found");
            return false;
        }
        void* ValuePtr = StructProp->ContainerPtrToValuePtr<void>(Registry);
        StructProp->SetObjectPropertyValue(ValuePtr, ItemStruct);
        return true;
    }

    // The DataRegistry subsystem: null once OutSubsystem holds it, otherwise the error reply.
    TSharedPtr<FJsonObject> DataRegistry_GetSubsystem(UDataRegistrySubsystem*& OutSubsystem)
    {
        OutSubsystem = UDataRegistrySubsystem::Get();
        if (!OutSubsystem)
        {
            return mcp_common::MakeErrorReply(
                TEXT("UDataRegistrySubsystem unavailable (subsystem not initialized -- is the DataRegistry plugin enabled?)"));
        }
        return nullptr;
    }

    // The UDataRegistry at RegistryPath, resolved without creating a package: null once OutRegistry holds it,
    // otherwise the error reply.
    TSharedPtr<FJsonObject> DataRegistry_Resolve(const FString& RegistryPath, UDataRegistry*& OutRegistry)
    {
        OutRegistry = mcp_common::ResolveAsset<UDataRegistry>(RegistryPath);
        if (!OutRegistry)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not load UDataRegistry at %s"), *RegistryPath));
        }
        return nullptr;
    }

    // The UDataRegistry at RegistryPath, loaded: null once OutRegistry holds it, otherwise the error reply.
    TSharedPtr<FJsonObject> DataRegistry_Load(const FString& RegistryPath, UDataRegistry*& OutRegistry)
    {
        OutRegistry = LoadObject<UDataRegistry>(nullptr, *RegistryPath);
        if (!OutRegistry)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("DataRegistry not found: %s"), *RegistryPath));
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// create_data_registry
// Creates a UDataRegistry asset with a registry type and item struct, sets the
// protected authoring fields via reflection, registers + saves the package.
// Idempotent: re-loads and returns an existing asset.
// ---------------------------------------------------------------------------
// MCP:create_data_registry - Create and save a UDataRegistry asset.  The registry's ItemStruct is set to the named UScriptStruct (e.g. "MirrorTableRow"); DataTable sources attached later must use a matching RowStruct. Idempotent: a registry already at the path is returned unchanged with already_exists=true and saved=false (registry_type and item_struct not applied), and nothing is saved; another asset class there is an error. A failed save makes success false with saved=false and save_error. Attach sources with add_data_registry_source, then register it with initialize_data_registry.
//   @param registry_name - Name of the new UDataRegistry asset.
//   @param package_path - Destination /Game folder (default /Game/DataRegistries).
//   @param item_struct - Name of the UScriptStruct used for items (e.g. "MirrorTableRow").
//   @param registry_type - RegistryType FName (defaults to registry_name).
//   @returns { registry_path: string, registry_type: string, item_struct: string, success: bool, already_exists: bool, type_verified: bool, struct_verified: bool, saved: bool, save_error: string, message?: string }

TSharedPtr<FJsonObject> HandleCreateDataRegistry(
    const TSharedPtr<FJsonObject>& Params)
{
    FString RegistryName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("registry_name"), RegistryName)) { return Missing; }

    FString PackagePath = TEXT("/Game/DataRegistries");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString ItemStructName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("item_struct"), ItemStructName)) { return Missing; }

    // registry_type defaults to the registry name if not supplied.
    FString RegistryTypeStr = RegistryName;
    Params->TryGetStringField(TEXT("registry_type"), RegistryTypeStr);

    UScriptStruct* ItemStruct = ResolveItemStruct(ItemStructName);
    if (!ItemStruct)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not resolve item_struct '%s' to a UScriptStruct"), *ItemStructName));
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *RegistryName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: a registry already there is returned unchanged and nothing is saved. A subclass of UDataRegistry
    // is not the asset asked for; an asset of an unrelated class does not resolve here, and NewAssetInPackage
    // below refuses it.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        if (UDataRegistry* Existing = mcp_common::ResolveAsset<UDataRegistry>(FullPath))
        {
            if (Existing->GetClass() != UDataRegistry::StaticClass())
            {
                return DataAuthoringUtils::OtherClassAtPathReply(FullPath, Existing, UDataRegistry::StaticClass());
            }
            TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
            ResultObj->SetStringField(TEXT("registry_path"), Existing->GetPathName());
            ResultObj->SetStringField(TEXT("registry_type"), Existing->GetRegistryType().ToString());
            ResultObj->SetStringField(TEXT("item_struct"),
                Existing->GetItemStruct() ? Existing->GetItemStruct()->GetName() : TEXT("None"));
            ResultObj->SetBoolField(TEXT("success"), true);
            ResultObj->SetBoolField(TEXT("already_exists"), true);
            ResultObj->SetBoolField(TEXT("saved"), false);
            ResultObj->SetStringField(TEXT("message"), TEXT("DataRegistry already exists; returned the existing asset unchanged, nothing saved"));
            return mcp_common::MakeOkReply(ResultObj);
        }
    }

    FString CreateError;
    UDataRegistry* Registry = mcp_common::NewAssetInPackage<UDataRegistry>(FullPath, RegistryName, TEXT("Failed to create UDataRegistry"), CreateError);
    if (!Registry)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // Set the protected authoring fields via reflection.
    FString ReflectError;
    if (!SetRegistryTypeReflective(Registry, FName(*RegistryTypeStr), ReflectError))
    {
        return mcp_common::MakeErrorReply(ReflectError);
    }
    if (!SetItemStructReflective(Registry, ItemStruct, ReflectError))
    {
        return mcp_common::MakeErrorReply(ReflectError);
    }

    FAssetRegistryModule::AssetCreated(Registry);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Registry->GetOutermost(), Registry, SaveError);

    // Read back via the public getters to verify the reflected writes landed.
    const FName VerifyType = Registry->GetRegistryType();
    const UScriptStruct* VerifyStruct = Registry->GetItemStruct();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
    ResultObj->SetStringField(TEXT("registry_type"), VerifyType.ToString());
    ResultObj->SetStringField(TEXT("item_struct"),
        VerifyStruct ? VerifyStruct->GetName() : TEXT("None"));
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("type_verified"), VerifyType == FName(*RegistryTypeStr));
    ResultObj->SetBoolField(TEXT("struct_verified"), VerifyStruct == ItemStruct);
    if (!bSaved && !SaveError.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_data_registry"));
}

// ---------------------------------------------------------------------------
// add_data_registry_source
// Creates a UDataRegistrySource_DataTable (Outer MUST be the registry, since the
// class is Within=DataRegistry), points it at a DataTable via SetSourceTable, and
// appends it into the registry's protected DataSources array via reflection.
// ---------------------------------------------------------------------------
// MCP:add_data_registry_source - Attach a DataTable source to a UDataRegistry.  Creates a UDataRegistrySource_DataTable (owned by the registry) pointing at the given DataTable and appends it to the registry's source list. The DataTable RowStruct must match the registry's ItemStruct.
//   @param registry_path - Content path to the UDataRegistry asset.
//   @param data_table_path - Content path to the source UDataTable.
//   @param precache - bool, enables synchronous cached lookups (default true).
//   @returns { registry_path: string, data_table_path: string, source_name: string, success: bool, precache: bool, source_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddDataRegistrySource(
    const TSharedPtr<FJsonObject>& Params)
{
    FString RegistryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("registry_path"), RegistryPath)) { return Missing; }

    FString TablePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("data_table_path"), TablePath)) { return Missing; }

    UDataRegistry* Registry = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = DataRegistry_Resolve(RegistryPath, Registry)) { return LoadError; }

    UDataTable* Table = Cast<UDataTable>(
        StaticLoadObject(UDataTable::StaticClass(), nullptr, *TablePath));
    if (!Table)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not load UDataTable at %s"), *TablePath));
    }

    // The DataTable RowStruct should match the registry ItemStruct for the source
    // to be useful; report a clean error if they mismatch.
    const UScriptStruct* RegStruct = Registry->GetItemStruct();
    const bool bStructMatch = (RegStruct != nullptr) && (Table->RowStruct == RegStruct);
    if (!bStructMatch)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("DataTable RowStruct '%s' does not match registry ItemStruct '%s'"),
            Table->RowStruct ? *Table->RowStruct->GetName() : TEXT("None"),
            RegStruct ? *RegStruct->GetName() : TEXT("None")));
    }

    Registry->Modify();

    // Outer MUST be the registry (Within=DataRegistry).
    UDataRegistrySource_DataTable* Source = NewObject<UDataRegistrySource_DataTable>(
        Registry, UDataRegistrySource_DataTable::StaticClass(), NAME_None,
        RF_Public | RF_Transactional);
    if (!Source)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to create UDataRegistrySource_DataTable"));
    }

    bool bPrecache = true;
    Params->TryGetBoolField(TEXT("precache"), bPrecache);

    FDataRegistrySource_DataTableRules Rules;
    Rules.bPrecacheTable = bPrecache;

    Source->SetSourceTable(TSoftObjectPtr<UDataTable>(FSoftObjectPath(Table->GetPathName())), Rules);

    int32 SourceCount = 0;
    FString ReflectError;
    if (!AppendDataSourceReflective(Registry, Source, SourceCount, ReflectError))
    {
        return mcp_common::MakeErrorReply(ReflectError);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Registry->GetOutermost(), Registry, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
    ResultObj->SetStringField(TEXT("data_table_path"), Table->GetPathName());
    ResultObj->SetStringField(TEXT("source_name"), Source->GetName());
    ResultObj->SetBoolField(TEXT("success"), true);
    ResultObj->SetBoolField(TEXT("precache"), bPrecache);
    ResultObj->SetNumberField(TEXT("source_count"), SourceCount);
    if (!bSaved && !SaveError.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_data_registry_source"));
}

// ---------------------------------------------------------------------------
// initialize_data_registry
// Loads + registers + initializes a registry through the subsystem so its
// sources become live and (for precached sources) cached for synchronous gets.
// ---------------------------------------------------------------------------
// MCP:initialize_data_registry - Register and initialize a UDataRegistry asset with the UDataRegistrySubsystem for this editor session, so get_data_registry_item can read its precached items: the asset is loaded, its path goes on the subsystem's load list (LoadRegistryPath), the registry map is refreshed and registries not yet initialized are initialized. Nothing is saved or written to the asset, and a repeat call adds nothing (idempotent). loaded, registered and initialized are read back from this asset, and it is a failure, success false with the reason, unless it ends up registered and initialized: another asset holding its registry_type, or no source that initializes, prevents that. A registry that does not load, or a missing subsystem (DataRegistry plugin off), is an error. Create one with create_data_registry and attach sources with add_data_registry_source; after edits use refresh_data_registry_map; state: get_data_registry_info, list_data_registry_types.
//   @param registry_path - Path of the UDataRegistry asset, e.g. "/Game/Data/DR_Items" or "/Game/Data/DR_Items.DR_Items".
//   @returns { registry_path: string, registry_type: string, success: bool, loaded: bool, registered: bool, initialized: bool }
//   @hints additive idempotent

TSharedPtr<FJsonObject> HandleInitializeDataRegistry(
    const TSharedPtr<FJsonObject>& Params)
{
    FString RegistryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("registry_path"), RegistryPath)) { return Missing; }

    UDataRegistrySubsystem* Subsystem = nullptr;
    if (TSharedPtr<FJsonObject> NoSubsystem = DataRegistry_GetSubsystem(Subsystem)) { return NoSubsystem; }

    // Resolve the registry to obtain its canonical object path for LoadRegistryPath.
    UDataRegistry* Registry = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = DataRegistry_Resolve(RegistryPath, Registry)) { return LoadError; }

    // LoadRegistryPath puts the path on the subsystem's load list and, once the subsystem is ready, loads the asset,
    // refreshes the registry map and initializes the registries not yet initialized. Its return value tests
    // AddUnique() != INDEX_NONE, which never fails, so it says nothing and is not read.
    const FSoftObjectPath AssetPath(Registry->GetPathName());
    Subsystem->LoadRegistryPath(AssetPath);
    Subsystem->RefreshRegistryMap();

    // Every flag is read back from THIS asset. The registry map is keyed by registry type, so the registry it holds
    // for this asset's type can be another asset of the same type: registered asks whether this object is in it.
    // GetAllRegistries also flushes a registry load the subsystem deferred.
    TArray<UDataRegistry*> RegisteredRegistries;
    Subsystem->GetAllRegistries(RegisteredRegistries, /*bSortByType*/ false);
    const FName RegType = Registry->GetRegistryType();
    const bool bLoaded = AssetPath.ResolveObject() == Registry;
    const bool bRegistered = RegisteredRegistries.Contains(Registry);
    const bool bInitialized = Registry->IsInitialized();
    const bool bReady = bRegistered && bInitialized;

    FString Failure;
    if (!bRegistered)
    {
        const UDataRegistry* TypeHolder = Subsystem->GetRegistryForType(RegType);
        Failure = TypeHolder
            ? FString::Printf(TEXT("initialize_data_registry: the subsystem holds %s for registry_type '%s', not this asset, so this one is not registered; two registries cannot share one registry_type"),
                *TypeHolder->GetPathName(), *RegType.ToString())
            : FString::Printf(TEXT("initialize_data_registry: the DataRegistry subsystem did not register this asset%s"),
                Subsystem->IsReadyForInitialization() ? TEXT("") : TEXT(" (the subsystem is not ready for initialization yet)"));
    }
    else if (!bInitialized)
    {
        // UDataRegistry::Initialize initializes the registry only when at least one of its sources initializes.
        const int32 SourceCount = CountDataSourcesReflective(Registry);
        Failure = SourceCount == 0
            ? FString(TEXT("initialize_data_registry: the registry is registered but not initialized: it has no source, and a registry initializes only when one of its sources does (attach one with add_data_registry_source)"))
            : FString::Printf(TEXT("initialize_data_registry: the registry is registered but not initialized: none of its %d source(s) initialized"), SourceCount);
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
    ResultObj->SetStringField(TEXT("registry_type"), RegType.ToString());
    ResultObj->SetBoolField(TEXT("success"), bReady);
    ResultObj->SetBoolField(TEXT("loaded"), bLoaded);
    ResultObj->SetBoolField(TEXT("registered"), bRegistered);
    ResultObj->SetBoolField(TEXT("initialized"), bInitialized);
    return mcp_common::MakeOutcomeReply(ResultObj, bReady, Failure);
}

// ---------------------------------------------------------------------------
// get_data_registry_info
// Reads registry type / item struct / id format / initialized state / item
// names and the configured source list.
// ---------------------------------------------------------------------------
// MCP:get_data_registry_info - Read a UDataRegistry asset: registry_type, item_struct, initialized, every DataSources[] slot (index, name, class, data_table, source_table) and item_names. Read-only. source_count counts every slot, a null one included (listed with class "null"). source_table is the SourceTable of any source class that has one (DataTable and CurveTable sources). item_names lists cached items only, so it stays empty until the registry is initialized (initialize_data_registry).
//   @param registry_path - Asset path of the UDataRegistry, e.g. "/Game/Data/DR_Items".
//   @returns { registry_path: string, registry_type: string, item_struct: string, initialized: bool, sources: [{index: int, class: string, name?: string, data_table?: string, source_table?: string}], source_count: int, item_names: [string], item_count: int }

TSharedPtr<FJsonObject> HandleGetDataRegistryInfo(
    const TSharedPtr<FJsonObject>& Params)
{
    FString RegistryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("registry_path"), RegistryPath)) { return Missing; }

    UDataRegistry* Registry = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = DataRegistry_Resolve(RegistryPath, Registry)) { return LoadError; }

    const UScriptStruct* ItemStruct = Registry->GetItemStruct();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
    ResultObj->SetStringField(TEXT("registry_type"), Registry->GetRegistryType().ToString());
    ResultObj->SetStringField(TEXT("item_struct"),
        ItemStruct ? ItemStruct->GetName() : TEXT("None"));
    ResultObj->SetBoolField(TEXT("initialized"), Registry->IsInitialized());

    // Source list (read via reflection), one entry per DataSources[] slot, null slots included.
    TArray<UDataRegistrySource*> Sources;
    ReadDataSourcesReflective(Registry, Sources);
    TArray<TSharedPtr<FJsonValue>> SourcesArr;
    for (int32 Index = 0; Index < Sources.Num(); ++Index)
    {
        const UDataRegistrySource* Src = Sources[Index];
        TSharedPtr<FJsonObject> SrcObj = MakeShared<FJsonObject>();
        SrcObj->SetNumberField(TEXT("index"), Index);
        if (!Src)
        {
            SrcObj->SetStringField(TEXT("class"), TEXT("null"));
            SourcesArr.Add(MakeShared<FJsonValueObject>(SrcObj));
            continue;
        }
        SrcObj->SetStringField(TEXT("name"), Src->GetName());
        SrcObj->SetStringField(TEXT("class"), Src->GetClass()->GetName());
        if (const UDataRegistrySource_DataTable* DTSrc = Cast<UDataRegistrySource_DataTable>(Src))
        {
            SrcObj->SetStringField(TEXT("data_table"), DTSrc->SourceTable.ToString());
        }
        FString SourceTable;
        if (ReadSourceTableReflective(Src, SourceTable))
        {
            SrcObj->SetStringField(TEXT("source_table"), SourceTable);
        }
        SourcesArr.Add(MakeShared<FJsonValueObject>(SrcObj));
    }
    ResultObj->SetArrayField(TEXT("sources"), SourcesArr);
    ResultObj->SetNumberField(TEXT("source_count"), SourcesArr.Num());

    // Item names (populated once the registry is initialized + sources resolved).
    TArray<FName> ItemNames;
    Registry->GetItemNames(ItemNames);
    TArray<TSharedPtr<FJsonValue>> ItemNamesArr;
    for (const FName& N : ItemNames)
    {
        ItemNamesArr.Add(MakeShared<FJsonValueString>(N.ToString()));
    }
    ResultObj->SetArrayField(TEXT("item_names"), ItemNamesArr);
    ResultObj->SetNumberField(TEXT("item_count"), ItemNamesArr.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// get_data_registry_item
// Synchronous precached lookup via UDataRegistrySubsystem::GetCachedItemRaw.
// HONEST ERROR if the item is not currently cached (a precached DataTable source
// caches on initialize; non-precached sources require an async acquire + runtime
// tick which is unavailable in this headless command path).
// ---------------------------------------------------------------------------
// MCP:get_data_registry_item - Synchronously look up a precached item by registry type and item name.  Uses UDataRegistrySubsystem::GetCachedItemRaw. Only returns data for items backed by a precached source (the registry must be initialized first). Returns a clean error if the item is not cached.
//   @param registry_type - The registry type name.
//   @param item_name - The item (row) name.
//   @returns { registry_type: string, item_name: string, item_struct: string, found: bool, item_data: string }

TSharedPtr<FJsonObject> HandleGetDataRegistryItem(
    const TSharedPtr<FJsonObject>& Params)
{
    FString RegistryType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("registry_type"), RegistryType)) { return Missing; }

    FString ItemName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("item_name"), ItemName)) { return Missing; }

    UDataRegistrySubsystem* Subsystem = nullptr;
    if (TSharedPtr<FJsonObject> NoSubsystem = DataRegistry_GetSubsystem(Subsystem)) { return NoSubsystem; }

    const FName RegTypeName(*RegistryType);
    const FName ItemFName(*ItemName);
    const FDataRegistryId ItemId(RegTypeName, ItemFName);

    const uint8* ItemMemory = nullptr;
    const UScriptStruct* ItemStruct = nullptr;
    const FDataRegistryCacheGetResult GetResult =
        Subsystem->GetCachedItemRaw(ItemMemory, ItemStruct, ItemId);

    if (!GetResult.WasFound() || ItemMemory == nullptr || ItemStruct == nullptr)
    {
        // HONEST ERROR — item not precached/cached. allow_errors substring: "not cached".
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Item '%s:%s' is not cached (precached source not initialized, or item requires async acquire)"),
            *RegistryType, *ItemName));
    }

    // Export the cached struct instance to a text representation for readback.
    FString ExportedText;
    ItemStruct->ExportText(ExportedText, ItemMemory, ItemMemory, nullptr, PPF_None, nullptr);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("registry_type"), RegistryType);
    ResultObj->SetStringField(TEXT("item_name"), ItemName);
    ResultObj->SetStringField(TEXT("item_struct"), ItemStruct->GetName());
    ResultObj->SetBoolField(TEXT("found"), true);
    ResultObj->SetStringField(TEXT("item_data"), ExportedText);
    return mcp_common::MakeOkReply(ResultObj);
}

// ---------------------------------------------------------------------------
// list_data_registry_types
// Enumerates all registries currently registered with the subsystem.
// ---------------------------------------------------------------------------
// MCP:list_data_registry_types - List every registry the UDataRegistrySubsystem currently has registered, sorted by type, each with registry_type, registry_path, item_struct and initialized. Read-only. An asset the subsystem has not loaded is absent: register it with initialize_data_registry, or call refresh_data_registry_map after edits.
//   @returns { registries: [{registry_type, registry_path, item_struct, initialized}], registry_count: int }

TSharedPtr<FJsonObject> HandleListDataRegistryTypes(
    const TSharedPtr<FJsonObject>& Params)
{
    UDataRegistrySubsystem* Subsystem = nullptr;
    if (TSharedPtr<FJsonObject> NoSubsystem = DataRegistry_GetSubsystem(Subsystem)) { return NoSubsystem; }

    TArray<UDataRegistry*> AllRegistries;
    Subsystem->GetAllRegistries(AllRegistries, /*bSortByType*/ true);

    TArray<TSharedPtr<FJsonValue>> TypesArr;
    for (UDataRegistry* Reg : AllRegistries)
    {
        if (!Reg)
        {
            continue;
        }
        TSharedPtr<FJsonObject> RegObj = MakeShared<FJsonObject>();
        RegObj->SetStringField(TEXT("registry_type"), Reg->GetRegistryType().ToString());
        RegObj->SetStringField(TEXT("registry_path"), Reg->GetPathName());
        const UScriptStruct* ItemStruct = Reg->GetItemStruct();
        RegObj->SetStringField(TEXT("item_struct"), ItemStruct ? ItemStruct->GetName() : TEXT("None"));
        RegObj->SetBoolField(TEXT("initialized"), Reg->IsInitialized());
        TypesArr.Add(MakeShared<FJsonValueObject>(RegObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("registries"), TypesArr);
    ResultObj->SetNumberField(TEXT("registry_count"), TypesArr.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

// ===========================================================================
// DataRegistry source management advanced
//
// The commands of this section, all headless (the sources themselves are
// enumerated by get_data_registry_info):
//
//   1. remove_data_registry_source    -- drop ONE source from DataSources[], or
//                                          with all=true empty it, and save
//                                          (see its block below). Does NOT touch
//                                          the subsystem registry map (no public
//                                          UnregisterRegistry; the registry will
//                                          still be queryable for already-cached
//                                          items until the editor restarts).
//   2. refresh_data_registry_map      -- call UDataRegistrySubsystem::
//                                          RefreshRegistryMap() to force the
//                                          subsystem to re-read the registry
//                                          map after asset edits.
//
// We do NOT stub unregister-from-subsystem because the engine has no public
// path for it.
// ===========================================================================

// ---------------------------------------------------------------------------
// remove_data_registry_source
// True inverse of add_data_registry_source: a reflective RemoveAt on the asset's
// protected DataSources[] array + save. Does NOT touch the subsystem (that axis
// belongs to initialize_data_registry / IgnoreRegistryPath). Selects the source
// to drop either by matching UDataRegistrySource_DataTable::SourceTable.ToString()
// against data_table_path, or by a direct source_index.
// Idempotent: if no matching source is found (e.g. already removed), returns
// success with removed:false and an unchanged count.
// ---------------------------------------------------------------------------
// MCP:remove_data_registry_source - Remove a DataTable source from a UDataRegistry asset (the true inverse of add_data_registry_source), or with all=true empty every DataSources[] slot.  Does a reflective RemoveAt (or Empty) on the registry's protected DataSources[] array and saves the asset. Select one source either by data_table_path (matched against each DataTable source's SourceTable) or by source_index. Does NOT unregister from the subsystem (there is no public UnregisterRegistry): already-cached items may stay queryable until the editor restarts; refresh_data_registry_map makes the subsystem re-read the registry map. Idempotent: a source that is not present, or all=true on a registry without sources, returns removed:false with an unchanged count.
//   @param registry_path - Content path to the UDataRegistry asset.
//   @param data_table_path - Content path of the source DataTable to remove (matched against SourceTable). Provide this OR source_index OR all.
//   @param source_index - Direct index into DataSources[] to remove. Provide this OR data_table_path OR all.
//   @param all - Optional bool, default false: true removes every source (null slots included) instead of one; it excludes data_table_path and source_index.
//   @returns { registry_path: string, source_index?: int, data_table_path?: string, all?: bool, source_count_before: int, source_count_after: int, removed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleRemoveDataRegistrySource(const TSharedPtr<FJsonObject>& Params)
{
    FString RegistryPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("registry_path"), RegistryPath)) { return Missing; }

    // Exactly one selector: data_table_path (match) OR source_index (direct) OR all.
    FString TablePath;
    const bool bHasTablePath =
        Params->TryGetStringField(TEXT("data_table_path"), TablePath) && !TablePath.IsEmpty();

    int32 RequestedIndex = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("source_index"), RequestedIndex);

    bool bAll = false;
    Params->TryGetBoolField(TEXT("all"), bAll);

    if (bAll && (bHasTablePath || bHasIndex))
    {
        return mcp_common::MakeErrorReply(
            TEXT("all=true removes every source; do not combine it with data_table_path or source_index"));
    }
    if (!bAll && !bHasTablePath && !bHasIndex)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide exactly one of: data_table_path, source_index or all=true"));
    }
    if (bHasTablePath && bHasIndex)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Provide only one of: data_table_path or source_index (not both)"));
    }

    UDataRegistry* Registry = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = DataRegistry_Load(RegistryPath, Registry)) { return LoadError; }

    const int32 BeforeCount = CountDataSourcesReflective(Registry);

    if (bAll)
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
        ResultObj->SetBoolField(TEXT("all"), true);
        ResultObj->SetNumberField(TEXT("source_count_before"), BeforeCount);
        if (BeforeCount == 0)
        {
            // Nothing to empty: idempotent no-op, nothing touched or saved.
            ResultObj->SetNumberField(TEXT("source_count_after"), 0);
            ResultObj->SetBoolField(TEXT("removed"), false);
            ResultObj->SetBoolField(TEXT("saved"), false);
            ResultObj->SetStringField(TEXT("note"), TEXT("no sources to remove"));
            return mcp_common::MakeOkReply(ResultObj);
        }

        Registry->Modify();
        ClearDataSourcesReflective(Registry);
        Registry->MarkPackageDirty();

        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Registry->GetOutermost(), Registry, SaveError);
        const int32 AfterCount = CountDataSourcesReflective(Registry);
        ResultObj->SetNumberField(TEXT("source_count_after"), AfterCount);
        ResultObj->SetBoolField(TEXT("removed"), AfterCount == 0);
        if (!bSaved && !SaveError.IsEmpty())
        {
            ResultObj->SetStringField(TEXT("save_error"), SaveError);
        }
        return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_data_registry_source"));
    }

    // Resolve the target index. -1 means "no matching source" (idempotent no-op).
    int32 TargetIndex = INDEX_NONE;
    FString MatchedTablePath;

    if (bHasIndex)
    {
        if (RequestedIndex < 0 || RequestedIndex >= BeforeCount)
        {
            // Out-of-bounds index: treat as honest no-op (nothing at that slot).
            UE_LOG(LogTemp, Verbose,
                TEXT("remove_data_registry_source: index %d out of bounds (count=%d) on %s"),
                RequestedIndex, BeforeCount, *RegistryPath);
        }
        else
        {
            TargetIndex = RequestedIndex;
            // Report the SourceTable of the slot being removed, if it is a DataTable source.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
            // 5.7 has no UDataRegistry::GetDataSources(); read the same protected DataSources[] by reflection.
            TArray<UDataRegistrySource*> Sources;
            ReadDataSourcesReflective(Registry, Sources);
#else
            const TArray<TObjectPtr<UDataRegistrySource>>& Sources = Registry->GetDataSources();
#endif
            if (Sources.IsValidIndex(TargetIndex))
            {
                if (const UDataRegistrySource_DataTable* DTSrc =
                        Cast<UDataRegistrySource_DataTable>(Sources[TargetIndex]))
                {
                    MatchedTablePath = DTSrc->SourceTable.ToString();
                }
            }
        }
    }
    else // bHasTablePath — match SourceTable.ToString().
    {
        // Normalise the requested table to its full object path so the compare is
        // robust to bare-package vs object-path inputs (headless-asset gotcha).
        FString WantPath = TablePath;
        if (UDataTable* WantTable = LoadObject<UDataTable>(nullptr, *TablePath))
        {
            WantPath = WantTable->GetPathName();
        }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
        // 5.7 has no UDataRegistry::GetDataSources(); read the same protected DataSources[] by reflection.
        TArray<UDataRegistrySource*> Sources;
        ReadDataSourcesReflective(Registry, Sources);
#else
        const TArray<TObjectPtr<UDataRegistrySource>>& Sources = Registry->GetDataSources();
#endif
        int32 MatchCount = 0;
        for (int32 i = 0; i < Sources.Num(); ++i)
        {
            const UDataRegistrySource_DataTable* DTSrc =
                Cast<UDataRegistrySource_DataTable>(Sources[i]);
            if (!DTSrc)
            {
                continue;
            }
            const FString SrcTablePath = DTSrc->SourceTable.ToString();
            // Match against both the raw input and the normalised full path.
            if (SrcTablePath == TablePath || SrcTablePath == WantPath)
            {
                ++MatchCount;
                if (TargetIndex == INDEX_NONE)
                {
                    TargetIndex = i;
                    MatchedTablePath = SrcTablePath;
                }
            }
        }

        if (MatchCount > 1)
        {
            // Ambiguous — refuse rather than guess which one to drop.
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("data_table_path '%s' matches %d sources; disambiguate with source_index"),
                *TablePath, MatchCount));
        }
    }

    // Not found -> idempotent success (removed:false, count unchanged).
    if (TargetIndex == INDEX_NONE)
    {
        UE_LOG(LogTemp, Verbose,
            TEXT("remove_data_registry_source: no matching source on %s (count=%d) -- no-op"),
            *RegistryPath, BeforeCount);
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
        ResultObj->SetNumberField(TEXT("source_index"), -1);
        ResultObj->SetStringField(TEXT("data_table_path"), bHasTablePath ? TablePath : FString());
        ResultObj->SetNumberField(TEXT("source_count_before"), BeforeCount);
        ResultObj->SetNumberField(TEXT("source_count_after"), BeforeCount);
        ResultObj->SetBoolField(TEXT("removed"), false);
        ResultObj->SetBoolField(TEXT("saved"), false);
        ResultObj->SetStringField(TEXT("note"), TEXT("source not found"));
        return mcp_common::MakeOkReply(ResultObj);
    }

    UE_LOG(LogTemp, Verbose,
        TEXT("remove_data_registry_source: removing index %d (table '%s') from %s (count=%d)"),
        TargetIndex, *MatchedTablePath, *RegistryPath, BeforeCount);

    Registry->Modify();

    int32 AfterCount = BeforeCount;
    FString ReflectError;
    if (!RemoveDataSourceAtReflective(Registry, TargetIndex, AfterCount, ReflectError))
    {
        return mcp_common::MakeErrorReply(ReflectError);
    }

    Registry->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Registry->GetOutermost(), Registry, SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("registry_path"), Registry->GetPathName());
    ResultObj->SetNumberField(TEXT("source_index"), TargetIndex);
    ResultObj->SetStringField(TEXT("data_table_path"), MatchedTablePath);
    ResultObj->SetNumberField(TEXT("source_count_before"), BeforeCount);
    ResultObj->SetNumberField(TEXT("source_count_after"), AfterCount);
    ResultObj->SetBoolField(TEXT("removed"), true);
    if (!bSaved && !SaveError.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("remove_data_registry_source"));
}

// MCP:refresh_data_registry_map - Force UDataRegistrySubsystem::RefreshRegistryMap().  Call this after editing registry assets (e.g. add/remove sources, change registry_type) to make the subsystem re-read its registry map without needing an editor restart.
//   @returns { refreshed: bool }

TSharedPtr<FJsonObject> HandleRefreshDataRegistryMap(const TSharedPtr<FJsonObject>& Params)
{
    UDataRegistrySubsystem* Subsystem = nullptr;
    if (TSharedPtr<FJsonObject> NoSubsystem = DataRegistry_GetSubsystem(Subsystem)) { return NoSubsystem; }
    Subsystem->RefreshRegistryMap();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("refreshed"), true);
    return mcp_common::MakeOkReply(ResultObj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_data_registry_source"), &HandleAddDataRegistrySource },
    { TEXT("create_data_registry"), &HandleCreateDataRegistry },
    { TEXT("get_data_registry_info"), &HandleGetDataRegistryInfo },
    { TEXT("get_data_registry_item"), &HandleGetDataRegistryItem },
    { TEXT("initialize_data_registry"), &HandleInitializeDataRegistry },
    { TEXT("list_data_registry_types"), &HandleListDataRegistryTypes },
    { TEXT("refresh_data_registry_map"), &HandleRefreshDataRegistryMap },
    { TEXT("remove_data_registry_source"), &HandleRemoveDataRegistrySource },
};
} // namespace UEAgentDataRegistryCommands
