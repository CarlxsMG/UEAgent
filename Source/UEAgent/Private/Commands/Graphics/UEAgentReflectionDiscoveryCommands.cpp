#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "Engine/World.h" // UWorld — list_objects_of_class' `world` selector holds one and compares it against UObject::GetWorld(); a forward decl would do, but a standalone (-SingleFile) compile must not depend on the unity blob supplying it
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

/**
 * Reflection discovery commands: enumerate the types, live instances, interfaces and modules a project
 * defines -- what list_subsystem_classes does for subsystems, for any UObject-derived hierarchy.
 * Read-only; runtime-safe (no PIE/render dependency).
 */
namespace UEAgentReflectionDiscoveryCommands
{

// MCP-DOMAIN: Reflection

namespace
{
    // Best-effort module name for a UField (UClass/UScriptStruct/UEnum): resolves the
    // /Script/<Module> outermost package name to the short module name. Returns an empty
    // string (never invented) when the outermost isn't a /Script/ package (e.g. a
    // Blueprint-generated type whose outer is a content package).
    FString ResolveModuleName(const UObject* Field)
    {
        if (!Field)
        {
            return FString();
        }
        const UPackage* Package = Field->GetOutermost();
        if (!Package)
        {
            return FString();
        }
        FStringView ModuleName;
        if (FPackageName::TryConvertScriptPackageNameToModuleName(Package->GetName(), ModuleName))
        {
            return FString(ModuleName);
        }
        return FString();
    }

    bool NameMatchesPattern(const FString& Name, const FString& Pattern)
    {
        return Pattern.IsEmpty() || Name.Contains(Pattern, ESearchCase::IgnoreCase);
    }
}

// MCP:list_derived_classes - Enumerate every class derived from a base class (native + Blueprint), the generalization of list_subsystem_classes to any UObject hierarchy. With loaded_only=false also surfaces not-yet-loaded Blueprint subclasses via the asset registry (class_name/path only, loaded=false).
//   @param base_class - Name/path of the base class (resolved via ResolveClass: bare name, /Script/ path, or Blueprint asset path)
//   @param include_blueprints - Include Blueprint-generated (non-native) subclasses (default true)
//   @param include_abstract - Include classes flagged CLASS_Abstract (default false)
//   @param loaded_only - If false (default), also queries the asset registry for Blueprint subclasses not currently loaded in memory
//   @returns { base_class: string, classes: [{class_name, path, is_blueprint, is_abstract, module?, loaded}], count: int }

TSharedPtr<FJsonObject> HandleListDerivedClasses(const TSharedPtr<FJsonObject>& Params)
{
    FString BaseClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("base_class"), BaseClassName)) { return Missing; }

    bool bIncludeBlueprints = true;
    Params->TryGetBoolField(TEXT("include_blueprints"), bIncludeBlueprints);
    bool bIncludeAbstract = false;
    Params->TryGetBoolField(TEXT("include_abstract"), bIncludeAbstract);
    bool bLoadedOnly = false;
    Params->TryGetBoolField(TEXT("loaded_only"), bLoadedOnly);

    UClass* Base = FUEAgentGraphUtils::ResolveClass(BaseClassName);
    if (!Base)
    {
        return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(BaseClassName));
    }

    // GetDerivedClasses does NOT filter by EClassFlags on its own (confirmed against
    // UObjectHash.cpp) -- filter Abstract/Deprecated/NewerVersionExists manually, same
    // idiom as list_subsystem_classes.
    TArray<UClass*> Derived;
    GetDerivedClasses(Base, Derived, /*bRecursive=*/true);

    TArray<TSharedPtr<FJsonValue>> ClassesArray;
    TSet<FTopLevelAssetPath> LoadedPaths;

    for (UClass* Cls : Derived)
    {
        if (!Cls || Cls->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            continue;
        }
        if (!bIncludeAbstract && Cls->HasAnyClassFlags(CLASS_Abstract))
        {
            continue;
        }
        const bool bIsBlueprint = !Cls->IsNative();
        if (bIsBlueprint && !bIncludeBlueprints)
        {
            continue;
        }

        LoadedPaths.Add(Cls->GetClassPathName());

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("class_name"), Cls->GetName());
        Entry->SetStringField(TEXT("path"), Cls->GetPathName());
        Entry->SetBoolField(TEXT("is_blueprint"), bIsBlueprint);
        Entry->SetBoolField(TEXT("is_abstract"), Cls->HasAnyClassFlags(CLASS_Abstract));
        const FString Module = ResolveModuleName(Cls);
        if (!Module.IsEmpty()) { Entry->SetStringField(TEXT("module"), Module); }
        Entry->SetBoolField(TEXT("loaded"), true);
        ClassesArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    // loaded_only=false: complete the picture with Blueprint subclasses that exist on
    // disk but are not currently loaded in memory (GetDerivedClasses/TObjectIterator can
    // only ever see loaded UClass objects). Honest limitation: without loading the asset,
    // is_abstract/module are unknowable, so those fields are omitted rather than guessed.
    if (!bLoadedOnly && bIncludeBlueprints)
    {
        TArray<FTopLevelAssetPath> ClassNames;
        ClassNames.Add(Base->GetClassPathName());
        TSet<FTopLevelAssetPath> ExcludedClassNames;
        TSet<FTopLevelAssetPath> DerivedClassNames;
        mcp_common::AssetRegistry().GetDerivedClassNames(ClassNames, ExcludedClassNames, DerivedClassNames);

        for (const FTopLevelAssetPath& Path : DerivedClassNames)
        {
            if (LoadedPaths.Contains(Path))
            {
                continue; // already emitted above (loaded copy takes precedence)
            }
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("class_name"), Path.GetAssetName().ToString());
            Entry->SetStringField(TEXT("path"), Path.ToString());
            Entry->SetBoolField(TEXT("is_blueprint"), true);
            Entry->SetBoolField(TEXT("loaded"), false);
            ClassesArray.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("base_class"), BaseClassName);
    ResultObj->SetArrayField(TEXT("classes"), ClassesArray);
    ResultObj->SetNumberField(TEXT("count"), ClassesArray.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:list_structs - Enumerate loaded UScriptStruct types (native + user-defined/Blueprint), optionally filtered by module or name substring.
//   @param module_filter - Optional module name substring filter (case-insensitive)
//   @param name_pattern - Optional struct name substring filter (case-insensitive)
//   @returns { structs: [{struct_name, path, module?, is_user_defined}], count: int }

TSharedPtr<FJsonObject> HandleListStructs(const TSharedPtr<FJsonObject>& Params)
{
    FString ModuleFilter, NamePattern;
    Params->TryGetStringField(TEXT("module_filter"), ModuleFilter);
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);

    TArray<TSharedPtr<FJsonValue>> StructsArray;
    // TObjectRange<T> does not exist in UE 5.8 (confirmed against source) -- TObjectIterator<T>
    // is the real iteration mechanism, same idiom as list_subsystem_classes.
    for (TObjectIterator<UScriptStruct> It; It; ++It)
    {
        UScriptStruct* Struct = *It;
        if (!Struct || !NameMatchesPattern(Struct->GetName(), NamePattern))
        {
            continue;
        }
        const FString Module = ResolveModuleName(Struct);
        if (!ModuleFilter.IsEmpty() && !NameMatchesPattern(Module, ModuleFilter))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("struct_name"), Struct->GetName());
        Entry->SetStringField(TEXT("path"), Struct->GetPathName());
        if (!Module.IsEmpty()) { Entry->SetStringField(TEXT("module"), Module); }
        // is_user_defined: !IsNative() is the same proxy list_subsystem_classes already uses
        // for native-vs-Blueprint; a UUserDefinedStruct (Blueprint struct asset) is non-native.
        Entry->SetBoolField(TEXT("is_user_defined"), !Struct->IsNative());
        StructsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("structs"), StructsArray);
    ResultObj->SetNumberField(TEXT("count"), StructsArray.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:list_enums - Enumerate loaded UEnum types (native + user-defined/Blueprint), optionally filtered by module or name substring.
//   @param module_filter - Optional module name substring filter (case-insensitive)
//   @param name_pattern - Optional enum name substring filter (case-insensitive)
//   @returns { enums: [{enum_name, path, module?, entries:[{name, value}]}], count: int }

TSharedPtr<FJsonObject> HandleListEnums(const TSharedPtr<FJsonObject>& Params)
{
    FString ModuleFilter, NamePattern;
    Params->TryGetStringField(TEXT("module_filter"), ModuleFilter);
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);

    TArray<TSharedPtr<FJsonValue>> EnumsArray;
    for (TObjectIterator<UEnum> It; It; ++It)
    {
        UEnum* Enum = *It;
        if (!Enum || !NameMatchesPattern(Enum->GetName(), NamePattern))
        {
            continue;
        }
        const FString Module = ResolveModuleName(Enum);
        if (!ModuleFilter.IsEmpty() && !NameMatchesPattern(Module, ModuleFilter))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("enum_name"), Enum->GetName());
        Entry->SetStringField(TEXT("path"), Enum->GetPathName());
        if (!Module.IsEmpty()) { Entry->SetStringField(TEXT("module"), Module); }

        TArray<TSharedPtr<FJsonValue>> EntriesArray;
        const int32 NumEnums = Enum->NumEnums();
        for (int32 i = 0; i < NumEnums - 1; ++i) // -1 to skip the autogenerated _MAX
        {
            if (Enum->HasMetaData(TEXT("Hidden"), i)) { continue; }
            TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
            ValueObj->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(i));
            ValueObj->SetNumberField(TEXT("value"), Enum->GetValueByIndex(i));
            EntriesArray.Add(MakeShared<FJsonValueObject>(ValueObj));
        }
        Entry->SetArrayField(TEXT("entries"), EntriesArray);

        EnumsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("enums"), EnumsArray);
    ResultObj->SetNumberField(TEXT("count"), EnumsArray.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:list_objects_of_class - Enumerate live instances of a class currently in memory, complementing get_actors_by_class (actors only) with any UObject-derived type. Accepts `world` to SCOPE the sweep to one world — 'server' vs 'client' is what separates an authoritative census from a replicated one, and without it a PIE session returns the instances of EVERY world mixed together (host and client both), which reads as duplicates. The response stamps the world that answered, so count:0 means "not in THIS world" instead of "does not exist".
//   @param class_name - Name/path of the class to enumerate instances of (resolved via ResolveClass)
//   @param world_only - Only include objects that resolve a UWorld via GetWorld() (default true). Ignored when `world` is given, which is already a stricter filter.
//   @param include_default - Include each class's Class Default Object (default false). A CDO has no world, so passing `world` excludes CDOs regardless of this flag.
//   @param world - Optional world selector that SCOPES the sweep: absent (default: every loaded instance, across all worlds) | 'editor' | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). Only objects whose GetWorld() IS that world are returned; an UNRECOGNIZED value is an error naming the worlds that exist, never a silent fallback to another world.
//   @returns { class_name: string, objects: [{object_path, name, class, outer}], count: int, world: string ('any' when no selector was given), world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleListObjectsOfClass(const TSharedPtr<FJsonObject>& Params)
{
    // NOTE: the param is named class_name, not class -- "class" is a reserved word in
    // Python, so a generated Python wrapper would rename the kwarg to `class_`
    // while the catalog kept `class`, drifting the two (the exact f_o_v/fov bug class
    // the codegen exists to prevent).
    FString ClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName)) { return Missing; }
    bool bWorldOnly = true;
    Params->TryGetBoolField(TEXT("world_only"), bWorldOnly);
    bool bIncludeDefault = false;
    Params->TryGetBoolField(TEXT("include_default"), bIncludeDefault);

    // Optional world selector. ABSENT: GetObjectsOfClass sweeps every loaded instance and only
    // world_only/include_default filter it. GIVEN, the
    // sweep is scoped to exactly that world -- which is what makes a co-op census trustworthy:
    // during PIE the editor world AND every PIE instance are loaded at once, so an unscoped
    // list mixes the host's actors with the client's. 'game' is an alias of
    // 'pie' (same as the Reflection handlers). An unknown selector FAILS with the list of
    // worlds that exist -- it must never quietly answer from a different world.
    UWorld* SelectedWorld = nullptr;
    FString WorldLabel = TEXT("any");
    FString WorldSel;
    Params->TryGetStringField(TEXT("world"), WorldSel);
    if (!WorldSel.TrimStartAndEnd().IsEmpty())
    {
        // The selector is non-empty here, so the default selector never applies.
        FString WorldError;
        SelectedWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), WorldLabel, WorldError);
        if (!SelectedWorld)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("list_objects_of_class: %s"), *WorldError));
        }
    }

    UClass* Class = FUEAgentGraphUtils::ResolveClass(ClassName);
    if (!Class)
    {
        return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(ClassName));
    }

    const EObjectFlags ExcludeFlags = bIncludeDefault ? RF_NoFlags : RF_ClassDefaultObject;
    TArray<UObject*> Objects;
    GetObjectsOfClass(Class, Objects, /*bIncludeDerivedClasses=*/true, ExcludeFlags);

    TArray<TSharedPtr<FJsonValue>> ObjectsArray;
    for (UObject* Object : Objects)
    {
        if (!Object)
        {
            continue;
        }
        // UObject::GetWorld() is virtual (Object.h, WITH_ENGINE); callable directly without
        // a cast to AActor/UActorComponent -- it follows the outer chain by default and is
        // overridden where that's insufficient.
        if (SelectedWorld)
        {
            // Scoped sweep: identity, not "has some world". Written as its own branch so the
            // no-selector path below stays byte-identical (including its laziness: GetWorld()
            // is not called at all when world_only=false and no selector was given).
            if (Object->GetWorld() != SelectedWorld)
            {
                continue;
            }
        }
        else if (bWorldOnly && Object->GetWorld() == nullptr)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("object_path"), Object->GetPathName());
        Entry->SetStringField(TEXT("name"), Object->GetName());
        Entry->SetStringField(TEXT("class"), Object->GetClass()->GetName());
        Entry->SetStringField(TEXT("outer"), Object->GetOuter() ? Object->GetOuter()->GetName() : FString());
        ObjectsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("class_name"), ClassName);
    ResultObj->SetArrayField(TEXT("objects"), ObjectsArray);
    ResultObj->SetNumberField(TEXT("count"), ObjectsArray.Num());
    ResultObj->SetStringField(TEXT("world"), WorldLabel);
    // The identity of the world that was swept. With no selector, SelectedWorld is null
    // and every evidence field comes back null -- an honest "no single world was scoped",
    // not a missing field. With one, count:0 reads as "not in THIS world".
    mcp_common::AddWorldEvidence(ResultObj, SelectedWorld);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:find_classes_implementing_interface - Enumerate every loaded class that implements a given UInterface. No inverse index exists in the engine (confirmed against source) -- this is an O(loaded classes) sweep, same idiom as list_subsystem_classes.
//   @param interface - Name/path of the interface to search for (resolved via ResolveClass)
//   @param include_blueprints - Include Blueprint-generated (non-native) classes (default true)
//   @returns { interface: string, classes: [{class_name, path, is_blueprint, implemented_by_k2}], count: int, scanned_class_count: int }

TSharedPtr<FJsonObject> HandleFindClassesImplementingInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString InterfaceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("interface"), InterfaceName)) { return Missing; }
    bool bIncludeBlueprints = true;
    Params->TryGetBoolField(TEXT("include_blueprints"), bIncludeBlueprints);

    UClass* Interface = FUEAgentGraphUtils::ResolveClass(InterfaceName);
    if (!Interface)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Interface not found: %s"), *InterfaceName));
    }
    if (!Interface->HasAnyClassFlags(CLASS_Interface))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class is not a UInterface: %s"), *InterfaceName));
    }

    TArray<TSharedPtr<FJsonValue>> ClassesArray;
    int32 ScannedCount = 0;

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Cls = *It;
        ++ScannedCount;
        if (!Cls || Cls->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
        {
            continue;
        }
        const bool bIsBlueprint = !Cls->IsNative();
        if (bIsBlueprint && !bIncludeBlueprints)
        {
            continue;
        }
        if (!Cls->ImplementsInterface(Interface))
        {
            continue;
        }

        // ImplementsInterface only answers yes/no; find the exact FImplementedInterface
        // entry (walking the hierarchy, same as BuildClassDescriptor) to read bImplementedByK2.
        bool bImplementedByK2 = false;
        for (UClass* Cur = Cls; Cur; Cur = Cur->GetSuperClass())
        {
            bool bFound = false;
            for (const FImplementedInterface& Impl : Cur->Interfaces)
            {
                if (Impl.Class && Impl.Class->IsChildOf(Interface))
                {
                    bImplementedByK2 = Impl.bImplementedByK2;
                    bFound = true;
                    break;
                }
            }
            if (bFound) { break; }
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("class_name"), Cls->GetName());
        Entry->SetStringField(TEXT("path"), Cls->GetPathName());
        Entry->SetBoolField(TEXT("is_blueprint"), bIsBlueprint);
        Entry->SetBoolField(TEXT("implemented_by_k2"), bImplementedByK2);
        ClassesArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("interface"), InterfaceName);
    ResultObj->SetArrayField(TEXT("classes"), ClassesArray);
    ResultObj->SetNumberField(TEXT("count"), ClassesArray.Num());
    // Never truncate silently: the full result is always returned; this count is
    // purely informational so a caller understands the O(n) cost of the sweep.
    ResultObj->SetNumberField(TEXT("scanned_class_count"), ScannedCount);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:list_modules - Enumerate known engine/game/plugin modules (loaded or not), optionally filtered by name substring.
//   @param name_pattern - Optional module name substring filter (case-insensitive)
//   @param loaded_only - Only include currently-loaded modules (default false)
//   @returns { modules: [{name, file_path, is_loaded, is_game_module}], count: int }

TSharedPtr<FJsonObject> HandleListModules(const TSharedPtr<FJsonObject>& Params)
{
    FString NamePattern;
    Params->TryGetStringField(TEXT("name_pattern"), NamePattern);
    bool bLoadedOnly = false;
    Params->TryGetBoolField(TEXT("loaded_only"), bLoadedOnly);

    // FModuleManager::QueryModules -- GetModuleList does NOT exist (confirmed against source).
    TArray<FModuleStatus> Statuses;
    FModuleManager::Get().QueryModules(Statuses);

    TArray<TSharedPtr<FJsonValue>> ModulesArray;
    for (const FModuleStatus& Status : Statuses)
    {
        if (!NameMatchesPattern(Status.Name, NamePattern))
        {
            continue;
        }
        if (bLoadedOnly && !Status.bIsLoaded)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Status.Name);
        Entry->SetStringField(TEXT("file_path"), Status.FilePath);
        Entry->SetBoolField(TEXT("is_loaded"), Status.bIsLoaded);
        Entry->SetBoolField(TEXT("is_game_module"), Status.bIsGameModule);
        ModulesArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("modules"), ModulesArray);
    ResultObj->SetNumberField(TEXT("count"), ModulesArray.Num());
    return mcp_common::MakeOkReply(ResultObj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("find_classes_implementing_interface"), &HandleFindClassesImplementingInterface },
    { TEXT("list_derived_classes"), &HandleListDerivedClasses },
    { TEXT("list_enums"), &HandleListEnums },
    { TEXT("list_modules"), &HandleListModules },
    { TEXT("list_objects_of_class"), &HandleListObjectsOfClass },
    { TEXT("list_structs"), &HandleListStructs },
};
} // namespace UEAgentReflectionDiscoveryCommands
