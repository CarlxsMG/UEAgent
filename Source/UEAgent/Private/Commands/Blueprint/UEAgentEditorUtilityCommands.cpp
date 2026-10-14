#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Asset / package machinery
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/TopLevelAssetPath.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"

// Blueprint compile / structural-modified
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"

// Blutility (editor) module
#include "EditorUtilityBlueprint.h"
#include "EditorUtilityBlueprintFactory.h"
#include "EditorUtilityObject.h"
#include "EditorUtilityWidget.h"
#include "EditorUtilityWidgetBlueprint.h"
#include "EditorUtilityWidgetBlueprintFactory.h"
#include "EditorUtilitySubsystem.h"

// UMG runtime: widget tree + CanvasPanel root
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/CanvasPanel.h"

// Editor subsystem access
#include "Editor.h"

// Scripted Asset/Actor Action Utility parent classes (Blutility).
#include "AssetActionUtility.h"
#include "ActorActionUtility.h"

/**
 * Editor Utility Widget / Blueprint (Blutility) commands.
 *
 * Authoring + running of editor scripting assets that live in the Blutility
 * editor module:
 *   - Editor Utility Blueprints               (UEditorUtilityBlueprint)
 *   - Editor Utility Widget Blueprints        (UEditorUtilityWidgetBlueprint), run as an editor tab
 *                                             through UEditorUtilitySubsystem::SpawnAndRegisterTabAndGetID
 *   - Scripted Asset/Actor Action Utilities   (UAssetActionUtility / UActorActionUtility)
 *
 * Module: Blutility (editor). Also uses UMG/UMGEditor for the widget tree / CanvasPanel root.
 */
namespace UEAgentEditorUtilityCommands
{

// MCP-DOMAIN: Blueprint

namespace
{
    /**
     * The fields every create reply of this family opens with: what was asked for and the Blueprint at that path,
     * whose parent_class falls back to FallbackParentName when the Blueprint has none.
     */
    TSharedPtr<FJsonObject> BlueprintReply_EditorUtility(const FString& Name, const FString& PackagePath,
                                                        const UBlueprint* Blueprint, const FString& FallbackParentName)
    {
        const UClass* Parent = Blueprint->ParentClass;
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("package_path"), PackagePath);
        Resp->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
        Resp->SetStringField(TEXT("blueprint_class"), Blueprint->GetClass()->GetName());
        Resp->SetStringField(TEXT("parent_class"), Parent ? Parent->GetName() : FallbackParentName);
        return Resp;
    }

    /** Every asset under SearchPath, subfolders included, whose class is the Blutility asset class AssetClassName or a child of it. */
    TArray<FAssetData> FindBlutilityAssets_EditorUtility(const FString& SearchPath, const TCHAR* AssetClassName)
    {
        IAssetRegistry& Registry = mcp_common::AssetRegistry();

        FARFilter Filter;
        Filter.ClassPaths.Add(FTopLevelAssetPath(TEXT("/Script/Blutility"), AssetClassName));
        Filter.bRecursiveClasses = true;
        Filter.bRecursivePaths = true;
        Filter.PackagePaths.Add(FName(*SearchPath));

        TArray<FAssetData> FoundAssets;
        Registry.GetAssets(Filter, FoundAssets);
        return FoundAssets;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Compile + save the blueprint asset's package to disk.
bool SaveBlueprintAsset(UBlueprint* Blueprint, FString& OutError)
{
    if (!Blueprint)
    {
        OutError = TEXT("Invalid Blueprint");
        return false;
    }

    // Recompile so the generated class reflects the edited asset.
    FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

    return mcp_common::SaveAssetPackage(Blueprint->GetOutermost(), Blueprint, OutError);
}

// Resolve the requested parent class for an Editor Utility Blueprint. Defaults to
// UEditorUtilityObject (the canonical Blutility object base). Validates the
// resolved class can actually back an editor utility blueprint.
UClass* ResolveEditorUtilityParentClass(const FString& ParentClass, FString& OutError)
{
    if (ParentClass.IsEmpty()
        || ParentClass == TEXT("UEditorUtilityObject")
        || ParentClass == TEXT("EditorUtilityObject"))
    {
        return UEditorUtilityObject::StaticClass();
    }

    // UObject is explicitly allowed.
    if (ParentClass == TEXT("UObject") || ParentClass == TEXT("Object"))
    {
        return UObject::StaticClass();
    }

    UClass* Found = nullptr;

    // Try a fully-qualified path first (e.g. /Script/Blutility.EditorUtilityObject or /Game/...).
    Found = LoadObject<UClass>(nullptr, *ParentClass);
    if (!Found)
    {
        // Fall back to a slow type search by short name (the engine has no ANY_PACKAGE).
        Found = UClass::TryFindTypeSlow<UClass>(ParentClass);
    }

    if (!Found)
    {
        OutError = FString::Printf(TEXT("parent_class '%s' could not be resolved to a UClass"), *ParentClass);
        return nullptr;
    }

    return Found;
}

// ---------------------------------------------------------------------------
// create_editor_utility_blueprint
// ---------------------------------------------------------------------------
// MCP:create_editor_utility_blueprint - Create (and save) a UEditorUtilityBlueprint (a Blutility object)
//   @param package_path - /Game folder to create the asset in (e.g. /Game/EditorUtilities)
//   @param name - Asset name (no extension)
//   @param parent_class - Parent UClass name or path (default UEditorUtilityObject)
//   @returns { name, package_path, asset_path, blueprint_class, parent_class, created, already_existed, saved, save_error }
TSharedPtr<FJsonObject> HandleCreateEditorUtilityBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString ParentClassName = TEXT("UEditorUtilityObject");
    Params->TryGetStringField(TEXT("parent_class"), ParentClassName);

    FString ResolveErr;
    UClass* ParentClass = ResolveEditorUtilityParentClass(ParentClassName, ResolveErr);
    if (!ParentClass)
    {
        return mcp_common::MakeErrorReply(ResolveErr);
    }

    // The factory requires a class that FKismetEditorUtilities can create a blueprint of.
    if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("parent_class '%s' cannot back an Editor Utility Blueprint"), *ParentClass->GetName()));
    }

    FString NormalizedPath = PackagePath;
    NormalizedPath.RemoveFromEnd(TEXT("/"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *NormalizedPath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load and return a full payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UObject* ExistingObj = UEditorAssetLibrary::LoadAsset(FullPath);
        UEditorUtilityBlueprint* Existing = Cast<UEditorUtilityBlueprint>(ExistingObj);
        if (Existing)
        {
            TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, Existing, FString());
            Resp->SetBoolField(TEXT("created"), false);
            Resp->SetBoolField(TEXT("already_existed"), true);
            Resp->SetBoolField(TEXT("saved"), true);
            return mcp_common::MakeOkReply(Resp);
        }
    }

    // Create via the real editor factory. IAssetTools::CreateAsset does NOT call
    // ConfigureProperties (only CreateAssetWithDialog does), so this is headless-safe;
    // we set ParentClass on the factory beforehand since the factory's FactoryCreateNew
    // reads it directly.
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UEditorUtilityBlueprintFactory* Factory = NewObject<UEditorUtilityBlueprintFactory>();
    Factory->ParentClass = ParentClass;

    UObject* NewAsset = AssetTools.CreateAsset(Name, NormalizedPath, UEditorUtilityBlueprint::StaticClass(), Factory);
    UEditorUtilityBlueprint* NewBP = Cast<UEditorUtilityBlueprint>(NewAsset);
    if (!NewBP)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create Editor Utility Blueprint at %s"), *FullPath));
    }

    FString SaveErr;
    const bool bSaved = SaveBlueprintAsset(NewBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, NewBP, ParentClass->GetName());
    Resp->SetBoolField(TEXT("created"), true);
    Resp->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_editor_utility_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// create_editor_utility_widget_blueprint
// ---------------------------------------------------------------------------
// MCP:create_editor_utility_widget_blueprint - Create (and save) a UEditorUtilityWidgetBlueprint with a CanvasPanel root
//   @param package_path - /Game folder to create the asset in (e.g. /Game/EditorUtilities)
//   @param name - Asset name (no extension)
//   @returns { name, package_path, asset_path, blueprint_class, parent_class, root_widget, root_widget_class, created, already_existed, root_created, saved, save_error }
TSharedPtr<FJsonObject> HandleCreateEditorUtilityWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString NormalizedPath = PackagePath;
    NormalizedPath.RemoveFromEnd(TEXT("/"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *NormalizedPath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load and return a full payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UObject* ExistingObj = UEditorAssetLibrary::LoadAsset(FullPath);
        UEditorUtilityWidgetBlueprint* Existing = Cast<UEditorUtilityWidgetBlueprint>(ExistingObj);
        if (Existing)
        {
            const UWidget* Root = Existing->WidgetTree ? Existing->WidgetTree->RootWidget : nullptr;
            TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, Existing, FString());
            Resp->SetStringField(TEXT("root_widget"), Root ? Root->GetName() : TEXT(""));
            Resp->SetStringField(TEXT("root_widget_class"), Root ? Root->GetClass()->GetName() : TEXT(""));
            Resp->SetBoolField(TEXT("created"), false);
            Resp->SetBoolField(TEXT("already_existed"), true);
            Resp->SetBoolField(TEXT("root_created"), false);
            Resp->SetBoolField(TEXT("saved"), true);
            return mcp_common::MakeOkReply(Resp);
        }
    }

    // Create via the real editor factory. We set ParentClass (UEditorUtilityWidget)
    // and RootWidgetClass (CanvasPanel) on the factory directly; IAssetTools::CreateAsset
    // does not invoke ConfigureProperties so no modal class picker is shown headless.
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UEditorUtilityWidgetBlueprintFactory* Factory = NewObject<UEditorUtilityWidgetBlueprintFactory>();
    Factory->ParentClass = UEditorUtilityWidget::StaticClass();
    // Default the factory's root widget to a CanvasPanel. The factory's
    // FactoryCreateNew also defaults to CanvasPanel when the template selector is
    // off, but set it explicitly so behavior is deterministic.
    Factory->RootWidgetClass = UCanvasPanel::StaticClass();

    UObject* NewAsset = AssetTools.CreateAsset(Name, NormalizedPath, UEditorUtilityWidgetBlueprint::StaticClass(), Factory);
    UEditorUtilityWidgetBlueprint* NewBP = Cast<UEditorUtilityWidgetBlueprint>(NewAsset);
    if (!NewBP)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create Editor Utility Widget Blueprint at %s"), *FullPath));
    }

    if (!NewBP->WidgetTree)
    {
        return mcp_common::MakeErrorReply(TEXT("Created Editor Utility Widget Blueprint has no WidgetTree"));
    }

    // Defensively ensure a CanvasPanel root exists (matches the widget handler).
    bool bCreatedRoot = false;
    if (NewBP->WidgetTree->RootWidget == nullptr)
    {
        UCanvasPanel* RootCanvas = NewBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
        if (!RootCanvas)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to construct root CanvasPanel"));
        }
        NewBP->WidgetTree->RootWidget = RootCanvas;
        NewBP->OnVariableAdded(RootCanvas->GetFName());
        bCreatedRoot = true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NewBP);

    FString SaveErr;
    const bool bSaved = SaveBlueprintAsset(NewBP, SaveErr);

    const UWidget* RootWidget = NewBP->WidgetTree->RootWidget;

    TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, NewBP,
                                                                UEditorUtilityWidget::StaticClass()->GetName());
    Resp->SetBoolField(TEXT("created"), true);
    Resp->SetBoolField(TEXT("already_existed"), false);
    Resp->SetBoolField(TEXT("root_created"), bCreatedRoot);
    Resp->SetStringField(TEXT("root_widget"), RootWidget ? RootWidget->GetName() : TEXT(""));
    Resp->SetStringField(TEXT("root_widget_class"), RootWidget ? RootWidget->GetClass()->GetName() : TEXT(""));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_editor_utility_widget_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// run_editor_utility_widget
// ---------------------------------------------------------------------------
// MCP:run_editor_utility_widget - Spawn and register an Editor Utility Widget Blueprint as an editor tab
//   @param widget_blueprint_path - /Game path of the Editor Utility Widget Blueprint
//   @returns { widget_blueprint_path, asset_path, tab_id, registration_name, registered, tab_exists, widget_spawned }
TSharedPtr<FJsonObject> HandleRunEditorUtilityWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_blueprint_path"), WidgetBPPath)) { return Missing; }

    if (!UEditorAssetLibrary::DoesAssetExist(WidgetBPPath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Editor Utility Widget Blueprint not found at path: %s"), *WidgetBPPath));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(WidgetBPPath);
    UEditorUtilityWidgetBlueprint* WidgetBP = Cast<UEditorUtilityWidgetBlueprint>(Asset);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset at %s is not an Editor Utility Widget Blueprint"), *WidgetBPPath));
    }

    if (!GEditor)
    {
        return mcp_common::MakeErrorReply(TEXT("GEditor is not available"));
    }

    UEditorUtilitySubsystem* Subsystem = GEditor->GetEditorSubsystem<UEditorUtilitySubsystem>();
    if (!Subsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("UEditorUtilitySubsystem is not available"));
    }

    // SpawnAndRegisterTabAndGetID registers a tab spawner and attempts to invoke it.
    // Headless / -unattended, the tab manager may not be able to surface a visible
    // tab, in which case the returned widget can be null even though the spawner was
    // registered. Report the honest result either way.
    FName NewTabID = NAME_None;
    UEditorUtilityWidget* SpawnedWidget = Subsystem->SpawnAndRegisterTabAndGetID(WidgetBP, NewTabID);

    const FName RegistrationName = WidgetBP->GetRegistrationName();
    const bool bTabExists = (NewTabID != NAME_None) ? Subsystem->DoesTabExist(NewTabID) : false;
    const bool bRegistered = (NewTabID != NAME_None) || !RegistrationName.IsNone();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("widget_blueprint_path"), WidgetBPPath);
    Resp->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
    Resp->SetStringField(TEXT("tab_id"), NewTabID.IsNone() ? TEXT("") : NewTabID.ToString());
    Resp->SetStringField(TEXT("registration_name"), RegistrationName.IsNone() ? TEXT("") : RegistrationName.ToString());
    Resp->SetBoolField(TEXT("registered"), bRegistered);
    Resp->SetBoolField(TEXT("tab_exists"), bTabExists);
    Resp->SetBoolField(TEXT("widget_spawned"), SpawnedWidget != nullptr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// list_editor_utility_widgets
// ---------------------------------------------------------------------------
// MCP:list_editor_utility_widgets - Enumerate UEditorUtilityWidgetBlueprint assets via the Asset Registry.
//   @param path - Optional content path to search under (default /Game)
//   @returns { name, asset_path, package_name, package_path, search_path, count, widgets }
TSharedPtr<FJsonObject> HandleListEditorUtilityWidgets(const TSharedPtr<FJsonObject>& Params)
{
    FString SearchPath = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), SearchPath); // optional override

    // UEditorUtilityWidgetBlueprint assets are stored under /Script/Blutility in the
    // asset registry (the BP asset class, not the generated class).
    const TArray<FAssetData> FoundAssets = FindBlutilityAssets_EditorUtility(SearchPath, TEXT("EditorUtilityWidgetBlueprint"));

    TArray<TSharedPtr<FJsonValue>> WidgetsOut;
    for (const FAssetData& Asset : FoundAssets)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Entry->SetStringField(TEXT("asset_path"), Asset.GetObjectPathString());
        Entry->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
        Entry->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());
        WidgetsOut.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("search_path"), SearchPath);
    Resp->SetNumberField(TEXT("count"), WidgetsOut.Num());
    Resp->SetArrayField(TEXT("widgets"), WidgetsOut);
    return mcp_common::MakeOkReply(Resp);
}

// ===========================================================================
// Editor Utility scripted actions (Asset/Actor Action Utilities)
// ===========================================================================
// UAssetActionUtility / UActorActionUtility are the blueprint parent classes for
// right-click "Scripted Asset/Actor Actions". A scripted action is any
// CallInEditor UFUNCTION (no return value) exposed on the generated class; the
// editor enumerates those via the asset registry "CallableFunctions" tag and
// surfaces them in the content-browser / level-editor right-click menus.
// Creating the utility BP and the reflection lookup of its action functions are
// fully headless-safe. Actually *invoking* a scripted action through the menu
// machinery requires a content-browser/level selection + the registered menu
// extension, which only exists in an interactive editor. We invoke parameterless
// CallInEditor functions directly for real when possible, and otherwise return an
// honest invoked=false with a "requires selected" note.
// ---------------------------------------------------------------------------

// Shared create path for the two scripted-action-utility commands. ParentClass is
// UAssetActionUtility or UActorActionUtility. CreateBlueprint(parent), compile+save.
TSharedPtr<FJsonObject> CreateScriptedActionUtility(const TSharedPtr<FJsonObject>& Params, UClass* ParentClass)
{
    if (!ParentClass)
    {
        return mcp_common::MakeErrorReply(TEXT("Internal error: null parent class"));
    }

    FString PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString NormalizedPath = PackagePath;
    NormalizedPath.RemoveFromEnd(TEXT("/"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *NormalizedPath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load it and return the same stable payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UObject* ExistingObj = UEditorAssetLibrary::LoadAsset(FullPath);
        UBlueprint* Existing = Cast<UBlueprint>(ExistingObj);
        if (Existing)
        {
            TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, Existing, ParentClass->GetName());
            Resp->SetBoolField(TEXT("created"), false);
            Resp->SetBoolField(TEXT("already_existed"), true);
            Resp->SetBoolField(TEXT("saved"), true);
            return mcp_common::MakeOkReply(Resp);
        }
        // Existing asset is not a blueprint -- fall through to error below.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("An asset already exists at %s and is not a Blueprint"), *FullPath));
    }

    if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("parent_class '%s' cannot back a scripted action utility blueprint"), *ParentClass->GetName()));
    }

    // Create the package + the Editor Utility Blueprint headlessly. We use
    // FKismetEditorUtilities::CreateBlueprint directly with UEditorUtilityBlueprint
    // as the BP class (the canonical class for AssetActionUtility / ActorActionUtility
    // blueprints) so no modal factory picker is ever shown.
    const FString PackageName = FullPath;
    UPackage* Package = CreatePackage(*PackageName);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create package %s"), *PackageName));
    }

    UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
        ParentClass,
        Package,
        FName(*Name),
        BPTYPE_Normal,
        UEditorUtilityBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass(),
        NAME_None);

    if (!NewBP)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create scripted action utility blueprint at %s"), *FullPath));
    }

    NewBP->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
    FAssetRegistryModule::AssetCreated(NewBP);

    FString SaveErr;
    const bool bSaved = SaveBlueprintAsset(NewBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, NewBP, ParentClass->GetName());
    Resp->SetBoolField(TEXT("created"), true);
    Resp->SetBoolField(TEXT("already_existed"), false);
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// create_asset_action_utility
// ---------------------------------------------------------------------------
// MCP:create_asset_action_utility - Create a scripted Asset Action Utility blueprint (parent UAssetActionUtility)
//   @param package_path - Destination content folder, e.g. "/Game/MyGame".
//   @param name - Asset name (no extension).
//   @returns success or error payload
TSharedPtr<FJsonObject> HandleCreateAssetActionUtility(const TSharedPtr<FJsonObject>& Params)
{
    return CreateScriptedActionUtility(Params, UAssetActionUtility::StaticClass());
}

// ---------------------------------------------------------------------------
// create_actor_action_utility
// ---------------------------------------------------------------------------
// MCP:create_actor_action_utility - Create a scripted Actor Action Utility blueprint (parent UActorActionUtility)
//   @param package_path - Destination content folder, e.g. "/Game/MyGame".
//   @param name - Asset name (no extension).
//   @returns success or error payload
TSharedPtr<FJsonObject> HandleCreateActorActionUtility(const TSharedPtr<FJsonObject>& Params)
{
    return CreateScriptedActionUtility(Params, UActorActionUtility::StaticClass());
}

// ---------------------------------------------------------------------------
// list_scripted_action_utilities
// ---------------------------------------------------------------------------
// Enumerate Editor Utility Blueprints whose generated/parent class is a child of
// UAssetActionUtility or UActorActionUtility. We resolve each candidate's
// generated class (loading the BP if necessary) and classify it.
// MCP:list_scripted_action_utilities - List Editor Utility Blueprints that are scripted Asset/Actor Action Utilities
//   @param path - Optional content path to search under (default /Game)
//   @returns { name, path, package_name, kind, parent_class, search_path, count, utilities }
TSharedPtr<FJsonObject> HandleListScriptedActionUtilities(const TSharedPtr<FJsonObject>& Params)
{
    FString SearchPath = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), SearchPath); // optional override

    // All Editor Utility Blueprints (BP asset class lives under /Script/Blutility).
    const TArray<FAssetData> FoundAssets = FindBlutilityAssets_EditorUtility(SearchPath, TEXT("EditorUtilityBlueprint"));

    UClass* AssetActionBase = UAssetActionUtility::StaticClass();
    UClass* ActorActionBase = UActorActionUtility::StaticClass();

    TArray<TSharedPtr<FJsonValue>> UtilitiesOut;
    for (const FAssetData& Asset : FoundAssets)
    {
        UObject* Loaded = Asset.GetAsset();
        UBlueprint* BP = Cast<UBlueprint>(Loaded);
        if (!BP)
        {
            continue;
        }

        // Prefer the compiled generated class; fall back to the parent class.
        UClass* TestClass = BP->GeneratedClass ? BP->GeneratedClass.Get() : BP->ParentClass.Get();
        if (!TestClass)
        {
            continue;
        }

        FString Kind;
        if (TestClass->IsChildOf(AssetActionBase))
        {
            Kind = TEXT("asset");
        }
        else if (TestClass->IsChildOf(ActorActionBase))
        {
            Kind = TEXT("actor");
        }
        else
        {
            continue; // not a scripted action utility
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Asset.AssetName.ToString());
        Entry->SetStringField(TEXT("path"), BP->GetPathName());
        Entry->SetStringField(TEXT("package_name"), Asset.PackageName.ToString());
        Entry->SetStringField(TEXT("kind"), Kind);
        Entry->SetStringField(TEXT("parent_class"), BP->ParentClass ? BP->ParentClass->GetName() : TEXT(""));
        UtilitiesOut.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("search_path"), SearchPath);
    Resp->SetNumberField(TEXT("count"), UtilitiesOut.Num());
    Resp->SetArrayField(TEXT("utilities"), UtilitiesOut);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// run_asset_action_utility
// ---------------------------------------------------------------------------
// A scripted action is a CallInEditor UFUNCTION (no return value) on the utility's
// generated class. We do a REAL reflection check that the function exists, then
// attempt a real headless invocation: if the function takes no parameters we can
// ProcessEvent it on the utility's default object. Functions that depend on a
// content-browser / level selection (which the engine surfaces only through the
// interactive right-click menu) are reported with an honest invoked=false note.
// MCP:run_asset_action_utility - Run (or introspect) a scripted action on an Asset/Actor Action Utility
//   @param utility_path - Object path of the utility blueprint, e.g.
//   @param function_name - CallInEditor action to run (default first action)
//   @returns { utility_path, kind, generated_class, action_functions, function_name, function_found, invoked, note }
TSharedPtr<FJsonObject> HandleRunAssetActionUtility(const TSharedPtr<FJsonObject>& Params)
{
    FString UtilityPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("utility_path"), UtilityPath)) { return Missing; }

    if (!UEditorAssetLibrary::DoesAssetExist(UtilityPath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Scripted action utility not found at path: %s"), *UtilityPath));
    }

    UObject* Asset = UEditorAssetLibrary::LoadAsset(UtilityPath);
    UBlueprint* BP = Cast<UBlueprint>(Asset);
    if (!BP)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset at %s is not a Blueprint"), *UtilityPath));
    }

    UClass* GenClass = BP->GeneratedClass ? BP->GeneratedClass.Get() : BP->ParentClass.Get();
    if (!GenClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Blueprint %s has no generated class"), *UtilityPath));
    }

    const bool bIsAssetAction = GenClass->IsChildOf(UAssetActionUtility::StaticClass());
    const bool bIsActorAction = GenClass->IsChildOf(UActorActionUtility::StaticClass());
    if (!bIsAssetAction && !bIsActorAction)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("%s is not a scripted Asset/Actor Action Utility"), *UtilityPath));
    }

    // Enumerate the CallInEditor action functions (mirror of the engine's
    // FAssetActionUtilityPrototype::AddTagsFor_CallableFunctions logic).
    const static FName NAME_CallInEditor(TEXT("CallInEditor"));
    TArray<TSharedPtr<FJsonValue>> ActionFuncs;
    UFunction* TargetFunc = nullptr;

    FString RequestedFunction;
    Params->TryGetStringField(TEXT("function_name"), RequestedFunction);

    for (TFieldIterator<UFunction> It(GenClass, EFieldIterationFlags::IncludeSuper); It; ++It)
    {
        UFunction* Func = *It;
        if (!Func)
        {
            continue;
        }
        if (Func->HasMetaData(NAME_CallInEditor) && Func->GetReturnProperty() == nullptr)
        {
            ActionFuncs.Add(MakeShared<FJsonValueString>(Func->GetName()));
            if (!RequestedFunction.IsEmpty() && Func->GetName().Equals(RequestedFunction, ESearchCase::IgnoreCase))
            {
                TargetFunc = Func;
            }
            else if (RequestedFunction.IsEmpty() && TargetFunc == nullptr)
            {
                // Default to the first action function when none was requested.
                TargetFunc = Func;
            }
        }
    }

    const bool bFunctionFound = (TargetFunc != nullptr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("utility_path"), BP->GetPathName());
    Resp->SetStringField(TEXT("kind"), bIsAssetAction ? TEXT("asset") : TEXT("actor"));
    Resp->SetStringField(TEXT("generated_class"), GenClass->GetName());
    Resp->SetArrayField(TEXT("action_functions"), ActionFuncs);
    Resp->SetStringField(TEXT("function_name"),
        TargetFunc ? TargetFunc->GetName() : RequestedFunction);
    Resp->SetBoolField(TEXT("function_found"), bFunctionFound);

    if (!bFunctionFound)
    {
        // No CallInEditor action exists (a freshly-created utility BP has none until
        // the user authors one). This is the honest, expected state -- report it.
        Resp->SetBoolField(TEXT("invoked"), false);
        Resp->SetStringField(TEXT("note"),
            ActionFuncs.Num() == 0
                ? TEXT("No CallInEditor scripted action functions are authored on this utility yet")
                : FString::Printf(TEXT("Requested function '%s' was not found among the scripted actions"), *RequestedFunction));
        return mcp_common::MakeOkReply(Resp);
    }

    // We have a real action UFUNCTION. Only parameterless functions can be invoked
    // headlessly without the interactive menu's selection context.
    const bool bHasParams = (TargetFunc->NumParms > 0);
    if (bHasParams)
    {
        Resp->SetBoolField(TEXT("invoked"), false);
        Resp->SetStringField(TEXT("note"),
            TEXT("Scripted action takes parameters and requires a content-browser/level selection ")
            TEXT("supplied by the interactive right-click menu; cannot be invoked headless without a selection"));
        return mcp_common::MakeOkReply(Resp);
    }

    // Invoke the parameterless CallInEditor action for real on a utility instance.
    // The action typically reads the editor selection (empty headless) and operates
    // on it -- invoking it is real even when the selection is empty.
    UObject* Instance = GenClass->GetDefaultObject();
    if (!Instance)
    {
        Resp->SetBoolField(TEXT("invoked"), false);
        Resp->SetStringField(TEXT("note"), TEXT("Could not obtain a default object to invoke the scripted action"));
        return mcp_common::MakeOkReply(Resp);
    }

    Instance->ProcessEvent(TargetFunc, nullptr);

    Resp->SetBoolField(TEXT("invoked"), true);
    Resp->SetStringField(TEXT("note"),
        TEXT("Invoked parameterless CallInEditor action headlessly (no content-browser/level selection was present)"));
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_actor_action_utility"), &HandleCreateActorActionUtility },
    { TEXT("create_asset_action_utility"), &HandleCreateAssetActionUtility },
    { TEXT("create_editor_utility_blueprint"), &HandleCreateEditorUtilityBlueprint },
    { TEXT("create_editor_utility_widget_blueprint"), &HandleCreateEditorUtilityWidgetBlueprint },
    { TEXT("list_editor_utility_widgets"), &HandleListEditorUtilityWidgets },
    { TEXT("list_scripted_action_utilities"), &HandleListScriptedActionUtilities },
    { TEXT("run_asset_action_utility"), &HandleRunAssetActionUtility },
    { TEXT("run_editor_utility_widget"), &HandleRunEditorUtilityWidget },
};
} // namespace UEAgentEditorUtilityCommands
