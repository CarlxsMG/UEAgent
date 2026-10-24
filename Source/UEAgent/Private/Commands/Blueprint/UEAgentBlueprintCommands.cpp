// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h" // FUEAgentGraphUtils::ResolveClass (native / /Script / Blueprint-class resolver)
#include "Commands/Level/PrimitiveComponentUtils.h"   // collision profile / mode / channel responses on a primitive
#include "Misc/PackageName.h"                        // FPackageName::ObjectPathToPackageName / IsValidLongPackageName / GetShortName
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet/BlueprintFunctionLibrary.h" // UBlueprintFunctionLibrary -- canonical parent for BPTYPE_FunctionLibrary
#include "Factories/BlueprintFactory.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"             // get_blueprint_functions: GetFunctionFlags (is_pure)
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/Engine.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "ScopedTransaction.h"                // FScopedTransaction around add_blueprint_function(override=true)
// compile_blueprint(return_messages=true): capture node-anchored compiler diagnostics.
#include "Kismet2/CompilerResultsLog.h"      // FCompilerResultsLog (UNREALED_API; UnrealEd already linked)
#include "EdGraphToken.h"                     // FEdGraphToken -> GetGraphObject()/GetPin()
#include "Logging/TokenizedMessage.h"         // FTokenizedMessage / EMessageSeverity / EMessageToken
#include "EdGraph/EdGraphNode.h"              // UEdGraphNode::NodeGuid / GetNodeTitle / ENodeTitleType
#include "EdGraph/EdGraphPin.h"               // UEdGraphPin::GetOwningNodeUnchecked
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h" // FComponentKey + ICH override of INHERITED component templates
#include "UObject/Field.h"
#include "UObject/FieldPath.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/GameMode.h"                    // create_blueprint: is_multiplayer of a GameMode Blueprint
#include "GameFramework/GameModeBase.h"
#include "Commands/Subsystem/SubsystemLifetime.h"      // create_blueprint: lifetime of a subsystem Blueprint
#include "Kismet/GameplayStatics.h"
// Blueprint Editor Library (UBlueprintEditorLibrary)
#include "BlueprintEditorLibrary.h"
#include "EdGraph/EdGraph.h"
// AI Perception - configure perception senses on a BP SCS component template.
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISenseConfig_Hearing.h"
#include "Perception/AISense.h"           // remove_ai_perception_sense: the removed config's sense, against the dominant one
#include "UObject/SavePackage.h"
#include "Engine/CollisionProfile.h" // UCollisionProfile channel-name <-> index resolution
#include "Editor.h"                                    // GEditor
// Not used directly here (PIE detection is mcp_common::IsPieActive). The include stays:
// bUseUnity=true, and removing it could break another file of the same unity blob that gets
// it through here.
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"
// element-level typed import for set_blueprint_component_array_element /
// set_blueprint_component_properties. mcp_common::SetObjectProperty is object+property-NAME
// level; writing a struct member INSIDE a TArray element needs the same routing one level down.
#include "JsonObjectConverter.h"   // FJsonObjectConverter::JsonValueToUProperty (same call shape as UEAgentAnimGraphCommands.cpp)
#include "GameplayTagContainer.h"  // FGameplayTag / FGameplayTagContainer -- mirrors McpCommonUtils.cpp routing
#include "UObject/UnrealType.h"    // FArrayProperty / FScriptArrayHelper / FDefaultConstructedPropertyElement (UnrealType.h)
#include "Commands/UI/UMGWidgetUtils.h" // RegisterUntrackedWidgetVariables before a Widget Blueprint compiles

/**
 * Blueprint asset commands: creation, compile and spawn, construction-script components and their
 * templates, materials on component templates, functions, variables and AI perception senses.
 */
namespace UEAgentBlueprintCommands
{

// Pieces several handlers of this file have in common: the error wording callers match on, the
// lookup of an OWN construction-script node by variable name, and small JSON row builders.
namespace UEAgentBlueprintCommandsImpl
{
// "Missing '<key>' parameter": the wording some handlers of this file use.
TSharedPtr<FJsonObject> MissingQuotedParam(const TCHAR* ParamKey)
{
    return mcp_common::MakeErrorReply(FString::Printf(TEXT("Missing '%s' parameter"), ParamKey));
}

TSharedPtr<FJsonObject> ComponentNotFound(const FString& RequestedComponent)
{
    return mcp_common::MakeErrorReply(FString::Printf(TEXT("Component not found: %s"), *RequestedComponent));
}

// Function and macro libraries and interfaces have no construction script, so no components.
TSharedPtr<FJsonObject> NoConstructionScript()
{
    return mcp_common::MakeErrorReply(TEXT("Blueprint has no SimpleConstructionScript"));
}

// First node of the construction script whose variable name matches, compared case-insensitively
// (the semantics of FString ==). Null when there is no such node, or no construction script.
USCS_Node* FindOwnScsNode(USimpleConstructionScript* Scs, const FString& VariableName)
{
    if (Scs == nullptr)
    {
        return nullptr;
    }
    for (USCS_Node* Candidate : Scs->GetAllNodes())
    {
        if (Candidate && Candidate->GetVariableName().ToString().Equals(VariableName, ESearchCase::IgnoreCase))
        {
            return Candidate;
        }
    }
    return nullptr;
}

// One material-slot row; an empty slot reads "None" with a blank path and class.
TSharedPtr<FJsonObject> MaterialSlotRow(int32 SlotIndex, const FString& ComponentLabel, const UMaterialInterface* Assigned)
{
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetNumberField(TEXT("slot"), SlotIndex);
    Row->SetStringField(TEXT("component"), ComponentLabel);
    Row->SetStringField(TEXT("material_name"), Assigned ? Assigned->GetName() : FString(TEXT("None")));
    Row->SetStringField(TEXT("material_path"), Assigned ? Assigned->GetPathName() : FString());
    Row->SetStringField(TEXT("material_class"), Assigned ? Assigned->GetClass()->GetName() : FString());
    return Row;
}

// The reply for a missing parameter: MissingQuotedParam or mcp_common::MissingParameterReply.
using FMissingParamReply = TSharedPtr<FJsonObject> (*)(const TCHAR* ParamKey);

// Reads the required blueprint_name and component_name: the reply naming the first one missing, else null.
TSharedPtr<FJsonObject> ReadBlueprintComponentNames(const TSharedPtr<FJsonObject>& Params, FMissingParamReply MissingReply,
    FString& OutBlueprintName, FString& OutComponentName)
{
    if (!Params->TryGetStringField(TEXT("blueprint_name"), OutBlueprintName))
    {
        return MissingReply(TEXT("blueprint_name"));
    }
    if (!Params->TryGetStringField(TEXT("component_name"), OutComponentName))
    {
        return MissingReply(TEXT("component_name"));
    }
    return nullptr;
}

// The Blueprint the required blueprint_name names (OutBlueprintName is the name as given). Null, with OutError holding
// the reply, when the param is missing or no Blueprint resolves.
UBlueprint* ResolveBlueprintParam(const TSharedPtr<FJsonObject>& Params, FMissingParamReply MissingReply,
    FString& OutBlueprintName, TSharedPtr<FJsonObject>& OutError)
{
    if (!Params->TryGetStringField(TEXT("blueprint_name"), OutBlueprintName))
    {
        OutError = MissingReply(TEXT("blueprint_name"));
        return nullptr;
    }
    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(OutBlueprintName);
    if (!Blueprint)
    {
        OutError = mcp_common::BlueprintNotFoundReply(OutBlueprintName);
    }
    return Blueprint;
}

// The template of the OWN construction-script node ComponentName of the Blueprint BlueprintName (OutBlueprint), as a
// TemplateType; inherited components are out of reach here. Null, with OutError holding the reply, when the
// Blueprint, its construction script or the node is missing, or the template is no TemplateType (WrongTypeError).
template <typename TemplateType>
TemplateType* ResolveOwnComponentTemplate(const FString& BlueprintName, const FString& ComponentName,
    const TCHAR* WrongTypeError, UBlueprint*& OutBlueprint, TSharedPtr<FJsonObject>& OutError)
{
    OutBlueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!OutBlueprint)
    {
        OutError = mcp_common::BlueprintNotFoundReply(BlueprintName);
        return nullptr;
    }
    if (!OutBlueprint->SimpleConstructionScript)
    {
        OutError = NoConstructionScript();
        return nullptr;
    }
    USCS_Node* OwnNode = FindOwnScsNode(OutBlueprint->SimpleConstructionScript, ComponentName);
    if (!OwnNode)
    {
        OutError = ComponentNotFound(ComponentName);
        return nullptr;
    }
    TemplateType* Template = Cast<TemplateType>(OwnNode->ComponentTemplate);
    if (!Template)
    {
        OutError = mcp_common::MakeErrorReply(WrongTypeError);
    }
    return Template;
}

// ReadBlueprintComponentNames ("Missing '<key>' parameter") then ResolveOwnComponentTemplate on those names: null on
// success, else the reply of the first check that failed.
template <typename TemplateType>
TSharedPtr<FJsonObject> ReadOwnComponentTemplate(const TSharedPtr<FJsonObject>& Params, const TCHAR* WrongTypeError,
    FString& OutBlueprintName, FString& OutComponentName, UBlueprint*& OutBlueprint, TemplateType*& OutTemplate)
{
    if (TSharedPtr<FJsonObject> Missing = ReadBlueprintComponentNames(Params, &MissingQuotedParam, OutBlueprintName, OutComponentName))
    {
        return Missing;
    }
    TSharedPtr<FJsonObject> ResolveError;
    OutTemplate = ResolveOwnComponentTemplate<TemplateType>(OutBlueprintName, OutComponentName, WrongTypeError, OutBlueprint, ResolveError);
    return ResolveError;
}
}

// MCP-DOMAIN: Blueprint

// Asset creation, component templates, compile and spawn

// MCP:create_blueprint - Create a new Blueprint asset, optionally as a child of ANY parent class -- native or another Blueprint (pass its "..._C" class or asset path) -- and save it to disk immediately unless PIE is running or save=false. This is also the way to make the framework classes (a GameMode, GameState, PlayerController, PlayerState, GameInstance or HUD Blueprint: pass that class as parent_class) and Blueprint subsystems (parent_class a USubsystem class such as "GameInstanceSubsystem"; lifetime says which family), and it makes Blueprint Function Libraries and Macro Libraries, which have no Actor parent and which the normal path would reject. It does NOT create interfaces (create_blueprint_interface). A plain Blueprint already at the path is returned unchanged with already_exists=true and saved=false, nothing saved and blueprint_type not applied -- when parent_class is given, only if its parent is parent_class or derives from it, otherwise it is an error and nothing is changed; another asset class there (a Widget or Anim Blueprint included) is an error and nothing is created. It never confirms the parent of a new Blueprint: read it back with get_blueprint_parent.
//   @pack blueprint_authoring
//   @param name - Blueprint name. Either a bare name (created under 'path' or /Game/Blueprints/) OR a full content path ("/Game/MyGame/Systems/World/BP_Foo"); an object-path form ("/Game/.../BP_Foo.BP_Foo") is also accepted.
//   @param parent_class - Optional parent class. Accepts a native name ("Actor", "Pawn", "Character", "GameModeBase", "PlayerController", "GameInstanceSubsystem", "BlueprintFunctionLibrary"), a "/Script/Module.Class" path, a Blueprint generated-class path ("/Game/.../BP_X.BP_X_C"), or a Blueprint asset path ("/Game/.../BP_X"). Defaults to AActor. Passing "BlueprintFunctionLibrary" makes a Function Library. When given, a Blueprint already at the path must have it (or a subclass of it) as parent.
//   @param blueprint_type - Optional Blueprint type override: "normal" (default), "function_library" (a UBlueprintFunctionLibrary of static functions; parent_class is ignored/forced), or "macro" (a Macro Library; keeps parent_class or defaults to Actor). Function-library is also auto-detected from parent_class="BlueprintFunctionLibrary".
//   @param path - Optional target content folder ("/Game/MyGame/Systems"). Used only when 'name' is a bare name; ignored when 'name' is already a full path.
//   @param save - Optional bool (default true). false registers the new Blueprint and marks its package dirty without saving it -- persist it later with save_asset.
//   @returns { name: string, path: string, package_path: string, parent_class: string, generated_class: string, blueprint_type: string, already_exists: bool, saved: bool (false for a Blueprint already there: nothing saved), save_error?: string, note?: string, is_multiplayer?: bool, lifetime?: string } -- parent_class is, for a new Blueprint, what was REQUESTED and resolved, not proof the asset carries it (confirm a child's real parent with get_blueprint_parent), and for one already there its own parent. is_multiplayer appears for a GameMode (true when the parent derives from AGameMode); lifetime for a subsystem (Editor, Engine, GameInstance, World, LocalPlayer or Unknown). saved:false is honest: it carries save_error when PIE was running or the Blueprint already there has unsaved changes, and a note when save=false or an existing Blueprint is returned unchanged -- an unsaved Blueprint exists only in the editor and a later compile on it also returns success.

TSharedPtr<FJsonObject> HandleCreateBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), BlueprintName)) { return Missing; }

    FString ParentClassStr;
    Params->TryGetStringField(TEXT("parent_class"), ParentClassStr);

    FString FolderOverride;
    Params->TryGetStringField(TEXT("path"), FolderOverride);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    // A named parent is resolved up front: a Blueprint already at the path is only returned when it has that parent.
    UClass* RequestedParentClass = nullptr;
    if (!ParentClassStr.IsEmpty())
    {
        RequestedParentClass = FUEAgentGraphUtils::ResolveClass(ParentClassStr);
        if (!RequestedParentClass)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parent class not found: %s"), *ParentClassStr));
        }
    }
    // Why the Blueprint already at the path is not the one asked for, or empty when it is.
    const auto ParentMismatch = [RequestedParentClass](const UBlueprint* Existing) -> FString
    {
        if (!RequestedParentClass || (Existing->ParentClass && Existing->ParentClass->IsChildOf(RequestedParentClass)))
        {
            return FString();
        }
        return FString::Printf(
            TEXT("A Blueprint already exists at %s with parent %s, which does not derive from parent_class %s; ")
            TEXT("nothing was created or changed. Pick another name, or delete that asset first."),
            *Existing->GetPathName(), Existing->ParentClass ? *Existing->ParentClass->GetName() : TEXT("none"),
            *RequestedParentClass->GetName());
    };

    // Resolve the package path -- no blind concatenation: appending a full-path name ("/Game/MyGame/.../BP_Foo")
    // to the default folder makes a malformed package ("/Game/Blueprints//Game/MyGame/...") and a slashed object
    // FName, which crashes the editor. A full-path 'name' (with or without the ".Object" suffix) is honored
    // as-is; a bare 'name' goes under 'path' (if given) or the /Game/Blueprints default. Everything is
    // validated before we touch CreatePackage.
    FString PackageName;
    if (BlueprintName.StartsWith(TEXT("/")))
    {
        PackageName = FPackageName::ObjectPathToPackageName(BlueprintName); // no-op when there is no ".Object" suffix
    }
    else
    {
        FString Folder = FolderOverride.IsEmpty() ? FString(TEXT("/Game/Blueprints")) : FolderOverride;
        Folder.RemoveFromEnd(TEXT("/"));
        PackageName = Folder / BlueprintName;
    }

    if (!FPackageName::IsValidLongPackageName(PackageName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid blueprint package path resolved from name='%s': '%s'"), *BlueprintName, *PackageName));
    }
    // The folder and the name each fit an FName and can still join past one, which the existence check below aborts on.
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
    const FString AssetName = FPackageName::GetShortName(PackageName);

    // The reply: the Blueprint, the parent class it reports and whether this call saved it.
    auto BlueprintReply = [&PackageName](UBlueprint* BP, const UClass* ParentClass, bool bAlreadyExists, bool bSaved,
        const FString& SaveError) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
        ResultObj->SetStringField(TEXT("name"), BP->GetName());
        ResultObj->SetStringField(TEXT("path"), BP->GetPathName());
        ResultObj->SetStringField(TEXT("package_path"), PackageName);
        ResultObj->SetStringField(TEXT("parent_class"), ParentClass ? ParentClass->GetPathName() : FString());
        ResultObj->SetStringField(TEXT("generated_class"),
            BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
        ResultObj->SetStringField(TEXT("blueprint_type"),
            BP->BlueprintType == BPTYPE_FunctionLibrary ? TEXT("function_library") :
            BP->BlueprintType == BPTYPE_MacroLibrary    ? TEXT("macro") :
            BP->BlueprintType == BPTYPE_Interface       ? TEXT("interface") : TEXT("normal"));
        ResultObj->SetBoolField(TEXT("already_exists"), bAlreadyExists);
        ResultObj->SetBoolField(TEXT("saved"), bSaved);
        // The framework families say what kind they are: a GameMode whether it runs match state, a subsystem its lifetime.
        if (ParentClass && ParentClass->IsChildOf(AGameModeBase::StaticClass()))
        {
            ResultObj->SetBoolField(TEXT("is_multiplayer"), ParentClass->IsChildOf(AGameMode::StaticClass()));
        }
        if (ParentClass && ParentClass->IsChildOf(USubsystem::StaticClass()))
        {
            ResultObj->SetStringField(TEXT("lifetime"), UEAgentSubsystemLifetime::DetectLifetimeCategory(
                BP->GeneratedClass ? BP->GeneratedClass.Get() : ParentClass));
        }
        if (!SaveError.IsEmpty())
        {
            ResultObj->SetStringField(TEXT("save_error"), SaveError);
        }
        else if (bAlreadyExists)
        {
            ResultObj->SetStringField(TEXT("note"), TEXT("the Blueprint already exists; returned it unchanged, nothing saved"));
        }
        return ResultObj;
    };
    // A Blueprint this call did not author is neither touched nor saved; a package with unsaved changes says so,
    // since those changes are not on disk.
    const auto UnsavedNote = [](const UBlueprint* BP)
    {
        const UPackage* ExistingPackage = BP->GetOutermost();
        return (ExistingPackage && ExistingPackage->IsDirty())
            ? FString(TEXT("Blueprint already existed in memory (not created by this call) and was left untouched; its ")
                TEXT("package has unsaved changes. Call save_asset explicitly to persist it."))
            : FString();
    };

    // Idempotent: a plain Blueprint already at the path is returned as it is -- parent_class and blueprint_type are
    // not applied, and nothing is saved. Any other class there, a Blueprint subclass such as a Widget or Anim
    // Blueprint included, is not the asset asked for and is refused.
    if (UEditorAssetLibrary::DoesAssetExist(PackageName))
    {
        const FString ObjectPath = PackageName + TEXT(".") + AssetName;
        UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *ObjectPath);
        if (!Existing || Existing->GetClass() != UBlueprint::StaticClass())
        {
            const FString AssetConflict = mcp_common::AssetNameConflict(PackageName, AssetName, UBlueprint::StaticClass());
            return mcp_common::MakeErrorReply(!AssetConflict.IsEmpty() ? AssetConflict
                : Existing ? FString::Printf(TEXT("'%s' already exists as a %s, not a plain Blueprint; nothing was created or saved. ")
                    TEXT("Choose another name, or delete or rename the existing asset first."), *ObjectPath, *Existing->GetClass()->GetName())
                : FString::Printf(TEXT("An asset already exists at '%s' but does not load as a Blueprint; nothing was created or saved."),
                    *ObjectPath));
        }
        const FString Mismatch = ParentMismatch(Existing);
        if (!Mismatch.IsEmpty())
        {
            return mcp_common::MakeErrorReply(Mismatch);
        }
        return BlueprintReply(Existing, Existing->ParentClass, /*bAlreadyExists=*/true, /*bSaved=*/false, UnsavedNote(Existing));
    }

    // Resolve parent class + Blueprint type with the plugin's canonical resolver -- covers native names,
    // "/Script/..." paths and BLUEPRINT classes ("..._C" path or BP asset -> GeneratedClass), so a Blueprint
    // parent (e.g. a BP from a vendored library) never silently falls back to AActor.
    //
    // Most BPs are BPTYPE_Normal parented to an Actor/object and are validated with CanCreateBlueprintOfClass.
    // But a Blueprint Function Library (and a Macro Library) is NOT created that way: CanCreateBlueprintOfClass
    // REJECTS UBlueprintFunctionLibrary ("Class cannot be a Blueprint parent"), which is why
    // UBlueprintFunctionLibraryFactory does not even call it. We replicate what the editor factories do (EditorFactories.cpp):
    //   - Function Library: parent = UBlueprintFunctionLibrary,   BPTYPE_FunctionLibrary  (parent_class is ignored).
    //   - Macro Library:    normal parent (Actor/object),         BPTYPE_MacroLibrary.
    //   - otherwise:        parent validated by CanCreateBlueprintOfClass, BPTYPE_Normal.
    // The type is auto-detected from the parent (parent_class="BlueprintFunctionLibrary" -> FL) and/or forced with
    // the optional 'blueprint_type' param.
    FString BlueprintTypeStr;
    Params->TryGetStringField(TEXT("blueprint_type"), BlueprintTypeStr);
    BlueprintTypeStr = BlueprintTypeStr.ToLower().Replace(TEXT(" "), TEXT("")).Replace(TEXT("_"), TEXT(""));

    UClass* SelectedParentClass = RequestedParentClass ? RequestedParentClass : AActor::StaticClass();

    const bool bWantFunctionLibrary =
        BlueprintTypeStr == TEXT("functionlibrary") || BlueprintTypeStr == TEXT("bftl") ||
        (SelectedParentClass && SelectedParentClass->IsChildOf(UBlueprintFunctionLibrary::StaticClass()));
    const bool bWantMacroLibrary =
        BlueprintTypeStr == TEXT("macro") || BlueprintTypeStr == TEXT("macrolibrary");

    EBlueprintType BlueprintType = BPTYPE_Normal;
    if (bWantFunctionLibrary)
    {
        // Mirror UBlueprintFunctionLibraryFactory exactly: the parent MUST be UBlueprintFunctionLibrary.
        SelectedParentClass = UBlueprintFunctionLibrary::StaticClass();
        BlueprintType = BPTYPE_FunctionLibrary;
    }
    else if (bWantMacroLibrary)
    {
        // Macro libraries keep a normal parent; default to Actor (UBlueprintMacroFactory's default) when the
        // caller gave none or an uncreatable one.
        if (ParentClassStr.IsEmpty() || !SelectedParentClass ||
            !FKismetEditorUtilities::CanCreateBlueprintOfClass(SelectedParentClass))
        {
            SelectedParentClass = AActor::StaticClass();
        }
        BlueprintType = BPTYPE_MacroLibrary;
    }
    else
    {
        // Normal blueprint: the parent must be a valid Blueprint base (clean error otherwise). The subsystem bases
        // are not BlueprintBase classes, yet a Blueprint subsystem is made exactly like this, so they skip the check.
        if (!SelectedParentClass->IsChildOf(USubsystem::StaticClass())
            && !FKismetEditorUtilities::CanCreateBlueprintOfClass(SelectedParentClass))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Class cannot be a Blueprint parent: %s"), *SelectedParentClass->GetPathName()));
        }
    }

    // Create with the canonical API (the same one the editor's "Create Child Blueprint Class" uses; it handles
    // Blueprint parents correctly). Idempotency guard: FKismetEditorUtilities::CreateBlueprint ASSERT-crashes
    // (Kismet2.cpp) if a UBlueprint with that name already lives in the package (e.g. delete_asset in the
    // editor does not always unload a loaded BP) -> we return the existing one instead of crashing.
    FString PackageError;
    UPackage* Package = mcp_common::CreateAssetPackage(PackageName, AssetName, UBlueprint::StaticClass(), PackageError);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(PackageError);
    }
    Package->FullyLoad();

    UBlueprint* NewBlueprint = FindObject<UBlueprint>(Package, *AssetName);
    const bool bAlreadyExists = (NewBlueprint != nullptr);
    if (bAlreadyExists)
    {
        const FString Mismatch = ParentMismatch(NewBlueprint);
        if (!Mismatch.IsEmpty())
        {
            return mcp_common::MakeErrorReply(Mismatch);
        }
    }

    // Registering the asset (AssetCreated + MarkPackageDirty below) does not write it: without a
    // SavePackage the Blueprint exists only in memory, and every later call on it (set_properties,
    // compile_blueprint) would still succeed. So the package is saved right away, UNLESS a PIE
    // session is active.
    //
    // PIE state is checked HERE, BEFORE deciding whether to save at all: when PIE is active the
    // save is skipped outright (never attempted, never silent) and the reply carries saved:false +
    // a save_error explaining why. The dispatcher already refuses this command during PIE
    // (McpPieSafety.gen.h, EReason::Creates); this branch covers the force=true path.
    const bool bPieActive = mcp_common::IsPieActive();

    bool bSaved = false;
    FString SaveError;

    if (!NewBlueprint)
    {
        // 7-arg overload (same as UBlueprint*Factory / create_blueprint_interface): pins the Blueprint /
        // BlueprintGeneratedClass types and honors the resolved BlueprintType (Normal/FunctionLibrary/MacroLibrary).
        NewBlueprint = Cast<UBlueprint>(FKismetEditorUtilities::CreateBlueprint(
            SelectedParentClass, Package, FName(*AssetName), BlueprintType,
            UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), FName(TEXT("CreateBlueprintMCP"))));
        if (!NewBlueprint)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to create blueprint"));
        }
        FAssetRegistryModule::AssetCreated(NewBlueprint);
        NewBlueprint->MarkPackageDirty();

        if (!bSave)
        {
            // Asked not to save: registered and dirty, persisted by a later save_asset.
        }
        else if (bPieActive)
        {
            SaveError = TEXT("Skipped saving to disk: a Play-in-Editor session is active. The Blueprint ")
                TEXT("exists only in memory -- call save_asset on it once PIE has stopped (create_blueprint ")
                TEXT("intentionally does not stop your PIE session just to persist a new asset).");
        }
        else
        {
            bSaved = mcp_common::SaveAssetPackage(Package, NewBlueprint, SaveError);
        }
    }
    else
    {
        // A Blueprint already in this package that the registry did not list (see the CreatePackage/FullyLoad
        // call above: one an earlier create_blueprint left only in memory): saved stays false.
        SaveError = UnsavedNote(NewBlueprint);
    }

    TSharedPtr<FJsonObject> Reply = BlueprintReply(NewBlueprint, bAlreadyExists ? NewBlueprint->ParentClass.Get() : SelectedParentClass,
        bAlreadyExists, bSaved, SaveError);
    if (!bSave && !bAlreadyExists)
    {
        Reply->SetStringField(TEXT("note"), TEXT("save=false: the Blueprint is registered and its package marked dirty, but not saved; persist it with save_asset"));
    }
    return Reply;
}

// ---------------------------------------------------------------------------
// Forward declarations for the file-local SCS authoring helpers used by
// HandleAddComponentToBlueprint below. Full definitions + doc comments live in
// the "namespace { ... }" block just above HandleAttachBlueprintComponent
// further down this file (shared by both handlers -- do not duplicate them).
// ---------------------------------------------------------------------------
namespace
{
    void MakeChildOfSCSNode(USimpleConstructionScript* SCS, USCS_Node* Child, USCS_Node* NewParent, FName SocketName);
    USceneComponent* FindNativeSceneComponentByName(UBlueprint* BP, const FString& CompName);
}

namespace UEAgentBlueprintCommandsImpl
{
// The ActorComponent class that component_type names, or null. A native class may be named with or without its
// U prefix and Component suffix ("StaticMesh", "UStaticMeshComponent"); a path loads directly: /Script/Module.Class,
// a Blueprint class object path, a Blueprint package path ("/Game/X/AC_Foo", completed to ".AC_Foo_C"), or a bare
// generated-class name ("AC_Foo_C", looked up in the asset registry under /Game). A spelling that resolves to a
// class that is not a component does not end the search. InOutType comes back completed, for the reply.
UClass* ResolveComponentClass(FString& InOutType)
{
    if (InOutType.IsEmpty())
    {
        return nullptr;
    }
    const auto IsComponentClass = [](const UClass* Class) { return Class != nullptr && Class->IsChildOf(UActorComponent::StaticClass()); };

    // Spellings of a native name, taken before the path forms below complete InOutType.
    TArray<FString> NativeSpellings;
    const bool bHasSuffix = InOutType.EndsWith(TEXT("Component"));
    for (const TCHAR* Prefix : { TEXT(""), TEXT("U") })
    {
        if (*Prefix != TEXT('\0') && InOutType.StartsWith(TEXT("U")))
        {
            continue;
        }
        NativeSpellings.AddUnique(Prefix + InOutType);
        if (!bHasSuffix)
        {
            NativeSpellings.AddUnique(Prefix + InOutType + TEXT("Component"));
        }
    }

    if (InOutType.Contains(TEXT("/")) && !InOutType.StartsWith(TEXT("/Script/")))
    {
        // A Blueprint component class is "<package>.<name>_C"; a package path lacks the second half.
        int32 Slash = INDEX_NONE;
        InOutType.FindLastChar(TEXT('/'), Slash);
        const FString Leaf = InOutType.RightChop(Slash + 1);
        if (!Leaf.IsEmpty() && !Leaf.Contains(TEXT(".")))
        {
            InOutType = FString::Printf(TEXT("%s.%s_C"), *InOutType, *Leaf);
        }
    }
    else if (!InOutType.Contains(TEXT("/")) && !InOutType.Contains(TEXT(".")) && InOutType.EndsWith(TEXT("_C")))
    {
        // The generated class of a Blueprint asset found by name; left as typed when there is none.
        const FString AssetName = InOutType.LeftChop(2);
        FARFilter BlueprintsUnderGame;
        BlueprintsUnderGame.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
        BlueprintsUnderGame.bRecursiveClasses = true;
        BlueprintsUnderGame.PackagePaths.Add(TEXT("/Game"));
        BlueprintsUnderGame.bRecursivePaths = true;
        TArray<FAssetData> Candidates;
        IAssetRegistry::GetChecked().GetAssets(BlueprintsUnderGame, Candidates);
        const FAssetData* Named = Candidates.FindByPredicate(
            [&AssetName](const FAssetData& Candidate) { return Candidate.AssetName.ToString() == AssetName; });
        if (Named != nullptr)
        {
            InOutType = FString::Printf(TEXT("%s.%s_C"), *Named->PackageName.ToString(), *AssetName);
        }
    }

    if (InOutType.Contains(TEXT("/")) || InOutType.Contains(TEXT(".")))
    {
        UClass* Loaded = LoadObject<UClass>(nullptr, *InOutType, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (Loaded == nullptr)
        {
            Loaded = LoadClass<UActorComponent>(nullptr, *InOutType, nullptr, LOAD_NoWarn | LOAD_Quiet);
        }
        if (IsComponentClass(Loaded))
        {
            return Loaded;
        }
    }

    // A bare name: a loaded class first (native preferred), then the engine's script package.
    for (const FString& Spelling : NativeSpellings)
    {
        UClass* Found = FindFirstObject<UClass>(*Spelling, EFindFirstObjectOptions::NativeFirst);
        if (!IsComponentClass(Found))
        {
            Found = LoadObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/Engine.%s"), *Spelling), nullptr, LOAD_NoWarn | LOAD_Quiet);
        }
        if (IsComponentClass(Found))
        {
            return Found;
        }
    }
    return nullptr;
}
}

// MCP:add_component_to_blueprint - Add a component of any class (engine class or a Blueprint AC_* class) to a Blueprint's SCS, with an optional relative transform and parent, then compile. Pick it to change the Blueprint so every instance gets the component; add_component_to_actor adds one to a single placed actor only, and attach_blueprint_component re-parents a component that already exists. TRAP 1: on a CHILD Blueprint whose scene root is INHERITED, a SCENE component can end up written into the PARENT's SCS (source:"inherited", owner_class = the parent) -- which silently edits a vendor/library asset; non-scene ActorComponents land correctly on the child. TRAP 2: a scene component of a BLUEPRINT class, or one given a location, can report success without persisting and then materialize LATER as a duplicate. Both have the same oracle: get_blueprint_components right after, checking owner_class and source of the new node -- never the return value.
//   @param blueprint_name - Name of the blueprint
//   @param component_type - Component class: a native short name ('StaticMesh', 'Box', 'Sphere'), a /Script path ('/Script/Engine.StaticMeshComponent'), OR a Blueprint component class given as its object path ('/Game/.../AC_Foo.AC_Foo_C'), a bare package path ('/Game/.../AC_Foo'), or a bare '<Name>_C' ('AC_Foo_C').
//   @param component_name - Name for the new component instance
//   @param location - Optional [x, y, z] relative location for scene components
//   @param rotation - Optional [pitch, yaw, roll] relative rotation for scene components
//   @param scale - Optional [x, y, z] relative scale for scene components
//   @param transform - Optional {"location":[x,y,z], "rotation":[p,y,r], "scale":[x,y,z]} alias for scene components; any flat location/rotation/scale arg above wins over the matching axis in here if both are given.
//   @param parent_component_name - Optional parent for scene components; omit / "root" attaches under the Blueprint's EXISTING scene root (SCS or native) instead of force-adding a second orphan root node. May also name a NATIVE inherited scene component (e.g. ACharacter's "CharacterMesh0"), mirroring attach_blueprint_component. Ignored for non-scene ActorComponents (no transform, no hierarchy to join).
//   @param socket_name - Optional socket/bone name on the parent (AttachToName)
//   @returns { component_name: string, component_type: string } -- an ECHO of the request with no owner, no source and no verification. It says nothing about WHERE the node landed: read get_blueprint_components afterwards.

TSharedPtr<FJsonObject> HandleAddComponentToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentType, ComponentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // These two are reported missing under their short names, 'type' and 'name'.
    if (!Params->TryGetStringField(TEXT("component_type"), ComponentType))
    {
        return mcp_common::MissingParameterReply(TEXT("type"));
    }
    if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
    {
        return mcp_common::MissingParameterReply(TEXT("name"));
    }

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    // component_type may name the class several ways; the reply echoes the spelling that resolved.
    UClass* ComponentClass = UEAgentBlueprintCommandsImpl::ResolveComponentClass(ComponentType);
    if (ComponentClass == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown component type: %s"), *ComponentType));
    }

    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        return mcp_common::MakeErrorReply(TEXT("Blueprint has no SimpleConstructionScript"));
    }

    FString ParentComponentName;
    Params->TryGetStringField(TEXT("parent_component_name"), ParentComponentName);
    FString SocketName;
    Params->TryGetStringField(TEXT("socket_name"), SocketName);
    const FName SocketFName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);

    // Resolve the EXISTING scene root BEFORE creating/linking the new node
    // (GetSceneRootComponentTemplate must reflect the tree as it stood prior to this call, not the
    // just-added node).
    USCS_Node* SceneRootNode = nullptr;
    USceneComponent* NativeRoot = SCS->GetSceneRootComponentTemplate(/*bShouldUseDefaultRoot=*/false, &SceneRootNode);

    // An explicit parent (scene components only) is resolved BEFORE the node exists, so a parent
    // that is not found fails the call without leaving a new node and template linked to nothing.
    const bool bSceneClass = ComponentClass->IsChildOf(USceneComponent::StaticClass());
    const bool bExplicitParent = bSceneClass && !ParentComponentName.IsEmpty() && !ParentComponentName.Equals(TEXT("root"), ESearchCase::IgnoreCase);
    USCS_Node* ExplicitParentNode = nullptr;
    USceneComponent* ExplicitNativeParent = nullptr;
    if (bExplicitParent)
    {
        ExplicitParentNode = SCS->FindSCSNode(*ParentComponentName);
        if (ExplicitParentNode == nullptr)
        {
            ExplicitNativeParent = FindNativeSceneComponentByName(Blueprint, ParentComponentName);
        }
        if (ExplicitParentNode == nullptr && ExplicitNativeParent == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName));
        }
    }

    // The node is created only now, and linked into the tree further down once its kind is known.
    USCS_Node* NewNode = SCS->CreateNode(ComponentClass, *ComponentName);
    if (!NewNode)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add component to blueprint"));
    }

    USceneComponent* SceneComponent = Cast<USceneComponent>(NewNode->ComponentTemplate);
    if (SceneComponent)
    {
        // The tool's summary says "with optional transform", so callers pass a nested
        // "transform": {"location":[...], "rotation":[...], "scale":[...]} object as often as the flat
        // top-level location/rotation/scale fields. Both forms are accepted: flat fields win if both
        // are present for the same axis.
        const TSharedPtr<FJsonObject>* TransformObj = nullptr;
        const bool bHasTransformObj = Params->TryGetObjectField(TEXT("transform"), TransformObj) && TransformObj && TransformObj->IsValid();
        TSharedPtr<FJsonObject> NestedTransform;
        if (bHasTransformObj)
        {
            NestedTransform = *TransformObj;
        }
        auto NestedHas = [&NestedTransform](const TCHAR* Key)
        {
            return NestedTransform.IsValid() && NestedTransform->HasField(Key);
        };

        // Resolve the three axes first, then write whichever were given (location, rotation, scale).
        TOptional<FVector> RelLocation;
        TOptional<FRotator> RelRotation;
        TOptional<FVector> RelScale;
        if (Params->HasField(TEXT("location")))
        {
            RelLocation = mcp_common::ReadJsonVector(Params, TEXT("location"));
        }
        else if (NestedHas(TEXT("location")))
        {
            RelLocation = mcp_common::ReadJsonVector(NestedTransform, TEXT("location"));
        }
        if (Params->HasField(TEXT("rotation")))
        {
            RelRotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
        }
        else if (NestedHas(TEXT("rotation")))
        {
            RelRotation = mcp_common::ReadJsonRotator(NestedTransform, TEXT("rotation"));
        }
        if (Params->HasField(TEXT("scale")))
        {
            RelScale = mcp_common::ReadJsonVector(Params, TEXT("scale"));
        }
        else if (NestedHas(TEXT("scale")))
        {
            RelScale = mcp_common::ReadJsonVector(NestedTransform, TEXT("scale"));
        }

        if (RelLocation.IsSet())
        {
            SceneComponent->SetRelativeLocation(RelLocation.GetValue());
        }
        if (RelRotation.IsSet())
        {
            SceneComponent->SetRelativeRotation(RelRotation.GetValue());
        }
        if (RelScale.IsSet())
        {
            SceneComponent->SetRelativeScale3D(RelScale.GetValue());
        }
    }

    // USimpleConstructionScript::AddNode() unconditionally drops the new node into RootNodes (engine
    // SimpleConstructionScript.cpp). For a USceneComponent added to a Blueprint that ALREADY has a
    // root, that makes a SECOND orphan root SCS node; at runtime ExecuteScriptOnActor snapshots
    // Actor->GetRootComponent() ONCE before the node loop (SimpleConstructionScript.cpp), and every
    // orphan root with no ParentComponentOrVariableName falls back to that stale snapshot -- so the LAST
    // orphan root processed clobbers Actor->SetRootComponent (SCS_Node.cpp) and earlier ones end up
    // registered but structurally disconnected from the actor's real hierarchy. So a scene component with no
    // explicit parent is attached under the Blueprint's REAL existing root (SCS or native) instead of
    // force-adding another root.
    // Deliberately NOT using USubobjectDataSubsystem here -- it crashes when the component class is
    // itself a Blueprint-generated ("BP class") type; MakeChildOfSCSNode/FindNativeSceneComponentByName
    // (defined below, shared with HandleAttachBlueprintComponent) mirror what that subsystem does
    // internally without ever going through it.
    if (SceneComponent)
    {
        if (bExplicitParent)
        {
            // Explicit parent requested (mirrors attach_blueprint_component's contract); resolved above.
            if (ExplicitParentNode != nullptr)
            {
                MakeChildOfSCSNode(SCS, NewNode, ExplicitParentNode, SocketFName);
            }
            else
            {
                SCS->AddNode(NewNode); // native-parented nodes live in RootNodes
                NewNode->SetParent(ExplicitNativeParent); // const USceneComponent* overload -> bIsParentComponentNative=true
                NewNode->AttachToName = SocketFName;
            }
        }
        else if (SceneRootNode)
        {
            // Blueprint already has an SCS-node scene root -> attach under it, don't clobber it.
            MakeChildOfSCSNode(SCS, NewNode, SceneRootNode, SocketFName);
        }
        else if (NativeRoot)
        {
            // Root is a NATIVE inherited component (e.g. ACharacter's CapsuleComponent) -> link natively.
            SCS->AddNode(NewNode);
            NewNode->SetParent(NativeRoot);
            NewNode->AttachToName = SocketFName;
        }
        else
        {
            // No root exists yet at all -- this new component becomes the root.
            SCS->AddNode(NewNode);
        }
    }
    else
    {
        // Non-scene ActorComponent (e.g. a gameplay AC_* component): no transform, no hierarchy to
        // join -- always a root SCS node.
        SCS->AddNode(NewNode);
    }

    // Mark structurally modified BEFORE compiling so any open Blueprint Editor tab's cached SCS tree
    // view (and dependents) is invalidated; CompileBlueprint alone does not do this. Matches
    // HandleAttachBlueprintComponent.
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // An echo of the request (component_type as resolved above), not a read-back of the tree.
    TSharedPtr<FJsonObject> Echo = MakeShared<FJsonObject>();
    Echo->SetStringField(TEXT("component_name"), ComponentName);
    Echo->SetStringField(TEXT("component_type"), ComponentType);
    return Echo;
}

// MCP:set_physics_properties - Set simulate-physics, mass override, linear and angular damping on ONE primitive component TEMPLATE of a Blueprint (an OWN SCS node only -- inherited components are not found here, and it is the template, not a placed actor). Only the fields present are touched. It does NOT compile and does NOT save. For any other property of a component template use set_blueprint_component_properties, which is ICH-aware and reaches inherited components.
//   @retired set_blueprint_component_properties - blueprint_name, component_name, property_name="BodyInstance", property_value={"bSimulatePhysics":true,"bOverrideMass":true,"MassInKgOverride":50,"LinearDamping":..,"AngularDamping":..}
//   @param blueprint_name - Name of the blueprint
//   @param component_name - Name of an OWN SCS component; must resolve to a UPrimitiveComponent or the call errors
//   @param simulate_physics - Optional bool to toggle physics simulation
//   @param mass - Optional mass override in kg (applied as SetMassOverrideInKg, not a scale)
//   @param linear_damping - Optional linear damping coefficient
//   @param angular_damping - Optional angular damping coefficient
//   @returns { component: string } -- just the component name back. NOTHING is read back, so verify with get_blueprint_component_property on each property you set.

TSharedPtr<FJsonObject> HandleSetPhysicsProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    UBlueprint* Blueprint = nullptr;
    UPrimitiveComponent* Primitive = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentBlueprintCommandsImpl::ReadOwnComponentTemplate(
            Params, TEXT("Component is not a primitive component"), BlueprintName, ComponentName, Blueprint, Primitive))
    {
        return Error;
    }

    // Each setting is applied only when its key is present in the request.
    if (Params->HasField(TEXT("simulate_physics")))
    {
        Primitive->SetSimulatePhysics(Params->GetBoolField(TEXT("simulate_physics")));
    }
    if (Params->HasField(TEXT("mass")))
    {
        // An absolute override in kilograms rather than a scale of the computed mass.
        const float MassKg = Params->GetNumberField(TEXT("mass"));
        Primitive->SetMassOverrideInKg(NAME_None, MassKg);
        UE_LOG(LogTemp, Verbose, TEXT("set_physics_properties: %s mass override %f kg"), *ComponentName, MassKg);
    }
    if (Params->HasField(TEXT("linear_damping")))
    {
        Primitive->SetLinearDamping(Params->GetNumberField(TEXT("linear_damping")));
    }
    if (Params->HasField(TEXT("angular_damping")))
    {
        Primitive->SetAngularDamping(Params->GetNumberField(TEXT("angular_damping")));
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("component"), ComponentName);
    return Reply;
}

namespace UEAgentBlueprintCommandsImpl
{
// The messages of a compile, for compile_blueprint(return_messages): severity, text, and the node the message is
// about when one of its graph tokens points at the node or at one of its pins. A message about no node (a
// class-level one) carries empty node fields.
TArray<TSharedPtr<FJsonValue>> CompilerMessages(const FCompilerResultsLog& Results)
{
    TArray<TSharedPtr<FJsonValue>> Listed;
    for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
    {
        const EMessageSeverity::Type Severity = Message->GetSeverity();
        const TCHAR* SeverityName = TEXT("note");
        if (Severity == EMessageSeverity::Error)
        {
            SeverityName = TEXT("error");
        }
        else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
        {
            SeverityName = TEXT("warning");
        }

        const UEdGraphNode* Subject = nullptr;
        for (const TSharedRef<IMessageToken>& Token : Message->GetMessageTokens())
        {
            if (Token->GetType() != EMessageToken::EdGraph)
            {
                continue;
            }
            const FEdGraphToken& GraphToken = static_cast<const FEdGraphToken&>(Token.Get());
            Subject = Cast<UEdGraphNode>(GraphToken.GetGraphObject());
            if (Subject == nullptr && GraphToken.GetPin() != nullptr)
            {
                Subject = GraphToken.GetPin()->GetOwningNodeUnchecked();
            }
            if (Subject != nullptr)
            {
                break;
            }
        }
        const UEdGraph* SubjectGraph = Subject ? Subject->GetGraph() : nullptr;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("severity"), SeverityName);
        Entry->SetStringField(TEXT("text"), Message->ToText().ToString());
        Entry->SetStringField(TEXT("node_id"), Subject ? Subject->NodeGuid.ToString() : FString());
        Entry->SetStringField(TEXT("node_title"), Subject ? Subject->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
        Entry->SetStringField(TEXT("graph_name"), SubjectGraph ? SubjectGraph->GetName() : FString());
        Listed.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return Listed;
}
}

// MCP:compile_blueprint - Compile a Blueprint to regenerate its class and return the real outcome: succeeded is true only with zero errors, with the error and warning counts and, on request, every compiler message tied to its node.
//   @para Compile after adding nodes, functions, variables or RPCs, and before anything calls them or Play-in-Editor starts. To look for errors without compiling, use check_blueprint_errors for one Blueprint or find_erroring_blueprints for a folder. It does not save: save_asset writes the result.
//   @para blueprint_name takes a short name or, safer, the full path that find_assets_by_name returns, since a short name shared by two assets compiles one of them. Example: {"blueprint_name": "/Game/Blueprints/BP_Door", "return_messages": true}. With return_messages each message carries the node_id and graph that get_graph_nodes and remove_graph_node take.
//   @para The compile blocks the editor until it ends. Blueprints that depend on this one get their bytecode recompiled, instances of its class are reinstanced, and a garbage collection runs afterwards, so a Blueprint with many dependents costs more and other packages can end up dirty. During Play-in-Editor the reinstancing leaves a world behind that kills the editor when PIE stops, which is why the call is refused then (see force).
//   @para num_errors 0 does not prove the graph works: an unresolved wildcard pin on an array or ForEach node can compile clean and still block PIE, and a "was pruned" warning means consumers read defaults, so read the warnings too.
//   @core
//   @param blueprint_name - Name of the blueprint, or its full asset path (e.g. /Game/MyGame/Core/BP_MyActor). A short name matching two assets compiles ONE of them silently -- pass the full path when in doubt.
//   @param return_messages - Optional (default false). When true, ALSO returns the per-message compiler results log so callers can see node-anchored errors/warnings. When false/absent the response omits the `messages` array but still reports the REAL compile status.
//   @returns { name, compiled, succeeded, num_errors, num_warnings, messages?: [{ severity: 'error'|'warning'|'note', text, node_id, node_title, graph_name }] } -- messages only with return_messages; the node fields are empty for a message tied to no node.

TSharedPtr<FJsonObject> HandleCompileBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    // A short name that matches several assets resolves to one of them; the caller is told so in the tool notes.
    FString BlueprintName;
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = UEAgentBlueprintCommandsImpl::ResolveBlueprintParam(
        Params, &UEAgentBlueprintCommandsImpl::MissingQuotedParam, BlueprintName, Error);
    if (!Blueprint)
    {
        return Error;
    }
    bool bReturnMessages = false;
    Params->TryGetBoolField(TEXT("return_messages"), bReturnMessages);

    // The outcome is read off the results log, never assumed: compiled and succeeded mean no errors (warnings still
    // compile). The log stays out of the output log unless the caller asked for the messages.
    FCompilerResultsLog Results;
    Results.SetSilentMode(!bReturnMessages);
    if (UWidgetBlueprint* AsWidgetBlueprint = Cast<UWidgetBlueprint>(Blueprint))
    {
        UMGWidgetUtils::RegisterUntrackedWidgetVariables(AsWidgetBlueprint);
    }
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

    const bool bErrorFree = Results.NumErrors == 0;
    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetBoolField(TEXT("succeeded"), bErrorFree);
    Report->SetBoolField(TEXT("compiled"), bErrorFree);
    Report->SetNumberField(TEXT("num_errors"), Results.NumErrors);
    Report->SetNumberField(TEXT("num_warnings"), Results.NumWarnings);
    Report->SetStringField(TEXT("name"), BlueprintName);
    if (bReturnMessages)
    {
        Report->SetArrayField(TEXT("messages"), UEAgentBlueprintCommandsImpl::CompilerMessages(Results));
    }
    return Report;
}

// MCP:spawn_blueprint_actor - Spawn an instance of a Blueprint class as an actor and report WHICH world took it and whether the actor has AUTHORITY. Pick it for any Blueprint class; spawn_actor places the built-in engine types (StaticMeshActor, lights, CameraActor, Character), and the typed spawn_* tools place an engine class with its own fields. The default (world absent or 'editor') places it in the EDITOR level, not in play. For anything that must exist for every player during PIE use world='server'; world='pie' is ambiguous and can land on the CLIENT, where the actor has no authority, no AI controller and no server-side existence while looking identical in the payload. The actor is NOT saved into the map: save_level afterwards for an editor-world spawn.
//   @retired spawn_actor - type=<Blueprint asset path>, name, location, rotation, world (has_authority is in the reply)
//   @param blueprint_name - Name of the blueprint
//   @param actor_name - Label applied to the spawned actor (SetActorLabel); it is not the object name and is not made unique
//   @param location - Optional [x, y, z] world location
//   @param rotation - Optional [pitch, yaw, roll] world rotation
//   @param world - Optional world selector: absent/'editor' (default) spawns into the editor level | 'server' spawns into the PIE world WITH AUTHORITY (what you want for anything that must exist for every player: an actor spawned into the client world has no authority and no server-side existence) | 'client' | 'pie:N' | 'pie' (ambiguous: PlayWorld, else the first PIE context -- can land on the CLIENT) | 'game' (alias of 'pie')
//   @returns { actor JSON via DescribeActorObject, world: string, has_authority: bool (AActor::HasAuthority() read off the spawned actor), local_role: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

// CANONICAL WORLD RESOLUTION. "PIE" is ambiguous in a multiplayer session: PlayWorld, or the first
// PIE context, can be a CLIENT world, where a spawned actor has no authority (no AI controller, no
// server-side existence). `world="server"` addresses the world that has it.
//
// Resolve the target world for a spawn from the "world" param:
//   absent / 'editor' (default) -> the editor world (backward-compatible).
//   'server' -> the PIE world WITH AUTHORITY (what an actor that must exist for everyone needs).
//   'client' / 'pie:N' -> that specific PIE world.
//   'pie' / 'game' -> ambiguous: PlayWorld, else the first PIE context. May well be the client.
// Returns nullptr + fills OutError (which lists the worlds that DO exist) on failure.
// OutWorldMode receives the selector actually used.
static UWorld* ResolveSpawnWorld(const TSharedPtr<FJsonObject>& Params, FString& OutError, FString& OutWorldMode)
{
    UWorld* Resolved = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutWorldMode, OutError);
    if (!Resolved || !Resolved->IsValidLowLevel())
    {
        if (OutError.IsEmpty())
        {
            OutError = TEXT("No valid world found");
        }
        return nullptr;
    }
    return Resolved;
}

TSharedPtr<FJsonObject> HandleSpawnBlueprintActor(const TSharedPtr<FJsonObject>& Params)
{
    // Every log line of this function carries the function name as its prefix.
    auto Trace = [](const FString& Line)
    {
        UE_LOG(LogTemp, Verbose, TEXT("HandleSpawnBlueprintActor: %s"), *Line);
    };
    auto TraceError = [](const FString& Line)
    {
        UE_LOG(LogTemp, Error, TEXT("HandleSpawnBlueprintActor: %s"), *Line);
    };

    Trace(TEXT("Starting blueprint actor spawn"));

    FString BlueprintName, ActorName;
    if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
    {
        TraceError(TEXT("Missing blueprint_name parameter"));
        return mcp_common::MissingParameterReply(TEXT("blueprint_name"));
    }
    if (!Params->TryGetStringField(TEXT("actor_name"), ActorName))
    {
        TraceError(TEXT("Missing actor_name parameter"));
        return mcp_common::MissingParameterReply(TEXT("actor_name"));
    }

    Trace(FString::Printf(TEXT("Looking for blueprint '%s'"), *BlueprintName));
    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        TraceError(FString::Printf(TEXT("Blueprint not found: %s"), *BlueprintName));
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }
    Trace(TEXT("Blueprint found, getting transform parameters"));

    // Absent keys keep the identity placement.
    FVector SpawnLocation = FVector::ZeroVector;
    FRotator SpawnRotation = FRotator::ZeroRotator;
    if (Params->HasField(TEXT("location")))
    {
        SpawnLocation = mcp_common::ReadJsonVector(Params, TEXT("location"));
        Trace(FString::Printf(TEXT("Location set to (%f, %f, %f)"), SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z));
    }
    if (Params->HasField(TEXT("rotation")))
    {
        SpawnRotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
        Trace(FString::Printf(TEXT("Rotation set to (%f, %f, %f)"), SpawnRotation.Pitch, SpawnRotation.Yaw, SpawnRotation.Roll));
    }

    // "world" chooses the destination: the editor level by default, or a live PIE world
    // (server / client / pie:N / pie) so the actor exists in play instead of the edited level.
    FString WorldError, WorldMode;
    UWorld* World = ResolveSpawnWorld(Params, WorldError, WorldMode);
    if (!World)
    {
        TraceError(WorldError);
        return mcp_common::MakeErrorReply(WorldError);
    }

    Trace(TEXT("Creating spawn transform"));
    const FTransform SpawnTransform(FQuat(SpawnRotation), SpawnLocation);

    // A short pause lets the engine settle a class that may have just been recompiled.
    FPlatformProcess::Sleep(0.2f);

    UClass* SpawnClass = Blueprint->GeneratedClass;
    Trace(FString::Printf(TEXT("About to spawn actor from blueprint '%s' with GeneratedClass: %s"),
        *BlueprintName, SpawnClass ? *SpawnClass->GetName() : TEXT("NULL")));
    AActor* Spawned = World->SpawnActor<AActor>(SpawnClass, SpawnTransform);
    Trace(FString::Printf(TEXT("SpawnActor completed, NewActor: %s"), Spawned ? *Spawned->GetName() : TEXT("NULL")));

    if (!Spawned)
    {
        TraceError(TEXT("Failed to spawn blueprint actor"));
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn blueprint actor"));
    }

    Trace(FString::Printf(TEXT("Setting actor label to '%s'"), *ActorName));
    Spawned->SetActorLabel(ActorName);

    Trace(TEXT("About to convert actor to JSON"));
    TSharedPtr<FJsonObject> Result = mcp_common::DescribeActorObject(Spawned, true);
    if (Result.IsValid())
    {
        // Name the world that took the actor and its real authority: a client-world spawn exists
        // for nobody else, yet would otherwise look exactly like a server-side one in this reply.
        const UEnum* NetRoleEnum = StaticEnum<ENetRole>();
        Result->SetStringField(TEXT("world"), WorldMode);
        Result->SetBoolField(TEXT("has_authority"), Spawned->HasAuthority());
        Result->SetStringField(TEXT("local_role"), NetRoleEnum
            ? NetRoleEnum->GetNameStringByValue(static_cast<int64>(Spawned->GetLocalRole()))
            : FString());
        mcp_common::AddWorldEvidence(Result, World);
    }
    Trace(TEXT("JSON conversion completed, returning result"));
    return Result;
}

// MCP:set_static_mesh_properties - Assign a static mesh asset and/or a slot-0 material to a mesh component TEMPLATE of a Blueprint (an OWN SCS node, not a placed actor) through the component's SetStaticMesh/SetMaterial. Does NOT compile and does NOT save; the reply reads the template back (static_mesh, material and a *_set flag for each). A path that does not load as a mesh or material, or a call with neither, fails with nothing changed. Use apply_material_to_blueprint when only a material is changing or the slot is not 0, and set_blueprint_component_properties for any other template property or an inherited component.
//   @retired set_blueprint_component_properties - blueprint_name, component_name, property_name="StaticMesh", property_value="/Game/SM_..."; the material with apply_material_to_blueprint
//   @param blueprint_name - Name of the blueprint
//   @param component_name - Name of an OWN SCS mesh component
//   @param static_mesh - Optional asset path of the UStaticMesh to assign; a path that does not load as a StaticMesh fails the call and nothing is changed
//   @param material - Optional asset path of the UMaterialInterface for slot 0; a path that does not load as a material fails the call and nothing is changed. At least one of static_mesh and material is required.
//   @returns { component: string, static_mesh: string, material: string, static_mesh_set: bool, material_set: bool } -- read back from the template: static_mesh is the mesh it holds and material its slot-0 material (the override, else the mesh's own), each an object path or "None"; <field>_set is true when that field was passed and the template now holds it. Nothing is compiled or saved.

TSharedPtr<FJsonObject> HandleSetStaticMeshProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    UBlueprint* Blueprint = nullptr;
    UStaticMeshComponent* MeshTemplate = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentBlueprintCommandsImpl::ReadOwnComponentTemplate(
            Params, TEXT("Component is not a static mesh component"), BlueprintName, ComponentName, Blueprint, MeshTemplate))
    {
        return Error;
    }

    // Both assignments are optional, but one that is passed must load as the right type. Both are resolved
    // before the first write, so a refused call leaves the template exactly as it was.
    const auto LoadParamAsset = [&Params](const TCHAR* Key, UClass* WantedClass, UObject*& OutAsset, FString& OutError) -> bool
    {
        OutAsset = nullptr;
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        if (!Value.IsValid() || Value->IsNull())
        {
            return true;
        }
        FString AssetPath;
        Value->TryGetString(AssetPath);
        UObject* Loaded = AssetPath.IsEmpty() ? nullptr : UEditorAssetLibrary::LoadAsset(AssetPath);
        if (!Loaded || !Loaded->IsA(WantedClass))
        {
            const FString Found = Loaded ? FString::Printf(TEXT(" (it is a %s)"), *Loaded->GetClass()->GetName()) : FString();
            OutError = FString::Printf(TEXT("%s '%s' does not load as a %s%s; nothing was changed"),
                Key, *AssetPath, *WantedClass->GetName(), *Found);
            return false;
        }
        OutAsset = Loaded;
        return true;
    };
    UObject* MeshAsset = nullptr;
    UObject* MaterialAsset = nullptr;
    FString LoadError;
    if (!LoadParamAsset(TEXT("static_mesh"), UStaticMesh::StaticClass(), MeshAsset, LoadError)
        || !LoadParamAsset(TEXT("material"), UMaterialInterface::StaticClass(), MaterialAsset, LoadError))
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    UStaticMesh* NewMesh = Cast<UStaticMesh>(MeshAsset);
    UMaterialInterface* SlotZeroMaterial = Cast<UMaterialInterface>(MaterialAsset);
    if (!NewMesh && !SlotZeroMaterial)
    {
        return mcp_common::MakeErrorReply(TEXT("Pass static_mesh and/or material; nothing was changed"));
    }

    if (NewMesh)
    {
        MeshTemplate->SetStaticMesh(NewMesh);
    }
    if (SlotZeroMaterial)
    {
        MeshTemplate->SetMaterial(0, SlotZeroMaterial);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    // Read back from the template: the reply says what it now holds, not what was asked for. The editor
    // material is the one shown in the details panel, without a Nanite override standing in for it.
    const UStaticMesh* HeldMesh = MeshTemplate->GetStaticMesh();
    const UMaterialInterface* HeldMaterial = MeshTemplate->GetEditorMaterial(0);
    const bool bMeshSet = NewMesh && HeldMesh == NewMesh;
    const bool bMaterialSet = SlotZeroMaterial && HeldMaterial == SlotZeroMaterial;

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("component"), ComponentName);
    Reply->SetStringField(TEXT("static_mesh"), HeldMesh ? HeldMesh->GetPathName() : FString(TEXT("None")));
    Reply->SetStringField(TEXT("material"), HeldMaterial ? HeldMaterial->GetPathName() : FString(TEXT("None")));
    Reply->SetBoolField(TEXT("static_mesh_set"), bMeshSet);
    Reply->SetBoolField(TEXT("material_set"), bMaterialSet);
    if ((NewMesh && !bMeshSet) || (SlotZeroMaterial && !bMaterialSet))
    {
        return mcp_common::MakeOutcomeReply(Reply, false,
            TEXT("The component template did not take every value passed; static_mesh and material say what it holds"));
    }
    return Reply;
}

// MCP:set_mesh_material_color - Tint one material slot of a Blueprint component template by creating a DYNAMIC material instance (MID) over the current or given material, setting one vector parameter, and assigning the MID to the slot. A MID is a RUNTIME object, so this is a quick preview, NOT the way to author a change that must survive save/reload -- for a persistent tint make a MaterialInstanceConstant and assign it with apply_material_to_blueprint. A parameter_name the material does not declare is accepted silently and simply does nothing.
//   @param blueprint_name - Name of the blueprint
//   @param component_name - Name of an OWN SCS component; must be a UPrimitiveComponent
//   @param color - RGBA color [R, G, B, A] as floats, each clamped to 0.0-1.0; exactly 4 values are required
//   @param material_slot - Optional slot index; defaults to 0
//   @param parameter_name - Optional vector parameter name; defaults to "BaseColor". A name the material does not declare is NOT reported as an error -- it just has no visible effect.
//   @param material_path - Optional material asset path to use as the MID's parent; without it the component's current slot material is used, falling back to /Engine/BasicShapes/BasicShapeMaterial
//   @returns { component: string, material_slot: int, parameter_name: string, color: [number, number, number, number], success: bool } -- success:true only means the MID was created and assigned; re-read with get_blueprint_material_info after a save/reload to see whether anything persisted.

TSharedPtr<FJsonObject> HandleSetMeshMaterialColor(const TSharedPtr<FJsonObject>& Params)
{
    // Any primitive works: static meshes, skeletal meshes, shapes...
    FString BlueprintName, ComponentName;
    UBlueprint* Blueprint = nullptr;
    UPrimitiveComponent* Primitive = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentBlueprintCommandsImpl::ReadOwnComponentTemplate(
            Params, TEXT("Component is not a primitive component"), BlueprintName, ComponentName, Blueprint, Primitive))
    {
        return Error;
    }

    // RGBA: exactly four numbers, each clamped into [0, 1].
    const TArray<TSharedPtr<FJsonValue>>* Channels = nullptr;
    if (!Params->TryGetArrayField(TEXT("color"), Channels) || Channels->Num() != 4)
    {
        return mcp_common::MakeErrorReply(TEXT("'color' must be an array of 4 float values [R, G, B, A]"));
    }
    float Rgba[4];
    for (int32 ChannelIndex = 0; ChannelIndex < 4; ++ChannelIndex)
    {
        Rgba[ChannelIndex] = static_cast<float>(FMath::Clamp((*Channels)[ChannelIndex]->AsNumber(), 0.0, 1.0));
    }
    const FLinearColor Tint(Rgba[0], Rgba[1], Rgba[2], Rgba[3]);

    int32 SlotIndex = 0;
    if (Params->HasField(TEXT("material_slot")))
    {
        SlotIndex = Params->GetIntegerField(TEXT("material_slot"));
    }
    FString VectorParamName = TEXT("BaseColor");
    Params->TryGetStringField(TEXT("parameter_name"), VectorParamName);

    // Parent of the dynamic instance: material_path when given (it must load), otherwise what the
    // slot currently holds, otherwise the engine's basic shape material.
    FString MaterialPath;
    const bool bExplicitParent = Params->TryGetStringField(TEXT("material_path"), MaterialPath);
    UMaterialInterface* ParentMaterial = nullptr;
    if (bExplicitParent)
    {
        ParentMaterial = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    }
    else
    {
        ParentMaterial = Primitive->GetMaterial(SlotIndex);
        if (!ParentMaterial)
        {
            ParentMaterial = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(TEXT("/Engine/BasicShapes/BasicShapeMaterial")));
        }
    }
    if (!ParentMaterial)
    {
        return mcp_common::MakeErrorReply(bExplicitParent
            ? FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath)
            : FString(TEXT("No material found on component and failed to load default material")));
    }

    UMaterialInstanceDynamic* TintInstance = UMaterialInstanceDynamic::Create(ParentMaterial, Primitive);
    if (!TintInstance)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create dynamic material instance"));
    }
    TintInstance->SetVectorParameterValue(FName(*VectorParamName), Tint);
    Primitive->SetMaterial(SlotIndex, TintInstance);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    UE_LOG(LogTemp, Log, TEXT("set_mesh_material_color: %s tinted (%f, %f, %f, %f)"), *ComponentName, Tint.R, Tint.G, Tint.B, Tint.A);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("component"), ComponentName);
    Reply->SetNumberField(TEXT("material_slot"), SlotIndex);
    Reply->SetStringField(TEXT("parameter_name"), VectorParamName);
    Reply->SetArrayField(TEXT("color"), mcp_common::ColorToJsonArray(Tint));
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// Materials: discovery, assignment on templates, and per-slot read-back

// MCP:get_available_materials - Enumerate material assets under a content path and return each one's name, object path, package and class -- the object path is what apply_material_to_blueprint / set_actor_material expect. One Asset Registry query on UMaterialInterface and all its subclasses (materials, material instances, project or plugin material classes); nothing is loaded, so a wide path is cheap. For one known name prefer resolve_asset_by_name.
//   @param search_path - Optional content path filter (e.g. "/Game/Materials"); empty/absent means the whole project and is reported back as "/Game/"
//   @param include_engine_materials - When true, also include /Engine/ materials; defaults to true, so the list is large unless a search_path narrows it
//   @returns { materials: [{ name, path, package, class }], count: int, search_path_used: string } -- there is no truncation flag: count is everything that matched.

TSharedPtr<FJsonObject> HandleGetAvailableMaterials(const TSharedPtr<FJsonObject>& Params)
{
    // No search_path means the whole project, reported back as "/Game/".
    FString SearchRoot;
    if (!Params->TryGetStringField(TEXT("search_path"), SearchRoot))
    {
        SearchRoot.Reset();
    }
    bool bWithEngineContent = true;
    if (Params->HasField(TEXT("include_engine_materials")))
    {
        bWithEngineContent = Params->GetBoolField(TEXT("include_engine_materials"));
    }

    // A caller path is normalised to "/Dir/" so the registry treats it as a folder.
    if (!SearchRoot.IsEmpty())
    {
        if (!SearchRoot.StartsWith(TEXT("/")))
        {
            SearchRoot.InsertAt(0, TEXT('/'));
        }
        if (!SearchRoot.EndsWith(TEXT("/")))
        {
            SearchRoot.AppendChar(TEXT('/'));
        }
    }
    const FString ListingRoot = SearchRoot.IsEmpty() ? FString(TEXT("/Game/")) : SearchRoot;

    // The asset registry alone, on UMaterialInterface AND every subclass (Material, the instance
    // classes, any project or plugin material class): nothing is loaded. The recursive class filter
    // already finds what an exact-class filter would miss, so no asset is loaded to find materials --
    // on a large project, with no search_path, that would be thousands of them.
    FARFilter MaterialFilter;
    MaterialFilter.bRecursivePaths = true;
    MaterialFilter.bRecursiveClasses = true;
    MaterialFilter.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
    FString FilterRoot = ListingRoot;
    FilterRoot.RemoveFromEnd(TEXT("/"));
    MaterialFilter.PackagePaths.Add(FName(*FilterRoot));
    UE_LOG(LogTemp, Log, TEXT("get_available_materials: searching %s"), *ListingRoot);
    if (bWithEngineContent)
    {
        MaterialFilter.PackagePaths.Add(FName(TEXT("/Engine")));
        UE_LOG(LogTemp, Log, TEXT("get_available_materials: engine materials included"));
    }

    IAssetRegistry& Registry = mcp_common::AssetRegistry();
    TArray<FAssetData> Found;
    Registry.GetAssets(MaterialFilter, Found);
    UE_LOG(LogTemp, Log, TEXT("get_available_materials: %d from the asset registry"), Found.Num());

    TArray<TSharedPtr<FJsonValue>> Rows;
    Rows.Reserve(Found.Num());
    for (const FAssetData& Entry : Found)
    {
        const FString EntryName = Entry.AssetName.ToString();
        const FString EntryObjectPath = Entry.GetObjectPathString();
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), EntryName);
        Row->SetStringField(TEXT("path"), EntryObjectPath);
        Row->SetStringField(TEXT("package"), Entry.PackageName.ToString());
        Row->SetStringField(TEXT("class"), Entry.AssetClassPath.ToString());
        Rows.Add(MakeShared<FJsonValueObject>(Row));
        UE_LOG(LogTemp, Verbose, TEXT("get_available_materials: %s (%s)"), *EntryName, *EntryObjectPath);
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetArrayField(TEXT("materials"), Rows);
    Reply->SetNumberField(TEXT("count"), Rows.Num());
    Reply->SetStringField(TEXT("search_path_used"), ListingRoot);
    return Reply;
}

// MCP:apply_material_to_blueprint - Assign a material ASSET to one slot of a Blueprint component TEMPLATE (an OWN SCS node, not a placed actor) -- the persistent counterpart of set_mesh_material_color, which only builds a runtime dynamic instance. Does NOT compile and does NOT save. Actors ALREADY placed in a level keep their serialized material: change the template and the placed instances stay as they were.
//   @param blueprint_name - Name of the blueprint
//   @param component_name - Name of an OWN SCS component; must be a UPrimitiveComponent
//   @param material_path - Asset path of the UMaterialInterface (material or material instance) to assign; a path that does not load is an error
//   @param material_slot - Optional material slot index; defaults to 0. It is not range-checked against the mesh's slot count.
//   @returns { blueprint_name: string, component_name: string, material_path: string, material_slot: int, success: bool } -- an echo: success:true only means the material loaded and SetMaterial was called. Read it back with get_blueprint_material_info.

TSharedPtr<FJsonObject> HandleApplyMaterialToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = UEAgentBlueprintCommandsImpl::ReadBlueprintComponentNames(
            Params, &UEAgentBlueprintCommandsImpl::MissingQuotedParam, BlueprintName, ComponentName))
    {
        return Missing;
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // Not range-checked against the mesh's slot count.
    const int32 SlotIndex = Params->HasField(TEXT("material_slot")) ? Params->GetIntegerField(TEXT("material_slot")) : 0;

    UBlueprint* Blueprint = nullptr;
    TSharedPtr<FJsonObject> ResolveError;
    UPrimitiveComponent* Primitive = UEAgentBlueprintCommandsImpl::ResolveOwnComponentTemplate<UPrimitiveComponent>(
        BlueprintName, ComponentName, TEXT("Component is not a primitive component"), Blueprint, ResolveError);
    if (!Primitive)
    {
        return ResolveError;
    }

    UMaterialInterface* NewMaterial = Cast<UMaterialInterface>(UEditorAssetLibrary::LoadAsset(MaterialPath));
    if (!NewMaterial)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load material: %s"), *MaterialPath));
    }
    Primitive->SetMaterial(SlotIndex, NewMaterial);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    // An echo of the request: nothing is read back from the template here.
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("component_name"), ComponentName);
    Reply->SetStringField(TEXT("material_path"), MaterialPath);
    Reply->SetNumberField(TEXT("material_slot"), SlotIndex);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:get_actor_material_info - List the material slots of an actor PLACED IN THE EDITOR WORLD, one row per slot per StaticMeshComponent, with the material actually assigned (name, path, class) or "None". Scope limits worth knowing: it only walks UStaticMeshComponents (a skeletal mesh's slots are invisible here) and only the EDITOR world (no PIE selector), and the actor is matched by its object NAME, not its outliner label. The Blueprint-template twin is get_blueprint_material_info.
//   @param actor_name - Object name of the actor in the EDITOR world (GetName(), not the display label shown in the outliner)
//   @returns { actor_name: string, material_slots: [{ slot, component, material_name, material_path, material_class }], total_slots: int } -- total_slots 0 means no StaticMeshComponent had slots, NOT that the actor has no materials.

TSharedPtr<FJsonObject> HandleGetActorMaterialInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to get editor world"));
    }

    // Matched on the object name (GetName), not the outliner label; the first hit wins.
    AActor* Hit = mcp_common::FindActorByObjectName(EditorWorld, ActorName);
    if (!Hit)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Actor not found: %s"), *ActorName));
    }

    // One row per slot of every StaticMeshComponent; other mesh kinds are not walked.
    TArray<UStaticMeshComponent*> StaticMeshes;
    Hit->GetComponents<UStaticMeshComponent>(StaticMeshes);
    TArray<TSharedPtr<FJsonValue>> SlotRows;
    for (UStaticMeshComponent* SmComponent : StaticMeshes)
    {
        if (!SmComponent)
        {
            continue;
        }
        const FString MeshLabel = SmComponent->GetName();
        const int32 SlotCount = SmComponent->GetNumMaterials();
        for (int32 Slot = 0; Slot < SlotCount; ++Slot)
        {
            SlotRows.Add(MakeShared<FJsonValueObject>(
                UEAgentBlueprintCommandsImpl::MaterialSlotRow(Slot, MeshLabel, SmComponent->GetMaterial(Slot))));
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("actor_name"), ActorName);
    Reply->SetArrayField(TEXT("material_slots"), SlotRows);
    Reply->SetNumberField(TEXT("total_slots"), SlotRows.Num());
    return Reply;
}

// MCP:get_blueprint_material_info - Read the material slots of ONE StaticMeshComponent TEMPLATE in a Blueprint (an OWN SCS node), with the material assigned to each slot or "None". This is the verification step after apply_material_to_blueprint / set_blueprint_component_properties. The slot count comes from the assigned static mesh's LOD 0 sections, so with no mesh assigned it returns ZERO slots and has_static_mesh:false -- which is a missing mesh, not a missing material. For a placed actor use get_actor_material_info.
//   @param blueprint_name - Name of the blueprint
//   @param component_name - Name of an OWN SCS component; it must be a UStaticMeshComponent (any other type is an error)
//   @returns { blueprint_name: string, component_name: string, material_slots: [{ slot, component, material_name, material_path, material_class }], total_slots: int, has_static_mesh: bool } -- read has_static_mesh FIRST: total_slots:0 with has_static_mesh:false means no mesh was assigned, so the array says nothing about materials.

TSharedPtr<FJsonObject> HandleGetBlueprintMaterialInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    UBlueprint* Blueprint = nullptr;
    UStaticMeshComponent* MeshTemplate = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentBlueprintCommandsImpl::ReadOwnComponentTemplate(
            Params, TEXT("Component is not a static mesh component"), BlueprintName, ComponentName, Blueprint, MeshTemplate))
    {
        return Error;
    }

    // The slot count comes from the assigned mesh (its LOD 0 sections): without a mesh there is
    // nothing to enumerate, which has_static_mesh:false tells the caller.
    TArray<TSharedPtr<FJsonValue>> SlotRows;
    UStaticMesh* AssignedMesh = MeshTemplate->GetStaticMesh();
    if (!AssignedMesh)
    {
        UE_LOG(LogTemp, Warning, TEXT("Component %s of Blueprint %s has no static mesh assigned"), *ComponentName, *BlueprintName);
    }
    else
    {
        const int32 SectionCount = AssignedMesh->GetNumSections(0);
        for (int32 Slot = 0; Slot < SectionCount; ++Slot)
        {
            SlotRows.Add(MakeShared<FJsonValueObject>(
                UEAgentBlueprintCommandsImpl::MaterialSlotRow(Slot, ComponentName, MeshTemplate->GetMaterial(Slot))));
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("component_name"), ComponentName);
    Reply->SetArrayField(TEXT("material_slots"), SlotRows);
    Reply->SetNumberField(TEXT("total_slots"), SlotRows.Num());
    Reply->SetBoolField(TEXT("has_static_mesh"), AssignedMesh != nullptr);
    return Reply;
}

// Structure: overview, components, function graphs and variables

// MCP:get_blueprint_info - One cheap overview of a Blueprint: parent class, compile status, and the counts + names of its OWN components, variables, function graphs and ubergraph pages. Everything here is what THIS asset declares -- nothing inherited, and interface-override graphs are not listed. So use it to orient, and the specialists to decide: get_blueprint_components for the real component set including ancestors, get_blueprint_functions for interface/macro/delegate graphs, get_blueprint_variables for variable flags, get_graph_nodes to read a graph.
//   @pack blueprint_authoring
//   @param blueprint_name - Name of the blueprint
//   @returns { blueprint_name: string, parent_class: string, components: [{ name, type }], component_count: int, variables: [{ name, type }], variable_count: int, functions: [{ name }], function_count: int, graphs: [{ name, type }], graph_count: int, has_event_graph: bool, event_graph_name?: string, status: string } -- parent_class is a short NAME (ambiguous between an engine class and a same-named project one), variable "type" is only the pin CATEGORY (no struct/class), and status is UpToDate/Error/Unknown.

TSharedPtr<FJsonObject> HandleGetBlueprintInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = UEAgentBlueprintCommandsImpl::ResolveBlueprintParam(
        Params, &mcp_common::MissingParameterReply, BlueprintName, Error);
    if (!Blueprint)
    {
        return Error;
    }

    // Everything below is what THIS asset declares: no inherited components, variables or graphs.
    TArray<TSharedPtr<FJsonValue>> ComponentRows;
    if (USimpleConstructionScript* OwnScs = Blueprint->SimpleConstructionScript)
    {
        for (USCS_Node* ScsNode : OwnScs->GetAllNodes())
        {
            if (!ScsNode || !ScsNode->ComponentTemplate)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("name"), ScsNode->GetVariableName().ToString());
            Row->SetStringField(TEXT("type"), ScsNode->ComponentTemplate->GetClass()->GetName());
            ComponentRows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    // "type" is only the pin category of each variable.
    TArray<TSharedPtr<FJsonValue>> VariableRows;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Variable.VarName.ToString());
        Row->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
        VariableRows.Add(MakeShared<FJsonValueObject>(Row));
    }

    // "graphs" lists the ubergraph pages first, then the function graphs; "functions" repeats the latter.
    TArray<TSharedPtr<FJsonValue>> FunctionRows;
    TArray<TSharedPtr<FJsonValue>> GraphRows;
    auto AddGraphRow = [&GraphRows](const UEdGraph* Graph, const TCHAR* Kind)
    {
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Graph->GetName());
        Row->SetStringField(TEXT("type"), Kind);
        GraphRows.Add(MakeShared<FJsonValueObject>(Row));
    };
    for (UEdGraph* Page : Blueprint->UbergraphPages)
    {
        if (Page)
        {
            AddGraphRow(Page, TEXT("Ubergraph"));
        }
    }
    for (UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
    {
        if (!FunctionGraph)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), FunctionGraph->GetName());
        FunctionRows.Add(MakeShared<FJsonValueObject>(Row));
        AddGraphRow(FunctionGraph, TEXT("Function"));
    }

    const TCHAR* StatusText = TEXT("Unknown");
    if (Blueprint->Status == BS_UpToDate)
    {
        StatusText = TEXT("UpToDate");
    }
    else if (Blueprint->Status == BS_Error)
    {
        StatusText = TEXT("Error");
    }

    TSharedPtr<FJsonObject> Overview = MakeShared<FJsonObject>();
    // The asset's own name, not the lookup string the caller passed.
    Overview->SetStringField(TEXT("blueprint_name"), Blueprint->GetName());
    Overview->SetStringField(TEXT("parent_class"), Blueprint->ParentClass ? Blueprint->ParentClass->GetName() : FString(TEXT("None")));
    Overview->SetArrayField(TEXT("components"), ComponentRows);
    Overview->SetNumberField(TEXT("component_count"), ComponentRows.Num());
    Overview->SetArrayField(TEXT("variables"), VariableRows);
    Overview->SetNumberField(TEXT("variable_count"), VariableRows.Num());
    Overview->SetArrayField(TEXT("functions"), FunctionRows);
    Overview->SetNumberField(TEXT("function_count"), FunctionRows.Num());
    Overview->SetArrayField(TEXT("graphs"), GraphRows);
    Overview->SetNumberField(TEXT("graph_count"), GraphRows.Num());
    UEdGraph* EventGraph = UBlueprintEditorLibrary::FindEventGraph(Blueprint);
    Overview->SetBoolField(TEXT("has_event_graph"), EventGraph != nullptr);
    if (EventGraph)
    {
        Overview->SetStringField(TEXT("event_graph_name"), EventGraph->GetName());
    }
    Overview->SetStringField(TEXT("status"), StatusText);
    return Overview;
}

// ---------------------------------------------------------------------------
// SCS structural-edit census + shared batch removal.
// Used by remove_component_from_blueprint.
// ---------------------------------------------------------------------------
namespace
{
    // An SCS keeps TWO node collections that must agree: the flat AllNodes registry and
    // the RootNodes -> ChildNodes tree. USimpleConstructionScript::RemoveNode on a NON-root
    // node unlinks it from its parent's ChildNodes (and from AllNodes), but its DESCENDANTS
    // stay in AllNodes while unreachable from any root -- silent orphans that
    // no read tool surfaces. ValidateSceneRootNodes can additionally move/drop DefaultSceneRoot.
    // Censusing BOTH sets before and after is the only honest way to report what a removal did.
    //
    // The target is resolved by NAME, so a removal never hits a neighbour by index: what can go
    // with it is the orphaning above, plus ValidateSceneRootNodes relocating the scene root.
    struct FScsCensus_BPCmd
    {
        TSet<FName>   AllNodes;
        TSet<FName>   Reachable;
        TArray<FName> RootNames;
    };

    void TakeScsCensus_BPCmd(USimpleConstructionScript* SCS, FScsCensus_BPCmd& Out)
    {
        Out.AllNodes.Reset();
        Out.Reachable.Reset();
        Out.RootNames.Reset();
        if (!SCS)
        {
            return;
        }

        for (USCS_Node* N : SCS->GetAllNodes())
        {
            if (N)
            {
                Out.AllNodes.Add(N->GetVariableName());
            }
        }

        // Explicit visited set: a corrupted parent/child cycle must NOT hang the game thread
        // (this runs inside the editor tick that serves the MCP session).
        TSet<USCS_Node*>   Visited;
        TArray<USCS_Node*> Stack;
        for (USCS_Node* R : SCS->GetRootNodes())
        {
            if (R)
            {
                Out.RootNames.Add(R->GetVariableName());
                Stack.Add(R);
            }
        }
        while (Stack.Num() > 0)
        {
            USCS_Node* Cur = Stack.Pop();
            if (!Cur || Visited.Contains(Cur))
            {
                continue;
            }
            Visited.Add(Cur);
            Out.Reachable.Add(Cur->GetVariableName());
            for (USCS_Node* C : Cur->GetChildNodes())
            {
                if (C)
                {
                    Stack.Add(C);
                }
            }
        }
    }

    // The names (a TSet or a TArray of FName) as a JSON string array, in iteration order.
    template <typename NameContainerType>
    TArray<TSharedPtr<FJsonValue>> NamesToJson_BPCmd(const NameContainerType& Names)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Reserve(Names.Num());
        for (const FName& N : Names)
        {
            Out.Add(MakeShared<FJsonValueString>(N.ToString()));
        }
        return Out;
    }

    // Collect the requested component names from either `component_name` (single) or
    // `component_names` (batch). Both may be present; the union is used, order preserved.
    TArray<FString> CollectRequestedComponentNames_BPCmd(const TSharedPtr<FJsonObject>& Params)
    {
        TArray<FString> Names;
        FString Single;
        if (Params->TryGetStringField(TEXT("component_name"), Single) && !Single.IsEmpty())
        {
            Names.AddUnique(Single);
        }
        const TArray<TSharedPtr<FJsonValue>>* Batch = nullptr;
        if (Params->TryGetArrayField(TEXT("component_names"), Batch) && Batch)
        {
            for (const TSharedPtr<FJsonValue>& V : *Batch)
            {
                FString S;
                if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
                {
                    Names.AddUnique(S);
                }
            }
        }
        return Names;
    }

    // The request of remove_component_from_blueprint: blueprint_name, the
    // requested component names and promote_children (default true). The Blueprint, or null with OutError holding
    // the reply when a name is missing, the Blueprint does not resolve or has no construction script.
    UBlueprint* ReadScsRemovalRequest_BPCmd(const TSharedPtr<FJsonObject>& Params, FString& OutBlueprintName,
        TArray<FString>& OutRequestedNames, bool& bOutPromoteChildren, TSharedPtr<FJsonObject>& OutError)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), OutBlueprintName, mcp_common::EAllowEmpty::Yes))
        {
            OutError = Missing;
            return nullptr;
        }

        // component_name and component_names are merged (order kept, duplicates dropped).
        OutRequestedNames = CollectRequestedComponentNames_BPCmd(Params);
        if (OutRequestedNames.Num() == 0)
        {
            OutError = mcp_common::MakeErrorReply(
                TEXT("Missing required parameter: component_name (or a non-empty component_names array)"));
            return nullptr;
        }

        bOutPromoteChildren = true;
        Params->TryGetBoolField(TEXT("promote_children"), bOutPromoteChildren);

        UBlueprint* Blueprint = mcp_common::ResolveBlueprint(OutBlueprintName);
        if (!Blueprint)
        {
            OutError = mcp_common::BlueprintNotFoundReply(OutBlueprintName);
            return nullptr;
        }
        if (!Blueprint->SimpleConstructionScript)
        {
            OutError = UEAgentBlueprintCommandsImpl::NoConstructionScript();
            return nullptr;
        }
        return Blueprint;
    }

    // Body of remove_component_from_blueprint.
    // Removes N nodes with ONE compile for the whole batch -- chaining single-node calls is
    // exactly the pattern that causes collateral deletions. Fills `Data` with the
    // double census. Returns true only when every requested node is gone from AllNodes AND no
    // NEW orphan appeared; on false, OutError says which of the two failed.
    bool PerformScsRemoval_BPCmd(
        UBlueprint* Blueprint,
        const TArray<FString>& RequestedNames,
        bool bPromoteChildren,
        const TSharedPtr<FJsonObject>& Data,
        FString& OutError)
    {
        USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
        if (!SCS)
        {
            OutError = TEXT("Blueprint has no SimpleConstructionScript");
            return false;
        }

        FScsCensus_BPCmd Before;
        TakeScsCensus_BPCmd(SCS, Before);

        TArray<TSharedPtr<FJsonValue>> RequestedJson;
        TArray<TSharedPtr<FJsonValue>> NotFoundJson;
        TSet<FName>        Targeted;
        TArray<USCS_Node*> Targets;
        for (const FString& Name : RequestedNames)
        {
            RequestedJson.Add(MakeShared<FJsonValueString>(Name));
            // FindSCSNode matches BOTH the variable name and the _GEN_VARIABLE internal name.
            USCS_Node* Node = SCS->FindSCSNode(FName(*Name));
            if (!Node)
            {
                NotFoundJson.Add(MakeShared<FJsonValueString>(Name));
                continue;
            }
            if (Targeted.Contains(Node->GetVariableName()))
            {
                continue; // same node named twice (variable name + _GEN_VARIABLE alias)
            }
            Targeted.Add(Node->GetVariableName());
            Targets.Add(Node);
        }

        // Transact + dirty BEFORE mutating. RemoveNode* edits RootNodes/AllNodes/ChildNodes
        // in place: without Modify() and MarkBlueprintAsStructurallyModified the edit would be
        // neither undoable nor reliably persisted.
        bool bCompiled = false;
        if (Targets.Num() > 0)
        {
            Blueprint->Modify();
            SCS->Modify();
            for (USCS_Node* Node : Targets)
            {
                if (!Node)
                {
                    continue;
                }
                Node->Modify();
                if (bPromoteChildren)
                {
                    // Keeps the subtree alive by re-parenting the children onto this node's
                    // parent (or promoting them to roots). This is the behaviour the editor's
                    // own delete uses; plain RemoveNode orphans descendants.
                    SCS->RemoveNodeAndPromoteChildren(Node);
                }
                else
                {
                    SCS->RemoveNode(Node);
                }
            }
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            FKismetEditorUtilities::CompileBlueprint(Blueprint); // ONE compile for the whole batch
            bCompiled = true;
        }

        // Re-read the SCS pointer: the compile must not be trusted to have kept it.
        SCS = Blueprint->SimpleConstructionScript;
        FScsCensus_BPCmd After;
        TakeScsCensus_BPCmd(SCS, After);

        const TSet<FName> Disappeared   = Before.AllNodes.Difference(After.AllNodes);
        const TSet<FName> OrphansBefore = Before.AllNodes.Difference(Before.Reachable);
        const TSet<FName> OrphansAfter  = After.AllNodes.Difference(After.Reachable);

        TArray<TSharedPtr<FJsonValue>> RemovedJson, AlsoRemovedJson, StillPresentJson, UnexpectedJson;
        for (const FName& N : Disappeared)
        {
            if (Targeted.Contains(N))
            {
                RemovedJson.Add(MakeShared<FJsonValueString>(N.ToString()));
            }
            else
            {
                // Collateral: a descendant that went with its parent (promote_children=false),
                // or anything else the engine dropped. Reported, never hidden.
                AlsoRemovedJson.Add(MakeShared<FJsonValueString>(N.ToString()));
            }
        }
        for (const FName& N : Targeted)
        {
            if (After.AllNodes.Contains(N))
            {
                StillPresentJson.Add(MakeShared<FJsonValueString>(N.ToString()));
            }
        }
        for (const FName& N : OrphansAfter)
        {
            if (!OrphansBefore.Contains(N))
            {
                // Present in AllNodes but unreachable from RootNodes: the exact silent-orphan
                // failure mode of the plain-RemoveNode path.
                UnexpectedJson.Add(MakeShared<FJsonValueString>(N.ToString()));
            }
        }

        Data->SetArrayField(TEXT("requested"), RequestedJson);
        Data->SetArrayField(TEXT("removed_components"), RemovedJson);
        Data->SetArrayField(TEXT("also_removed"), AlsoRemovedJson);
        Data->SetArrayField(TEXT("unexpected_losses"), UnexpectedJson);
        Data->SetArrayField(TEXT("not_found"), NotFoundJson);
        Data->SetArrayField(TEXT("still_present"), StillPresentJson);
        Data->SetNumberField(TEXT("node_count_before"), Before.AllNodes.Num());
        Data->SetNumberField(TEXT("node_count_after"), After.AllNodes.Num());
        Data->SetNumberField(TEXT("reachable_before"), Before.Reachable.Num());
        Data->SetNumberField(TEXT("reachable_after"), After.Reachable.Num());
        Data->SetArrayField(TEXT("orphans_before"), NamesToJson_BPCmd(OrphansBefore));
        Data->SetArrayField(TEXT("orphans_after"), NamesToJson_BPCmd(OrphansAfter));
        Data->SetArrayField(TEXT("root_nodes_before"), NamesToJson_BPCmd(Before.RootNames));
        Data->SetArrayField(TEXT("root_nodes_after"), NamesToJson_BPCmd(After.RootNames));
        Data->SetBoolField(TEXT("root_nodes_changed"), !(Before.RootNames == After.RootNames));
        Data->SetBoolField(TEXT("promote_children"), bPromoteChildren);
        Data->SetBoolField(TEXT("compiled"), bCompiled);
        Data->SetNumberField(TEXT("compile_count"), bCompiled ? 1 : 0);

        if (StillPresentJson.Num() > 0)
        {
            OutError = FString::Printf(
                TEXT("%d requested component(s) are still present in the SCS after the removal (see still_present)"),
                StillPresentJson.Num());
            return false;
        }
        if (UnexpectedJson.Num() > 0)
        {
            OutError = FString::Printf(
                TEXT("%d node(s) became unreachable from RootNodes while still listed in AllNodes (see unexpected_losses)"),
                UnexpectedJson.Num());
            return false;
        }
        return true;
    }
}

// MCP:remove_component_from_blueprint - Remove one or more component nodes from a Blueprint's SCS, with a DOUBLE CENSUS (flat AllNodes vs a walk of RootNodes) so collateral losses are reported instead of hidden: also_removed names descendants that went with their parent and unexpected_losses names nodes left in AllNodes but unreachable from any root. Batch-capable and that is the point -- chaining SINGLE-node calls on the same Blueprint is exactly the pattern that deletes components nobody asked for, so pass component_names and take ONE compile for the lot. A re-added component comes back with its CLASS defaults: per-instance overrides are gone and do not come back on save. IDEMPOTENT, so it is safe in scripts that re-run: a requested component that is not there is listed in not_found, and when none of them is there the call still succeeds with already_removed:true and nothing removed.
//   @param blueprint_name - Name/path of the blueprint
//   @param component_name - Name of the component to remove (single form; matches the variable name or the _GEN_VARIABLE internal name)
//   @param component_names - Optional array of component names removed as ONE batch with a SINGLE compile. Chaining single-node calls is the pattern that causes collateral deletions. Merged with component_name when both are given.
//   @param promote_children - Optional bool, default true: keep descendants by re-parenting them (RemoveNodeAndPromoteChildren). Pass false for plain RemoveNode, which orphans the whole subtree.
//   @returns { success, error?, data: { blueprint_name, removed_component, component_name, removed, already_removed, requested[], removed_components[], also_removed[], unexpected_losses[], not_found[], still_present[], node_count_before, node_count_after, reachable_before, reachable_after, orphans_before[], orphans_after[], root_nodes_before[], root_nodes_after[], root_nodes_changed, promote_children, compiled, compile_count, success, verification } } -- removed: at least one node went; already_removed: none of the requested names was there (a no-op, not an error).

TSharedPtr<FJsonObject> HandleRemoveComponentFromBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    TArray<FString> RequestedNames;
    bool bPromoteChildren = true;
    TSharedPtr<FJsonObject> RequestError;
    UBlueprint* Blueprint = ReadScsRemovalRequest_BPCmd(Params, BlueprintName, RequestedNames, bPromoteChildren, RequestError);
    if (!Blueprint)
    {
        return RequestError;
    }

    // Remove (double census inside the shared helper)
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_name"), BlueprintName);
    // The first requested name, under both of the names callers read it by.
    Data->SetStringField(TEXT("removed_component"), RequestedNames[0]);
    Data->SetStringField(TEXT("component_name"), RequestedNames[0]);

    FString RemoveError;
    const bool bOk = PerformScsRemoval_BPCmd(Blueprint, RequestedNames, bPromoteChildren, Data, RemoveError);
    Data->SetBoolField(TEXT("success"), bOk);

    // `removed` / `already_removed` describe the batch as a whole. A name this call did NOT find was
    // already gone: that stays idempotent, not an error.
    const TArray<TSharedPtr<FJsonValue>>* RemovedArr = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* NotFoundArr = nullptr;
    const int32 RemovedNum  = (Data->TryGetArrayField(TEXT("removed_components"), RemovedArr) && RemovedArr) ? RemovedArr->Num() : 0;
    const int32 NotFoundNum = (Data->TryGetArrayField(TEXT("not_found"), NotFoundArr) && NotFoundArr) ? NotFoundArr->Num() : 0;
    Data->SetBoolField(TEXT("removed"), RemovedNum > 0);
    Data->SetBoolField(TEXT("already_removed"), NotFoundNum == RequestedNames.Num());

    return mcp_common::MakeVerifiedReply(
        Data, bOk,
        bOk ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("scs_double_census"),
        TEXT("AllNodes set + RootNodes->ChildNodes walk compared before and after the removal"),
        RemoveError);
}

namespace UEAgentBlueprintCommandsImpl
{
// The function the Blueprint inherits under Name, when a Blueprint can really override it: an interface
// function, or a BlueprintImplementableEvent / BlueprintNativeEvent of a parent class (the editor's 'Override'
// list). GetOverrideFunctionClass matches any inherited member by name, a native BlueprintCallable included, so
// that is checked here. OutDeclaringClass is the interface or class that declares it.
UFunction* OverridableInheritedFunction(UBlueprint* Blueprint, const FName Name, UClass*& OutDeclaringClass)
{
    UFunction* Inherited = nullptr;
    OutDeclaringClass = FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, Name, &Inherited);
    if (Inherited == nullptr || OutDeclaringClass == nullptr)
    {
        return nullptr;
    }
    const bool bFromInterface = FBlueprintEditorUtils::GetInterfaceFunction(Blueprint, Name) != nullptr;
    return (bFromInterface || UEdGraphSchema_K2::CanKismetOverrideFunction(Inherited)) ? Inherited : nullptr;
}

// Whether the Blueprint has a function graph of its own (not an interface, macro or event graph) named Name.
bool HasOwnFunctionGraph(const UBlueprint* Blueprint, const FName Name)
{
    return Blueprint->FunctionGraphs.ContainsByPredicate(
        [Name](const TObjectPtr<UEdGraph>& Graph) { return Graph && Graph->GetFName() == Name; });
}

// Whether Graph is the graph of a function of an interface the Blueprint implements.
bool IsImplementedInterfaceGraph(const UBlueprint* Blueprint, UEdGraph* Graph)
{
    return Blueprint->ImplementedInterfaces.ContainsByPredicate(
        [Graph](const FBPInterfaceDescription& Interface) { return Interface.Graphs.Contains(Graph); });
}

// What add_blueprint_function answers: the function graph the Blueprint has for the request.
TSharedPtr<FJsonObject> FunctionGraphReply(const FString& BlueprintName, const FString& FunctionName, const UEdGraph& Graph)
{
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetBoolField(TEXT("success"), true);
    Reply->SetStringField(TEXT("graph_name"), Graph.GetName());
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("function_name"), FunctionName);
    return Reply;
}

// The reply of an override: which shape it took, whether it was there already, and the graph to wire.
TSharedPtr<FJsonObject> OverrideReply(const FString& BlueprintName, const FString& FunctionName, const UEdGraph& Graph,
    const TCHAR* Kind, bool bAlreadyExisted)
{
    TSharedPtr<FJsonObject> Reply = FunctionGraphReply(BlueprintName, FunctionName, Graph);
    Reply->SetStringField(TEXT("kind"), Kind);
    Reply->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Reply->SetBoolField(TEXT("created"), !bAlreadyExisted);
    Reply->SetBoolField(TEXT("was_already_overridden"), bAlreadyExisted);
    return Reply;
}

// add_blueprint_function(override=true): creates or resolves what IMPLEMENTS the inherited member FunctionName
// (an interface member or a parent virtual). A member WITH a return value gets a real FUNCTION graph -- in
// ImplementedInterfaces[].Graphs for an interface, where a plain new function is refused and an event node would
// be a useless return-less stub -- and an event-shape member (no return value) gets a genuine event OVERRIDE node
// in the event graph, placed at node_position when the params give one. Idempotent in both branches.
TSharedPtr<FJsonObject> ImplementInheritedMember(UBlueprint* Blueprint, const FString& BlueprintName, const FString& FunctionName,
    const TSharedPtr<FJsonObject>& Params)
{
    FScopedTransaction Transaction(NSLOCTEXT("UEAgentBlueprintCommands", "ImplInheritedMember", "MCP: Override Inherited Function"));
    Blueprint->Modify();

    // 1) Conform: for every implemented interface, (re)create the FUNCTION graph of any interface method that has a
    //    return value / outputs (cannot be an event) in ImplementedInterfaces[].Graphs, with the correct Entry+Result
    //    terminators. Idempotent -- a no-op if the graph already exists.
    FBlueprintEditorUtils::ConformImplementedInterfaces(Blueprint);

    // 2) Resolvable as a function graph now? FindGraphByNameDeep searches interface graphs, so it catches the
    //    just-conformed graph as well as any pre-existing function override.
    if (UEdGraph* Existing = FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, FunctionName))
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        return OverrideReply(BlueprintName, FunctionName, *Existing, TEXT("function"), true);
    }

    // 3) Not a function graph yet -> resolve the override target (interface method OR parent virtual).
    //    GetOverrideFunctionClass checks interface functions first, then walks the parent hierarchy.
    UFunction* OverrideFunc = nullptr;
    UClass* OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, FName(*FunctionName), &OverrideFunc);
    if (!OverrideFunc || !OverrideFuncClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No implementable interface/overridable function named '%s' on '%s' (is the interface implemented?)"),
            *FunctionName, *BlueprintName));
    }

    // 4) If it CAN be an event (no return value), it belongs in the event graph as an event OVERRIDE
    //    (bOverrideFunction=true), not in a function graph. Mirrors UBlueprintEditorLibrary::AddEventOverride;
    //    idempotent via FindOverrideForFunction.
    if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(OverrideFunc))
    {
        UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Blueprint);
        if (!EventGraph)
        {
            return mcp_common::MakeErrorReply(TEXT("Blueprint has no event graph to place the interface event override"));
        }

        // The event is referenced on the class that DECLARES it (what FBlueprintEventNodeSpawner does). Once the
        // Blueprint overrides it, its skeleton class carries a stub of the event, GetOverrideFunctionClass answers the
        // Blueprint's own class, and FindOverrideForFunction(that class) misses the existing node -- a second call
        // would add a duplicate event. So the declaring class is the root of the super-function chain, and an
        // override event of that name found anywhere in the Blueprint is THE override (there is one per name).
        const UFunction* RootDeclaration = OverrideFunc;
        while (const UFunction* SuperFunction = RootDeclaration->GetSuperFunction())
        {
            RootDeclaration = SuperFunction;
        }
        UClass* EventOwnerClass = RootDeclaration->GetOwnerClass() ? RootDeclaration->GetOwnerClass()->GetAuthoritativeClass() : OverrideFuncClass;
        const FName EventName(*FunctionName);
        UK2Node_Event* EventNode = FBlueprintEditorUtils::FindOverrideForFunction(Blueprint, EventOwnerClass, EventName);
        if (!EventNode)
        {
            TArray<UK2Node_Event*> ExistingEvents;
            FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(Blueprint, ExistingEvents);
            for (UK2Node_Event* Candidate : ExistingEvents)
            {
                if (Candidate && Candidate->bOverrideFunction && Candidate->EventReference.GetMemberName() == EventName)
                {
                    EventNode = Candidate;
                    break;
                }
            }
        }
        const bool bAlready = (EventNode != nullptr);
        if (!EventNode)
        {
            EventGraph->Modify();
            FGraphNodeCreator<UK2Node_Event> Creator(*EventGraph);
            EventNode = Creator.CreateNode();
            EventNode->EventReference.SetExternalMember(EventName, EventOwnerClass);
            EventNode->bOverrideFunction = true;
            // Only a node made here is placed; an existing override stays where it is.
            if (Params->HasField(TEXT("node_position")))
            {
                const FVector2D Position = mcp_common::ReadJsonVector2D(Params, TEXT("node_position"));
                EventNode->NodePosX = (int32)Position.X;
                EventNode->NodePosY = (int32)Position.Y;
            }
            Creator.Finalize();
            FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
            EventGraph->NotifyGraphChanged();
        }

        TSharedPtr<FJsonObject> Reply = OverrideReply(BlueprintName, FunctionName, *EventGraph, TEXT("event"), bAlready);
        Reply->SetStringField(TEXT("node_id"), EventNode->NodeGuid.ToString());
        return Reply;
    }

    // 5) Create the function-override graph exactly like SMyBlueprint::ImplementFunction: a new graph whose
    //    Entry/Result terminators are built from the override function's signature (return pin included).
    UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
        Blueprint, FName(*FunctionName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (!NewGraph)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create the override function graph"));
    }
    FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, /*bIsUserCreated=*/false, OverrideFuncClass);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    return OverrideReply(BlueprintName, FunctionName, *NewGraph, TEXT("function"), false);
}
}

// MCP:add_blueprint_function - Create a NEW function graph on a Blueprint, or with override=true a real OVERRIDE of an inherited/interface member: an event-shape member (no return value) becomes a genuine event OVERRIDE node in the event graph (node_id returned, placed at node_position), a value-returning one a real override FUNCTION graph (in ImplementedInterfaces[].Graphs for an interface member) -- the only correct route for interface events, since a hand-placed event node can compile yet never fire. The override is idempotent (was_already_overridden) and leaves the Blueprint needing a compile, unsaved. It REFUSES rather than guessing: a name that collides with an overridable inherited function errors with a hint instead of silently creating a "<name>_0" shadow, and an EVENT-shape member already shadowed by a local function graph errors telling you to remove that graph first. The new graph does not exist in the generated class until compile_blueprint, so compile BEFORE add_function_parameter or any node that calls it -- otherwise a build_blueprint_graph step fails with "Function not found" and rolls the whole batch back.
//   @param blueprint_name - Name of the blueprint (for override=true it must already implement the interface / inherit the virtual; implement_interface first).
//   @param function_name - Name for the new function; with override=true, the EXACT name of the inherited (interface or parent-class) function to override (e.g. "CanInteract", "GetInteractText").
//   @param override - Optional. When true, create an OVERRIDE of an inherited function ('My Blueprint > Override'): resolves the interface/parent-virtual signature and creates the override graph (function-shape) or event override node (event-shape) with the inherited signature + exact name -- NOT a new deduped function. When omitted and the name collides with an inherited overridable function, the call errors with a hint (instead of silently making a '<name>_0' shadow); pass override=false to force a brand-new function anyway.
//   @param node_position [array] - Optional [x, y] canvas position for an event-override node that override=true CREATES (an existing one is not moved; a function graph has no single node to place).
//   @returns { blueprint_name: string, function_name: string, graph_name: string, success: bool [, kind, already_existed, created, was_already_overridden, node_id, already_exists, is_interface_function, note] } -- with override=true: kind='function' (graph_name is an editable function-override graph, ready to wire with get_graph_nodes / connect_pins) or kind='event' (an event OVERRIDE node was created or reused in the event graph graph_name names, and node_id is that node); created is !already_existed, and was_already_overridden is already_existed. success:true can come back WITHOUT the graph actually appearing, so confirm with get_blueprint_functions before wiring it.

TSharedPtr<FJsonObject> HandleAddBlueprintFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // `override` has three answers: true overrides an inherited function, false creates a new function whatever
    // the name, and leaving it out refuses a name that would only shadow an overridable inherited function.
    const bool bOverrideStated = Params->HasField(TEXT("override"));
    bool bOverride = false;
    Params->TryGetBoolField(TEXT("override"), bOverride);

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    const FName Name(*FunctionName);
    UClass* DeclaringClass = nullptr;
    UFunction* Inherited = UEAgentBlueprintCommandsImpl::OverridableInheritedFunction(Blueprint, Name, DeclaringClass);

    if (bOverride)
    {
        if (Inherited == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("'%s' is not an overridable inherited function on '%s'. Only interface functions and BlueprintImplementableEvent/BlueprintNativeEvent functions can be overridden (see list_overridable_functions). Omit override (or pass override=false) to create a new function instead."),
                *FunctionName, *BlueprintName));
        }
        // A member without a return value is overridden by an event node, never by a function graph, so a function
        // graph of the same name only shadows it -- what re-parenting onto a class that newly declares the event
        // leaves behind. ImplementInheritedMember would take that graph for the override and answer
        // already_existed, so the graph has to go first.
        if (UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Inherited) && UEAgentBlueprintCommandsImpl::HasOwnFunctionGraph(Blueprint, Name))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("'%s' is an EVENT-shape inherited member (declared on %s), but a local FUNCTION graph named '%s' already exists and SHADOWS it -- it does NOT override the base contract (classic re-parent leftover). Remove the local function first (remove_blueprint_function '%s') and retry so the real event override is created."),
                *FunctionName, *DeclaringClass->GetName(), *FunctionName, *FunctionName));
        }
        // The override takes the inherited signature and the exact name: an override graph or an event node, by the
        // member's shape.
        return UEAgentBlueprintCommandsImpl::ImplementInheritedMember(Blueprint, BlueprintName, FunctionName, Params);
    }

    if (UEdGraph* Existing = UBlueprintEditorLibrary::FindGraph(Blueprint, Name))
    {
        // The graph of an interface function that returns a value comes with the interface and is the one to edit,
        // so it is reported as there rather than as a clash.
        if (!UEAgentBlueprintCommandsImpl::IsImplementedInterfaceGraph(Blueprint, Existing))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function already exists: %s"), *FunctionName));
        }
        TSharedPtr<FJsonObject> Reply = UEAgentBlueprintCommandsImpl::FunctionGraphReply(BlueprintName, FunctionName, *Existing);
        Reply->SetBoolField(TEXT("already_exists"), true);
        Reply->SetBoolField(TEXT("is_interface_function"), true);
        Reply->SetStringField(TEXT("note"), TEXT("Interface-override function graph (has a return value): it already exists and is editable. Wire it with get_graph_nodes/connect_pins/set_pin_default_value, or call add_blueprint_function with override=true."));
        return Reply;
    }

    // Under an inherited name AddFunctionGraph makes "<name>_0": a new function that looks like an override and only
    // shadows the inherited one. That takes the caller saying so (override=false).
    if (Inherited != nullptr && !bOverrideStated)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' is an inherited overridable function (declared on %s). add_blueprint_function would create a NEW deduped '%s_0' shadow, NOT an override. Pass override=true to override it (inherited signature + exact name), or override=false to force a brand-new function anyway."),
            *FunctionName, *DeclaringClass->GetName(), *FunctionName));
    }

    // The generated class has the new function after the next compile.
    UEdGraph* Created = UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, FunctionName);
    if (!Created)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to add function graph"));
    }
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    return UEAgentBlueprintCommandsImpl::FunctionGraphReply(BlueprintName, FunctionName, *Created);
}

// MCP:remove_blueprint_function - Delete a function graph OWNED BY the Blueprint (Blueprint->FunctionGraphs) by name. WARNING: It resolves more graphs than it can delete: name lookup goes through FindGraph (all graphs, interface overrides and macros included) but the delete only touches FunctionGraphs and only when bAllowDeletion is true, so an INTERFACE OVERRIDE resolves and cannot be removed. That case is a hard error naming the reason -- to change an interface override, author the signature on the INTERFACE and re-create it (add_function_parameter on the BPI_ -> compile_blueprint -> add_blueprint_function with override=true). Not idempotent: a name that does not resolve is an error. TRAP when deleting to re-create: the class is NOT regenerated here, so an immediate add_blueprint_function with the same name fails with "is an inherited overridable function (declared on <this very class>)" because the stale UFunction is still in the generated class -- put a compile_blueprint BETWEEN the two calls. Callers of the removed function are left broken until the next compile. A macro graph: remove_blueprint_macro.
//   @param blueprint_name - Name of the blueprint
//   @param function_name - Name of the function graph to remove; must exist, be owned by this Blueprint and be deletable, or the call errors
//   @returns { blueprint_name: string, removed_function: string, success: bool } -- success:true means the graph is gone from Blueprint->FunctionGraphs (re-read after the delete), not merely that the call ran.

TSharedPtr<FJsonObject> HandleRemoveBlueprintFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    // FindGraph searches EVERY graph of the asset (interface overrides and macros included).
    const FName GraphFName(*FunctionName);
    UEdGraph* ResolvedGraph = UBlueprintEditorLibrary::FindGraph(Blueprint, GraphFName);
    if (!ResolvedGraph)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function not found: %s"), *FunctionName));
    }

    // The resolve and the delete disagree about WHICH graphs exist, and the delete returns
    // nothing, so the mismatch is checked here:
    //   - FindGraph (BlueprintEditorLibrary.cpp) walks UBlueprint::GetAllGraphs, which appends
    //     ImplementedInterfaces[].Graphs (Blueprint.cpp) => an interface override RESOLVES.
    //   - RemoveFunctionGraph (BlueprintEditorLibrary.cpp) only iterates Blueprint->FunctionGraphs
    //     and additionally requires bAllowDeletion, which an interface-override graph has FALSE.
    // Either way the delete is a silent no-op that leaves the graph whole (same NodeGuids, same
    // wiring), so success is decided by re-reading FunctionGraphs afterwards.
    const bool bWasOwnFunctionGraph = Blueprint->FunctionGraphs.Contains(ResolvedGraph);

    UBlueprintEditorLibrary::RemoveFunctionGraph(Blueprint, GraphFName);

    // POST-CHECK: FBlueprintEditorUtils::RemoveGraph does FunctionGraphs.Remove(GraphToRemove)
    // (BlueprintEditorUtils.cpp), so a real delete leaves this Contains false. Report BEFORE
    // marking the asset modified -- nothing changed, so nothing should be dirtied.
    if (Blueprint->FunctionGraphs.Contains(ResolvedGraph))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Graph '%s' resolved but was NOT removed (bAllowDeletion=%d). RemoveFunctionGraph only "
                 "deletes user-deletable graphs of Blueprint->FunctionGraphs. Nothing was changed."),
            *FunctionName, ResolvedGraph->bAllowDeletion ? 1 : 0));
    }

    if (!bWasOwnFunctionGraph)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Graph '%s' exists but is NOT an own function graph (interface override / macro / "
                 "ubergraph / delegate signature), so remove_blueprint_function cannot delete it and "
                 "nothing was changed. For an interface override, author the signature on the INTERFACE "
                 "and re-create the override: add_function_parameter on the BPI_ -> compile_blueprint -> "
                 "add_blueprint_function with override=true."),
            *FunctionName));
    }

    // Only a confirmed delete dirties the asset.
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("removed_function"), FunctionName);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

namespace UEAgentBlueprintCommandsImpl
{
// One declared variable, for get_blueprint_variables: identity, tooltip and the property flags of its description
// (not of a compiled property). instance_editable and expose_on_spawn are the older names of is_editable and
// is_expose_on_spawn.
TSharedPtr<FJsonObject> DeclaredVariableRow(const FBPVariableDescription& Variable)
{
    TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
    Row->SetStringField(TEXT("name"), Variable.VarName.ToString());
    Row->SetStringField(TEXT("type"), Variable.VarType.PinCategory.ToString());
    Row->SetStringField(TEXT("category"), Variable.Category.ToString());
    // FriendlyName is a plain FString on the description, not an FText.
    Row->SetStringField(TEXT("tooltip"), Variable.FriendlyName);

    struct FFlagField
    {
        const TCHAR* Key;
        uint64 Mask;
    };
    const FFlagField FlagFields[] = {
        { TEXT("is_public"),              CPF_BlueprintVisible },
        { TEXT("is_editable"),            CPF_Edit },
        { TEXT("is_blueprint_read_only"), CPF_BlueprintReadOnly },
        { TEXT("is_replicated"),          CPF_Net },
        { TEXT("is_expose_on_spawn"),     CPF_ExposeOnSpawn },
        { TEXT("instance_editable"),      CPF_Edit },
        { TEXT("expose_on_spawn"),        CPF_ExposeOnSpawn },
    };
    for (const FFlagField& Field : FlagFields)
    {
        Row->SetBoolField(Field.Key, (Variable.PropertyFlags & Field.Mask) != 0);
    }
    return Row;
}

// The optional max_results of the Blueprint listings, 1..10000 (default 1000): the error reply, or null once OutCap
// holds it.
TSharedPtr<FJsonObject> ReadListingCap(const TSharedPtr<FJsonObject>& Params, int32& OutCap)
{
    OutCap = 1000;
    double Requested = 0.0;
    if (Params->TryGetNumberField(TEXT("max_results"), Requested))
    {
        OutCap = (int32)Requested;
        if (Requested != (double)OutCap || OutCap < 1 || OutCap > 10000)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Parameter 'max_results' must be a whole number from 1 to 10000; got %g"), Requested));
        }
    }
    return nullptr;
}

// The other Blueprint assets under /Game (Widget and Anim Blueprints included) that share a bare Name, when there is
// more than one: their object paths, else an empty list. A path-shaped Name is never ambiguous.
TArray<TSharedPtr<FJsonValue>> AmbiguousBlueprintMatches(const FString& Name)
{
    TArray<TSharedPtr<FJsonValue>> Matches;
    if (Name.IsEmpty() || Name.Contains(TEXT("/")))
    {
        return Matches;
    }
    FARFilter AnyBlueprintKind;
    AnyBlueprintKind.PackagePaths.Add(TEXT("/Game"));
    AnyBlueprintKind.bRecursivePaths = true;
    AnyBlueprintKind.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
    AnyBlueprintKind.bRecursiveClasses = true;
    TArray<FAssetData> Pool;
    mcp_common::AssetRegistry().GetAssets(AnyBlueprintKind, Pool);
    for (const FAssetData& Candidate : Pool)
    {
        if (Candidate.AssetName.ToString() == Name)
        {
            Matches.Add(MakeShared<FJsonValueString>(Candidate.GetObjectPathString()));
        }
    }
    if (Matches.Num() < 2)
    {
        Matches.Reset();
    }
    return Matches;
}

// { blueprint_path, blueprint_valid, ambiguous_matches? }: which asset a Blueprint listing actually read.
void AddListedBlueprintIdentity(const TSharedPtr<FJsonObject>& Reply, const FString& RequestedName, const UBlueprint* Blueprint)
{
    Reply->SetStringField(TEXT("blueprint_path"), Blueprint ? Blueprint->GetPathName() : FString());
    Reply->SetBoolField(TEXT("blueprint_valid"), IsValid(Blueprint));
    const TArray<TSharedPtr<FJsonValue>> Ambiguous = AmbiguousBlueprintMatches(RequestedName);
    if (Ambiguous.Num() > 0)
    {
        Reply->SetArrayField(TEXT("ambiguous_matches"), Ambiguous);
    }
}

// True when Graph overrides something: a function of the same name already on the parent chain, or one of an
// interface the Blueprint implements. OutDeclaringClass names where it was found.
bool FindOverriddenDeclaration(const UBlueprint* Blueprint, const UEdGraph* Graph, FString& OutDeclaringClass)
{
    const FName GraphName = Graph->GetFName();
    if (UClass* Parent = Blueprint->ParentClass)
    {
        if (const UFunction* Inherited = Parent->FindFunctionByName(GraphName))
        {
            const UClass* Owner = Inherited->GetOwnerClass();
            OutDeclaringClass = Owner ? Owner->GetPathName() : FString();
            return true;
        }
    }
    for (const FBPInterfaceDescription& Implemented : Blueprint->ImplementedInterfaces)
    {
        const UClass* InterfaceClass = Implemented.Interface.Get();
        if (InterfaceClass && InterfaceClass->FindFunctionByName(GraphName))
        {
            OutDeclaringClass = InterfaceClass->GetPathName();
            return true;
        }
    }
    return false;
}

// Purity is read off the graph's own entry node (FUNC_BlueprintPure), never inferred.
bool IsPureGraph(UEdGraph* Graph)
{
    TArray<UK2Node_FunctionEntry*> EntryNodes;
    Graph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
    if (EntryNodes.Num() == 0 || EntryNodes[0] == nullptr)
    {
        return false;
    }
    return (static_cast<uint32>(EntryNodes[0]->GetFunctionFlags()) & static_cast<uint32>(FUNC_BlueprintPure)) != 0;
}
}

// MCP:get_blueprint_functions - List the graphs of a Blueprint -- functions, interface implementation graphs, macros, delegate signatures and, on request, event graph pages -- each with its node count, a REAL is_override (resolved against the parent class chain AND every implemented interface, naming the declaring class) and is_pure (read off the graph's FunctionEntry FUNC_BlueprintPure flag, not guessed). Enumerates the FULL graph set (UBlueprint::GetAllGraphs), not just Blueprint->FunctionGraphs: an INTERFACE override graph lives in ImplementedInterfaces[].Graphs, which FunctionGraphs alone misses (a function that exists would read as "missing"). Each entry carries graph_type so the caller can tell a plain function from an interface override, a macro or a delegate signature. The map to read before get_graph_nodes, which needs an exact graph name. WARNING: graph_type 'event_graph' entries are the TRANSIENT compiled event graphs and are absent on a Blueprint not compiled this session -- the authored EventGraph is the 'ubergraph' page. get_class_functions has the compiled class surface (inherited and native functions included).
//   @param blueprint_name - Name of the blueprint, or its full path. A bare name that matches more than one Blueprint asset (a backup copy shares the original's name) resolves to one of them and lists the others in `ambiguous_matches` -- pass the full path then.
//   @param filter_type - Optional 'all' (default), 'functions' or 'events' (case-insensitive, singular accepted). 'functions' keeps function, interface, macro and delegate_signature graphs; 'events' keeps the event side (ubergraph pages and compiled event graphs) and lists it without include_ubergraphs.
//   @param include_ubergraphs - Optional bool (default false): also list the event side -- the authored EventGraph/ubergraph pages (graph_type='ubergraph') and the transient compiled event graphs (graph_type='event_graph'). Off by default so 'functions' stays function-shaped.
//   @param include_macros - Optional bool (default true): list MacroGraphs (graph_type='macro').
//   @param include_interface_graphs - Optional bool (default true): list ImplementedInterfaces[].Graphs (graph_type='interface').
//   @param include_subgraphs - Optional bool (default false): also list nested/collapsed sub-graphs reached by GetAllGraphs but owned by no top-level list (graph_type='subgraph')
//   @param max_results - Optional maximum number of entries to return, 1..10000 (default 1000); results_truncated says whether any were left out.
//   @returns { blueprint_name: string, blueprint_path: string, blueprint_valid: bool, ambiguous_matches?: [string], filter_type: string, functions: [{ name, node_count, graph_type, is_interface, interface_name?, interface_class?, is_user_constructed, is_pure?, is_override?, override_declaring_class? }], function_count: int, override_count: int, own_function_graph_count: int, interface_graph_count: int, macro_graph_count: int, delegate_graph_count: int, ubergraph_count: int, event_graph_count: int, subgraph_count: int, graphs_total: int, max_results: int, results_truncated: bool } -- graph_type is function|interface|macro|delegate_signature|ubergraph|event_graph|subgraph; is_pure/is_override are on function, interface and macro entries (a macro is neither), override_declaring_class names the class or interface (path) that declares what is overridden; is_user_constructed is just node_count>2, i.e. "someone wrote something beyond the auto Entry/Result terminators", because implementing an interface pre-creates empty graphs. The *_count totals count every graph of that kind, listed or not: subgraph_count>0 with include_subgraphs off is the warning that collapsed graphs exist and are not listed.

TSharedPtr<FJsonObject> HandleGetBlueprintFunctions(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = UEAgentBlueprintCommandsImpl::ResolveBlueprintParam(
        Params, &mcp_common::MissingParameterReply, BlueprintName, Error);
    if (!Blueprint)
    {
        return Error;
    }
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentBlueprintCommandsImpl::ReadListingCap(Params, Cap))
    {
        return Invalid;
    }
    // The description reads "all, functions, or events", which invites the singular, so the VALUE is normalized
    // before validation (trimmed, lower-cased, singular accepted).
    FString Scope = TEXT("all");
    Params->TryGetStringField(TEXT("filter_type"), Scope);
    Scope = Scope.TrimStartAndEnd().ToLower();
    if (Scope == TEXT("function"))
    {
        Scope = TEXT("functions");
    }
    else if (Scope == TEXT("event"))
    {
        Scope = TEXT("events");
    }
    if (Scope != TEXT("all") && Scope != TEXT("functions") && Scope != TEXT("events"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Parameter 'filter_type' must be 'all', 'functions', or 'events'; got '%s'"), *Scope));
    }
    // The extra listings are off by default so "functions" stays function-shaped.
    bool bIncludeUbergraphs = false;
    Params->TryGetBoolField(TEXT("include_ubergraphs"), bIncludeUbergraphs);
    bool bIncludeSubgraphs = false;
    Params->TryGetBoolField(TEXT("include_subgraphs"), bIncludeSubgraphs);
    bool bIncludeMacros = true;
    Params->TryGetBoolField(TEXT("include_macros"), bIncludeMacros);
    bool bIncludeInterfaceGraphs = true;
    Params->TryGetBoolField(TEXT("include_interface_graphs"), bIncludeInterfaceGraphs);
    const bool bFunctionSide = Scope != TEXT("events");
    const bool bEventSide = Scope == TEXT("events") || (Scope == TEXT("all") && bIncludeUbergraphs);

    // The graphs come from GetAllGraphs, the enumeration FindGraphByNameDeep looks names up in, so whatever is
    // findable by name is listable. A graph's kind is the Blueprint list that holds it; one in none of them is a
    // collapsed graph nested in another.
    TMap<const UEdGraph*, const UClass*> InterfaceOf;
    for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
    {
        for (const UEdGraph* Graph : Interface.Graphs)
        {
            if (Graph != nullptr)
            {
                InterfaceOf.Add(Graph, Interface.Interface.Get());
            }
        }
    }
    const auto KindOf = [Blueprint, &InterfaceOf](UEdGraph* Graph) -> const TCHAR*
    {
        if (InterfaceOf.Contains(Graph))                         { return TEXT("interface"); }
        if (Blueprint->FunctionGraphs.Contains(Graph))           { return TEXT("function"); }
        if (Blueprint->MacroGraphs.Contains(Graph))              { return TEXT("macro"); }
        if (Blueprint->DelegateSignatureGraphs.Contains(Graph))  { return TEXT("delegate_signature"); }
        if (Blueprint->UbergraphPages.Contains(Graph))           { return TEXT("ubergraph"); }
        return TEXT("subgraph");
    };
    const auto IsListed = [&](const FString& Kind) -> bool
    {
        if (Kind == TEXT("ubergraph") || Kind == TEXT("event_graph")) { return bEventSide; }
        if (Kind == TEXT("subgraph"))                                 { return bIncludeSubgraphs; }
        if (!bFunctionSide)                                           { return false; }
        if (Kind == TEXT("macro"))                                    { return bIncludeMacros; }
        if (Kind == TEXT("interface"))                                { return bIncludeInterfaceGraphs; }
        return true;
    };

    // The authored graphs, then the transient compiled event graphs, which GetAllGraphs does not reach.
    TArray<UEdGraph*> AllGraphs;
    Blueprint->GetAllGraphs(AllGraphs);
    TArray<TPair<UEdGraph*, FString>> Classified;
    for (UEdGraph* Graph : AllGraphs)
    {
        if (Graph != nullptr)
        {
            Classified.Emplace(Graph, KindOf(Graph));
        }
    }
    for (UEdGraph* Graph : Blueprint->EventGraphs)
    {
        if (Graph != nullptr)
        {
            Classified.Emplace(Graph, TEXT("event_graph"));
        }
    }

    TMap<FString, int32> CountOfKind;
    TArray<TSharedPtr<FJsonValue>> Listed;
    int32 OverrideCount = 0;
    bool bTruncated = false;
    for (const TPair<UEdGraph*, FString>& Item : Classified)
    {
        UEdGraph* Graph = Item.Key;
        const FString& Kind = Item.Value;
        ++CountOfKind.FindOrAdd(Kind);
        if (!IsListed(Kind))
        {
            continue;
        }
        if (Listed.Num() >= Cap)
        {
            bTruncated = true;
            continue;
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("graph_type"), Kind);
        const UClass* const* Interface = InterfaceOf.Find(Graph);
        Entry->SetBoolField(TEXT("is_interface"), Interface != nullptr);
        if (Interface != nullptr)
        {
            Entry->SetStringField(TEXT("interface_name"), *Interface ? (*Interface)->GetName() : FString(TEXT("<unresolved interface>")));
            Entry->SetStringField(TEXT("interface_class"), *Interface ? (*Interface)->GetPathName() : FString());
        }
        Entry->SetStringField(TEXT("name"), Graph->GetName());
        Entry->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
        // The graph of an interface function with a return value exists from the moment the interface is added, so
        // "the graph exists" is not "someone wrote logic": more than 2 nodes is something beyond the automatic entry
        // and result.
        Entry->SetBoolField(TEXT("is_user_constructed"), Graph->Nodes.Num() > 2);
        // Purity and overriding are a function's properties: an interface graph always overrides its interface, a
        // macro has no UFunction and is neither.
        if (Kind == TEXT("function"))
        {
            FString DeclaredIn;
            const bool bOverrides = UEAgentBlueprintCommandsImpl::FindOverriddenDeclaration(Blueprint, Graph, DeclaredIn);
            Entry->SetBoolField(TEXT("is_pure"), UEAgentBlueprintCommandsImpl::IsPureGraph(Graph));
            Entry->SetBoolField(TEXT("is_override"), bOverrides);
            if (bOverrides)
            {
                Entry->SetStringField(TEXT("override_declaring_class"), DeclaredIn);
                ++OverrideCount;
            }
        }
        else if (Kind == TEXT("interface"))
        {
            Entry->SetBoolField(TEXT("is_pure"), UEAgentBlueprintCommandsImpl::IsPureGraph(Graph));
            Entry->SetBoolField(TEXT("is_override"), true);
            Entry->SetStringField(TEXT("override_declaring_class"), *Interface ? (*Interface)->GetPathName() : FString());
            ++OverrideCount;
        }
        else if (Kind == TEXT("macro"))
        {
            Entry->SetBoolField(TEXT("is_pure"), false);
            Entry->SetBoolField(TEXT("is_override"), false);
        }
        Listed.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    UEAgentBlueprintCommandsImpl::AddListedBlueprintIdentity(Reply, BlueprintName, Blueprint);
    Reply->SetStringField(TEXT("filter_type"), Scope);
    Reply->SetArrayField(TEXT("functions"), Listed);
    Reply->SetNumberField(TEXT("function_count"), Listed.Num());
    Reply->SetNumberField(TEXT("override_count"), OverrideCount);
    // Own function graphs alone, kept for the callers that compare.
    Reply->SetNumberField(TEXT("own_function_graph_count"), CountOfKind.FindRef(TEXT("function")));
    Reply->SetNumberField(TEXT("interface_graph_count"), CountOfKind.FindRef(TEXT("interface")));
    Reply->SetNumberField(TEXT("macro_graph_count"), CountOfKind.FindRef(TEXT("macro")));
    Reply->SetNumberField(TEXT("delegate_graph_count"), CountOfKind.FindRef(TEXT("delegate_signature")));
    Reply->SetNumberField(TEXT("ubergraph_count"), CountOfKind.FindRef(TEXT("ubergraph")));
    Reply->SetNumberField(TEXT("event_graph_count"), CountOfKind.FindRef(TEXT("event_graph")));
    Reply->SetNumberField(TEXT("subgraph_count"), CountOfKind.FindRef(TEXT("subgraph")));
    Reply->SetNumberField(TEXT("graphs_total"), AllGraphs.Num());
    Reply->SetNumberField(TEXT("max_results"), Cap);
    Reply->SetBoolField(TEXT("results_truncated"), bTruncated);
    return Reply;
}

// MCP:get_blueprint_variables - List the variables this Blueprint DECLARES (its NewVariables) with name, pin category, editor category, tooltip and the flags that matter for co-op and UI work: is_public (BlueprintVisible), is_editable, is_blueprint_read_only, is_replicated (CPF_Net) and is_expose_on_spawn. Inherited variables, native C++ UPROPERTYs, function locals and components are NOT here -- get_class_properties on the generated class (..._C) has the full property surface. THE FLAGS ARE READ OFF THE STORED VARIABLE DESCRIPTION, not the compiled property, and two of them mislead: is_editable checks CPF_Edit and ignores CPF_DisableEditOnInstance, so it says true for variables the editor refuses to edit per instance; is_expose_on_spawn checks a property flag while set_blueprint_variable_metadata writes the ExposeOnSpawn METADATA, so it says false for variables that DO show up as SpawnActor pins. Oracles: write the placed instance for editability, the spawn node's pins for exposure, get_blueprint_variable_detailed_info for type/replication and one variable's default value.
//   @param blueprint_name - Name of the blueprint, or its full path. A bare name that matches more than one Blueprint asset resolves to one of them and lists the others in `ambiguous_matches`.
//   @param max_results - Optional maximum number of variables to return, 1..10000 (default 1000); results_truncated says whether any were left out.
//   @returns { blueprint_name: string, blueprint_path: string, blueprint_valid: bool, ambiguous_matches?: [string], variables: [{ name, type, category, tooltip, is_public, is_editable, is_blueprint_read_only, is_replicated, is_expose_on_spawn, instance_editable, expose_on_spawn }], variable_count: int, variables_count: int, max_results: int, results_truncated: bool } -- "type" is only the pin CATEGORY (e.g. "struct", "object"), not the struct or class; instance_editable and expose_on_spawn repeat is_editable and is_expose_on_spawn; variable_count and variables_count are the same number.

TSharedPtr<FJsonObject> HandleGetBlueprintVariables(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = UEAgentBlueprintCommandsImpl::ResolveBlueprintParam(
        Params, &mcp_common::MissingParameterReply, BlueprintName, Error);
    if (!Blueprint)
    {
        return Error;
    }
    int32 Cap = 0;
    if (TSharedPtr<FJsonObject> Invalid = UEAgentBlueprintCommandsImpl::ReadListingCap(Params, Cap))
    {
        return Invalid;
    }

    // The variables as the Blueprint declares them, so one added since the last compile is listed too.
    TArray<TSharedPtr<FJsonValue>> Declared;
    for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
    {
        if (Declared.Num() >= Cap)
        {
            break;
        }
        Declared.Add(MakeShared<FJsonValueObject>(UEAgentBlueprintCommandsImpl::DeclaredVariableRow(Variable)));
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    UEAgentBlueprintCommandsImpl::AddListedBlueprintIdentity(Reply, BlueprintName, Blueprint);
    Reply->SetArrayField(TEXT("variables"), Declared);
    Reply->SetNumberField(TEXT("variable_count"), Declared.Num());
    Reply->SetNumberField(TEXT("variables_count"), Declared.Num());
    Reply->SetNumberField(TEXT("max_results"), Cap);
    Reply->SetBoolField(TEXT("results_truncated"), Blueprint->NewVariables.Num() > Declared.Num());
    return Reply;
}

// MCP:remove_unused_variables - Delete EVERY variable the engine considers unused from a Blueprint in one shot (GatherUnusedVariables + RemoveUnusedVariables) and report which ones went. Destructive and non-selective -- "unused" means no graph node reads or writes it, so a variable only touched from C++, a DataTable, a save game or a runtime name lookup counts as unused and is deleted. Preview it by comparing get_blueprint_variables before/after, or delete a specific one with remove_blueprint_variable.
//   @param blueprint_name - Name of the blueprint
//   @returns { blueprint_name: string, removed_count: int, removed_variables?: [string] } -- removed_variables is the list gathered BEFORE the removal and removed_count what the engine reported removing; when they disagree, trust neither and re-read get_blueprint_variables.

TSharedPtr<FJsonObject> HandleRemoveUnusedVariables(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    TSharedPtr<FJsonObject> Error;
    UBlueprint* Blueprint = UEAgentBlueprintCommandsImpl::ResolveBlueprintParam(
        Params, &mcp_common::MissingParameterReply, BlueprintName, Error);
    if (!Blueprint)
    {
        return Error;
    }

    // The names are captured BEFORE the removal: afterwards the properties are gone.
    TArray<FProperty*> Unused;
    UBlueprintEditorLibrary::GatherUnusedVariables(Blueprint, Unused);
    TArray<TSharedPtr<FJsonValue>> UnusedNames;
    for (const FProperty* Candidate : Unused)
    {
        if (Candidate)
        {
            UnusedNames.Add(MakeShared<FJsonValueString>(Candidate->GetName()));
        }
    }

    // The count is whatever the engine reports having removed.
    const int32 EngineRemovedCount = UBlueprintEditorLibrary::RemoveUnusedVariables(Blueprint);
    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetNumberField(TEXT("removed_count"), EngineRemovedCount);
    if (UnusedNames.Num() > 0)
    {
        Reply->SetArrayField(TEXT("removed_variables"), UnusedNames);
    }
    return Reply;
}

// ---------------------------------------------------------------------------
// SCS (Simple Construction Script) component-hierarchy authoring helpers.
// File-local: shared by attach / reparent / remove-node handlers below.
// ---------------------------------------------------------------------------
namespace
{
    /**
     * Fully detach `Child` from its current location (parent's ChildNodes OR the SCS root set),
     * removing it from AllNodes too. Mirrors the engine's USubobjectDataSubsystem::DetachSubobject,
     * which calls SCS->RemoveNode(Child) (default args) so the single subsequent re-add restores
     * exactly one AllNodes entry. NOTE: AddNode / AddChildNode do NOT dedup AllNodes, so a detach
     * that leaves the node in AllNodes would duplicate it -- always detach via RemoveNode first.
     */
    void DetachFromCurrentLocation(USimpleConstructionScript* SCS, USCS_Node* Child)
    {
        // RemoveNode: if Child is a root it is removed from RootNodes + AllNodes; otherwise it is
        // removed from its parent's ChildNodes + AllNodes (RemoveNode delegates to RemoveChildNode
        // with the default bRemoveFromAllNodes=true). Defer scene-root validation until after re-attach.
        SCS->RemoveNode(Child, /*bValidateSceneRootNodes=*/false);
    }

    /**
     * Parent `Child` under the SCS `NewParent` node via AddChildNode (the canonical in-tree linkage,
     * matching SubobjectDataSubsystem), setting the attach socket. Detaches `Child` from its previous
     * location first so state stays disjoint. Assumes a non-null NewParent that is NOT `Child` and not
     * a descendant of `Child`.
     */
    void MakeChildOfSCSNode(USimpleConstructionScript* SCS, USCS_Node* Child, USCS_Node* NewParent, FName SocketName)
    {
        DetachFromCurrentLocation(SCS, Child);   // removes from old parent/root AND AllNodes
        NewParent->AddChildNode(Child);          // in-tree linkage via ChildNodes (the canonical reparent)
        // ParentComponentOrVariableName encodes attachment to an INHERITED/native parent OUTSIDE this
        // SCS; for an in-tree child it MUST be None or the compile-time FixupParentage walk ensure()s
        // ("possible cyclic linkage"). NOT Child->SetParent(NewParent): that sets it to the tree parent
        // and trips the ensure. Clear any stale inherited linkage carried over from a prior location.
        Child->ParentComponentOrVariableName = NAME_None;
        Child->ParentComponentOwnerClassName = NAME_None;
        Child->bIsParentComponentNative = false;
        Child->AttachToName = SocketName;        // NAME_None clears the socket
    }

    /**
     * Resolve a NATIVE inherited USceneComponent TEMPLATE by name (e.g. ACharacter's "CharacterMesh0")
     * from the Blueprint's CDO -- for a component that lives in a parent C++ class, so it is neither in
     * this BP's SCS nor the scene root. The CDO's component set already includes every inherited native
     * subobject, so a single sweep suffices; the parent-class CDO is tried first (it declares the
     * component), the generated-class CDO as a fallback. Matches GetFName() then GetName(). This mirrors
     * how set_blueprint_component_properties resolves its "inherited-native" template. Returns nullptr if
     * no native scene component of that name exists. Only the resolved component's FName is used by the
     * caller (USCS_Node::SetParent copies the name + flags bIsParentComponentNative), so either CDO works.
     */
    USceneComponent* FindNativeSceneComponentByName(UBlueprint* BP, const FString& CompName)
    {
        if (!BP)
        {
            return nullptr;
        }
        const FName CompFName(*CompName);
        TArray<AActor*> CDOs;
        if (BP->ParentClass)
        {
            if (AActor* A = Cast<AActor>(BP->ParentClass->GetDefaultObject())) { CDOs.Add(A); }
        }
        if (BP->GeneratedClass)
        {
            if (AActor* A = Cast<AActor>(BP->GeneratedClass->GetDefaultObject())) { CDOs.AddUnique(A); }
        }
        for (AActor* CDO : CDOs)
        {
            TInlineComponentArray<USceneComponent*> Comps;
            CDO->GetComponents(Comps);
            for (USceneComponent* C : Comps)
            {
                if (C && (C->GetFName() == CompFName || C->GetName() == CompName))
                {
                    return C;
                }
            }
        }
        return nullptr;
    }
}

// MCP:attach_blueprint_component - Re-parents a component already in a Blueprint's Components tree (an SCS component), at an optional socket or bone: under another SCS component, under a native scene component inherited from a C++ parent (e.g. ACharacter's "CharacterMesh0", so its sockets are reachable), or under the scene root when parent_component_name is omitted or "root". The Blueprint is then compiled and left unsaved (save_asset writes it); the call is one undo step that restores the previous parent and socket. Idempotent: a component already under that parent with the same socket returns already_attached true and nothing is recompiled. Refused with nothing changed when the Blueprint, the component or the parent is not found, or when the parent is the component itself or one of its descendants. Refused while Play-in-Editor runs unless force=true. To add a new component use add_component_to_blueprint (inverse: remove_component_from_blueprint); read the tree with get_blueprint_components; for actors placed in a level use attach_actor.
//   @param blueprint_name - Blueprint asset path (e.g. "/Game/Characters/BP_Hero") or bare asset name, looked up under /Game/Blueprints first and then anywhere under /Game.
//   @param component_name - Variable name of the SCS component to move (the child), as get_blueprint_components lists it; inherited native components cannot be moved.
//   @param parent_component_name - Optional parent component name, as get_blueprint_components lists it; omit / "root" / the native root name attaches to the scene root. May also name a NATIVE inherited scene component (e.g. "CharacterMesh0") to parent the child natively to it.
//   @param socket_name - Optional socket or bone name on the parent (AttachToName), e.g. "hand_r"; not checked against the parent's mesh, and omitting it clears any socket.
//   @returns { blueprint_name, component_name, parent, parent_is_native, attach_socket, already_attached }

TSharedPtr<FJsonObject> HandleAttachBlueprintComponent(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    if (TSharedPtr<FJsonObject> Missing = UEAgentBlueprintCommandsImpl::ReadBlueprintComponentNames(
            Params, &mcp_common::MissingParameterReply, BlueprintName, ComponentName))
    {
        return Missing;
    }

    FString ParentComponentName;
    Params->TryGetStringField(TEXT("parent_component_name"), ParentComponentName);

    FString SocketName;
    Params->TryGetStringField(TEXT("socket_name"), SocketName);
    const FName SocketFName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }
    USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
    if (!SCS)
    {
        return mcp_common::MakeErrorReply(TEXT("Blueprint has no SimpleConstructionScript"));
    }

    USCS_Node* ChildNode = SCS->FindSCSNode(*ComponentName);
    if (!ChildNode)
    {
        return UEAgentBlueprintCommandsImpl::ComponentNotFound(ComponentName);
    }

    // Build a helper to emit a consistent success payload from the child's current linkage.
    auto MakeLinkagePayload = [&](bool bAlreadyAttached) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("blueprint_name"), BlueprintName);
        Obj->SetStringField(TEXT("component_name"), ComponentName);
        // In-tree parent comes from the ChildNodes tree (FindParentNode); the name field is only the
        // fallback for a node natively-parented outside this SCS (see MakeChildOfSCSNode).
        USCS_Node* InTreeParent = SCS->FindParentNode(ChildNode);
        Obj->SetStringField(TEXT("parent"), InTreeParent
            ? InTreeParent->GetVariableName().ToString()
            : ChildNode->ParentComponentOrVariableName.ToString());
        Obj->SetBoolField(TEXT("parent_is_native"), ChildNode->bIsParentComponentNative);
        Obj->SetStringField(TEXT("attach_socket"), ChildNode->AttachToName.ToString());
        Obj->SetBoolField(TEXT("already_attached"), bAlreadyAttached);
        return Obj;
    };

    // Records the construction script and the child node for undo before the first change: AttachToName and the
    // parent fields live on the node, and a re-parent that only changes those is not recorded by the SCS calls.
    auto RecordForUndo = [&]()
    {
        SCS->SetFlags(RF_Transactional);
        SCS->Modify();
        ChildNode->SetFlags(RF_Transactional);
        ChildNode->Modify();
    };

    // Keeps the child a ROOT SCS node (native-parented nodes live in RootNodes). It is restructured only when it
    // is currently a child of another node (the detach removes it from AllNodes; AddNode re-adds it once); a node
    // that is already a root leaves the root set untouched.
    auto KeepAsRootNode = [&]()
    {
        if (SCS->FindParentNode(ChildNode) != nullptr)
        {
            DetachFromCurrentLocation(SCS, ChildNode);
            SCS->AddNode(ChildNode);
        }
    };

    // Marks the structural change, compiles, and reports the new linkage.
    auto CommitLinkage = [&]()
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
        FKismetEditorUtilities::CompileBlueprint(Blueprint);
        return MakeLinkagePayload(/*bAlreadyAttached=*/false);
    };

    // Resolve the scene root once: GetSceneRootComponentTemplate(false, &OutNode) returns the scene
    // root template; OutNode is non-null when that root is an SCS node in THIS BP, otherwise a non-null
    // return is a NATIVE root from the parent-class CDO.
    USCS_Node* SceneRootNode = nullptr;
    USceneComponent* NativeRoot = SCS->GetSceneRootComponentTemplate(/*bShouldUseDefaultRoot=*/false, &SceneRootNode);

    // Attach-to-root when no parent given, "root", or the name resolves to the native root component.
    const bool bNameIsNativeRoot = !ParentComponentName.IsEmpty() && NativeRoot
        && NativeRoot->GetFName() == FName(*ParentComponentName);
    const bool bAttachToRoot = ParentComponentName.IsEmpty()
        || ParentComponentName.Equals(TEXT("root"), ESearchCase::IgnoreCase)
        || bNameIsNativeRoot;

    // ---- Attach under an explicit SCS-node parent -------------------------
    if (!bAttachToRoot)
    {
        USCS_Node* ParentNode = SCS->FindSCSNode(*ParentComponentName);
        if (!ParentNode)
        {
            // Fallback: the named parent may be a NATIVE inherited USceneComponent (e.g. ACharacter's
            // CharacterMesh0 at socket "spine_4", or a native scene component like the capsule) -- one
            // that is neither in this BP's SCS nor the scene root, so FindSCSNode misses it. Resolve it
            // from the parent-class CDO and parent the child to it natively (same mechanism as the
            // NativeRoot branch below). A ChildActor attached at a socket (e.g. "spine_2") benefits too.
            if (USceneComponent* NativeParent = FindNativeSceneComponentByName(Blueprint, ParentComponentName))
            {
                // Idempotency: already natively parented to this component with the same socket.
                if (ChildNode->bIsParentComponentNative
                    && ChildNode->ParentComponentOrVariableName == NativeParent->GetFName()
                    && SCS->FindParentNode(ChildNode) == nullptr
                    && ChildNode->AttachToName == SocketFName)
                {
                    return MakeLinkagePayload(/*bAlreadyAttached=*/true);
                }
                RecordForUndo();
                KeepAsRootNode();
                ChildNode->SetParent(NativeParent); // const USceneComponent* overload: sets bIsParentComponentNative=true
                ChildNode->AttachToName = SocketFName;
                return CommitLinkage();
            }
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName));
        }

        if (ParentNode == ChildNode)
        {
            return mcp_common::MakeErrorReply(TEXT("A component cannot be parented to itself"));
        }
        // Cycle guard: parent must not live inside the child's own subtree.
        if (ParentNode->IsChildOf(ChildNode))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Cannot attach '%s' under its own descendant '%s' (would create a cycle)"),
                *ComponentName, *ParentComponentName));
        }

        // Idempotency: already a child of this parent with the same socket.
        if (ParentNode->GetChildNodes().Contains(ChildNode) && ChildNode->AttachToName == SocketFName)
        {
            return MakeLinkagePayload(/*bAlreadyAttached=*/true);
        }

        RecordForUndo();
        MakeChildOfSCSNode(SCS, ChildNode, ParentNode, SocketFName);
        return CommitLinkage();
    }

    // ---- Attach to root ---------------------------------------------------
    // SceneRootNode / NativeRoot were resolved above. If the scene root is an SCS node in this BP,
    // attach under it; else if there is a native root, link natively; else the child becomes the root.
    if (SceneRootNode && SceneRootNode != ChildNode)
    {
        if (SceneRootNode->GetChildNodes().Contains(ChildNode) && ChildNode->AttachToName == SocketFName)
        {
            return MakeLinkagePayload(/*bAlreadyAttached=*/true);
        }
        RecordForUndo();
        MakeChildOfSCSNode(SCS, ChildNode, SceneRootNode, SocketFName);
        return CommitLinkage();
    }

    if (NativeRoot)
    {
        // Idempotency: already natively parented to the root with the same socket.
        if (ChildNode->bIsParentComponentNative && SCS->FindParentNode(ChildNode) == nullptr
            && ChildNode->AttachToName == SocketFName)
        {
            return MakeLinkagePayload(/*bAlreadyAttached=*/true);
        }
        RecordForUndo();
        KeepAsRootNode();
        ChildNode->SetParent(NativeRoot); // const USceneComponent* overload: sets bIsParentComponentNative=true
        ChildNode->AttachToName = SocketFName;
        return CommitLinkage();
    }

    // No scene root exists (or the child itself is the scene root): the child stays/becomes the root.
    const bool bAlreadyRoot = (SCS->FindParentNode(ChildNode) == nullptr)
        && !ChildNode->bIsParentComponentNative
        && ChildNode->ParentComponentOrVariableName.IsNone()
        && ChildNode->AttachToName == SocketFName;
    if (bAlreadyRoot)
    {
        return MakeLinkagePayload(/*bAlreadyAttached=*/true);
    }

    RecordForUndo();
    KeepAsRootNode();
    ChildNode->ParentComponentOrVariableName = NAME_None;
    ChildNode->ParentComponentOwnerClassName = NAME_None;
    ChildNode->bIsParentComponentNative = false;
    ChildNode->AttachToName = SocketFName;
    return CommitLinkage();
}

// =====================================================================================
// AI Perception sense authoring
// =====================================================================================
namespace
{
    // Locate the UAIPerceptionComponent SCS template named ComponentName on Blueprint.
    // Returns nullptr and fills OutError honestly if the node is missing or the wrong type.
    UAIPerceptionComponent* FindPerceptionTemplate(UBlueprint* Blueprint, const FString& ComponentName, FString& OutError)
    {
        // A Blueprint without a construction script simply has no such component.
        USimpleConstructionScript* OwnScs = Blueprint->SimpleConstructionScript;
        USCS_Node* PerceptionNode = OwnScs ? UEAgentBlueprintCommandsImpl::FindOwnScsNode(OwnScs, ComponentName) : nullptr;
        if (!PerceptionNode)
        {
            OutError = FString::Printf(TEXT("Component not found: %s"), *ComponentName);
            return nullptr;
        }

        UAIPerceptionComponent* Perc = Cast<UAIPerceptionComponent>(PerceptionNode->ComponentTemplate);
        if (!Perc)
        {
            OutError = FString::Printf(TEXT("Component '%s' is not an AIPerceptionComponent"), *ComponentName);
            return nullptr;
        }
        return Perc;
    }

    // Serialize a single configured sense's public fields into a JSON object for read-back.
    TSharedPtr<FJsonObject> SenseConfigToJson(const UAISenseConfig* Config)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        if (!Config)
        {
            return Obj;
        }
        Obj->SetStringField(TEXT("config_class"), Config->GetClass()->GetName());

        if (const UAISenseConfig_Sight* Sight = Cast<UAISenseConfig_Sight>(Config))
        {
            Obj->SetStringField(TEXT("sense_type"), TEXT("sight"));
            Obj->SetNumberField(TEXT("sight_radius"), Sight->SightRadius);
            Obj->SetNumberField(TEXT("lose_sight_radius"), Sight->LoseSightRadius);
            Obj->SetNumberField(TEXT("peripheral_vision_angle"), Sight->PeripheralVisionAngleDegrees);
            Obj->SetBoolField(TEXT("detect_enemies"), Sight->DetectionByAffiliation.bDetectEnemies != 0);
            Obj->SetBoolField(TEXT("detect_neutrals"), Sight->DetectionByAffiliation.bDetectNeutrals != 0);
            Obj->SetBoolField(TEXT("detect_friendlies"), Sight->DetectionByAffiliation.bDetectFriendlies != 0);
        }
        else if (const UAISenseConfig_Hearing* Hearing = Cast<UAISenseConfig_Hearing>(Config))
        {
            Obj->SetStringField(TEXT("sense_type"), TEXT("hearing"));
            Obj->SetNumberField(TEXT("hearing_range"), Hearing->HearingRange);
            Obj->SetBoolField(TEXT("detect_enemies"), Hearing->DetectionByAffiliation.bDetectEnemies != 0);
            Obj->SetBoolField(TEXT("detect_neutrals"), Hearing->DetectionByAffiliation.bDetectNeutrals != 0);
            Obj->SetBoolField(TEXT("detect_friendlies"), Hearing->DetectionByAffiliation.bDetectFriendlies != 0);
        }
        else
        {
            Obj->SetStringField(TEXT("sense_type"), TEXT("unknown"));
        }
        return Obj;
    }

    // Persist a Blueprint asset to disk so a cov reload round-trips the edited template.
    bool SavePerceptionBlueprint(UBlueprint* Blueprint, FString& OutError)
    {
        if (!Blueprint)
        {
            OutError = TEXT("Invalid blueprint for save");
            return false;
        }
        return mcp_common::SaveAssetPackage(Blueprint->GetOutermost(), Blueprint, OutError);
    }
}

// Senses are authored on the AIPerceptionComponent SCS template: SensesConfig is EditDefaultsOnly +
// Instanced, so a config created there as a subobject persists with the asset.

// MCP:add_ai_perception_sense - Configure a sight or hearing sense on an AIPerceptionComponent TEMPLATE of a Blueprint, creating the config as an Instanced subobject of the template so it persists with the asset, and saving the package. Idempotent: ConfigureSense dedups by config class, so re-calling with the same sense_type UPDATES the existing config in place instead of stacking a second one. Only "sight" and "hearing" are supported (no damage, touch or team senses). It authors the TEMPLATE only -- whether the AI actually perceives anything is a live-PIE question this cannot answer; read the authored result back with get_ai_perception_senses. Inverse: remove_ai_perception_sense.
//   @param blueprint_name - Name of the blueprint owning the component
//   @param component_name - Name of the AIPerceptionComponent SCS node
//   @param sense_type - "sight" or "hearing"
//   @param dominant - Optional bool; if true, sets this sense as the component's dominant sense
//   @param sight_radius - (sight) Maximum sight distance, cm (ClampMin 0)
//   @param lose_sight_radius - (sight) Distance to keep seeing an already-seen target, cm (ClampMin 0)
//   @param peripheral_vision_angle - (sight) Peripheral vision half-angle, degrees (Clamp 0..180)
//   @param hearing_range - (hearing) Hearing range, cm (ClampMin 0)
//   @param detect_enemies - Optional bool affiliation filter
//   @param detect_neutrals - Optional bool affiliation filter
//   @param detect_friendlies - Optional bool affiliation filter
//   @returns { blueprint_name, component_name, sense_type, config_class, <sense fields>, note }
//   @note Runtime perception detection is PIE-only and out of scope; this only authors the template config.

TSharedPtr<FJsonObject> HandleAddAIPerceptionSense(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, SenseType;
    if (TSharedPtr<FJsonObject> Missing = UEAgentBlueprintCommandsImpl::ReadBlueprintComponentNames(
            Params, &UEAgentBlueprintCommandsImpl::MissingQuotedParam, BlueprintName, ComponentName))
    {
        return Missing;
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sense_type"), SenseType, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    SenseType = SenseType.ToLower();

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    FString FindErr;
    UAIPerceptionComponent* Perc = FindPerceptionTemplate(Blueprint, ComponentName, FindErr);
    if (!Perc)
    {
        return mcp_common::MakeErrorReply(FindErr);
    }

    // Affiliation filter helper (applied to whichever config we author).
    auto ApplyAffiliation = [&Params](FAISenseAffiliationFilter& Filter)
    {
        bool bVal = false;
        if (Params->TryGetBoolField(TEXT("detect_enemies"), bVal))    { Filter.bDetectEnemies = bVal; }
        if (Params->TryGetBoolField(TEXT("detect_neutrals"), bVal))   { Filter.bDetectNeutrals = bVal; }
        if (Params->TryGetBoolField(TEXT("detect_friendlies"), bVal)) { Filter.bDetectFriendlies = bVal; }
    };

    UAISenseConfig* AuthoredConfig = nullptr;

    if (SenseType == TEXT("sight"))
    {
        // Instanced subobject of the template (Outer = Perc) so it serializes/persists.
        UAISenseConfig_Sight* Cfg = NewObject<UAISenseConfig_Sight>(Perc);

        double NumVal = 0.0;
        if (Params->TryGetNumberField(TEXT("sight_radius"), NumVal))
        {
            Cfg->SightRadius = FMath::Max(0.0f, static_cast<float>(NumVal)); // ClampMin 0
        }
        if (Params->TryGetNumberField(TEXT("lose_sight_radius"), NumVal))
        {
            Cfg->LoseSightRadius = FMath::Max(0.0f, static_cast<float>(NumVal)); // ClampMin 0
        }
        if (Params->TryGetNumberField(TEXT("peripheral_vision_angle"), NumVal))
        {
            Cfg->PeripheralVisionAngleDegrees = FMath::Clamp(static_cast<float>(NumVal), 0.0f, 180.0f); // Clamp 0..180
        }
        ApplyAffiliation(Cfg->DetectionByAffiliation);

        AuthoredConfig = Cfg;
    }
    else if (SenseType == TEXT("hearing"))
    {
        UAISenseConfig_Hearing* Cfg = NewObject<UAISenseConfig_Hearing>(Perc);

        double NumVal = 0.0;
        if (Params->TryGetNumberField(TEXT("hearing_range"), NumVal))
        {
            Cfg->HearingRange = FMath::Max(0.0f, static_cast<float>(NumVal)); // ClampMin 0
        }
        ApplyAffiliation(Cfg->DetectionByAffiliation);

        AuthoredConfig = Cfg;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown sense_type: '%s' (expected 'sight' or 'hearing')"), *SenseType));
    }

    // Auto-dedups by config class -> idempotent re-add updates in place.
    Perc->ConfigureSense(*AuthoredConfig);

    // Optional dominant sense.
    bool bDominant = false;
    if (Params->TryGetBoolField(TEXT("dominant"), bDominant) && bDominant)
    {
        Perc->SetDominantSense(AuthoredConfig->GetSenseImplementation());
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    FString SaveErr;
    if (!SavePerceptionBlueprint(Blueprint, SaveErr))
    {
        return mcp_common::MakeErrorReply(SaveErr);
    }

    UE_LOG(LogTemp, Verbose, TEXT("add_ai_perception_sense: configured '%s' sense on %s.%s"),
           *SenseType, *BlueprintName, *ComponentName);

    // Read back the configured sense from the template for an honest round-trip.
    TSharedPtr<FJsonObject> ResultObj = SenseConfigToJson(AuthoredConfig);
    ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
    ResultObj->SetStringField(TEXT("component_name"), ComponentName);
    ResultObj->SetBoolField(TEXT("dominant"), bDominant);
    ResultObj->SetStringField(TEXT("note"),
        TEXT("Authored the perception sense config on the SCS component template; runtime detection is PIE-only and out of scope."));
    return ResultObj;
}

// MCP:get_ai_perception_senses - Read back the senses configured on an AIPerceptionComponent TEMPLATE of a Blueprint: for each one its config class plus the fields that matter (sight radius / lose-sight radius / peripheral angle, or hearing range) and the three affiliation filters. This is the verification step for add_ai_perception_sense. It reads the OWN SCS template, so an AIPerception component inherited from a parent Blueprint is not found, and a config class other than Sight or Hearing comes back with sense_type "unknown" and no fields.
//   @param blueprint_name - Name of the blueprint owning the component
//   @param component_name - Name of an OWN SCS AIPerceptionComponent node; another component type is an explicit error
//   @returns { blueprint_name, component_name, senses: [ { config_class, sense_type, sight_radius|hearing_range, ... , detect_enemies, detect_neutrals, detect_friendlies } ], sense_count } -- sense_count 0 means the template has no configured sense, which is also what an AI that never perceives anything looks like.

TSharedPtr<FJsonObject> HandleGetAIPerceptionSenses(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    if (TSharedPtr<FJsonObject> Missing = UEAgentBlueprintCommandsImpl::ReadBlueprintComponentNames(
            Params, &UEAgentBlueprintCommandsImpl::MissingQuotedParam, BlueprintName, ComponentName))
    {
        return Missing;
    }

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    FString FindErr;
    UAIPerceptionComponent* Perc = FindPerceptionTemplate(Blueprint, ComponentName, FindErr);
    if (!Perc)
    {
        return mcp_common::MakeErrorReply(FindErr);
    }

    TArray<TSharedPtr<FJsonValue>> Senses;
    for (UAIPerceptionComponent::TAISenseConfigConstIterator It = Perc->GetSensesConfigIterator(); It; ++It)
    {
        const UAISenseConfig* Config = *It;
        if (Config)
        {
            Senses.Add(MakeShared<FJsonValueObject>(SenseConfigToJson(Config)));
        }
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("component_name"), ComponentName);
    Reply->SetArrayField(TEXT("senses"), Senses);
    Reply->SetNumberField(TEXT("sense_count"), Senses.Num());
    return Reply;
}

// MCP:remove_ai_perception_sense - Remove a sense (its config) from an AIPerceptionComponent TEMPLATE of a Blueprint, then compile the Blueprint and save the package, as add_ai_perception_sense does; no undo step. Inverse of add_ai_perception_sense: sense_type "sight" or "hearing", or any other configured sense by its config class name (AISenseConfig_Damage...). When it was the dominant sense, the dominant sense is cleared (dominant_cleared). A sense the template does not configure is an honest no-op (removed=false, nothing compiled or saved); a missing Blueprint or component, or a component that is not an AIPerceptionComponent, is an error. The reply re-reads the template's senses; read them with get_ai_perception_senses. Refused during PIE unless force=true.
//   @param blueprint_name - Name of the blueprint owning the component
//   @param component_name - Name of the AIPerceptionComponent SCS node
//   @param sense_type - "sight", "hearing", or the short name / class name of another configured sense ("damage", "AISenseConfig_Damage")
//   @returns { blueprint_name, component_name, sense_type, removed, removed_count, dominant_cleared, senses: [string], sense_count, saved, note? }

TSharedPtr<FJsonObject> HandleRemoveAIPerceptionSense(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, SenseType;
    if (TSharedPtr<FJsonObject> Missing = UEAgentBlueprintCommandsImpl::ReadBlueprintComponentNames(
            Params, &UEAgentBlueprintCommandsImpl::MissingQuotedParam, BlueprintName, ComponentName))
    {
        return Missing;
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sense_type"), SenseType)) { return Missing; }

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }
    FString FindErr;
    UAIPerceptionComponent* Perc = FindPerceptionTemplate(Blueprint, ComponentName, FindErr);
    if (!Perc)
    {
        return mcp_common::MakeErrorReply(FindErr);
    }

    // SensesConfig is protected and has no remover: reach it as the property it is (an Instanced UPROPERTY).
    FArrayProperty* SensesProperty = FindFProperty<FArrayProperty>(UAIPerceptionComponent::StaticClass(), TEXT("SensesConfig"));
    if (!SensesProperty || !CastField<FObjectPropertyBase>(SensesProperty->Inner))
    {
        return mcp_common::MakeErrorReply(TEXT("UAIPerceptionComponent has no SensesConfig object array in this engine; nothing was changed"));
    }
    const FString Wanted = SenseType.StartsWith(TEXT("AISenseConfig_"), ESearchCase::IgnoreCase) ? SenseType : TEXT("AISenseConfig_") + SenseType;
    const auto Matches = [&Wanted](const UObject* Config)
    {
        return Config && Config->GetClass()->GetName().Equals(Wanted, ESearchCase::IgnoreCase);
    };
    const auto SenseNames = [Perc]()
    {
        TArray<FString> Names;
        for (UAIPerceptionComponent::TAISenseConfigConstIterator It = Perc->GetSensesConfigIterator(); It; ++It)
        {
            if (*It)
            {
                Names.Add((*It)->GetClass()->GetName());
            }
        }
        return Names;
    };

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Reply->SetStringField(TEXT("component_name"), ComponentName);
    Reply->SetStringField(TEXT("sense_type"), SenseType);

    FScriptArrayHelper Senses(SensesProperty, SensesProperty->ContainerPtrToValuePtr<void>(Perc));
    const FObjectPropertyBase* Inner = CastField<FObjectPropertyBase>(SensesProperty->Inner);
    TArray<int32> ToRemove;
    TSubclassOf<UAISense> RemovedImplementation;
    for (int32 Index = 0; Index < Senses.Num(); ++Index)
    {
        const UAISenseConfig* Config = Cast<UAISenseConfig>(Inner->GetObjectPropertyValue(Senses.GetRawPtr(Index)));
        if (Matches(Config))
        {
            ToRemove.Add(Index);
            RemovedImplementation = Config->GetSenseImplementation();
        }
    }
    if (ToRemove.Num() == 0)
    {
        const TArray<FString> Names = SenseNames();
        Reply->SetBoolField(TEXT("removed"), false);
        Reply->SetNumberField(TEXT("removed_count"), 0);
        Reply->SetBoolField(TEXT("dominant_cleared"), false);
        Reply->SetArrayField(TEXT("senses"), mcp_common::StringsToJson(Names));
        Reply->SetNumberField(TEXT("sense_count"), Names.Num());
        Reply->SetBoolField(TEXT("saved"), false);
        Reply->SetStringField(TEXT("note"), FString::Printf(TEXT("the template configures no %s; nothing changed"), *Wanted));
        return Reply;
    }

    Perc->Modify();
    for (int32 Index = ToRemove.Num() - 1; Index >= 0; --Index)
    {
        Senses.RemoveValues(ToRemove[Index], 1);
    }
    const bool bDominantCleared = RemovedImplementation && Perc->GetDominantSense() == RemovedImplementation;
    if (bDominantCleared)
    {
        Perc->SetDominantSense(nullptr);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);

    // The re-read, on the template the Blueprint holds after the compile.
    UAIPerceptionComponent* After = FindPerceptionTemplate(Blueprint, ComponentName, FindErr);
    TArray<FString> Left;
    if (After)
    {
        for (UAIPerceptionComponent::TAISenseConfigConstIterator It = After->GetSensesConfigIterator(); It; ++It)
        {
            if (*It)
            {
                Left.Add((*It)->GetClass()->GetName());
            }
        }
    }
    if (!After || Left.ContainsByPredicate([&Wanted](const FString& Name) { return Name.Equals(Wanted, ESearchCase::IgnoreCase); }))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s is still configured on the template after the removal; nothing was saved"), *Wanted));
    }

    FString SaveErr;
    const bool bSaved = SavePerceptionBlueprint(Blueprint, SaveErr);
    Reply->SetBoolField(TEXT("removed"), true);
    Reply->SetNumberField(TEXT("removed_count"), ToRemove.Num());
    Reply->SetBoolField(TEXT("dominant_cleared"), bDominantCleared);
    Reply->SetArrayField(TEXT("senses"), mcp_common::StringsToJson(Left));
    Reply->SetNumberField(TEXT("sense_count"), Left.Num());
    return mcp_common::MakeSavedReply(Reply, bSaved, TEXT("remove_ai_perception_sense"), SaveErr);
}

// ===========================================================================
// Blueprint variable advanced + function override discovery
// ===========================================================================

// MCP:get_blueprint_variable_detailed_info - Read ONE variable's full detail: its default value, the structured type (category, sub-category object and container kind -- not just the pin category get_blueprint_variables gives you), and every checkbox of the variable Details panel -- is_instance_editable / is_expose_on_spawn / is_blueprint_read_only / is_expose_to_cinematics / is_replicated / replication_condition / save_game. This is the headless oracle for "does this variable really persist into a .sav?", which otherwise costs a co-op PIE session plus a binary grep of the slot file. save_game is reported from BOTH places the flag lives, because they disagree until you compile: `save_game` is CPF_SaveGame as AUTHORED (FBPVariableDescription::PropertyFlags, exactly what the editor checkbox writes) while `save_game_generated` is the same flag on the FProperty of the GENERATED class, which is the one the save archive consults -- so save_game:true + save_game_generated:false means MARKED BUT NOT COMPILED, and nothing will persist yet; `save_game_matches` says it in one field. Read is_replicated FIRST: replication_condition is always populated (COND_None by default) and only means anything when replication is on. `default_value` is read from the CDO when the class is compiled (`default_value_source:"cdo"`), so it does not come back EMPTY for arrays, maps and every variable of an already-compiled Blueprint. Booleans of this family are contrasted, not believed -- audit_replication is the ground truth for replication.
//   @param blueprint_name - Name of the blueprint
//   @param variable_name - Name of a variable DECLARED on this Blueprint (e.g. "MyFloat"); inherited variables are not covered
//   @returns { blueprint_name: string, variable_name: string, default_value: string, default_value_source: "cdo"|"blueprint_description", default_value_serialized: string, var_type: string, property_type: string, var_type_detail: {category, sub_category, sub_category_object, sub_category_object_path, container}, is_instance_editable: bool, is_expose_on_spawn: bool, is_blueprint_read_only: bool, is_expose_to_cinematics: bool, is_replicated: bool, replication_condition: string, save_game: bool, save_game_generated: bool, save_game_generated_found: bool, save_game_matches: bool }
//   WARNING: the returns line above is ONE line ON PURPOSE, and must stay that way: the catalog codegen
//     matches only the marker line (ANNOTATION_RETURNS_RE in codegen_command_catalog.py), so a wrapped
//     returns line reaches callers cut after its first line, every flag past it included. Do not
//     re-wrap it, and do not start a note line with the returns keyword either: that same regex would
//     swallow the note as if it were the shape.
//   WARNING: default_value_serialized is FBPVariableDescription::DefaultValue verbatim: a ONE-SHOT MAILBOX the
//     Full compile consumes and clears (KismetCompiler.cpp). Empty there is NORMAL after a
//     compile and does NOT mean the default was lost -- compare default_value, which comes from the CDO.
//   NOTE on `property_type`: FBPVariableDescription has no PropertyType, so this is a readable
//     descriptor DERIVED from VarType (FEdGraphPinType), in the same "Category:SubObject[]" shape
//     diff_blueprints already emits (Analysis/UEAgentAnalysisCommands.cpp) so the two are
//     comparable. `var_type_detail` is the structured form of the same thing, keyed like
//     FUEAgentGraphUtils::PinToJson (Common/UEAgentGraphUtils.cpp). `var_type` is the bare
//     PinCategory.
//   NOTE on `replication_condition`: it is the raw ELifetimeCondition stored on the variable and it is
//     ALWAYS populated (COND_None by default), replicated or not. It only MEANS anything when
//     `is_replicated` is true; on a non-replicated variable it is dead data the editor never consults.
//     Read is_replicated FIRST: it comes from CPF_Net, and it is what tells the two apart.
//   NOTE on `save_game`: the WRITE side is set_blueprint_variable_metadata(save_game=...). It does not compile,
//     so the honest loop is set_blueprint_variable_metadata -> compile_blueprint -> save_asset -> re-read here
//     and check save_game_generated, not save_game.
TSharedPtr<FJsonObject> HandleGetBlueprintVariableDetailedInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, VariableName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variable_name"), VariableName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UBlueprint* Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
    if (!Blueprint)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    FBPVariableDescription* VarDesc = nullptr;
    for (FBPVariableDescription& Desc : Blueprint->NewVariables)
    {
        if (Desc.VarName == FName(*VariableName))
        {
            VarDesc = &Desc;
            break;
        }
    }
    if (!VarDesc)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Variable '%s' not found on blueprint '%s'"), *VariableName, *BlueprintName));
    }

    // FBPVariableDescription has no PropertyType, so `property_type` is a
    // descriptor derived from VarType (FEdGraphPinType), in the same format as diff_blueprints'
    // VarType lambda so the two tools are comparable.
    const FEdGraphPinType& PinType = VarDesc->VarType;
    const UObject* SubCatObj = PinType.PinSubCategoryObject.IsValid() ? PinType.PinSubCategoryObject.Get() : nullptr;

    const TCHAR* ContainerStr = TEXT("none");
    switch (PinType.ContainerType)
    {
        case EPinContainerType::Array: ContainerStr = TEXT("array"); break;
        case EPinContainerType::Set:   ContainerStr = TEXT("set");   break;
        case EPinContainerType::Map:   ContainerStr = TEXT("map");   break;
        default: break;
    }

    FString PropertyTypeStr = PinType.PinCategory.ToString();
    if (SubCatObj) { PropertyTypeStr += TEXT(":") + SubCatObj->GetName(); }
    if (PinType.ContainerType == EPinContainerType::Array)    { PropertyTypeStr += TEXT("[]"); }
    else if (PinType.ContainerType == EPinContainerType::Set) { PropertyTypeStr += TEXT("<Set>"); }
    else if (PinType.ContainerType == EPinContainerType::Map) { PropertyTypeStr += TEXT("<Map>"); }

    TSharedPtr<FJsonObject> TypeDetail = MakeShared<FJsonObject>();
    TypeDetail->SetStringField(TEXT("category"), PinType.PinCategory.ToString());
    TypeDetail->SetStringField(TEXT("sub_category"), PinType.PinSubCategory.ToString());
    TypeDetail->SetStringField(TEXT("sub_category_object"), SubCatObj ? SubCatObj->GetName() : TEXT(""));
    TypeDetail->SetStringField(TEXT("sub_category_object_path"), SubCatObj ? SubCatObj->GetPathName() : TEXT(""));
    TypeDetail->SetStringField(TEXT("container"), ContainerStr);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Data->SetStringField(TEXT("variable_name"), VariableName);

    // FBPVariableDescription::DefaultValue is a ONE-SHOT MAILBOX, not storage. A Full compile feeds
    // it into the new CDO and then clears it -- `Variable.DefaultValue.Empty()`
    // (KismetCompiler.cpp) -- so after any compile it is "" for EVERY variable of EVERY
    // Blueprint, which would read as "the default was lost". The value that actually holds is the
    // one on the CDO, so it is preferred, and the reply says which one answered.
    FString EffectiveDefault = VarDesc->DefaultValue;
    const TCHAR* DefaultSource = TEXT("blueprint_description");
    if (UClass* GenClass = Blueprint->GeneratedClass)
    {
        if (UObject* CDO = GenClass->GetDefaultObject(/*bCreateIfNeeded=*/false))
        {
            if (FProperty* Prop = FindFProperty<FProperty>(CDO->GetClass(), VarDesc->VarName))
            {
                FString FromCDO;
                if (FBlueprintEditorUtils::PropertyValueToString(Prop, reinterpret_cast<const uint8*>(CDO), FromCDO))
                {
                    EffectiveDefault = FromCDO;
                    DefaultSource = TEXT("cdo");
                }
            }
        }
    }
    Data->SetStringField(TEXT("default_value"), EffectiveDefault);
    Data->SetStringField(TEXT("default_value_source"), DefaultSource);
    Data->SetStringField(TEXT("default_value_serialized"), VarDesc->DefaultValue);
    Data->SetStringField(TEXT("var_type"), PinType.PinCategory.ToString());
    Data->SetStringField(TEXT("property_type"), PropertyTypeStr);
    Data->SetObjectField(TEXT("var_type_detail"), TypeDetail);
    Data->SetBoolField(TEXT("is_instance_editable"), (VarDesc->PropertyFlags & CPF_Edit) != 0);
    Data->SetBoolField(TEXT("is_expose_on_spawn"), (VarDesc->PropertyFlags & CPF_ExposeOnSpawn) != 0);
    // "BlueprintReadOnly" is CPF_BlueprintReadOnly (ObjectMacros.h), as in get_blueprint_variables
    // (UEAgentAnalysisCommands.cpp) -- not CPF_BlueprintVisible, which the editor sets on
    // essentially EVERY Blueprint variable and so would answer `true` for all of them.
    Data->SetBoolField(TEXT("is_blueprint_read_only"), (VarDesc->PropertyFlags & CPF_BlueprintReadOnly) != 0);
    // "Expose to Cinematics" in the variable details panel toggles CPF_Interp
    // (ObjectMacros.h; Editor/Kismet BlueprintDetailsCustomization.cpp reads exactly this flag).
    Data->SetBoolField(TEXT("is_expose_to_cinematics"), (VarDesc->PropertyFlags & CPF_Interp) != 0);
    // "SaveGame" -- the Details panel checkbox, written by set_blueprint_variable_metadata(save_game=...)
    //   (set_properties refuses it for not being CPF_Edit, and Python for
    //   CPF_NativeAccessSpecifierProtected).
    //   WARNING: IT LIVES IN TWO PLACES AND THEY CAN DISAGREE, so both are reported instead of picking one:
    //     - `save_game`           = CPF_SaveGame as AUTHORED, on FBPVariableDescription::PropertyFlags.
    //       That is literally what the checkbox writes (FBlueprintEditorUtils::SetVariableSaveGameFlag,
    //       BlueprintEditorUtils.cpp), and it is the same source as every other flag above.
    //     - `save_game_generated` = the same flag on the FProperty of the GENERATED class, which is the
    //       one that decides what a .sav actually contains: the save archive serializes with
    //       ArIsSaveGame=true and asks the compiled FProperty, never the editor descriptor.
    //   The only thing that copies one onto the other is a REAL compile -- KismetCompiler.cpp,
    //   `NewProperty->SetPropertyFlags((EPropertyFlags)Variable.PropertyFlags)`. Marking the box only
    //   structurally-modifies the Blueprint, and that regenerates the SKELETON alone
    //   (BlueprintEditorUtils.cpp, EBlueprintCompileOptions::RegenerateSkeletonOnly), so
    //   `save_game:true + save_game_generated:false` is the NORMAL state between authoring and
    //   compiling -- and it means the value will NOT persist yet. `save_game_matches` answers that in
    //   one look so the caller never has to remember to compare.
    const bool bSaveGameAuthored = (VarDesc->PropertyFlags & CPF_SaveGame) != 0;
    bool bSaveGameGenerated      = false;
    bool bSaveGameGeneratedFound = false;
    if (UClass* GenClassForFlags = Blueprint->GeneratedClass)
    {
        if (const FProperty* GenProp = FindFProperty<FProperty>(GenClassForFlags, VarDesc->VarName))
        {
            bSaveGameGeneratedFound = true;
            bSaveGameGenerated      = GenProp->HasAnyPropertyFlags(CPF_SaveGame);
        }
    }
    Data->SetBoolField(TEXT("save_game"), bSaveGameAuthored);
    Data->SetBoolField(TEXT("save_game_generated"), bSaveGameGenerated);
    // False here means there is no compiled property to read at all (never-compiled Blueprint, or
    //   a variable added since the last compile) => save_game_generated is not a measurement, and the
    //   variable definitely does not persist yet. Distinguishing this from a real `false` is the point.
    Data->SetBoolField(TEXT("save_game_generated_found"), bSaveGameGeneratedFound);
    Data->SetBoolField(TEXT("save_game_matches"),
        bSaveGameGeneratedFound && (bSaveGameGenerated == bSaveGameAuthored));
    // replication_condition alone reads "COND_None" both for a non-replicated variable and for one
    // replicated with the default condition -- indistinguishable. The condition is meaningless unless
    // this is true.
    Data->SetBoolField(TEXT("is_replicated"), (VarDesc->PropertyFlags & CPF_Net) != 0);
    Data->SetStringField(TEXT("replication_condition"), UEnum::GetValueAsString(VarDesc->ReplicationCondition));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// override-surface helpers for list_overridable_functions.
// ---------------------------------------------------------------------------
namespace
{
    // How much logic hangs off an event-override node. An event override that exists but
    // drives nothing is a stub; a graph-shape override is measured by its node count instead.
    // Follows only EXEC output links, with a visited set (a graph can loop back).
    int32 CountEventDownstreamNodes_BPCmd(UEdGraphNode* Start)
    {
        if (!Start)
        {
            return 0;
        }
        TSet<UEdGraphNode*>   Visited;
        TArray<UEdGraphNode*> Stack;
        Visited.Add(Start);
        Stack.Add(Start);
        int32 Count = 0;
        while (Stack.Num() > 0)
        {
            UEdGraphNode* Cur = Stack.Pop();
            if (!Cur)
            {
                continue;
            }
            for (UEdGraphPin* Pin : Cur->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Next = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
                    if (Next && !Visited.Contains(Next))
                    {
                        Visited.Add(Next);
                        Stack.Add(Next);
                        ++Count;
                    }
                }
            }
        }
        return Count;
    }

    // Locate where an inherited function is ACTUALLY implemented, walking up from
    // BaseClass. This is what separates a concrete ancestor (AC_X, with a graph carrying
    // logic) from its `_Abstract` sibling (declares the member, implements nothing) -- the
    // "which one do I inherit from" question, answerable BEFORE creating the child.
    // OutNodeCount == -1 means "not inspectable", which is NOT the same as zero.
    void ResolveParentImplementation_BPCmd(
        UClass* BaseClass, UFunction* Func, bool bIsEventShape,
        FString& OutKind, FString& OutOwnerClass, FString& OutGraphName, int32& OutNodeCount)
    {
        OutKind = TEXT("none");
        OutOwnerClass.Reset();
        OutGraphName.Reset();
        OutNodeCount = -1;

        if (!Func)
        {
            return;
        }
        const FName FuncName = Func->GetFName();

        for (UClass* C = BaseClass; C; C = C->GetSuperClass())
        {
            UBlueprintGeneratedClass* BGC = Cast<UBlueprintGeneratedClass>(C);
            UBlueprint* OwnerBP = BGC ? Cast<UBlueprint>(BGC->ClassGeneratedBy) : nullptr;
            if (!OwnerBP)
            {
                continue; // native link in the chain: nothing graph-shaped to inspect here
            }

            if (bIsEventShape)
            {
                if (UK2Node_Event* Ev = FBlueprintEditorUtils::FindOverrideForFunction(OwnerBP, Func->GetOwnerClass(), FuncName))
                {
                    OutKind       = TEXT("blueprint_event");
                    OutOwnerClass = C->GetPathName();
                    OutGraphName  = Ev->GetGraph() ? Ev->GetGraph()->GetName() : FString();
                    OutNodeCount  = CountEventDownstreamNodes_BPCmd(Ev);
                    return;
                }
            }
            if (UEdGraph* G = FUEAgentGraphUtils::FindGraphByNameDeep(OwnerBP, FuncName.ToString()))
            {
                OutKind       = TEXT("blueprint_graph");
                OutOwnerClass = C->GetPathName();
                OutGraphName  = G->GetName();
                OutNodeCount  = G->Nodes.Num();
                return;
            }
        }

        // No Blueprint ancestor implements it. If the DECLARING class is native, the body is
        // C++: honestly "not inspectable from here", never "a stub".
        if (Func->GetOwnerClass() && !Cast<UBlueprintGeneratedClass>(Func->GetOwnerClass()))
        {
            OutKind       = TEXT("native");
            OutOwnerClass = Func->GetOwnerClass()->GetPathName();
        }
    }
}

// MCP:list_overridable_functions - Enumerate the functions/events that a Blueprint deriving from a class could override. Works on an EXISTING Blueprint (blueprint_name) or on ANY class (class_name) so the override surface can be inspected BEFORE the child exists. Gated with UEdGraphSchema_K2::CanKismetOverrideFunction -- the same predicate add_blueprint_function(override=true) enforces -- so discovery and authoring agree (a bare NativeEvent||FUNC_BlueprintEvent filter lets a parent Blueprint's *__DelegateSignature through as "overridable"). Each entry reports where the member is DECLARED and where it is last IMPLEMENTED, with is_stub, which is what distinguishes a concrete ancestor from its `_Abstract` sibling.
//   @param blueprint_name - Name/path of an existing Blueprint. Its ParentClass is the override surface.
//   @param class_name - Alternative to blueprint_name: any class ("Actor", "/Script/Engine.Pawn", "/Game/.../BP_X.BP_X_C", or a Blueprint asset path). The override surface is THAT class, i.e. what a new child would get. Ignored when blueprint_name is given.
//   @param include_non_overridable - Optional bool (default false). true also lists the entries a NativeEvent||FUNC_BlueprintEvent filter passes but CanKismetOverrideFunction rejects (delegate signatures, BlueprintInternalUseOnly, deprecated), each flagged can_kismet_override=false. Nothing is lost silently.
//   @returns { blueprint_name?, class_name?, source_mode, resolved_class, base_class, ancestors: [string], count, functions: [{ function_name, display_name, declaring_class, is_native_event, is_event, is_interface, interface_name?, kind, can_kismet_override, has_override_graph, has_user_logic, is_implemented, parent_impl_kind, parent_impl_owner_class, parent_impl_graph, parent_impl_node_count, is_stub, is_stub_basis }] }
TSharedPtr<FJsonObject> HandleListOverridableFunctions(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    FString ClassName;
    Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName);
    Params->TryGetStringField(TEXT("class_name"), ClassName);
    if (BlueprintName.IsEmpty() && ClassName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing parameter: pass 'blueprint_name' (existing child) or 'class_name' (surface of any class)"));
    }

    bool bIncludeNonOverridable = false;
    Params->TryGetBoolField(TEXT("include_non_overridable"), bIncludeNonOverridable);

    // BaseClass = the class whose members can be overridden.
    //   blueprint_name -> the Blueprint's ParentClass (what THIS child can override).
    //   class_name     -> that class itself (what a NEW child of it would be able to override).
    UBlueprint* Blueprint = nullptr;
    UClass*     BaseClass = nullptr;
    FString     SourceMode;
    FString     ResolvedClassPath;

    if (!BlueprintName.IsEmpty())
    {
        Blueprint = mcp_common::ResolveBlueprint(BlueprintName);
        if (!Blueprint)
        {
            return mcp_common::BlueprintNotFoundReply(BlueprintName);
        }
        BaseClass = Blueprint->ParentClass;
        if (!BaseClass)
        {
            return mcp_common::MakeErrorReply(TEXT("Blueprint has no ParentClass"));
        }
        SourceMode        = TEXT("blueprint");
        ResolvedClassPath = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName() : Blueprint->GetPathName();
    }
    else
    {
        BaseClass = FUEAgentGraphUtils::ResolveClass(ClassName);
        if (!BaseClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class not found: %s"), *ClassName));
        }
        SourceMode        = TEXT("class");
        ResolvedClassPath = BaseClass->GetPathName();
        // A Blueprint-generated base gives us its UBlueprint too: harmless, and it lets the
        // has_override_graph/is_implemented columns stay meaningful in class mode.
        if (UBlueprintGeneratedClass* BGC = Cast<UBlueprintGeneratedClass>(BaseClass))
        {
            Blueprint = Cast<UBlueprint>(BGC->ClassGeneratedBy);
        }
    }

    // Full ancestor chain. TFieldIterator<UFunction> already walks supers
    // (EFieldIterationFlags::Default == IncludeSuper|IncludeDeprecated, UnrealType.h), so
    // function ENUMERATION is not one-level; what it does not give is the chain itself, per-entry
    // declaring_class, and the interfaces implemented by ANCESTOR Blueprints
    // (Blueprint->ImplementedInterfaces only lists this asset's own).
    TArray<TSharedPtr<FJsonValue>> AncestorsJson;
    for (UClass* C = BaseClass; C; C = C->GetSuperClass())
    {
        AncestorsJson.Add(MakeShared<FJsonValueString>(C->GetPathName()));
    }

    TArray<TSharedPtr<FJsonValue>> Funcs;
    int32 SkippedNonOverridable = 0;

    auto EmitFunc = [&](UFunction* Func, bool bInterface, const FString& IfaceName)
    {
        const bool bIsNativeEvent = Func->HasMetaData(TEXT("NativeEvent"));
        const bool bIsEvent       = Func->HasAnyFunctionFlags(FUNC_BlueprintEvent);
        const bool bCanBeEvent    = UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(Func);
        const bool bCanOverride   = UEdGraphSchema_K2::CanKismetOverrideFunction(Func);

        // ConformImplementedInterfaces PRE-CREATES the override graph for return-value
        // interface functions the instant the interface is added, so graph-existence
        // alone is NOT "the developer implemented it". Expose both signals:
        // has_override_graph (slot exists) and has_user_logic (nodes beyond the auto
        // Entry/Result terminators, i.e. > 2). is_implemented uses the meaningful one.
        UEdGraph* G = Blueprint ? FUEAgentGraphUtils::FindGraphByNameDeep(Blueprint, Func->GetName()) : nullptr;
        const bool bHasOverrideGraph = (G != nullptr);
        const bool bHasUserLogic     = (G && G->Nodes.Num() > 2);
        bool bImplemented = false;
        if (Blueprint)
        {
            if (bCanBeEvent)
            {
                // Interface-event K2Node_Event nodes match on EventReference member-parent ==
                // interface; passing the owner (interface generated class) is correct.
                bImplemented = (FBlueprintEditorUtils::FindOverrideForFunction(
                    Blueprint, Func->GetOwnerClass(), Func->GetFName()) != nullptr);
            }
            else
            {
                bImplemented = bHasUserLogic;
            }
        }

        // Where the inherited body actually lives (or does not).
        FString ImplKind, ImplOwner, ImplGraph;
        int32   ImplNodes = -1;
        ResolveParentImplementation_BPCmd(BaseClass, Func, bCanBeEvent, ImplKind, ImplOwner, ImplGraph, ImplNodes);

        bool    bIsStub = false;
        FString StubBasis;
        if (ImplKind == TEXT("blueprint_graph"))
        {
            // Entry (+ Result) terminators are auto-created: <= 2 nodes is an empty body.
            bIsStub   = (ImplNodes <= 2);
            StubBasis = TEXT("graph_node_count");
        }
        else if (ImplKind == TEXT("blueprint_event"))
        {
            bIsStub   = (ImplNodes <= 0);
            StubBasis = TEXT("event_downstream_node_count");
        }
        else if (ImplKind == TEXT("native"))
        {
            bIsStub   = false;
            StubBasis = TEXT("native_body_not_inspectable");
        }
        else
        {
            bIsStub   = true;
            StubBasis = TEXT("no_ancestor_implementation");
        }

        TSharedPtr<FJsonObject> F = MakeShared<FJsonObject>();
        F->SetStringField(TEXT("function_name"), Func->GetName());
        F->SetStringField(TEXT("display_name"), Func->GetDisplayNameText().ToString());
        F->SetStringField(TEXT("declaring_class"), Func->GetOwnerClass() ? Func->GetOwnerClass()->GetPathName() : FString());
        F->SetBoolField(TEXT("is_native_event"), bIsNativeEvent);
        F->SetBoolField(TEXT("is_event"), bIsEvent);
        F->SetBoolField(TEXT("is_interface"), bInterface);
        if (bInterface) { F->SetStringField(TEXT("interface_name"), IfaceName); }
        F->SetStringField(TEXT("kind"), bCanBeEvent ? TEXT("event") : TEXT("function"));
        F->SetBoolField(TEXT("can_kismet_override"), bCanOverride);
        F->SetBoolField(TEXT("has_override_graph"), bHasOverrideGraph);
        F->SetBoolField(TEXT("has_user_logic"), bHasUserLogic);
        F->SetBoolField(TEXT("is_implemented"), bImplemented);
        F->SetStringField(TEXT("parent_impl_kind"), ImplKind);
        F->SetStringField(TEXT("parent_impl_owner_class"), ImplOwner);
        F->SetStringField(TEXT("parent_impl_graph"), ImplGraph);
        F->SetNumberField(TEXT("parent_impl_node_count"), ImplNodes);
        F->SetBoolField(TEXT("is_stub"), bIsStub);
        F->SetStringField(TEXT("is_stub_basis"), StubBasis);
        Funcs.Add(MakeShared<FJsonValueObject>(F));
    };

    // The gate is CanKismetOverrideFunction (EdGraphSchema_K2), the SAME predicate
    // add_blueprint_function(override=true) uses -- it additionally rejects FUNC_Delegate,
    // BlueprintInternalUseOnly and deprecated members that a bare NativeEvent||FUNC_BlueprintEvent
    // test passes. Each name is listed once; DeclaringInterface is null for a class-chain function.
    TSet<FName> Seen;
    auto ConsiderFunc = [&](UFunction* Candidate, UClass* DeclaringInterface)
    {
        if (!Candidate || Seen.Contains(Candidate->GetFName())) { return; }

        const bool bLegacyMatch = Candidate->HasMetaData(TEXT("NativeEvent")) || Candidate->HasAnyFunctionFlags(FUNC_BlueprintEvent);
        const bool bCanOverride = UEdGraphSchema_K2::CanKismetOverrideFunction(Candidate);
        if (!bCanOverride && !(bIncludeNonOverridable && bLegacyMatch))
        {
            if (bLegacyMatch) { ++SkippedNonOverridable; }
            return;
        }
        Seen.Add(Candidate->GetFName());
        EmitFunc(Candidate, /*bInterface=*/DeclaringInterface != nullptr,
            DeclaringInterface ? DeclaringInterface->GetName() : FString());
    };

    // (a) Class-chain functions.
    for (TFieldIterator<UFunction> It(BaseClass); It; ++It)
    {
        ConsiderFunc(*It, nullptr);
    }

    // (b) Interfaces. UClass::Interfaces holds only the interfaces added AT that class, so the
    //     whole chain has to be walked: Blueprint->ImplementedInterfaces alone misses an interface
    //     implemented by a GRANDPARENT Blueprint.
    TSet<UClass*> SeenInterfaces;
    auto EmitInterfaceClass = [&](UClass* IfaceClass)
    {
        if (!IfaceClass || SeenInterfaces.Contains(IfaceClass)) { return; }
        SeenInterfaces.Add(IfaceClass);
        for (TFieldIterator<UFunction> It(IfaceClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
        {
            ConsiderFunc(*It, IfaceClass);
        }
    };

    for (UClass* C = BaseClass; C; C = C->GetSuperClass())
    {
        for (const FImplementedInterface& Impl : C->Interfaces)
        {
            EmitInterfaceClass(Impl.Class);
        }
        if (UBlueprintGeneratedClass* BGC = Cast<UBlueprintGeneratedClass>(C))
        {
            if (UBlueprint* AncestorBP = Cast<UBlueprint>(BGC->ClassGeneratedBy))
            {
                for (const FBPInterfaceDescription& D : AncestorBP->ImplementedInterfaces)
                {
                    EmitInterfaceClass(D.Interface.Get());
                }
            }
        }
    }
    // Interfaces declared on the QUERIED Blueprint itself (blueprint mode: these are not on
    // BaseClass, which is the parent).
    if (Blueprint)
    {
        for (const FBPInterfaceDescription& D : Blueprint->ImplementedInterfaces)
        {
            EmitInterfaceClass(D.Interface.Get());
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    if (!BlueprintName.IsEmpty()) { Data->SetStringField(TEXT("blueprint_name"), BlueprintName); }
    if (!ClassName.IsEmpty())     { Data->SetStringField(TEXT("class_name"), ClassName); }
    Data->SetStringField(TEXT("source_mode"), SourceMode);
    Data->SetStringField(TEXT("resolved_class"), ResolvedClassPath);
    Data->SetStringField(TEXT("base_class"), BaseClass->GetPathName());
    Data->SetArrayField(TEXT("ancestors"), AncestorsJson);
    Data->SetNumberField(TEXT("count"), Funcs.Num());
    // How many bLegacyMatch entries the real gate rejects. A non-zero value here is the
    // measurement of the discovery-vs-authoring mismatch, not a hidden loss.
    Data->SetNumberField(TEXT("filtered_non_overridable"), SkippedNonOverridable);
    Data->SetBoolField(TEXT("include_non_overridable"), bIncludeNonOverridable);
    Data->SetArrayField(TEXT("functions"), Funcs);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Author a component TEMPLATE on a Blueprint SCS node.
// The config-domain set_component_property only reaches LIVE level-actor components;
// a Blueprint's component archetype lives on USCS_Node::ComponentTemplate (the
// _GEN_VARIABLE). The handlers below resolve that template and read or write it; every
// writer marks the Blueprint modified so the change reinstances on the next compile.
// ---------------------------------------------------------------------------
namespace
{
    // Walk ancestor classes (parent, grandparent, ...) to find the SCS node that DECLARES
    // an inherited component -- i.e. the first ancestor Blueprint whose SCS created it. That node
    // is what an FComponentKey needs so the child class' InheritableComponentHandler can override it.
    // OutDeclaringClass (optional) receives the ancestor class that owns the node.
    static USCS_Node* FindDeclaringSCSNode(UBlueprint* BP, FName VarName, UBlueprintGeneratedClass** OutDeclaringClass = nullptr)
    {
        for (UClass* C = BP->ParentClass; C; C = C->GetSuperClass())
        {
            if (UBlueprintGeneratedClass* BGC = Cast<UBlueprintGeneratedClass>(C))
            {
                if (BGC->SimpleConstructionScript)
                {
                    if (USCS_Node* N = BGC->SimpleConstructionScript->FindSCSNode(VarName))
                    {
                        if (OutDeclaringClass)
                        {
                            *OutDeclaringClass = BGC;
                        }
                        return N;
                    }
                }
            }
        }
        return nullptr;
    }

    // Resolve a WRITABLE component template for a Blueprint: its own SCS components, components INHERITED
    // from a parent/grandparent Blueprint (through a per-child override template minted or fetched on the
    // child class' InheritableComponentHandler, ICH) and, best-effort, native inherited subobjects.
    // OutSource records which path resolved: "scs" | "inherited-ich" | "inherited-native".
    // Returns nullptr + fills OutError (as a ready error response) on failure.
    static UActorComponent* ResolveWritableComponentTemplate(
        UBlueprint* BP, const FString& CompName, bool bAllowCreate, FString& OutSource, TSharedPtr<FJsonObject>& OutError)
    {
        OutError = nullptr;
        const FName CompFName(*CompName);

        // (a) OWN SCS component: this Blueprint's own _GEN_VARIABLE archetype.
        if (BP->SimpleConstructionScript)
        {
            if (USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(CompFName))
            {
                if (Node->ComponentTemplate)
                {
                    OutSource = TEXT("scs");
                    return Node->ComponentTemplate;
                }
            }
        }

        // (b) INHERITED via ICH: the component is declared in an ancestor Blueprint's SCS. Route the
        //     override through THIS child class' InheritableComponentHandler so the child carries its
        //     own default. GetOverridenComponentTemplate returns an existing override; when absent and
        //     bAllowCreate, CreateOverridenComponentTemplate mints one (a copy of the best archetype).
        if (USCS_Node* Decl = FindDeclaringSCSNode(BP, CompFName))
        {
            const FComponentKey Key(Decl);
            // CRIT (inherited-ich persistence): acquire the ICH through the UBlueprint overload, NOT
            // BPGC->GetInheritableComponentHandler(). The Kismet compiler propagates the ICH with
            //   BPGClass->InheritableComponentHandler = Blueprint->InheritableComponentHandler   (KismetCompiler.cpp ~3245)
            // and gates CleanAndSanitizeClass' SubObjectsToSave (~803) + post-compile ValidateTemplates (~4885)
            // on the SAME UBlueprint ref. A BPGC-only ICH leaves UBlueprint::InheritableComponentHandler == null,
            // so the CompileBlueprint() the write handler runs next overwrites the BPGC ICH with null and DESTROYS
            // the freshly minted override -> it never survives to disk. The UBlueprint overload delegates
            // creation to the BPGC one AND stamps the UBlueprint back-reference the compiler expects.
            UInheritableComponentHandler* ICH = BP->GetInheritableComponentHandler(/*bCreateIfNecessary=*/bAllowCreate);
            if (ICH)
            {
                UActorComponent* T = ICH->GetOverridenComponentTemplate(Key);
                if (!T && bAllowCreate)
                {
                    ICH->Modify(); // capture the Records-array mutation (transaction + package dirty)
                    T = ICH->CreateOverridenComponentTemplate(Key);
                }
                if (T)
                {
                    OutSource = TEXT("inherited-ich");
                    return T;
                }
            }
        }

        // (c) NATIVE inherited subobject (best-effort): a CreateDefaultSubobject component with no
        //     declaring SCS node. We can write the like-named subobject on the CDO, but persistence
        //     across compile is uncertain -- the confident path is (b).
        if (AActor* CDO = Cast<AActor>(BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr))
        {
            for (UActorComponent* C : CDO->GetComponents())
            {
                if (C && C->GetName() == CompName)
                {
                    OutSource = TEXT("inherited-native");
                    return C;
                }
            }
        }

        // (d) Not found anywhere.
        OutError = mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Component '%s' not found in Blueprint '%s' (searched own SCS, inherited SCS via ICH, and native CDO subobjects)"),
            *CompName, *BP->GetName()));
        return nullptr;
    }

    // READ-ONLY twin of ResolveWritableComponentTemplate -- resolves the template a fresh
    // instance would actually read, and NEVER mints an ICH override. Shared with
    // HandleGetBlueprintComponentProperty so set_blueprint_component_properties can re-read the
    // very same cascade AFTER its compile (the post-write oracle) instead of hand-rolling a
    // second, drifting copy. Order: own SCS -> nearest existing ICH override -> the ancestor's
    // declaring SCS template -> native CDO subobject. Returns nullptr when nothing matches.
    static UActorComponent* ResolveReadOnlyComponentTemplate_BPCmd(
        UBlueprint* BP, const FString& CompName, bool bIncludeInherited,
        FString& OutSource, FString& OutResolvedFromClass)
    {
        OutSource.Reset();
        OutResolvedFromClass.Reset();
        if (!BP)
        {
            return nullptr;
        }
        const FName CompFName(*CompName);

        // (a) OWN SCS component: this Blueprint's own _GEN_VARIABLE archetype.
        if (BP->SimpleConstructionScript)
        {
            if (USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(CompFName))
            {
                if (Node->ComponentTemplate)
                {
                    OutSource = TEXT("scs");
                    OutResolvedFromClass = BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : BP->GetPathName();
                    return Node->ComponentTemplate;
                }
            }
        }

        if (bIncludeInherited)
        {
            // Walk the ancestor Blueprint chain to the declaring SCS node + its owning class.
            UBlueprintGeneratedClass* DeclClass = nullptr;
            USCS_Node* Decl = FindDeclaringSCSNode(BP, CompFName, &DeclClass);
            if (Decl)
            {
                const FComponentKey Key(Decl);
                // (b) existing ICH override -- walk from THIS class UP to (not incl.) the
                //     declaring class, taking the NEAREST override. Mirrors how
                //     USCS_Node::GetActualComponentTemplate resolves at spawn, so a
                //     mid-hierarchy override (A declares -> B overrides -> C reads) is found.
                //     GetInheritableComponentHandler(false) NEVER mints one.
                UActorComponent* Found = nullptr;
                for (UClass* C = BP->GeneratedClass; C && C != DeclClass && !Found; C = C->GetSuperClass())
                {
                    if (UBlueprintGeneratedClass* Cur = Cast<UBlueprintGeneratedClass>(C))
                    {
                        if (UInheritableComponentHandler* ICH = Cur->GetInheritableComponentHandler(false))
                        {
                            if (UActorComponent* Ovr = ICH->GetOverridenComponentTemplate(Key))
                            {
                                Found = Ovr;
                                OutSource = TEXT("inherited-ich");
                                OutResolvedFromClass = Cur->GetPathName();
                            }
                        }
                    }
                }
                if (Found)
                {
                    return Found;
                }
                // (c) else the ancestor's declaring SCS-node template (the inherited default).
                if (Decl->ComponentTemplate)
                {
                    OutSource = TEXT("inherited-scs");
                    OutResolvedFromClass = DeclClass ? DeclClass->GetPathName() : FString();
                    return Decl->ComponentTemplate;
                }
            }

            // (d) native inherited subobject on the CDO (CreateDefaultSubobject with no SCS node).
            if (BP->GeneratedClass)
            {
                if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject()))
                {
                    for (UActorComponent* C : CDO->GetComponents())
                    {
                        if (C && C->GetName() == CompName)
                        {
                            OutSource = TEXT("inherited-native");
                            OutResolvedFromClass = BP->GeneratedClass->GetPathName();
                            return C;
                        }
                    }
                }
            }
        }

        return nullptr;
    }

    // property-LEVEL typed import. mcp_common::SetObjectProperty works at
    // object + property-NAME level; writing a struct member INSIDE a TArray element, or
    // filling a standalone scratch buffer, needs the same routing one level down. Mirrors
    // McpCommonUtils.cpp exactly: FGameplayTagContainer array sugar, single
    // FGameplayTag validated against the registry, recursive nested-tag guardrail, then the
    // canonical JsonValueToUProperty. Kept in lockstep on purpose -- two different import
    // semantics for "the same value" is how silent divergence starts.
    static bool ImportJsonIntoPropertyValue_BPCmd(
        FProperty* Prop, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, FString& OutError)
    {
        if (!Prop || !ValuePtr)
        {
            OutError = TEXT("Invalid property or value pointer");
            return false;
        }
        if (!Value.IsValid())
        {
            OutError = FString::Printf(TEXT("Missing JSON value for '%s'"), *Prop->GetName());
            return false;
        }

        if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
        {
            if (StructProp->Struct == FGameplayTagContainer::StaticStruct() && Value->Type == EJson::Array)
            {
                FGameplayTagContainer Container;
                for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
                {
                    const FString TagName = Entry.IsValid() ? Entry->AsString() : FString();
                    if (TagName.IsEmpty()) { continue; }
                    const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
                    if (!Tag.IsValid())
                    {
                        OutError = FString::Printf(
                            TEXT("Unregistered gameplay tag '%s' for '%s'; refusing to dead-set an invalid tag."),
                            *TagName, *Prop->GetName());
                        return false;
                    }
                    Container.AddTag(Tag);
                }
                *static_cast<FGameplayTagContainer*>(ValuePtr) = MoveTemp(Container);
                return true;
            }
            if (StructProp->Struct == FGameplayTag::StaticStruct())
            {
                FString TagName;
                if (Value->Type == EJson::String)
                {
                    TagName = Value->AsString();
                }
                else if (Value->Type == EJson::Object)
                {
                    const TSharedPtr<FJsonObject> TagObj = Value->AsObject();
                    if (TagObj.IsValid()) { TagObj->TryGetStringField(TEXT("TagName"), TagName); }
                }
                TagName.TrimStartAndEndInline();
                if (TagName.IsEmpty())
                {
                    // Empty string / empty struct clears the tag to the (valid) empty FGameplayTag.
                    *static_cast<FGameplayTag*>(ValuePtr) = FGameplayTag();
                    return true;
                }
                const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/false);
                if (!Tag.IsValid())
                {
                    OutError = FString::Printf(
                        TEXT("Unregistered gameplay tag '%s' for '%s' (not in the tag registry); refusing to dead-set an invalid tag."),
                        *TagName, *Prop->GetName());
                    return false;
                }
                *static_cast<FGameplayTag*>(ValuePtr) = Tag;
                return true;
            }
        }

        // Recursive tag guardrail: a tag nested in a struct/array/set/map would otherwise be
        // dead-set without ever consulting the registry.
        FString TagError;
        if (!mcp_common::ValidateGameplayTagsInJson(Prop, Value, TagError))
        {
            OutError = FString::Printf(TEXT("Failed to set '%s': %s"), *Prop->GetName(), *TagError);
            return false;
        }

        FText FailReason;
        if (!FJsonObjectConverter::JsonValueToUProperty(
                Value, Prop, ValuePtr, /*CheckFlags=*/0, /*SkipFlags=*/0, /*bStrictMode=*/false, &FailReason))
        {
            OutError = FString::Printf(TEXT("Failed to set '%s' (type %s): %s"),
                *Prop->GetName(), *Prop->GetClass()->GetName(), *FailReason.ToString());
            return false;
        }
        return true;
    }

    // Stable string key for grouping/duplicate detection. Prefers the typed JSON form
    // (an FGameplayTag comes back as the bare "A.B.C" string via McpPropertyExportOverride,
    // NOT the "(TagName=...)" export blob) and falls back to export text for everything else.
    FString PropertyValueToKeyString_BPCmd(FProperty* Prop, const void* ValuePtr)
    {
        if (!Prop || !ValuePtr)
        {
            return FString();
        }
        const TSharedPtr<FJsonValue> V = mcp_common::PropertyValueToJson(Prop, ValuePtr);
        FString Key;
        if (V.IsValid() && V->TryGetString(Key))
        {
            return Key;
        }
        double Num = 0.0;
        if (V.IsValid() && V->TryGetNumber(Num))
        {
            return FString::SanitizeFloat(Num);
        }
        bool bVal = false;
        if (V.IsValid() && V->TryGetBool(bVal))
        {
            return bVal ? TEXT("true") : TEXT("false");
        }
        FString Exported;
        Prop->ExportTextItem_Direct(Exported, ValuePtr, /*DefaultValue=*/nullptr, /*Parent=*/nullptr, PPF_None);
        return Exported;
    }

    // A key that carries no information -- an empty/None gap in the array. These are the
    // rows that look like duplicates of each other but are really holes; they get their own
    // bucket so a "7 duplicates" verdict is not manufactured out of 7 empty slots.
    bool IsEmptyKeyString_BPCmd(const FString& Key)
    {
        FString Trimmed = Key;
        Trimmed.TrimStartAndEndInline();
        return Trimmed.IsEmpty()
            || Trimmed.Equals(TEXT("None"), ESearchCase::IgnoreCase)
            || Trimmed.Equals(TEXT("()"))
            || Trimmed.Equals(TEXT("null"), ESearchCase::IgnoreCase);
    }
}

// MCP:get_blueprint_component_object_path - Resolve a Blueprint component's TEMPLATE archetype and return its object path, so get_properties/set_properties/call_function can target that component directly.
//   @param blueprint_name - Name/path of the Blueprint that owns the component.
//   @param component_name - Component variable name. Resolves an OWN SCS component (the _GEN_VARIABLE archetype on USCS_Node::ComponentTemplate) or an INHERITED/native component (the like-named subobject on the generated-class CDO).
//   @returns { object_path: string, class: string, source: 'scs'|'inherited' } -- object_path is GetPathName() (feed it to get_properties / set_properties); class is the component class' full path.

TSharedPtr<FJsonObject> HandleGetBlueprintComponentObjectPath(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName)) { return Missing; }

    UBlueprint* BP = mcp_common::ResolveBlueprint(BlueprintName);
    if (!BP)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    UActorComponent* Template = nullptr;
    FString Source;

    // 1. Own SCS component: the _GEN_VARIABLE archetype. FindSCSNode matches BOTH the variable
    //    name AND the _GEN_VARIABLE internal name (same resolution set_blueprint_component_properties uses).
    if (BP->SimpleConstructionScript)
    {
        if (USCS_Node* Node = BP->SimpleConstructionScript->FindSCSNode(FName(*ComponentName)))
        {
            if (Node->ComponentTemplate)
            {
                Template = Node->ComponentTemplate;
                Source = TEXT("scs");
            }
        }
    }

    // 2. Inherited component (declared in a parent Blueprint's SCS, or a native
    //    CreateDefaultSubobject): its archetype is the like-named subobject instantiated on the
    //    generated-class CDO. That CDO subobject is exactly what get_properties / set_properties resolve
    //    for inherited components, so returning its path lets callers target it uniformly.
    if (!Template && BP->GeneratedClass)
    {
        if (AActor* CDO = Cast<AActor>(BP->GeneratedClass->GetDefaultObject()))
        {
            for (UActorComponent* C : CDO->GetComponents())
            {
                if (C && C->GetName() == ComponentName)
                {
                    Template = C;
                    Source = TEXT("inherited");
                    break;
                }
            }
        }
    }

    if (!Template)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Component '%s' not found in Blueprint '%s' (searched SCS nodes and generated-class CDO subobjects)"),
            *ComponentName, *BlueprintName));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("object_path"), Template->GetPathName());
    Data->SetStringField(TEXT("class"), Template->GetClass()->GetPathName());
    Data->SetStringField(TEXT("source"), Source);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_blueprint_component_property - Read the EFFECTIVE default of a property (or, without property_name, ALL properties) on a Blueprint component TEMPLATE as typed JSON, resolving components INHERITED from a parent/grandparent Blueprint. Use it rather than get_object_info on one template's path, which reads that template alone and misses inherited SCS components and their ICH overrides. Order: own SCS template -> nearest existing ICH override -> ancestor's declaring SCS template -> native CDO subobject; source says which. NEVER mints an ICH override. group_by_field flags duplicate keys in an array of structs. Write: set_blueprint_component_properties.
//   @param blueprint_name - Blueprint asset path or short name
//   @param component_name - Component variable name (own or inherited)
//   @param property_name - Property to read; omit to return ALL properties of the resolved template
//   @param include_inherited - Also resolve inherited/ICH/native components (default true); when false only this Blueprint's own SCS components are considered
//   @param group_by_field - Optional. Only with property_name pointing at a TArray of STRUCTS: group the elements by this struct member and report duplicate_groups[]/empty_key_indices[]/has_duplicates. Two elements sharing a key is invisible in a plain value dump, yet when that array feeds a TMap the LAST one silently wins (Map.Add overwrites) -- this is the guardrail for exactly that class of bug.
//   @param report_duplicates - Optional bool. Implied true when group_by_field is given; pass true WITHOUT group_by_field to group by the WHOLE element value instead of one member.
//   @returns { blueprint_name: string, component_name: string, component_class: string, source: 'scs'|'inherited-ich'|'inherited-scs'|'inherited-native', resolved_from_class: string, property_name: string, value: any (single-property mode), properties: object, property_count: int (all-properties mode), duplicate_analysis?: { grouped_by, element_count, distinct_keys, duplicate_groups: [{key, count, indices[]}], empty_key_indices: [int], has_duplicates: bool, keys: [string] } }

TSharedPtr<FJsonObject> HandleGetBlueprintComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName)) { return Missing; }

    // Without a property_name every property of the template is returned.
    const bool bSingle = Params->TryGetStringField(TEXT("property_name"), PropertyName) && !PropertyName.IsEmpty();

    bool bIncludeInherited = true;
    Params->TryGetBoolField(TEXT("include_inherited"), bIncludeInherited);

    FString GroupByField;
    Params->TryGetStringField(TEXT("group_by_field"), GroupByField);
    bool bReportDuplicates = false;
    Params->TryGetBoolField(TEXT("report_duplicates"), bReportDuplicates);
    const bool bWantDuplicateAnalysis = bReportDuplicates || !GroupByField.IsEmpty();

    UBlueprint* BP = mcp_common::ResolveBlueprint(BlueprintName);
    if (!BP)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    // The read-only cascade, shared with set_blueprint_component_properties
    // so the "what would a fresh instance read" question has ONE implementation.
    FString Source;
    FString ResolvedFromClass;
    UActorComponent* Template =
        ResolveReadOnlyComponentTemplate_BPCmd(BP, ComponentName, bIncludeInherited, Source, ResolvedFromClass);

    if (!Template)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Component '%s' not found in Blueprint '%s' (searched own SCS%s)"),
            *ComponentName, *BlueprintName,
            bIncludeInherited ? TEXT(", inherited SCS/ICH, and native CDO subobjects") : TEXT("")));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("blueprint_name"), BlueprintName);
    Result->SetStringField(TEXT("component_name"), ComponentName);
    Result->SetStringField(TEXT("component_class"), Template->GetClass()->GetPathName());
    Result->SetStringField(TEXT("source"), Source);
    Result->SetStringField(TEXT("resolved_from_class"), ResolvedFromClass);

    if (bSingle)
    {
        FProperty* Prop = FindFProperty<FProperty>(Template->GetClass(), FName(*PropertyName));
        if (!Prop)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Property '%s' not found on component '%s' (class %s)"),
                *PropertyName, *ComponentName, *Template->GetClass()->GetName()));
        }
        Result->SetStringField(TEXT("property_name"), PropertyName);
        Result->SetField(TEXT("value"),
            mcp_common::PropertyValueToJson(Prop, Prop->ContainerPtrToValuePtr<void>(Template)));

        // ---- duplicate / grouping analysis (opt-in) ---------------------------------
        // A plain value dump shows 25 elements and nothing else. When that array is later
        // folded into a TMap keyed by one of its members, two elements sharing that key means
        // the LAST one silently overwrites the first (TMap::Add semantics) -- a defect that is
        // structurally invisible in the dump. This grouping is the guardrail.
        if (bWantDuplicateAnalysis)
        {
            FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop);
            if (!ArrProp)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("group_by_field/report_duplicates require '%s' to be a TArray property (it is %s)"),
                    *PropertyName, *Prop->GetClass()->GetName()));
            }

            FProperty* KeyProp = nullptr;   // null => key on the WHOLE element
            if (!GroupByField.IsEmpty())
            {
                FStructProperty* InnerStruct = CastField<FStructProperty>(ArrProp->Inner);
                if (!InnerStruct || !InnerStruct->Struct)
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("group_by_field='%s' requires the elements of '%s' to be structs (inner is %s). Omit group_by_field and pass report_duplicates=true to group by the whole element."),
                        *GroupByField, *PropertyName, *ArrProp->Inner->GetClass()->GetName()));
                }
                KeyProp = InnerStruct->Struct->FindPropertyByName(FName(*GroupByField));
                if (!KeyProp)
                {
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("Struct member '%s' not found on element type '%s' of '%s'"),
                        *GroupByField, *InnerStruct->Struct->GetName(), *PropertyName));
                }
            }

            FScriptArrayHelper Helper(ArrProp, Prop->ContainerPtrToValuePtr<void>(Template));
            const int32 Count = Helper.Num();

            TMap<FString, TArray<int32>> Groups;
            TArray<TSharedPtr<FJsonValue>> KeysJson;
            TArray<TSharedPtr<FJsonValue>> EmptyIdxJson;
            for (int32 i = 0; i < Count; ++i)
            {
                uint8* ElemPtr = Helper.GetRawPtr(i);
                const FString Key = KeyProp
                    ? PropertyValueToKeyString_BPCmd(KeyProp, KeyProp->ContainerPtrToValuePtr<void>(ElemPtr))
                    : PropertyValueToKeyString_BPCmd(ArrProp->Inner, ElemPtr);
                KeysJson.Add(MakeShared<FJsonValueString>(Key));
                if (IsEmptyKeyString_BPCmd(Key))
                {
                    // Empty/None gaps are NOT duplicates of each other; they get their own bucket
                    // so a "N duplicates" verdict is never manufactured out of N holes.
                    EmptyIdxJson.Add(MakeShared<FJsonValueNumber>(i));
                    continue;
                }
                Groups.FindOrAdd(Key).Add(i);
            }

            TArray<TSharedPtr<FJsonValue>> DupGroups;
            for (const TPair<FString, TArray<int32>>& Pair : Groups)
            {
                if (Pair.Value.Num() < 2)
                {
                    continue;
                }
                TArray<TSharedPtr<FJsonValue>> Indices;
                for (int32 Idx : Pair.Value)
                {
                    Indices.Add(MakeShared<FJsonValueNumber>(Idx));
                }
                TSharedPtr<FJsonObject> GroupObj = MakeShared<FJsonObject>();
                GroupObj->SetStringField(TEXT("key"), Pair.Key);
                GroupObj->SetNumberField(TEXT("count"), Pair.Value.Num());
                GroupObj->SetArrayField(TEXT("indices"), Indices);
                // With TMap::Add semantics the highest index is the one that survives.
                GroupObj->SetNumberField(TEXT("last_index_wins"), Pair.Value.Last());
                DupGroups.Add(MakeShared<FJsonValueObject>(GroupObj));
            }

            TSharedPtr<FJsonObject> Analysis = MakeShared<FJsonObject>();
            Analysis->SetStringField(TEXT("grouped_by"), KeyProp ? GroupByField : FString(TEXT("<whole element>")));
            Analysis->SetNumberField(TEXT("element_count"), Count);
            Analysis->SetNumberField(TEXT("distinct_keys"), Groups.Num());
            Analysis->SetArrayField(TEXT("duplicate_groups"), DupGroups);
            Analysis->SetArrayField(TEXT("empty_key_indices"), EmptyIdxJson);
            Analysis->SetBoolField(TEXT("has_duplicates"), DupGroups.Num() > 0);
            Analysis->SetArrayField(TEXT("keys"), KeysJson);
            Result->SetObjectField(TEXT("duplicate_analysis"), Analysis);
            // Mirrored at the top level too: callers that only want the verdict shouldn't
            // have to know the nested shape.
            Result->SetBoolField(TEXT("has_duplicates"), DupGroups.Num() > 0);
        }
    }
    else if (bWantDuplicateAnalysis)
    {
        return mcp_common::MakeErrorReply(
            TEXT("group_by_field/report_duplicates need a 'property_name' naming the TArray to analyse (they do not apply to the all-properties dump)"));
    }
    else
    {
        TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
        int32 Count = 0;
        for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            Props->SetField(Prop->GetName(),
                mcp_common::PropertyValueToJson(Prop, Prop->ContainerPtrToValuePtr<void>(Template)));
            ++Count;
        }
        Result->SetObjectField(TEXT("properties"), Props);
        Result->SetNumberField(TEXT("property_count"), Count);
    }

    return Result;
}

// MCP:set_blueprint_component_collision - Set collision on a Blueprint component TEMPLATE (own SCS or inherited via ICH), not on a spawned actor, applying in this order: `profile` through SetCollisionProfileName (which expands the profile into the FBodyInstance response array -- a raw reflection write would set the string and leave BodyInstance inconsistent, and that is the whole reason this is a separate tool), then `collision_enabled`, then per-channel `responses`. Use it instead of set_blueprint_component_properties for anything collision-shaped. Unknown channel, response or collision_enabled names are hard errors, never a silent default. Note the ORDER: a later profile would reset the per-channel responses, so responses always win here.
//   @param blueprint_name - Blueprint asset path or name (required)
//   @param component_name - Component to modify (required); must resolve to a UPrimitiveComponent or the call errors with "no collision to set"
//   @param profile - Collision profile name, e.g. "BlockAll" (optional). Applied FIRST, so it does not undo the per-channel responses below.
//   @param collision_enabled - ECollisionEnabled value name: NoCollision, QueryOnly, PhysicsOnly or QueryAndPhysics (optional). Any other string is an error.
//   @param responses - Object mapping channel -> response, e.g. {"Visibility":"Ignore","ECC_Pawn":"Overlap"} (optional). Channels accept the friendly display name or an ECC_* token (custom project channels included); responses are Ignore, Overlap or Block. An unknown channel is refused rather than defaulting to WorldStatic.
//   @param compile - Optional bool (default FALSE): compile the Blueprint afterwards so the FBodyInstance change reinstances on the next spawn/PIE. Without it the edit stays in the uncompiled template.
//   @param return_messages - No effect here: it is only read by the compile this tool may run internally, whose message log is discarded. Call compile_blueprint directly if you want the messages.
//   @returns { success, data: { blueprint_name, component_name, source ("scs" | "inherited-ich" | "inherited-native": which template was written), profile?, collision_enabled?, responses_applied: [{channel, response}], compiled } } -- responses_applied lists ONLY the per-channel writes; profile and collision_enabled appear as their own fields only when they were given. Nothing is re-read: verify with get_blueprint_component_property or get_collision_info.
TSharedPtr<FJsonObject> HandleSetBlueprintComponentCollision(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName;
    if (TSharedPtr<FJsonObject> Missing = UEAgentBlueprintCommandsImpl::ReadBlueprintComponentNames(
            Params, &mcp_common::MissingParameterReply, BlueprintName, ComponentName))
    {
        return Missing;
    }
    UBlueprint* BP = mcp_common::ResolveBlueprint(BlueprintName);
    if (!BP)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    // The same writable template set_blueprint_component_properties writes: own SCS, an ICH override
    // for an INHERITED component (minted if absent), or a native inherited subobject.
    FString Source;
    TSharedPtr<FJsonObject> Err;
    UActorComponent* Tmpl = ResolveWritableComponentTemplate(BP, ComponentName, /*bAllowCreate=*/true, Source, Err);
    if (!Tmpl) { return Err; }

    UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Tmpl);
    if (!Prim)
    {
        return mcp_common::MakeErrorReply(TEXT("Component is not a UPrimitiveComponent; it has no collision to set"));
    }
    Prim->Modify();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Applied;

    // The profile goes through SetCollisionProfileName, which expands it into the FBodyInstance response array (a raw
    // reflection write would set the string but leave BodyInstance inconsistent -- the reason this is its own tool);
    // then collision_enabled, then the per-channel responses ({"ECC_Pawn":"Overlap", "Visibility":"Block", ...}).
    FString Profile;
    Params->TryGetStringField(TEXT("profile"), Profile);
    FString EnabledStr;
    Params->TryGetStringField(TEXT("collision_enabled"), EnabledStr);
    const TSharedPtr<FJsonObject>* ResponsesObj = nullptr;
    const bool bHasResponses = Params->TryGetObjectField(TEXT("responses"), ResponsesObj);
    FString CollisionError;
    if (!PrimitiveComponentUtils::ApplyCollisionSettings(Prim, Profile, EnabledStr, bHasResponses ? ResponsesObj : nullptr,
            Data, Applied, CollisionError))
    {
        return mcp_common::MakeErrorReply(CollisionError);
    }

    FBlueprintEditorUtils::MarkBlueprintAsModified(BP);

    // Optional compile so the FBodyInstance changes reliably reinstance on next spawn/PIE.
    bool bCompile = false;
    if (Params->TryGetBoolField(TEXT("compile"), bCompile) && bCompile)
    {
        HandleCompileBlueprint(Params);
    }

    Data->SetStringField(TEXT("blueprint_name"), BP->GetName());
    Data->SetStringField(TEXT("component_name"), ComponentName);
    Data->SetStringField(TEXT("source"), Source);
    Data->SetArrayField(TEXT("responses_applied"), Applied);
    Data->SetBoolField(TEXT("compiled"), bCompile);
    return mcp_common::MakeOkReply(Data);
}

// =====================================================================================
// Batch component-override re-authoring + element-level TArray authoring.
// Both live below ResolveWritableComponentTemplate / ResolveReadOnlyComponentTemplate_BPCmd
// on purpose: they reuse the SAME two cascades set_blueprint_component_properties already
// uses, so "where does this value come from" has one answer across the whole file.
// =====================================================================================

// HARD ENGINE LIMIT: RE-CLASSIFYING an
// inherited component (changing the CLASS of the template, not its values) is impossible
// through the ICH: InheritableComponentHandler.cpp mints the override with
// BestArchetype->GetClass(), IsRecordValid invalidates any record whose class
// differs, and ValidateTemplates deletes the record and renames the template to
// the transient package -- so any hack evaporates on the next load. Neither tool below
// attempts it; both only ever write VALUES onto an existing template.

// MCP:set_blueprint_component_properties - Set the DEFAULT of one or several properties on Blueprint component TEMPLATES -- own SCS components, ones INHERITED from a parent Blueprint (written as an InheritableComponentHandler override) and native subobjects -- with ONE compile and ONE save, then verify each value on the post-compile template. Use it for component templates: set_properties neither creates an inherited override nor compiles or saves; set_component_property edits placed actors. One write: component_name + property_name + property_value; several: overrides[]. TRAPS behind a green reply: (1) it can drop another component IN MEMORY -- count get_blueprint_components before and after, cure with reload_asset_package(on_dirty="discard"); (2) an object-reference write can land None and still report override_persisted:true -- check get_blueprint_component_overrides; (3) actors already placed keep their serialized value. Collision: set_blueprint_component_collision; override-free read: get_blueprint_component_property.
//   @param blueprint_name - Blueprint asset path or short name
//   @param overrides - Optional array of { component_name, property_name, property_value } objects, applied in the given order. Required unless the single-write form (component_name + property_name + property_value) is given; both together apply overrides[] first, then the single write.
//   @param component_name - Optional single-write form: component variable name (own or inherited). Needs property_name and property_value too.
//   @param property_name - Optional single-write form: property to set on that component template.
//   @param property_value - Optional single-write form: the new value (any JSON type; same importer as set_component_property). An array value REWRITES THE WHOLE ARRAY, including the empty/None gaps -- set_blueprint_component_array_element edits one element and proves the siblings did not move.
//   @param compile - Optional bool (default true). false skips the compile: nothing is verifiable, every entry reports matches_requested=false with verification_status='not_verifiable'
//   @param save - Optional bool (default true). Saves the Blueprint package ONCE, after the compile
//   @returns { success, error?, data: { blueprint_name, requested_count, applied_count, verified_count, mismatched_count, failed_count, compiled, saved, save_error?, equality_predicate, results: [{ index, component_name, property_name, source, component_class, override_persisted, applied, apply_error?, verification_status, matches_requested, post_value, post_source, note? }], verification } } -- source tells which template was hit (scs / inherited-ich / inherited-scs / inherited-native); component_class is that template's class, read before the compile; override_persisted is false only when an inherited-ich override did not survive the compile.

TSharedPtr<FJsonObject> HandleApplyComponentPropertyOverrides(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }

    // overrides[] and the single-write form are both optional; at least one has to be there.
    const TArray<TSharedPtr<FJsonValue>>* OverridesArr = nullptr;
    const bool bHasOverrides = Params->TryGetArrayField(TEXT("overrides"), OverridesArr) && OverridesArr && OverridesArr->Num() > 0;
    FString SingleComponent;
    FString SingleProperty;
    const bool bHasSingleComponent = Params->TryGetStringField(TEXT("component_name"), SingleComponent) && !SingleComponent.IsEmpty();
    const bool bHasSingleProperty = Params->TryGetStringField(TEXT("property_name"), SingleProperty) && !SingleProperty.IsEmpty();
    // property_value is a RAW JsonValue (numbers/bools/arrays/objects flow straight into the importer).
    const TSharedPtr<FJsonValue> SingleValue = Params->TryGetField(TEXT("property_value"));
    const bool bHasSingle = bHasSingleComponent || bHasSingleProperty || SingleValue.IsValid();
    if (bHasSingle && !(bHasSingleComponent && bHasSingleProperty && SingleValue.IsValid()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The single-write form needs component_name, property_name and property_value together; missing: %s"),
            !bHasSingleComponent ? TEXT("component_name") : (!bHasSingleProperty ? TEXT("property_name") : TEXT("property_value"))));
    }
    if (!bHasOverrides && !bHasSingle)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: overrides (non-empty array of {component_name, property_name, property_value}), or component_name + property_name + property_value for a single write"));
    }

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UBlueprint* BP = mcp_common::ResolveBlueprint(BlueprintName);
    if (!BP)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    // ---- 1. Parse + apply every override onto its writable template ------------------
    struct FPendingOverride
    {
        FString                Component;
        FString                Property;
        TSharedPtr<FJsonValue> Value;
        FString                Source;
        FString                ComponentClass;
        bool                   bApplied = false;
        FString                ApplyError;
    };

    TArray<FPendingOverride> Pending;
    const int32 ListedCount = bHasOverrides ? OverridesArr->Num() : 0;
    Pending.Reserve(ListedCount + 1);

    for (int32 i = 0; i < ListedCount; ++i)
    {
        const TSharedPtr<FJsonValue>& Entry = (*OverridesArr)[i];
        const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(EntryObjPtr) || !EntryObjPtr || !EntryObjPtr->IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("overrides[%d] is not an object"), i));
        }
        const TSharedPtr<FJsonObject>& EntryObj = *EntryObjPtr;

        FPendingOverride P;
        if (!EntryObj->TryGetStringField(TEXT("component_name"), P.Component) || P.Component.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("overrides[%d] is missing 'component_name'"), i));
        }
        if (!EntryObj->TryGetStringField(TEXT("property_name"), P.Property) || P.Property.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("overrides[%d] is missing 'property_name'"), i));
        }
        P.Value = EntryObj->TryGetField(TEXT("property_value"));
        if (!P.Value.IsValid())
        {
            P.Value = EntryObj->TryGetField(TEXT("value")); // tolerated alias
        }
        if (!P.Value.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("overrides[%d] is missing 'property_value'"), i));
        }
        Pending.Add(MoveTemp(P));
    }
    if (bHasSingle)
    {
        FPendingOverride P;
        P.Component = SingleComponent;
        P.Property = SingleProperty;
        P.Value = SingleValue;
        Pending.Add(MoveTemp(P));
    }

    int32 AppliedCount = 0;
    for (FPendingOverride& P : Pending)
    {
        TSharedPtr<FJsonObject> ErrObj;
        UActorComponent* Tmpl =
            ResolveWritableComponentTemplate(BP, P.Component, /*bAllowCreate=*/true, P.Source, ErrObj);
        if (!Tmpl)
        {
            // Keep going: a batch that aborts halfway leaves the Blueprint in a state nobody
            // asked for. Every failure is reported per entry and folded into `success`.
            P.ApplyError = FString::Printf(TEXT("Component '%s' could not be resolved to a writable template"), *P.Component);
            continue;
        }
        // Read BEFORE the compile: reinstancing can trash/duplicate the template object.
        P.ComponentClass = Tmpl->GetClass()->GetName();
        Tmpl->Modify();
        FString SetErr;
        if (!mcp_common::SetObjectProperty(Tmpl, P.Property, P.Value, SetErr))
        {
            P.ApplyError = SetErr;
            continue;
        }
        P.bApplied = true;
        ++AppliedCount;
    }

    // ---- 2. ONE structural mark + ONE compile for the whole batch --------------------
    // ICH override records are structural and only take effect after a compile; doing this
    // once is the entire point of the tool.
    bool bCompiled = false;
    if (AppliedCount > 0)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        if (bCompile)
        {
            FKismetEditorUtilities::CompileBlueprint(BP);
            bCompiled = true;
        }
    }

    // ---- 3. Verify against the POST-COMPILE template ---------------------------------
    // Oracle: import the REQUESTED json into a standalone default-constructed value of the
    // same FProperty and compare with the engine's own equality (FProperty::Identical,
    // PPF_DeepComparison -- not exported text, which reports two equal FTexts as different).
    // This is deliberately NOT "read back what we just wrote": the scratch holds what the
    // CALLER asked for, so a value the engine clamps, ignores, or loses during the compile
    // (a dropped ICH record) comes back matches_requested=false.
    TArray<TSharedPtr<FJsonValue>> Results;
    int32 VerifiedCount = 0, MismatchedCount = 0, FailedCount = 0;

    for (int32 i = 0; i < Pending.Num(); ++i)
    {
        const FPendingOverride& P = Pending[i];
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetNumberField(TEXT("index"), i);
        R->SetStringField(TEXT("component_name"), P.Component);
        R->SetStringField(TEXT("property_name"), P.Property);
        R->SetStringField(TEXT("source"), P.Source);
        R->SetStringField(TEXT("component_class"), P.ComponentClass);
        // An ICH override record only takes effect through the compile: confirm it survived it.
        bool bOverridePersisted = true;
        if (P.bApplied && P.Source == TEXT("inherited-ich"))
        {
            bOverridePersisted = false;
            if (USCS_Node* Decl = FindDeclaringSCSNode(BP, FName(*P.Component)))
            {
                if (UInheritableComponentHandler* ICH = BP->GetInheritableComponentHandler(/*bCreateIfNecessary=*/false))
                {
                    bOverridePersisted = (ICH->GetOverridenComponentTemplate(FComponentKey(Decl)) != nullptr);
                }
            }
        }
        R->SetBoolField(TEXT("override_persisted"), bOverridePersisted);
        R->SetBoolField(TEXT("applied"), P.bApplied);
        if (!P.ApplyError.IsEmpty())
        {
            R->SetStringField(TEXT("apply_error"), P.ApplyError);
        }

        bool    bMatches = false;
        FString Status;
        FString Note;
        FString PostSource, PostFromClass;

        if (!P.bApplied)
        {
            Status = TEXT("failed");
            Note   = TEXT("the write itself did not happen; nothing to verify");
            ++FailedCount;
        }
        else if (!bCompiled)
        {
            Status = TEXT("not_verifiable");
            Note   = TEXT("compile=false: the template was written but nothing was reinstanced, so no post-compile value exists to check");
        }
        else
        {
            UActorComponent* Post = ResolveReadOnlyComponentTemplate_BPCmd(
                BP, P.Component, /*bIncludeInherited=*/true, PostSource, PostFromClass);
            FProperty* Prop = Post ? FindFProperty<FProperty>(Post->GetClass(), FName(*P.Property)) : nullptr;
            if (!Post)
            {
                Status = TEXT("failed");
                Note   = TEXT("the component no longer resolves after the compile");
                ++FailedCount;
            }
            else if (!Prop)
            {
                Status = TEXT("failed");
                Note   = TEXT("the property no longer exists on the post-compile component class");
                ++FailedCount;
            }
            else
            {
                const void* PostValuePtr = Prop->ContainerPtrToValuePtr<void>(Post);
                R->SetField(TEXT("post_value"), mcp_common::PropertyValueToJson(Prop, PostValuePtr));

                FDefaultConstructedPropertyElement Scratch(Prop);
                FString ImportErr;
                if (!ImportJsonIntoPropertyValue_BPCmd(Prop, Scratch.GetObjAddress(), P.Value, ImportErr))
                {
                    Status = TEXT("not_verifiable");
                    Note   = FString::Printf(
                        TEXT("the requested value could not be re-imported into a scratch value for comparison: %s"), *ImportErr);
                }
                else
                {
                    // Engine equality, NOT export text: PPF_DeepComparison is what makes two
                    // distinct-but-equivalent instanced subobjects compare equal and stops the
                    // FText localization-identity false positives.
                    bMatches = Prop->Identical(PostValuePtr, Scratch.GetObjAddress(), PPF_DeepComparison);
                    Status   = bMatches ? TEXT("verified") : TEXT("mismatch");
                    if (bMatches) { ++VerifiedCount; } else { ++MismatchedCount; }
                }
            }
        }

        R->SetStringField(TEXT("verification_status"), Status);
        R->SetBoolField(TEXT("matches_requested"), bMatches);
        if (!PostSource.IsEmpty())
        {
            R->SetStringField(TEXT("post_source"), PostSource);
            R->SetStringField(TEXT("post_resolved_from_class"), PostFromClass);
        }
        if (!Note.IsEmpty())
        {
            R->SetStringField(TEXT("note"), Note);
        }
        Results.Add(MakeShared<FJsonValueObject>(R));
    }

    // ---- 4. ONE save --------------------------------------------------------------
    bool    bSaved = false;
    FString SaveErr;
    if (bSave && AppliedCount > 0)
    {
        bSaved = mcp_common::SaveAssetPackage(BP->GetOutermost(), BP, SaveErr);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_name"), BP->GetName());
    Data->SetNumberField(TEXT("requested_count"), Pending.Num());
    Data->SetNumberField(TEXT("applied_count"), AppliedCount);
    Data->SetNumberField(TEXT("verified_count"), VerifiedCount);
    Data->SetNumberField(TEXT("mismatched_count"), MismatchedCount);
    Data->SetNumberField(TEXT("failed_count"), FailedCount);
    Data->SetBoolField(TEXT("compiled"), bCompiled);
    Data->SetNumberField(TEXT("compile_count"), bCompiled ? 1 : 0);
    Data->SetBoolField(TEXT("saved"), bSaved);
    if (!SaveErr.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveErr);
    }
    Data->SetStringField(TEXT("equality_predicate"), TEXT("FProperty::Identical(PPF_DeepComparison) vs the requested value re-imported into a scratch element"));
    Data->SetArrayField(TEXT("results"), Results);

    const bool bAllApplied = (AppliedCount == Pending.Num());
    const bool bSaveOk     = (!bSave || AppliedCount == 0 || bSaved);
    const bool bOk         = bAllApplied && (MismatchedCount == 0) && (FailedCount == 0) && bSaveOk;

    mcp_common::EMcpVerification Verification;
    if (!bCompiled)
    {
        Verification = mcp_common::EMcpVerification::NotVerifiable;
    }
    else if (MismatchedCount > 0 || FailedCount > 0)
    {
        Verification = mcp_common::EMcpVerification::Failed;
    }
    else
    {
        Verification = mcp_common::EMcpVerification::Verified;
    }

    FString Error;
    if (!bAllApplied || FailedCount > 0)
    {
        Error = FString::Printf(TEXT("%d of %d override(s) could not be applied (see results[].apply_error)"),
                                Pending.Num() - AppliedCount + FailedCount, Pending.Num());
    }
    else if (MismatchedCount > 0)
    {
        Error = FString::Printf(TEXT("%d override(s) do NOT match the requested value on the post-compile template (see results[].matches_requested)"),
                                MismatchedCount);
    }
    else if (!bSaveOk)
    {
        Error = SaveErr.IsEmpty() ? FString(TEXT("Saving the Blueprint package failed")) : SaveErr;
    }

    return mcp_common::MakeVerifiedReply(
        Data, bOk, Verification,
        TEXT("post_compile_template_reread"),
        TEXT("each value re-read from the template the read-only cascade resolves AFTER the compile, compared with FProperty::Identical against the requested value"),
        Error);
}

// MCP:set_blueprint_component_array_element - Author ONE element of a TArray property on a Blueprint component TEMPLATE (own SCS, or ICH-aware for a component inherited from an ancestor Blueprint): patch named struct members of an existing element, insert a new element at an index, or remove one. Every OTHER element is exported before and after and compared, so a whole-array rewrite that quietly loses the empty/None gaps cannot pass itself off as a targeted edit. There is deliberately NO 'move' operation.
//   @param blueprint_name - Blueprint asset path or short name
//   @param component_name - Component variable name (own or inherited)
//   @param property_name - Name of the TArray property on the component template
//   @param index - 0-based element index. set_fields/remove need an existing index; insert accepts 0..Num (Num appends)
//   @param operation - 'set_fields' (default), 'insert' or 'remove'
//   @param fields - Object of { StructMemberName: value } applied to the element. Used by set_fields, and by insert when the element type is a struct
//   @param element - Whole-element JSON value. Used by insert for NON-struct element types (or instead of 'fields' for a struct)
//   @param compile - Optional bool (default true): compile so the edited template reinstances
//   @param save - Optional bool (default true): save the Blueprint package after the compile
//   @returns { success, error?, data: { blueprint_name, component_name, property_name, source, operation, index, inner_type, num_before, num_after, fields_applied[], field_errors[], siblings_unchanged, siblings_changed[], element_value, post_compile_num, post_compile_element, compiled, saved, save_error?, verification } }

TSharedPtr<FJsonObject> HandleSetBlueprintComponentArrayElement(const TSharedPtr<FJsonObject>& Params)
{
    FString BlueprintName, ComponentName, PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("blueprint_name"), BlueprintName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), ComponentName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }

    double IndexNum = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("index"), IndexNum)) { return Missing; }
    const int32 Index = static_cast<int32>(IndexNum);
    if (Index < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("'index' must be >= 0"));
    }

    FString Operation;
    Params->TryGetStringField(TEXT("operation"), Operation);
    if (Operation.IsEmpty())
    {
        Operation = TEXT("set_fields");
    }
    Operation = Operation.ToLower();
    const bool bSetFields = (Operation == TEXT("set_fields"));
    const bool bInsert    = (Operation == TEXT("insert"));
    const bool bRemove    = (Operation == TEXT("remove"));
    if (!bSetFields && !bInsert && !bRemove)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown operation '%s' (expected set_fields, insert or remove). 'move' is intentionally not supported."), *Operation));
    }

    const TSharedPtr<FJsonObject>* FieldsObjPtr = nullptr;
    // An EMPTY 'fields' object counts as "no fields": otherwise the call would report a
    // successful edit that wrote nothing at all.
    const bool bHasFields = Params->TryGetObjectField(TEXT("fields"), FieldsObjPtr)
        && FieldsObjPtr && FieldsObjPtr->IsValid() && (*FieldsObjPtr)->Values.Num() > 0;
    TSharedPtr<FJsonValue> ElementJson = Params->TryGetField(TEXT("element"));

    bool bCompile = true;
    Params->TryGetBoolField(TEXT("compile"), bCompile);
    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    UBlueprint* BP = mcp_common::ResolveBlueprint(BlueprintName);
    if (!BP)
    {
        return mcp_common::BlueprintNotFoundReply(BlueprintName);
    }

    FString Source;
    TSharedPtr<FJsonObject> ErrObj;
    UActorComponent* Tmpl = ResolveWritableComponentTemplate(BP, ComponentName, /*bAllowCreate=*/true, Source, ErrObj);
    if (!Tmpl)
    {
        return ErrObj;
    }

    FProperty* RawProp = FindFProperty<FProperty>(Tmpl->GetClass(), FName(*PropertyName));
    if (!RawProp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Property '%s' not found on component '%s' (class %s)"),
            *PropertyName, *ComponentName, *Tmpl->GetClass()->GetName()));
    }
    FArrayProperty* ArrProp = CastField<FArrayProperty>(RawProp);
    if (!ArrProp || !ArrProp->Inner)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Property '%s' is not a TArray (it is %s)"), *PropertyName, *RawProp->GetClass()->GetName()));
    }
    FStructProperty* InnerStruct = CastField<FStructProperty>(ArrProp->Inner);

    // Bounds are validated BEFORE touching FScriptArrayHelper: InsertValues/RemoveValues
    // assert (check()) on an out-of-range index, and an assert here kills the editor.
    FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Tmpl));
    const int32 NumBefore = Helper.Num();
    if (bInsert)
    {
        if (Index > NumBefore)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("insert index %d is out of range for '%s' (valid 0..%d)"), Index, *PropertyName, NumBefore));
        }
    }
    else if (Index >= NumBefore)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("index %d is out of range for '%s' (%d element(s))"), Index, *PropertyName, NumBefore));
    }
    if (bSetFields && !bHasFields && !ElementJson.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("operation='set_fields' needs a non-empty 'fields' object (or an 'element' value)"));
    }
    if (bSetFields && bHasFields && !InnerStruct)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'fields' requires struct elements; '%s' holds %s. Pass 'element' with the whole value instead."),
            *PropertyName, *ArrProp->Inner->GetClass()->GetName()));
    }

    // Sibling snapshot BEFORE the edit -- exported text per element, which is the finest-grained
    // "did anything else move" signal available without a second scratch array.
    auto SnapshotElements = [&]() -> TArray<FString>
    {
        TArray<FString> Out;
        const int32 N = Helper.Num();
        Out.Reserve(N);
        for (int32 i = 0; i < N; ++i)
        {
            FString S;
            ArrProp->Inner->ExportTextItem_Direct(S, Helper.GetRawPtr(i), /*DefaultValue=*/nullptr, /*Parent=*/nullptr, PPF_None);
            Out.Add(MoveTemp(S));
        }
        return Out;
    };
    const TArray<FString> Before = SnapshotElements();

    Tmpl->Modify();

    TArray<TSharedPtr<FJsonValue>> FieldsApplied;
    TArray<TSharedPtr<FJsonValue>> FieldErrors;
    bool bWriteOk = true;

    if (bRemove)
    {
        Helper.RemoveValues(Index, 1);
    }
    else
    {
        if (bInsert)
        {
            Helper.InsertValues(Index, 1); // constructs the new element (ConstructItems)
        }

        uint8* ElemPtr = Helper.GetRawPtr(Index);

        // A whole-element value wins over per-field patching when both are given for insert.
        if (ElementJson.IsValid() && (!bHasFields || !InnerStruct))
        {
            FString ImportErr;
            if (!ImportJsonIntoPropertyValue_BPCmd(ArrProp->Inner, ElemPtr, ElementJson, ImportErr))
            {
                FieldErrors.Add(MakeShared<FJsonValueString>(ImportErr));
                bWriteOk = false;
            }
            else
            {
                FieldsApplied.Add(MakeShared<FJsonValueString>(TEXT("<whole element>")));
            }
        }

        if (bHasFields && InnerStruct && InnerStruct->Struct)
        {
            // FJsonObject::Values keys are UE::FSharedString -- iterate with `auto`
            // and deref (*Key) to a TCHAR*, exactly like HandleSetBlueprintComponentCollision.
            for (const auto& Pair : (*FieldsObjPtr)->Values)
            {
                const FString MemberName(*Pair.Key);
                FProperty* Member = InnerStruct->Struct->FindPropertyByName(FName(*MemberName));
                if (!Member)
                {
                    FieldErrors.Add(MakeShared<FJsonValueString>(FString::Printf(
                        TEXT("Struct member '%s' not found on '%s'"), *MemberName, *InnerStruct->Struct->GetName())));
                    bWriteOk = false;
                    continue;
                }
                FString ImportErr;
                if (!ImportJsonIntoPropertyValue_BPCmd(Member, Member->ContainerPtrToValuePtr<void>(ElemPtr), Pair.Value, ImportErr))
                {
                    FieldErrors.Add(MakeShared<FJsonValueString>(ImportErr));
                    bWriteOk = false;
                    continue;
                }
                FieldsApplied.Add(MakeShared<FJsonValueString>(MemberName));
            }
        }
    }

    const TArray<FString> After = SnapshotElements();
    const int32 NumAfter = Helper.Num();

    // Sibling comparison, index-shift aware: insert pushes everything at/after `index` up by
    // one, remove pulls everything after it down by one, set_fields keeps them in place.
    TArray<TSharedPtr<FJsonValue>> SiblingsChanged;
    for (int32 i = 0; i < Before.Num(); ++i)
    {
        if (bSetFields && i == Index)
        {
            continue; // the targeted element is SUPPOSED to change
        }
        if (bRemove && i == Index)
        {
            continue; // removed on purpose
        }
        int32 Mapped = i;
        if (bInsert && i >= Index)      { Mapped = i + 1; }
        else if (bRemove && i > Index)  { Mapped = i - 1; }
        if (!After.IsValidIndex(Mapped) || After[Mapped] != Before[i])
        {
            SiblingsChanged.Add(MakeShared<FJsonValueNumber>(i));
        }
    }
    const bool bSiblingsUnchanged = (SiblingsChanged.Num() == 0);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("blueprint_name"), BP->GetName());
    Data->SetStringField(TEXT("component_name"), ComponentName);
    Data->SetStringField(TEXT("property_name"), PropertyName);
    Data->SetStringField(TEXT("source"), Source);
    Data->SetStringField(TEXT("operation"), Operation);
    Data->SetNumberField(TEXT("index"), Index);
    Data->SetStringField(TEXT("inner_type"), ArrProp->Inner->GetCPPType());
    Data->SetNumberField(TEXT("num_before"), NumBefore);
    Data->SetNumberField(TEXT("num_after"), NumAfter);
    Data->SetArrayField(TEXT("fields_applied"), FieldsApplied);
    Data->SetArrayField(TEXT("field_errors"), FieldErrors);
    Data->SetBoolField(TEXT("siblings_unchanged"), bSiblingsUnchanged);
    Data->SetArrayField(TEXT("siblings_changed"), SiblingsChanged);
    if (!bRemove && Helper.IsValidIndex(Index))
    {
        Data->SetField(TEXT("element_value"), mcp_common::PropertyValueToJson(ArrProp->Inner, Helper.GetRawPtr(Index)));
    }

    // Capture what we still need from the (possibly reinstanced) template BEFORE compiling.
    const FString CompClassName = Tmpl->GetClass()->GetName();
    Data->SetStringField(TEXT("component_class"), CompClassName);

    bool bCompiled = false;
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
    if (bCompile)
    {
        FKismetEditorUtilities::CompileBlueprint(BP);
        bCompiled = true;
    }
    Data->SetBoolField(TEXT("compiled"), bCompiled);

    // Post-compile re-read: the array length is the cheapest honest proof the edit survived
    // reinstancing (a lost ICH record shows up here as the ancestor's original length).
    bool bPostOk = true;
    if (bCompiled)
    {
        FString PostSource, PostFromClass;
        UActorComponent* Post = ResolveReadOnlyComponentTemplate_BPCmd(
            BP, ComponentName, /*bIncludeInherited=*/true, PostSource, PostFromClass);
        FArrayProperty* PostArr = Post ? CastField<FArrayProperty>(FindFProperty<FProperty>(Post->GetClass(), FName(*PropertyName))) : nullptr;
        if (PostArr && PostArr->Inner)
        {
            FScriptArrayHelper PostHelper(PostArr, PostArr->ContainerPtrToValuePtr<void>(Post));
            Data->SetNumberField(TEXT("post_compile_num"), PostHelper.Num());
            Data->SetStringField(TEXT("post_source"), PostSource);
            if (!bRemove && PostHelper.IsValidIndex(Index))
            {
                Data->SetField(TEXT("post_compile_element"),
                    mcp_common::PropertyValueToJson(PostArr->Inner, PostHelper.GetRawPtr(Index)));
            }
            bPostOk = (PostHelper.Num() == NumAfter);
        }
        else
        {
            // Say so instead of quietly claiming the edit stuck.
            Data->SetNumberField(TEXT("post_compile_num"), -1);
            Data->SetStringField(TEXT("post_compile_note"),
                TEXT("the component/property did not resolve after the compile; the post-compile state is UNKNOWN"));
            bPostOk = false;
        }
    }

    bool    bSaved = false;
    FString SaveErr;
    if (bSave)
    {
        bSaved = mcp_common::SaveAssetPackage(BP->GetOutermost(), BP, SaveErr);
    }
    Data->SetBoolField(TEXT("saved"), bSaved);
    if (!SaveErr.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveErr);
    }

    const bool bOk = bWriteOk && bSiblingsUnchanged && bPostOk && (!bSave || bSaved);

    FString Error;
    if (!bWriteOk)
    {
        Error = TEXT("one or more field writes failed (see field_errors)");
    }
    else if (!bSiblingsUnchanged)
    {
        Error = FString::Printf(TEXT("%d sibling element(s) changed as a side effect (see siblings_changed)"), SiblingsChanged.Num());
    }
    else if (!bPostOk)
    {
        Error = TEXT("the post-compile array does not match the edited array (the change did not survive reinstancing)");
    }
    else if (bSave && !bSaved)
    {
        Error = SaveErr.IsEmpty() ? FString(TEXT("Saving the Blueprint package failed")) : SaveErr;
    }

    return mcp_common::MakeVerifiedReply(
        Data, bOk,
        bCompiled ? (bOk ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed)
                  : mcp_common::EMcpVerification::NotVerifiable,
        TEXT("element_export_diff_plus_post_compile_reread"),
        TEXT("every element exported before and after the edit (index-shift aware), then the array re-read from the post-compile template"),
        Error);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_ai_perception_sense"), &HandleAddAIPerceptionSense },
    { TEXT("add_blueprint_function"), &HandleAddBlueprintFunction },
    { TEXT("add_component_to_blueprint"), &HandleAddComponentToBlueprint },
    { TEXT("set_blueprint_component_properties"), &HandleApplyComponentPropertyOverrides },
    { TEXT("apply_material_to_blueprint"), &HandleApplyMaterialToBlueprint },
    { TEXT("attach_blueprint_component"), &HandleAttachBlueprintComponent },
    { TEXT("compile_blueprint"), &HandleCompileBlueprint },
    { TEXT("create_blueprint"), &HandleCreateBlueprint },
    { TEXT("get_actor_material_info"), &HandleGetActorMaterialInfo },
    { TEXT("get_ai_perception_senses"), &HandleGetAIPerceptionSenses },
    { TEXT("remove_ai_perception_sense"), &HandleRemoveAIPerceptionSense },
    { TEXT("get_available_materials"), &HandleGetAvailableMaterials },
    { TEXT("get_blueprint_component_object_path"), &HandleGetBlueprintComponentObjectPath },
    { TEXT("get_blueprint_component_property"), &HandleGetBlueprintComponentProperty },
    { TEXT("get_blueprint_functions"), &HandleGetBlueprintFunctions },
    { TEXT("get_blueprint_info"), &HandleGetBlueprintInfo },
    { TEXT("get_blueprint_material_info"), &HandleGetBlueprintMaterialInfo },
    { TEXT("get_blueprint_variable_detailed_info"), &HandleGetBlueprintVariableDetailedInfo },
    { TEXT("get_blueprint_variables"), &HandleGetBlueprintVariables },
    { TEXT("list_overridable_functions"), &HandleListOverridableFunctions },
    { TEXT("remove_blueprint_function"), &HandleRemoveBlueprintFunction },
    { TEXT("remove_component_from_blueprint"), &HandleRemoveComponentFromBlueprint },
    { TEXT("remove_unused_variables"), &HandleRemoveUnusedVariables },
    { TEXT("set_blueprint_component_array_element"), &HandleSetBlueprintComponentArrayElement },
    { TEXT("set_blueprint_component_collision"), &HandleSetBlueprintComponentCollision },
    { TEXT("set_mesh_material_color"), &HandleSetMeshMaterialColor },
    { TEXT("set_physics_properties"), &HandleSetPhysicsProperties },
    { TEXT("set_static_mesh_properties"), &HandleSetStaticMeshProperties },
    { TEXT("spawn_blueprint_actor"), &HandleSpawnBlueprintActor },
};
} // namespace UEAgentBlueprintCommands
