// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h"

#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h" // defines FStructVariableDescription
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "UObject/Interface.h"

#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Factories/EnumFactory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/SavePackage.h"

#define LOCTEXT_NAMESPACE "UEAgentType"

/**
 * User-defined data types: Blueprint Enums, Structs and Interfaces, plus editing of enum entries and
 * struct members. Creates the assets (UUserDefinedEnum / UUserDefinedStruct / interface UBlueprint),
 * registers them with the asset registry and marks their packages dirty.
 */
namespace UEAgentTypeCommands
{

// MCP-DOMAIN: Editor

namespace
{
    // The content folder the "path" param names, normalized; DefaultPath when it is absent or names nothing.
    FString Type_ReadPackagePath(const TSharedPtr<FJsonObject>& Params, const TCHAR* DefaultPath)
    {
        FString Path = DefaultPath;
        Params->TryGetStringField(TEXT("path"), Path);
        return mcp_common::NormalizeFolder(Path, DefaultPath);
    }

    // A new package at FullPath. Null on success, else the error reply: an asset already exists there, or the
    // package cannot be created.
    TSharedPtr<FJsonObject> Type_CreateNewPackage(const FString& FullPath, UPackage*& OutPackage)
    {
        if (UEditorAssetLibrary::DoesAssetExist(FullPath))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset already exists: %s"), *FullPath));
        }
        OutPackage = CreatePackage(*FullPath);
        if (!OutPackage)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to create package"));
        }
        return nullptr;
    }

    // The user-defined struct at StructPath. Null on success, else the error reply.
    TSharedPtr<FJsonObject> Type_LoadStruct(const FString& StructPath, UUserDefinedStruct*& OutStruct)
    {
        OutStruct = LoadObject<UUserDefinedStruct>(nullptr, *StructPath);
        if (!OutStruct)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("UserDefinedStruct not found: %s"), *StructPath));
        }
        return nullptr;
    }

    // The user-defined enum at EnumPath. Null on success, else the error reply.
    TSharedPtr<FJsonObject> Type_LoadEnum(const FString& EnumPath, UUserDefinedEnum*& OutEnum)
    {
        OutEnum = LoadObject<UUserDefinedEnum>(nullptr, *EnumPath);
        if (!OutEnum)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("UserDefinedEnum not found: %s"), *EnumPath));
        }
        return nullptr;
    }

    const TCHAR* ContainerTypeName(EPinContainerType Type)
    {
        switch (Type)
        {
            case EPinContainerType::Array: return TEXT("Array");
            case EPinContainerType::Set:   return TEXT("Set");
            case EPinContainerType::Map:   return TEXT("Map");
            default:                       return TEXT("None");
        }
    }

    // Describe one struct member for round-trip reads.
    TSharedPtr<FJsonObject> MakeMemberObject(const FStructVariableDescription& D)
    {
        TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
        M->SetStringField(TEXT("name"), D.FriendlyName);
        M->SetStringField(TEXT("var_name"), D.VarName.ToString());
        M->SetStringField(TEXT("guid"), D.VarGuid.ToString());
        M->SetStringField(TEXT("category"), D.Category.ToString());
        if (!D.SubCategory.IsNone())
        {
            M->SetStringField(TEXT("sub_category"), D.SubCategory.ToString());
        }
        if (!D.SubCategoryObject.IsNull())
        {
            M->SetStringField(TEXT("sub_category_object"), D.SubCategoryObject.ToString());
        }
        M->SetStringField(TEXT("container_type"), ContainerTypeName(D.ContainerType));
        M->SetStringField(TEXT("default_value"), D.DefaultValue);
        M->SetStringField(TEXT("current_default_value"), D.CurrentDefaultValue);
        if (!D.ToolTip.IsEmpty())
        {
            M->SetStringField(TEXT("tooltip"), D.ToolTip);
        }
        M->SetBoolField(TEXT("edit_on_instance"), !D.bDontEditOnInstance);
        M->SetBoolField(TEXT("save_game"), D.bEnableSaveGame != 0);
        return M;
    }

    // Resolve a member by its FriendlyName (display name set via add_struct_member),
    // falling back to the internal VarName. Returns its stable VarGuid (invalid if absent).
    FGuid FindMemberGuidByName(const UUserDefinedStruct* Struct, const FString& MemberName)
    {
        const TArray<FStructVariableDescription>& Desc = FStructureEditorUtils::GetVarDesc(Struct);
        for (const FStructVariableDescription& D : Desc)
        {
            if (D.FriendlyName.Equals(MemberName, ESearchCase::IgnoreCase))
            {
                return D.VarGuid;
            }
        }
        for (const FStructVariableDescription& D : Desc)
        {
            if (D.VarName.ToString().Equals(MemberName, ESearchCase::IgnoreCase))
            {
                return D.VarGuid;
            }
        }
        return FGuid();
    }

    void Type_MarkStructDirty(const UUserDefinedStruct* Struct)
    {
        if (UPackage* Package = Struct->GetOutermost())
        {
            Package->MarkPackageDirty();
        }
    }

    // Registers a newly created Asset and dirties its Package; returns the reply that names it.
    TSharedPtr<FJsonObject> Type_RegisterNewAsset(UObject* Asset, UPackage* Package, const FString& Name, const FString& FullPath)
    {
        FAssetRegistryModule::AssetCreated(Asset);
        Package->MarkPackageDirty();

        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("name"), Name);
        Result->SetStringField(TEXT("path"), FullPath);
        return Result;
    }

    // A reply about one member of a user-defined struct.
    TSharedPtr<FJsonObject> Type_StructMemberResult(const FString& StructPath, const FString& MemberName)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("struct_path"), StructPath);
        Result->SetStringField(TEXT("member_name"), MemberName);
        return Result;
    }
}

// MCP:create_enum - Create a Blueprint enum asset from an ordered list of enumerator DISPLAY names -- states, categories and types that Blueprints and DataTable columns switch on. Errors if the asset already exists (not idempotent). Beware the display/internal split that follows: the engine stores enumerators internally as NewEnumerator0, NewEnumerator1, ... and writers into DataTables must use those internal names, because a display name containing spaces is cut at the first space by the text importer and silently lands on index 0. So keep single-word display names, or expect to write NewEnumeratorN. add_enum_entry / rename_enum_entry / reorder_enum_entry edit it afterwards.
//   @param name - Enum asset name.
//   @param entries - Ordered list of enumerator display-name strings.
//   @param path - Target content path for the new asset (default /Game/Blueprints)
//   @returns { name, path, entry_count }
TSharedPtr<FJsonObject> HandleCreateEnum(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* EntriesJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("entries"), EntriesJson) || EntriesJson == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: entries (array of strings)"));
    }
    TArray<FString> Entries;
    for (const TSharedPtr<FJsonValue>& Value : *EntriesJson)
    {
        if (Value.IsValid() && Value->Type == EJson::String)
        {
            Entries.Add(Value->AsString());
        }
    }
    if (Entries.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("'entries' must contain at least one enumerator name"));
    }

    const FString PackagePath = Type_ReadPackagePath(Params, TEXT("/Game/Blueprints"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
    UPackage* Package = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_CreateNewPackage(FullPath, Package)) { return Error; }

    // NOTE: UEnumFactory (Factories/EnumFactory.h) is the supported way to create a
    // UUserDefinedEnum asset in the editor.
    UEnumFactory* Factory = NewObject<UEnumFactory>();
    UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(Factory->FactoryCreateNew(
        UUserDefinedEnum::StaticClass(), Package, FName(*Name),
        RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
    if (!Enum)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create UserDefinedEnum"));
    }

    // Grow to the requested count (NumEnums() includes the hidden _MAX entry), then
    // assign display names in order. AddNewEnumeratorForUserDefinedEnum /
    // SetEnumeratorDisplayName are the documented FEnumEditorUtils mutators.
    while (Enum->NumEnums() - 1 < Entries.Num())
    {
        FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
    }
    for (int32 i = 0; i < Entries.Num(); ++i)
    {
        FEnumEditorUtils::SetEnumeratorDisplayName(Enum, i, FText::FromString(Entries[i]));
    }

    TSharedPtr<FJsonObject> Result = Type_RegisterNewAsset(Enum, Package, Name, FullPath);
    Result->SetNumberField(TEXT("entry_count"), Entries.Num());
    return Result;
}

// MCP:create_struct - Create a Blueprint Struct (UUserDefinedStruct). Idempotent by reset+rebuild: a stale struct already at the path is deleted and recreated fresh (reset_stale:true); a same-path asset of a different type is an honest error.
//   @param name - Asset name (no path)
//   @param path - Target content path for the new asset (default /Game/Blueprints)
//   @returns { name, path, reset_stale }
TSharedPtr<FJsonObject> HandleCreateStruct(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const FString PackagePath = Type_ReadPackagePath(Params, TEXT("/Game/Blueprints"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
    // Idempotent by reset+rebuild, not by refusing outright: a stale
    // UUserDefinedStruct left over from a prior run (e.g. member renames that failed
    // silently, see HandleAddStructMember) would otherwise poison every later run that
    // reuses this path. A collision with an asset of a DIFFERENT type is still an
    // honest error -- we never destroy content that isn't our own struct.
    bool bResetStale = false;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UObject* Stale = LoadObject<UObject>(nullptr, *FullPath);
        if (!Stale || !Stale->IsA<UUserDefinedStruct>())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset already exists: %s"), *FullPath));
        }
        if (!UEditorAssetLibrary::DeleteLoadedAsset(Stale))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Asset already exists and could not be reset for a fresh rebuild: %s"), *FullPath));
        }
        bResetStale = true;
    }

    UPackage* Package = CreatePackage(*FullPath);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create package"));
    }

    // NOTE: FStructureEditorUtils::CreateUserDefinedStruct is the canonical creator;
    // it returns a UUserDefinedStruct seeded with one default member.
    UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(
        Package, FName(*Name), RF_Public | RF_Standalone | RF_Transactional);
    if (!Struct)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create UserDefinedStruct"));
    }

    TSharedPtr<FJsonObject> Result = Type_RegisterNewAsset(Struct, Package, Name, FullPath);
    Result->SetBoolField(TEXT("reset_stale"), bResetStale);
    return Result;
}

// MCP:add_struct_member - Add a field to a Blueprint (user-defined) struct: the way to grow a data shape -- a DataTable row struct, a save record, a component's payload -- without opening the struct editor. Honest on collisions: if the new name is invalid or already used it ROLLS BACK the auto-named member it had to create first and returns an error, instead of reporting success under the engine's MemberVar_N name and leaving orphans behind. Adding a field to a struct a DataTable already uses changes every row (new field at its default), so re-read the table afterwards. get_struct_members lists what is there; rename_struct_member and remove_struct_member are the edits.
//   @param struct_path - Full asset path of the struct (e.g. /Game/Blueprints/MyStruct).
//   @param member_name - New member name.
//   @param member_type - Type string resolved through the Blueprint pin-type resolver: scalars (bool, int, int64, float, string, name, text, byte), plus struct/enum/class/object types addressed by name or asset path. An unresolvable string is an error, not a silent default.
//   @returns { struct_path, member_name, member_type, member_count }
TSharedPtr<FJsonObject> HandleAddStructMember(const TSharedPtr<FJsonObject>& Params)
{
    FString StructPath, MemberName, MemberType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("struct_path"), StructPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("member_name"), MemberName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("member_type"), MemberType, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UUserDefinedStruct* Struct = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadStruct(StructPath, Struct)) { return Error; }

    FEdGraphPinType PinType;
    if (!FUEAgentGraphUtils::ResolvePinType(MemberType, PinType))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve member_type: %s"), *MemberType));
    }

    if (!FStructureEditorUtils::AddVariable(Struct, PinType))
    {
        return mcp_common::MakeErrorReply(TEXT("AddVariable failed"));
    }

    // Rename the freshly-added member (the last entry in the description list).
    // RenameVariable fails silently (no log/exception) on a name collision, so its return
    // is checked and the orphaned auto-named member ("MemberVar_N") is rolled back. Ignoring
    // it would report success under the auto-generated name and leave one unnamed orphan
    // member behind on every collision (a growing MemberVar_N pile across repeated runs).
    const auto& Desc = FStructureEditorUtils::GetVarDesc(Struct);
    if (Desc.Num() > 0)
    {
        const FGuid NewGuid = Desc.Last().VarGuid;
        if (!FStructureEditorUtils::RenameVariable(Struct, NewGuid, MemberName))
        {
            FStructureEditorUtils::RemoveVariable(Struct, NewGuid);
            Type_MarkStructDirty(Struct);
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not rename member to '%s' (invalid name or collides with another member)"), *MemberName));
        }
    }

    Type_MarkStructDirty(Struct);

    TSharedPtr<FJsonObject> Result = Type_StructMemberResult(StructPath, MemberName);
    Result->SetStringField(TEXT("member_type"), MemberType);
    Result->SetNumberField(TEXT("member_count"), Desc.Num());
    return Result;
}

// MCP:create_blueprint_interface - Create a Blueprint Interface asset: a contract of function signatures that unrelated classes can implement, so callers talk to the contract instead of a concrete class. The right tool when several different kinds of actor must answer the same request (interactable, damageable, workable) and you do not want a shared base class -- one interface replaces N-by-M hard-wired cases. Created empty: add functions with add_blueprint_function, then implement_interface on each class and add_blueprint_function with override=true per function.
//   @param name - Asset name (no path)
//   @param path - Target content path for the new asset (default /Game/Blueprints)
//   @returns { name, path, type }
TSharedPtr<FJsonObject> HandleCreateBlueprintInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const FString PackagePath = Type_ReadPackagePath(Params, TEXT("/Game/Blueprints"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
    UPackage* Package = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_CreateNewPackage(FullPath, Package)) { return Error; }

    // A Blueprint Interface is a UBlueprint of type BPTYPE_Interface parented to UInterface.
    UBlueprint* InterfaceBP = FKismetEditorUtilities::CreateBlueprint(
        UInterface::StaticClass(), Package, FName(*Name), BPTYPE_Interface,
        UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
    if (!InterfaceBP)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create Blueprint Interface"));
    }

    TSharedPtr<FJsonObject> Result = Type_RegisterNewAsset(InterfaceBP, Package, Name, FullPath);
    Result->SetStringField(TEXT("type"), TEXT("BlueprintInterface"));
    return Result;
}

// MCP:get_struct_members - List the members of a user-defined struct (UUserDefinedStruct) with their type, default value, and flags.  The reader that pairs with add_struct_member.
//   @param struct_path - Full asset path of the UUserDefinedStruct (e.g. /Game/Blueprints/MyStruct).
//   @returns { struct_path: string, member_count: int, members: [ { name: string, var_name: string, guid: string, category: string, sub_category?: string, sub_category_object?: string, container_type: string, default_value: string, current_default_value: string, tooltip?: string, edit_on_instance: bool, save_game: bool } ] }
TSharedPtr<FJsonObject> HandleGetStructMembers(const TSharedPtr<FJsonObject>& Params)
{
    FString StructPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("struct_path"), StructPath)) { return Missing; }

    UUserDefinedStruct* Struct = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadStruct(StructPath, Struct)) { return Error; }

    const TArray<FStructVariableDescription>& Desc = FStructureEditorUtils::GetVarDesc(Struct);
    TArray<TSharedPtr<FJsonValue>> Members;
    for (const FStructVariableDescription& D : Desc)
    {
        Members.Add(MakeShared<FJsonValueObject>(MakeMemberObject(D)));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("struct_path"), StructPath);
    Result->SetNumberField(TEXT("member_count"), Desc.Num());
    Result->SetArrayField(TEXT("members"), Members);
    return Result;
}

// MCP:set_struct_member_default - Set the default value of a user-defined struct member (read-modify-write; the value is validated against the member's type by the engine, so an incompatible value is an honest error).  Idempotent: re-setting the same value is a no-op success.
//   @param struct_path - Full asset path of the UUserDefinedStruct.
//   @param member_name - Member to edit, by its display name (or internal var name).
//   @param default_value - New default as a string appropriate to the member's type (e.g. "7", "true", "1.5", "Hello"); pass "" to clear.
//   @returns { struct_path: string, member_name: string, default_value: string, applied_default_value: string, already_set: bool, applied: bool, member: object }
TSharedPtr<FJsonObject> HandleSetStructMemberDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString StructPath, MemberName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("struct_path"), StructPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("member_name"), MemberName)) { return Missing; }
    FString NewDefault;
    if (!Params->TryGetStringField(TEXT("default_value"), NewDefault))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: default_value (may be an empty string to clear)"));
    }

    UUserDefinedStruct* Struct = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadStruct(StructPath, Struct)) { return Error; }

    const FGuid Guid = FindMemberGuidByName(Struct, MemberName);
    if (!Guid.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Struct member not found: %s"), *MemberName));
    }

    const FStructVariableDescription* Before = FStructureEditorUtils::GetVarDescByGuid(Struct, Guid);
    const FString OldDefault = Before ? Before->DefaultValue : FString();

    // ChangeVariableDefaultValue returns false both for an invalid value AND when the value is
    // already set -- so detect the no-op case first and only treat a real change failure as error.
    const bool bAlreadySet = (NewDefault == OldDefault);
    bool bApplied = false;
    if (!bAlreadySet)
    {
        if (!FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, NewDefault))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not set default value '%s' on member '%s' -- the value is invalid for its type"),
                    *NewDefault, *MemberName));
        }
        bApplied = true;
    }

    Type_MarkStructDirty(Struct);

    // Re-fetch after the change (the struct recompiles on a default-value change).
    const FStructVariableDescription* After = FStructureEditorUtils::GetVarDescByGuid(Struct, Guid);

    TSharedPtr<FJsonObject> Result = Type_StructMemberResult(StructPath, MemberName);
    Result->SetStringField(TEXT("default_value"), NewDefault);
    Result->SetStringField(TEXT("applied_default_value"), After ? After->DefaultValue : NewDefault);
    Result->SetBoolField(TEXT("already_set"), bAlreadySet);
    Result->SetBoolField(TEXT("applied"), bApplied);
    if (After)
    {
        Result->SetObjectField(TEXT("member"), MakeMemberObject(*After));
    }
    return Result;
}

// MCP:remove_struct_member - Remove a member from a user-defined struct.  Idempotent (a missing member reports already_absent without error); the engine refuses to remove the last remaining member (a struct cannot be empty) -- reported as an honest error.
//   @param struct_path - Full asset path of the UUserDefinedStruct.
//   @param member_name - Member to remove, by its display name (or internal var name).
//   @returns { struct_path: string, member_name: string, removed: bool, already_absent: bool, member_count: int }
TSharedPtr<FJsonObject> HandleRemoveStructMember(const TSharedPtr<FJsonObject>& Params)
{
    FString StructPath, MemberName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("struct_path"), StructPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("member_name"), MemberName)) { return Missing; }

    UUserDefinedStruct* Struct = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadStruct(StructPath, Struct)) { return Error; }

    const FGuid Guid = FindMemberGuidByName(Struct, MemberName);
    const bool bAlreadyAbsent = !Guid.IsValid();
    bool bRemoved = false;
    if (!bAlreadyAbsent)
    {
        const int32 CountBefore = FStructureEditorUtils::GetVarDesc(Struct).Num();
        bRemoved = FStructureEditorUtils::RemoveVariable(Struct, Guid);
        if (!bRemoved)
        {
            if (CountBefore <= 1)
            {
                return mcp_common::MakeErrorReply(
                    TEXT("Cannot remove the last member of a struct (a UserDefinedStruct cannot be empty)"));
            }
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("RemoveVariable failed for member '%s'"), *MemberName));
        }
        Type_MarkStructDirty(Struct);
    }

    TSharedPtr<FJsonObject> Result = Type_StructMemberResult(StructPath, MemberName);
    Result->SetBoolField(TEXT("removed"), bRemoved);
    Result->SetBoolField(TEXT("already_absent"), bAlreadyAbsent);
    Result->SetNumberField(TEXT("member_count"), FStructureEditorUtils::GetVarDesc(Struct).Num());
    return Result;
}

// MCP:rename_struct_member - Rename a member of a user-defined struct by resolving its current display name to its stable VarGuid, then applying the new display name (the same FStructureEditorUtils::RenameVariable path add_struct_member uses to set the initial name).  Idempotent: renaming a member to the name it already has is a no-op success (already_named). An unknown current member, or a new name that collides with another member's name, is an honest error.
//   @param struct_path - Full asset path of the UUserDefinedStruct.
//   @param member_name - Member to rename, by its current display name (or internal var name).
//   @param new_name - New display name for the member.
//   @returns { struct_path: string, member_name: string, new_name: string, renamed: bool, already_named: bool, member: object }
TSharedPtr<FJsonObject> HandleRenameStructMember(const TSharedPtr<FJsonObject>& Params)
{
    FString StructPath, MemberName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("struct_path"), StructPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("member_name"), MemberName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

    UUserDefinedStruct* Struct = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadStruct(StructPath, Struct)) { return Error; }

    const FGuid Guid = FindMemberGuidByName(Struct, MemberName);
    if (!Guid.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Struct member not found: %s"), *MemberName));
    }

    // Idempotency: if the member's current display name already equals new_name, do nothing.
    const FStructVariableDescription* Before = FStructureEditorUtils::GetVarDescByGuid(Struct, Guid);
    const bool bAlreadyNamed = (Before && Before->FriendlyName.Equals(NewName, ESearchCase::CaseSensitive));

    bool bRenamed = false;
    if (!bAlreadyNamed)
    {
        // RenameVariable returns false for an invalid/colliding name -- honest error.
        if (!FStructureEditorUtils::RenameVariable(Struct, Guid, NewName))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not rename member '%s' to '%s' (invalid name or collides with another member)"),
                    *MemberName, *NewName));
        }
        bRenamed = true;
        Type_MarkStructDirty(Struct);
    }

    // Re-fetch after the change (the struct recompiles on a rename).
    const FStructVariableDescription* After = FStructureEditorUtils::GetVarDescByGuid(Struct, Guid);

    TSharedPtr<FJsonObject> Result = Type_StructMemberResult(StructPath, MemberName);
    Result->SetStringField(TEXT("new_name"), NewName);
    Result->SetBoolField(TEXT("renamed"), bRenamed);
    Result->SetBoolField(TEXT("already_named"), bAlreadyNamed);
    if (After)
    {
        Result->SetObjectField(TEXT("member"), MakeMemberObject(*After));
    }
    return Result;
}

// ===========================================================================
// Enum-side operations via FEnumEditorUtils.
// The engine mutators are all void and address entries by int32 index; NumEnums()
// includes the hidden trailing _MAX, so the user-visible count is NumEnums()-1.
// Verify by reading state before/after (no success bool). Author in-memory like the
// rest of this family (MarkPackageDirty; no SavePackage / no recompile needed).
// ===========================================================================
namespace
{
    // User-visible enumerator count (excludes the hidden _MAX sentinel).
    int32 UserEnumCount(const UUserDefinedEnum* Enum)
    {
        return Enum ? FMath::Max(0, Enum->NumEnums() - 1) : 0;
    }

    // Resolve an enumerator index by display name (then internal name); INDEX_NONE if absent.
    // Scans only the user range [0, NumEnums()-2], excluding _MAX.
    int32 FindEnumEntryIndexByName(const UUserDefinedEnum* Enum, const FString& EntryName)
    {
        if (!Enum) { return INDEX_NONE; }
        const int32 Count = UserEnumCount(Enum);
        for (int32 i = 0; i < Count; ++i)
        {
            if (Enum->GetDisplayNameTextByIndex(i).ToString().Equals(EntryName, ESearchCase::IgnoreCase))
            {
                return i;
            }
        }
        for (int32 i = 0; i < Count; ++i)
        {
            if (Enum->GetNameStringByIndex(i).Equals(EntryName, ESearchCase::IgnoreCase))
            {
                return i;
            }
        }
        return INDEX_NONE;
    }

    // Ordered user-entry display names (excludes _MAX) as a JSON string array.
    TArray<TSharedPtr<FJsonValue>> EnumEntryNamesJson(const UUserDefinedEnum* Enum)
    {
        TArray<TSharedPtr<FJsonValue>> A;
        const int32 Count = UserEnumCount(Enum);
        for (int32 i = 0; i < Count; ++i)
        {
            A.Add(MakeShared<FJsonValueString>(Enum->GetDisplayNameTextByIndex(i).ToString()));
        }
        return A;
    }

    // A reply about one enumerator of a Blueprint enum.
    TSharedPtr<FJsonObject> Type_EnumEntryResult(const FString& EnumPath, const FString& EntryName)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("enum_path"), EnumPath);
        Result->SetStringField(TEXT("entry_name"), EntryName);
        return Result;
    }
}

// MCP:set_enum_bitflags - Toggle the "bitflags" meta on a Blueprint Enum (UUserDefinedEnum) so its enumerators can be combined as bit flags.  Idempotent: re-setting the same state reports already_set with applied=false.
//   @param enum_path - Full asset path of the UUserDefinedEnum (e.g. /Game/Blueprints/MyEnum).
//   @param bitflags [bool] - Desired state: true to mark as bitflags, false to clear.
//   @returns { enum_path: string, bitflags: bool, already_set: bool, applied: bool }
TSharedPtr<FJsonObject> HandleSetEnumBitflags(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("enum_path"), EnumPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bBitflags = false;
    if (!Params->TryGetBoolField(TEXT("bitflags"), bBitflags))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: bitflags (bool)"));
    }

    UUserDefinedEnum* Enum = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadEnum(EnumPath, Enum)) { return Error; }

    const bool bCurrent = FEnumEditorUtils::IsEnumeratorBitflagsType(Enum);
    const bool bAlreadySet = (bCurrent == bBitflags);
    if (!bAlreadySet)
    {
        FEnumEditorUtils::SetEnumeratorBitflagsTypeState(Enum, bBitflags);
        Enum->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("enum_path"), EnumPath);
    Result->SetBoolField(TEXT("bitflags"), FEnumEditorUtils::IsEnumeratorBitflagsType(Enum));
    Result->SetBoolField(TEXT("already_set"), bAlreadySet);
    Result->SetBoolField(TEXT("applied"), !bAlreadySet);
    return Result;
}

// MCP:remove_enum_entry - Remove one enumerator from a Blueprint Enum by display name (or internal name).  Idempotent (a missing entry reports already_absent without error); refuses to remove the last remaining entry. The hidden _MAX sentinel is never counted or removable.
//   @param enum_path - Full asset path of the UUserDefinedEnum.
//   @param entry_name - Enumerator to remove, by display name (or internal name).
//   @returns { enum_path: string, entry_name: string, removed: bool, already_absent: bool, entry_count: int }
TSharedPtr<FJsonObject> HandleRemoveEnumEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumPath, EntryName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("enum_path"), EnumPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("entry_name"), EntryName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UUserDefinedEnum* Enum = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadEnum(EnumPath, Enum)) { return Error; }

    const int32 Index = FindEnumEntryIndexByName(Enum, EntryName);
    const bool bAlreadyAbsent = (Index == INDEX_NONE);

    if (!bAlreadyAbsent && UserEnumCount(Enum) <= 1)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Cannot remove the last enumerator of an enum"));
    }

    bool bRemoved = false;
    if (!bAlreadyAbsent)
    {
        FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum, Index);
        Enum->MarkPackageDirty();
        // Verify by read-back: the name does not resolve.
        bRemoved = (FindEnumEntryIndexByName(Enum, EntryName) == INDEX_NONE);
    }

    TSharedPtr<FJsonObject> Result = Type_EnumEntryResult(EnumPath, EntryName);
    Result->SetBoolField(TEXT("removed"), bRemoved);
    Result->SetBoolField(TEXT("already_absent"), bAlreadyAbsent);
    Result->SetNumberField(TEXT("entry_count"), UserEnumCount(Enum));
    return Result;
}

// MCP:reorder_enum_entry - Move one enumerator to a new position in a Blueprint Enum, shifting the others (e.g. [A,B,C,D] move B->index 2 = [A,C,B,D]).  to_index is clamped to the user-entry range (the trailing _MAX sentinel is excluded). An unknown entry is an honest error.
//   @param enum_path - Full asset path of the UUserDefinedEnum.
//   @param entry_name - Enumerator to move, by display name (or internal name).
//   @param to_index [int] - Target zero-based position among the user entries.
//   @returns { enum_path: string, entry_name: string, from_index: int, to_index: int, moved: bool, entries: [string] }
TSharedPtr<FJsonObject> HandleReorderEnumEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumPath, EntryName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("enum_path"), EnumPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("entry_name"), EntryName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 ToIndex = 0;
    if (!Params->TryGetNumberField(TEXT("to_index"), ToIndex))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: to_index (int)"));
    }

    UUserDefinedEnum* Enum = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadEnum(EnumPath, Enum)) { return Error; }

    const int32 FromIndex = FindEnumEntryIndexByName(Enum, EntryName);
    if (FromIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Enumerator '%s' not found in enum"), *EntryName));
    }

    // Clamp to the user range [0, NumEnums()-2] -- never the _MAX slot.
    const int32 ClampedTo = FMath::Clamp(ToIndex, 0, FMath::Max(0, UserEnumCount(Enum) - 1));

    bool bMoved = false;
    if (ClampedTo != FromIndex)
    {
        FEnumEditorUtils::MoveEnumeratorInUserDefinedEnum(Enum, FromIndex, ClampedTo);
        Enum->MarkPackageDirty();
        bMoved = (FindEnumEntryIndexByName(Enum, EntryName) == ClampedTo);
    }

    TSharedPtr<FJsonObject> Result = Type_EnumEntryResult(EnumPath, EntryName);
    Result->SetNumberField(TEXT("from_index"), FromIndex);
    Result->SetNumberField(TEXT("to_index"), ClampedTo);
    Result->SetBoolField(TEXT("moved"), bMoved);
    Result->SetArrayField(TEXT("entries"), EnumEntryNamesJson(Enum));
    return Result;
}

// MCP:add_enum_entry - Append one enumerator to a Blueprint Enum (UUserDefinedEnum) and give it a display name.  Idempotent (an entry whose display/internal name already exists reports already_existed with added=false, leaving the enum unchanged -- the engine itself does NOT dedup). The new entry lands at the end, before the hidden _MAX sentinel.
//   @param enum_path - Full asset path of the UUserDefinedEnum (e.g. /Game/Blueprints/MyEnum).
//   @param entry_name - Display name for the new enumerator.
//   @returns { enum_path: string, entry_name: string, added: bool, already_existed: bool, entry_count: int, entries: [string] }
TSharedPtr<FJsonObject> HandleAddEnumEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumPath, EntryName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("enum_path"), EnumPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("entry_name"), EntryName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UUserDefinedEnum* Enum = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadEnum(EnumPath, Enum)) { return Error; }

    UE_LOG(LogTemp, Verbose, TEXT("add_enum_entry: enum='%s' entry='%s' user_count(before)=%d"),
        *EnumPath, *EntryName, UserEnumCount(Enum));

    // Idempotency (handler-imposed -- the engine does NOT dedup): if the name already
    // resolves, report already_existed without mutating.
    if (FindEnumEntryIndexByName(Enum, EntryName) != INDEX_NONE)
    {
        UE_LOG(LogTemp, Verbose, TEXT("add_enum_entry: '%s' already present -> already_existed"), *EntryName);
        TSharedPtr<FJsonObject> Result = Type_EnumEntryResult(EnumPath, EntryName);
        Result->SetBoolField(TEXT("added"), false);
        Result->SetBoolField(TEXT("already_existed"), true);
        Result->SetNumberField(TEXT("entry_count"), UserEnumCount(Enum));
        Result->SetArrayField(TEXT("entries"), EnumEntryNamesJson(Enum));
        return Result;
    }

    // Append one entry (auto-named at GetMaxEnumValue()), then name it. The new entry
    // is the last user slot: index UserEnumCount(Enum)-1 (the hidden _MAX stays last).
    // Mirrors the create_enum add+name loop, reduced to one entry.
    FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
    const int32 NewIndex = UserEnumCount(Enum) - 1;
    const bool bNamed = FEnumEditorUtils::SetEnumeratorDisplayName(Enum, NewIndex, FText::FromString(EntryName));
    if (!bNamed)
    {
        // SetEnumeratorDisplayName rejects an invalid/duplicate display name -- honest error.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not set display name '%s' on the new enumerator (invalid or duplicate name)"),
                *EntryName));
    }

    Enum->MarkPackageDirty();

    // Verify by read-back: the name now resolves to a user entry.
    const int32 ResolvedIndex = FindEnumEntryIndexByName(Enum, EntryName);
    const bool bAdded = (ResolvedIndex != INDEX_NONE);

    UE_LOG(LogTemp, Verbose, TEXT("add_enum_entry: added='%s' at index=%d added=%d user_count(after)=%d"),
        *EntryName, ResolvedIndex, bAdded ? 1 : 0, UserEnumCount(Enum));

    TSharedPtr<FJsonObject> Result = Type_EnumEntryResult(EnumPath, EntryName);
    Result->SetBoolField(TEXT("added"), bAdded);
    Result->SetBoolField(TEXT("already_existed"), false);
    Result->SetNumberField(TEXT("entry_count"), UserEnumCount(Enum));
    Result->SetArrayField(TEXT("entries"), EnumEntryNamesJson(Enum));
    return Result;
}

// MCP:rename_enum_entry - Rename one enumerator of a Blueprint Enum (UUserDefinedEnum): resolve the entry by its current display name (or internal name) to its index, then set a new display name via FEnumEditorUtils::SetEnumeratorDisplayName.  Idempotent: renaming an entry to the name it already has is a no-op success (already_named). An unknown current entry is an honest error; so is a new name that is empty/invalid or duplicates another entry's display name (the engine's IsEnumeratorDisplayNameValid rejects it). The hidden _MAX sentinel is never addressable.
//   @param enum_path - Full asset path of the UUserDefinedEnum (e.g. /Game/Blueprints/MyEnum).
//   @param entry_name - Enumerator to rename, by its current display name (or internal name).
//   @param new_name - New display name for the enumerator.
//   @returns { enum_path: string, entry_name: string, new_name: string, renamed: bool, already_named: bool, index: int, entries: [string] }
TSharedPtr<FJsonObject> HandleRenameEnumEntry(const TSharedPtr<FJsonObject>& Params)
{
    FString EnumPath, EntryName, NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("enum_path"), EnumPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("entry_name"), EntryName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

    UUserDefinedEnum* Enum = nullptr;
    if (TSharedPtr<FJsonObject> Error = Type_LoadEnum(EnumPath, Enum)) { return Error; }

    const int32 Index = FindEnumEntryIndexByName(Enum, EntryName);
    if (Index == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Enumerator '%s' not found in enum"), *EntryName));
    }

    // Idempotency: if the entry's current display name already equals new_name, do nothing.
    // (SetEnumeratorDisplayName's own validation skips the entry's own index, so it would also
    // accept this, but detecting it here keeps the read-back semantics honest.)
    const bool bAlreadyNamed = Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(NewName, ESearchCase::CaseSensitive);

    bool bRenamed = false;
    if (!bAlreadyNamed)
    {
        // SetEnumeratorDisplayName rejects an empty/invalid or duplicate display name -- honest error.
        if (!FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(NewName)))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Could not rename enumerator '%s' to '%s' (invalid or duplicate display name)"),
                    *EntryName, *NewName));
        }
        Enum->MarkPackageDirty();
        // Verify by read-back: the new name now resolves to the same index.
        bRenamed = (FindEnumEntryIndexByName(Enum, NewName) == Index);
    }

    TSharedPtr<FJsonObject> Result = Type_EnumEntryResult(EnumPath, EntryName);
    Result->SetStringField(TEXT("new_name"), NewName);
    Result->SetBoolField(TEXT("renamed"), bRenamed);
    Result->SetBoolField(TEXT("already_named"), bAlreadyNamed);
    Result->SetNumberField(TEXT("index"), Index);
    Result->SetArrayField(TEXT("entries"), EnumEntryNamesJson(Enum));
    return Result;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_enum_entry"), &HandleAddEnumEntry },
    { TEXT("add_struct_member"), &HandleAddStructMember },
    { TEXT("create_blueprint_interface"), &HandleCreateBlueprintInterface },
    { TEXT("create_enum"), &HandleCreateEnum },
    { TEXT("create_struct"), &HandleCreateStruct },
    { TEXT("get_struct_members"), &HandleGetStructMembers },
    { TEXT("remove_enum_entry"), &HandleRemoveEnumEntry },
    { TEXT("remove_struct_member"), &HandleRemoveStructMember },
    { TEXT("rename_enum_entry"), &HandleRenameEnumEntry },
    { TEXT("rename_struct_member"), &HandleRenameStructMember },
    { TEXT("reorder_enum_entry"), &HandleReorderEnumEntry },
    { TEXT("set_enum_bitflags"), &HandleSetEnumBitflags },
    { TEXT("set_struct_member_default"), &HandleSetStructMemberDefault },
};
} // namespace UEAgentTypeCommands

#undef LOCTEXT_NAMESPACE
