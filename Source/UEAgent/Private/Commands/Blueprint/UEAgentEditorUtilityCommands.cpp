// Copyright (c) 2026 CarlxsMG.

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
#include "UObject/Script.h"  // FEditorScriptExecutionGuard

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
// MCP:create_editor_utility_blueprint - Create an Editor Utility Blueprint at package_path/name with the editor factory, parented to parent_class, then compile and save it. Idempotent by path: an Editor Utility Blueprint already there is returned as it is (created=false, already_existed=true, saved=false), nothing compiled or saved, with the parent_class it really has; the requested parent_class is not applied to it. A parent_class that does not resolve or cannot back a Blueprint, another kind of asset at that path, or an invalid path is an error and nothing is created. A failed save makes success false with save_error and keeps the new asset in memory. Refused during PIE unless force=true. A tool with a UI panel: create_editor_utility_widget_blueprint. Right-click actions are made here too: parent_class "AssetActionUtility" (Content Browser assets) or "ActorActionUtility" (level actors) -- their CallInEditor functions become the actions.
//   @param package_path - Content folder for the asset, e.g. "/Game/EditorUtilities"; a trailing slash is ignored.
//   @param name - Asset name without path or extension, e.g. "EUB_BatchRename".
//   @param parent_class - Parent class by name without the U or A prefix, e.g. "EditorUtilityActor", or by path such as "/Script/Blutility.EditorUtilityObject" (default EditorUtilityObject); it must be Blueprintable.
//   @returns { name, package_path, asset_path, blueprint_class, parent_class, created, already_existed, saved, save_error? }
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

    // An asset of another kind at that path is an error before anything is created: the factory would
    // otherwise meet it in CreateAsset, which asks to overwrite it.
    const FString Conflict = mcp_common::AssetNameConflict(FullPath, Name, UEditorUtilityBlueprint::StaticClass());
    if (!Conflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(Conflict);
    }

    // Idempotent: an existing Editor Utility Blueprint is returned as it is, with its own parent class and nothing
    // compiled or saved.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UEditorUtilityBlueprint* Existing = Cast<UEditorUtilityBlueprint>(UEditorAssetLibrary::LoadAsset(FullPath));
        if (!Existing)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("an asset already exists at %s but does not load as an Editor Utility Blueprint; nothing was created"),
                *FullPath));
        }
        TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, Existing, FString());
        Resp->SetBoolField(TEXT("created"), false);
        Resp->SetBoolField(TEXT("already_existed"), true);
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
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
// MCP:create_editor_utility_widget_blueprint - Create an Editor Utility Widget Blueprint at package_path/name with the editor factory, parented to UEditorUtilityWidget, give it a CanvasPanel root named RootCanvas when the factory made no root, then compile and save it. Idempotent by path: an Editor Utility Widget Blueprint already there is returned as it is (created=false, already_existed=true, saved=false), nothing compiled or saved, with the parent_class and root it really has. Another kind of asset at that path, or an invalid path, is an error and nothing is created. A failed save makes success false with save_error and keeps the new asset in memory. Refused during PIE unless force=true. Open it as a tab with run_editor_utility_widget; find them with list_editor_utility_widgets; a plain Widget Blueprint is create_widget_blueprint.
//   @param package_path - Content folder for the asset, e.g. "/Game/EditorUtilities"; a trailing slash is ignored.
//   @param name - Asset name without path or extension, e.g. "EUW_LevelTools".
//   @returns { name, package_path, asset_path, blueprint_class, parent_class, root_widget, root_widget_class, created, already_existed, root_created, saved, save_error? }
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

    // An asset of another kind at that path is an error before anything is created: the factory would
    // otherwise meet it in CreateAsset, which asks to overwrite it.
    const FString Conflict = mcp_common::AssetNameConflict(FullPath, Name, UEditorUtilityWidgetBlueprint::StaticClass());
    if (!Conflict.IsEmpty())
    {
        return mcp_common::MakeErrorReply(Conflict);
    }

    // Idempotent: an existing Editor Utility Widget Blueprint is returned as it is, with its own parent class and
    // nothing compiled or saved.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UEditorUtilityWidgetBlueprint* Existing = Cast<UEditorUtilityWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(FullPath));
        if (!Existing)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("an asset already exists at %s but does not load as an Editor Utility Widget Blueprint; nothing was created"),
                *FullPath));
        }
        const UWidget* Root = Existing->WidgetTree ? Existing->WidgetTree->RootWidget : nullptr;
        TSharedPtr<FJsonObject> Resp = BlueprintReply_EditorUtility(Name, PackagePath, Existing, FString());
        Resp->SetStringField(TEXT("root_widget"), Root ? Root->GetName() : TEXT(""));
        Resp->SetStringField(TEXT("root_widget_class"), Root ? Root->GetClass()->GetName() : TEXT(""));
        Resp->SetBoolField(TEXT("created"), false);
        Resp->SetBoolField(TEXT("already_existed"), true);
        Resp->SetBoolField(TEXT("root_created"), false);
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
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
// MCP:run_editor_utility_widget - Open an Editor Utility Widget Blueprint (by asset path) as an editor tab through UEditorUtilitySubsystem: register its tab spawner and invoke the tab, which creates the widget and so runs whatever its Blueprint does on construct. The tab ID is the asset path plus _ActiveTab, so a repeat call brings back the same tab instead of adding a second one. Nothing is saved. widget_spawned is false when no live widget came back; in a commandlet nothing is registered and tab_id is empty. A missing asset, or one that is not an Editor Utility Widget Blueprint, is an error. Create one with create_editor_utility_widget_blueprint; find them with list_editor_utility_widgets; right-click actions: run_asset_action_utility.
//   @param widget_blueprint_path - Asset path of the Editor Utility Widget Blueprint, e.g. "/Game/EditorTools/EUW_Tools" (the object path form also works).
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
// MCP:list_editor_utility_widgets - List the Editor Utility Widget Blueprint assets under a content folder and its subfolders, read from the Asset Registry without loading them. Returns search_path, count and widgets [{name, asset_path, package_name, package_path}]; open one as an editor tab with run_editor_utility_widget.
//   @param path - Content folder to search recursively, without a trailing slash, e.g. "/Game/EditorUtilities" (default /Game).
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

// ---------------------------------------------------------------------------
// list_scripted_action_utilities
// ---------------------------------------------------------------------------
// Enumerate Editor Utility Blueprints whose generated/parent class is a child of
// UAssetActionUtility or UActorActionUtility. We resolve each candidate's
// generated class (loading the BP if necessary) and classify it.
// MCP:list_scripted_action_utilities - List the Editor Utility Blueprints under a content folder (subfolders included) whose class derives from UAssetActionUtility (kind asset: Content Browser right-click actions) or UActorActionUtility (kind actor: level actor actions). Each Editor Utility Blueprint found is LOADED to test its class, so a large folder is slow; plain Blueprints and Editor Utility Widgets are not listed. Read-only; a folder with none, or one that does not exist, is an empty list, not an error. Run one with run_asset_action_utility, which also lists its CallInEditor actions; create one with create_editor_utility_blueprint (parent_class "AssetActionUtility" or "ActorActionUtility"); Editor Utility Widgets: list_editor_utility_widgets.
//   @param path - Content folder to search, subfolders included, e.g. "/Game/EditorTools", without a trailing slash (default /Game).
//   @returns { search_path: string, count: int, utilities: [{ name: string, path: string, package_name: string, kind: string, parent_class: string }] }
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
// ProcessEvent it on a transient instance of the utility class. Functions that depend on a
// content-browser / level selection (which the engine surfaces only through the
// interactive right-click menu) are reported with an honest invoked=false note.
// MCP:run_asset_action_utility - Invoke one CallInEditor action of a scripted Asset or Actor Action Utility Blueprint (by asset path) on a new transient instance of its class, as the right-click menu does but with no Content Browser or level selection. function_name picks the action, case-insensitive, else the first one found; the reply always lists every action in action_functions. Only a parameterless action runs: one that takes parameters, a name that matches no action, a utility with no actions or an abstract utility class is a success with invoked=false and a note saying why. What the action does is up to its Blueprint; this tool saves nothing itself. A missing asset, one that is not a Blueprint, or a Blueprint that is not an Asset/Actor Action Utility is an error. Find them with list_scripted_action_utilities; create one with create_editor_utility_blueprint (parent_class "AssetActionUtility" or "ActorActionUtility").
//   @param utility_path - Asset path of the utility Blueprint, e.g. "/Game/EditorTools/EUA_RenameTextures" (the object path form also works).
//   @param function_name - Name of the CallInEditor action to run, case-insensitive; when absent the first action found on the class runs.
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

    // Invoke the parameterless CallInEditor action for real, as the editor's scripted-action menu does
    // (FBlutilityMenuExtensions): on a new transient instance of the utility class, never on its class default
    // object, rooted while it runs because an action may collect garbage, and under an FEditorScriptExecutionGuard.
    // The action typically reads the editor selection (empty headless) and operates on it -- invoking it is real
    // even when the selection is empty. An abstract class cannot be instantiated (NewObject would assert), so its
    // action is not run.
    if (GenClass->HasAnyClassFlags(CLASS_Abstract))
    {
        Resp->SetBoolField(TEXT("invoked"), false);
        Resp->SetStringField(TEXT("note"),
            TEXT("The utility class is abstract, so there is no instance to run the scripted action on"));
        return mcp_common::MakeOkReply(Resp);
    }

    UObject* Instance = NewObject<UObject>(GetTransientPackage(), GenClass);
    Instance->AddToRoot();
    {
        FEditorScriptExecutionGuard ScriptGuard;
        Instance->ProcessEvent(TargetFunc, nullptr);
    }
    Instance->RemoveFromRoot();

    Resp->SetBoolField(TEXT("invoked"), true);
    Resp->SetStringField(TEXT("note"),
        TEXT("Invoked parameterless CallInEditor action headlessly (no content-browser/level selection was present)"));
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_editor_utility_blueprint"), &HandleCreateEditorUtilityBlueprint },
    { TEXT("create_editor_utility_widget_blueprint"), &HandleCreateEditorUtilityWidgetBlueprint },
    { TEXT("list_editor_utility_widgets"), &HandleListEditorUtilityWidgets },
    { TEXT("list_scripted_action_utilities"), &HandleListScriptedActionUtilities },
    { TEXT("run_asset_action_utility"), &HandleRunAssetActionUtility },
    { TEXT("run_editor_utility_widget"), &HandleRunEditorUtilityWidget },
};
} // namespace UEAgentEditorUtilityCommands
