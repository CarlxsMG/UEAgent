#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/UI/UMGWidgetUtils.h"
#include "EngineUtils.h"        // TActorIterator (set_viewmodel_field world walks)

// Asset machinery
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "Misc/PackageName.h"

// Widget BP + tree + widget types
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

// WidgetBlueprintExtension (editor-only) -- gives us the MVVM BP-side state.
#include "WidgetBlueprintExtension.h"

// UMG list/tile/tree views.
#include "Components/ListView.h"
#include "Components/ListViewBase.h"
#include "Components/TileView.h"
#include "Components/TreeView.h"
#include "Components/WidgetComponent.h"   // UWidgetComponent (set_viewmodel_field world walk)

// MVVM runtime + BP-side authoring.
#include "MVVMViewModelBase.h"
#include "Types/MVVMViewModelContext.h"
#include "Types/MVVMFieldVariant.h"
#include "View/MVVMView.h"
#include "View/MVVMViewClass.h"   // UMVVMViewClass, FMVVMViewClass_Source (set_viewmodel_field read-back)
#include "Extensions/UserWidgetExtension.h"
#include "MVVMWidgetBlueprintExtension_View.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMPropertyPath.h"
// UMVVMEditorSubsystem::SetSourcePathForBinding/SetDestinationPathForBinding -- the supported
// way to write FMVVMBlueprintPropertyPath onto a binding (bind_widget_to_viewmodel).
#include "MVVMEditorSubsystem.h"

// CommonUI -- UCommonActivatableWidgetStack.
#include "Widgets/CommonActivatableWidgetContainer.h"
#include "CommonActivatableWidget.h"
#include "CommonUserWidget.h"

// Drag-drop base (Payload UPROPERTY exists).
#include "Blueprint/DragDropOperation.h"

// FScopedTransaction (editor-time transactional edits).
#include "ScopedTransaction.h"

// GEditor (UMVVMEditorSubsystem lookup) + PIE world walk (set_pie_list_view_items).
#include "Editor.h"
#include "Engine/LocalPlayer.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"  // TObjectIterator<UUserWidget> (same as click_pie_widget)

namespace UEAgentUMGMVVMCommands
{

// MCP-DOMAIN: UI
// MVVM + ListView + TileView + DragDrop + CommonUI

namespace
{
    // The widget_blueprint_name the authoring handlers require: the error reply when it or Params is missing.
    TSharedPtr<FJsonObject> ReadWidgetBlueprintName_MVVM(const TSharedPtr<FJsonObject>& Params, FString& OutName)
    {
        if (!Params.IsValid())
        {
            return mcp_common::MakeErrorReply(TEXT("Missing params"));
        }
        if (!Params->TryGetStringField(TEXT("widget_blueprint_name"), OutName) || OutName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TEXT("widget_blueprint_name is required"));
        }
        return nullptr;
    }

    // A reply opened with its success flag and the widget_blueprint_name the command was given.
    TSharedPtr<FJsonObject> StartReply_MVVM(bool bSuccess, const FString& WidgetBPName)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetBoolField(TEXT("success"), bSuccess);
        Out->SetStringField(TEXT("widget_blueprint_name"), WidgetBPName);
        return Out;
    }

    // The note of the reply to a Widget Blueprint without MVVM authoring support.
    const TCHAR* const NoMvvmSupportNote_MVVM =
        TEXT("Widget does not have MVVM panel support -- enable 'Supports Extensions' on the widget BP class settings.");

    // Save the widget BP package to disk and recompile the generated class.
    bool SaveWidgetBlueprint(UWidgetBlueprint* WidgetBP, FString& OutError)
    {
        if (!WidgetBP)
        {
            OutError = TEXT("Invalid Widget Blueprint");
            return false;
        }
        FKismetEditorUtilities::CompileBlueprint(WidgetBP, EBlueprintCompileOptions::SkipGarbageCollection);
        return mcp_common::SaveAssetPackage(WidgetBP->GetOutermost(), WidgetBP, OutError);
    }

    // -------------------------------------------------------------------------
    // Load a UClass by either an asset path (/Game/.../MyWidget_C) or a
    // qualified script path (/Script/ModuleName.ClassName).
    // -------------------------------------------------------------------------
    UClass* LoadWidgetClass(const FString& InClassRef, bool bRequireUserWidget, FString& OutError)
    {
        if (InClassRef.IsEmpty())
        {
            OutError = TEXT("class reference cannot be empty");
            return nullptr;
        }

        UClass* Found = nullptr;

        // 1. Asset path: /Game/Path/Asset.Asset
        if (InClassRef.StartsWith(TEXT("/")))
        {
            // Try as a Blueprint asset first.
            if (UEditorAssetLibrary::DoesAssetExist(InClassRef))
            {
                UObject* Asset = UEditorAssetLibrary::LoadAsset(InClassRef);
                if (UBlueprint* BP = Cast<UBlueprint>(Asset))
                {
                    Found = BP->GeneratedClass;
                }
            }
            if (!Found)
            {
                Found = LoadObject<UClass>(nullptr, *InClassRef);
            }
        }
        else
        {
            // 2. Qualified script path: /Script/<Module>.<ClassName>.
            Found = LoadObject<UClass>(nullptr, *InClassRef);
            if (!Found)
            {
                Found = FindObject<UClass>(nullptr, *InClassRef);
            }
        }

        if (!Found)
        {
            OutError = FString::Printf(TEXT("Class not found: %s"), *InClassRef);
            return nullptr;
        }
        if (bRequireUserWidget && !Found->IsChildOf(UUserWidget::StaticClass()))
        {
            OutError = FString::Printf(TEXT("Class %s is not a UUserWidget"), *Found->GetName());
            return nullptr;
        }
        return Found;
    }

    // The UMVVMViewModelBase subclass InClassRef names, or null with OutError.
    UClass* LoadViewModelClass_MVVM(const FString& InClassRef, FString& OutError)
    {
        UClass* Class = LoadWidgetClass(InClassRef, /*bRequireUserWidget=*/false, OutError);
        if (Class && !Class->IsChildOf(UMVVMViewModelBase::StaticClass()))
        {
            OutError = FString::Printf(TEXT("Class %s is not a UMVVMViewModelBase subclass"), *Class->GetName());
            return nullptr;
        }
        return Class;
    }

    /**
     * Why UMVVMView::SetViewModelByClass gave an instance of VMClass to no context of ViewClass, read the way the
     * engine picks the context: the one source whose class VMClass is a child of, which has to be settable.
     */
    FString ViewModelNotSetReason_MVVM(const UMVVMViewClass* ViewClass, const UClass* VMClass)
    {
        TArray<const FMVVMViewClass_Source*> Matches;
        for (const FMVVMViewClass_Source& ClassSource : ViewClass->GetSources())
        {
            if (VMClass->IsChildOf(ClassSource.GetSourceClass()))
            {
                Matches.Add(&ClassSource);
            }
        }
        if (Matches.Num() == 0)
        {
            return FString::Printf(
                TEXT("set_viewmodel_field: the MVVM view of the widget has no view-model context of class %s or of a parent ")
                TEXT("class, so nothing was assigned; add one with add_viewmodel"), *VMClass->GetName());
        }
        if (Matches.Num() > 1)
        {
            TArray<FString> Names;
            for (const FMVVMViewClass_Source* Match : Matches)
            {
                Names.Add(Match->GetName().ToString());
            }
            return FString::Printf(
                TEXT("set_viewmodel_field: %d view-model contexts (%s) match class %s, and the engine assigns by class only ")
                TEXT("when exactly one does, so nothing was assigned"),
                Matches.Num(), *FString::Join(Names, TEXT(", ")), *VMClass->GetName());
        }
        if (!Matches[0]->CanBeSet())
        {
            return FString::Printf(
                TEXT("set_viewmodel_field: view-model context %s cannot be set at runtime (only a Manual context, or one ")
                TEXT("with a setter function, can), so nothing was assigned"), *Matches[0]->GetName().ToString());
        }
        return FString::Printf(
            TEXT("set_viewmodel_field: the engine assigned the instance to no context, although context %s matches its ")
            TEXT("class; the MVVM message log says why"), *Matches[0]->GetName().ToString());
    }

    // Find a widget by name anywhere in a Widget BP's design-time WidgetTree (bind_widget_to_viewmodel
    // destination resolution, add_tree_view parent_widget_name resolution).
    UWidget* FindWidgetInTreeByName(UWidgetTree* Tree, const FName& Name)
    {
        UWidget* Found = nullptr;
        if (Tree)
        {
            Tree->ForEachWidget([&](UWidget* W)
            {
                if (W && !Found && W->GetFName() == Name)
                {
                    Found = W;
                }
            });
        }
        return Found;
    }

    // WidgetBP's widget tree, or null with OutError when it has none.
    UWidgetTree* GetWidgetTree_MVVM(UWidgetBlueprint* WidgetBP, FString& OutError)
    {
        if (!WidgetBP->WidgetTree)
        {
            OutError = TEXT("Widget BP has no WidgetTree");
        }
        return WidgetBP->WidgetTree;
    }

    /**
     * What add_list_view / add_tile_view / add_tree_view resolve before they edit: the Widget Blueprint and its tree,
     * the widget that already holds the view's name (of any class; null when none does), and the entry widget class
     * (null when none is given).
     */
    struct FListViewTarget_MVVM
    {
        UWidgetBlueprint* WidgetBP = nullptr;
        UWidgetTree*      Tree = nullptr;
        FName             ViewName;
        bool              bAlreadyExisted = false;
        UWidget*          ExistingWidget = nullptr;
        UClass*           EntryClass = nullptr;
    };

    /** Out for the view ViewName in WidgetBPName: the error reply when the blueprint, its tree or the entry class cannot be found. */
    TSharedPtr<FJsonObject> ResolveListViewTarget_MVVM(const FString& WidgetBPName, const FString& ViewName,
                                                       const FString& EntryClassRef, FListViewTarget_MVVM& Out)
    {
        FString LoadErr;
        Out.WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
        Out.Tree = Out.WidgetBP ? GetWidgetTree_MVVM(Out.WidgetBP, LoadErr) : nullptr;
        if (!Out.Tree)
        {
            return mcp_common::MakeErrorReply(LoadErr);
        }

        // Idempotent dedup by widget name.
        Out.ViewName = FName(*ViewName);
        Out.ExistingWidget = FindWidgetInTreeByName(Out.Tree, Out.ViewName);
        Out.bAlreadyExisted = Out.ExistingWidget != nullptr;

        if (!EntryClassRef.IsEmpty())
        {
            Out.EntryClass = LoadWidgetClass(EntryClassRef, /*bRequireUserWidget=*/true, LoadErr);
            if (!Out.EntryClass)
            {
                return mcp_common::MakeErrorReply(LoadErr);
            }
        }
        return nullptr;
    }

    /**
     * Why add_list_view / add_tile_view / add_tree_view cannot place a new view in Tree, or empty when they can: it
     * goes under ParentPanel when one is given, else under the root panel, or becomes the root of an empty tree. A
     * root that is not a panel cannot take a child, and making the view the root instead would orphan that root and
     * its subtree; a panel that takes a single child and already has one cannot take another.
     */
    FString ListViewHostError_MVVM(const UWidgetTree* Tree, const UPanelWidget* ParentPanel)
    {
        const UPanelWidget* Host = ParentPanel;
        if (!Host)
        {
            const UWidget* Root = Tree->RootWidget;
            if (!Root)
            {
                return FString();
            }
            Host = Cast<UPanelWidget>(Root);
            if (!Host)
            {
                return FString::Printf(
                    TEXT("the root widget '%s' is a %s, not a panel, so it cannot hold the new view; make a panel the root ")
                    TEXT("first (add_tree_view can also take a panel as parent_widget_name). Nothing was added."),
                    *Root->GetName(), *Root->GetClass()->GetName());
            }
        }
        if (!Host->CanAddMoreChildren())
        {
            return FString::Printf(
                TEXT("panel '%s' (%s) takes a single child and already has one, so it cannot hold the new view. Nothing was added."),
                *Host->GetName(), *Host->GetClass()->GetName());
        }
        return FString();
    }

    // add_list_view / add_tile_view / add_tree_view: constructs a ViewT (UListView or a subclass) named WidgetName
    // in Tree, sets its EntryWidgetClass to EntryClass when given, and adds it under ParentPanel -- without one,
    // under the root canvas or panel, or as the root of an empty tree. Call ListViewHostError_MVVM first: null,
    // with nothing constructed, when there is no such place; null also when Tree cannot construct the view.
    template <typename ViewT>
    ViewT* AddListTypeView_MVVM(UWidgetTree* Tree, const FName& WidgetName, UClass* EntryClass, UPanelWidget* ParentPanel)
    {
        if (!ListViewHostError_MVVM(Tree, ParentPanel).IsEmpty())
        {
            return nullptr;
        }

        ViewT* NewView = Tree->ConstructWidget<ViewT>(ViewT::StaticClass(), WidgetName);
        if (!NewView)
        {
            return nullptr;
        }

        if (EntryClass)
        {
            // UListViewBase::EntryWidgetClass is the TSubclassOf<UUserWidget> for entries.
            FProperty* EntryProp = NewView->GetClass()->FindPropertyByName(TEXT("EntryWidgetClass"));
            if (FClassProperty* AsClassProp = CastField<FClassProperty>(EntryProp))
            {
                AsClassProp->SetPropertyValue_InContainer(NewView, EntryClass);
            }
        }

        if (ParentPanel)
        {
            ParentPanel->AddChild(NewView);
        }
        else
        {
            UWidget* Root = Tree->RootWidget;
            if (UCanvasPanel* Canvas = Cast<UCanvasPanel>(Root))
            {
                Canvas->AddChildToCanvas(NewView);
            }
            else if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
            {
                Panel->AddChild(NewView);
            }
            else
            {
                // An empty tree (ListViewHostError_MVVM refuses a root that is not a panel): become the root.
                Tree->RootWidget = NewView;
            }
        }
        return NewView;
    }

    /**
     * The class field ClassField and entry_widget_class of the add_list_view / add_tile_view / add_tree_view reply,
     * read off View, the widget that holds the requested name: the view just added, or the widget that already held
     * that name, whatever its class. The class is spelled as in C++ (UListView, UTileView, UTreeView); a widget that
     * is not a list-type view has no entry class, so that field is empty.
     */
    void AddViewClassFields_MVVM(const TSharedPtr<FJsonObject>& Out, const TCHAR* ClassField, const UWidget* View)
    {
        const UClass* ViewClass = View->GetClass();
        const UListViewBase* AsListView = Cast<UListViewBase>(View);
        const UClass* EntryClass = AsListView ? AsListView->GetEntryWidgetClass().Get() : nullptr;
        Out->SetStringField(ClassField, FString(ViewClass->GetPrefixCPP()) + ViewClass->GetName());
        Out->SetStringField(TEXT("entry_widget_class"), EntryClass ? EntryClass->GetPathName() : FString());
    }

    // Parse a "Context.Property" string into its two segments. Multi-segment nested sub-paths
    // ("A.B.C", e.g. a struct sub-field) are an HONEST, documented non-goal of
    // bind_widget_to_viewmodel: resolving those needs a chain of FStructProperty walks into
    // FMVVMBlueprintFieldPath, which is materially riskier to get right than a single reflected
    // FProperty lookup. A verified single-segment implementation beats an
    // unverified multi-segment one -- callers get a clear error instead of a silently-wrong path.
    bool ParseTwoSegmentPath(const FString& InPath, FString& OutContext, FString& OutProperty, FString& OutError)
    {
        if (InPath.IsEmpty())
        {
            OutError = TEXT("path is empty");
            return false;
        }
        FString Context, Property;
        if (!InPath.Split(TEXT("."), &Context, &Property))
        {
            OutError = FString::Printf(TEXT("'%s' must be of the form 'Context.Property' (exactly one dot)"), *InPath);
            return false;
        }
        if (Context.IsEmpty() || Property.IsEmpty())
        {
            OutError = FString::Printf(TEXT("'%s' has an empty segment"), *InPath);
            return false;
        }
        if (Property.Contains(TEXT(".")))
        {
            OutError = FString::Printf(
                TEXT("'%s' has more than one dot ('%s.%s') -- multi-segment nested sub-paths are NOT supported by this tool; only 'Context.Property' (exactly one dot) resolves to a real property path"),
                *InPath, *Context, *Property);
            return false;
        }
        OutContext = Context;
        OutProperty = Property;
        return true;
    }

    // Map the string values accepted by add_viewmodel's creation_type param onto the real engine
    // enum (MVVMBlueprintViewModelContext.h). Only these 4 modes are exposed;
    // Resolver/Context exist in the engine enum but need extra authoring state this tool doesn't
    // set up, so they are deliberately left out rather than exposed half-working.
    bool ParseViewModelCreationType(const FString& InStr, EMVVMBlueprintViewModelContextCreationType& OutType, FString& OutError)
    {
        if (InStr.Equals(TEXT("CreateInstance"), ESearchCase::IgnoreCase))
        {
            OutType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
            return true;
        }
        if (InStr.Equals(TEXT("PropertyPath"), ESearchCase::IgnoreCase))
        {
            OutType = EMVVMBlueprintViewModelContextCreationType::PropertyPath;
            return true;
        }
        if (InStr.Equals(TEXT("GlobalViewModelCollection"), ESearchCase::IgnoreCase))
        {
            OutType = EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection;
            return true;
        }
        if (InStr.Equals(TEXT("Manual"), ESearchCase::IgnoreCase))
        {
            OutType = EMVVMBlueprintViewModelContextCreationType::Manual;
            return true;
        }
        OutError = FString::Printf(
            TEXT("creation_type '%s' is not one of: CreateInstance, PropertyPath, GlobalViewModelCollection, Manual"),
            *InStr);
        return false;
    }

    // -------------------------------------------------------------------------
    // set_pie_list_view_items helpers -- resolve a live widget of the PIE world.
    // -------------------------------------------------------------------------

    // Recursively search a resolved widget's panel-children subtree for a descendant whose
    // object name matches Name.
    UWidget* FindDescendantWidgetByName(UWidget* Root, const FName& Name)
    {
        if (!Root) { return nullptr; }
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
        {
            const int32 Count = Panel->GetChildrenCount();
            for (int32 i = 0; i < Count; ++i)
            {
                UWidget* Child = Panel->GetChildAt(i);
                if (!Child) { continue; }
                if (Child->GetFName() == Name) { return Child; }
                if (UWidget* Deep = FindDescendantWidgetByName(Child, Name)) { return Deep; }
            }
        }
        return nullptr;
    }

    // Resolve a LIVE UWidget by name among the active viewport UserWidgets of a PIE world:
    // top-level UserWidget by its own name, else a named widget in its live WidgetTree.
    // Optional player_index filter (>=0 enables it).
    UWidget* FindLivePieWidgetByName(UWorld* PieWorld, const FName& TargetName, int32 PlayerIndex)
    {
        for (TObjectIterator<UUserWidget> It; It; ++It)
        {
            UUserWidget* UserWidget = *It;
            if (!UMGWidgetUtils::IsLiveViewportWidget(UserWidget, PieWorld, PlayerIndex)) { continue; }

            if (UserWidget->GetFName() == TargetName)
            {
                return UserWidget;
            }
            if (UserWidget->WidgetTree)
            {
                if (UWidget* Found = UserWidget->WidgetTree->FindWidget(TargetName))
                {
                    return Found;
                }
            }
        }
        return nullptr;
    }

    /**
     * Resolves a LIVE CommonUI activatable container by name. Returns false with a useful
     * reason instead of a silent nullptr: the two possible failures -- no PIE, or the name is
     * not a stack -- call for different actions, so they are told apart.
     */
    bool ResolveLiveCommonUIStack(const TSharedPtr<FJsonObject>& Params, const FString& StackName,
                                  UCommonActivatableWidgetContainerBase*& OutStack, FString& OutError)
    {
        OutStack = nullptr;

        FString WorldError, WorldSelector;
        UWorld* PieWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), WorldSelector, WorldError);
        if (!PieWorld)
        {
            OutError = WorldError.IsEmpty()
                ? TEXT("no Play-In-Editor session is running: a CommonUI stack only holds widgets at "
                       "runtime, so there is nothing to push onto or pop from. Start PIE first.")
                : WorldError;
            return false;
        }

        int32 PlayerIndex = 0;
        Params->TryGetNumberField(TEXT("player_index"), PlayerIndex);

        UWidget* Found = FindLivePieWidgetByName(PieWorld, FName(*StackName), PlayerIndex);
        if (!Found)
        {
            OutError = FString::Printf(
                TEXT("widget '%s' not found among the active viewport widgets of the PIE world"), *StackName);
            return false;
        }

        OutStack = Cast<UCommonActivatableWidgetContainerBase>(Found);
        if (!OutStack)
        {
            OutError = FString::Printf(
                TEXT("'%s' is a %s, not a CommonUI activatable container. Only a stack or queue holds "
                     "pushable screens; create one with create_common_ui_stack."),
                *StackName, *Found->GetClass()->GetName());
            return false;
        }
        return true;
    }
}

// =============================================================================
// Ctor + dispatch

// =============================================================================
// add_viewmodel -- append a FMVVMBlueprintViewModelContext to the widget BP's
// MVVM authoring extension. Idempotent by ContextName.
// =============================================================================
// MCP:add_viewmodel - Add a view-model context named context_name, of class view_model_class, to the MVVM view of a Widget Blueprint loaded by asset path, then compile and save the Blueprint, in one undo step. creation_type says how the widget gets its instance at runtime: CreateInstance by default, PropertyPath from source_property_path, GlobalViewModelCollection by global_identifier, or Manual (supplied at runtime, e.g. by set_viewmodel_field). Idempotent by context_name: a context of that name already there is kept as it is, not updated, and comes back with already_existed=true, though the Blueprint is still compiled and saved. A Blueprint without MVVM support fails with a note; a bad class or creation_type is an error; a failed save keeps the edit in memory with saved=false and save_error. Refused during PIE unless force=true. Undo it with remove_viewmodel; bind fields with bind_widget_to_viewmodel.
//   @param widget_blueprint_name - /Game path of the Widget Blueprint asset, e.g. "/Game/UI/WBP_HUD".
//   @param view_model_class - View model class: asset path (/Game/...) or /Script/<Module>.<ClassName>
//   @param context_name - FName identifying this context (idempotency key)
//   @param creation_type - Optional (default "CreateInstance"): one of "CreateInstance" | "PropertyPath" | "GlobalViewModelCollection" | "Manual" (EMVVMBlueprintViewModelContextCreationType).
//   @param source_property_path - Optional: only used when creation_type="PropertyPath" (required in that case). Sets FMVVMBlueprintViewModelContext::ViewModelPropertyPath.
//   @param global_identifier - Optional: only used when creation_type="GlobalViewModelCollection" (required in that case). Sets FMVVMBlueprintViewModelContext::GlobalViewModelIdentifier.
//   @returns { widget_blueprint_name, view_model_class, context_name, creation_type, source_property_path?, global_identifier?, already_existed, view_model_count, has_mvvm_extension, saved, save_error?, note? }
TSharedPtr<FJsonObject> HandleAddViewModel(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }

    FString ViewModelClassRef;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("view_model_class"), ViewModelClassRef)) { return Missing; }

    FString ContextNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("context_name"), ContextNameStr)) { return Missing; }
    const FName ContextName(*ContextNameStr);

    // Optional creation_type (+ its mode-specific companion fields). Omitted -> "CreateInstance".
    FString CreationTypeStr = TEXT("CreateInstance");
    Params->TryGetStringField(TEXT("creation_type"), CreationTypeStr);
    EMVVMBlueprintViewModelContextCreationType CreationType = EMVVMBlueprintViewModelContextCreationType::CreateInstance;
    FString CreationTypeErr;
    if (!ParseViewModelCreationType(CreationTypeStr, CreationType, CreationTypeErr))
    {
        return mcp_common::MakeErrorReply(CreationTypeErr);
    }

    FString SourcePropertyPathStr;
    const bool bHasSourcePropertyPath =
        Params->TryGetStringField(TEXT("source_property_path"), SourcePropertyPathStr) && !SourcePropertyPathStr.IsEmpty();
    FString GlobalIdentifierStr;
    const bool bHasGlobalIdentifier =
        Params->TryGetStringField(TEXT("global_identifier"), GlobalIdentifierStr) && !GlobalIdentifierStr.IsEmpty();

    if (CreationType == EMVVMBlueprintViewModelContextCreationType::PropertyPath && !bHasSourcePropertyPath)
    {
        return mcp_common::MakeErrorReply(TEXT("creation_type=PropertyPath requires a non-empty source_property_path"));
    }
    if (CreationType == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection && !bHasGlobalIdentifier)
    {
        return mcp_common::MakeErrorReply(TEXT("creation_type=GlobalViewModelCollection requires a non-empty global_identifier"));
    }

    FString LoadErr;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    UClass* VMClass = LoadViewModelClass_MVVM(ViewModelClassRef, LoadErr);
    if (!VMClass)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    // Resolve the MVVM BP-side authoring extension. Returns nullptr if the widget
    // BP doesn't have MVVM panel support ("Supports Extensions" not enabled).
    UMVVMWidgetBlueprintExtension_View* MvvmExt = UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(WidgetBP);
    if (!MvvmExt)
    {
        TSharedPtr<FJsonObject> Out = StartReply_MVVM(false, WidgetBPName);
        Out->SetStringField(TEXT("view_model_class"), VMClass->GetPathName());
        Out->SetStringField(TEXT("context_name"), ContextName.ToString());
        Out->SetStringField(TEXT("note"), NoMvvmSupportNote_MVVM);
        return Out;
    }

    UMVVMBlueprintView* BlueprintView = MvvmExt->GetBlueprintView();
    if (!BlueprintView)
    {
        return mcp_common::MakeErrorReply(
            TEXT("MVVMWidgetBlueprintExtension_View has no BlueprintView (corrupted extension state)"));
    }

    // Idempotent check by ContextName.
    bool bAlreadyExisted = false;
    int32 ExistingCount = BlueprintView->GetViewModels().Num();
    for (const FMVVMBlueprintViewModelContext& Ctx : BlueprintView->GetViewModels())
    {
        if (Ctx.GetViewModelName() == ContextName)
        {
            bAlreadyExisted = true;
            break;
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_AddViewModel", "MCP: Add MVVM ViewModel Context"));
    WidgetBP->Modify();
    MvvmExt->Modify();
    BlueprintView->Modify();

    if (!bAlreadyExisted)
    {
        FMVVMBlueprintViewModelContext NewContext(VMClass, ContextName);
        NewContext.CreationType = CreationType;
        if (CreationType == EMVVMBlueprintViewModelContextCreationType::PropertyPath)
        {
            NewContext.ViewModelPropertyPath = SourcePropertyPathStr;
        }
        else if (CreationType == EMVVMBlueprintViewModelContextCreationType::GlobalViewModelCollection)
        {
            NewContext.GlobalViewModelIdentifier = FName(*GlobalIdentifierStr);
        }
        BlueprintView->AddViewModel(NewContext);
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_viewmodel: added %s (%s) to %s (creation_type=%s)"),
               *ContextName.ToString(), *VMClass->GetName(), *WidgetBPName, *CreationTypeStr);
    }

    int32 NewCount = BlueprintView->GetViewModels().Num();

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    Out->SetStringField(TEXT("view_model_class"), VMClass->GetPathName());
    Out->SetStringField(TEXT("context_name"), ContextName.ToString());
    Out->SetStringField(TEXT("creation_type"), CreationTypeStr);
    if (bHasSourcePropertyPath)
    {
        Out->SetStringField(TEXT("source_property_path"), SourcePropertyPathStr);
    }
    if (bHasGlobalIdentifier)
    {
        Out->SetStringField(TEXT("global_identifier"), GlobalIdentifierStr);
    }
    Out->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Out->SetNumberField(TEXT("view_model_count"), NewCount);
    Out->SetBoolField(TEXT("has_mvvm_extension"), true);
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// =============================================================================
// remove_viewmodel -- inverse of add_viewmodel. Removes an
// FMVVMBlueprintViewModelContext from the widget BP's MVVM authoring extension
// by FGuid. Idempotent: already-absent is an honest success, not an error.
// =============================================================================
// MCP:remove_viewmodel - Remove the view-model context named context_name from the MVVM view of a Widget Blueprint loaded by asset path, then compile and save the Blueprint in one undo step; view_model_count is what the view keeps. Idempotent: a name that matches no context, or a Blueprint with no MVVM view at all (has_mvvm_extension=false), is a success with removed=false, saved=false and a note, and nothing is changed or saved. A missing Blueprint is an error; a failed save keeps the removal in memory with saved=false and save_error. Refused during PIE unless force=true. Inverse: add_viewmodel; drop a single binding instead: unbind_widget_from_viewmodel.
//   @param widget_blueprint_name - /Game path of the Widget Blueprint asset (string)
//   @param context_name - FName identifying the context to remove (idempotency key)
//   @returns { widget_blueprint_name, context_name, already_existed, removed, view_model_count, has_mvvm_extension, saved, save_error?, note? }
TSharedPtr<FJsonObject> HandleRemoveViewModel(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }

    FString ContextNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("context_name"), ContextNameStr)) { return Missing; }
    const FName ContextName(*ContextNameStr);

    FString LoadErr;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    // Resolve the MVVM BP-side authoring extension. Returns nullptr if the widget
    // BP has no MVVM view. For a remove, that means there is no context to delete:
    // an already-absent success, with nothing modified or saved.
    UMVVMWidgetBlueprintExtension_View* MvvmExt = UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(WidgetBP);
    if (!MvvmExt)
    {
        TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
        Out->SetStringField(TEXT("context_name"), ContextName.ToString());
        Out->SetBoolField(TEXT("already_existed"), false);
        Out->SetBoolField(TEXT("removed"), false);
        Out->SetNumberField(TEXT("view_model_count"), 0);
        Out->SetBoolField(TEXT("has_mvvm_extension"), false);
        Out->SetBoolField(TEXT("saved"), false);
        Out->SetStringField(TEXT("note"),
            TEXT("The Widget Blueprint has no MVVM view, so it has no view-model context to remove; nothing was changed or saved."));
        return Out;
    }

    UMVVMBlueprintView* BlueprintView = MvvmExt->GetBlueprintView();
    if (!BlueprintView)
    {
        return mcp_common::MakeErrorReply(
            TEXT("MVVMWidgetBlueprintExtension_View has no BlueprintView (corrupted extension state)"));
    }

    // Look up the context by name. The API only removes by FGuid.
    const FMVVMBlueprintViewModelContext* Vm = BlueprintView->FindViewModel(ContextName);
    if (Vm == nullptr)
    {
        // Already-absent honest success -- no Remove, no save.
        TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
        Out->SetStringField(TEXT("context_name"), ContextName.ToString());
        Out->SetBoolField(TEXT("already_existed"), false);
        Out->SetBoolField(TEXT("removed"), false);
        Out->SetNumberField(TEXT("view_model_count"), BlueprintView->GetViewModels().Num());
        Out->SetBoolField(TEXT("has_mvvm_extension"), true);
        Out->SetBoolField(TEXT("saved"), false);
        Out->SetStringField(TEXT("note"), TEXT("No view-model context has that name; nothing was changed or saved."));
        return Out;
    }

    const FGuid Id = Vm->GetViewModelId();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_RemoveViewModel", "MCP: Remove MVVM ViewModel Context"));
    WidgetBP->Modify();
    MvvmExt->Modify();
    BlueprintView->Modify();

    const bool bRemoved = BlueprintView->RemoveViewModel(Id);
    UE_LOG(LogTemp, Verbose, TEXT("[MCP] remove_viewmodel: removed=%d %s (%s) from %s"),
           bRemoved ? 1 : 0, *ContextName.ToString(), *Id.ToString(), *WidgetBPName);

    const int32 NewCount = BlueprintView->GetViewModels().Num();

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    Out->SetStringField(TEXT("context_name"), ContextName.ToString());
    Out->SetBoolField(TEXT("already_existed"), true);
    Out->SetBoolField(TEXT("removed"), bRemoved);
    Out->SetNumberField(TEXT("view_model_count"), NewCount);
    Out->SetBoolField(TEXT("has_mvvm_extension"), true);
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// =============================================================================
// set_viewmodel_field -- runtime path. Finds an in-world UserWidget instance,
// instantiates the VM, gives it to the widget's UMVVMView::SetViewModelByClass and reads it back.
// =============================================================================
// MCP:set_viewmodel_field - Create a fresh instance of view_model_class and give it to a live UUserWidget through UMVVMView::SetViewModelByClass, which fills the view-model context of the widget whose class matches; context_name does not pick it. The widget is the one a UWidgetComponent shows: on the first actor whose label matches widget_instance_name, or, when the name contains a slash, on the first actor whose component uses that widget class, searched across every open world, PIE included. Runtime state only: nothing is compiled or saved, and each call swaps in a new instance. set and assigned_context are read back: if no context took it (none or several match, or it is not settable), success is false with the reason. A widget without an MVVM view, a class that is abstract or not a UMVVMViewModelBase, or no matching widget is an error. Authoring-time contexts: add_viewmodel; bindings: bind_widget_to_viewmodel.
//   @param widget_instance_name - Label of an actor that has a widget component, matched case-insensitively, or, when it contains a slash, the widget class its component uses, e.g. "/Game/UI/WBP_Nameplate".
//   @param view_model_class - VM class: asset path or /Script/<Module>.<ClassName>
//   @param context_name - Name of the view-model context meant; echoed back, since the context is chosen by the class of view_model_class (assigned_context says which took it).
//   @returns { widget_instance_name, view_model_class, context_name, set, assigned_context, instance_class, error?, note? }
TSharedPtr<FJsonObject> HandleSetViewModelField(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing params"));
    }

    FString InstanceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_instance_name"), InstanceName)) { return Missing; }

    FString VMClassRef;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("view_model_class"), VMClassRef)) { return Missing; }

    FString ContextNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("context_name"), ContextNameStr)) { return Missing; }

    FString LoadErr;
    UClass* VMRawClass = LoadViewModelClass_MVVM(VMClassRef, LoadErr);
    if (!VMRawClass)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }
    // NewObject of an abstract class trips an ensure in the editor (and a check outside it).
    if (VMRawClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("view_model_class %s is abstract, so it cannot be instantiated; give a concrete subclass"), *VMRawClass->GetName()));
    }

    // Resolve the in-world widget instance by actor label first, then by asset path.
    UUserWidget* Widget = nullptr;
    if (InstanceName.Contains(TEXT("/")))
    {
        // Treat as an asset reference (asset path of the class).
        UClass* InstanceClass = LoadWidgetClass(InstanceName, /*bRequireUserWidget=*/true, LoadErr);
        if (!InstanceClass)
        {
            return mcp_common::MakeErrorReply(LoadErr);
        }
        // Walk every world and find widgets whose GetClass() matches.
        if (GEngine)
        {
            for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
            {
                if (UWorld* W = Ctx.World())
                {
                    for (TActorIterator<AActor> It(W); It; ++It)
                    {
                        if (UWidgetComponent* WC = It->FindComponentByClass<UWidgetComponent>())
                        {
                            if (WC->GetWidgetClass() && WC->GetWidgetClass()->IsChildOf(InstanceClass))
                            {
                                if (UUserWidget* Inst = WC->GetUserWidgetObject())
                                {
                                    Widget = Inst;
                                    break;
                                }
                            }
                        }
                    }
                    if (Widget) { break; }
                }
            }
        }
    }
    else
    {
        // Treat as an Actor label.
        if (GEngine)
        {
            for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
            {
                if (UWorld* W = Ctx.World())
                {
                    for (TActorIterator<AActor> It(W); It; ++It)
                    {
                        if (It->GetActorLabel().Equals(InstanceName, ESearchCase::IgnoreCase))
                        {
                            if (UWidgetComponent* WC = It->FindComponentByClass<UWidgetComponent>())
                            {
                                if (UUserWidget* Inst = WC->GetUserWidgetObject())
                                {
                                    Widget = Inst;
                                    break;
                                }
                            }
                        }
                    }
                    if (Widget) { break; }
                }
            }
        }
    }

    if (!Widget)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No in-world UUserWidget instance found for '%s' (no actor label / widget-component class match)"), *InstanceName));
    }

    // UMVVMBlueprintLibrary::SetViewModelByClass returns nothing and only logs when the widget has no MVVM view, so
    // the view is required up front, its own SetViewModelByClass is called and the assignment is read back from it.
    UMVVMView* View = Widget->GetExtension<UMVVMView>();
    const UMVVMViewClass* ViewClass = View ? View->GetViewClass() : nullptr;
    if (!ViewClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("the %s widget found for '%s' has no MVVM view, so it has no view-model context to fill; give its Widget ")
            TEXT("Blueprint one with add_viewmodel. Nothing was changed."),
            *Widget->GetClass()->GetName(), *InstanceName));
    }

    UMVVMViewModelBase* VMInstance = NewObject<UMVVMViewModelBase>(Widget, VMRawClass);
    if (!VMInstance)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to instantiate view model"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_SetViewModelField", "MCP: Set MVVM ViewModel Instance"));
    Widget->Modify();

    View->SetViewModelByClass(VMInstance);

    // Read back: the context of the view that holds the new instance now.
    FName AssignedContext;
    for (const FMVVMView_Source& Source : View->GetSources())
    {
        if (Source.Source == VMInstance)
        {
            AssignedContext = ViewClass->GetSource(Source.ClassKey).GetName();
            break;
        }
    }
    const bool bSet = !AssignedContext.IsNone();

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("success"), bSet);
    Out->SetStringField(TEXT("widget_instance_name"), InstanceName);
    Out->SetStringField(TEXT("view_model_class"), VMRawClass->GetPathName());
    Out->SetStringField(TEXT("context_name"), ContextNameStr);
    Out->SetBoolField(TEXT("set"), bSet);
    Out->SetStringField(TEXT("assigned_context"), bSet ? AssignedContext.ToString() : FString());
    Out->SetStringField(TEXT("instance_class"), VMInstance->GetClass()->GetName());

    if (!bSet)
    {
        // Nothing was assigned, so the call leaves no undo step.
        mcp_common::DiscardUndoStepOfThisCall();
        Out->SetStringField(TEXT("error"), ViewModelNotSetReason_MVVM(ViewClass, VMRawClass));
        return Out;
    }

    if (AssignedContext != FName(*ContextNameStr))
    {
        Out->SetStringField(TEXT("note"), FString::Printf(
            TEXT("the instance went to context %s, the one whose class matches; context_name %s does not choose it"),
            *AssignedContext.ToString(), *ContextNameStr));
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_viewmodel_field: bound %s (%s) to widget '%s'"),
           *AssignedContext.ToString(), *VMInstance->GetClass()->GetName(), *InstanceName);
    return Out;
}

// =============================================================================
// bind_widget_to_viewmodel -- append a FMVVMBlueprintViewBinding to the widget
// BP's MVVM authoring extension, with REAL SourcePath/DestinationPath.
//
//   A binding whose SourcePath/DestinationPath stay in their default-constructed
//   (empty/invalid) state is inert, so both paths are built and written for real.
//
//   How, per the engine source
//   (Engine/Plugins/Runtime/ModelViewViewModel/...):
//     - FMVVMBlueprintPropertyPath has no FString constructor. It is built field-by-field:
//       AppendPropertyPath(const UBlueprint*, UE::MVVM::FMVVMConstFieldVariant) appends a
//       reflected FProperty/UFunction, and SetWidgetName/SetViewModelId/SetSelfContext pick
//       the path's root. This EXACT sequence is what the MVVM editor UI itself does when you
//       drag-bind a field (see ModelViewViewModelEditor/Private/Customizations/
//       MVVMPropertyBindingExtension.cpp and Private/Framework/MVVMBindingEditorHelper.cpp).
//     - UMVVMEditorSubsystem::SetSourcePathForBinding/SetDestinationPathForBinding (not a raw
//       Binding.SourcePath = ... assignment) is the correct write path: it also tears down any
//       stale conversion-function wrapper graph and marks the BP modified, exactly like the
//       editor UI. bAllowEventConversion is passed false so a delegate-shaped destination is
//       never silently swapped into an Event (which would remove our Binding).
//
//   "ViewModel.Property" / "Widget.Property" -- exactly ONE dot -- is what resolves to a real
//   path. Deeper nested sub-paths ("Widget.Nested.SubField") are an honest, documented
//   non-goal (see ParseTwoSegmentPath): they'd need a chain of FStructProperty lookups this
//   tool does not attempt (a verified subset is preferred over a guessed general case).
// =============================================================================
// MCP:bind_widget_to_viewmodel - Append an FMVVMBlueprintViewBinding to a Widget BP's MVVM authoring state, with a REAL SourcePath/DestinationPath (resolved via reflection and written through UMVVMEditorSubsystem, then re-read to verify the write actually stuck).
//   @param widget_blueprint_name - /Game path of the Widget Blueprint asset
//   @param source_path - "ViewModel.Property" (exactly one dot): ViewModel = an existing viewmodel context name (added via add_viewmodel), Property = a reflected property on that viewmodel's class. Multi-segment ("A.B.C") is NOT supported.
//   @param destination_path - "Widget.Property" (exactly one dot): Widget = a widget name in this Widget BP's WidgetTree, Property = a reflected property on that widget's class. Multi-segment is NOT supported.
//   @param binding_mode - optional: "OneWay" (default), "TwoWay", "OneTime", "OneWayToSource"
//   @returns { widget_blueprint_name, source_path, destination_path, binding_mode, created, binding_count, binding_id, source_path_verified, destination_path_verified, has_mvvm_extension }
TSharedPtr<FJsonObject> HandleBindWidgetToViewModel(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }
    FString SourcePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_path"), SourcePath)) { return Missing; }
    FString DestinationPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("destination_path"), DestinationPath)) { return Missing; }
    FString BindingModeStr = TEXT("OneWay");
    Params->TryGetStringField(TEXT("binding_mode"), BindingModeStr);

    EMVVMBindingMode BindingMode = EMVVMBindingMode::OneWayToDestination;
    if (BindingModeStr.Equals(TEXT("TwoWay"), ESearchCase::IgnoreCase))
    {
        BindingMode = EMVVMBindingMode::TwoWay;
    }
    else if (BindingModeStr.Equals(TEXT("OneTime"), ESearchCase::IgnoreCase))
    {
        // Engine enum: OneTimeToDestination (the public-facing "OneTime").
        // OneTimeToSource is UMETA(Hidden) -- runtime-only.
        BindingMode = EMVVMBindingMode::OneTimeToDestination;
    }
    else if (BindingModeStr.Equals(TEXT("OneWayToSource"), ESearchCase::IgnoreCase))
    {
        BindingMode = EMVVMBindingMode::OneWayToSource;
    }
    // else leave as OneWayToDestination (the enum name UE uses for "OneWay").

    // Parse both dot-paths BEFORE touching any asset (fail fast on bad input).
    FString SourceContextName, SourcePropertyName, ParseErr;
    if (!ParseTwoSegmentPath(SourcePath, SourceContextName, SourcePropertyName, ParseErr))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("source_path: %s"), *ParseErr));
    }
    FString DestWidgetName, DestPropertyName;
    if (!ParseTwoSegmentPath(DestinationPath, DestWidgetName, DestPropertyName, ParseErr))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("destination_path: %s"), *ParseErr));
    }

    FString LoadErr;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    UMVVMWidgetBlueprintExtension_View* MvvmExt = UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(WidgetBP);
    if (!MvvmExt)
    {
        TSharedPtr<FJsonObject> Out = StartReply_MVVM(false, WidgetBPName);
        Out->SetStringField(TEXT("note"), NoMvvmSupportNote_MVVM);
        return Out;
    }

    UMVVMBlueprintView* BlueprintView = MvvmExt->GetBlueprintView();
    if (!BlueprintView)
    {
        return mcp_common::MakeErrorReply(
            TEXT("MVVMWidgetBlueprintExtension_View has no BlueprintView"));
    }

    // Resolve the SOURCE: "ViewModel.Property" against the widget BP's registered viewmodel
    // contexts (by context name), then a reflected FProperty on that viewmodel's class.
    const FMVVMBlueprintViewModelContext* VmCtx = BlueprintView->FindViewModel(FName(*SourceContextName));
    if (!VmCtx)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("source_path: no viewmodel context named '%s' on '%s' (add it first with add_viewmodel)"),
            *SourceContextName, *WidgetBPName));
    }
    UClass* VmClass = VmCtx->GetViewModelClass();
    if (!VmClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("source_path: viewmodel context '%s' has no resolved class"), *SourceContextName));
    }
    FProperty* SourceProperty = VmClass->FindPropertyByName(FName(*SourcePropertyName));
    if (!SourceProperty)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("source_path: property '%s' not found on viewmodel class %s"),
            *SourcePropertyName, *VmClass->GetName()));
    }

    // Resolve the DESTINATION: "Widget.Property" against the widget BP's WidgetTree (by widget
    // name), then a reflected FProperty on that widget's class.
    UWidgetTree* Tree = GetWidgetTree_MVVM(WidgetBP, LoadErr);
    if (!Tree)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }
    UWidget* DestWidget = FindWidgetInTreeByName(Tree, FName(*DestWidgetName));
    if (!DestWidget)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("destination_path: widget '%s' not found in '%s'"), *DestWidgetName, *WidgetBPName));
    }
    FProperty* DestProperty = DestWidget->GetClass()->FindPropertyByName(FName(*DestPropertyName));
    if (!DestProperty)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("destination_path: property '%s' not found on widget '%s' (class %s)"),
            *DestPropertyName, *DestWidgetName, *DestWidget->GetClass()->GetName()));
    }

    UMVVMEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
    if (!EditorSubsystem)
    {
        return mcp_common::MakeErrorReply(TEXT("UMVVMEditorSubsystem is not available (no GEditor?)"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_BindWidget", "MCP: Add MVVM Binding"));
    WidgetBP->Modify();
    MvvmExt->Modify();
    BlueprintView->Modify();

    FMVVMBlueprintViewBinding& Binding = BlueprintView->AddDefaultBinding();
    const FGuid BindingId = Binding.BindingId;

    // Build the real property paths -- same construction sequence the MVVM editor UI uses.
    FMVVMBlueprintPropertyPath SourcePropertyPath;
    SourcePropertyPath.SetViewModelId(VmCtx->GetViewModelId());
    SourcePropertyPath.AppendPropertyPath(WidgetBP, UE::MVVM::FMVVMConstFieldVariant(SourceProperty));

    FMVVMBlueprintPropertyPath DestinationPropertyPath;
    DestinationPropertyPath.AppendPropertyPath(WidgetBP, UE::MVVM::FMVVMConstFieldVariant(DestProperty));
    if (DestWidget->GetFName() == WidgetBP->GetFName())
    {
        DestinationPropertyPath.SetSelfContext();
    }
    else
    {
        DestinationPropertyPath.SetWidgetName(DestWidget->GetFName());
    }

    // Write through the subsystem (not raw field assignment): it also tears down any stale
    // conversion-function wrapper graph and marks the BP modified, like the editor UI does.
    EditorSubsystem->SetSourcePathForBinding(WidgetBP, Binding, SourcePropertyPath);
    EditorSubsystem->SetDestinationPathForBinding(WidgetBP, Binding, DestinationPropertyPath, /*bAllowEventConversion=*/false);

    Binding.BindingType = BindingMode;
    Binding.bEnabled = true;
    Binding.bCompile = true;

    const int32 BindingCount = BlueprintView->GetNumBindings();

    // Self-verification: re-read GetBindings() by BindingId and confirm SourcePath/
    // DestinationPath actually landed non-empty. Do NOT trust "it compiled" as a signal the
    // write took -- an inert binding with empty paths compiles too.
    bool bSourceWritten = false;
    bool bDestWritten = false;
    for (const FMVVMBlueprintViewBinding& B : BlueprintView->GetBindings())
    {
        if (B.BindingId == BindingId)
        {
            bSourceWritten = B.SourcePath.HasPaths() && (B.SourcePath.GetViewModelId() == VmCtx->GetViewModelId());
            bDestWritten = B.DestinationPath.HasPaths();
            break;
        }
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] bind_widget_to_viewmodel: %s -> %s (mode=%s) on %s (count=%d, verified src=%d dst=%d)"),
           *SourcePath, *DestinationPath, *BindingModeStr, *WidgetBPName, BindingCount, bSourceWritten ? 1 : 0, bDestWritten ? 1 : 0);

    if (!bSourceWritten || !bDestWritten)
    {
        // Honest failure: the write didn't stick. Roll back the half-built binding rather than
        // leaving a broken one registered (never a silent no-op, never a broken registered command).
        BlueprintView->RemoveBinding(&Binding);
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("bind_widget_to_viewmodel: post-write verification failed (source_written=%d, destination_written=%d) -- binding was rolled back, nothing was saved"),
            bSourceWritten ? 1 : 0, bDestWritten ? 1 : 0));
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    Out->SetStringField(TEXT("source_path"), SourcePath);
    Out->SetStringField(TEXT("destination_path"), DestinationPath);
    Out->SetStringField(TEXT("binding_mode"), BindingModeStr);
    Out->SetBoolField(TEXT("created"), true);
    Out->SetNumberField(TEXT("binding_count"), BindingCount);
    Out->SetStringField(TEXT("binding_id"), BindingId.ToString());
    Out->SetBoolField(TEXT("source_path_verified"), bSourceWritten);
    Out->SetBoolField(TEXT("destination_path_verified"), bDestWritten);
    Out->SetBoolField(TEXT("has_mvvm_extension"), true);
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// =============================================================================
// unbind_widget_from_viewmodel -- inverse of bind_widget_to_viewmodel. Removes
// an FMVVMBlueprintViewBinding from the widget BP's MVVM authoring extension.
// Identified by index (primary, always usable) or by binding GUID (optional).
// Idempotent: out-of-range / unknown-GUID is an honest already-absent success.
// =============================================================================
// MCP:unbind_widget_from_viewmodel - Remove an FMVVMBlueprintViewBinding from a Widget BP's MVVM authoring state, by index (primary) or binding GUID. Inverse of bind_widget_to_viewmodel; idempotent (already-absent is an honest success).
//   @param widget_blueprint_name - /Game path of the Widget Blueprint asset
//   @param binding_index - int index of the binding to remove (primary identifier; one of binding_index / binding_id required)
//   @param binding_id - optional GUID string of the binding to remove (alternative to binding_index)
//   @returns { widget_blueprint_name, binding_index?, binding_id?, already_existed, removed, binding_count, has_mvvm_extension, saved, save_error? }
TSharedPtr<FJsonObject> HandleUnbindWidgetFromViewModel(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }

    // Exactly one of binding_index (primary) / binding_id (optional) must be present.
    int32 BindingIndex = INDEX_NONE;
    const bool bHasIndex = Params->TryGetNumberField(TEXT("binding_index"), BindingIndex);
    FString BindingIdStr;
    const bool bHasId = Params->TryGetStringField(TEXT("binding_id"), BindingIdStr) && !BindingIdStr.IsEmpty();
    if (!bHasIndex && !bHasId)
    {
        return mcp_common::MakeErrorReply(
            TEXT("one of binding_index (int) or binding_id (GUID string) is required"));
    }

    FString LoadErr;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    // Resolve the MVVM BP-side authoring extension. Returns nullptr if the widget
    // BP doesn't have MVVM panel support ("Supports Extensions" not enabled). For an
    // unbind, that means there is nothing to delete -- honest already-absent success.
    UMVVMWidgetBlueprintExtension_View* MvvmExt = UWidgetBlueprintExtension::GetExtension<UMVVMWidgetBlueprintExtension_View>(WidgetBP);
    if (!MvvmExt)
    {
        TSharedPtr<FJsonObject> Out = StartReply_MVVM(false, WidgetBPName);
        Out->SetStringField(TEXT("note"), NoMvvmSupportNote_MVVM);
        return Out;
    }

    UMVVMBlueprintView* BlueprintView = MvvmExt->GetBlueprintView();
    if (!BlueprintView)
    {
        return mcp_common::MakeErrorReply(
            TEXT("MVVMWidgetBlueprintExtension_View has no BlueprintView"));
    }

    // Primary identifier: index. GUID is only consulted when no index was supplied.
    const bool bUseIndex = bHasIndex;

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_UnbindWidget", "MCP: Remove MVVM Binding"));
    WidgetBP->Modify();
    MvvmExt->Modify();
    BlueprintView->Modify();

    bool bAlreadyExisted = false;
    bool bRemoved = false;

    if (bUseIndex)
    {
        const int32 NumBindings = BlueprintView->GetNumBindings();
        if (BindingIndex >= 0 && BindingIndex < NumBindings)
        {
            bAlreadyExisted = true;
            BlueprintView->RemoveBindingAt(BindingIndex);
            bRemoved = true;
        }
        // else: out-of-range -> already-absent honest success (bAlreadyExisted/bRemoved stay false).
    }
    else
    {
        FGuid Id;
        if (!FGuid::Parse(BindingIdStr, Id))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("binding_id is not a valid GUID: %s"), *BindingIdStr));
        }
        FMVVMBlueprintViewBinding* B = BlueprintView->GetBinding(Id);
        if (B != nullptr)
        {
            bAlreadyExisted = true;
            BlueprintView->RemoveBinding(B);
            bRemoved = true;
        }
        // else: unknown GUID -> already-absent honest success.
    }

    const int32 BindingCount = BlueprintView->GetNumBindings();

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] unbind_widget_from_viewmodel: removed=%d (by %s) on %s (count=%d)"),
           bRemoved ? 1 : 0, bUseIndex ? TEXT("index") : TEXT("guid"), *WidgetBPName, BindingCount);

    // Only persist when we actually mutated the view.
    bool bSaved = false;
    FString SaveErr;
    if (bRemoved)
    {
        bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);
    }

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    if (bUseIndex)
    {
        Out->SetNumberField(TEXT("binding_index"), BindingIndex);
    }
    else
    {
        Out->SetStringField(TEXT("binding_id"), BindingIdStr);
    }
    Out->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Out->SetBoolField(TEXT("removed"), bRemoved);
    Out->SetNumberField(TEXT("binding_count"), BindingCount);
    Out->SetBoolField(TEXT("has_mvvm_extension"), true);
    Out->SetBoolField(TEXT("saved"), bSaved);
    if (bRemoved && !bSaved)
    {
        Out->SetStringField(TEXT("save_error"), SaveErr);
    }
    return Out;
}

// =============================================================================
// add_list_view -- adds a UListView to the widget BP's WidgetTree root.
// Idempotent by list_view_name.
// =============================================================================
// MCP:add_list_view - Construct a UListView named list_view_name in the tree of a Widget Blueprint loaded by asset path, with EntryWidgetClass from entry_widget_class when given, and add it to the root canvas or panel, then compile and save the Blueprint in one undo step. In an empty tree it becomes the root; a root that is not a panel, or a full single-child panel, is an error and nothing is added. Idempotent by name: a widget already called list_view_name, of any class, is left untouched (already_existed=true, widget_added=false; list_view_class and entry_widget_class are its own), the Blueprint still compiled and saved. A missing Blueprint, or an entry class that is not a UUserWidget, is an error; a failed save keeps the edit in memory with saved=false and save_error. Refused during PIE unless force=true. Tiles: add_tile_view; a tree: add_tree_view; fill it in PIE: set_pie_list_view_items.
//   @param widget_blueprint_name - /Game path of the Widget Blueprint
//   @param list_view_name - FName of the new UListView widget
//   @param entry_widget_class - Optional entry widget class (asset or /Script path)
//   @returns { widget_blueprint_name, list_view_name, list_view_class, already_existed, entry_widget_class, widget_added, saved, save_error? }
TSharedPtr<FJsonObject> HandleAddListView(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }
    FString ListViewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("list_view_name"), ListViewName)) { return Missing; }

    FString EntryClassRef;
    Params->TryGetStringField(TEXT("entry_widget_class"), EntryClassRef);

    FListViewTarget_MVVM View;
    if (TSharedPtr<FJsonObject> Failure = ResolveListViewTarget_MVVM(WidgetBPName, ListViewName, EntryClassRef, View))
    {
        return Failure;
    }
    if (!View.bAlreadyExisted)
    {
        const FString HostError = ListViewHostError_MVVM(View.Tree, nullptr);
        if (!HostError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(HostError);
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_AddListView", "MCP: Add ListView"));
    View.WidgetBP->Modify();
    View.Tree->Modify();

    // The widget the reply describes: the one already holding the name, left untouched, or the new view.
    UWidget* ReportedView = View.ExistingWidget;
    if (!ReportedView)
    {
        ReportedView = AddListTypeView_MVVM<UListView>(View.Tree, View.ViewName, View.EntryClass, nullptr);
        if (!ReportedView)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Tree->ConstructWidget<UListView> returned null"));
        }
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(View.WidgetBP, SaveErr);

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_list_view: %s on %s (already=%d)"),
           *ListViewName, *WidgetBPName, View.bAlreadyExisted ? 1 : 0);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    Out->SetStringField(TEXT("list_view_name"), ListViewName);
    AddViewClassFields_MVVM(Out, TEXT("list_view_class"), ReportedView);
    Out->SetBoolField(TEXT("already_existed"), View.bAlreadyExisted);
    Out->SetBoolField(TEXT("widget_added"), !View.bAlreadyExisted);
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// =============================================================================
// add_tile_view -- same shape as add_list_view but forces UTileView.
// =============================================================================
// MCP:add_tile_view - Construct a UTileView named list_view_name in the tree of a Widget Blueprint loaded by asset path, with its EntryWidgetClass set when entry_widget_class is given, and add it to the root canvas or panel, then compile and save the Blueprint in one undo step. In an empty tree it becomes the root; a root that is not a panel, or a full single-child panel, is an error and nothing is added. Idempotent by name: a widget already called list_view_name, of any class, is left untouched (already_existed=true, widget_added=false; list_view_class and entry_widget_class are its own), the Blueprint still compiled and saved. A missing Blueprint or a non-UUserWidget entry class is an error; a failed save keeps the edit in memory with saved=false and save_error. Refused during PIE unless force=true. A list: add_list_view; a tree: add_tree_view; fill it in PIE: set_pie_list_view_items.
//   @param widget_blueprint_name - /Game path of the Widget Blueprint, e.g. "/Game/UI/WBP_Inventory".
//   @param list_view_name - Name of the new UTileView widget in the tree, e.g. "ItemTiles"; also the idempotency key.
//   @param entry_widget_class - Optional UUserWidget class for each tile: an asset path such as "/Game/UI/WBP_ItemTile" or a /Script path.
//   @returns { widget_blueprint_name, list_view_name, list_view_class, already_existed, entry_widget_class, widget_added, saved, save_error? }
TSharedPtr<FJsonObject> HandleAddTileView(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }
    FString ListViewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("list_view_name"), ListViewName)) { return Missing; }

    FString EntryClassRef;
    Params->TryGetStringField(TEXT("entry_widget_class"), EntryClassRef);

    FListViewTarget_MVVM View;
    if (TSharedPtr<FJsonObject> Failure = ResolveListViewTarget_MVVM(WidgetBPName, ListViewName, EntryClassRef, View))
    {
        return Failure;
    }
    if (!View.bAlreadyExisted)
    {
        const FString HostError = ListViewHostError_MVVM(View.Tree, nullptr);
        if (!HostError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(HostError);
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_AddTileView", "MCP: Add TileView"));
    View.WidgetBP->Modify();
    View.Tree->Modify();

    // The widget the reply describes: the one already holding the name, left untouched, or the new view.
    UWidget* ReportedView = View.ExistingWidget;
    if (!ReportedView)
    {
        ReportedView = AddListTypeView_MVVM<UTileView>(View.Tree, View.ViewName, View.EntryClass, nullptr);
        if (!ReportedView)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Tree->ConstructWidget<UTileView> returned null"));
        }
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(View.WidgetBP, SaveErr);

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_tile_view: %s on %s (already=%d)"),
           *ListViewName, *WidgetBPName, View.bAlreadyExisted ? 1 : 0);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    Out->SetStringField(TEXT("list_view_name"), ListViewName);
    AddViewClassFields_MVVM(Out, TEXT("list_view_class"), ReportedView);
    Out->SetBoolField(TEXT("already_existed"), View.bAlreadyExisted);
    Out->SetBoolField(TEXT("widget_added"), !View.bAlreadyExisted);
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// =============================================================================
// set_widget_drag_drop -- ENGINE GAP HONESTLY DOCUMENTED.
//
// There is NO `DragDropOperationClass` UPROPERTY on UWidget, and the
// OnDragDetected/OnDrop dynamic delegates are DECLARE_DYNAMIC (cannot be
// assigned via reflection). The class is configured per-widget via the BP
// graph event node; this command returns the honest gap rather than
// fabricating a property that doesn't exist.
// =============================================================================
// MCP:set_widget_drag_drop - Set DragDropOperationClass on a widget found by name in the tree of a Widget Blueprint, then compile and save the Blueprint in one undo step (set=true, changed=true, property_set, saved). A widget already holding that class is left as it is: changed=false, saved=false and a note, nothing compiled or saved. Among the engine UMG widgets only list, tile and tree views have that property (UListViewBase); any other widget gets nothing written and set=false with a note, because drag and drop there is the OnDragDetected and OnDrop events of a User Widget graph: author those with build_blueprint_graph, after list_overridable_functions. operation_class defaults to /Script/UMG.DragDropOperation; one that does not load or is not a UDragDropOperation subclass is an error, and so is a missing Blueprint or widget: nothing is written. Refused during PIE unless force=true. Make the view with add_list_view, add_tile_view or add_tree_view.
//   @param widget_blueprint_name - /Game path of the Widget Blueprint, e.g. "/Game/UI/WBP_Inventory".
//   @param widget_name - Name of the widget in the tree of that Blueprint, e.g. "ItemTiles"; only a list, tile or tree view can take the class.
//   @param operation_class - Class path of a UDragDropOperation subclass, e.g. "/Script/UMG.DragDropOperation" or "/Game/UI/DDO_Item.DDO_Item_C"; defaults to /Script/UMG.DragDropOperation.
//   @returns { widget_blueprint_name, widget_name, operation_class, set, changed, has_drag_drop_property, operation_class_loaded, property_set?, saved?, save_error?, note? }
TSharedPtr<FJsonObject> HandleSetWidgetDragDrop(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    FString OperationClassRef = TEXT("/Script/UMG.DragDropOperation");
    Params->TryGetStringField(TEXT("operation_class"), OperationClassRef);

    // The class is checked before anything is looked up or written: a TSubclassOf<UDragDropOperation> must never
    // hold a class that is not a UDragDropOperation, and a class that does not load is a bad request, not a gap.
    UClass* OpClass = OperationClassRef.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *OperationClassRef);
    if (!OpClass && !OperationClassRef.IsEmpty())
    {
        OpClass = FindObject<UClass>(nullptr, *OperationClassRef);
    }
    if (!OpClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("operation_class '%s' does not load as a class; give a class path such as /Script/UMG.DragDropOperation ")
            TEXT("or /Game/UI/DDO_Item.DDO_Item_C. Nothing was changed."), *OperationClassRef));
    }
    if (!OpClass->IsChildOf(UDragDropOperation::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("operation_class '%s' is %s, which is not a UDragDropOperation subclass. Nothing was changed."),
            *OperationClassRef, *OpClass->GetPathName()));
    }

    FString LoadErr;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
    UWidgetTree* Tree = WidgetBP ? GetWidgetTree_MVVM(WidgetBP, LoadErr) : nullptr;
    if (!Tree)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    UWidget* Target = nullptr;
    Tree->ForEachWidget([&](UWidget* W)
    {
        if (W && W->GetFName() == FName(*WidgetName))
        {
            Target = W;
        }
    });
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget '%s' not found in '%s'"), *WidgetName, *WidgetBPName));
    }

    // Inspect the target widget class for ANY property whose name matches
    // "DragDropOperation*" (covers future UE versions / specialized widgets
    // like UListViewBase which DO have such a property).
    bool bHasProp = false;
    FString PropName;
    for (TFieldIterator<FProperty> It(Target->GetClass()); It; ++It)
    {
        if (It->GetName().StartsWith(TEXT("DragDropOperation")))
        {
            bHasProp = true;
            PropName = It->GetName();
            break;
        }
    }

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    if (bHasProp)
    {
        FProperty* P = Target->GetClass()->FindPropertyByName(*PropName);
        FClassProperty* AsClassProp = CastField<FClassProperty>(P);
        if (AsClassProp && AsClassProp->MetaClass && OpClass->IsChildOf(AsClassProp->MetaClass))
        {
            Out->SetBoolField(TEXT("success"), true);
            Out->SetBoolField(TEXT("set"), true);
            Out->SetStringField(TEXT("property_set"), PropName);
            if (AsClassProp->GetObjectPropertyValue_InContainer(Target) == OpClass)
            {
                // The widget already holds that class: nothing is modified, compiled or saved.
                Out->SetBoolField(TEXT("changed"), false);
                Out->SetBoolField(TEXT("saved"), false);
                Out->SetStringField(TEXT("note"), FString::Printf(
                    TEXT("%s already holds %s; nothing was written or saved"), *PropName, *OpClass->GetName()));
            }
            else
            {
                FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_SetDragDrop", "MCP: Set Widget DragDrop"));
                WidgetBP->Modify();
                Tree->Modify();
                Target->Modify();
                AsClassProp->SetPropertyValue_InContainer(Target, OpClass);

                FString SaveErr;
                const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

                Out->SetBoolField(TEXT("changed"), true);
                mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
            }
        }
        else
        {
            Out->SetBoolField(TEXT("set"), false);
            Out->SetBoolField(TEXT("changed"), false);
            Out->SetStringField(TEXT("note"), FString::Printf(
                TEXT("Widget has a property named %s, but it is not a class property that takes %s; nothing was written"),
                *PropName, *OpClass->GetName()));
        }
    }
    else
    {
        Out->SetBoolField(TEXT("set"), false);
        Out->SetBoolField(TEXT("changed"), false);
        Out->SetStringField(TEXT("note"),
            TEXT("Target widget has no DragDropOperationClass UPROPERTY. OnDragDetected/OnDrop are DECLARE_DYNAMIC_MULTICAST_DELEGATEs -- must be wired in the widget BP graph event node, not via reflection."));
    }

    Out->SetStringField(TEXT("widget_blueprint_name"), WidgetBPName);
    Out->SetStringField(TEXT("widget_name"), WidgetName);
    Out->SetStringField(TEXT("operation_class"), OperationClassRef);
    Out->SetBoolField(TEXT("has_drag_drop_property"), bHasProp);
    Out->SetBoolField(TEXT("operation_class_loaded"), OpClass != nullptr);
    return Out;
}

// =============================================================================
// create_common_ui_stack -- replace the WidgetTree root with a
// UCommonActivatableWidgetStack (or create one as root).
//
// root_class is the class of the root widget being constructed: UCommonActivatableWidgetStack
// is a normal UWidget, so a project can author a BP subclass of it (e.g. with custom
// transition/input behavior). It is NOT an initial pushed activatable widget: the stack has no
// authoring-time slot for that -- it is populated at RUNTIME via
// AddWidget(TSubclassOf<UCommonActivatableWidget>) (CommonActivatableWidgetContainer.h).
// root_class is optional; omitted = the base UCommonActivatableWidgetStack.
// =============================================================================
// MCP:create_common_ui_stack - Set the WidgetTree root to a UCommonActivatableWidgetStack (creating one if absent). root_class, when given, picks which UCommonActivatableWidgetStack subclass gets constructed (e.g. a project-authored BP subclass) -- an unresolvable or non-subclass value does not fail the call, it honestly falls back to the base stack class with a note (see root_class_note in the response).
//   @param widget_blueprint_name - /Game path of the Widget Blueprint
//   @param root_class - Optional. Asset path (/Game/...) or /Script/<Module>.<ClassName> of a UCommonActivatableWidgetStack subclass. Omitted = base UCommonActivatableWidgetStack.
//   @returns { widget_blueprint_name, root_class?, already_existed, stack_class, root_class_requested, root_class_resolved, root_class_note?, root_changed, root_widget, root_widget_class }
TSharedPtr<FJsonObject> HandleCreateCommonUIStack(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }
    // root_class is OPTIONAL. When given it must resolve to a UCommonActivatableWidgetStack
    // SUBCLASS. Omitted -> the hardcoded base stack class.
    FString RootClassStr;
    const bool bHasRootClass = Params->TryGetStringField(TEXT("root_class"), RootClassStr) && !RootClassStr.IsEmpty();

    FString LoadErr;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPName, LoadErr);
    UWidgetTree* Tree = WidgetBP ? GetWidgetTree_MVVM(WidgetBP, LoadErr) : nullptr;
    if (!Tree)
    {
        return mcp_common::MakeErrorReply(LoadErr);
    }

    bool bAlreadyExisted = (Tree->RootWidget != nullptr)
        && Tree->RootWidget->IsA<UCommonActivatableWidgetStack>();

    // Resolve root_class to a real UClass BEFORE mutating anything. Honest fallback (not a hard
    // error): an unresolvable or non-subclass root_class falls back to the base stack class with
    // a clear note -- consistent with this file's other honest-gap responses (e.g.
    // set_widget_drag_drop) rather than either silently ignoring the param or hard-failing an
    // optional argument.
    UClass* StackClass = UCommonActivatableWidgetStack::StaticClass();
    bool bRootClassResolved = false;
    FString RootClassNote;
    if (bHasRootClass)
    {
        FString ClassLoadErr;
        UClass* Resolved = LoadWidgetClass(RootClassStr, /*bRequireUserWidget=*/false, ClassLoadErr);
        if (!Resolved)
        {
            RootClassNote = FString::Printf(
                TEXT("root_class '%s' could not be resolved (%s) -- falling back to base UCommonActivatableWidgetStack"),
                *RootClassStr, *ClassLoadErr);
        }
        else if (!Resolved->IsChildOf(UCommonActivatableWidgetStack::StaticClass()))
        {
            RootClassNote = FString::Printf(
                TEXT("root_class '%s' resolves to %s, which is not a subclass of UCommonActivatableWidgetStack -- falling back to base UCommonActivatableWidgetStack"),
                *RootClassStr, *Resolved->GetName());
        }
        else
        {
            StackClass = Resolved;
            bRootClassResolved = true;
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_CreateCommonStack", "MCP: Create CommonUI Stack"));
    WidgetBP->Modify();
    Tree->Modify();

    bool bRootChanged = false;
    if (!bAlreadyExisted)
    {
        // Build a new UCommonActivatableWidgetStack (or the resolved subclass) and graft it as root.
        UCommonActivatableWidgetStack* NewStack = Tree->ConstructWidget<UCommonActivatableWidgetStack>(
            StackClass,
            TEXT("CommonActivatableWidgetStack"));
        if (!NewStack)
        {
            return mcp_common::MakeErrorReply(
                TEXT("ConstructWidget<UCommonActivatableWidgetStack> returned null -- ensure CommonUI plugin is enabled in this project."));
        }
        Tree->RootWidget = NewStack;
        bRootChanged = true;
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] create_common_ui_stack: %s (already=%d, root_changed=%d, root_class_resolved=%d)"),
           *WidgetBPName, bAlreadyExisted ? 1 : 0, bRootChanged ? 1 : 0, bRootClassResolved ? 1 : 0);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    if (bHasRootClass)
    {
        Out->SetStringField(TEXT("root_class"), RootClassStr);
    }
    Out->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Out->SetStringField(TEXT("stack_class"), TEXT("UCommonActivatableWidgetStack"));
    Out->SetBoolField(TEXT("root_class_requested"), bHasRootClass);
    Out->SetBoolField(TEXT("root_class_resolved"), bRootClassResolved);
    if (!RootClassNote.IsEmpty())
    {
        Out->SetStringField(TEXT("root_class_note"), RootClassNote);
    }
    Out->SetBoolField(TEXT("root_changed"), bRootChanged);
    Out->SetStringField(TEXT("root_widget"), Tree->RootWidget ? Tree->RootWidget->GetName() : FString());
    Out->SetStringField(TEXT("root_widget_class"), Tree->RootWidget ? Tree->RootWidget->GetClass()->GetName() : FString());
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// =============================================================================
// set_pie_list_view_items -- populate a LIVE UListView/UTileView in the running
// PIE session. add_list_view/add_tile_view only construct an EMPTY view; this
// is the headless way to populate and exercise one. Companion to those two
// authoring tools.
// =============================================================================
// MCP:set_pie_list_view_items - Populate a LIVE UListView/UTileView in the running PIE session. Resolves widget_name (optionally drilling into child_name) among the active viewport UserWidgets using the same walk click_pie_widget uses, resolves each entry in items[] to a UObject* (an actor name/label in the PIE world, else a /Game asset path via FSoftObjectPath::TryLoad), then calls SetListItems (or AddItems when append=true). Companion to add_list_view/add_tile_view, which only construct an EMPTY view.
//   @param widget_name - Required. Name of the target UListView/UTileView in the live tree (as reported by read_pie_widgets), resolved the same way click_pie_widget resolves widget_name.
//   @param child_name - Optional: after resolving widget_name, drill into a descendant widget with this name and populate THAT widget instead.
//   @param player_index - Optional (default 0): restrict widget resolution to viewport UserWidgets owned by this local player index (single-player PIE resolves globally).
//   @param items - Required array of strings. Each entry is either an actor name/label to resolve in the PIE world, or a /Game asset path to load via FSoftObjectPath::TryLoad.
//   @param append - Optional (default false). false = SetListItems (replaces the current item list). true = AddItems (appends).
//   @param refresh - Optional (default true). Accepted for API symmetry: UListView::SetListItems/AddItems call UListViewBase::RequestRefresh() UNCONDITIONALLY, so refresh=false cannot honestly suppress it -- see refresh_note in the response rather than a silent no-op.
//   @param world - Optional world selector: absent/'pie' (default) | 'server' | 'client' | 'pie:N' | 'game' (alias of 'pie'). In a 2-player PIE session each client owns its own widgets.
//   @returns { success, widget_name, resolved_widget, resolved_class, child_name?, player_index, requested_items, resolved_count, unresolved_items[], append, refresh, refresh_note, item_count_after, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
TSharedPtr<FJsonObject> HandleSetPieListViewItems(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing params"));
    }

    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("set_pie_list_view_items: missing required parameter: widget_name"));
    }

    FString ChildName;
    Params->TryGetStringField(TEXT("child_name"), ChildName); // optional

    int32 PlayerIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), PlayerIndex); // optional, default 0

    const TArray<TSharedPtr<FJsonValue>>* ItemsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("items"), ItemsArray) || !ItemsArray)
    {
        return mcp_common::MakeErrorReply(TEXT("set_pie_list_view_items: items (array of strings) is required"));
    }

    bool bAppend = false;
    Params->TryGetBoolField(TEXT("append"), bAppend);

    bool bRefresh = true;
    Params->TryGetBoolField(TEXT("refresh"), bRefresh);

    FString WorldError, WorldSelector;
    UWorld* PieWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), WorldSelector, WorldError);
    if (!PieWorld)
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty()
            ? TEXT("set_pie_list_view_items: no Play-In-Editor session is running (start PIE first)")
            : *FString::Printf(TEXT("set_pie_list_view_items: %s"), *WorldError));
    }

    UWidget* Target = FindLivePieWidgetByName(PieWorld, FName(*WidgetName), PlayerIndex);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_pie_list_view_items: widget '%s' not found in any active viewport UserWidget of the PIE world"),
            *WidgetName));
    }

    // Optional drill into a named child/descendant of the resolved widget (same pattern
    // click_pie_widget uses for its own child_name param).
    if (!ChildName.IsEmpty())
    {
        const FName ChildFName(*ChildName);
        UWidget* Child = nullptr;
        if (UUserWidget* TargetUW = Cast<UUserWidget>(Target))
        {
            if (TargetUW->WidgetTree)
            {
                Child = TargetUW->WidgetTree->FindWidget(ChildFName);
            }
        }
        if (!Child)
        {
            Child = FindDescendantWidgetByName(Target, ChildFName);
        }
        if (!Child)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_pie_list_view_items: child widget '%s' not found under '%s'"), *ChildName, *WidgetName));
        }
        Target = Child;
    }

    // UTileView derives from UListView (both share SetListItems/AddItems/GetNumItems), so a
    // single Cast<UListView> covers both add_list_view and add_tile_view targets.
    UListView* ListView = Cast<UListView>(Target);
    if (!ListView)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_pie_list_view_items: resolved widget '%s' (class %s) is not a UListView/UTileView"),
            *Target->GetName(), *Target->GetClass()->GetName()));
    }

    // Resolve each item string to a live UObject*: an actor name/label in the PIE world first
    // (cheap, no load), else a /Game asset path via FSoftObjectPath::TryLoad.
    TArray<UObject*> ResolvedItems;
    TArray<TSharedPtr<FJsonValue>> UnresolvedItems;
    for (const TSharedPtr<FJsonValue>& ItemValue : *ItemsArray)
    {
        FString ItemStr;
        if (!ItemValue.IsValid() || !ItemValue->TryGetString(ItemStr) || ItemStr.IsEmpty())
        {
            UnresolvedItems.Add(MakeShared<FJsonValueString>(TEXT("<invalid non-string item>")));
            continue;
        }

        UObject* Resolved = mcp_common::FindActorByName(PieWorld, ItemStr);
        if (!Resolved)
        {
            FSoftObjectPath SoftPath(ItemStr);
            if (SoftPath.IsValid())
            {
                Resolved = SoftPath.TryLoad();
            }
        }

        if (Resolved)
        {
            ResolvedItems.Add(Resolved);
        }
        else
        {
            UnresolvedItems.Add(MakeShared<FJsonValueString>(ItemStr));
        }
    }

    // Never silently no-op: if the caller supplied items but NONE resolved, that's almost
    // certainly a mistake (typo'd actor name / bad asset path), not a request to clear the
    // list -- fail loud instead of quietly wiping it.
    if (ItemsArray->Num() > 0 && ResolvedItems.Num() == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_pie_list_view_items: none of the %d item(s) resolved to an actor or a loadable asset -- no mutation performed"),
            ItemsArray->Num()));
    }

    if (bAppend)
    {
        ListView->AddItems(ResolvedItems);
    }
    else
    {
        ListView->SetListItems(ResolvedItems);
    }

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_pie_list_view_items: %s resolved=%d/%d append=%d"),
           *WidgetName, ResolvedItems.Num(), ItemsArray->Num(), bAppend ? 1 : 0);

    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetBoolField(TEXT("success"), true);
    Out->SetStringField(TEXT("widget_name"), WidgetName);
    Out->SetStringField(TEXT("resolved_widget"), Target->GetName());
    Out->SetStringField(TEXT("resolved_class"), Target->GetClass()->GetName());
    if (!ChildName.IsEmpty())
    {
        Out->SetStringField(TEXT("child_name"), ChildName);
    }
    Out->SetNumberField(TEXT("player_index"), PlayerIndex);
    Out->SetNumberField(TEXT("requested_items"), ItemsArray->Num());
    Out->SetNumberField(TEXT("resolved_count"), ResolvedItems.Num());
    Out->SetArrayField(TEXT("unresolved_items"), UnresolvedItems);
    Out->SetBoolField(TEXT("append"), bAppend);
    Out->SetBoolField(TEXT("refresh"), bRefresh);
    Out->SetStringField(TEXT("refresh_note"),
        TEXT("UListView::SetListItems/AddItems call UListViewBase::RequestRefresh() unconditionally in UE 5.8 -- refresh=false is accepted but cannot honestly suppress it (reimplementing SetListItems ourselves to gate that would be materially riskier than the value it adds)."));
    Out->SetNumberField(TEXT("item_count_after"), ListView->GetNumItems());
    // WHICH client's list view was populated.
    Out->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Out, PieWorld);
    return Out;
}

// =============================================================================
// add_tree_view -- sibling of add_list_view/add_tile_view: constructs a
// UTreeView under the widget BP root (or a named parent panel). Idempotent by
// widget_name.
//
// HONEST SCOPE: this is an AUTHORING tool only (same posture as
// add_list_view/add_tile_view for their own EntryWidgetClass). Populating the
// tree's actual hierarchy (SetTreeItemsSource + the OnGetItemChildren delegate
// that supplies each node's children) is out of scope: OnGetItemChildren is a
// native TSlateDelegates callback set via UTreeView::SetOnGetItemChildren (a
// C++ template method, not a reflected UPROPERTY/UFUNCTION) or the BP-only
// dynamic delegate wired in the widget BP graph -- neither is something this
// reflection-based authoring tool can set. This tool only constructs the
// widget and wires its EntryWidgetClass, exactly like add_list_view/
// add_tile_view do.
// =============================================================================
// MCP:add_tree_view - Construct a UTreeView under the widget BP root (or a named parent panel widget) and add it to the tree. A root that is not a panel, or a full single-child panel, is an error and nothing is added. Idempotent by name: a widget already called widget_name, of any class, is left untouched (already_existed=true; tree_view_class and entry_widget_class are its own), the Blueprint still compiled and saved. HONEST SCOPE: authoring only, same posture as add_list_view/add_tile_view -- constructs the widget + wires EntryWidgetClass. Populating the tree's actual hierarchy (SetTreeItemsSource + the OnGetItemChildren delegate that supplies each node's children) is OUT OF SCOPE: that delegate is a native TSlateDelegates callback, not reflectively assignable -- wire it in the widget BP graph (native C++/BP override) or drive it live in PIE.
//   @param widget_blueprint_name - /Game path of the Widget Blueprint
//   @param widget_name - FName of the new UTreeView widget
//   @param entry_widget_class - Optional entry widget class (asset or /Script path)
//   @param parent_widget_name - Optional: name of an existing panel widget (UPanelWidget) in the tree to insert the new UTreeView under. Omitted = same root-insertion behavior as add_list_view/add_tile_view (canvas/panel root; in an empty tree the view becomes the root; a non-panel root is an error).
//   @returns { widget_blueprint_name, widget_name, tree_view_class, already_existed, entry_widget_class, parent_widget_name?, widget_added, saved, save_error?, note }
TSharedPtr<FJsonObject> HandleAddTreeView(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPName;
    if (TSharedPtr<FJsonObject> Missing = ReadWidgetBlueprintName_MVVM(Params, WidgetBPName)) { return Missing; }
    FString TreeViewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), TreeViewName)) { return Missing; }

    FString EntryClassRef;
    Params->TryGetStringField(TEXT("entry_widget_class"), EntryClassRef);

    FString ParentWidgetName;
    Params->TryGetStringField(TEXT("parent_widget_name"), ParentWidgetName);

    FListViewTarget_MVVM View;
    if (TSharedPtr<FJsonObject> Failure = ResolveListViewTarget_MVVM(WidgetBPName, TreeViewName, EntryClassRef, View))
    {
        return Failure;
    }

    // Resolve the optional parent panel BEFORE mutating anything, so a bad parent_widget_name
    // fails clean without a half-built tree left behind.
    UPanelWidget* ParentPanel = nullptr;
    if (!ParentWidgetName.IsEmpty())
    {
        UWidget* ParentWidget = FindWidgetInTreeByName(View.Tree, FName(*ParentWidgetName));
        if (!ParentWidget)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("add_tree_view: parent_widget_name '%s' not found in '%s'"), *ParentWidgetName, *WidgetBPName));
        }
        ParentPanel = Cast<UPanelWidget>(ParentWidget);
        if (!ParentPanel)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("add_tree_view: parent_widget_name '%s' (class %s) is not a panel widget that can accept children"),
                *ParentWidgetName, *ParentWidget->GetClass()->GetName()));
        }
    }
    if (!View.bAlreadyExisted)
    {
        const FString HostError = ListViewHostError_MVVM(View.Tree, ParentPanel);
        if (!HostError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(HostError);
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MVVM_AddTreeView", "MCP: Add TreeView"));
    View.WidgetBP->Modify();
    View.Tree->Modify();

    // The widget the reply describes: the one already holding the name, left untouched, or the new view.
    UWidget* ReportedView = View.ExistingWidget;
    if (!ReportedView)
    {
        ReportedView = AddListTypeView_MVVM<UTreeView>(View.Tree, View.ViewName, View.EntryClass, ParentPanel);
        if (!ReportedView)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Tree->ConstructWidget<UTreeView> returned null"));
        }
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(View.WidgetBP, SaveErr);

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] add_tree_view: %s on %s (already=%d)"),
           *TreeViewName, *WidgetBPName, View.bAlreadyExisted ? 1 : 0);

    TSharedPtr<FJsonObject> Out = StartReply_MVVM(true, WidgetBPName);
    Out->SetStringField(TEXT("widget_name"), TreeViewName);
    AddViewClassFields_MVVM(Out, TEXT("tree_view_class"), ReportedView);
    Out->SetBoolField(TEXT("already_existed"), View.bAlreadyExisted);
    if (!ParentWidgetName.IsEmpty())
    {
        Out->SetStringField(TEXT("parent_widget_name"), ParentWidgetName);
    }
    Out->SetBoolField(TEXT("widget_added"), !View.bAlreadyExisted);
    Out->SetStringField(TEXT("note"),
        TEXT("Authoring only: constructs the UTreeView + EntryWidgetClass. Populating the tree (SetTreeItemsSource + the OnGetItemChildren delegate that supplies each node's children) is out of scope -- that delegate is a native TSlateDelegates callback, not reflectively assignable; wire it in the widget BP graph or drive it live in PIE."));
    mcp_common::AddSaveOutcome(Out, bSaved, SaveErr);
    return Out;
}

// ---------------------------------------------------------------------------
// PUSH / POP ON A CommonUI STACK
//
// Why: create_common_ui_stack only sets the stack as the root: the content is
// populated AT RUNTIME with AddWidget. Without push/pop the
// screen state machine cannot be driven to a specific state to verify it -- which is exactly
// what testing a menu requires.
//
// WARNING: THIS IS RUNTIME-ONLY, not authoring. WidgetList and the container's widget pool are
// Transient (CommonActivatableWidgetContainer.h): in the editor without PIE there is
// nothing to push, and the command says so instead of pretending it did something.
// ---------------------------------------------------------------------------
// MCP:push_common_ui_widget - Push a screen onto a LIVE CommonUI activatable stack in the running PIE session -- the operation that lets a menu state machine be driven to a specific state and then asserted. create_common_ui_stack only puts the stack in place; its contents are populated at runtime with AddWidget, which this calls headless, so "open the pause menu, then the options page, now check the back button" is reproducible. WARNING: RUNTIME ONLY: the container's widget list and its widget pool are Transient, so with no PIE session there is genuinely nothing to push onto and this says so rather than pretending. The pushed widget is activated by the stack itself; read the result with get_common_ui_stack or read_pie_widgets, and move focus into it with set_pie_widget_focus.
//   @param stack_widget_name [string!] - Name of the live UCommonActivatableWidgetStack (or any activatable container) in the running session. Resolved like click_pie_widget, nested Widget Blueprints included.
//   @param widget_class [string!] - The UCommonActivatableWidget class to push: a /Game Widget Blueprint path, its generated-class path, or a native class path.
//   @param player_index - Optional local player index whose viewport widgets are searched (default 0).
//   @param world - Optional world selector: absent/'pie' | 'server' | 'client' | 'pie:N'.
//   @returns { stack_widget_name, pushed_class, pushed_widget: string, widget_count: int, active_widget: string } -- widget_count and active_widget are read off the stack AFTER the push, so they are evidence rather than an echo.
TSharedPtr<FJsonObject> HandlePushCommonUIWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString StackName, ClassStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stack_widget_name"), StackName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_class"), ClassStr)) { return Missing; }

    UCommonActivatableWidgetContainerBase* Stack = nullptr;
    FString ResolveErr;
    if (!ResolveLiveCommonUIStack(Params, StackName, Stack, ResolveErr))
        return mcp_common::MakeErrorReply(ResolveErr);

    FString ClassErr;
    UClass* PushClass = LoadWidgetClass(ClassStr, /*bRequireUserWidget=*/ true, ClassErr);
    if (!PushClass || !PushClass->IsChildOf(UCommonActivatableWidget::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("widget_class '%s' does not resolve to a UCommonActivatableWidget subclass%s. A plain "
                 "UserWidget cannot go on an activatable stack -- it has no Activate/Deactivate."),
            *ClassStr, ClassErr.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *ClassErr)));
    }

    // AddWidget is a template ONLY in its return type; the parameter is a TSubclassOf, so a
    // class resolved at runtime goes in without any problem.
    UCommonActivatableWidget* Pushed =
        Stack->AddWidget<UCommonActivatableWidget>(TSubclassOf<UCommonActivatableWidget>(PushClass));

    if (!Pushed)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("the stack refused to create '%s' (AddWidget returned null)."), *PushClass->GetName()));
    }

    UCommonActivatableWidget* Active = Stack->GetActiveWidget();

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("stack_widget_name"), StackName);
    R->SetStringField(TEXT("pushed_class"), PushClass->GetName());
    R->SetStringField(TEXT("pushed_widget"), Pushed->GetName());
    R->SetNumberField(TEXT("widget_count"), Stack->GetNumWidgets());
    R->SetStringField(TEXT("active_widget"), Active ? Active->GetName() : FString());
    return mcp_common::MakeOkReply(R);
}

// MCP:pop_common_ui_widget - Pop the ACTIVE screen off a live CommonUI stack, or clear the whole stack -- the inverse of push_common_ui_widget and the other half of driving a menu state machine headless (open, go deeper, come back, assert what is on top). WARNING: RUNTIME ONLY, same as the push: with no PIE session there is nothing on any stack. Popping an empty stack is an honest success with popped:false, not an error.
//   @param stack_widget_name [string!] - Name of the live activatable container in the running session.
//   @param clear_all [bool] - Remove EVERY widget instead of just the active one (default false).
//   @param player_index - Optional local player index whose viewport widgets are searched (default 0).
//   @param world - Optional world selector: absent/'pie' | 'server' | 'client' | 'pie:N'.
//   @returns { stack_widget_name, popped: bool, popped_widget: string, cleared_all: bool, widget_count: int, active_widget: string } -- widget_count and active_widget are read AFTER the operation; popped:false with widget_count:0 means the stack was already empty.
TSharedPtr<FJsonObject> HandlePopCommonUIWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString StackName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stack_widget_name"), StackName)) { return Missing; }

    UCommonActivatableWidgetContainerBase* Stack = nullptr;
    FString ResolveErr;
    if (!ResolveLiveCommonUIStack(Params, StackName, Stack, ResolveErr))
        return mcp_common::MakeErrorReply(ResolveErr);

    bool bClearAll = false;
    Params->TryGetBoolField(TEXT("clear_all"), bClearAll);

    FString PoppedName;
    bool bPopped = false;

    if (bClearAll)
    {
        bPopped = Stack->GetNumWidgets() > 0;
        Stack->ClearWidgets();
    }
    else if (UCommonActivatableWidget* Active = Stack->GetActiveWidget())
    {
        PoppedName = Active->GetName();
        Stack->RemoveWidget(*Active);
        bPopped = true;
    }

    UCommonActivatableWidget* NowActive = Stack->GetActiveWidget();

    TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
    R->SetStringField(TEXT("stack_widget_name"), StackName);
    R->SetBoolField(TEXT("popped"), bPopped);
    R->SetStringField(TEXT("popped_widget"), PoppedName);
    R->SetBoolField(TEXT("cleared_all"), bClearAll);
    R->SetNumberField(TEXT("widget_count"), Stack->GetNumWidgets());
    R->SetStringField(TEXT("active_widget"), NowActive ? NowActive->GetName() : FString());
    return mcp_common::MakeOkReply(R);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_list_view"), &HandleAddListView },
    { TEXT("add_tile_view"), &HandleAddTileView },
    { TEXT("add_tree_view"), &HandleAddTreeView },
    { TEXT("add_viewmodel"), &HandleAddViewModel },
    { TEXT("bind_widget_to_viewmodel"), &HandleBindWidgetToViewModel },
    { TEXT("create_common_ui_stack"), &HandleCreateCommonUIStack },
    { TEXT("pop_common_ui_widget"), &HandlePopCommonUIWidget },
    { TEXT("push_common_ui_widget"), &HandlePushCommonUIWidget },
    { TEXT("remove_viewmodel"), &HandleRemoveViewModel },
    { TEXT("set_pie_list_view_items"), &HandleSetPieListViewItems },
    { TEXT("set_viewmodel_field"), &HandleSetViewModelField },
    { TEXT("set_widget_drag_drop"), &HandleSetWidgetDragDrop },
    { TEXT("unbind_widget_from_viewmodel"), &HandleUnbindWidgetFromViewModel },
};
} // namespace UEAgentUMGMVVMCommands
