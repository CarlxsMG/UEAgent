#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/UI/UMGWidgetUtils.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Components/WidgetComponent.h"
#include "Blueprint/UserWidget.h"
#include "UMG.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectGlobals.h"
#include "Kismet/GameplayStatics.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/GridPanel.h"
#include "Components/HorizontalBox.h"
#include "Components/VerticalBox.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/ProgressBar.h"
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/ComboBoxString.h"
#include "Components/ListView.h"
#include "Components/TreeView.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBar.h"
#include "Components/SpinBox.h"
#include "Components/MultiLineEditableTextBox.h"
#include "Components/RichTextBlock.h"
#include "Components/UniformGridPanel.h"
#include "Components/WrapBox.h"
#include "Components/SizeBox.h"
#include "Components/Border.h"
#include "Components/Overlay.h"
#include "Components/StackBox.h"
#include "Components/WidgetSwitcher.h"
#include "Components/ContentWidget.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/BlueprintSupport.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/FieldPath.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "LevelEditorViewport.h"
#include "ImageUtils.h"
#include "HighResScreenshot.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/SpotLight.h"
#include "Camera/CameraActor.h"
#include "Components/StaticMeshComponent.h"
#include "EditorSubsystem.h"
#include "Subsystems/EditorActorSubsystem.h"

/**
 * Level-placed UMG widget commands.
 *
 * The lookup commands resolve "widget_name" against a UWidgetComponent (a world-space 3D widget, e.g. a
 * nameplate, healthbar or interaction prompt hanging off an actor) found by scanning ALL actors of the
 * CURRENT EDITOR WORLD (GEditor->GetEditorWorldContext().World(), NEVER the running PIE world);
 * list_level_widget_components enumerates exactly that population. create_widget creates a UUserWidget
 * against either the editor world or a running PIE world's local PlayerController (target_world).
 *
 * This family has NOTHING to do with:
 *   - Widget Blueprint AUTHORING (the CanvasPanel/box/grid slot layout inside a WBP asset): that is
 *     set_widget_slot_properties and friends in UEAgentWidgetCommands.cpp.
 *   - Runtime UI actually on screen in a running PIE session (HUD, menus, panels added via
 *     AddToViewport()): that is read_pie_widgets / click_pie_widget, also in UEAgentWidgetCommands.cpp.
 */
namespace UEAgentUICommands
{

// MCP-DOMAIN: UI

namespace
{
    // Appended to every "Widget 'X' not found" error in this file (except remove_widget, whose
    // fallback search is broader -- see its own comment) so callers don't have to rediscover by
    // trial and error that this file only ever searches world-space UWidgetComponents in the
    // EDITOR level.
    const TCHAR* GLevelWidgetLookupHint = TEXT(
        " This command only searches world-space UWidgetComponent instances on actors in the "
        "CURRENT EDITOR LEVEL (GEditor's world, never a running PIE session). Use "
        "list_level_widget_components to discover valid widget_name values instead of guessing. "
        "For runtime UI actually on screen in a running PIE session (HUD, menus, panels added "
        "via AddToViewport()), use read_pie_widgets/click_pie_widget instead "
        "(UEAgentWidgetCommands.cpp).");
}

// File-local helpers of the level-widget commands below.
namespace UEAgentUICommandsImpl
{
    /** Inverse of UMGWidgetUtils::SlateVisibilityName for the five settable values. FString equality: ignores case. */
    bool ParseVisibilityName(const FString& Name, ESlateVisibility& OutVisibility)
    {
        const ESlateVisibility Settable[] =
        {
            ESlateVisibility::Visible,
            ESlateVisibility::Collapsed,
            ESlateVisibility::Hidden,
            ESlateVisibility::HitTestInvisible,
            ESlateVisibility::SelfHitTestInvisible,
        };
        for (const ESlateVisibility Candidate : Settable)
        {
            if (Name == UMGWidgetUtils::SlateVisibilityName(Candidate))
            {
                OutVisibility = Candidate;
                return true;
            }
        }
        return false;
    }

    TSharedPtr<FJsonObject> DrawSizeObject(const FVector2D& Size)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("width"), Size.X);
        Out->SetNumberField(TEXT("height"), Size.Y);
        return Out;
    }

    /** Direct children of the widget's root panel; 0 when the root is not a panel. */
    int32 RootPanelChildCount(const UUserWidget* Widget)
    {
        const UPanelWidget* Panel = Cast<UPanelWidget>(Widget->GetRootWidget());
        return Panel ? Panel->GetChildrenCount() : 0;
    }

    /** The per-child fields of get_level_widget_properties (name, class, depth, flags, visibility). */
    TSharedPtr<FJsonObject> DescribeHierarchyNode(UWidget* Node, int32 Depth)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("name"), Node->GetName());
        Out->SetStringField(TEXT("class"), Node->GetClass()->GetName());
        Out->SetNumberField(TEXT("depth"), Depth);
        Out->SetBoolField(TEXT("valid"), IsValid(Node));
        Out->SetBoolField(TEXT("enabled"), Node->GetIsEnabled());
        Out->SetStringField(TEXT("visibility"), UMGWidgetUtils::SlateVisibilityName(Node->GetVisibility()));
        return Out;
    }

    /**
     * The UWidgetComponent of World whose assigned UUserWidget is named WidgetName, and that widget; false with both
     * null when there is none. Every UWidgetComponent of every actor is searched (GetComponents, not
     * FindComponentByClass), so an actor carrying several -- a nameplate and a healthbar -- is searched in full.
     */
    bool FindWidgetComponentByName(UWorld* World, const FString& WidgetName, UWidgetComponent*& OutWidgetComponent, UUserWidget*& OutUserWidget)
    {
        OutWidgetComponent = nullptr;
        OutUserWidget = nullptr;

        if (!World)
        {
            return false;
        }

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!IsValid(Actor)) { continue; }

            TArray<UWidgetComponent*> ActorWidgetComponents;
            Actor->GetComponents<UWidgetComponent>(ActorWidgetComponents);

            for (UWidgetComponent* WidgetComponent : ActorWidgetComponents)
            {
                if (!IsValid(WidgetComponent)) { continue; }

                UUserWidget* Widget = Cast<UUserWidget>(WidgetComponent->GetWidget());
                if (Widget && Widget->GetName() == WidgetName)
                {
                    OutWidgetComponent = WidgetComponent;
                    OutUserWidget = Widget;
                    return true;
                }
            }
        }

        return false;
    }

    /**
     * Front half shared by the three level-widget readers: the editor world (GEditor is used
     * without a null check), then the by-name search. Null with both out pointers filled, or
     * the error reply.
     */
    TSharedPtr<FJsonObject> ResolveLevelWidget(const FString& WidgetName, UWidgetComponent*& OutHost, UUserWidget*& OutWidget)
    {
        UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
        if (!EditorWorld)
        {
            return mcp_common::MakeErrorReply(TEXT("No world found"));
        }
        FindWidgetComponentByName(EditorWorld, WidgetName, OutHost, OutWidget);
        if (!OutWidget)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Widget '%s' not found.%s"), *WidgetName, GLevelWidgetLookupHint));
        }
        return nullptr;
    }

    /** list_level_widget_components rows for every valid UWidgetComponent of one actor. */
    void AppendWidgetComponentRows(AActor* Actor, TArray<TSharedPtr<FJsonValue>>& OutRows)
    {
        TArray<UWidgetComponent*> Hosts;
        Actor->GetComponents<UWidgetComponent>(Hosts);
        const FString OwnerName = Actor->GetName();
        for (UWidgetComponent* Host : Hosts)
        {
            if (!IsValid(Host))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("actor_name"), OwnerName);
            Row->SetStringField(TEXT("widget_component_name"), Host->GetName());

            // Unassigned widget / widget class are explicit JSON nulls, not missing keys.
            if (const UUserWidget* Assigned = Cast<UUserWidget>(Host->GetWidget()))
            {
                Row->SetStringField(TEXT("widget_name"), Assigned->GetName());
            }
            else
            {
                Row->SetField(TEXT("widget_name"), MakeShared<FJsonValueNull>());
            }
            const TSubclassOf<UUserWidget> AssignedClass = Host->GetWidgetClass();
            if (AssignedClass)
            {
                Row->SetStringField(TEXT("widget_class"), AssignedClass->GetName());
            }
            else
            {
                Row->SetField(TEXT("widget_class"), MakeShared<FJsonValueNull>());
            }
            OutRows.Add(MakeShared<FJsonValueObject>(Row));
        }
    }

    /** remove_widget's fallback: the first UUserWidget object of World with that name. */
    UUserWidget* FindLooseUserWidget(UWorld* World, const FString& WidgetName)
    {
        for (TObjectIterator<UUserWidget> Scan; Scan; ++Scan)
        {
            UUserWidget* Candidate = *Scan;
            if (Candidate != nullptr && Candidate->GetWorld() == World && Candidate->GetName() == WidgetName)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    /** Where create_widget builds the widget. */
    struct FWidgetTarget
    {
        FString Label;                                 // reported as target_world
        UWorld* World = nullptr;
        APlayerController* LocalController = nullptr; // only ever set for a live world
    };

    /**
     * create_widget's target_world -> world (+ the first local PlayerController of a live world).
     * Without a selector the target is "pie" when a PIE world resolves, else "editor". A live
     * world is ALWAYS probed (with the requested selector, or "pie" when nothing or
     * "editor" was asked) because that default depends on the answer. Returns the error message
     * to report, or an empty string on success.
     */
    FString ResolveWidgetTarget(bool bHasSelector, const FString& Selector, FWidgetTarget& Out)
    {
        const bool bAsked = bHasSelector && !Selector.IsEmpty();
        FString LiveSelector = (bAsked && !Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
            ? Selector
            : FString(TEXT("pie"));
        if (LiveSelector.Equals(TEXT("game"), ESearchCase::IgnoreCase))
        {
            LiveSelector = TEXT("pie");
        }
        TSharedPtr<FJsonObject> Probe = MakeShared<FJsonObject>();
        Probe->SetStringField(TEXT("world"), LiveSelector);
        FString ProbeError;
        UWorld* LiveWorld = mcp_common::ResolveWorldFromParams(Probe, ProbeError);

        Out.Label = bAsked ? Selector.ToLower() : FString(LiveWorld ? TEXT("pie") : TEXT("editor"));
        if (Out.Label.Equals(TEXT("editor")))
        {
            // The editor world is never a game world: no local PlayerController, so
            // add_to_viewport is reported as unavailable rather than simulated.
            Out.World = mcp_common::GetEditorWorld();
        }
        else
        {
            if (!LiveWorld)
            {
                // The resolver's message names the worlds that DO exist.
                return ProbeError.IsEmpty()
                    ? FString(TEXT("target_world='pie' requested but no Play-In-Editor session is running (start PIE first)"))
                    : ProbeError;
            }
            Out.World = LiveWorld;
            Out.LocalController = LiveWorld->GetFirstPlayerController();
        }

        if (!Out.World || !Out.World->IsValidLowLevel())
        {
            return TEXT("No valid world found");
        }
        return FString();
    }

    /** LoadClass on the path as given, then once more through its FSoftObjectPath form. */
    UClass* LoadUserWidgetClass(const FString& ClassPath)
    {
        if (UClass* Loaded = LoadClass<UUserWidget>(nullptr, *ClassPath))
        {
            return Loaded;
        }
        const FSoftObjectPath Normalized(ClassPath);
        return LoadClass<UUserWidget>(nullptr, *Normalized.ToString());
    }
}

// Widgets placed in the level

// MCP:get_level_widget_properties - Read the live state of a UUserWidget hosted on a UWidgetComponent in the current editor level, found by the widget's object name (list_level_widget_components gives the names): class, validity, visibility, enabled and focus, the hosting component's draw size and relative transform, parent, and the direct children of its root panel (one level). Use it for this runtime state, which no UPROPERTY read gives; inspect_object reads the widget's raw properties. A running PIE session is never searched: use read_pie_widgets for live game UI.
//   @param widget_name - Name of the widget to query
//   @returns { widget_name: string, widget_class: string, widget_valid: bool, widget_visible: bool, visibility: string, size?: { width: number, height: number }, position?: { x: number, y: number, z: number }, rotation?: { pitch: number, yaw: number, roll: number }, scale?: { x: number, y: number, z: number }, parent_widget?: string, parent_class?: string, child_count: number, children: [{ name: string, class: string, depth: number (1), valid: bool, enabled: bool, visibility: string }], is_enabled: bool, has_focus: bool }

TSharedPtr<FJsonObject> HandleGetWidgetProperties(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentUICommandsImpl;

    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UWidgetComponent* Host = nullptr;
    UUserWidget* Widget = nullptr;
    if (TSharedPtr<FJsonObject> Refusal = Impl::ResolveLevelWidget(TargetName, Host, Widget))
    {
        return Refusal;
    }

    const ESlateVisibility Visibility = Widget->GetVisibility();
    TSharedPtr<FJsonObject> Facts = MakeShared<FJsonObject>();
    Facts->SetStringField(TEXT("widget_name"), TargetName);
    Facts->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetName());
    Facts->SetBoolField(TEXT("widget_valid"), IsValid(Widget));
    Facts->SetBoolField(TEXT("widget_visible"), Visibility == ESlateVisibility::Visible);
    Facts->SetStringField(TEXT("visibility"), UMGWidgetUtils::SlateVisibilityName(Visibility));

    // Placement of the hosting component, relative to its parent.
    if (Host)
    {
        Facts->SetObjectField(TEXT("size"), Impl::DrawSizeObject(Host->GetDrawSize()));
        Facts->SetObjectField(TEXT("position"), mcp_common::VectorToJsonObject(Host->GetRelativeLocation()));
        Facts->SetObjectField(TEXT("rotation"), mcp_common::RotatorToJsonObject(Host->GetRelativeRotation()));
        Facts->SetObjectField(TEXT("scale"), mcp_common::VectorToJsonObject(Host->GetRelativeScale3D()));
    }

    const UWidget* Parent = Widget->GetParent();
    Facts->SetStringField(TEXT("parent_widget"), Parent ? Parent->GetName() : FString(TEXT("None")));
    Facts->SetStringField(TEXT("parent_class"), Parent ? Parent->GetClass()->GetName() : FString(TEXT("None")));
    Facts->SetNumberField(TEXT("child_count"), Impl::RootPanelChildCount(Widget));
    // Exactly one level below the root panel: its valid direct children, not walked further.
    TArray<TSharedPtr<FJsonValue>> Children;
    if (const UPanelWidget* Panel = Cast<UPanelWidget>(Widget->GetRootWidget()))
    {
        const int32 ChildTotal = Panel->GetChildrenCount();
        for (int32 ChildIndex = 0; ChildIndex < ChildTotal; ++ChildIndex)
        {
            UWidget* Child = Panel->GetChildAt(ChildIndex);
            if (IsValid(Child))
            {
                Children.Add(MakeShared<FJsonValueObject>(Impl::DescribeHierarchyNode(Child, 1)));
            }
        }
    }
    Facts->SetArrayField(TEXT("children"), Children);
    Facts->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());
    Facts->SetBoolField(TEXT("has_focus"), Widget->HasAnyUserFocus());
    return mcp_common::MakeOkReply(Facts);
}

// MCP:set_level_widget_visibility - Set the Slate visibility of a UUserWidget hosted on a UWidgetComponent in the current editor level, found by the widget's object name (list_level_widget_components gives the names; a running PIE session is never searched). visibility is matched without case, and an invalid value is an error before the lookup; a widget not found is an error too. The change goes to the live widget instance only: no undo step, nothing marked dirty or saved, and a widget the component rebuilds, after a level reload for instance, starts from its class default again. A repeat call changes nothing. previous_visibility and actual_visibility are read around the write, and visibility_set compares the result with the request. Read it with get_level_widget_properties; live PIE UI is read_pie_widgets; a Widget Blueprint's own widgets are set_widget_property.
//   @param widget_name - Object name of the UUserWidget a UWidgetComponent in the editor level hosts, as list_level_widget_components reports it in widget_name
//   @param visibility - Target visibility state: Visible, Collapsed, Hidden, HitTestInvisible, or SelfHitTestInvisible (matched without case)
//   @returns { widget_name: string, visibility_set: bool, previous_visibility: string, requested_visibility: string, actual_visibility: string, widget_valid: bool }

TSharedPtr<FJsonObject> HandleSetWidgetVisibility(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentUICommandsImpl;

    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString RequestedName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("visibility"), RequestedName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The requested value is validated before anything is looked up.
    ESlateVisibility Requested = ESlateVisibility::Visible;
    if (!Impl::ParseVisibilityName(RequestedName, Requested))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid visibility value: %s. Valid values: Visible, Collapsed, Hidden, HitTestInvisible, SelfHitTestInvisible"),
            *RequestedName));
    }

    UWidgetComponent* Host = nullptr;
    UUserWidget* Widget = nullptr;
    if (TSharedPtr<FJsonObject> Refusal = Impl::ResolveLevelWidget(TargetName, Host, Widget))
    {
        return Refusal;
    }

    // Write, then re-read: visibility_set compares what the widget reports afterwards with
    // what was asked for.
    const ESlateVisibility Before = Widget->GetVisibility();
    Widget->SetVisibility(Requested);
    const ESlateVisibility After = Widget->GetVisibility();

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("widget_name"), TargetName);
    Outcome->SetBoolField(TEXT("visibility_set"), After == Requested);
    Outcome->SetStringField(TEXT("previous_visibility"), UMGWidgetUtils::SlateVisibilityName(Before));
    Outcome->SetStringField(TEXT("requested_visibility"), RequestedName);
    Outcome->SetStringField(TEXT("actual_visibility"), UMGWidgetUtils::SlateVisibilityName(After));
    Outcome->SetBoolField(TEXT("widget_valid"), IsValid(Widget));
    return mcp_common::MakeOkReply(Outcome);
}

// MCP:list_level_widget_components - CALL THIS FIRST to discover valid widget_name values instead of guessing: it enumerates ALL UWidgetComponents on ALL actors of the current EDITOR level, which is the EXACT population that get_level_widget_properties / set_level_widget_visibility search by widget_name (remove_widget searches it too). It reads GEditor's world and never a running PIE session (use read_pie_widgets for that), and it lists every widget component on an actor, not just the first. WARNING: THIS POPULATION IS NOT WIDGET BLUEPRINTS, and that is the trap the name does not warn you about: these are world-space UWidgetComponents hosted on actors. To author the CONTENT of a Widget Blueprint asset -- its tree, styles or layout -- you want get_widget_blueprint_tree / set_widget_property / set_widget_slot_properties instead. In particular ANCHORS live on a UCanvasPanelSlot inside a Widget Blueprint's tree and have no meaning at all on a level UWidgetComponent: set them with set_widget_slot_properties(widget_bp_path, widget_name, anchors=[min_x,min_y,max_x,max_y]).
//   @param actor_name_filter - Optional substring filter on the owning actor's name (Contains match)
//   @returns { components: [{ actor_name: string, widget_component_name: string, widget_name: string|null, widget_class: string|null }], component_count: number, actor_name_filter: string|null }

TSharedPtr<FJsonObject> HandleListLevelWidgetComponents(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentUICommandsImpl;

    FString NameFilter;
    const bool bFiltered = Params->TryGetStringField(TEXT("actor_name_filter"), NameFilter) && !NameFilter.IsEmpty();

    UWorld* EditorWorld = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(EditorWorld, TEXT("No world found"))) { return NoWorld; }

    // Every UWidgetComponent of every actor whose name passes the optional filter
    // (FString::Contains, so the substring match ignores case).
    TArray<TSharedPtr<FJsonValue>> Rows;
    for (AActor* Actor : TActorRange<AActor>(EditorWorld))
    {
        if (IsValid(Actor) && (!bFiltered || Actor->GetName().Contains(NameFilter)))
        {
            Impl::AppendWidgetComponentRows(Actor, Rows);
        }
    }

    TSharedPtr<FJsonObject> Listing = MakeShared<FJsonObject>();
    Listing->SetArrayField(TEXT("components"), Rows);
    Listing->SetNumberField(TEXT("component_count"), Rows.Num());
    if (bFiltered)
    {
        Listing->SetStringField(TEXT("actor_name_filter"), NameFilter);
    }
    else
    {
        Listing->SetField(TEXT("actor_name_filter"), MakeShared<FJsonValueNull>());
    }
    return mcp_common::MakeOkReply(Listing);
}

// Widget creation and removal

// MCP:create_widget - Create a runtime UUserWidget instance of a concrete UserWidget class in a running PIE world (the default while PIE runs) or the editor world, owned by that world's first local PlayerController when there is one, and optionally AddToViewport it. Nothing is saved, and an unreferenced widget is garbage-collected. In the editor world add_to_viewport never adds (no PlayerController): read added_to_viewport. A taken name gets a unique variant, reported in widget_name; a class that does not load, is abstract or is not a UserWidget is an error. To change the asset use add_widget_to_blueprint; remove_widget searches the same worlds.
//   @param widget_class_path - Full path to widget class, e.g. '/Game/UI/MyWidget.MyWidget_C'
//   @param widget_name - Name to assign to the created widget instance (a name another widget already has gets a unique variant, which widget_name in the response reports)
//   @param add_to_viewport - If true, AddToViewport the widget (optional, default false). It only ever really adds when the target world has a local PlayerController -- check added_to_viewport in the response, and see target_world for when that is possible at all.
//   @param target_world - Optional: "editor" | "pie" | "server" (the PIE world with authority) | "client" | "pie:N" | "game" (alias of "pie"). Defaults to "pie" when a Play-In-Editor session is running, else "editor". IMPORTANT: "editor" NEVER has a PlayerController, because GEditor's world is not a game world, so add_to_viewport can never really add there -- that is reported honestly (added_to_viewport stays false), never simulated. "pie" resolves the running PIE world and its first local PlayerController, so add_to_viewport there performs a REAL AddToViewport().
//   @returns { widget_name: string, widget_class_path: string, widget_class_name: string, target_world: string, added_to_viewport: bool, add_to_viewport_note?: string, widget_valid: bool, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
//   NOTE (impl): "pie" scans GEngine->GetWorldContexts() for EWorldType::PIE, the same idiom as
//   read_pie_widgets / click_pie_widget in UEAgentWidgetCommands.cpp.

TSharedPtr<FJsonObject> HandleCreateWidget(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentUICommandsImpl;

    FString ClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_class_path"), ClassPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString InstanceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), InstanceName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // CANONICAL WORLD RESOLUTION (mcp_common::ResolveWorldFromParams, inside
    // ResolveWidgetTarget). A widget added to the wrong client's viewport is invisible to the
    // player who was supposed to see it, so 'server' / 'client' / 'pie:N' are addressable on
    // top of the default (omitted = "pie" while PIE runs, else "editor").
    FString Selector;
    const bool bHasSelector = Params->TryGetStringField(TEXT("target_world"), Selector);
    Selector.TrimStartAndEndInline();

    Impl::FWidgetTarget Target;
    const FString TargetError = Impl::ResolveWidgetTarget(bHasSelector, Selector, Target);
    if (!TargetError.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TargetError);
    }

    UClass* WidgetClass = Impl::LoadUserWidgetClass(ClassPath);
    if (!WidgetClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Widget class not found: %s. Use full path like '/Game/UI/MyWidget.MyWidget_C'"), *ClassPath));
    }
    if (!WidgetClass->IsChildOf(UUserWidget::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class is not a UserWidget: %s"), *ClassPath));
    }
    // An abstract class (UUserWidget itself included) cannot be instantiated: the engine only
    // rejects it under WITH_EDITOR, and a packaged build would trip the abstract-class ensure
    // during construction. Refuse it here with a clear message instead.
    if (WidgetClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Cannot create widget from abstract class '%s'; provide a concrete UserWidget subclass"), *ClassPath));
    }

    // Owned by the local PlayerController when there is one, otherwise by the world alone (the
    // editor world, or a PIE world whose local player does not exist yet).
    UUserWidget* Created = Target.LocalController
        ? CreateWidget<UUserWidget>(Target.LocalController, WidgetClass)
        : CreateWidget<UUserWidget>(Target.World, WidgetClass);
    if (!Created)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create widget. Ensure the target world has a valid PlayerController."));
    }
    // A name another widget of the same outer already has gets a unique variant: widget_name reports it.
    const FName CreatedName = mcp_common::RenameToFreeName(Created, InstanceName, nullptr, REN_None);

    // add_to_viewport only ever really adds with a resolved local PlayerController; without one
    // it is never simulated -- the note below says why nothing happened.
    bool bAddToViewport = false;
    Params->TryGetBoolField(TEXT("add_to_viewport"), bAddToViewport);
    const bool bHasLocalPlayer = Target.LocalController != nullptr;
    if (bAddToViewport && bHasLocalPlayer)
    {
        Created->AddToViewport();
    }

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("widget_name"), InstanceName.IsEmpty() ? InstanceName : CreatedName.ToString());
    Outcome->SetStringField(TEXT("widget_class_path"), ClassPath);
    Outcome->SetStringField(TEXT("widget_class_name"), WidgetClass->GetName());
    Outcome->SetStringField(TEXT("target_world"), Target.Label);
    Outcome->SetBoolField(TEXT("added_to_viewport"), bAddToViewport && Created->IsInViewport());
    Outcome->SetBoolField(TEXT("widget_valid"), IsValid(Created));
    // WHICH world got the widget -- the wrong client's viewport looks identical otherwise.
    mcp_common::AddWorldEvidence(Outcome, Target.World);

    if (bAddToViewport && !bHasLocalPlayer)
    {
        Outcome->SetStringField(TEXT("add_to_viewport_note"),
            Target.Label == TEXT("editor")
                ? TEXT("add_to_viewport was requested but target_world='editor' has no PlayerController "
                       "(the editor world is never a game world); AddToViewport() was NOT called. Pass "
                       "target_world='pie' with a running PIE session for a real AddToViewport, or use "
                       "read_pie_widgets/click_pie_widget (UEAgentWidgetCommands.cpp) to inspect/drive "
                       "runtime UI that is already on screen.")
                : TEXT("add_to_viewport was requested but no local PlayerController was found in the PIE "
                       "world; AddToViewport() was NOT called."));
    }
    return mcp_common::MakeOkReply(Outcome);
}

// MCP:remove_widget - Detach a runtime UserWidget, found by object name, from the viewport or panel holding it with RemoveFromParent. It searches the worlds create_widget uses: the one target_world names, or without it the running PIE world first and then the editor world; in each, the first widget hosted by a level UWidgetComponent, then any other UserWidget of that world. The object is not deleted, no asset changes and there is no undo. was_in_viewport and had_parent are measured before the call: both false means nothing was detached, as for a component-hosted widget. It never edits a Widget Blueprint asset.
//   @param widget_name - Object name of the runtime widget to detach (case-insensitive), as list_level_widget_components or create_widget report it
//   @param target_world - Optional world to search, with the values create_widget takes (editor, pie, server, client, pie:N, game). Omitted: the PIE world while PIE runs, then the editor world.
//   @returns { widget_name: string, target_world: string, was_in_viewport: bool, had_parent: bool, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleRemoveWidget(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentUICommandsImpl;

    FString TargetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), TargetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The worlds create_widget builds in, resolved the same way: target_world when given;
    // otherwise its default (the PIE world while PIE runs, else the editor world), followed by
    // the editor world so a level-hosted widget is still found while PIE runs.
    FString Selector;
    const bool bHasSelector = Params->TryGetStringField(TEXT("target_world"), Selector);
    Selector.TrimStartAndEndInline();

    TArray<Impl::FWidgetTarget, TInlineAllocator<2>> Targets;
    {
        Impl::FWidgetTarget First;
        const FString TargetError = Impl::ResolveWidgetTarget(bHasSelector, Selector, First);
        if (!TargetError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TargetError);
        }
        Targets.Add(First);
    }
    const bool bAsked = bHasSelector && !Selector.IsEmpty();
    if (!bAsked && Targets[0].Label != TEXT("editor"))
    {
        Impl::FWidgetTarget EditorTarget;
        EditorTarget.Label = TEXT("editor");
        EditorTarget.World = mcp_common::GetEditorWorld();
        if (EditorTarget.World && EditorTarget.World->IsValidLowLevel())
        {
            Targets.Add(EditorTarget);
        }
    }

    // Broader than the other lookups: level widget components first (all of them on every
    // actor), then any UUserWidget object living in that world under that name, such as
    // one made by create_widget.
    UUserWidget* Victim = nullptr;
    const Impl::FWidgetTarget* FoundIn = nullptr;
    TArray<FString> Searched;
    for (const Impl::FWidgetTarget& Target : Targets)
    {
        Searched.Add(Target.Label);
        UWidgetComponent* Host = nullptr;
        Impl::FindWidgetComponentByName(Target.World, TargetName, Host, Victim);
        if (!Victim)
        {
            Victim = Impl::FindLooseUserWidget(Target.World, TargetName);
        }
        if (Victim)
        {
            FoundIn = &Target;
            break;
        }
    }
    if (!Victim)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Widget '%s' not found in the %s world(s)"),
            *TargetName, *FString::Join(Searched, TEXT(" and "))));
    }

    // Both facts are taken before RemoveFromParent(), which changes them: it detaches the
    // widget from the viewport (AddToViewport) or from its parent panel, whichever holds it.
    // The object itself is left to the garbage collector.
    const bool bWasInViewport = Victim->IsInViewport();
    const bool bHadParent = Victim->GetParent() != nullptr;
    Victim->RemoveFromParent();

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("widget_name"), TargetName);
    Outcome->SetStringField(TEXT("target_world"), FoundIn->Label);
    Outcome->SetBoolField(TEXT("was_in_viewport"), bWasInViewport);
    Outcome->SetBoolField(TEXT("had_parent"), bHadParent);
    // WHICH world the widget was found in: a name can exist in both the PIE and the editor world.
    mcp_common::AddWorldEvidence(Outcome, FoundIn->World);
    return mcp_common::MakeOkReply(Outcome);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("create_widget"), &HandleCreateWidget },
    { TEXT("get_level_widget_properties"), &HandleGetWidgetProperties },
    { TEXT("list_level_widget_components"), &HandleListLevelWidgetComponents },
    { TEXT("remove_widget"), &HandleRemoveWidget },
    { TEXT("set_level_widget_visibility"), &HandleSetWidgetVisibility },
};
} // namespace UEAgentUICommands
