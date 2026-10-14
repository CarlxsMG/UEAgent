#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
// JSON applier driven by the JSON (not by the struct) + dotted paths.
// It is what makes it possible to write WidgetStyle.Normal.TintColor and KNOW whether it landed or was ignored.
#include "Commands/Common/McpJsonPropertyApply.h"
// Generic class resolver reused so widget_type can name a UserWidget-derived Widget Blueprint
// (/Game/... asset path) or any native UMG class path, not just the curated palette maps below.
#include "Commands/Common/UEAgentGraphUtils.h"
#include "Commands/UI/UMGWidgetUtils.h"

// Asset / package machinery
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/UObjectGlobals.h"   // GetTransientPackage() (circular-reference rejection cleanup)
#include "Misc/PackageName.h"

// UMGEditor module: Widget Blueprint asset + factory
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"

// Blueprint compile
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"

// UMG runtime module: widget tree + widget types
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"   // play/stop_widget_animation: resolve the LIVE compiled Animations array
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/Image.h"
// set_pie_widget_value / read_pie_widgets(include_values) -- input widget classes.
#include "Components/Slider.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/EditableText.h"
#include "Components/ComboBoxString.h"
#include "Components/ProgressBar.h"
#include "Components/RichTextBlock.h"   // URichTextBlock does NOT derive from UTextBlock
// Gamepad navigation: UWidgetNavigation is an Instanced subobject, not an asset.
#include "Blueprint/WidgetNavigation.h"
// Read and set the real focus: Slate's, not the PlayerController's.
#include "Framework/Application/SlateApplication.h"
#include "Slate/SObjectWidget.h"
#include "Widgets/SWidget.h"
#include "Types/SlateEnums.h"            // EUINavigation
#include "Input/NavigationReply.h"       // EUINavigationRule

// Slate layout types used by canvas slot setters
#include "Layout/Margin.h"
#include "Widgets/Layout/Anchors.h"
#include "Layout/Geometry.h"          // read_pie_widgets(include_geometry): FGeometry
#include "Slate/WidgetTransform.h"    // read_pie_widgets(include_geometry): FWidgetTransform fields
#include "Styling/SlateColor.h"       // SetWidgetPropertyValue FSlateColor pre-check

// WidgetTree structural edits via the SAME headless-safe primitives the Designer's
// F2/drag-reparent/duplicate use (UWidgetBlueprint* + UWidget*, no live FWidgetBlueprintEditor
// required -- see WidgetBlueprintOperationUtils.cpp).
#include "WidgetBlueprintOperationUtils.h"

// MovieScene track/keyframe authoring on a UWidgetAnimation's inner UMovieScene.
#include "Tracks/MovieScenePropertyTrack.h"
#include "Tracks/MovieSceneFloatTrack.h"
#include "Tracks/MovieSceneColorTrack.h"
#include "Tracks/MovieSceneBoolTrack.h"
#include "Channels/MovieSceneFloatChannel.h"
#include "Channels/MovieSceneBoolChannel.h"

// Non-Canvas slot layout (box family + grid). All setters are UMG_API,
// BlueprintCallable and NOT editor-gated; the direct slot members are UE_DEPRECATED(5.1)
// so the typed setters are the only supported authoring path.
#include "Components/SlateWrapperTypes.h"   // FSlateChildSize / ESlateSizeRule
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/BorderSlot.h"
#include "Components/Border.h"          // scaffold_menu_screen: the background panel
#include "Components/ButtonSlot.h"      // scaffold_menu_screen: centre the button label
#include "EdGraph/EdGraph.h"            // scaffold_menu_screen: FGraphNodeCreator
#include "K2Node_ComponentBoundEvent.h" // scaffold_menu_screen: each button's OnClicked
#include "Components/SizeBoxSlot.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/GridSlot.h"

// UMG widget animations: UWidgetAnimation wraps a UMovieScene.
// Property bindings: the validation is the UMG compiler's own, which reports through an
// FCompilerResultsLog; its messages are returned verbatim to the caller.
#include "Kismet2/CompilerResultsLog.h"
#include "Logging/TokenizedMessage.h"
#include "Engine/Blueprint.h"                 // UBlueprint::GetGuidFromClassByFieldName<UFunction>

#include "Animation/WidgetAnimation.h"
#include "MovieScene.h"

// render_widget_preview -- draw a WBP to PNG without starting the game.
// Modules it needs: UMG, Slate, SlateCore, RenderCore, ImageWrapper (all in UEAgent.Build.cs).
#include "Slate/WidgetRenderer.h"            // FWidgetRenderer::DrawWidget
#include "Engine/TextureRenderTarget2D.h"    // UTextureRenderTarget2D render destination
#include "ImageUtils.h"                      // FImageUtils::GetRenderTargetImage / SaveImageByExtension
#include "ImageCore.h"                       // FImage / ERawImageFormat / EGammaSpace
#include "Misc/App.h"                        // FApp::CanEverRender -- the honest -nullrhi gate
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"         // CreateDirectoryTree for the output folder
#include "HAL/FileManager.h"                 // IFileManager::FileSize (byte_size measured, not estimated)

// Live PIE UMG read-back: iterate active UUserWidgets in the running PIE world.
#include "Engine/Engine.h"            // GEngine, FWorldContext
#include "Engine/World.h"             // UWorld, EWorldType
#include "UObject/UObjectIterator.h"  // TObjectIterator<UUserWidget>
// click_pie_widget: fire a live widget's BlueprintAssignable multicast delegate by reflection.
#include "UObject/StructOnScope.h"    // zero-init params frame for delegates that take arguments

/**
 * UMG Widget Blueprint commands.
 *
 * Creates, edits and introspects UWidgetBlueprint assets and their UWidgetTree: widgets and their
 * properties (dotted paths + batch, with value read-back), slots (Canvas, box family and grid shortcuts,
 * plus a generic reflection writer for every other slot type), style presets, design-time PNG previews
 * (FWidgetRenderer; a DESIGN preview, not the HUD), tree edits (rename / reparent / reorder / duplicate),
 * property bindings, navigation, named slots, menu-screen scaffolding, and widget animations with their
 * MovieScene tracks and keys. Also reads and drives the live UMG of a running PIE session: read, click,
 * set value, focus, and animation playback, which falls back to an honest headless gate without PIE.
 *
 * Uses the UMGEditor module (UWidgetBlueprint, UWidgetBlueprintFactory, UWidgetBlueprintGeneratedClass,
 * FWidgetBlueprintOperationUtils) plus the UMG runtime module (UWidgetTree, UPanelWidget,
 * UCanvasPanel/Slot, UTextBlock, UButton, UImage, UVerticalBox, the input-widget classes) and the
 * MovieScene / MovieSceneTracks modules for widget animation authoring.
 *
 * Writing a property, top-level or nested, goes through mcp_common::ApplyJsonPathToObject; why (the
 * engine converter's silent no-op) is explained under "THE ENGINE TRAP BEHIND
 * mcp_common::ApplyJsonPathToObject" below.
 */
namespace UEAgentWidgetCommands
{

// (FMulticastDelegateProperty / FindFProperty come from UObject/UnrealType.h, already included.)
// MCP-DOMAIN: UI

// ---------------------------------------------------------------------------
// Slot helpers -- string<->enum for Slate alignment/size, shared by
// set_box_slot_properties and set_grid_slot_properties. Accept the short tokens
// ("Fill"/"Left"/...) with or without the HAlign_/VAlign_ prefix, case-insensitive.
// ---------------------------------------------------------------------------
namespace
{
    bool ParseHAlign(const FString& In, EHorizontalAlignment& Out)
    {
        FString S = In.TrimStartAndEnd();
        S.RemoveFromStart(TEXT("HAlign_"), ESearchCase::IgnoreCase);
        if (S.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))   { Out = HAlign_Fill;   return true; }
        if (S.Equals(TEXT("Left"), ESearchCase::IgnoreCase))   { Out = HAlign_Left;   return true; }
        if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase) ||
            S.Equals(TEXT("Centre"), ESearchCase::IgnoreCase)) { Out = HAlign_Center; return true; }
        if (S.Equals(TEXT("Right"), ESearchCase::IgnoreCase))  { Out = HAlign_Right;  return true; }
        return false;
    }

    bool ParseVAlign(const FString& In, EVerticalAlignment& Out)
    {
        FString S = In.TrimStartAndEnd();
        S.RemoveFromStart(TEXT("VAlign_"), ESearchCase::IgnoreCase);
        if (S.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))   { Out = VAlign_Fill;   return true; }
        if (S.Equals(TEXT("Top"), ESearchCase::IgnoreCase))    { Out = VAlign_Top;    return true; }
        if (S.Equals(TEXT("Center"), ESearchCase::IgnoreCase) ||
            S.Equals(TEXT("Centre"), ESearchCase::IgnoreCase)) { Out = VAlign_Center; return true; }
        if (S.Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) { Out = VAlign_Bottom; return true; }
        return false;
    }

    FString HAlignToString(EHorizontalAlignment In)
    {
        switch (In)
        {
        case HAlign_Fill:   return TEXT("Fill");
        case HAlign_Left:   return TEXT("Left");
        case HAlign_Center: return TEXT("Center");
        case HAlign_Right:  return TEXT("Right");
        default:            return TEXT("Unknown");
        }
    }

    FString VAlignToString(EVerticalAlignment In)
    {
        switch (In)
        {
        case VAlign_Fill:   return TEXT("Fill");
        case VAlign_Top:    return TEXT("Top");
        case VAlign_Center: return TEXT("Center");
        case VAlign_Bottom: return TEXT("Bottom");
        default:            return TEXT("Unknown");
        }
    }

    bool ParseSizeRule(const FString& In, ESlateSizeRule::Type& Out)
    {
        const FString S = In.TrimStartAndEnd();
        if (S.Equals(TEXT("Fill"), ESearchCase::IgnoreCase))      { Out = ESlateSizeRule::Fill;      return true; }
        if (S.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) ||
            S.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase)) { Out = ESlateSizeRule::Automatic; return true; }
        return false;
    }

    FString SizeRuleToString(ESlateSizeRule::Type In)
    {
        return In == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Automatic");
    }

    // Read a [L,T,R,B] (or [uniform] / [horizontal, vertical]) JSON array into FMargin.
    bool TryGetMarginField(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, FMargin& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(Field, Arr) || !Arr)
        {
            return false;
        }
        const int32 N = Arr->Num();
        if (N == 1)
        {
            Out = FMargin(static_cast<float>((*Arr)[0]->AsNumber()));
            return true;
        }
        if (N == 2)
        {
            Out = FMargin(static_cast<float>((*Arr)[0]->AsNumber()),
                          static_cast<float>((*Arr)[1]->AsNumber()));
            return true;
        }
        if (N >= 4)
        {
            Out = FMargin(static_cast<float>((*Arr)[0]->AsNumber()),
                          static_cast<float>((*Arr)[1]->AsNumber()),
                          static_cast<float>((*Arr)[2]->AsNumber()),
                          static_cast<float>((*Arr)[3]->AsNumber()));
            return true;
        }
        return false;
    }

    TArray<TSharedPtr<FJsonValue>> MarginToJsonArray(const FMargin& M)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        Out.Add(MakeShared<FJsonValueNumber>(M.Left));
        Out.Add(MakeShared<FJsonValueNumber>(M.Top));
        Out.Add(MakeShared<FJsonValueNumber>(M.Right));
        Out.Add(MakeShared<FJsonValueNumber>(M.Bottom));
        return Out;
    }

namespace
{
    /**
     * A pending navigation write. It is a struct and not a nested TPair on purpose:
     * TArray<TPair<A,TPair<B,C>>>::Add({x,{y,z}}) does NOT compile -- TPair is not an aggregate
     * and brace-init finds no overload. A flat struct is, and it also reads better.
     */
    struct FNavWrite_Widget
    {
        EUINavigation      Direction;
        EUINavigationRule  Rule;
        FName              Focus;
    };

    /** The six directions a widget's navigation rules cover. */
    const EUINavigation AllNavDirections_Widget[] = { EUINavigation::Up, EUINavigation::Down,
        EUINavigation::Left, EUINavigation::Right, EUINavigation::Next, EUINavigation::Previous };

    /** "Up"/"down"/"NEXT" -> EUINavigation. Returns false if it is not a direction. */
    bool ParseNavDirection_Widget(const FString& In, EUINavigation& Out)
    {
        const FString S = In.TrimStartAndEnd();
        if (S.Equals(TEXT("Up"), ESearchCase::IgnoreCase))       { Out = EUINavigation::Up;       return true; }
        if (S.Equals(TEXT("Down"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Down;     return true; }
        if (S.Equals(TEXT("Left"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Left;     return true; }
        if (S.Equals(TEXT("Right"), ESearchCase::IgnoreCase))    { Out = EUINavigation::Right;    return true; }
        if (S.Equals(TEXT("Next"), ESearchCase::IgnoreCase))     { Out = EUINavigation::Next;     return true; }
        if (S.Equals(TEXT("Previous"), ESearchCase::IgnoreCase)) { Out = EUINavigation::Previous; return true; }
        return false;
    }

    const TCHAR* NavDirectionToString_Widget(EUINavigation Dir)
    {
        switch (Dir)
        {
        case EUINavigation::Up:       return TEXT("Up");
        case EUINavigation::Down:     return TEXT("Down");
        case EUINavigation::Left:     return TEXT("Left");
        case EUINavigation::Right:    return TEXT("Right");
        case EUINavigation::Next:     return TEXT("Next");
        case EUINavigation::Previous: return TEXT("Previous");
        default:                      return TEXT("Invalid");
        }
    }

    /** "Wrap"/"explicit"/"Stop" -> EUINavigationRule, via the UEnum so there is no hand-kept list. */
    bool ParseNavRule_Widget(const FString& In, EUINavigationRule& Out)
    {
        const UEnum* Enum = StaticEnum<EUINavigationRule>();
        if (!Enum)
        {
            return false;
        }
        const FString S = In.TrimStartAndEnd();
        const int64 ByName = Enum->GetValueByNameString(S, EGetByNameFlags::CaseSensitive);
        if (ByName != INDEX_NONE)
        {
            Out = static_cast<EUINavigationRule>(ByName);
            return true;
        }
        // Second, case-insensitive pass: "wrap" has to work the same as "Wrap".
        const int32 Num = Enum->NumEnums();
        for (int32 i = 0; i < Num; ++i)
        {
            if (Enum->GetNameStringByIndex(i).Equals(S, ESearchCase::IgnoreCase))
            {
                Out = static_cast<EUINavigationRule>(Enum->GetValueByIndex(i));
                return true;
            }
        }
        return false;
    }

    FString NavRuleToString_Widget(EUINavigationRule Rule)
    {
        const UEnum* Enum = StaticEnum<EUINavigationRule>();
        return Enum ? Enum->GetNameStringByValue(static_cast<int64>(Rule)) : FString::FromInt((int32)Rule);
    }

    /**
     * The six directions of a widget as JSON. A widget WITHOUT a navigation subobject is not a
     * gap in the report: all six are emitted as Escape, which is exactly what it will do in game.
     * Returning {} would force the reader to know that "absent" means "Escape".
     */
    TSharedPtr<FJsonObject> WidgetNavigationToJson_Widget(const UWidget* Widget)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        if (!Widget)
        {
            return Out;
        }

        UWidgetNavigation* Nav = Widget->Navigation;
        for (EUINavigation Dir : AllNavDirections_Widget)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            if (Nav)
            {
                const FWidgetNavigationData& Data = Nav->GetNavigationData(Dir);
                Entry->SetStringField(TEXT("rule"), NavRuleToString_Widget(Data.Rule));
                Entry->SetStringField(TEXT("widget_to_focus"), Data.WidgetToFocus.ToString());
            }
            else
            {
                Entry->SetStringField(TEXT("rule"), NavRuleToString_Widget(EUINavigationRule::Escape));
                Entry->SetStringField(TEXT("widget_to_focus"), FString());
            }
            Out->SetObjectField(NavDirectionToString_Widget(Dir), Entry);
        }
        Out->SetBoolField(TEXT("has_authored_navigation"), Nav != nullptr);
        return Out;
    }
}

    // ------------------------------------------------------------------
    // Style writing by DOTTED PATH.
    //
    // WHY THIS BLOCK EXISTS. The engine's converter iterates the STRUCT's fields and
    // looks each one up in the JSON; a name that does not match is not an error to it,
    // it returns success having written ZERO fields (details in the "THE ENGINE TRAP"
    // block further down). For styling that is lethal, because the field touched most
    // when styling -- the TintColor of an FSlateBrush inside an FButtonStyle -- lives
    // TWO levels below any top-level property.
    //
    // mcp_common::ApplyJsonPathToObject inverts the direction of the iteration
    // (it walks the JSON, not the struct) and returns WHAT actually got written.
    // These helpers only wrap it for the two targets in this file -- a UWidget and
    // its UPanelSlot -- and translate the report to JSON without embellishing it.
    // ------------------------------------------------------------------

    /** What a batch write really left done, so it can be reported without embellishment. */
    struct FStyleWriteOutcome_Widget
    {
        mcp_common::FJsonApplyReport Report;
        TArray<FString>              OkPaths;      // requested paths that applied in full
        TArray<FString>              FailedPaths;  // requested paths that did NOT apply
        TSet<FName>                  TouchedRoots; // first segment of every written path
    };

    /**
     * Pushes the change we just wrote via reflection out to the editor.
     *
     * Without this the write is real in memory but the Designer keeps painting the old value
     * until the asset is closed and reopened, and -- what really matters -- the underlying
     * Slate SWidget never finds out: UWidget::SynchronizeProperties() (Widget.h, public and
     * virtual) is exactly the function that pushes the reflected values down to the Slate
     * widget. On a UPanelSlot it does not need to be called by hand because its own
     * PostEditChangeProperty already calls it (PanelSlot.h).
     */
    void NotifyPropertyWritten_Widget(UObject* Target, FName RootPropName)
    {
        if (!Target)
        {
            return;
        }
        // PostEditChangeProperty only exists in the editor. On a UPanelSlot it is also ALL that is
        // needed, because its own already calls SynchronizeProperties() (PanelSlot.h).
        if (FProperty* RootProp = Target->GetClass()->FindPropertyByName(RootPropName))
        {
            FPropertyChangedEvent Evt(RootProp, EPropertyChangeType::ValueSet);
            Target->PostEditChangeProperty(Evt);
        }
        // UWidget::SynchronizeProperties is NOT under WITH_EDITOR (Widget.h) and on a UWidget
        // nobody calls it for us: it is what pushes the reflected value down to the Slate SWidget.
        if (UWidget* AsWidget = Cast<UWidget>(Target))
        {
            AsWidget->SynchronizeProperties();
        }
    }

    /**
     * Writes {dotted path -> value} on Target and notifies each touched root ONCE.
     *
     * A path counts as applied only if the applier left neither an Ignored nor an Error under
     * it: a style JSON where 5 fields land and 1 is misspelled is a PARTIAL FAILURE, and the
     * caller must be able to see it field by field -- which is exactly what the engine's
     * converter cannot tell apart.
     */
    void WriteStylePathMap_Widget(UObject* Target,
                                  const TSharedPtr<FJsonObject>& PathMap,
                                  FStyleWriteOutcome_Widget& Out)
    {
        if (!Target || !PathMap.IsValid())
        {
            return;
        }

        for (const auto& Pair : PathMap->Values)
        {
            // FJsonObject::Values is keyed by UE::FSharedString, which does NOT convert to const
            // FString&: the string has to be materialized, not taken as a borrowed reference.
            const FString Path(*Pair.Key);

            // Watermark to know what ended up under THIS path and not under a previous one.
            const int32 AppliedBefore = Out.Report.Applied.Num();
            const int32 IgnoredBefore = Out.Report.Ignored.Num();
            const int32 ErrorsBefore  = Out.Report.Errors.Num();

            FString ResolveError;
            const bool bOk = mcp_common::ApplyJsonPathToObject(Target, Path, Pair.Value,
                                                               Out.Report, ResolveError);

            const bool bWroteSomething = Out.Report.Applied.Num() > AppliedBefore;
            const bool bCleanUnderPath = Out.Report.Ignored.Num() == IgnoredBefore
                                      && Out.Report.Errors.Num()  == ErrorsBefore;

            if (bOk && bWroteSomething && bCleanUnderPath)
            {
                Out.OkPaths.Add(Path);

                // The root is the first segment, and there are TWO possible separators. Looking
                // only at the dot splits "Items[0].Name" into "Items[0]", which is not the name of
                // any property: FindPropertyByName would fail and the editor would not be notified.
                int32 DotAt = INDEX_NONE;
                int32 BracketAt = INDEX_NONE;
                const bool bHasDot = Path.FindChar(TEXT('.'), DotAt);
                const bool bHasBracket = Path.FindChar(TEXT('['), BracketAt);

                int32 Cut = INDEX_NONE;
                if (bHasDot && bHasBracket) { Cut = FMath::Min(DotAt, BracketAt); }
                else if (bHasDot)           { Cut = DotAt; }
                else if (bHasBracket)       { Cut = BracketAt; }

                const FString RootSegment = (Cut != INDEX_NONE) ? Path.Left(Cut) : Path;
                Out.TouchedRoots.Add(FName(*RootSegment));
            }
            else
            {
                Out.FailedPaths.Add(Path);
                if (!ResolveError.IsEmpty())
                {
                    Out.Report.Errors.Add({Path, ResolveError});
                }
            }
        }

        for (const FName& Root : Out.TouchedRoots)
        {
            NotifyPropertyWritten_Widget(Target, Root);
        }
    }

    /** Dumps the report as is: what was applied, what was ignored and what failed, with the reason. */
    void FillWriteReportJson_Widget(const FStyleWriteOutcome_Widget& Outcome,
                                    const TSharedPtr<FJsonObject>& Resp)
    {
        Resp->SetArrayField(TEXT("applied"), mcp_common::StringsToJson(Outcome.Report.Applied));
        Resp->SetArrayField(TEXT("ignored"), mcp_common::StringsToJson(Outcome.Report.Ignored));

        TArray<TSharedPtr<FJsonValue>> ErrorsArr;
        for (const TPair<FString, FString>& E : Outcome.Report.Errors)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("path"), E.Key);
            Entry->SetStringField(TEXT("error"), E.Value);
            ErrorsArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        Resp->SetArrayField(TEXT("errors"), ErrorsArr);
        Resp->SetArrayField(TEXT("failed_paths"), mcp_common::StringsToJson(Outcome.FailedPaths));

        Resp->SetNumberField(TEXT("applied_count"), Outcome.Report.Applied.Num());
        Resp->SetBoolField(TEXT("all_applied"), Outcome.FailedPaths.Num() == 0
                                             && !Outcome.Report.HasFailures());
    }

    // ------------------------------------------------------------------
    // read_pie_widgets helpers -- live UMG read-back from the PIE session.
    // ------------------------------------------------------------------

    // Forward declaration: the read_pie_widgets include_values/include_geometry enrichment,
    // defined further down in this namespace; needed here since BuildPieWidgetNode calls it.
    void AddPieWidgetValueAndGeometry(const TSharedPtr<FJsonObject>& Node, UWidget* Widget,
                                       bool bIncludeValues, bool bIncludeGeometry);

    /**
     * Serializes a live UWidget and its children. Only reads game-thread state
     * (name/class/visibility/hover/text); never touches the render thread.
     *
     * It DESCENDS INTO NESTED WBPs: a screen is usually a WBP made of WBPs, so the
     * interesting part is one level below the first one. Cycles are avoided with a visited
     * set plus a depth cap.
     *
     * A cut by cap or by cycle is NEVER silent: `subtree_omitted` comes out with the reason,
     * because a node with no children and a node that was not descended into read the same.
     */
    TSharedPtr<FJsonObject> BuildPieWidgetNode(UWidget* Widget, bool bIncludeText, bool bIncludeVisibility,
                                                bool bIncludeValues, bool bIncludeGeometry,
                                                TSet<const UWidget*>& Visited, int32 Depth, int32 MaxDepth)
    {
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        if (!Widget)
        {
            return Node;
        }

        Node->SetStringField(TEXT("widget_name"), Widget->GetName());
        Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

        if (bIncludeVisibility)
        {
            Node->SetStringField(TEXT("visibility"), UMGWidgetUtils::SlateVisibilityName(Widget->GetVisibility()));
            Node->SetBoolField(TEXT("is_hovered"), Widget->IsHovered());
            // Focus is the state that decides whether a navigation key does anything. It goes
            // with visibility because whoever asks "what is visible" almost always also wants to
            // know what is selected -- and it is ONE call instead of two.
            Node->SetBoolField(TEXT("has_keyboard_focus"), Widget->HasKeyboardFocus());
        }

        if (bIncludeText)
        {
            // UTextBlock also covers UCommonTextBlock, which derives from it. URichTextBlock does
            // NOT: it derives from UTextLayoutWidget, so a Cast<UTextBlock> alone misses rich text --
            // which half the UI of an AAA game uses.
            if (UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
            {
                Node->SetStringField(TEXT("text"), TextBlock->GetText().ToString());
            }
            else if (URichTextBlock* RichText = Cast<URichTextBlock>(Widget))
            {
                Node->SetStringField(TEXT("text"), RichText->GetText().ToString());
                Node->SetBoolField(TEXT("is_rich_text"), true);
            }
        }

        AddPieWidgetValueAndGeometry(Node, Widget, bIncludeValues, bIncludeGeometry);

        // Cycle: the same widget twice in the same branch. It is cut, and the response says so.
        bool bAlreadyVisited = false;
        Visited.Add(Widget, &bAlreadyVisited);
        if (bAlreadyVisited)
        {
            Node->SetStringField(TEXT("subtree_omitted"), TEXT("cycle: this widget already appears higher in the tree"));
            Node->SetArrayField(TEXT("children"), TArray<TSharedPtr<FJsonValue>>());
            return Node;
        }

        const bool bDepthExhausted = MaxDepth >= 0 && Depth >= MaxDepth;

        TArray<TSharedPtr<FJsonValue>> Children;

        // (a) Panel children: the usual.
        if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
        {
            const int32 Count = Panel->GetChildrenCount();
            if (bDepthExhausted && Count > 0)
            {
                Node->SetStringField(TEXT("subtree_omitted"),
                    FString::Printf(TEXT("max_depth reached: %d child widget(s) not walked"), Count));
            }
            else
            {
                for (int32 i = 0; i < Count; ++i)
                {
                    if (UWidget* Child = Panel->GetChildAt(i))
                    {
                        Children.Add(MakeShared<FJsonValueObject>(BuildPieWidgetNode(
                            Child, bIncludeText, bIncludeVisibility, bIncludeValues, bIncludeGeometry,
                            Visited, Depth + 1, MaxDepth)));
                    }
                }
            }
        }
        // (b) Nested WBP: its own tree hangs off its WidgetTree, not off GetChildAt.
        else if (UUserWidget* AsUserWidget = Cast<UUserWidget>(Widget))
        {
            UWidget* InnerRoot = AsUserWidget->WidgetTree ? AsUserWidget->WidgetTree->RootWidget : nullptr;
            if (InnerRoot)
            {
                if (bDepthExhausted)
                {
                    Node->SetStringField(TEXT("subtree_omitted"),
                        TEXT("max_depth reached: the nested Widget Blueprint's own tree was not walked"));
                }
                else
                {
                    Node->SetBoolField(TEXT("is_nested_user_widget"), true);
                    Children.Add(MakeShared<FJsonValueObject>(BuildPieWidgetNode(
                        InnerRoot, bIncludeText, bIncludeVisibility, bIncludeValues, bIncludeGeometry,
                        Visited, Depth + 1, MaxDepth)));
                }
            }
        }

        Node->SetArrayField(TEXT("children"), Children);
        return Node;
    }

    /** Wrapper with the default values, so the visited set is not repeated at every call site. */
    TSharedPtr<FJsonObject> BuildPieWidgetNode(UWidget* Widget, bool bIncludeText, bool bIncludeVisibility,
                                                bool bIncludeValues = false, bool bIncludeGeometry = false,
                                                int32 MaxDepth = -1)
    {
        TSet<const UWidget*> Visited;
        return BuildPieWidgetNode(Widget, bIncludeText, bIncludeVisibility, bIncludeValues, bIncludeGeometry,
                                  Visited, /*Depth=*/ 0, MaxDepth);
    }

    // click_pie_widget helper: recursively search a resolved widget's subtree (panel children)
    // for a descendant whose object name matches Name. Used to drill into child_name after the
    // top-level widget_name is resolved. Returns nullptr if not found.
    // It also searches INSIDE nested WBPs, for the same reason as the reader -- the button
    // to press usually lives in a WBP component, not in the outer screen.
    // Without this, click_pie_widget(child_name=...) could never reach it.
    UWidget* FindChildWidgetByName(UWidget* Root, const FName& Name, TSet<const UWidget*>* Visited = nullptr)
    {
        if (!Root)
        {
            return nullptr;
        }

        TSet<const UWidget*> LocalVisited;
        TSet<const UWidget*>& Seen = Visited ? *Visited : LocalVisited;
        bool bAlready = false;
        Seen.Add(Root, &bAlready);
        if (bAlready)
        {
            return nullptr;
        }

        auto Consider = [&](UWidget* Child) -> UWidget*
        {
            if (!Child) { return nullptr; }
            if (Child->GetFName() == Name) { return Child; }
            return FindChildWidgetByName(Child, Name, &Seen);
        };

        if (UPanelWidget* Panel = Cast<UPanelWidget>(Root))
        {
            const int32 Count = Panel->GetChildrenCount();
            for (int32 i = 0; i < Count; ++i)
            {
                if (UWidget* Found = Consider(Panel->GetChildAt(i)))
                {
                    return Found;
                }
            }
        }
        else if (UUserWidget* AsUserWidget = Cast<UUserWidget>(Root))
        {
            UWidget* InnerRoot = AsUserWidget->WidgetTree ? AsUserWidget->WidgetTree->RootWidget : nullptr;
            if (UWidget* Found = Consider(InnerRoot))
            {
                return Found;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Shared PIE-world / live-widget resolution (read_pie_widgets, click_pie_widget,
    // play/stop_widget_animation and set_pie_widget_value).
    // ------------------------------------------------------------------

    // The PIE world the `world` param selects (default 'pie') for a command that cannot run without PIE: null with
    // OutError, prefixed with Command, when the selector resolves to no world.
    UWorld* RequirePieWorld_Widget(const TSharedPtr<FJsonObject>& Params, const TCHAR* Command, FString& OutError,
                                   FString* OutSelector = nullptr)
    {
        FString Selector;
        FString WorldError;
        UWorld* PieWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), Selector, WorldError);
        if (OutSelector)
        {
            *OutSelector = Selector;
        }
        if (!PieWorld)
        {
            OutError = WorldError.IsEmpty()
                ? FString::Printf(TEXT("%s: no Play-In-Editor session is running (start PIE first)"), Command)
                : FString::Printf(TEXT("%s: %s"), Command, *WorldError);
        }
        return PieWorld;
    }

    // Resolve a live UWidget by name among the active viewport UUserWidgets of a PIE world: the
    // top-level UserWidget by its own name, or a descendant found by walking its live WidgetTree.
    // Honors an optional player_index filter (>=0 to filter; -1/negative = no filter) and an
    // optional child_name drill-down into the resolved widget's subtree. Shared by click_pie_widget
    // and set_pie_widget_value (same resolution rules, same error shape).
    UWidget* ResolveLivePieWidget(UWorld* PieWorld, const FName& WidgetName, int32 PlayerIndex,
                                  const FString& ChildName, FString& OutError)
    {
        UWidget* Target = nullptr;
        for (TObjectIterator<UUserWidget> It; It; ++It)
        {
            UUserWidget* UserWidget = *It;
            if (!UMGWidgetUtils::IsLiveViewportWidget(UserWidget, PieWorld, PlayerIndex)) { continue; }

            if (UserWidget->GetFName() == WidgetName)
            {
                Target = UserWidget;
                break;
            }
            if (UserWidget->WidgetTree)
            {
                if (UWidget* Found = UserWidget->WidgetTree->FindWidget(WidgetName))
                {
                    Target = Found;
                    break;
                }
                // UWidgetTree::FindWidget only looks at ITS own tree -- it does not descend into
                // the tree of a nested UserWidget. With the UI built from WBP components (how a
                // real screen is built) that leaves out almost every button:
                // click_pie_widget, set_pie_widget_value and set_pie_widget_focus would say "not
                // found" for a widget that is on screen. The custom finder does descend.
                if (UWidget* Deep = FindChildWidgetByName(UserWidget, WidgetName))
                {
                    Target = Deep;
                    break;
                }
            }
        }

        if (!Target)
        {
            OutError = FString::Printf(
                TEXT("widget '%s' not found in any active viewport UserWidget of the PIE world"),
                *WidgetName.ToString());
            return nullptr;
        }

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
                Child = FindChildWidgetByName(Target, ChildFName);
            }
            if (!Child)
            {
                OutError = FString::Printf(TEXT("child widget '%s' not found under '%s'"),
                    *ChildName, *WidgetName.ToString());
                return nullptr;
            }
            Target = Child;
        }

        return Target;
    }

    // Find a live UUserWidget INSTANCE (as opposed to a named descendant widget) among the active
    // viewport widgets of a PIE world, whose class is WidgetGeneratedClass or a subclass of it.
    // Used by play/stop_widget_animation, which need the OWNING UUserWidget (PlayAnimation /
    // StopAnimation are UUserWidget methods), not just any resolvable descendant.
    UUserWidget* FindLivePieUserWidgetOfClass(UWorld* PieWorld, UClass* WidgetGeneratedClass)
    {
        for (TObjectIterator<UUserWidget> It; It; ++It)
        {
            UUserWidget* UserWidget = *It;
            if (!UMGWidgetUtils::IsLiveViewportWidget(UserWidget, PieWorld)) { continue; }
            if (WidgetGeneratedClass && !UserWidget->GetClass()->IsChildOf(WidgetGeneratedClass)) { continue; }
            return UserWidget;
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // read_pie_widgets(include_values / include_geometry) -- shared enrichment applied both to the
    // top-level UserWidget entry (built inline in HandleReadPieWidgets) and to every nested widget
    // (via BuildPieWidgetNode).
    // ------------------------------------------------------------------
    void AddPieWidgetValueAndGeometry(const TSharedPtr<FJsonObject>& Node, UWidget* Widget,
                                       bool bIncludeValues, bool bIncludeGeometry)
    {
        if (!Widget) { return; }

        if (bIncludeValues)
        {
            Node->SetBoolField(TEXT("is_enabled"), Widget->GetIsEnabled());
            if (USlider* Slider = Cast<USlider>(Widget))
            {
                Node->SetNumberField(TEXT("value"), Slider->GetValue());
            }
            else if (UProgressBar* Bar = Cast<UProgressBar>(Widget))
            {
                Node->SetNumberField(TEXT("percent"), Bar->GetPercent());
            }
            else if (UCheckBox* CheckBox = Cast<UCheckBox>(Widget))
            {
                Node->SetBoolField(TEXT("is_checked"), CheckBox->IsChecked());
            }
            else if (UComboBoxString* Combo = Cast<UComboBoxString>(Widget))
            {
                Node->SetStringField(TEXT("selected_option"), Combo->GetSelectedOption());
            }
        }

        if (bIncludeGeometry)
        {
            TSharedPtr<FJsonObject> GeoNode = MakeShared<FJsonObject>();

            const FGeometry& Geo = Widget->GetCachedGeometry();
            const FVector2D AbsPos = Geo.GetAbsolutePosition();
            const FVector2D AbsSize = Geo.GetAbsoluteSize();
            GeoNode->SetArrayField(TEXT("absolute_position"), mcp_common::Vector2DToJsonArray(AbsPos));
            GeoNode->SetArrayField(TEXT("absolute_size"), mcp_common::Vector2DToJsonArray(AbsSize));
            GeoNode->SetNumberField(TEXT("render_opacity"), Widget->GetRenderOpacity());

            const FWidgetTransform& RT = Widget->GetRenderTransform();
            TSharedPtr<FJsonObject> RTNode = MakeShared<FJsonObject>();
            RTNode->SetArrayField(TEXT("translation"), mcp_common::Vector2DToJsonArray(RT.Translation));
            RTNode->SetArrayField(TEXT("scale"), mcp_common::Vector2DToJsonArray(RT.Scale));
            RTNode->SetArrayField(TEXT("shear"), mcp_common::Vector2DToJsonArray(RT.Shear));
            RTNode->SetNumberField(TEXT("angle"), RT.Angle);
            GeoNode->SetObjectField(TEXT("render_transform"), RTNode);

            Node->SetObjectField(TEXT("geometry"), GeoNode);
        }
    }

    // ------------------------------------------------------------------
    // add_widget_animation_track / add_widget_animation_keyframe -- widget-type catalogs +
    // property-track lookup.
    // ------------------------------------------------------------------

    // The BASIC widget palette (add_widget_to_blueprint). Single source of truth shared with
    // ResolveWidgetClass and list_widget_types(family="basic").
    const TMap<FString, FString>& GetBasicWidgetTypeMap()
    {
        static const TMap<FString, FString> Map = {
            { TEXT("TextBlock"),    TEXT("/Script/UMG.TextBlock") },
            { TEXT("Button"),       TEXT("/Script/UMG.Button") },
            { TEXT("Image"),        TEXT("/Script/UMG.Image") },
            { TEXT("VerticalBox"),  TEXT("/Script/UMG.VerticalBox") },
            { TEXT("CanvasPanel"),  TEXT("/Script/UMG.CanvasPanel") },
        };
        return Map;
    }

    // The EXTENDED widget palette (add_widget_extended). Strict superset of the basic
    // palette. Single source of truth shared with ResolveExtendedWidgetClass
    // and list_widget_types(family="extended", the default).
    const TMap<FString, FString>& GetExtendedWidgetTypeMap()
    {
        static const TMap<FString, FString> Map = []
        {
            TMap<FString, FString> Extended = GetBasicWidgetTypeMap();
            Extended.Append(TMap<FString, FString>{
                { TEXT("ProgressBar"),     TEXT("/Script/UMG.ProgressBar") },
                { TEXT("Slider"),          TEXT("/Script/UMG.Slider") },
                { TEXT("CheckBox"),        TEXT("/Script/UMG.CheckBox") },
                { TEXT("ComboBoxString"),  TEXT("/Script/UMG.ComboBoxString") },
                { TEXT("ScrollBox"),       TEXT("/Script/UMG.ScrollBox") },
                { TEXT("Border"),          TEXT("/Script/UMG.Border") },
                { TEXT("Overlay"),         TEXT("/Script/UMG.Overlay") },
                { TEXT("SizeBox"),         TEXT("/Script/UMG.SizeBox") },
                { TEXT("WidgetSwitcher"),  TEXT("/Script/UMG.WidgetSwitcher") },
                { TEXT("HorizontalBox"),   TEXT("/Script/UMG.HorizontalBox") },
                { TEXT("EditableTextBox"), TEXT("/Script/UMG.EditableTextBox") },
                { TEXT("Spacer"),          TEXT("/Script/UMG.Spacer") },
                // GridPanel, so its UGridSlot children are authorable via set_grid_slot_properties.
                { TEXT("GridPanel"),       TEXT("/Script/UMG.GridPanel") },
                // Non-abstract/constructible: their UCLASS() in
                // Engine/Source/Runtime/UMG/Public/Components/*.h carries no Abstract specifier.
                { TEXT("RichTextBlock"),          TEXT("/Script/UMG.RichTextBlock") },
                { TEXT("EditableText"),           TEXT("/Script/UMG.EditableText") },
                { TEXT("MultiLineEditableText"),  TEXT("/Script/UMG.MultiLineEditableText") },
                { TEXT("ComboBox"),               TEXT("/Script/UMG.ComboBox") },
                { TEXT("SpinBox"),                TEXT("/Script/UMG.SpinBox") },
                { TEXT("ScaleBox"),               TEXT("/Script/UMG.ScaleBox") },
                { TEXT("SafeZone"),               TEXT("/Script/UMG.SafeZone") },
                { TEXT("UniformGridPanel"),       TEXT("/Script/UMG.UniformGridPanel") },
                { TEXT("WrapBox"),                TEXT("/Script/UMG.WrapBox") },
                { TEXT("MenuAnchor"),             TEXT("/Script/UMG.MenuAnchor") },
            });
            return Extended;
        }();
        return Map;
    }

    // Find a property track of TrackClass on Guid whose animated PropertyName matches exactly.
    // MovieScene->FindTrack<T>(Guid) only disambiguates by an explicit TrackName (not by which
    // property it animates), so two different same-typed properties on the same possessable would
    // otherwise collide on the first-found track; this scans FindTracks() and matches on
    // GetPropertyName() instead. Shared by add_widget_animation_track (idempotent reuse) and
    // add_widget_animation_keyframe (exact resolve).
    UMovieScenePropertyTrack* FindWidgetPropertyTrack(UMovieScene* MovieScene, TSubclassOf<UMovieSceneTrack> TrackClass,
                                                       const FGuid& Guid, const FName& PropertyName)
    {
        if (!MovieScene) { return nullptr; }
        for (UMovieSceneTrack* Track : MovieScene->FindTracks(TrackClass, Guid))
        {
            if (UMovieScenePropertyTrack* PropTrack = Cast<UMovieScenePropertyTrack>(Track))
            {
                if (PropTrack->GetPropertyName() == PropertyName)
                {
                    return PropTrack;
                }
            }
        }
        return nullptr;
    }

    // Dual match (object name OR designer display label) used by get/remove/play/stop_widget_animation
    // and add_widget_animation_track / add_widget_animation_keyframe. UWidgetBlueprint::
    // Animations is WITH_EDITORONLY_DATA-gated (WidgetBlueprint.h), so this whole body is gated too
    // (mirrors every other Animations-array access in this file); all callers already only reach it
    // from inside their own #if WITH_EDITOR block.
    UWidgetAnimation* FindWidgetAnimationByName(UWidgetBlueprint* WidgetBP, const FString& AnimationName)
    {
        if (!WidgetBP) { return nullptr; }
        for (UWidgetAnimation* Anim : WidgetBP->Animations)
        {
            if (Anim && (Anim->GetName() == AnimationName || Anim->GetDisplayLabel() == AnimationName))
            {
                return Anim;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------
    // Transitive circular-reference check for embedding a UserWidget (nested Widget Blueprint) into
    // another Widget Blueprint's tree.
    //
    // UWidgetBlueprint::IsWidgetFreeFromCircularReferences(UUserWidget*) -- the engine's
    // own pre-check (used by FWidgetBlueprintEditorUtils before a palette/hierarchy drag-drop is
    // allowed to complete) -- only works on an ALREADY-INITIALIZED UUserWidget instance: it reads
    // UserWidget->WidgetGeneratedBy and UserWidget->WidgetTree, BOTH of which are only populated by
    // UWidgetBlueprintGeneratedClass::InitializeWidgetStatic (real runtime widget creation), NOT by a
    // bare UWidgetTree::ConstructWidget<T>() call (see WidgetBlueprintGeneratedClass.cpp
    // and WidgetTree.cpp / WidgetTemplateClass.cpp -- ConstructWidget is a plain NewObject with neither
    // field set). So calling it on a freshly-constructed instance -- which is the only widget we ever
    // HAVE at this authoring stage -- only ever catches the DIRECT self-embed case
    // (UserWidget->GetClass() == GeneratedClass); embedding A into B then B into A (a
    // genuine transitive cycle) sails through it. The COMPILER'S OWN guard
    // (WidgetBlueprintCompiler.cpp -> UWidgetBlueprint::HasCircularReferences(), which walks the
    // COMPILED class hierarchy) DOES catch the transitive case correctly, but only strips the offending
    // widget from the freshly-compiled GeneratedClass's WidgetTreeArchetype -- NOT from
    // UWidgetBlueprint::WidgetTree (the editable SOURCE tree this file's handlers and
    // get_widget_blueprint_tree read) -- so the widget silently keeps "existing" in the editor/asset
    // (added:true, still listed by get_widget_blueprint_tree) while being invisible at runtime.
    //
    // Hence our own transitive walk BEFORE constructing anything, using only public runtime API
    // (UWidgetBlueprintGeneratedClass::GetWidgetTreeArchetype(), falling back to the SOURCE
    // UWidgetBlueprint::WidgetTree when the class's ClassGeneratedBy resolves one, so an edit made but
    // not yet recompiled is still seen). Subsumes the direct-self-embed case too (EmbeddingClass ==
    // TargetBlueprint->GeneratedClass on the first call).
    bool WouldCreateCircularWidgetReference(const UClass* EmbeddingClass, const UWidgetBlueprint* TargetBlueprint,
                                             TSet<const UClass*>& Visited)
    {
        if (!EmbeddingClass || !TargetBlueprint)
        {
            return false;
        }
        if (TargetBlueprint->GeneratedClass && EmbeddingClass == TargetBlueprint->GeneratedClass)
        {
            return true;
        }
        if (Visited.Contains(EmbeddingClass))
        {
            return false; // already explored this branch (diamond-shaped nesting, not itself a cycle)
        }
        Visited.Add(EmbeddingClass);

        const UWidgetBlueprintGeneratedClass* EmbeddingWBGC = Cast<UWidgetBlueprintGeneratedClass>(EmbeddingClass);
        if (!EmbeddingWBGC)
        {
            return false; // a native (non-Blueprint) UUserWidget subclass can't nest a Widget Blueprint
        }

        UWidgetTree* Tree = nullptr;
        if (const UWidgetBlueprint* SourceBP = Cast<UWidgetBlueprint>(EmbeddingWBGC->ClassGeneratedBy))
        {
            Tree = SourceBP->WidgetTree; // prefer the editable source tree (may be ahead of a stale compile)
        }
        if (!Tree)
        {
            Tree = EmbeddingWBGC->GetWidgetTreeArchetype();
        }
        if (!Tree)
        {
            return false;
        }

        TArray<UWidget*> AllWidgets;
        Tree->GetAllWidgets(AllWidgets);
        for (UWidget* Widget : AllWidgets)
        {
            if (UUserWidget* NestedUserWidget = Cast<UUserWidget>(Widget))
            {
                if (WouldCreateCircularWidgetReference(NestedUserWidget->GetClass(), TargetBlueprint, Visited))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // ------------------------------------------------------------------
    // Steps the authoring handlers share.
    // ------------------------------------------------------------------

    /** A reply object that opens with widget_bp_path and one more string field, Field = Value. */
    TSharedPtr<FJsonObject> StartReply_Widget(const FString& WidgetBPPath, const TCHAR* Field, const FString& Value)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("widget_bp_path"), WidgetBPPath);
        Resp->SetStringField(Field, Value);
        return Resp;
    }

    /** WidgetBP's widget tree, or null with OutError when it has none. */
    UWidgetTree* GetWidgetTree_Widget(UWidgetBlueprint* WidgetBP, FString& OutError)
    {
        if (!WidgetBP->WidgetTree)
        {
            OutError = TEXT("Widget Blueprint has no WidgetTree");
        }
        return WidgetBP->WidgetTree;
    }

    /** The widget named WidgetName in WidgetBP's tree, or null with OutError when the tree or the widget is missing. */
    UWidget* FindWidgetInBlueprint_Widget(UWidgetBlueprint* WidgetBP, const FString& WidgetName, FString& OutError)
    {
        UWidgetTree* WidgetTree = GetWidgetTree_Widget(WidgetBP, OutError);
        if (!WidgetTree)
        {
            return nullptr;
        }
        UWidget* Widget = WidgetTree->FindWidget(FName(*WidgetName));
        if (!Widget)
        {
            OutError = FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName);
        }
        return Widget;
    }

    /**
     * The widget_bp_path and widget_name a command requires, the Widget Blueprint the path names and the widget the
     * name names in its tree: the error reply when a param is missing or either cannot be found.
     */
    TSharedPtr<FJsonObject> ReadWidgetTarget_Widget(const TSharedPtr<FJsonObject>& Params, FString& OutWidgetBPPath,
                                                    FString& OutWidgetName, UWidgetBlueprint*& OutWidgetBP, UWidget*& OutWidget)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), OutWidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), OutWidgetName)) { return Missing; }

        FString Error;
        OutWidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(OutWidgetBPPath, Error);
        OutWidget = OutWidgetBP ? FindWidgetInBlueprint_Widget(OutWidgetBP, OutWidgetName, Error) : nullptr;
        if (!OutWidget)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    /**
     * The widget_bp_path and animation_name an animation command requires and the Widget Blueprint the path names:
     * the error reply when a param is missing or the asset does not load.
     */
    TSharedPtr<FJsonObject> ReadAnimationTarget_Widget(const TSharedPtr<FJsonObject>& Params, FString& OutWidgetBPPath,
                                                       FString& OutAnimationName, UWidgetBlueprint*& OutWidgetBP)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), OutWidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_name"), OutAnimationName)) { return Missing; }

        FString Error;
        OutWidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(OutWidgetBPPath, Error);
        if (!OutWidgetBP)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    /** A widget add_widget_to_blueprint / add_widget_extended put in the tree, or found there under its name. */
    struct FAddedWidget_Widget
    {
        UWidget*      Widget = nullptr;
        UPanelWidget* Parent = nullptr;
        UPanelSlot*   Slot = nullptr;
        bool          bAlreadyExisted = false;
    };

    /**
     * The tree edit of add_widget_to_blueprint and add_widget_extended: constructs WidgetClass as WidgetName under the
     * panel ParentName (the root widget when empty) and registers it as a Blueprint variable. A widget already named
     * WidgetName is returned as it is. False with OutError when the widget cannot be added.
     */
    bool AddWidgetToTree_Widget(UWidgetBlueprint* WidgetBP, UClass* WidgetClass, const FString& WidgetType,
                                const FString& WidgetName, const FString& ParentName,
                                FAddedWidget_Widget& Out, FString& OutError)
    {
        UWidgetTree* WidgetTree = WidgetBP->WidgetTree;

        // Reject embedding a UserWidget that would (directly or transitively, through however many
        // levels of nested Widget Blueprints) contain THIS Widget Blueprint again -- see
        // WouldCreateCircularWidgetReference's comment for why the engine's own instance-based
        // circular-reference check can't be relied on at this authoring stage.
        if (WidgetClass->IsChildOf(UUserWidget::StaticClass()))
        {
            TSet<const UClass*> Visited;
            if (WouldCreateCircularWidgetReference(WidgetClass, WidgetBP, Visited))
            {
                OutError = FString::Printf(TEXT("widget_type '%s' would create a circular reference: it embeds this Widget Blueprint, directly or indirectly"), *WidgetType);
                return false;
            }
        }

        // Idempotent: if a widget with this name already exists, return it (so re-runs
        // and repeated authoring calls succeed instead of hard-failing).
        if (UWidget* ExistingWidget = WidgetTree->FindWidget(FName(*WidgetName)))
        {
            Out.Widget = ExistingWidget;
            Out.Parent = ExistingWidget->GetParent();
            Out.Slot = ExistingWidget->Slot;
            Out.bAlreadyExisted = true;
            return true;
        }

        // Resolve the parent panel: explicit parent_name, else the root widget.
        UPanelWidget* ParentPanel = nullptr;
        if (!ParentName.IsEmpty())
        {
            UWidget* Found = WidgetTree->FindWidget(FName(*ParentName));
            if (!Found)
            {
                OutError = FString::Printf(TEXT("parent_name '%s' not found in widget tree"), *ParentName);
                return false;
            }
            ParentPanel = Cast<UPanelWidget>(Found);
            if (!ParentPanel)
            {
                OutError = FString::Printf(TEXT("parent_name '%s' is not a panel widget and cannot host children"), *ParentName);
                return false;
            }
        }
        else
        {
            ParentPanel = Cast<UPanelWidget>(WidgetTree->RootWidget);
            if (!ParentPanel)
            {
                OutError = TEXT("Root widget is not a panel; specify parent_name pointing at a panel widget");
                return false;
            }
        }

        // Construct the new widget with the WidgetTree as outer (the factory pattern). For a
        // UserWidget-derived WidgetClass this embeds a nested Widget Blueprint instance -- the exact
        // same call the Designer's own palette-drop uses (FWidgetTemplateClass::CreateNamed).
        UWidget* NewWidget = WidgetTree->ConstructWidget<UWidget>(WidgetClass, FName(*WidgetName));
        if (!NewWidget)
        {
            OutError = FString::Printf(TEXT("Failed to construct widget of type %s"), *WidgetType);
            return false;
        }

        UPanelSlot* NewSlot = ParentPanel->AddChild(NewWidget);
        if (!NewSlot)
        {
            OutError = FString::Printf(TEXT("Failed to add widget '%s' under parent '%s'"), *WidgetName, *ParentPanel->GetName());
            return false;
        }

        // Register the new widget as a blueprint variable so it persists / is reachable.
        WidgetBP->OnVariableAdded(NewWidget->GetFName());

        Out.Widget = NewWidget;
        Out.Parent = ParentPanel;
        Out.Slot = NewSlot;
        return true;
    }

    /** The add_widget_to_blueprint / add_widget_extended reply fields; bAdded says whether the tree holds the widget. */
    TSharedPtr<FJsonObject> AddedWidgetJson_Widget(const FString& WidgetBPPath, const FString& WidgetType,
                                                  const FAddedWidget_Widget& Added, bool bAdded)
    {
        TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("added_widget_name"), Added.Widget->GetName());
        Resp->SetStringField(TEXT("widget_type"), WidgetType);
        Resp->SetStringField(TEXT("widget_class"), Added.Widget->GetClass()->GetName());
        Resp->SetStringField(TEXT("parent_name"), Added.Parent ? Added.Parent->GetName() : TEXT(""));
        Resp->SetStringField(TEXT("slot_class"), Added.Slot ? Added.Slot->GetClass()->GetName() : TEXT(""));
        Resp->SetBoolField(TEXT("added"), bAdded);
        return Resp;
    }

    /** The optional horizontal_alignment / vertical_alignment of a box or grid slot. */
    struct FSlotAlignment_Widget
    {
        bool                 bHasH = false;
        EHorizontalAlignment H = HAlign_Fill;
        bool                 bHasV = false;
        EVerticalAlignment   V = VAlign_Fill;
    };

    /** Reads horizontal_alignment / vertical_alignment; false with OutError on a value that names no alignment. */
    bool ReadSlotAlignment_Widget(const TSharedPtr<FJsonObject>& Params, FSlotAlignment_Widget& Out, FString& OutError)
    {
        FString Str;
        if (Params->TryGetStringField(TEXT("horizontal_alignment"), Str))
        {
            if (!ParseHAlign(Str, Out.H))
            {
                OutError = FString::Printf(TEXT("Invalid horizontal_alignment '%s' (use Fill|Left|Center|Right)"), *Str);
                return false;
            }
            Out.bHasH = true;
        }
        if (Params->TryGetStringField(TEXT("vertical_alignment"), Str))
        {
            if (!ParseVAlign(Str, Out.V))
            {
                OutError = FString::Printf(TEXT("Invalid vertical_alignment '%s' (use Fill|Top|Center|Bottom)"), *Str);
                return false;
            }
            Out.bHasV = true;
        }
        return true;
    }

    /**
     * The reply of a dotted-path write on Target: the requested paths, the write report and the TYPED re-read of each
     * requested path -- the real oracle of the call: `applied` says the write met no obstacle, `current_values` says
     * what the asset holds after it.
     */
    void FillPathWriteJson_Widget(UObject* Target, const TSharedPtr<FJsonObject>& PathMap,
                                  const FStyleWriteOutcome_Widget& Outcome, const TSharedPtr<FJsonObject>& Resp)
    {
        TArray<TSharedPtr<FJsonValue>> RequestedArr;
        for (const auto& Pair : PathMap->Values)
        {
            RequestedArr.Add(MakeShared<FJsonValueString>(FString(*Pair.Key)));
        }
        Resp->SetArrayField(TEXT("requested_paths"), RequestedArr);

        FillWriteReportJson_Widget(Outcome, Resp);

        TSharedPtr<FJsonObject> CurrentValues = MakeShared<FJsonObject>();
        for (const auto& Pair : PathMap->Values)
        {
            // FJsonObject::Values is keyed by UE::FSharedString, NOT by FString: it has to be materialized.
            const FString PathKey(*Pair.Key);

            FProperty* LeafProp = nullptr;
            void* LeafPtr = nullptr;
            FString ResolveErr;
            if (mcp_common::ResolvePropertyPath(Target->GetClass(), Target, PathKey, LeafProp, LeafPtr, ResolveErr)
                && LeafProp && LeafPtr)
            {
                if (TSharedPtr<FJsonValue> ReadBack = mcp_common::PropertyValueToJson(LeafProp, LeafPtr))
                {
                    CurrentValues->SetField(PathKey, ReadBack);
                }
            }
        }
        Resp->SetObjectField(TEXT("current_values"), CurrentValues);
    }

    /** A live PIE instance of a Widget Blueprint and its compiled copy of one of the blueprint's animations. */
    struct FLiveWidgetAnimation_Widget
    {
        UWorld*           PieWorld = nullptr;
        FString           WorldSelector;
        UUserWidget*      Instance = nullptr;
        UWidgetAnimation* Animation = nullptr;
    };

    /**
     * play/stop_widget_animation: a live instance of WidgetBP's generated class in the PIE world the `world` param
     * selects, and Target re-resolved by name on that instance's class (the instance plays the compiled copy, not
     * the editor-time object on WidgetBP->Animations).
     */
    FLiveWidgetAnimation_Widget FindLiveWidgetAnimation_Widget(const TSharedPtr<FJsonObject>& Params, UWidgetBlueprint* WidgetBP,
                                                              const UWidgetAnimation* Target, const FString& AnimationName)
    {
        FLiveWidgetAnimation_Widget Live;
        FString WorldError;
        Live.PieWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), Live.WorldSelector, WorldError);
        Live.Instance = (Live.PieWorld && WidgetBP->GeneratedClass)
            ? FindLivePieUserWidgetOfClass(Live.PieWorld, WidgetBP->GeneratedClass) : nullptr;
        if (Live.Instance)
        {
            if (UWidgetBlueprintGeneratedClass* GenClass = Cast<UWidgetBlueprintGeneratedClass>(Live.Instance->GetClass()))
            {
                for (UWidgetAnimation* Anim : GenClass->Animations)
                {
                    if (Anim && (Anim->GetFName() == Target->GetFName() || Anim->GetDisplayLabel() == AnimationName))
                    {
                        Live.Animation = Anim;
                        break;
                    }
                }
            }
        }
        return Live;
    }

    /** The play/stop_widget_animation reply fields that do not depend on whether the animation ran live. */
    TSharedPtr<FJsonObject> LiveAnimationJson_Widget(const FString& WidgetBPPath, const FString& AnimationName,
                                                    const FLiveWidgetAnimation_Widget& Live)
    {
        TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
        Resp->SetBoolField(TEXT("animation_exists"), true);
        // WHICH PIE world was searched for a live instance. With 2 players each client owns
        // its own widgets, so "no live instance found" is only meaningful next to the world read.
        Resp->SetStringField(TEXT("world"), Live.WorldSelector);
        mcp_common::AddWorldEvidence(Resp, Live.PieWorld);
        return Resp;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Generic escape hatch used by BOTH resolvers below once the curated maps miss. Delegates to the
// same class resolver the graph/component/reparent commands already rely on for arbitrary class
// references, so widget_type can additionally be:
//   - a native UMG (or any other module's) widget class path: /Script/UMG.Throbber, /Script/UMG.ListView...
//   - a UserWidget-derived Widget Blueprint, by its /Game asset path ("/Game/UI/WBP_HealthBar") or
//     the fully-qualified generated-class path ("/Game/UI/WBP_HealthBar.WBP_HealthBar_C") -- ResolveClass
//     loads the UBlueprint and returns its GeneratedClass, which IsChildOf(UWidget) accepts.
//   - a bare/native class name (with or without the U prefix), same fallback ResolveClass gives
//     component_type / new_parent_class elsewhere in this plugin.
// Non-abstract/non-deprecated is enforced the same way the Designer's own widget-creation path does
// (FWidgetTemplateClass::CreateNamed rejects CLASS_Abstract | CLASS_Deprecated before constructing).
UClass* ResolveAnyWidgetClass(const FString& WidgetType)
{
    UClass* Resolved = FUEAgentGraphUtils::ResolveClass(WidgetType);
    if (!Resolved || !Resolved->IsChildOf(UWidget::StaticClass()))
    {
        return nullptr;
    }
    if (Resolved->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
    {
        return nullptr;
    }
    return Resolved;
}

// Resolve a UMG widget type name ("TextBlock", "Button", ...) to its UClass.
// A name of the basic palette is looked up by its fully qualified script path
// /Script/UMG.<Name> (FindObject<UClass>(nullptr,"Name") returns null: there is
// no ANY_PACKAGE); any other name falls through to ResolveAnyWidgetClass.
UClass* ResolveWidgetClass(const FString& WidgetType)
{
    if (const FString* ClassPath = GetBasicWidgetTypeMap().Find(WidgetType))
    {
        return LoadObject<UClass>(nullptr, **ClassPath);
    }

    // Not a curated basic name -- fall through to the generic resolver (native class path,
    // or a UserWidget-derived Widget Blueprint by /Game asset path) instead of hard-rejecting.
    return ResolveAnyWidgetClass(WidgetType);
}

// Resolve the EXTENDED widget palette. Same /Script/UMG.<Name> lookup as
// ResolveWidgetClass, but covers the broader supported set. The basic palette
// types are included too so the extended endpoint is a strict superset.
UClass* ResolveExtendedWidgetClass(const FString& WidgetType)
{
    if (const FString* ClassPath = GetExtendedWidgetTypeMap().Find(WidgetType))
    {
        return LoadObject<UClass>(nullptr, **ClassPath);
    }

    // Not a curated extended name -- fall through to the generic resolver.
    return ResolveAnyWidgetClass(WidgetType);
}

// Resolve the requested UserWidget parent class (defaults to UUserWidget).
UClass* ResolveUserWidgetParentClass(const FString& ParentClass)
{
    if (ParentClass.IsEmpty() || ParentClass == TEXT("UUserWidget") || ParentClass == TEXT("UserWidget"))
    {
        return UUserWidget::StaticClass();
    }

    // Try a fully-qualified path first (e.g. /Script/UMG.UserWidget or /Game/...).
    if (UClass* Found = LoadObject<UClass>(nullptr, *ParentClass))
    {
        if (Found->IsChildOf(UUserWidget::StaticClass()))
        {
            return Found;
        }
        return nullptr;
    }

    // Fall back to a slow type search by short name.
    if (UClass* Found = UClass::TryFindTypeSlow<UClass>(ParentClass))
    {
        if (Found->IsChildOf(UUserWidget::StaticClass()))
        {
            return Found;
        }
    }
    return nullptr;
}

// Compile + save the widget blueprint's package to disk. bSave false (a command's save=false) does neither: the
// blueprint stays dirty in memory and OutError is SkipNote.
bool SaveWidgetBlueprint(UWidgetBlueprint* WidgetBP, FString& OutError, bool bSave = true, const TCHAR* SkipNote = TEXT(""))
{
    if (!bSave)
    {
        OutError = SkipNote;
        return false;
    }
    if (!WidgetBP)
    {
        OutError = TEXT("Invalid Widget Blueprint");
        return false;
    }

    // Recompile so the generated class reflects the edited widget tree.
    FKismetEditorUtilities::CompileBlueprint(WidgetBP, EBlueprintCompileOptions::SkipGarbageCollection);

    return mcp_common::SaveAssetPackage(WidgetBP->GetOutermost(), WidgetBP, OutError);
}

// ---------------------------------------------------------------------------
// THE ENGINE TRAP BEHIND mcp_common::ApplyJsonPathToObject
//
// Widget properties are written by mcp_common::ApplyJsonPathToObject, which resolves dotted paths
// and reports field by field, instead of the engine's JSON converter. The trap is in the engine and
// bites anyone who delegates to the converter:
//
// From Engine/Source/Runtime/JsonUtilities/Private/JsonObjectConverter.cpp
// (JsonAttributesToUStructWithContainer), which is where mcp_common::SetObjectProperty ends up:
//   (a) flat FMargin {Left,Top,Right,Bottom} -> WORKS. Its UPROPERTY fields are named
//       exactly that (Layout/Margin.h).
//   (b) flat FLinearColor {R,G,B,A}          -> WORKS. UHT NoExport reflection with fields
//       literally named R/G/B/A (NoExportTypes.h).
//   (c) FSlateColor                            -> WARNING: SILENT NO-OP. Its reflected fields are
//       SpecifiedColor and ColorUseRule (SlateCore/Public/Styling/SlateColor.h), NOT R/G/B/A. The
//       converter iterates THE STRUCT'S FIELDS and looks each one up in the JSON; an {R,G,B,A} --
//       the shape anyone tries first, because it is FLinearColor's -- matches neither of the two,
//       and with bStrictMode=false (what SetObjectProperty passes) it RETURNS TRUE having
//       written ZERO fields. UTextBlock::ColorAndOpacity is of this type.
//   (d) FObjectPropertyBase inside a struct (FSlateBrush::ResourceObject) from a path
//       string -> WORKS the same as at top level (ImportText_Direct -> StaticLoadObject).
//
// Hence the design rule of the applier: ITERATE THE JSON, NOT THE STRUCT. As long as the
// direction is "for each field of the struct, look it up in the JSON", a key that does not match
// is indistinguishable from an absent key, and success is indistinguishable from silence.

// ---------------------------------------------------------------------------
// THE READER -- why the tree carries values, not just names.
//
// {name, class, children} alone cannot answer "what colour does this button have now", and then
// verifying a style is not a state read (reading the resulting value) but someone running PIE and
// looking -- the expensive resource.
//
// Two modes, because they answer two different questions:
//   - property_paths     -> "what is THIS worth exactly", including when it is the default. It is
//                            the oracle that closes a set_widget_property.
//   - include_properties -> "what is particular about this widget", i.e. what DIFFERS from its
//                            archetype. Dumping the ~120 properties of every UWidget would be noise
//                            nobody reads; the touched ones are a handful and they are the signal.
// ---------------------------------------------------------------------------
/**
 * Dumps Obj's properties into Node as TYPED JSON (mcp_common::PropertyValueToJson), not as
 * opaque export text: PropertyPaths always, plus the ones that differ from the archetype
 * when bNonDefaults. Works the same for a UWidget and for its UPanelSlot, which is why it
 * takes UObject* rather than UWidget*.
 */
void FillObjectPropertiesJson(UObject* Obj, const TArray<FString>& PropertyPaths, bool bNonDefaults,
                              const TSharedPtr<FJsonObject>& Node)
{
    if (!Obj || !Node.IsValid())
    {
        return;
    }

    // --- Explicit paths. Always emitted; one that does not resolve is reported, not silenced.
    if (PropertyPaths.Num() > 0)
    {
        TSharedPtr<FJsonObject> Explicit = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Unresolved;
        for (const FString& Path : PropertyPaths)
        {
            FProperty* LeafProp = nullptr;
            void* LeafPtr = nullptr;
            FString ResolveErr;
            if (mcp_common::ResolvePropertyPath(Obj->GetClass(), Obj, Path, LeafProp, LeafPtr, ResolveErr)
                && LeafProp && LeafPtr)
            {
                if (TSharedPtr<FJsonValue> Val = mcp_common::PropertyValueToJson(LeafProp, LeafPtr))
                {
                    Explicit->SetField(Path, Val);
                }
            }
            else
            {
                Unresolved.Add(MakeShared<FJsonValueString>(Path));
            }
        }
        Node->SetObjectField(TEXT("properties"), Explicit);
        if (Unresolved.Num() > 0)
        {
            // A path that does not resolve on THIS widget is often correct on another one (a
            // WidgetStyle does not exist on a UTextBlock): it is information, not a tree failure.
            Node->SetArrayField(TEXT("unresolved_property_paths"), Unresolved);
        }
    }

    if (!bNonDefaults)
    {
        return;
    }

    // --- What differs from the archetype.
    //
    // WidgetTree::ConstructWidget does NewObject WITHOUT passing a Template (WidgetTree.h),
    // so for a widget of a WidgetTree the archetype IS its class's CDO. GetArchetype() is still used
    // instead of GetClass()->GetDefaultObject(): it is what list_instance_overrides already does, and
    // if a widget ever comes from a reinstanced parent WBP the archetype will still be the correct
    // baseline and the CDO will not. Which of the two was used is reported, so nobody has to guess.
    UObject* Archetype = Obj->GetArchetype();
    if (!Archetype || Archetype->GetClass() != Obj->GetClass())
    {
        Archetype = Obj->GetClass()->GetDefaultObject();
    }
    Node->SetBoolField(TEXT("archetype_is_cdo"),
                       Archetype != nullptr && Archetype == Archetype->GetClass()->GetDefaultObject());

    // The same mask the delta engine of list_instance_overrides uses: what is never serialized
    // cannot be an override, and including it here would be guaranteed noise.
    const uint64 NeverSavedMask = CPF_Transient | CPF_DuplicateTransient
                                | CPF_NonPIEDuplicateTransient | CPF_Deprecated;

    TSharedPtr<FJsonObject> NonDefault = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop)
        {
            continue;
        }
        // CPF_Edit is the same filter that decides what shows in the editor's details panel:
        // what is not editable there is not style anyone would want to read either.
        if (!Prop->HasAnyPropertyFlags(CPF_Edit))
        {
            continue;
        }
        if (Prop->HasAnyPropertyFlags(NeverSavedMask))
        {
            continue;
        }
        if (!Archetype)
        {
            continue;
        }

        // The verdict comes from the shared comparator, NOT a bare FProperty::Identical: it tells a
        // real override apart from noise that only changes the exported text (localization identity
        // of an FText, a different outer of a referenced object). Without that distinction half a
        // dozen untouched properties would show up as "touched" on every widget.
        const mcp_common::FPropertyDiffResult Diff =
            mcp_common::ComparePropertyValues(Prop, Obj, Archetype);
        if (Diff.Kind != mcp_common::EPropertyDiffKind::RealOverride
            && Diff.Kind != mcp_common::EPropertyDiffKind::TypeMismatch)
        {
            continue;
        }

        const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);
        if (TSharedPtr<FJsonValue> Val = mcp_common::PropertyValueToJson(Prop, ValuePtr))
        {
            NonDefault->SetField(Prop->GetName(), Val);
        }
    }
    Node->SetObjectField(TEXT("non_default_properties"), NonDefault);
}

// What get_widget_blueprint_tree emits besides the {name,class,children} skeleton.
// Everything is off by default: the bare tree is what name-only callers consume, and
// dumping every property of every widget would be unreadable and expensive.
struct FTreeDumpOptions
{
    bool bIncludeProperties = false;  // the ones that DIFFER from the archetype (the useful signal)
    bool bIncludeSlot       = false;  // UPanelSlot class + its non-default properties
    bool bIncludeNavigation = false;  // the six gamepad navigation directions
    TArray<FString> PropertyPaths;    // explicit paths: ALWAYS emitted, default or not
    int32 MaxDepth = -1;              // -1 = unlimited
};

// Node {name, class, children[]} of a widget, plus whatever the options ask for (recursive).
TSharedPtr<FJsonObject> WidgetToTreeJson(UWidget* Widget, const FTreeDumpOptions& Options, int32 Depth = 0)
{
    TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
    if (!Widget)
    {
        return Node;
    }

    Node->SetStringField(TEXT("name"), Widget->GetName());
    Node->SetStringField(TEXT("class"), Widget->GetClass()->GetName());

    FillObjectPropertiesJson(Widget, Options.PropertyPaths, Options.bIncludeProperties, Node);

    if (Options.bIncludeNavigation)
    {
        Node->SetObjectField(TEXT("navigation"), WidgetNavigationToJson_Widget(Widget));
    }

    if (Options.bIncludeSlot)
    {
        TSharedPtr<FJsonObject> SlotNode = MakeShared<FJsonObject>();
        if (UPanelSlot* Slot = Widget->Slot)
        {
            SlotNode->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetName());
            SlotNode->SetStringField(TEXT("parent"), Slot->Parent ? Slot->Parent->GetName() : TEXT(""));
            // The widget's property_paths are not passed to the slot: they are paths of ANOTHER
            // object and would all come out unresolved, which is noise shaped like an error.
            FillObjectPropertiesJson(Slot, TArray<FString>(), /*bNonDefaults=*/ true, SlotNode);
        }
        else
        {
            SlotNode->SetStringField(TEXT("slot_class"), TEXT(""));
            SlotNode->SetStringField(TEXT("note"),
                TEXT("no slot: this widget is the tree root or is not parented under a panel"));
        }
        Node->SetObjectField(TEXT("slot"), SlotNode);
    }

    TArray<TSharedPtr<FJsonValue>> Children;
    const bool bDepthExhausted = Options.MaxDepth >= 0 && Depth >= Options.MaxDepth;
    if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
    {
        const int32 Count = Panel->GetChildrenCount();
        if (bDepthExhausted)
        {
            // Cutting silently would make a full panel pass for a leaf. Say how many were left out.
            Node->SetNumberField(TEXT("children_omitted"), Count);
        }
        else
        {
            for (int32 i = 0; i < Count; ++i)
            {
                if (UWidget* Child = Panel->GetChildAt(i))
                {
                    Children.Add(MakeShared<FJsonValueObject>(WidgetToTreeJson(Child, Options, Depth + 1)));
                }
            }
        }
    }
    Node->SetArrayField(TEXT("children"), Children);
    return Node;
}

// ---------------------------------------------------------------------------
// create_widget_blueprint
// ---------------------------------------------------------------------------
// MCP:create_widget_blueprint - Create (and save) a new UWidgetBlueprint asset with a CanvasPanel root.
//   @param package_path - /Game folder to create the asset in (e.g. /Game/UI)
//   @param name - Asset name (no extension)
//   @param parent_class - Parent UUserWidget subclass (default "UUserWidget").
//   @returns { name: string, package_path: string, asset_path: string, parent_class: string, created: bool, already_existed: bool, root_created: bool, root_widget: string, root_widget_class: string, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleCreateWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    FString ParentClassName = TEXT("UUserWidget");
    Params->TryGetStringField(TEXT("parent_class"), ParentClassName);

    UClass* ParentClass = ResolveUserWidgetParentClass(ParentClassName);
    if (!ParentClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("parent_class '%s' could not be resolved to a UUserWidget subclass"), *ParentClassName));
    }

    FString NormalizedPath = PackagePath;
    NormalizedPath.RemoveFromEnd(TEXT("/"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *NormalizedPath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load and return it.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        FString LoadErr;
        UWidgetBlueprint* Existing = UMGWidgetUtils::LoadWidgetBlueprint(FullPath, LoadErr);
        if (Existing)
        {
            TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
            Resp->SetStringField(TEXT("name"), Name);
            Resp->SetStringField(TEXT("package_path"), PackagePath);
            Resp->SetStringField(TEXT("asset_path"), Existing->GetPathName());
            Resp->SetStringField(TEXT("parent_class"), ParentClass->GetName());
            Resp->SetBoolField(TEXT("created"), false);
            Resp->SetBoolField(TEXT("already_existed"), true);
            const UWidget* Root = Existing->WidgetTree ? Existing->WidgetTree->RootWidget : nullptr;
            Resp->SetStringField(TEXT("root_widget"), Root ? Root->GetName() : TEXT(""));
            // Mirror the freshly-created payload so callers/tests get a stable shape
            // regardless of idempotent reuse.
            Resp->SetStringField(TEXT("root_widget_class"), Root ? Root->GetClass()->GetName() : TEXT(""));
            Resp->SetBoolField(TEXT("saved"), true);
            return mcp_common::MakeOkReply(Resp);
        }
    }

    // Create via the real editor factory so the generated class + widget tree are
    // set up correctly (FKismetEditorUtilities::CreateBlueprint under the hood).
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
    Factory->ParentClass = ParentClass;

    UObject* NewAsset = AssetTools.CreateAsset(Name, NormalizedPath, UWidgetBlueprint::StaticClass(), Factory);
    UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(NewAsset);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create Widget Blueprint at %s"), *FullPath));
    }

    if (!WidgetBP->WidgetTree)
    {
        return mcp_common::MakeErrorReply(TEXT("Created Widget Blueprint has no WidgetTree"));
    }

    // The factory only creates a root widget when the project's DefaultRootWidget
    // setting is set (it is nullptr by default), so create a CanvasPanel root here
    // when one is missing. The root widget must be outered to the WidgetTree.
    bool bCreatedRoot = false;
    if (WidgetBP->WidgetTree->RootWidget == nullptr)
    {
        UCanvasPanel* RootCanvas = WidgetBP->WidgetTree->ConstructWidget<UCanvasPanel>(
            UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
        if (!RootCanvas)
        {
            return mcp_common::MakeErrorReply(TEXT("Failed to construct root CanvasPanel"));
        }
        WidgetBP->WidgetTree->RootWidget = RootCanvas;
        WidgetBP->OnVariableAdded(RootCanvas->GetFName());
        bCreatedRoot = true;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    const UWidget* RootWidget = WidgetBP->WidgetTree->RootWidget;

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("name"), Name);
    Resp->SetStringField(TEXT("package_path"), PackagePath);
    Resp->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
    Resp->SetStringField(TEXT("parent_class"), ParentClass->GetName());
    Resp->SetBoolField(TEXT("created"), true);
    Resp->SetBoolField(TEXT("already_existed"), false);
    Resp->SetBoolField(TEXT("root_created"), bCreatedRoot);
    Resp->SetStringField(TEXT("root_widget"), RootWidget ? RootWidget->GetName() : TEXT(""));
    Resp->SetStringField(TEXT("root_widget_class"),
        RootWidget ? RootWidget->GetClass()->GetName() : TEXT(""));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("create_widget_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_widget_to_blueprint
// ---------------------------------------------------------------------------
// MCP:add_widget_to_blueprint - Add a widget to a Widget Blueprint's tree under a named panel (or the root).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_type - one of TextBlock, Button, Image, VerticalBox, CanvasPanel (the curated basic names; see list_widget_types), OR any native widget class path (e.g. /Script/UMG.Throbber), OR a UserWidget-derived Widget Blueprint to embed as a nested instance, by its /Game asset path (e.g. /Game/UI/WBP_HealthBar) or generated-class path (/Game/UI/WBP_HealthBar.WBP_HealthBar_C)
//   @param widget_name - unique name for the new widget
//   @param parent_name - Optional name of a panel widget to host the new child; defaults to the root widget.
//   @returns { widget_bp_path: string, added_widget_name: string, widget_type: string, widget_class: string, parent_name: string, slot_class: string, added: bool, already_existed: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddWidgetToBlueprint(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString WidgetType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_type"), WidgetType)) { return Missing; }

    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }

    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName); // optional

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidgetTree* WidgetTree = WidgetBP ? GetWidgetTree_Widget(WidgetBP, Error) : nullptr;
    if (!WidgetTree)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UClass* WidgetClass = ResolveWidgetClass(WidgetType);
    if (!WidgetClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported widget_type '%s'. Supported: the curated basic names (TextBlock, Button, Image, VerticalBox, CanvasPanel -- see list_widget_types), a native widget class path (/Script/Module.ClassName), or a UserWidget-derived Widget Blueprint's /Game asset path"), *WidgetType));
    }

    FAddedWidget_Widget Added;
    if (!AddWidgetToTree_Widget(WidgetBP, WidgetClass, WidgetType, WidgetName, ParentName, Added, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (Added.bAlreadyExisted)
    {
        TSharedPtr<FJsonObject> Resp = AddedWidgetJson_Widget(WidgetBPPath, WidgetType, Added, /*bAdded=*/ true);
        Resp->SetBoolField(TEXT("already_existed"), true);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    // Verify the widget is now findable in the tree.
    UWidget* Verify = WidgetTree->FindWidget(FName(*WidgetName));
    return mcp_common::MakeSavedReply(AddedWidgetJson_Widget(WidgetBPPath, WidgetType, Added, Verify != nullptr),
                                      bSaved, TEXT("add_widget_to_blueprint"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_widget_property
// ---------------------------------------------------------------------------
// MCP:set_widget_property - Set ONE or MANY reflected properties on a widget of a Widget Blueprint, addressing NESTED STYLE FIELDS by dotted path ("WidgetStyle.Normal.TintColor", "WidgetStyle.Hovered.OutlineSettings.CornerRadii") and reporting FIELD BY FIELD what actually landed.  Two calling shapes over one write path: property_name+value writes a single path; `properties` writes a whole map with ONE Modify + ONE compile + ONE save (a FButtonStyle is ~30 leaf fields -- one call, not 30 recompiles). WARNING: The oracle is `applied[]`/`ignored[]`/`errors[]`, NOT success: a JSON key that matches no reflected field lands in `ignored[]` and `all_applied` goes false instead of the silent no-op the engine's converter reports as success. Read the result back with get_widget_blueprint_tree(property_paths=[...]).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the widget in the tree (get_widget_blueprint_tree lists them)
//   @param property_name - Single reflected property name ("Text") OR a dotted path into nested structs ("WidgetStyle.Normal.TintColor"); array indices are supported ("Items[0].Label"). Requires `value`. Omit when using `properties`.
//   @param value - New value for property_name: string, number, bool, JSON object (struct, applied field by field) or JSON array. Colors accept {"R":1,"G":0,"B":0,"A":1}, [r,g,b,a] or "#RRGGBBAA"; FMargin accepts a scalar, [h,v] or [l,t,r,b]; an FSlateBrush accepts a texture/material path string; enums accept the short name ("Box").
//   @param properties [object] - Batch form: {"<dotted path>": <value>, ...}. Every path is written against the SAME widget under one Modify/compile/save. Combinable with property_name+value (both end up in the same map).
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true). Pass false to chain several calls and save once at the end -- the asset stays dirty in memory until something saves it.
//   @returns { widget_bp_path: string, widget_name: string, widget_class: string, requested_paths: [string], applied: [string], ignored: [string], errors: [{path, error}], failed_paths: [string], applied_count: int, all_applied: bool, current_values: object, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleSetWidgetProperty(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }

    // Both calling shapes collapse into ONE map {path -> value}: property_name/value is sugar for
    // a one-pair map. That way there is a single write path to maintain, and the batch is not a
    // parallel path that could diverge from the simple case.
    TSharedPtr<FJsonObject> PathMap = MakeShared<FJsonObject>();

    const TSharedPtr<FJsonObject>* BatchObj = nullptr;
    if (Params->TryGetObjectField(TEXT("properties"), BatchObj) && BatchObj && BatchObj->IsValid())
    {
        for (const auto& Pair : (*BatchObj)->Values)
        {
            PathMap->SetField(FString(*Pair.Key), Pair.Value);
        }
    }

    FString PropertyName;
    if (Params->TryGetStringField(TEXT("property_name"), PropertyName) && !PropertyName.IsEmpty())
    {
        TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
        if (!Value.IsValid())
        {
            return mcp_common::MakeErrorReply(
                TEXT("property_name was given without value. Pass both, or use the properties map."));
        }
        PathMap->SetField(PropertyName, Value);
    }

    if (PathMap->Values.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to write: pass property_name + value, or a properties map {\"path\": value}."));
    }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* Widget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error) : nullptr;
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    Widget->Modify();

    FStyleWriteOutcome_Widget Outcome;
    WriteStylePathMap_Widget(Widget, PathMap, Outcome);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("widget_class"), Widget->GetClass()->GetName());
    FillPathWriteJson_Widget(Widget, PathMap, Outcome, Resp);

    // A VALUE change is not a structural change: MarkBlueprintAsStructurallyModified forces
    // recompiling everything that depends on this WBP too, and styling touches values, not the shape
    // of the tree or a function signature (both in BlueprintEditorUtils.h).
    FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave,
        TEXT("save=false: the Widget Blueprint is modified IN MEMORY and left dirty on purpose; "
             "call again with save=true (or save_asset) or the change dies with the editor."));
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_widget_slot_properties (CanvasPanelSlot anchors / offsets / position / size)
// ---------------------------------------------------------------------------
// MCP:set_widget_slot_properties - Set CanvasPanelSlot layout for a widget parented under a CanvasPanel.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the widget whose canvas slot to edit
//   @param anchors - Optional [MinX, MinY, MaxX, MaxY] anchor floats.
//   @param offsets - Optional [Left, Top, Right, Bottom] offset floats.
//   @param position - Optional [X, Y] position.
//   @param size - Optional [X, Y] size.
//   @param z_order - Optional int draw/hit-test order (higher draws on top); use with reorder_widget_child's note for CanvasPanel stacking.
//   @param auto_size - Optional bool: when true, the slot sizes itself to the widget's desired size instead of `size`.
//   @returns { widget_bp_path: string, widget_name: string, anchors_set: bool, offsets_set: bool, position_set: bool, size_set: bool, z_order_set: bool, auto_size_set: bool, current_anchors: [float], current_offsets: [float], current_position: [float], current_size: [float], current_z_order: int, current_auto_size: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleSetWidgetSlotProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, WidgetName;
    UWidgetBlueprint* WidgetBP = nullptr;
    UWidget* Widget = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadWidgetTarget_Widget(Params, WidgetBPPath, WidgetName, WidgetBP, Widget))
    {
        return Failure;
    }

    UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
    if (!CanvasSlot)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget '%s' is not parented under a CanvasPanel (slot is not a CanvasPanelSlot)"), *WidgetName));
    }

    CanvasSlot->Modify();

    bool bSetAnchors = false;
    bool bSetOffsets = false;
    bool bSetPosition = false;
    bool bSetSize = false;

    // anchors: [MinX, MinY, MaxX, MaxY]
    const TArray<TSharedPtr<FJsonValue>>* AnchorsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("anchors"), AnchorsArr) && AnchorsArr && AnchorsArr->Num() >= 4)
    {
        FAnchors NewAnchors(
            static_cast<float>((*AnchorsArr)[0]->AsNumber()),
            static_cast<float>((*AnchorsArr)[1]->AsNumber()),
            static_cast<float>((*AnchorsArr)[2]->AsNumber()),
            static_cast<float>((*AnchorsArr)[3]->AsNumber()));
        CanvasSlot->SetAnchors(NewAnchors);
        bSetAnchors = true;
    }

    // offsets: [Left, Top, Right, Bottom]  (FMargin)
    const TArray<TSharedPtr<FJsonValue>>* OffsetsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("offsets"), OffsetsArr) && OffsetsArr && OffsetsArr->Num() >= 4)
    {
        FMargin NewOffsets(
            static_cast<float>((*OffsetsArr)[0]->AsNumber()),
            static_cast<float>((*OffsetsArr)[1]->AsNumber()),
            static_cast<float>((*OffsetsArr)[2]->AsNumber()),
            static_cast<float>((*OffsetsArr)[3]->AsNumber()));
        CanvasSlot->SetOffsets(NewOffsets);
        bSetOffsets = true;
    }

    // position: [X, Y]
    const TArray<TSharedPtr<FJsonValue>>* PositionArr = nullptr;
    if (Params->TryGetArrayField(TEXT("position"), PositionArr) && PositionArr && PositionArr->Num() >= 2)
    {
        FVector2D NewPos(
            (*PositionArr)[0]->AsNumber(),
            (*PositionArr)[1]->AsNumber());
        CanvasSlot->SetPosition(NewPos);
        bSetPosition = true;
    }

    // size: [X, Y]
    const TArray<TSharedPtr<FJsonValue>>* SizeArr = nullptr;
    if (Params->TryGetArrayField(TEXT("size"), SizeArr) && SizeArr && SizeArr->Num() >= 2)
    {
        FVector2D NewSize(
            (*SizeArr)[0]->AsNumber(),
            (*SizeArr)[1]->AsNumber());
        CanvasSlot->SetSize(NewSize);
        bSetSize = true;
    }

    // z_order: int (optional)
    bool bSetZOrder = false;
    double ZOrderNum = 0.0;
    if (Params->TryGetNumberField(TEXT("z_order"), ZOrderNum))
    {
        CanvasSlot->SetZOrder(FMath::RoundToInt(ZOrderNum));
        bSetZOrder = true;
    }

    // auto_size: bool (optional)
    bool bSetAutoSize = false;
    bool AutoSizeVal = false;
    if (Params->TryGetBoolField(TEXT("auto_size"), AutoSizeVal))
    {
        CanvasSlot->SetAutoSize(AutoSizeVal);
        bSetAutoSize = true;
    }

    if (!bSetAnchors && !bSetOffsets && !bSetPosition && !bSetSize && !bSetZOrder && !bSetAutoSize)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No slot properties provided. Supply at least one of: anchors, offsets, position, size, z_order, auto_size"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    // Read back the resulting slot layout for verification.
    const FAnchors CurAnchors = CanvasSlot->GetAnchors();
    const FMargin CurOffsets = CanvasSlot->GetOffsets();
    const FVector2D CurPos = CanvasSlot->GetPosition();
    const FVector2D CurSize = CanvasSlot->GetSize();

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetBoolField(TEXT("anchors_set"), bSetAnchors);
    Resp->SetBoolField(TEXT("offsets_set"), bSetOffsets);
    Resp->SetBoolField(TEXT("position_set"), bSetPosition);
    Resp->SetBoolField(TEXT("size_set"), bSetSize);
    Resp->SetBoolField(TEXT("z_order_set"), bSetZOrder);
    Resp->SetBoolField(TEXT("auto_size_set"), bSetAutoSize);

    TArray<TSharedPtr<FJsonValue>> AnchorsOut;
    AnchorsOut.Add(MakeShared<FJsonValueNumber>(CurAnchors.Minimum.X));
    AnchorsOut.Add(MakeShared<FJsonValueNumber>(CurAnchors.Minimum.Y));
    AnchorsOut.Add(MakeShared<FJsonValueNumber>(CurAnchors.Maximum.X));
    AnchorsOut.Add(MakeShared<FJsonValueNumber>(CurAnchors.Maximum.Y));
    Resp->SetArrayField(TEXT("current_anchors"), AnchorsOut);

    Resp->SetArrayField(TEXT("current_offsets"), MarginToJsonArray(CurOffsets));
    Resp->SetArrayField(TEXT("current_position"), mcp_common::Vector2DToJsonArray(CurPos));
    Resp->SetArrayField(TEXT("current_size"), mcp_common::Vector2DToJsonArray(CurSize));

    Resp->SetNumberField(TEXT("current_z_order"), CanvasSlot->GetZOrder());
    Resp->SetBoolField(TEXT("current_auto_size"), CanvasSlot->GetAutoSize());

    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_widget_slot_properties"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_widget_blueprint_tree
// ---------------------------------------------------------------------------
// MCP:get_widget_blueprint_tree - Return a Widget Blueprint's widget tree as nested {name, class, children[]} AND, on request, the actual VALUES of its widgets and slots -- which is what makes verifying a style a cheap read instead of a PIE session. Off by default (a bare tree is what name-only callers want); three switches turn it on: property_paths reads exact dotted paths ("WidgetStyle.Normal.TintColor") on every node whether or not they still hold the default -- this is the oracle that closes a set_widget_property; include_properties reports only what DIFFERS from each widget's archetype, i.e. what this Widget Blueprint actually changed, instead of the ~120 inherited properties nobody reads; include_slot adds each widget's UPanelSlot class plus its own non-default values (position, anchors, padding, row/column). Values come back as TYPED JSON, not UE export-text. widget_name dumps one subtree instead of the whole asset.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - Optional: dump only this widget and its descendants instead of the whole tree.
//   @param property_paths [array] - Optional list of property names or dotted paths read on EVERY node, emitted under "properties" even when the value is still the default. A path that does not exist on a given widget is listed in that node's "unresolved_property_paths" (normal: a WidgetStyle has no meaning on a TextBlock), never silently dropped.
//   @param include_properties [bool] - Emit each widget's editable properties that DIFFER from its archetype, under "non_default_properties" (default false).
//   @param include_slot [bool] - Emit each widget's UPanelSlot class and its non-default slot values under "slot" (default false).
//   @param include_navigation [bool] - Emit each widget's six gamepad/keyboard navigation directions under "navigation" (default false). A widget with no authored navigation reports all six as Escape with has_authored_navigation:false -- which is what it will actually do in game, rather than an empty object the caller has to interpret. Author them with set_widget_navigation.
//   @param max_depth - Optional recursion limit (-1 = unlimited, the default). A panel cut off by the limit reports "children_omitted" instead of passing for a leaf.
//   @returns { widget_bp_path: string, asset_path: string, widget_count: int, root_widget: string, dumped_root: string, include_properties: bool, include_slot: bool, include_navigation: bool, property_paths: [string], tree: {name, class, properties?, unresolved_property_paths?, non_default_properties?, slot?, children: [...], children_omitted?} }

TSharedPtr<FJsonObject> HandleGetWidgetBlueprintTree(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidgetTree* WidgetTree = WidgetBP ? GetWidgetTree_Widget(WidgetBP, Error) : nullptr;
    if (!WidgetTree)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FTreeDumpOptions Options;
    Params->TryGetBoolField(TEXT("include_properties"), Options.bIncludeProperties);
    Params->TryGetBoolField(TEXT("include_slot"), Options.bIncludeSlot);
    Params->TryGetBoolField(TEXT("include_navigation"), Options.bIncludeNavigation);

    double MaxDepthNum = -1.0;
    if (Params->TryGetNumberField(TEXT("max_depth"), MaxDepthNum))
    {
        Options.MaxDepth = FMath::RoundToInt(MaxDepthNum);
    }

    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("property_paths"), PathsArr) && PathsArr)
    {
        for (const TSharedPtr<FJsonValue>& Entry : *PathsArr)
        {
            const FString Path = Entry.IsValid() ? Entry->AsString() : FString();
            if (!Path.IsEmpty())
            {
                Options.PropertyPaths.Add(Path);
            }
        }
    }

    TArray<UWidget*> AllWidgets;
    WidgetTree->GetAllWidgets(AllWidgets);

    // Root of the dump: the tree's, or the requested widget. A widget_name that does not exist is a
    // named error -- returning the whole tree would be answering a different question.
    UWidget* DumpRoot = WidgetTree->RootWidget;
    FString SubtreeName;
    if (Params->TryGetStringField(TEXT("widget_name"), SubtreeName) && !SubtreeName.IsEmpty())
    {
        DumpRoot = FindWidgetInBlueprint_Widget(WidgetBP, SubtreeName, Error);
        if (!DumpRoot)
        {
            return mcp_common::MakeErrorReply(Error);
        }
    }

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("asset_path"), WidgetBP->GetPathName());
    Resp->SetNumberField(TEXT("widget_count"), AllWidgets.Num());
    Resp->SetStringField(TEXT("root_widget"), WidgetTree->RootWidget ? WidgetTree->RootWidget->GetName() : TEXT(""));
    Resp->SetStringField(TEXT("dumped_root"), DumpRoot ? DumpRoot->GetName() : TEXT(""));
    Resp->SetBoolField(TEXT("include_properties"), Options.bIncludeProperties);
    Resp->SetBoolField(TEXT("include_slot"), Options.bIncludeSlot);

    Resp->SetArrayField(TEXT("property_paths"), mcp_common::StringsToJson(Options.PropertyPaths));

    if (DumpRoot)
    {
        Resp->SetObjectField(TEXT("tree"), WidgetToTreeJson(DumpRoot, Options, /*Depth=*/ 0));
    }
    else
    {
        Resp->SetObjectField(TEXT("tree"), MakeShared<FJsonObject>());
    }

    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// add_widget_extended
//
// Same authoring path as add_widget_to_blueprint (ConstructWidget under the
// WidgetTree -> UPanelWidget::AddChild -> OnVariableAdded -> compile + save),
// but supporting the EXTENDED widget-type set resolved via /Script/UMG.<Name>.
// ---------------------------------------------------------------------------
// MCP:add_widget_extended - Add an EXTENDED-palette widget to a Widget Blueprint's tree.  Same authoring path as add_widget_to_blueprint but supports a wider set of widget types (resolved via /Script/UMG.<Name>), PLUS arbitrary native widget classes and UserWidget (nested Widget Blueprint) instances.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_type - one of TextBlock, Button, Image, VerticalBox, CanvasPanel, ProgressBar, Slider, CheckBox, ComboBoxString, ScrollBox, Border, Overlay, SizeBox, WidgetSwitcher, HorizontalBox, EditableTextBox, Spacer, GridPanel, RichTextBlock, EditableText, MultiLineEditableText, ComboBox, SpinBox, ScaleBox, SafeZone, UniformGridPanel, WrapBox, MenuAnchor (the curated names; see list_widget_types), OR any other native widget class path (e.g. /Script/UMG.Throbber, /Script/UMG.ListView), OR a UserWidget-derived Widget Blueprint to embed as a nested instance, by its /Game asset path (e.g. /Game/UI/WBP_HealthBar) or generated-class path (/Game/UI/WBP_HealthBar.WBP_HealthBar_C)
//   @param widget_name - unique name for the new widget
//   @param parent_name - Optional name of a panel widget to host the new child; defaults to the root widget.
//   @returns { widget_bp_path: string, added_widget_name: string, widget_type: string, widget_class: string, parent_name: string, slot_class: string, added: bool, already_existed: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddWidgetExtended(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString WidgetType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_type"), WidgetType)) { return Missing; }

    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }

    FString ParentName;
    Params->TryGetStringField(TEXT("parent_name"), ParentName); // optional

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidgetTree* WidgetTree = WidgetBP ? GetWidgetTree_Widget(WidgetBP, Error) : nullptr;
    if (!WidgetTree)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UClass* WidgetClass = ResolveExtendedWidgetClass(WidgetType);
    if (!WidgetClass)
    {
        TArray<FString> SupportedNames;
        GetExtendedWidgetTypeMap().GetKeys(SupportedNames);
        SupportedNames.Sort();
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported widget_type '%s'. Supported (extended, see list_widget_types): %s -- ")
                TEXT("or pass a native widget class path (/Script/Module.ClassName) or a UserWidget-derived Widget ")
                TEXT("Blueprint's /Game asset path to embed a nested instance"),
                *WidgetType, *FString::Join(SupportedNames, TEXT(", "))));
    }

    FAddedWidget_Widget Added;
    if (!AddWidgetToTree_Widget(WidgetBP, WidgetClass, WidgetType, WidgetName, ParentName, Added, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (Added.bAlreadyExisted)
    {
        TSharedPtr<FJsonObject> Resp = AddedWidgetJson_Widget(WidgetBPPath, WidgetType, Added, /*bAdded=*/ true);
        Resp->SetBoolField(TEXT("already_existed"), true);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    // Verify the widget is now findable in the tree.
    UWidget* Verify = WidgetTree->FindWidget(FName(*WidgetName));
    return mcp_common::MakeSavedReply(AddedWidgetJson_Widget(WidgetBPPath, WidgetType, Added, Verify != nullptr),
                                      bSaved, TEXT("add_widget_extended"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_widget_animation
//
// Creates a fully-valid UWidgetAnimation on the UWidgetBlueprint, mirroring the
// editor's "New Animation" flow (UMGEditor/Private/TabFactory/AnimationTabSummoner.cpp
// FAnimationListView::OnNewAnimationClicked):
//   NewObject<UWidgetAnimation>(WidgetBP, FName(), RF_Transactional)
//   -> Rename to a unique name + SetDisplayLabel
//   -> MovieScene = NewObject<UMovieScene>(Anim, <name>, RF_Transactional)
//   -> SetDisplayRate(20fps) + SetPlaybackRange(0..5s) + WorkStart/WorkEnd
//   -> WidgetBP->Animations.Add(...)
// UWidgetBlueprint::Animations and UWidgetAnimation::MovieScene are both
// WITH_EDITORONLY_DATA but accessible to editor-module code.
// ---------------------------------------------------------------------------
// MCP:add_widget_animation - Add a UWidgetAnimation (with an inner UMovieScene) to a Widget Blueprint.  Mirrors the editor's "New Animation" flow: constructs a UWidgetAnimation on the blueprint, gives it an inner UMovieScene (20fps, 0..5s range), sets the display label, appends to WidgetBP->Animations, then compiles and saves.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param animation_name - name/display label for the new animation
//   @returns { widget_bp_path: string, animation_name: string, object_name: string, display_name: string, added: bool, already_existed: bool, has_movie_scene: bool, animation_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, AnimationName;
    UWidgetBlueprint* WidgetBP = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadAnimationTarget_Widget(Params, WidgetBPPath, AnimationName, WidgetBP))
    {
        return Failure;
    }

    // Idempotent: if an animation with this name already exists, return it.
    if (UWidgetAnimation* Existing = FindWidgetAnimationByName(WidgetBP, AnimationName))
    {
        TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
        Resp->SetStringField(TEXT("object_name"), Existing->GetName());
        Resp->SetBoolField(TEXT("added"), true);
        Resp->SetBoolField(TEXT("already_existed"), true);
        Resp->SetNumberField(TEXT("animation_count"), WidgetBP->Animations.Num());
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    WidgetBP->Modify();

    // Pick a unique object name. MakeUniqueObjectName avoids the Rename-onto-occupied
    // crash if a stale object of this name lingers in the package.
    const FName DesiredName(*AnimationName);
    FName UniqueName = DesiredName;
    if (StaticFindObjectFast(UWidgetAnimation::StaticClass(), WidgetBP, DesiredName) != nullptr)
    {
        UniqueName = MakeUniqueObjectName(WidgetBP, UWidgetAnimation::StaticClass(), DesiredName);
    }

    UWidgetAnimation* NewAnimation = NewObject<UWidgetAnimation>(WidgetBP, UniqueName, RF_Transactional);
    if (!NewAnimation)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to construct UWidgetAnimation"));
    }

    // Friendly designer label = the requested name.
    NewAnimation->SetDisplayLabel(AnimationName);

    // Inner movie scene (outered to the animation), matching the editor flow.
    UMovieScene* MovieScene = NewObject<UMovieScene>(NewAnimation, UniqueName, RF_Transactional);
    if (!MovieScene)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to construct inner UMovieScene"));
    }
    NewAnimation->MovieScene = MovieScene;

    // 20 fps display rate; 0..5s playback range (the editor's defaults).
    const float InTime = 0.0f;
    const float OutTime = 5.0f;
    MovieScene->SetDisplayRate(FFrameRate(20, 1));
    const FFrameTime InFrame = InTime * MovieScene->GetTickResolution();
    const FFrameTime OutFrame = OutTime * MovieScene->GetTickResolution();
    MovieScene->SetPlaybackRange(TRange<FFrameNumber>(InFrame.FrameNumber, OutFrame.FrameNumber + 1));
    MovieScene->GetEditorData().WorkStart = InTime;
    MovieScene->GetEditorData().WorkEnd = OutTime;

    WidgetBP->Animations.Add(NewAnimation);

    // Register the animation as a blueprint variable (widgets + animations share
    // the WidgetVariableNameToGuidMap), then mark structurally modified + save.
    WidgetBP->OnVariableAdded(NewAnimation->GetFName());
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
    Resp->SetStringField(TEXT("object_name"), NewAnimation->GetName());
    Resp->SetStringField(TEXT("display_name"), NewAnimation->GetDisplayLabel());
    Resp->SetBoolField(TEXT("added"), true);
    Resp->SetBoolField(TEXT("already_existed"), false);
    Resp->SetBoolField(TEXT("has_movie_scene"), NewAnimation->MovieScene != nullptr);
    Resp->SetNumberField(TEXT("animation_count"), WidgetBP->Animations.Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_widget_animation"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_widget_animations -- real read of WidgetBP->Animations.
// ---------------------------------------------------------------------------
// MCP:get_widget_animations - List the UWidgetAnimations on a Widget Blueprint.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @returns { widget_bp_path: string, asset_path: string, animations: [{name, display_name, has_movie_scene}], animation_count: int }

TSharedPtr<FJsonObject> HandleGetWidgetAnimations(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TArray<TSharedPtr<FJsonValue>> AnimsOut;
    for (UWidgetAnimation* Anim : WidgetBP->Animations)
    {
        if (!Anim)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("name"), Anim->GetName());
        Node->SetStringField(TEXT("display_name"), Anim->GetDisplayLabel());
        Node->SetBoolField(TEXT("has_movie_scene"), Anim->MovieScene != nullptr);
        AnimsOut.Add(MakeShared<FJsonValueObject>(Node));
    }

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("asset_path"), WidgetBP->GetPathName());
    Resp->SetArrayField(TEXT("animations"), AnimsOut);
    Resp->SetNumberField(TEXT("animation_count"), AnimsOut.Num());
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// remove_widget_animation -- real inverse of add_widget_animation.
// Removes a UWidgetAnimation from WidgetBP->Animations, unregisters it from the
// WidgetVariableNameToGuidMap (OnVariableRemoved, WidgetBlueprint.cpp), then
// marks the blueprint structurally modified and saves. Match is dual: object
// FName OR designer display label. Absent animation = honest no-op (no save).
// ---------------------------------------------------------------------------
// MCP:remove_widget_animation - Remove a UWidgetAnimation from a Widget Blueprint.  Inverse of add_widget_animation: finds the animation by object name or display label, removes it from WidgetBP->Animations, unregisters it as a blueprint variable (OnVariableRemoved), then compiles and saves. If no such animation exists it is an honest no-op (removed:false, already_existed:false, no save).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param animation_name - object name OR display label of the animation to remove
//   @returns { widget_bp_path: string, animation_name: string, object_name: string, removed: bool, already_existed: bool, animation_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleRemoveWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, AnimationName;
    UWidgetBlueprint* WidgetBP = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadAnimationTarget_Widget(Params, WidgetBPPath, AnimationName, WidgetBP))
    {
        return Failure;
    }

    // Locate the animation by dual match (object name OR display label).
    UWidgetAnimation* Target = FindWidgetAnimationByName(WidgetBP, AnimationName);

    // Absent: honest no-op, no save.
    if (!Target)
    {
        TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_existed"), false);
        Resp->SetNumberField(TEXT("animation_count"), WidgetBP->Animations.Num());
        return mcp_common::MakeOkReply(Resp);
    }

    WidgetBP->Modify();
    const FName ObjectName = Target->GetFName();
    WidgetBP->Animations.Remove(Target);

    // Unregister from the shared WidgetVariableNameToGuidMap (widgets + animations),
    // then mark structurally modified + save.
    WidgetBP->OnVariableRemoved(ObjectName);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
    Resp->SetStringField(TEXT("object_name"), ObjectName.ToString());
    Resp->SetBoolField(TEXT("removed"), true);
    Resp->SetBoolField(TEXT("already_existed"), true);
    Resp->SetNumberField(TEXT("animation_count"), WidgetBP->Animations.Num());
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_widget_animation"), SaveErr);
}

// ---------------------------------------------------------------------------
// play_widget_animation -- REAL playback when a live PIE instance of this WidgetBP is found in the
// viewport; honest headless gate otherwise.
//
// The animation object on WidgetBP->Animations is an EDITOR-TIME object; a live UUserWidget's
// class is UWidgetBlueprintGeneratedClass, which carries its OWN COMPILED COPY of the animations
// (WidgetBlueprintGeneratedClass.h: TArray<TObjectPtr<UWidgetAnimation>> Animations) -- a
// DIFFERENT UObject instance than WidgetBP->Animations, produced by the blueprint compiler. Calling
// UUserWidget::PlayAnimation with the wrong (editor-time) object would not reliably drive that
// instance's UMovieSceneSequencePlayer, so this re-resolves the SAME animation (by name, which the
// compiler preserves) on the LIVE instance's generated class before calling PlayAnimation.
// ---------------------------------------------------------------------------
// MCP:play_widget_animation - Play a widget animation. REAL playback (UUserWidget::PlayAnimation) when a live PIE instance of this WidgetBP is found in the viewport's active UUserWidgets; falls back to an honest headless gate (played:false, headless:true, requires_pie:true) when no PIE session is running or no live instance is found.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param animation_name - object name OR display label of the animation to play
//   @param start_at_time - Optional float seconds to start playback at (default 0).
//   @param num_loops - Optional int number of loops (default 1).
//   @param play_mode - Optional Forward | Reverse | PingPong (default Forward).
//   @param playback_speed - Optional float speed multiplier (default 1.0).
//   @param restore_state - Optional bool: restore the pre-animation state on finish (default false).
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). In a 2-player PIE session EVERY client has its OWN widgets, so the default may well read the other player's HUD.
//   @returns { widget_bp_path: string, animation_name: string, animation_exists: bool, played: bool, headless: bool, requires_pie: bool, note: string, saved: bool, live_widget_name?: string, live_animation_name?: string, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandlePlayWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, AnimationName;
    UWidgetBlueprint* WidgetBP = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadAnimationTarget_Widget(Params, WidgetBPPath, AnimationName, WidgetBP))
    {
        return Failure;
    }

    // Validate the animation exists by dual match (object name OR display label).
    UWidgetAnimation* Target = FindWidgetAnimationByName(WidgetBP, AnimationName);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget animation '%s' not found on %s"), *AnimationName, *WidgetBPPath));
    }

    // Try to find a live instance in a running PIE session and play for real.
    const FLiveWidgetAnimation_Widget Live = FindLiveWidgetAnimation_Widget(Params, WidgetBP, Target, AnimationName);
    TSharedPtr<FJsonObject> Resp = LiveAnimationJson_Widget(WidgetBPPath, AnimationName, Live);

    if (Live.Instance && Live.Animation)
    {
        double StartAtTime = 0.0;
        Params->TryGetNumberField(TEXT("start_at_time"), StartAtTime);
        double NumLoopsNum = 1.0;
        Params->TryGetNumberField(TEXT("num_loops"), NumLoopsNum);
        FString PlayModeStr = TEXT("Forward");
        Params->TryGetStringField(TEXT("play_mode"), PlayModeStr);
        double PlaybackSpeed = 1.0;
        Params->TryGetNumberField(TEXT("playback_speed"), PlaybackSpeed);
        bool bRestoreState = false;
        Params->TryGetBoolField(TEXT("restore_state"), bRestoreState);

        EUMGSequencePlayMode::Type PlayMode = EUMGSequencePlayMode::Forward;
        if (PlayModeStr.Equals(TEXT("Reverse"), ESearchCase::IgnoreCase)) { PlayMode = EUMGSequencePlayMode::Reverse; }
        else if (PlayModeStr.Equals(TEXT("PingPong"), ESearchCase::IgnoreCase)) { PlayMode = EUMGSequencePlayMode::PingPong; }

        Live.Instance->PlayAnimation(Live.Animation, static_cast<float>(StartAtTime),
            FMath::Max(1, FMath::RoundToInt(NumLoopsNum)), PlayMode, static_cast<float>(PlaybackSpeed), bRestoreState);

        Resp->SetBoolField(TEXT("played"), true);
        Resp->SetBoolField(TEXT("headless"), false);
        Resp->SetBoolField(TEXT("requires_pie"), false);
        Resp->SetStringField(TEXT("live_widget_name"), Live.Instance->GetName());
        Resp->SetStringField(TEXT("live_animation_name"), Live.Animation->GetName());
        Resp->SetStringField(TEXT("note"), TEXT("Played on a live PIE instance via UUserWidget::PlayAnimation."));
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }

    // Fallback: honest headless gate (no PIE session, or no live instance of this WidgetBP found).
    Resp->SetBoolField(TEXT("played"), false);
    Resp->SetBoolField(TEXT("headless"), true);
    Resp->SetBoolField(TEXT("requires_pie"), true);
    Resp->SetStringField(TEXT("note"),
        Live.PieWorld
            ? TEXT("A PIE session is running, but no active viewport UUserWidget of this Widget Blueprint's "
                   "generated class was found (it may not be constructed/added to viewport yet).")
            : TEXT("PlayAnimation requires a live UUserWidget instance (runtime/PIE); no PIE session is "
                   "running. Wire autoplay via an Event Construct -> Play Animation node, or call this in a "
                   "PIE session."));
    Resp->SetBoolField(TEXT("saved"), false);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// stop_widget_animation -- REAL stop (mirrors play_widget_animation) when a live PIE instance of
// this WidgetBP is found; honest headless gate otherwise.
// ---------------------------------------------------------------------------
// MCP:stop_widget_animation - Stop a widget animation. REAL stop (UUserWidget::StopAnimation) when a live PIE instance of this WidgetBP is found in the viewport's active UUserWidgets; falls back to an honest headless gate (stopped:false, headless:true, requires_pie:true) otherwise.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param animation_name - object name OR display label of the animation to stop
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). In a 2-player PIE session EVERY client has its OWN widgets, so the default may well read the other player's HUD.
//   @returns { widget_bp_path: string, animation_name: string, animation_exists: bool, stopped: bool, headless: bool, requires_pie: bool, note: string, saved: bool, live_widget_name?: string, live_animation_name?: string, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleStopWidgetAnimation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, AnimationName;
    UWidgetBlueprint* WidgetBP = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadAnimationTarget_Widget(Params, WidgetBPPath, AnimationName, WidgetBP))
    {
        return Failure;
    }

    UWidgetAnimation* Target = FindWidgetAnimationByName(WidgetBP, AnimationName);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget animation '%s' not found on %s"), *AnimationName, *WidgetBPPath));
    }

    const FLiveWidgetAnimation_Widget Live = FindLiveWidgetAnimation_Widget(Params, WidgetBP, Target, AnimationName);
    TSharedPtr<FJsonObject> Resp = LiveAnimationJson_Widget(WidgetBPPath, AnimationName, Live);

    if (Live.Instance && Live.Animation)
    {
        Live.Instance->StopAnimation(Live.Animation);

        Resp->SetBoolField(TEXT("stopped"), true);
        Resp->SetBoolField(TEXT("headless"), false);
        Resp->SetBoolField(TEXT("requires_pie"), false);
        Resp->SetStringField(TEXT("live_widget_name"), Live.Instance->GetName());
        Resp->SetStringField(TEXT("live_animation_name"), Live.Animation->GetName());
        Resp->SetStringField(TEXT("note"), TEXT("Stopped on a live PIE instance via UUserWidget::StopAnimation."));
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }

    Resp->SetBoolField(TEXT("stopped"), false);
    Resp->SetBoolField(TEXT("headless"), true);
    Resp->SetBoolField(TEXT("requires_pie"), true);
    Resp->SetStringField(TEXT("note"),
        Live.PieWorld
            ? TEXT("A PIE session is running, but no active viewport UUserWidget of this Widget Blueprint's "
                   "generated class was found.")
            : TEXT("StopAnimation requires a live UUserWidget with an active animation player (runtime/PIE); "
                   "no PIE session is running."));
    Resp->SetBoolField(TEXT("saved"), false);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_box_slot_properties
// Configure the layout of a widget parented under a box-family panel
// (HorizontalBox/VerticalBox/Overlay/Border/SizeBox/ScrollBox). Closes the
// creator/configurer asymmetry: add_widget_extended can CREATE these panels and
// parent children under them, but set_widget_slot_properties only handles
// CanvasPanelSlot. Dispatches by concrete slot type via the typed UMG_API setters
// (the direct slot members are UE_DEPRECATED(5.1)); a prop a given slot does not
// support (Size on Overlay/Border/SizeBox) is an honest note, not a silent drop.
// Setting the same values again is naturally idempotent.
// ---------------------------------------------------------------------------
// MCP:set_box_slot_properties - Configure a widget's box-family slot layout (alignment/padding/size).  Applies when the widget is parented under a HorizontalBox/VerticalBox/Overlay/Border/SizeBox/ScrollBox; dispatches by the concrete slot type. Size only applies to Horizontal/Vertical/ScrollBox slots -- on the others it is returned as an honest note. For a CanvasPanel child use set_widget_slot_properties; for a GridPanel child use set_grid_slot_properties.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the child widget whose box slot to edit
//   @param horizontal_alignment - Optional alignment: Fill | Left | Center | Right
//   @param vertical_alignment - Optional alignment: Fill | Top | Center | Bottom
//   @param padding [array] - Optional [Left, Top, Right, Bottom] floats (also accepts [uniform] or [horizontal, vertical]).
//   @param size_rule - Optional Fill | Auto (Horizontal/Vertical/ScrollBox slots only).
//   @param size_value - Optional float fill weight (used with size_rule = Fill; default 1.0).
//   @returns { widget_bp_path: string, widget_name: string, slot_type: string, settings_applied: object, applied_count: int, notes: [string], saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleSetBoxSlotProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, WidgetName;
    UWidgetBlueprint* WidgetBP = nullptr;
    UWidget* Widget = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadWidgetTarget_Widget(Params, WidgetBPPath, WidgetName, WidgetBP, Widget))
    {
        return Failure;
    }
    UPanelSlot* Slot = Widget->Slot;
    if (!Slot)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget '%s' has no slot (it is not parented under a panel)"), *WidgetName));
    }

    // Parse + validate inputs up-front (type-independent).
    FString Error;
    FSlotAlignment_Widget Align;
    if (!ReadSlotAlignment_Widget(Params, Align, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FMargin Pad;
    const bool bHasPad = TryGetMarginField(Params, TEXT("padding"), Pad);

    bool bHasSize = false;
    FSlateChildSize SizeVal;
    {
        FString RuleStr;
        double ValNum = 0.0;
        const bool bHasRule = Params->TryGetStringField(TEXT("size_rule"), RuleStr);
        const bool bHasVal = Params->TryGetNumberField(TEXT("size_value"), ValNum);
        if (bHasRule || bHasVal)
        {
            ESlateSizeRule::Type Rule = ESlateSizeRule::Fill;
            if (bHasRule && !ParseSizeRule(RuleStr, Rule))
            {
                return mcp_common::MakeErrorReply(
                    FString::Printf(TEXT("Invalid size_rule '%s' (use Fill|Auto)"), *RuleStr));
            }
            SizeVal.SizeRule = Rule;
            SizeVal.Value = bHasVal ? static_cast<float>(ValNum) : 1.0f;
            bHasSize = true;
        }
    }

    if (!Align.bHasH && !Align.bHasV && !bHasPad && !bHasSize)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No slot properties provided. Supply at least one of: horizontal_alignment, vertical_alignment, padding, size_rule/size_value"));
    }

    Slot->Modify();
    int32 AppliedCount = 0;
    TArray<FString> Notes;
    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();

    // The three properties every box-family slot supports (duck-typed across the
    // concrete slot classes, which share these method names but no common base).
    auto ApplyCommon = [&](auto* S)
    {
        if (Align.bHasH) { S->SetHorizontalAlignment(Align.H); ++AppliedCount; }
        if (Align.bHasV) { S->SetVerticalAlignment(Align.V); ++AppliedCount; }
        if (bHasPad) { S->SetPadding(Pad); ++AppliedCount; }
    };
    auto ReadCommon = [&](auto* S)
    {
        Applied->SetStringField(TEXT("horizontal_alignment"), HAlignToString(S->GetHorizontalAlignment()));
        Applied->SetStringField(TEXT("vertical_alignment"), VAlignToString(S->GetVerticalAlignment()));
        Applied->SetArrayField(TEXT("padding"), MarginToJsonArray(S->GetPadding()));
    };
    // Size lives only on Horizontal/Vertical/ScrollBox slots; keep it out of the
    // generic ReadCommon so the lambdas never instantiate GetSize() on a slot
    // class that lacks it.
    auto ApplySize = [&](auto* S) { if (bHasSize) { S->SetSize(SizeVal); ++AppliedCount; } };
    auto ReadSize = [&](auto* S)
    {
        const FSlateChildSize Sz = S->GetSize();
        Applied->SetStringField(TEXT("size_rule"), SizeRuleToString(Sz.SizeRule));
        Applied->SetNumberField(TEXT("size_value"), Sz.Value);
    };

    // Distinct branch names (not a shared `S`) -- UE builds with /WX and an
    // else-if chain reusing one name trips C4456 (declaration hides previous).
    bool bHandled = false;
    if (UHorizontalBoxSlot* HBox = Cast<UHorizontalBoxSlot>(Slot))
    {
        ApplyCommon(HBox); ApplySize(HBox); ReadCommon(HBox); ReadSize(HBox); bHandled = true;
    }
    else if (UVerticalBoxSlot* VBox = Cast<UVerticalBoxSlot>(Slot))
    {
        ApplyCommon(VBox); ApplySize(VBox); ReadCommon(VBox); ReadSize(VBox); bHandled = true;
    }
    else if (UScrollBoxSlot* SBox = Cast<UScrollBoxSlot>(Slot))
    {
        ApplyCommon(SBox); ApplySize(SBox); ReadCommon(SBox); ReadSize(SBox); bHandled = true;
    }
    else if (UOverlaySlot* OvSlot = Cast<UOverlaySlot>(Slot))
    {
        ApplyCommon(OvSlot);
        if (bHasSize) { Notes.Add(TEXT("size: OverlaySlot has no Size property (ignored)")); }
        ReadCommon(OvSlot); bHandled = true;
    }
    else if (UBorderSlot* BdSlot = Cast<UBorderSlot>(Slot))
    {
        ApplyCommon(BdSlot);
        if (bHasSize) { Notes.Add(TEXT("size: BorderSlot has no Size property (ignored)")); }
        ReadCommon(BdSlot); bHandled = true;
    }
    else if (USizeBoxSlot* SzSlot = Cast<USizeBoxSlot>(Slot))
    {
        ApplyCommon(SzSlot);
        if (bHasSize) { Notes.Add(TEXT("size: SizeBoxSlot has no Size property (ignored)")); }
        ReadCommon(SzSlot); bHandled = true;
    }

    if (!bHandled)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget '%s' slot is a %s, not a box-family slot. set_box_slot_properties supports Horizontal/Vertical/Overlay/Border/SizeBox/ScrollBox slots; use set_widget_slot_properties for a CanvasPanelSlot or set_grid_slot_properties for a GridSlot"),
                *WidgetName, *Slot->GetClass()->GetName()));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());
    Resp->SetObjectField(TEXT("settings_applied"), Applied);
    Resp->SetNumberField(TEXT("applied_count"), AppliedCount);
    Resp->SetArrayField(TEXT("notes"), mcp_common::StringsToJson(Notes));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_box_slot_properties"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_grid_slot_properties
// Configure the UGridSlot of a widget parented under a GridPanel (create one via
// add_widget_extended widget_type="GridPanel"). Uses the typed UMG_API setters
// (the direct members are UE_DEPRECATED(5.1)). Idempotent.
// ---------------------------------------------------------------------------
// MCP:set_grid_slot_properties - Configure a widget's UGridSlot layout (row/column/spans/alignment/padding/layer/nudge).  Applies when the widget is parented under a GridPanel (create one with add_widget_extended widget_type="GridPanel"). Setting the same values again is idempotent.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the child widget whose grid slot to edit
//   @param row - Optional int row index of the cell
//   @param column - Optional int column index of the cell
//   @param row_span - Optional int number of rows this slot spans
//   @param column_span - Optional int number of columns this slot spans
//   @param horizontal_alignment - Optional alignment: Fill | Left | Center | Right
//   @param vertical_alignment - Optional alignment: Fill | Top | Center | Bottom
//   @param padding [array] - Optional [Left, Top, Right, Bottom] floats.
//   @param layer - Optional int draw/hit-test layer offset (positive draws on top).
//   @param nudge - Optional [X, Y] content offset.
//   @returns { widget_bp_path: string, widget_name: string, slot_type: string, settings_applied: object, applied_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleSetGridSlotProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, WidgetName;
    UWidgetBlueprint* WidgetBP = nullptr;
    UWidget* Widget = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadWidgetTarget_Widget(Params, WidgetBPPath, WidgetName, WidgetBP, Widget))
    {
        return Failure;
    }
    UGridSlot* Grid = Cast<UGridSlot>(Widget->Slot);
    if (!Grid)
    {
        const FString SlotType = Widget->Slot ? Widget->Slot->GetClass()->GetName() : TEXT("none");
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget '%s' slot is a %s, not a GridSlot. set_grid_slot_properties requires the widget to be parented under a GridPanel"),
                *WidgetName, *SlotType));
    }

    // Parse + validate alignment up-front.
    FString Error;
    FSlotAlignment_Widget Align;
    if (!ReadSlotAlignment_Widget(Params, Align, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    Grid->Modify();
    int32 AppliedCount = 0;
    double Num = 0.0;
    if (Params->TryGetNumberField(TEXT("row"), Num))         { Grid->SetRow(FMath::RoundToInt(Num)); ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("column"), Num))      { Grid->SetColumn(FMath::RoundToInt(Num)); ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("row_span"), Num))    { Grid->SetRowSpan(FMath::RoundToInt(Num)); ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("column_span"), Num)) { Grid->SetColumnSpan(FMath::RoundToInt(Num)); ++AppliedCount; }
    if (Params->TryGetNumberField(TEXT("layer"), Num))       { Grid->SetLayer(FMath::RoundToInt(Num)); ++AppliedCount; }
    if (Align.bHasH) { Grid->SetHorizontalAlignment(Align.H); ++AppliedCount; }
    if (Align.bHasV) { Grid->SetVerticalAlignment(Align.V); ++AppliedCount; }

    FMargin Pad;
    if (TryGetMarginField(Params, TEXT("padding"), Pad)) { Grid->SetPadding(Pad); ++AppliedCount; }

    const TArray<TSharedPtr<FJsonValue>>* NudgeArr = nullptr;
    if (Params->TryGetArrayField(TEXT("nudge"), NudgeArr) && NudgeArr && NudgeArr->Num() >= 2)
    {
        Grid->SetNudge(FVector2D((*NudgeArr)[0]->AsNumber(), (*NudgeArr)[1]->AsNumber()));
        ++AppliedCount;
    }

    if (AppliedCount == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No slot properties provided. Supply at least one of: row, column, row_span, column_span, horizontal_alignment, vertical_alignment, padding, layer, nudge"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    // Full read-back for round-trip verification.
    TSharedPtr<FJsonObject> Applied = MakeShared<FJsonObject>();
    Applied->SetNumberField(TEXT("row"), Grid->GetRow());
    Applied->SetNumberField(TEXT("column"), Grid->GetColumn());
    Applied->SetNumberField(TEXT("row_span"), Grid->GetRowSpan());
    Applied->SetNumberField(TEXT("column_span"), Grid->GetColumnSpan());
    Applied->SetNumberField(TEXT("layer"), Grid->GetLayer());
    Applied->SetStringField(TEXT("horizontal_alignment"), HAlignToString(Grid->GetHorizontalAlignment()));
    Applied->SetStringField(TEXT("vertical_alignment"), VAlignToString(Grid->GetVerticalAlignment()));
    Applied->SetArrayField(TEXT("padding"), MarginToJsonArray(Grid->GetPadding()));
    const FVector2D Ndg = Grid->GetNudge();
    Applied->SetArrayField(TEXT("nudge"), mcp_common::Vector2DToJsonArray(Ndg));

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("slot_type"), Grid->GetClass()->GetName());
    Resp->SetObjectField(TEXT("settings_applied"), Applied);
    Resp->SetNumberField(TEXT("applied_count"), AppliedCount);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("set_grid_slot_properties"), SaveErr);
}

// ---------------------------------------------------------------------------
// read_pie_widgets -- live UMG read-back from the running PIE session.
//
// Enumerates the active UUserWidgets in the PIE world (added to the viewport)
// and walks each one's live UWidgetTree, emitting name/class + optional
// visibility/hover/text per widget as a parent/child tree. This reads only
// game-thread-safe widget-tree state; it NEVER touches the render thread.
//
// PIE world: the canonical world resolution (RequirePieWorld_Widget); with no PIE running
// the answer is a clear error.
// Active widgets: TObjectIterator<UUserWidget> filtered to the PIE world and to
// UWidget::IsInViewport() (top-level UI actually on screen), skipping CDOs/archetypes.
//
// Limitations:
//   - Only UTextBlock text is captured; other text-bearing widgets are reported by
//     class only.
//   - include_bindings is accepted but not implemented (runtime property/delegate
//     binding introspection is omitted); a top-level note flags this when requested.
// ---------------------------------------------------------------------------
// MCP:read_pie_widgets - Read LIVE UMG from the running PIE session (game thread): enumerate the active UUserWidgets added to the viewport and walk each widget tree, emitting name/class plus optional visibility/hover/text/value/geometry as a parent/child tree.  Reads only game-thread-safe widget-tree state (never the render thread). Errors clearly if no Play-In-Editor session is running.  DESCENDS INTO NESTED WIDGET BLUEPRINTS: an AAA screen is a Widget Blueprint composed of Widget Blueprints, so everything interesting sits one level below the first. Cycles are handled with a visited set plus max_depth, and a subtree that IS cut says so in subtree_omitted instead of looking like a leaf. Rich text is read too (URichTextBlock does not derive from UTextBlock). Not implemented: include_bindings, which is accepted and flagged in bindings_note.
//   @param root_filter - Optional case-sensitive substring; only top-level UserWidgets whose object name contains it are returned.
//   @param include_text - Optional bool (default true): include UTextBlock text.
//   @param include_visibility - Optional bool (default true): include visibility, is_hovered and has_keyboard_focus per widget. has_keyboard_focus is what tells you whether a navigation key will do anything at all: with nothing focused every key is a no-op, which reads exactly like broken navigation.
//   @param include_bindings - Optional bool (default false): NOT implemented in this subset; sets bindings_note when true.
//   @param include_values - Optional bool (default false): include is_enabled + widget-type-specific value (Slider.value / ProgressBar.percent / CheckBox.is_checked / ComboBoxString.selected_option).
//   @param include_geometry - Optional bool (default false): include GetCachedGeometry()-derived absolute_position/absolute_size, render_opacity, and render_transform.
//   @param max_depth - Optional recursion limit (-1 = unlimited, the default). Counts every level including the descent into a nested Widget Blueprint's own tree. A subtree cut by the limit reports subtree_omitted rather than passing for a leaf.
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). In a 2-player PIE session EVERY client has its OWN widgets, so the default may well read the other player's HUD.
//   @returns { widgets: [{ widget_name, class, is_any_animation_playing, visibility?, is_hovered?, children: [{ widget_name, class, visibility?, is_hovered?, text?, is_enabled?, value?, percent?, is_checked?, selected_option?, geometry?, children: [...] }] }], widget_count: int, scanned_viewport_widgets: int, include_text: bool, include_visibility: bool, include_bindings: bool, include_values: bool, include_geometry: bool, bindings_note?: string, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleReadPieWidgets(const TSharedPtr<FJsonObject>& Params)
{
    FString RootFilter;
    Params->TryGetStringField(TEXT("root_filter"), RootFilter); // optional

    bool bIncludeText = true;
    Params->TryGetBoolField(TEXT("include_text"), bIncludeText);
    bool bIncludeVisibility = true;
    Params->TryGetBoolField(TEXT("include_visibility"), bIncludeVisibility);
    bool bIncludeBindings = false;
    Params->TryGetBoolField(TEXT("include_bindings"), bIncludeBindings);
    bool bIncludeValues = false;
    Params->TryGetBoolField(TEXT("include_values"), bIncludeValues);
    bool bIncludeGeometry = false;
    Params->TryGetBoolField(TEXT("include_geometry"), bIncludeGeometry);

    double MaxDepthNum = -1.0;
    Params->TryGetNumberField(TEXT("max_depth"), MaxDepthNum);
    const int32 MaxDepth = FMath::RoundToInt(MaxDepthNum);

    FString WorldError, WorldSelector;
    UWorld* PieWorld = RequirePieWorld_Widget(Params, TEXT("read_pie_widgets"), WorldError, &WorldSelector);
    if (!PieWorld)
    {
        return mcp_common::MakeErrorReply(WorldError);
    }

    TArray<TSharedPtr<FJsonValue>> WidgetsOut;
    int32 ScannedViewportWidgets = 0;

    for (TObjectIterator<UUserWidget> It; It; ++It)
    {
        UUserWidget* UserWidget = *It;
        if (!UMGWidgetUtils::IsLiveViewportWidget(UserWidget, PieWorld))
        {
            continue;
        }
        ++ScannedViewportWidgets;

        const FString UwName = UserWidget->GetName();
        if (!RootFilter.IsEmpty() && !UwName.Contains(RootFilter))
        {
            continue;
        }

        TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
        Node->SetStringField(TEXT("widget_name"), UwName);
        Node->SetStringField(TEXT("class"), UserWidget->GetClass()->GetName());
        if (bIncludeVisibility)
        {
            Node->SetStringField(TEXT("visibility"), UMGWidgetUtils::SlateVisibilityName(UserWidget->GetVisibility()));
            Node->SetBoolField(TEXT("is_hovered"), UserWidget->IsHovered());
        }
        AddPieWidgetValueAndGeometry(Node, UserWidget, bIncludeValues, bIncludeGeometry);

        // The reader for "is an animation running" (play/stop_widget_animation only write). Without
        // it, waiting for a transition to finish before asserting is a guessed sleep.
        Node->SetBoolField(TEXT("is_any_animation_playing"), UserWidget->IsAnyAnimationPlaying());

        // Walk the live widget tree under this UserWidget's root.
        TArray<TSharedPtr<FJsonValue>> Children;
        if (UserWidget->WidgetTree && UserWidget->WidgetTree->RootWidget)
        {
            Children.Add(MakeShared<FJsonValueObject>(
                BuildPieWidgetNode(UserWidget->WidgetTree->RootWidget, bIncludeText, bIncludeVisibility,
                    bIncludeValues, bIncludeGeometry, MaxDepth)));
        }
        Node->SetArrayField(TEXT("children"), Children);

        WidgetsOut.Add(MakeShared<FJsonValueObject>(Node));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetArrayField(TEXT("widgets"), WidgetsOut);
    Resp->SetNumberField(TEXT("widget_count"), WidgetsOut.Num());
    Resp->SetNumberField(TEXT("scanned_viewport_widgets"), ScannedViewportWidgets);
    // A widget_count of 0 means "not in THIS world". In a 2-player PIE each client owns
    // its own HUD, so which world was scanned is half the answer.
    Resp->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Resp, PieWorld);
    Resp->SetBoolField(TEXT("include_text"), bIncludeText);
    Resp->SetBoolField(TEXT("include_visibility"), bIncludeVisibility);
    Resp->SetBoolField(TEXT("include_bindings"), bIncludeBindings);
    Resp->SetBoolField(TEXT("include_values"), bIncludeValues);
    Resp->SetBoolField(TEXT("include_geometry"), bIncludeGeometry);
    if (bIncludeBindings)
    {
        Resp->SetStringField(TEXT("bindings_note"),
            TEXT("include_bindings is not supported in this subset; runtime property/delegate "
                 "binding introspection is omitted."));
    }
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// click_pie_widget -- fire a LIVE UMG widget's event in the running PIE session.
//
// read_pie_widgets is READ-ONLY; this fires a live widget's event headless, so a UI flow can be
// validated without a human clicking (driving it via call_function instead bypasses the real
// client->server RPC path, which yields false negatives). This resolves a live UWidget by
// name (REUSING the read_pie_widgets walk: PIE world -> active viewport UUserWidgets ->
// WidgetTree->FindWidget by name; supports child_name) and fires its named
// BlueprintAssignable multicast delegate.
//
// Firing path (DEFAULT, reliable): FindFProperty<FMulticastDelegateProperty> for the event
// name on the widget (UMG events like UButton::OnClicked are FOnButtonClickedEvent,
// DECLARE_DYNAMIC_MULTICAST_DELEGATE -> reflected as FMulticastDelegateProperty), then
// broadcast it (FMulticastScriptDelegate::ProcessDelegate) on the game thread. This runs the
// exact bound handlers a real click would, including the client->server RPC path.
//
// ALTERNATIVE (NOT used): a synthetic Slate press via UWidget::GetCachedWidget() geometry +
// FSlateApplication::Get().ProcessMouseButtonDown/Up with an FPointerEvent at the cached
// geometry centre. That drives the Slate input stack end-to-end but is fragile (needs the
// widget laid out / hit-testable this frame and a focused viewport); the delegate broadcast
// above is the reliable default.
// ---------------------------------------------------------------------------
// MCP:click_pie_widget - Fire a LIVE UMG widget's event in the running PIE session (game thread): resolve a UWidget by name among the active viewport UserWidgets (reusing the read_pie_widgets walk) and BROADCAST its named BlueprintAssignable multicast delegate (default OnClicked). Unlike read_pie_widgets (read-only) this MUTATES runtime state -- it runs the exact bound handlers a real click would, including the client->server RPC path, so equip/inventory/craft UI flows can be driven headless instead of by a human (driving them via call_function bypasses that RPC path and yields false negatives). Errors clearly if no Play-In-Editor session is running or the widget/event is not found.
//   @param widget_name - Required. Name of the target UWidget in the live tree (as reported by read_pie_widgets), e.g. "ConfirmButton". Matches a widget inside an active viewport UserWidget's WidgetTree, or a top-level UserWidget by its own name.
//   @param event - Optional (default "OnClicked"): BlueprintAssignable multicast delegate to fire (e.g. "OnClicked", "OnPressed", "OnReleased").
//   @param child_name - Optional: after resolving widget_name, drill into a descendant widget with this name (the resolved widget's own WidgetTree if it is a UserWidget, else a named child in its subtree) and fire the event on THAT widget instead.
//   @param player_index - Optional (default 0): when set, only consider viewport UserWidgets owned by the local player with this index (single-player PIE resolves globally).
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). In a 2-player PIE session EVERY client has its OWN widgets, so the default may well read the other player's HUD.
//   @returns { success: bool, widget_name, resolved_widget, resolved_class, event, is_bound: bool, fired: bool, player_index, child_name?, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number } -- is_bound is false when the delegate exists but has no bound handlers (broadcast is still a safe no-op).
TSharedPtr<FJsonObject> HandleClickPieWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("click_pie_widget: missing required parameter: widget_name"));
    }

    FString EventName = TEXT("OnClicked");
    Params->TryGetStringField(TEXT("event"), EventName);
    if (EventName.IsEmpty()) { EventName = TEXT("OnClicked"); }

    FString ChildName;
    Params->TryGetStringField(TEXT("child_name"), ChildName); // optional

    int32 PlayerIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), PlayerIndex); // optional, default 0

    FString WorldError, WorldSelector;
    UWorld* PieWorld = RequirePieWorld_Widget(Params, TEXT("click_pie_widget"), WorldError, &WorldSelector);
    if (!PieWorld)
    {
        return mcp_common::MakeErrorReply(WorldError);
    }

    // Resolve the target UWidget (+ optional child_name drill-down) -- shared with set_pie_widget_value.
    FString ResolveErr;
    UWidget* Target = ResolveLivePieWidget(PieWorld, FName(*WidgetName), PlayerIndex, ChildName, ResolveErr);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("click_pie_widget: %s"), *ResolveErr));
    }

    // Find the named BlueprintAssignable multicast delegate on the widget by reflection.
    // FindFProperty matches the concrete inline/sparse FMulticastDelegateProperty subclasses.
    FMulticastDelegateProperty* DelegateProp =
        FindFProperty<FMulticastDelegateProperty>(Target->GetClass(), FName(*EventName));
    if (!DelegateProp)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("click_pie_widget: widget '%s' (class %s) has no BlueprintAssignable multicast delegate named '%s' (try 'OnClicked'/'OnPressed'/'OnReleased')"),
            *Target->GetName(), *Target->GetClass()->GetName(), *EventName));
    }

    // Broadcast it on the game thread. GetMulticastDelegate() yields the live invocation list;
    // ProcessDelegate runs every bound handler exactly as a real click would (safe even if
    // nothing is bound). Delegates that take parameters get a zero-initialized params frame.
    const FMulticastScriptDelegate* ScriptDelegate =
        DelegateProp->GetMulticastDelegate(DelegateProp->ContainerPtrToValuePtr<void>(Target));
    const bool bIsBound = (ScriptDelegate != nullptr) && ScriptDelegate->IsBound();

    if (ScriptDelegate)
    {
        UFunction* SignatureFunction = DelegateProp->SignatureFunction;
        if (SignatureFunction && SignatureFunction->ParmsSize > 0)
        {
            FStructOnScope ParamFrame(SignatureFunction);
            ScriptDelegate->ProcessDelegate<UObject>(ParamFrame.GetStructMemory());
        }
        else
        {
            ScriptDelegate->ProcessDelegate<UObject>(nullptr);
        }
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("resolved_widget"), Target->GetName());
    Resp->SetStringField(TEXT("resolved_class"), Target->GetClass()->GetName());
    Resp->SetStringField(TEXT("event"), EventName);
    Resp->SetBoolField(TEXT("is_bound"), bIsBound);
    Resp->SetBoolField(TEXT("fired"), ScriptDelegate != nullptr);
    Resp->SetNumberField(TEXT("player_index"), PlayerIndex);
    // WHICH client's widget was clicked -- with 2 players this is the whole question.
    Resp->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Resp, PieWorld);
    if (!ChildName.IsEmpty())
    {
        Resp->SetStringField(TEXT("child_name"), ChildName);
    }
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// set_pie_widget_value -- companion to click_pie_widget: actually MUTATE an input widget's real
// value, instead of firing its delegate with zero-init params (click_pie_widget's own documented
// false-negative for slider/checkbox/text widgets -- the broadcast runs bound handlers, but the
// widget's own Value/CheckedState/Text is never touched first).
//
// In Engine/Source/Runtime/UMG/Private/Components/{Slider,CheckBox,
// EditableTextBox}.cpp: USlider::SetValue and UComboBoxString::SetSelectedOption/SetSelectedIndex
// ALREADY auto-broadcast their change event as an engine-internal side effect when the value
// differs from the previous one (Slider: HandleOnValueChanged -> OnValueChanged.Broadcast; ComboBox:
// HandleSelectionChanged -> OnSelectionChanged.Broadcast) -- fire_event cannot suppress that without
// invasively double-writing the value, so this reports whether a broadcast occurred (from the actual
// before/after change) rather than pretending the flag controls it. UCheckBox::SetIsChecked and
// UEditableTextBox::SetText / UEditableText::SetText do NOT auto-broadcast (their events only fire
// from the SLATE-level user-interaction callback, e.g. SlateOnCheckStateChangedCallback) -- for
// those, fire_event genuinely gates a manual broadcast this command performs afterward.
// UProgressBar::SetPercent has no BlueprintAssignable change event on UProgressBar at all
// (no DECLARE_DYNAMIC_MULTICAST_DELEGATE in Components/ProgressBar.h) -- always set:true, no event.
// ---------------------------------------------------------------------------
// MCP:set_pie_widget_value - Set a LIVE PIE input widget's REAL value (companion to click_pie_widget, which only broadcasts a delegate with zero-init params and never touches Slider/CheckBox/EditableTextBox/ComboBoxString state first). Resolves the widget exactly like click_pie_widget (same widget_name/child_name/player_index rules), then calls the widget's real setter: USlider::SetValue, UCheckBox::SetIsChecked, UEditableTextBox::SetText/UEditableText::SetText, UComboBoxString::SetSelectedOption, or UProgressBar::SetPercent. fire_event genuinely gates a manual OnCheckStateChanged/OnTextChanged(+OnTextCommitted) broadcast for CheckBox/EditableText(Box); Slider/ComboBoxString auto-broadcast on the engine side whenever the value actually changes (fire_event cannot suppress that -- see event_fired_note); ProgressBar has no change event at all. Errors clearly if no PIE session is running, the widget is not found, or its class is not one of the supported input types.
//   @param widget_name - Required. Name of the target UWidget in the live tree (as reported by read_pie_widgets).
//   @param child_name - Optional: drill into a descendant widget with this name after resolving widget_name.
//   @param player_index - Optional (default 0): filter to viewport UserWidgets owned by this local player index.
//   @param value - Required object: {"type":"float"|"bool"|"string", "value": <matching JSON type>}.
//   @param fire_event - Optional bool (default true): broadcast the widget's change event where the engine does not already do so unconditionally (CheckBox / EditableTextBox / EditableText).
//   @param commit_type - Optional "Default" | "OnEnter" | "OnUserMovedFocus": when present (EditableTextBox/EditableText only, and fire_event is true), also broadcasts OnTextCommitted with this commit type.
//   @param world - Optional world selector: absent/'pie' (default: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). In a 2-player PIE session EVERY client has its OWN widgets, so the default may well read the other player's HUD.
//   @returns { widget_name, resolved_widget, resolved_class, value_type, value, set: bool, event_fired: bool, player_index, child_name?, event_fired_note?, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleSetPieWidgetValue(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("set_pie_widget_value: missing required parameter: widget_name"));
    }

    FString ChildName;
    Params->TryGetStringField(TEXT("child_name"), ChildName); // optional

    int32 PlayerIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), PlayerIndex); // optional, default 0

    const TSharedPtr<FJsonObject>* ValueObjPtr = nullptr;
    if (!Params->TryGetObjectField(TEXT("value"), ValueObjPtr) || !ValueObjPtr || !ValueObjPtr->IsValid())
    {
        return mcp_common::MakeErrorReply(
            TEXT("set_pie_widget_value: missing required parameter: value (object {\"type\":..,\"value\":..})"));
    }
    const TSharedPtr<FJsonObject> ValueObj = *ValueObjPtr;

    FString ValueType;
    if (!ValueObj->TryGetStringField(TEXT("type"), ValueType) || ValueType.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("set_pie_widget_value: value.type is required (float|bool|string)"));
    }

    bool bFireEvent = true;
    Params->TryGetBoolField(TEXT("fire_event"), bFireEvent);

    FString CommitTypeStr;
    const bool bHasCommitType = Params->TryGetStringField(TEXT("commit_type"), CommitTypeStr);

    FString WorldError, WorldSelector;
    UWorld* PieWorld = RequirePieWorld_Widget(Params, TEXT("set_pie_widget_value"), WorldError, &WorldSelector);
    if (!PieWorld)
    {
        return mcp_common::MakeErrorReply(WorldError);
    }

    FString ResolveErr;
    UWidget* Target = ResolveLivePieWidget(PieWorld, FName(*WidgetName), PlayerIndex, ChildName, ResolveErr);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("set_pie_widget_value: %s"), *ResolveErr));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("resolved_widget"), Target->GetName());
    Resp->SetStringField(TEXT("resolved_class"), Target->GetClass()->GetName());
    Resp->SetStringField(TEXT("value_type"), ValueType);
    Resp->SetNumberField(TEXT("player_index"), PlayerIndex);
    // WHICH client's widget received the value.
    Resp->SetStringField(TEXT("world"), WorldSelector);
    mcp_common::AddWorldEvidence(Resp, PieWorld);
    if (!ChildName.IsEmpty())
    {
        Resp->SetStringField(TEXT("child_name"), ChildName);
    }

    bool bSet = false;
    bool bEventFired = false;

    // UEditableTextBox and UEditableText take the same steps. SetText does NOT auto-broadcast OnTextChanged
    // (Slate-callback-only), so fire_event gates a manual OnTextChanged broadcast, plus OnTextCommitted when
    // commit_type is given. Returns the error, or an empty string once the text is set.
    auto SetEditableText = [&](auto* TextWidget, const TCHAR* WidgetKind) -> FString
    {
        if (!ValueType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
        {
            return FString::Printf(TEXT("set_pie_widget_value: widget '%s' is %s; value.type must be 'string'"),
                *Target->GetName(), WidgetKind);
        }
        FString StrVal;
        if (!ValueObj->TryGetStringField(TEXT("value"), StrVal))
        {
            return TEXT("set_pie_widget_value: value.value (string) is required for type 'string'");
        }
        const FText NewText = FText::FromString(StrVal);
        TextWidget->SetText(NewText);
        bSet = true;
        if (bFireEvent)
        {
            TextWidget->OnTextChanged.Broadcast(NewText);
            bEventFired = true;
            if (bHasCommitType)
            {
                ETextCommit::Type CommitType = ETextCommit::Default;
                if (CommitTypeStr.Equals(TEXT("OnEnter"), ESearchCase::IgnoreCase)) { CommitType = ETextCommit::OnEnter; }
                else if (CommitTypeStr.Equals(TEXT("OnUserMovedFocus"), ESearchCase::IgnoreCase)) { CommitType = ETextCommit::OnUserMovedFocus; }
                TextWidget->OnTextCommitted.Broadcast(NewText, CommitType);
            }
        }
        Resp->SetStringField(TEXT("value"), StrVal);
        return FString();
    };

    if (USlider* Slider = Cast<USlider>(Target))
    {
        if (!ValueType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_pie_widget_value: widget '%s' is a Slider; value.type must be 'float'"), *Target->GetName()));
        }
        double NumVal = 0.0;
        if (!ValueObj->TryGetNumberField(TEXT("value"), NumVal))
        {
            return mcp_common::MakeErrorReply(TEXT("set_pie_widget_value: value.value (number) is required for type 'float'"));
        }
        const float OldValue = Slider->GetValue();
        Slider->SetValue(static_cast<float>(NumVal));
        bSet = true;
        bEventFired = (Slider->GetValue() != OldValue); // USlider::SetValue auto-broadcasts on change (engine behavior)
        Resp->SetNumberField(TEXT("value"), Slider->GetValue());
        if (!bFireEvent)
        {
            Resp->SetStringField(TEXT("event_fired_note"),
                TEXT("USlider::SetValue broadcasts OnValueChanged automatically when the value changes; fire_event=false could not suppress it (engine behavior)."));
        }
    }
    else if (UCheckBox* CheckBox = Cast<UCheckBox>(Target))
    {
        if (!ValueType.Equals(TEXT("bool"), ESearchCase::IgnoreCase))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_pie_widget_value: widget '%s' is a CheckBox; value.type must be 'bool'"), *Target->GetName()));
        }
        bool BoolVal = false;
        if (!ValueObj->TryGetBoolField(TEXT("value"), BoolVal))
        {
            return mcp_common::MakeErrorReply(TEXT("set_pie_widget_value: value.value (bool) is required for type 'bool'"));
        }
        CheckBox->SetIsChecked(BoolVal); // does NOT auto-broadcast OnCheckStateChanged (Slate-callback-only)
        bSet = true;
        if (bFireEvent)
        {
            CheckBox->OnCheckStateChanged.Broadcast(BoolVal);
            bEventFired = true;
        }
        Resp->SetBoolField(TEXT("value"), BoolVal);
    }
    else if (UEditableTextBox* EditableTextBox = Cast<UEditableTextBox>(Target))
    {
        const FString TextError = SetEditableText(EditableTextBox, TEXT("an EditableTextBox"));
        if (!TextError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TextError);
        }
    }
    else if (UEditableText* EditableText = Cast<UEditableText>(Target))
    {
        const FString TextError = SetEditableText(EditableText, TEXT("an EditableText"));
        if (!TextError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(TextError);
        }
    }
    else if (UComboBoxString* Combo = Cast<UComboBoxString>(Target))
    {
        if (!ValueType.Equals(TEXT("string"), ESearchCase::IgnoreCase))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_pie_widget_value: widget '%s' is a ComboBoxString; value.type must be 'string'"), *Target->GetName()));
        }
        FString StrVal;
        if (!ValueObj->TryGetStringField(TEXT("value"), StrVal))
        {
            return mcp_common::MakeErrorReply(TEXT("set_pie_widget_value: value.value (string) is required for type 'string'"));
        }
        const FString OldOption = Combo->GetSelectedOption();
        Combo->SetSelectedOption(StrVal);
        bSet = true;
        bEventFired = (Combo->GetSelectedOption() != OldOption); // auto-broadcasts on change (engine behavior)
        Resp->SetStringField(TEXT("value"), Combo->GetSelectedOption());
        if (!bFireEvent)
        {
            Resp->SetStringField(TEXT("event_fired_note"),
                TEXT("UComboBoxString::SetSelectedOption broadcasts OnSelectionChanged automatically when the selection changes; fire_event=false could not suppress it (engine behavior)."));
        }
    }
    else if (UProgressBar* Bar = Cast<UProgressBar>(Target))
    {
        if (!ValueType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("set_pie_widget_value: widget '%s' is a ProgressBar; value.type must be 'float'"), *Target->GetName()));
        }
        double NumVal = 0.0;
        if (!ValueObj->TryGetNumberField(TEXT("value"), NumVal))
        {
            return mcp_common::MakeErrorReply(TEXT("set_pie_widget_value: value.value (number) is required for type 'float'"));
        }
        Bar->SetPercent(static_cast<float>(NumVal));
        bSet = true;
        bEventFired = false; // UProgressBar has no BlueprintAssignable change event
        Resp->SetNumberField(TEXT("value"), Bar->GetPercent());
        Resp->SetStringField(TEXT("event_fired_note"), TEXT("UProgressBar has no change event; set:true only."));
    }
    else
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("set_pie_widget_value: widget '%s' (class %s) is not a supported input widget (Slider|CheckBox|EditableTextBox|EditableText|ComboBoxString|ProgressBar)"),
            *Target->GetName(), *Target->GetClass()->GetName()));
    }

    Resp->SetBoolField(TEXT("set"), bSet);
    Resp->SetBoolField(TEXT("event_fired"), bEventFired);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// rename_widget
// ---------------------------------------------------------------------------
// Uses FWidgetBlueprintOperationUtils::RenameWidget(UWidgetBlueprint*, UWidget*, FString) directly
// -- the SAME primitive FWidgetBlueprintEditorUtils::RenameWidget (the Designer's F2) forwards to
// once it has resolved a live BlueprintEditor; the Operation-level overload needs no live editor
// (see WidgetBlueprintOperationUtils.cpp: when no FWidgetBlueprintEditor is open for
// this blueprint it just falls through to Widget->SetDisplayLabel + Widget->Rename, which is exactly
// headless-safe). RenameWidget on a collision just returns false with no reason string, so
// VerifyWidgetRename is called FIRST for an honest, engine-faithful diagnostic (matches this file's
// "never a silent/unexplained failure" convention).
// ---------------------------------------------------------------------------
// MCP:rename_widget - Rename a widget in a Widget Blueprint's tree (WidgetTree membership, blueprint variable map, bindings, and animation bindings all updated in one call -- the same path as the Designer's F2).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param old_name - current name of the widget in the tree
//   @param new_name - desired new name
//   @returns { widget_bp_path: string, old_name: string, new_name: string, resolved_name: string, renamed: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleRenameWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString OldName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("old_name"), OldName)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* Widget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, OldName, Error) : nullptr;
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UWidgetTree* WidgetTree = WidgetBP->WidgetTree;

    // Idempotent no-op when the names already match (honestly reported, no engine call needed).
    if (OldName == NewName)
    {
        TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("old_name"), OldName);
        Resp->SetStringField(TEXT("new_name"), NewName);
        Resp->SetStringField(TEXT("resolved_name"), Widget->GetName());
        Resp->SetBoolField(TEXT("renamed"), true);
        Resp->SetBoolField(TEXT("saved"), true);
        return mcp_common::MakeOkReply(Resp);
    }

    FText VerifyError;
    if (!FWidgetBlueprintOperationUtils::VerifyWidgetRename(WidgetBP, Widget, FText::FromString(NewName), VerifyError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot rename '%s' to '%s': %s"), *OldName, *NewName, *VerifyError.ToString()));
    }

    const bool bRenamed = FWidgetBlueprintOperationUtils::RenameWidget(WidgetBP, Widget, NewName);
    if (!bRenamed)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RenameWidget failed for '%s' -> '%s' (unexpected: VerifyWidgetRename passed)"), *OldName, *NewName));
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    // Re-resolve to report the actual (name-sanitized) result.
    UWidget* Renamed = WidgetTree->FindWidget(FName(*NewName));

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("old_name"), OldName);
    Resp->SetStringField(TEXT("new_name"), NewName);
    Resp->SetStringField(TEXT("resolved_name"), Renamed ? Renamed->GetName() : NewName);
    Resp->SetBoolField(TEXT("renamed"), true);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("rename_widget"), SaveErr);
}

// ---------------------------------------------------------------------------
// reparent_widget
// ---------------------------------------------------------------------------
// Uses FWidgetBlueprintOperationUtils::MoveWidget (UWidgetBlueprint* + UWidget* + UPanelWidget* +
// ChildIndex, no live editor needed) INSTEAD OF hand-rolled OldParent->RemoveChild + NewParent->
// AddChild + ShiftChild -- see WidgetBlueprintOperationUtils.cpp: MoveWidget already
// validates CanAddToParent + the parent/child cycle (IsParentChildCycleFree), correctly creates the
// new Slot type for the destination panel, PRESERVES the old slot's properties across the move
// (ExportPropertiesToText/ImportPropertiesFromText) where the new slot class accepts them, and
// handles named-slot content correctly. This is strictly more correct than a hand-rolled sequence.
// ---------------------------------------------------------------------------
// MCP:reparent_widget - Move a widget to a new parent panel (optionally at a specific child index), via FWidgetBlueprintOperationUtils::MoveWidget -- the same primitive backing the Designer's drag-and-drop reparent. Validates the new parent is a panel widget and refuses to create a cycle. Optional slot_properties are applied to the widget's NEW slot afterward via the shared reflection setter (e.g. {"Row":1,"Column":2} for a GridSlot). THIS IS ALSO HOW YOU FILL A NAMED SLOT: a UNamedSlot is a UContentWidget (hence a panel), so add one with add_widget_extended widget_type="NamedSlot" and reparent the content into it -- there is no separate SetContentForSlot tool and none is needed (it answers reparented:true, new_slot_class:PanelSlot).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the widget to move
//   @param new_parent_name - name of the panel widget to become the new parent
//   @param target_index - Optional int child index to insert at (default: append at the end)
//   @param slot_properties - Optional object of {PropertyName: value} applied to the resulting slot via reflection (best-effort; per-field errors are reported, not fatal)
//   @returns { widget_bp_path: string, widget_name: string, old_parent_name: string, new_parent_name: string, new_slot_class: string, resolved_child_index: int, reparented: bool, slot_properties_applied: int, slot_property_errors?: [string], saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleReparentWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    FString NewParentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_parent_name"), NewParentName)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* Widget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error) : nullptr;
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UWidgetTree* WidgetTree = WidgetBP->WidgetTree;

    UPanelWidget* OldParent = Widget->GetParent();
    if (!OldParent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Widget '%s' has no parent (it is the root widget) and cannot be reparented"), *WidgetName));
    }

    UWidget* NewParentAsWidget = WidgetTree->FindWidget(FName(*NewParentName));
    if (!NewParentAsWidget)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("new_parent_name '%s' not found in widget tree"), *NewParentName));
    }
    UPanelWidget* NewParent = Cast<UPanelWidget>(NewParentAsWidget);
    if (!NewParent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("new_parent_name '%s' is a %s, not a panel widget, and cannot host children"),
            *NewParentName, *NewParentAsWidget->GetClass()->GetName()));
    }

    int32 TargetIndex = -1; // -1 = append (MoveWidget's own convention)
    double IndexNum = 0.0;
    if (Params->TryGetNumberField(TEXT("target_index"), IndexNum))
    {
        TargetIndex = FMath::RoundToInt(IndexNum);
    }

    FText MoveError;
    const bool bMoved = FWidgetBlueprintOperationUtils::MoveWidget(WidgetBP, Widget, NewParent, TargetIndex, MoveError);
    if (!bMoved)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("reparent_widget failed for '%s' -> '%s': %s"), *WidgetName, *NewParentName, *MoveError.ToString()));
    }

    // Extra properties on the NEW slot. Best-effort per field: one that does not land does not
    // invalidate the reparent, which already happened. It goes through the path applier, not the
    // engine's converter, where a misspelled field name would count as applied.
    int32 SlotPropsApplied = 0;
    TArray<FString> SlotPropErrors;
    const TSharedPtr<FJsonObject>* SlotPropsObjPtr = nullptr;
    if (Params->TryGetObjectField(TEXT("slot_properties"), SlotPropsObjPtr) && SlotPropsObjPtr && SlotPropsObjPtr->IsValid() && Widget->Slot)
    {
        Widget->Slot->Modify();

        FStyleWriteOutcome_Widget SlotOutcome;
        WriteStylePathMap_Widget(Widget->Slot, *SlotPropsObjPtr, SlotOutcome);

        SlotPropsApplied = SlotOutcome.OkPaths.Num();
        for (const FString& Ignored : SlotOutcome.Report.Ignored)
        {
            SlotPropErrors.Add(FString::Printf(TEXT("%s: no such property on %s"),
                                               *Ignored, *Widget->Slot->GetClass()->GetName()));
        }
        for (const TPair<FString, FString>& E : SlotOutcome.Report.Errors)
        {
            SlotPropErrors.Add(FString::Printf(TEXT("%s: %s"), *E.Key, *E.Value));
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("old_parent_name"), OldParent->GetName());
    Resp->SetStringField(TEXT("new_parent_name"), NewParentName);
    Resp->SetStringField(TEXT("new_slot_class"), Widget->Slot ? Widget->Slot->GetClass()->GetName() : TEXT(""));
    Resp->SetNumberField(TEXT("resolved_child_index"), NewParent->GetChildIndex(Widget));
    Resp->SetBoolField(TEXT("reparented"), true);
    Resp->SetNumberField(TEXT("slot_properties_applied"), SlotPropsApplied);
    if (SlotPropErrors.Num() > 0)
    {
        Resp->SetArrayField(TEXT("slot_property_errors"), mcp_common::StringsToJson(SlotPropErrors));
    }
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("reparent_widget"), SaveErr);
}

// ---------------------------------------------------------------------------
// reorder_widget_child
// ---------------------------------------------------------------------------
// MCP:reorder_widget_child - Change a widget's index among its parent panel's children via UPanelWidget::ShiftChild (clamps new_index internally; safe with any int). For a CanvasPanel parent, visual stacking is governed by each child's CanvasPanelSlot ZOrder (set_widget_slot_properties z_order), NOT child index -- the index shift is still applied and reported honestly, with a note.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the widget to reorder
//   @param new_index - desired index among its parent's children (clamped to [0, child_count])
//   @returns { widget_bp_path: string, widget_name: string, parent_name: string, old_index: int, requested_index: int, resolved_index: int, child_count: int, reordered: bool, parent_is_canvas_panel: bool, note?: string, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleReorderWidgetChild(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    double NewIndexNum = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("new_index"), NewIndexNum)) { return Missing; }
    const int32 NewIndex = FMath::RoundToInt(NewIndexNum);

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* Widget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error) : nullptr;
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UPanelWidget* Parent = Widget->GetParent();
    if (!Parent)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Widget '%s' has no parent panel (it is the root widget) and cannot be reordered"), *WidgetName));
    }

    const int32 OldIndex = Parent->GetChildIndex(Widget);
    const int32 ChildCount = Parent->GetChildrenCount();

    Parent->Modify();
    Parent->ShiftChild(NewIndex, Widget);

    const int32 ResolvedIndex = Parent->GetChildIndex(Widget);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("parent_name"), Parent->GetName());
    Resp->SetNumberField(TEXT("old_index"), OldIndex);
    Resp->SetNumberField(TEXT("requested_index"), NewIndex);
    Resp->SetNumberField(TEXT("resolved_index"), ResolvedIndex);
    Resp->SetNumberField(TEXT("child_count"), ChildCount);
    Resp->SetBoolField(TEXT("reordered"), true);

    const bool bIsCanvas = Parent->IsA<UCanvasPanel>();
    Resp->SetBoolField(TEXT("parent_is_canvas_panel"), bIsCanvas);
    if (bIsCanvas)
    {
        Resp->SetStringField(TEXT("note"),
            TEXT("Parent is a CanvasPanel: child index does not control visual stacking order there -- "
                 "that is governed by each child's CanvasPanelSlot ZOrder (set_widget_slot_properties "
                 "z_order). The index shift was still applied."));
    }

    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("reorder_widget_child"), SaveErr);
}

// ---------------------------------------------------------------------------
// list_widget_types
// ---------------------------------------------------------------------------
// MCP:list_widget_types - List the CURATED widget_type names accepted by add_widget_to_blueprint (family="basic") / add_widget_extended (family="extended", the default -- a strict superset of basic), with each entry's resolved /Script/UMG.<Name> class path and whether LoadObject actually resolves it in this build (should always be true; reported for honesty rather than assumed). This list is a convenience shortlist, NOT the full set of accepted values -- both tools also accept any native widget class path (/Script/Module.ClassName) and any UserWidget-derived Widget Blueprint's /Game asset path (embeds a nested instance); see custom_class_note in the response.
//   @param family - Optional "basic" | "extended" (default "extended")
//   @returns { family: string, widget_types: [{ widget_type: string, class_path: string, resolvable: bool }], count: int, custom_class_note: string }

TSharedPtr<FJsonObject> HandleListWidgetTypes(const TSharedPtr<FJsonObject>& Params)
{
    FString Family = TEXT("extended");
    if (Params.IsValid())
    {
        Params->TryGetStringField(TEXT("family"), Family);
    }
    Family = Family.ToLower();

    if (Family != TEXT("basic") && Family != TEXT("extended"))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid family '%s' (use 'basic' or 'extended')"), *Family));
    }

    const TMap<FString, FString>& Map = (Family == TEXT("basic")) ? GetBasicWidgetTypeMap() : GetExtendedWidgetTypeMap();

    TArray<FString> Names;
    Map.GetKeys(Names);
    Names.Sort();

    TArray<TSharedPtr<FJsonValue>> TypesOut;
    for (const FString& Name : Names)
    {
        const FString& ClassPath = Map.FindChecked(Name); // TMap has no operator[]; every Name came from Map.GetKeys()
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("widget_type"), Name);
        Entry->SetStringField(TEXT("class_path"), ClassPath);
        Entry->SetBoolField(TEXT("resolvable"), LoadObject<UClass>(nullptr, *ClassPath) != nullptr);
        TypesOut.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("family"), Family);
    Resp->SetArrayField(TEXT("widget_types"), TypesOut);
    Resp->SetNumberField(TEXT("count"), TypesOut.Num());
    Resp->SetStringField(TEXT("custom_class_note"),
        TEXT("This is a curated shortlist, not the full accepted set. widget_type also accepts: any ")
        TEXT("native widget class path (/Script/Module.ClassName, e.g. /Script/UMG.Throbber) not listed ")
        TEXT("above; or a UserWidget-derived Widget Blueprint's /Game asset path (e.g. /Game/UI/WBP_HealthBar) ")
        TEXT("or generated-class path (/Game/UI/WBP_HealthBar.WBP_HealthBar_C) to embed it as a nested instance."));
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// duplicate_widget
// ---------------------------------------------------------------------------
// In Engine/Source/Runtime/CoreUObject/Private/Serialization/
// DuplicateDataWriter.cpp (FDuplicateDataWriter::GetDuplicatedObject), UE's object duplication only
// deep-copies a referenced UObject when that object's OUTER CHAIN also leads back to the object
// being duplicated. UPanelWidget's child WIDGETS are outered to the shared UWidgetTree (not to
// their parent panel widget) -- only their PanelSlot wrapper is outered to the panel. So
// DuplicateObject<UWidget>(SourcePanel, WidgetTree) WOULD duplicate the Slots array (in-scope) but
// leave each slot's Content pointing at the SAME ORIGINAL child widget instances (out of scope) --
// an aliased, broken tree (one child, two "parents"). Refuses that case honestly instead of handing
// back a widget that looks duplicated but silently shares children with the original.
// For the supported (non-panel, or childless-panel) case: duplicates WITHOUT forcing the final name
// (auto-unique name from the engine), then renames via the SAME vetted primitives as rename_widget
// (VerifyWidgetRename + RenameWidget) for an honest, engine-faithful collision diagnostic.
// ---------------------------------------------------------------------------
// MCP:duplicate_widget - Duplicate a widget (DuplicateObject<UWidget> outered to the WidgetTree) and add the clone under a target parent panel, copying the source's slot property values onto the new slot when the slot classes match. Refuses (clear error, no partial/broken result) to duplicate a panel widget that currently has children -- UE's object duplication would leave the clone's slots aliasing the SAME original child widgets rather than deep-copying them (see FDuplicateDataWriter); duplicate leaf widgets, or rebuild the panel's children individually with add_widget_extended. Name collisions on new_name are a clear error (not a silent alias of the pre-existing widget at that name).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the widget to duplicate (must not be a panel widget with children)
//   @param new_name - unique name for the duplicate
//   @param target_parent_name - Optional name of the panel widget to host the duplicate; defaults to the source widget's own current parent (sibling placement).
//   @returns { widget_bp_path: string, source_widget_name: string, new_widget_name: string, widget_class: string, target_parent_name: string, slot_class: string, slot_class_matched_source: bool, slot_properties_copied: int, duplicated: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleDuplicateWidget(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }
    FString TargetParentName;
    Params->TryGetStringField(TEXT("target_parent_name"), TargetParentName); // optional

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* SourceWidget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error) : nullptr;
    if (!SourceWidget)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UWidgetTree* WidgetTree = WidgetBP->WidgetTree;

    if (UPanelWidget* SourcePanel = Cast<UPanelWidget>(SourceWidget))
    {
        if (SourcePanel->GetChildrenCount() > 0)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("duplicate_widget does not support panel widgets with children ('%s' is a %s with %d children): ")
                TEXT("UE's object duplication only deep-copies objects outered to the source, and widget-tree ")
                TEXT("children are outered to the WidgetTree (not to their parent panel), so the clone's slots ")
                TEXT("would end up aliasing the SAME child widget instances as the original -- a broken tree, not ")
                TEXT("a real copy. Duplicate leaf widgets, or build the panel's children individually with add_widget_extended."),
                *WidgetName, *SourceWidget->GetClass()->GetName(), SourcePanel->GetChildrenCount()));
        }
    }

    UPanelWidget* TargetParent = nullptr;
    if (!TargetParentName.IsEmpty())
    {
        UWidget* Found = WidgetTree->FindWidget(FName(*TargetParentName));
        if (!Found)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("target_parent_name '%s' not found in widget tree"), *TargetParentName));
        }
        TargetParent = Cast<UPanelWidget>(Found);
        if (!TargetParent)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("target_parent_name '%s' is not a panel widget and cannot host children"), *TargetParentName));
        }
    }
    else
    {
        TargetParent = SourceWidget->GetParent();
        if (!TargetParent)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Widget '%s' has no parent (it is the root widget) -- specify target_parent_name to place the duplicate"),
                *WidgetName));
        }
    }

    // Duplicate with an auto-unique name (NAME_None), then rename via the vetted rename_widget
    // primitives (see the duplicate_widget comment above for why not to force the final name directly).
    UWidget* NewWidget = DuplicateObject<UWidget>(SourceWidget, WidgetTree);
    if (!NewWidget)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to duplicate widget '%s'"), *WidgetName));
    }

    FText VerifyError;
    if (!FWidgetBlueprintOperationUtils::VerifyWidgetRename(WidgetBP, NewWidget, FText::FromString(NewName), VerifyError))
    {
        // The duplicate was constructed but never attached to the tree -- a plain rename-to-transient
        // is enough to discard it cleanly (nothing else references it yet).
        NewWidget->Rename(nullptr, GetTransientPackage());
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("duplicate_widget: cannot name the duplicate '%s': %s"), *NewName, *VerifyError.ToString()));
    }
    FWidgetBlueprintOperationUtils::RenameWidget(WidgetBP, NewWidget, NewName);

    UPanelSlot* NewSlot = TargetParent->AddChild(NewWidget);
    if (!NewSlot)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Failed to add duplicated widget '%s' under parent '%s'"), *NewWidget->GetName(), *TargetParent->GetName()));
    }

    // Copy the ORIGINAL widget's slot property values onto the new slot when the slot classes match
    // (AddChild already created the correct slot subclass for TargetParent's panel type, which may
    // differ from SourceWidget's original slot class when target_parent_name points at a different
    // kind of panel -- in that case there is nothing sensible to copy field-by-field).
    int32 SlotPropsCopied = 0;
    const bool bSlotClassMatched = SourceWidget->Slot && NewSlot && SourceWidget->Slot->GetClass() == NewSlot->GetClass();
    if (bSlotClassMatched)
    {
        NewSlot->Modify();
        for (TFieldIterator<FProperty> PropIt(NewSlot->GetClass()); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible)) { continue; }
            Prop->CopySingleValue(
                Prop->ContainerPtrToValuePtr<void>(NewSlot),
                Prop->ContainerPtrToValuePtr<void>(SourceWidget->Slot));
            ++SlotPropsCopied;
        }
    }

    WidgetBP->OnVariableAdded(NewWidget->GetFName());
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    UWidget* Verify = WidgetTree->FindWidget(FName(*NewName));

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("source_widget_name"), WidgetName);
    Resp->SetStringField(TEXT("new_widget_name"), NewWidget->GetName());
    Resp->SetStringField(TEXT("widget_class"), NewWidget->GetClass()->GetName());
    Resp->SetStringField(TEXT("target_parent_name"), TargetParent->GetName());
    Resp->SetStringField(TEXT("slot_class"), NewSlot->GetClass()->GetName());
    Resp->SetBoolField(TEXT("slot_class_matched_source"), bSlotClassMatched);
    Resp->SetNumberField(TEXT("slot_properties_copied"), SlotPropsCopied);
    Resp->SetBoolField(TEXT("duplicated"), Verify != nullptr);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("duplicate_widget"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_widget_animation_track
// ---------------------------------------------------------------------------
// Replicates (headlessly -- no live ISequencer/FWidgetBlueprintEditor needed) the shape the editor's
// FWidgetBlueprintEditor::AddObjectToAnimation + Sequencer's "Add Track" produce, using the SAME
// low-level MovieScene primitives this plugin already uses for ULevelSequence property tracks
// (UEAgentSequencerCommands.cpp HandleAddPropertyTrack -- same UMovieScene API, since a
// UWidgetAnimation's MovieScene is a plain UMovieScene like any other sequence's):
//   1. Find-or-create a UMovieScenePossessable bound to the named widget (MovieScene->AddPossessable
//      + a matching FWidgetAnimationBinding pushed onto WidgetAnimation->AnimationBindings -- the
//      runtime lookup UWidgetAnimationBinding::FindRuntimeObject uses to resolve back to the live
//      UWidget by name+WidgetTree at playback time).
//   2. Resolve property_path to a top-level FProperty on the widget's class (single-level names
//      only -- e.g. "RenderOpacity", "ColorAndOpacity" -- not dotted sub-paths).
//   3. Add (or reuse) the matching typed UMovieScenePropertyTrack + a full-range section.
// Covers float (FFloatProperty/FDoubleProperty) and color (FLinearColor OR FSlateColor structs,
// both produce a UMovieSceneColorTrack + 4 FMovieSceneFloatChannel section -- see
// MovieSceneColorTrack.h/.cpp). bool is included too (same code shape, low
// risk). NOT exercised end-to-end in a live PIE playback
// (author-time construction follows the exact API contracts; runtime evaluation of
// an FSlateColor-typed property specifically relies on the engine's own built-in property-binding
// resolution).
// ---------------------------------------------------------------------------
// MCP:add_widget_animation_track - Add (or reuse) a MovieScene property track for a widget's property inside a UWidgetAnimation: finds/creates a UMovieScenePossessable bound to the named widget (+ matching FWidgetAnimationBinding), resolves property_path to a top-level reflected property on the widget's class, and adds the matching typed track (UMovieSceneFloatTrack | UMovieSceneColorTrack | UMovieSceneBoolTrack) with a full-playback-range section. track_type="auto" infers from the property's reflected type (FFloatProperty/FDoubleProperty->float; FLinearColor or FSlateColor struct->color; FBoolProperty->bool). Call add_widget_animation_keyframe afterward to place keys. Single-level property_path only (not dotted sub-paths).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param animation_name - object name OR display label of the target animation
//   @param widget_name - name of the widget (in the SAME WidgetBP's tree) to animate
//   @param property_path - name of a top-level reflected property on the widget's class (e.g. "RenderOpacity", "ColorAndOpacity")
//   @param track_type - Optional "auto" (default) | "float" | "color" | "bool"
//   @returns { widget_bp_path: string, animation_name: string, widget_name: string, property_path: string, track_type: string, possessable_guid: string, binding_reused: bool, track_class: string, track_reused: bool, section_class: string, section_created: bool, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddWidgetAnimationTrack(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AnimationName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_name"), AnimationName)) { return Missing; }
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    FString PropertyPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_path"), PropertyPath)) { return Missing; }
    FString TrackTypeRaw = TEXT("auto");
    Params->TryGetStringField(TEXT("track_type"), TrackTypeRaw);
    FString TrackType = TrackTypeRaw.ToLower();
    if (TrackType != TEXT("auto") && TrackType != TEXT("float") && TrackType != TEXT("color") && TrackType != TEXT("bool"))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid track_type '%s' (use auto|float|color|bool)"), *TrackTypeRaw));
    }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidgetTree* WidgetTree = WidgetBP ? GetWidgetTree_Widget(WidgetBP, Error) : nullptr;
    if (!WidgetTree)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UWidgetAnimation* Animation = FindWidgetAnimationByName(WidgetBP, AnimationName);
    if (!Animation || !Animation->MovieScene)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget animation '%s' not found on %s (call add_widget_animation first)"), *AnimationName, *WidgetBPPath));
    }

    UWidget* Widget = FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error);
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FProperty* Property = Widget->GetClass()->FindPropertyByName(*PropertyPath);
    if (!Property)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Property '%s' not found on widget '%s' (class %s). Only top-level reflected properties are supported (no dotted sub-paths)."),
            *PropertyPath, *WidgetName, *Widget->GetClass()->GetName()));
    }

    if (TrackType == TEXT("auto"))
    {
        if (CastField<FBoolProperty>(Property))
        {
            TrackType = TEXT("bool");
        }
        else if (const FStructProperty* StructProp = CastField<FStructProperty>(Property))
        {
            if (StructProp->Struct == TBaseStructure<FLinearColor>::Get() || StructProp->Struct == FSlateColor::StaticStruct())
            {
                TrackType = TEXT("color");
            }
            else
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Cannot auto-detect track_type for property '%s' (struct %s is not FLinearColor/FSlateColor); specify track_type explicitly"),
                    *PropertyPath, *StructProp->Struct->GetName()));
            }
        }
        else if (CastField<FFloatProperty>(Property) || CastField<FDoubleProperty>(Property))
        {
            TrackType = TEXT("float");
        }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Cannot auto-detect track_type for property '%s' (reflected type %s is not float/color/bool-shaped); specify track_type explicitly"),
                *PropertyPath, *Property->GetClass()->GetName()));
        }
    }

    Animation->Modify();
    Animation->MovieScene->Modify();

    // Find-or-create the possessable binding for this widget.
    const FName WidgetFName = Widget->GetFName();
    FGuid PossessableGuid;
    bool bBindingReused = false;
    for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
    {
        if (Binding.WidgetName == WidgetFName && Binding.SlotWidgetName.IsNone()
            && Animation->MovieScene->FindPossessable(Binding.AnimationGuid))
        {
            PossessableGuid = Binding.AnimationGuid;
            bBindingReused = true;
            break;
        }
    }
    if (!PossessableGuid.IsValid())
    {
        PossessableGuid = Animation->MovieScene->AddPossessable(WidgetName, Widget->GetClass());

        FWidgetAnimationBinding NewBinding;
        NewBinding.WidgetName = WidgetFName;
        NewBinding.SlotWidgetName = NAME_None;
        NewBinding.AnimationGuid = PossessableGuid;
        NewBinding.bIsRootWidget = (Widget == WidgetTree->RootWidget);
        Animation->AnimationBindings.Add(NewBinding);
    }

    // Find-or-create the typed property track (matched by property name, not just class -- see
    // FindWidgetPropertyTrack's own comment for why FindTrack<T>(Guid) alone is not enough).
    UMovieScenePropertyTrack* PropTrack = nullptr;
    bool bTrackReused = false;
    const FName PropertyFName(*PropertyPath);
    if (TrackType == TEXT("float"))
    {
        PropTrack = FindWidgetPropertyTrack(Animation->MovieScene, UMovieSceneFloatTrack::StaticClass(), PossessableGuid, PropertyFName);
        if (PropTrack) { bTrackReused = true; }
        else { PropTrack = Animation->MovieScene->AddTrack<UMovieSceneFloatTrack>(PossessableGuid); }
    }
    else if (TrackType == TEXT("color"))
    {
        PropTrack = FindWidgetPropertyTrack(Animation->MovieScene, UMovieSceneColorTrack::StaticClass(), PossessableGuid, PropertyFName);
        if (PropTrack) { bTrackReused = true; }
        else { PropTrack = Animation->MovieScene->AddTrack<UMovieSceneColorTrack>(PossessableGuid); }
    }
    else // bool
    {
        PropTrack = FindWidgetPropertyTrack(Animation->MovieScene, UMovieSceneBoolTrack::StaticClass(), PossessableGuid, PropertyFName);
        if (PropTrack) { bTrackReused = true; }
        else { PropTrack = Animation->MovieScene->AddTrack<UMovieSceneBoolTrack>(PossessableGuid); }
    }

    if (!PropTrack)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to add/find %s property track"), *TrackType));
    }

    if (!bTrackReused)
    {
        PropTrack->SetPropertyNameAndPath(PropertyFName, PropertyPath);
    }

    bool bSectionCreated = false;
    UMovieSceneSection* Section = (PropTrack->GetAllSections().Num() > 0) ? PropTrack->GetAllSections()[0] : nullptr;
    if (!Section)
    {
        Section = PropTrack->CreateNewSection();
        if (Section)
        {
            Section->SetRange(Animation->MovieScene->GetPlaybackRange());
            PropTrack->AddSection(*Section);
            bSectionCreated = true;
        }
    }
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create property section"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
    Resp->SetStringField(TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("property_path"), PropertyPath);
    Resp->SetStringField(TEXT("track_type"), TrackType);
    Resp->SetStringField(TEXT("possessable_guid"), PossessableGuid.ToString());
    Resp->SetBoolField(TEXT("binding_reused"), bBindingReused);
    Resp->SetStringField(TEXT("track_class"), PropTrack->GetClass()->GetName());
    Resp->SetBoolField(TEXT("track_reused"), bTrackReused);
    Resp->SetStringField(TEXT("section_class"), Section->GetClass()->GetName());
    Resp->SetBoolField(TEXT("section_created"), bSectionCreated);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_widget_animation_track"), SaveErr);
}

// ---------------------------------------------------------------------------
// add_widget_animation_keyframe
// ---------------------------------------------------------------------------
// MCP:add_widget_animation_keyframe - Add a keyframe to an existing widget-animation property track's section (call add_widget_animation_track first). Converts time_seconds to an FFrameNumber via the MovieScene's tick resolution, extends the section's range to cover the key if needed, then adds the key on the matching channel(s) via the requested interpolation (AddLinearKey | AddCubicKey | AddConstantKey for float/color's FMovieSceneFloatChannel(s); bool channels have no interpolation concept and always use UpdateOrAddKey).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param animation_name - object name OR display label of the target animation
//   @param widget_name - name of the animated widget
//   @param property_path - the SAME property_path used in add_widget_animation_track
//   @param time_seconds - time (seconds, >= 0) at which to place the key
//   @param value - key value: a number for float, a bool for bool, or a [R,G,B,A] (A optional, defaults 1.0) array for color
//   @param interpolation - Optional "linear" (default) | "cubic" | "constant" (float/color only; ignored for bool)
//   @returns { widget_bp_path: string, animation_name: string, widget_name: string, property_path: string, track_type: string, time_seconds: float, key_frame: int, keys_added: int, interpolation: string, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddWidgetAnimationKeyframe(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AnimationName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("animation_name"), AnimationName)) { return Missing; }
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    FString PropertyPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_path"), PropertyPath)) { return Missing; }
    double TimeSeconds = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time_seconds"), TimeSeconds)) { return Missing; }
    if (TimeSeconds < 0.0)
    {
        return mcp_common::MakeErrorReply(TEXT("time_seconds must be >= 0.0"));
    }
    TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
    if (!Value.IsValid())
    {
        return mcp_common::MissingParameterReply(TEXT("value"));
    }
    FString InterpolationRaw = TEXT("linear");
    Params->TryGetStringField(TEXT("interpolation"), InterpolationRaw);
    const FString Interpolation = InterpolationRaw.ToLower();
    if (Interpolation != TEXT("linear") && Interpolation != TEXT("cubic") && Interpolation != TEXT("constant"))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid interpolation '%s' (use linear|cubic|constant)"), *InterpolationRaw));
    }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidgetTree* WidgetTree = WidgetBP ? GetWidgetTree_Widget(WidgetBP, Error) : nullptr;
    if (!WidgetTree)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UWidgetAnimation* Animation = FindWidgetAnimationByName(WidgetBP, AnimationName);
    if (!Animation || !Animation->MovieScene)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget animation '%s' not found on %s"), *AnimationName, *WidgetBPPath));
    }

    UWidget* Widget = FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error);
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Resolve the possessable Guid for this widget from the animation's bindings.
    const FName WidgetFName = Widget->GetFName();
    FGuid PossessableGuid;
    for (const FWidgetAnimationBinding& Binding : Animation->AnimationBindings)
    {
        if (Binding.WidgetName == WidgetFName && Binding.SlotWidgetName.IsNone())
        {
            PossessableGuid = Binding.AnimationGuid;
            break;
        }
    }
    if (!PossessableGuid.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No animation binding for widget '%s' on '%s' -- call add_widget_animation_track first"),
            *WidgetName, *AnimationName));
    }

    // Resolve the matching property track by class+property name (float, then color, then bool).
    const FName PropertyFName(*PropertyPath);
    FString TrackType;
    UMovieScenePropertyTrack* PropTrack = FindWidgetPropertyTrack(Animation->MovieScene, UMovieSceneFloatTrack::StaticClass(), PossessableGuid, PropertyFName);
    if (PropTrack) { TrackType = TEXT("float"); }
    if (!PropTrack)
    {
        PropTrack = FindWidgetPropertyTrack(Animation->MovieScene, UMovieSceneColorTrack::StaticClass(), PossessableGuid, PropertyFName);
        if (PropTrack) { TrackType = TEXT("color"); }
    }
    if (!PropTrack)
    {
        PropTrack = FindWidgetPropertyTrack(Animation->MovieScene, UMovieSceneBoolTrack::StaticClass(), PossessableGuid, PropertyFName);
        if (PropTrack) { TrackType = TEXT("bool"); }
    }
    if (!PropTrack)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("No property track for '%s' on widget '%s' -- call add_widget_animation_track first"),
            *PropertyPath, *WidgetName));
    }
    if (PropTrack->GetAllSections().Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Property track has no section"));
    }
    UMovieSceneSection* Section = PropTrack->GetAllSections()[0];
    if (!Section)
    {
        return mcp_common::MakeErrorReply(TEXT("Property track section is null"));
    }

    Section->Modify();

    // Convert seconds -> tick frame; extend the section range to cover the key if needed.
    const FFrameRate TickResolution = Animation->MovieScene->GetTickResolution();
    const FFrameNumber KeyFrame = TickResolution.AsFrameTime(TimeSeconds).RoundToFrame();
    {
        TRange<FFrameNumber> Range = Section->GetRange();
        const bool bBelow = Range.GetLowerBound().IsClosed() && KeyFrame < Range.GetLowerBoundValue();
        const bool bAbove = Range.GetUpperBound().IsClosed() && KeyFrame > Range.GetUpperBoundValue();
        if (bBelow) { Section->SetStartFrame(TRangeBound<FFrameNumber>::Inclusive(KeyFrame)); }
        if (bAbove) { Section->SetEndFrame(TRangeBound<FFrameNumber>::Exclusive(KeyFrame + 1)); }
    }

    int32 KeysAdded = 0;

    if (TrackType == TEXT("bool"))
    {
        if (Value->Type != EJson::Boolean && Value->Type != EJson::Number)
        {
            return mcp_common::MakeErrorReply(TEXT("value must be a bool (or 0/non-zero number) for a bool track"));
        }
        const bool BoolValue = (Value->Type == EJson::Boolean) ? Value->AsBool() : (Value->AsNumber() != 0.0);
        TArrayView<FMovieSceneBoolChannel*> BoolChannels = Section->GetChannelProxy().GetChannels<FMovieSceneBoolChannel>();
        for (FMovieSceneBoolChannel* Channel : BoolChannels)
        {
            if (Channel)
            {
                Channel->GetData().UpdateOrAddKey(KeyFrame, BoolValue);
                ++KeysAdded;
            }
        }
    }
    else
    {
        // float -> 1 channel; color -> 4 channels (R,G,B,A).
        TArray<float> ChannelValues;
        if (TrackType == TEXT("float"))
        {
            if (Value->Type != EJson::Number)
            {
                return mcp_common::MakeErrorReply(TEXT("value must be a number for a float track"));
            }
            ChannelValues.Add(static_cast<float>(Value->AsNumber()));
        }
        else // color
        {
            if (Value->Type != EJson::Array)
            {
                return mcp_common::MakeErrorReply(TEXT("value must be a [R,G,B,A] array (A optional) for a color track"));
            }
            const TArray<TSharedPtr<FJsonValue>>& Arr = Value->AsArray();
            if (Arr.Num() < 3)
            {
                return mcp_common::MakeErrorReply(TEXT("value must have at least [R,G,B] for a color track"));
            }
            ChannelValues.Add(static_cast<float>(Arr[0]->AsNumber()));
            ChannelValues.Add(static_cast<float>(Arr[1]->AsNumber()));
            ChannelValues.Add(static_cast<float>(Arr[2]->AsNumber()));
            ChannelValues.Add(Arr.IsValidIndex(3) ? static_cast<float>(Arr[3]->AsNumber()) : 1.0f);
        }

        TArrayView<FMovieSceneFloatChannel*> FloatChannels = Section->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
        for (int32 i = 0; i < FloatChannels.Num(); ++i)
        {
            FMovieSceneFloatChannel* Channel = FloatChannels[i];
            if (!Channel) { continue; }
            const float ChannelValue = ChannelValues.IsValidIndex(i) ? ChannelValues[i] : ChannelValues.Last();

            if (Interpolation == TEXT("cubic"))
            {
                Channel->AddCubicKey(KeyFrame, ChannelValue);
            }
            else if (Interpolation == TEXT("constant"))
            {
                Channel->AddConstantKey(KeyFrame, ChannelValue);
            }
            else
            {
                Channel->AddLinearKey(KeyFrame, ChannelValue);
            }
            ++KeysAdded;
        }
    }

    if (KeysAdded == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No keyable channels found on the property section"));
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("animation_name"), AnimationName);
    Resp->SetStringField(TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("property_path"), PropertyPath);
    Resp->SetStringField(TEXT("track_type"), TrackType);
    Resp->SetNumberField(TEXT("time_seconds"), TimeSeconds);
    Resp->SetNumberField(TEXT("key_frame"), KeyFrame.Value);
    Resp->SetNumberField(TEXT("keys_added"), KeysAdded);
    Resp->SetStringField(TEXT("interpolation"), TrackType == TEXT("bool") ? TEXT("n/a") : Interpolation);
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_widget_animation_keyframe"), SaveErr);
}

// ---------------------------------------------------------------------------
// set_slot_properties -- GENERIC slot writer via reflection.
//
// Why it exists when there are already three slot shortcuts. UMG has 17 UPanelSlot subclasses
// and the three shortcuts (canvas / box family / grid) dispatch with one Cast<> per concrete type,
// so they cover 8: NINE have no other writer -- UButtonSlot (i.e. any widget
// inside a Button), UUniformGridSlot, UWrapBoxSlot, UStackBoxSlot, UWidgetSwitcherSlot,
// USafeZoneSlot, UScaleBoxSlot, UBackgroundBlurSlot and UWindowTitleBarAreaSlot. And there is no
// common base class to put them under: all 17 derive flat from UPanelSlot, which is exactly why
// the shortcuts have to duck-type with lambdas. Every UPanelSlot is a UObject with
// UPROPERTYs, so reflection covers them ALL -- including whatever the next engine version
// brings -- with no extra code.
//
// The shortcuts stay: they are the convenient, validated route for the 90% case
// (canvas anchors, box alignment, grid row/column) and they also normalise formats that pure
// reflection does not guess ([L,T,R,B] to FMargin, "Fill" to ESlateSizeRule).
// ---------------------------------------------------------------------------
// MCP:set_slot_properties - Set ANY property of a widget's UPanelSlot BY REFLECTION, whatever the concrete slot class is -- including the 9 of UMG's 17 slot types that no shortcut covers (UButtonSlot, which is every widget inside a Button; UUniformGridSlot, UWrapBoxSlot, UStackBoxSlot, UWidgetSwitcherSlot, USafeZoneSlot, UScaleBoxSlot, UBackgroundBlurSlot, UWindowTitleBarAreaSlot) and any slot type a future engine version adds. Accepts dotted paths into nested slot structs ("Padding.Left", "Size.SizeRule") and reports field by field what landed, same honest contract as set_widget_property: the oracle is applied[]/ignored[]/errors[], not success. For the common cases prefer the validated shortcuts, which also normalise formats this does not guess: set_widget_slot_properties (CanvasPanel anchors/offsets/z-order), set_box_slot_properties (HorizontalBox/VerticalBox/Overlay/Border/SizeBox/ScrollBox alignment+padding+size) and set_grid_slot_properties (GridPanel row/column/spans).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - name of the widget whose slot to edit (its PARENT panel decides the slot class; read it from get_widget_blueprint_tree include_slot=true)
//   @param properties [object] - {"<property or dotted path>": <value>, ...} written onto the slot object, e.g. {"Padding":[8,4,8,4],"HorizontalAlignment":"HAlign_Center"}. Enums accept the short name; FMargin accepts a scalar, [h,v] or [l,t,r,b].
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true).
//   @returns { widget_bp_path: string, widget_name: string, parent_name: string, slot_class: string, requested_paths: [string], applied: [string], ignored: [string], errors: [{path, error}], failed_paths: [string], applied_count: int, all_applied: bool, current_values: object, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleSetSlotProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }

    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid()
        || (*PropsObj)->Values.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: properties (a non-empty {\"name\": value} object)"));
    }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* Widget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error) : nullptr;
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The slot is CREATED by the parent panel when it adopts the child: a loose widget (or the tree
    // root) has none, and that is not a failure of the call but of where the widget sits.
    UPanelSlot* Slot = Widget->Slot;
    if (!Slot)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Widget '%s' has no slot: it is the tree root or has no parent panel. A slot only ")
            TEXT("exists for a widget parented under a panel -- add it with add_widget_extended ")
            TEXT("(parent_name=...) or move it with reparent_widget first."), *WidgetName));
    }

    Slot->Modify();

    FStyleWriteOutcome_Widget Outcome;
    WriteStylePathMap_Widget(Slot, *PropsObj, Outcome);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("parent_name"), Slot->Parent ? Slot->Parent->GetName() : TEXT(""));
    Resp->SetStringField(TEXT("slot_class"), Slot->GetClass()->GetName());
    FillPathWriteJson_Widget(Slot, *PropsObj, Outcome, Resp);

    FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave,
        TEXT("save=false: the Widget Blueprint is modified IN MEMORY and left dirty on purpose."));
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// apply_widget_style_preset -- one style block applied to MANY widgets.
//
// Why it exists. set_widget_property writes many paths of ONE widget; styling a screen is the
// opposite: THE SAME rule over every button, every title, every box. Without this, a panel of
// 12 buttons is 12 calls identical except for the name -- and as soon as the style changes,
// another 12. With this it is one call and a single compile+save.
//
// There is also an architectural reason, not just convenience: when the UI comes from a vendored
// library, its assets are READ-ONLY, so restyling them means deriving a child WBP in your own
// content folder and rewriting ITS style. That is exactly "apply a block to everything that
// matches", repeated in every child WBP.
// ---------------------------------------------------------------------------
// MCP:apply_widget_style_preset - Apply ONE block of style properties to EVERY widget of a Widget Blueprint that matches a filter, in a single pass with ONE compile + ONE save -- the multi-widget companion to set_widget_property's multi-path form. Selection is target_widgets (explicit names, wins over everything) OR target_class (matched with IsA, so subclasses count) AND/OR name_pattern (wildcard). At least one selector is required: to hit every widget in the tree say so explicitly with name_pattern="*". Every value goes through the same honest applier, so the per-widget report distinguishes what landed from what was ignored; dry_run=true resolves the match and reports it WITHOUT writing, which is the cheap way to check a filter before it touches 40 widgets.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param preset [object] - Dict of {"<property or dotted path>": <value>} applied to each matched widget, e.g. {"WidgetStyle.Normal.TintColor":"#1B1B1BFF"}. Optional when the source_* params are used; when both are given the inline values WIN, so you can say "the theme, but this one in red". A path that does not exist on a given matched widget lands in that widget's ignored[] instead of failing the whole call.
//   @param source_bp_path - THEME MODE: /Game path of a Widget Blueprint holding a REFERENCE widget to copy the style from, instead of writing the values inline. This is what makes one widget the single source of truth rather than scattering the same colour across 40 calls.
//   @param source_widget_name - Name of the reference widget inside source_bp_path. Required with source_bp_path.
//   @param source_property_paths [array] - Which paths to copy off the reference widget, e.g. ["WidgetStyle.Normal.TintColor","WidgetStyle.NormalPadding"]. A path that does not resolve on the REFERENCE is an error, not a silent skip: an incomplete theme that says nothing is worse than a refusal. Use audit_widget_style with the same reference + paths to find what has drifted before fixing it.
//   @param target_class - Class filter matched with IsA: a curated name (Button, TextBlock), a native class path (/Script/UMG.Button) or a Widget Blueprint asset path. Subclasses match too.
//   @param name_pattern - Wildcard on the widget's name (Btn_*, *_Title, or * for every widget). Combined with target_class as AND.
//   @param target_widgets [array] - Explicit widget names. When present, target_class and name_pattern are IGNORED and only these are touched (reported in selector_used).
//   @param dry_run [bool] - Resolve the match and report it without writing anything (default false).
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true; a dry run never saves).
//   @returns { widget_bp_path: string, selector_used: string, matched: [string], matched_count: int, dry_run: bool, results: [{widget_name, widget_class, applied, ignored, errors, failed_paths, applied_count, all_applied}], widgets_fully_applied: int, widgets_with_failures: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleApplyWidgetStylePreset(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The preset is built from TWO possible sources, and that is what makes this tool a theme
    // system rather than a pile of values: either the values come inline (preset), or they are
    // COPIED from a reference widget (source_*). With the latter there is ONE source of truth
    // -- that widget -- and not 40 copies of the brand colour spread over 40 calls.
    TSharedPtr<FJsonObject> EffectivePreset = MakeShared<FJsonObject>();
    FString SourceDescription;

    FString SourceBPPath, SourceWidgetName;
    const TArray<TSharedPtr<FJsonValue>>* SourcePathsArr = nullptr;
    const bool bHasSource =
        Params->TryGetStringField(TEXT("source_bp_path"), SourceBPPath) && !SourceBPPath.IsEmpty()
        && Params->TryGetStringField(TEXT("source_widget_name"), SourceWidgetName) && !SourceWidgetName.IsEmpty()
        && Params->TryGetArrayField(TEXT("source_property_paths"), SourcePathsArr)
        && SourcePathsArr && SourcePathsArr->Num() > 0;

    if (bHasSource)
    {
        FString SourceError;
        UWidgetBlueprint* SourceBP = UMGWidgetUtils::LoadWidgetBlueprint(SourceBPPath, SourceError);
        if (!SourceBP || !SourceBP->WidgetTree)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("source_bp_path '%s' is not a loadable Widget Blueprint: %s"), *SourceBPPath, *SourceError));
        }
        UWidget* SourceWidget = SourceBP->WidgetTree->FindWidget(FName(*SourceWidgetName));
        if (!SourceWidget)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("source widget '%s' not found in %s"), *SourceWidgetName, *SourceBPPath));
        }

        for (const TSharedPtr<FJsonValue>& E : *SourcePathsArr)
        {
            const FString Path = E.IsValid() ? E->AsString() : FString();
            if (Path.IsEmpty()) { continue; }

            FProperty* LeafProp = nullptr;
            void* LeafPtr = nullptr;
            FString ResolveErr;
            if (!mcp_common::ResolvePropertyPath(SourceWidget->GetClass(), SourceWidget, Path,
                                                 LeafProp, LeafPtr, ResolveErr))
            {
                // A path that does not exist on the SOURCE is an error of the call, not a finding:
                // copying "nothing" and carrying on would leave an incomplete theme without saying so.
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("source_property_paths: '%s' does not resolve on the source widget '%s' (%s): %s"),
                    *Path, *SourceWidgetName, *SourceWidget->GetClass()->GetName(), *ResolveErr));
            }
            if (TSharedPtr<FJsonValue> Val = mcp_common::PropertyValueToJson(LeafProp, LeafPtr))
            {
                EffectivePreset->SetField(Path, Val);
            }
        }
        SourceDescription = FString::Printf(TEXT("%s:%s"), *SourceBPPath, *SourceWidgetName);
    }

    // The inline values are applied AFTER, so they win: that allows "the theme, but this
    // button in red" without duplicating the whole theme.
    const TSharedPtr<FJsonObject>* PresetObj = nullptr;
    if (Params->TryGetObjectField(TEXT("preset"), PresetObj) && PresetObj && PresetObj->IsValid())
    {
        for (const auto& Pair : (*PresetObj)->Values)
        {
            EffectivePreset->SetField(FString(*Pair.Key), Pair.Value);
        }
    }

    if (EffectivePreset->Values.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to apply: pass preset {path: value}, or source_bp_path + source_widget_name + "
                 "source_property_paths to copy the style off a reference widget."));
    }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidgetTree* WidgetTree = WidgetBP ? GetWidgetTree_Widget(WidgetBP, Error) : nullptr;
    if (!WidgetTree)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TArray<UWidget*> AllWidgets;
    WidgetTree->GetAllWidgets(AllWidgets);

    // --- Selection. Explicit names win over filters: if the caller bothered to list them, a
    // target_class carried over from a previous call must not trim them.
    TArray<UWidget*> Matched;
    FString SelectorUsed;
    TArray<FString> UnknownNames;

    const TArray<TSharedPtr<FJsonValue>>* ExplicitArr = nullptr;
    const bool bHasExplicit = Params->TryGetArrayField(TEXT("target_widgets"), ExplicitArr)
                           && ExplicitArr && ExplicitArr->Num() > 0;

    FString TargetClassName;
    const bool bHasClass = Params->TryGetStringField(TEXT("target_class"), TargetClassName)
                        && !TargetClassName.IsEmpty();

    FString NamePattern;
    const bool bHasPattern = Params->TryGetStringField(TEXT("name_pattern"), NamePattern)
                          && !NamePattern.IsEmpty();

    if (bHasExplicit)
    {
        SelectorUsed = TEXT("target_widgets");
        for (const TSharedPtr<FJsonValue>& Entry : *ExplicitArr)
        {
            const FString WantedName = Entry.IsValid() ? Entry->AsString() : FString();
            if (WantedName.IsEmpty())
            {
                continue;
            }
            if (UWidget* Found = WidgetTree->FindWidget(FName(*WantedName)))
            {
                Matched.AddUnique(Found);
            }
            else
            {
                UnknownNames.Add(WantedName);
            }
        }
    }
    else if (bHasClass || bHasPattern)
    {
        UClass* FilterClass = nullptr;
        if (bHasClass)
        {
            FilterClass = ResolveAnyWidgetClass(TargetClassName);
            if (!FilterClass)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("target_class '%s' does not resolve to a UWidget subclass. Use a curated name ")
                    TEXT("(list_widget_types), a native path like /Script/UMG.Button, or a Widget ")
                    TEXT("Blueprint asset path."), *TargetClassName));
            }
        }

        TArray<FString> SelectorParts;
        if (bHasClass)   { SelectorParts.Add(FString::Printf(TEXT("target_class=%s"), *FilterClass->GetName())); }
        if (bHasPattern) { SelectorParts.Add(FString::Printf(TEXT("name_pattern=%s"), *NamePattern)); }
        SelectorUsed = FString::Join(SelectorParts, TEXT(" AND "));

        for (UWidget* W : AllWidgets)
        {
            if (!W)
            {
                continue;
            }
            if (FilterClass && !W->IsA(FilterClass))
            {
                continue;
            }
            if (bHasPattern && !W->GetName().MatchesWildcard(NamePattern))
            {
                continue;
            }
            Matched.Add(W);
        }
    }
    else
    {
        // With no selector, "everything" is NOT styled by default: applying a preset to a whole
        // tree by omission is exactly the accident this command could cause most easily.
        return mcp_common::MakeErrorReply(
            TEXT("No selector given. Pass target_widgets (explicit names), target_class, and/or ")
            TEXT("name_pattern. To deliberately hit every widget in the tree, pass name_pattern=*"));
    }

    bool bDryRun = false;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("selector_used"), SelectorUsed);
    Resp->SetStringField(TEXT("style_source"), bHasSource ? SourceDescription : TEXT("inline preset"));
    Resp->SetBoolField(TEXT("dry_run"), bDryRun);

    TArray<TSharedPtr<FJsonValue>> MatchedArr;
    for (UWidget* W : Matched)
    {
        MatchedArr.Add(MakeShared<FJsonValueString>(W->GetName()));
    }
    Resp->SetArrayField(TEXT("matched"), MatchedArr);
    Resp->SetNumberField(TEXT("matched_count"), Matched.Num());

    if (UnknownNames.Num() > 0)
    {
        Resp->SetArrayField(TEXT("unknown_widget_names"), mcp_common::StringsToJson(UnknownNames));
    }

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 FullyApplied = 0;
    int32 WithFailures = 0;

    if (!bDryRun)
    {
        for (UWidget* W : Matched)
        {
            W->Modify();

            FStyleWriteOutcome_Widget Outcome;
            WriteStylePathMap_Widget(W, EffectivePreset, Outcome);

            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("widget_name"), W->GetName());
            Entry->SetStringField(TEXT("widget_class"), W->GetClass()->GetName());
            FillWriteReportJson_Widget(Outcome, Entry);
            Results.Add(MakeShared<FJsonValueObject>(Entry));

            const bool bClean = Outcome.FailedPaths.Num() == 0 && !Outcome.Report.HasFailures();
            if (bClean)
            {
                ++FullyApplied;
            }
            else
            {
                ++WithFailures;
            }
        }
    }
    Resp->SetArrayField(TEXT("results"), Results);
    Resp->SetNumberField(TEXT("widgets_fully_applied"), FullyApplied);
    Resp->SetNumberField(TEXT("widgets_with_failures"), WithFailures);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bSaved = false;
    FString SaveErr;
    if (bDryRun)
    {
        SaveErr = TEXT("dry_run=true: nothing was written, so nothing was saved.");
    }
    else if (Matched.Num() == 0)
    {
        SaveErr = TEXT("no widget matched the selector: nothing was written, so nothing was saved.");
    }
    else
    {
        FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBP);
        bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave,
            TEXT("save=false: the Widget Blueprint is modified IN MEMORY and left dirty on purpose."));
    }
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// render_widget_preview -- THE VISUAL ORACLE.
//
// Why it exists. Styling is the one kind of work where the honest oracle is "it looks right";
// without it, answering means starting PIE and having someone look. With it, "what does this WBP
// look like" is a static read plus one image to SHOW.
//
// WARNING: WHAT THIS IMAGE IS NOT, and it has to be said before someone uses it to diagnose:
// it is a DESIGN preview, the same one the Designer paints, not a capture of the in-game HUD. It
// is built with the designer flags set (Designing|Previewing), so everything the widget's code
// guards with IsDesignTime() behaves as in the editor; and everything that depends on the live
// game -- bindings to a PlayerState, text filled in NativeConstruct, data that arrives through a
// ViewModel -- comes out at its default value, EMPTY, not the game's. For that there are
// read_pie_widgets and capture_pie_screenshot.
//
// There is a second limit, this one from the engine: FWidgetRenderer::DrawWidget returns nullptr
// when FApp::CanEverRender() is false (editor with -nullrhi / headless without RHI). That is NOT
// disguised as an empty image: the answer is rendered:false with the reason.
// ---------------------------------------------------------------------------
// MCP:render_widget_preview - Render a Widget Blueprint to a PNG on disk WITHOUT running the game: builds the widget with the editor's own designer flags, draws it through FWidgetRenderer into a render target and writes the file. This is what turns "does this style look right?" from a PIE session someone has to watch into a static read plus one image to LOOK at. HONEST SCOPE: it is a DESIGN preview, the same thing the UMG Designer shows -- not the live HUD. Anything that only exists while the game runs (text filled in NativeConstruct, property bindings, ViewModel data, anything behind an IsDesignTime() guard) renders at its default/empty value. Use read_pie_widgets or capture_pie_screenshot for the running session. Returns rendered:false with a reason (never a blank image) when the editor has no RHI (-nullrhi/headless), because FWidgetRenderer cannot draw there.
//   @param widget_bp_path [string!] - /Game path of the Widget Blueprint asset to render
//   @param output_path - Optional output file. A bare name lands under <ProjectSaved>/MCP/WidgetPreviews/, an absolute path is used verbatim, and .png is appended when missing. Defaults to the asset's name.
//   @param width [int] - Optional render width in pixels. Omit BOTH width and height to render at the widget's own desired size (a Slate prepass measures it).
//   @param height [int] - Optional render height in pixels. Omit BOTH width and height to render at the widget's own desired size.
//   @param scale [number] - Optional draw scale applied by the renderer (default 1.0), the equivalent of DPI scaling in the Designer.
//   @param opaque_background [bool] - Optional (default false). Force every pixel's alpha to 255 so the PNG is not transparent where the widget did not paint -- the same trick capture_viewport uses. Leave false to keep real transparency.
//   @returns { widget_bp_path: string, rendered: bool, file_path: string, width: int, height: int, scale: number, drew_at_desired_size: bool, byte_size: int, designer_flags: string, preview_note: string, error?: string }

TSharedPtr<FJsonObject> HandleRenderWidgetPreview(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UClass* GeneratedClass = WidgetBP->GeneratedClass;
    if (!GeneratedClass || !GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Widget Blueprint '%s' has no compiled UUserWidget class to instantiate. Compile it ")
            TEXT("first (compile_blueprint) -- a WBP that never compiled cannot be rendered."),
            *WidgetBPPath));
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World, TEXT("No editor world available to own the preview widget (GEditor has no edit context)."))) { return NoWorld; }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("widget_bp_path"), WidgetBPPath);

    // The engine gate, BEFORE building anything: DrawWidget returns nullptr without an RHI, and a
    // silent nullptr there would read as "the widget paints nothing", which is exactly the false
    // conclusion this command exists to avoid.
    if (!FApp::CanEverRender())
    {
        Resp->SetBoolField(TEXT("rendered"), false);
        Resp->SetStringField(TEXT("error"),
            TEXT("this editor cannot render (FApp::CanEverRender() is false -- started with -nullrhi ")
            TEXT("or otherwise headless). FWidgetRenderer needs a real RHI; there is no headless ")
            TEXT("fallback that would produce an honest image."));
        return mcp_common::MakeOkReply(Resp);
    }

    UUserWidget* Preview = CreateWidget<UUserWidget>(World, GeneratedClass);
    if (!Preview)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("CreateWidget failed for '%s'"), *GeneratedClass->GetName()));
    }

    // The same flags the Designer sets. ExecutePreConstruct is included on purpose: PreConstruct is
    // where a well-written widget places what is visible in the editor, and without it the image
    // would come out emptier than the widget's author intends.
    FString DesignerFlagsUsed = TEXT("none (non-editor build)");
    Preview->SetDesignerFlags(EWidgetDesignFlags::Designing
                            | EWidgetDesignFlags::Previewing
                            | EWidgetDesignFlags::ExecutePreConstruct);
    DesignerFlagsUsed = TEXT("Designing|Previewing|ExecutePreConstruct");

    TSharedRef<SWidget> SlateWidget = Preview->TakeWidget();

    double ScaleNum = 1.0;
    Params->TryGetNumberField(TEXT("scale"), ScaleNum);
    const float DrawScale = FMath::Clamp(static_cast<float>(ScaleNum), 0.05f, 8.0f);

    // Size: the requested one, or the one the widget itself says it needs. The latter requires a
    // prepass -- without it, GetDesiredSize() returns zero and the image would be 0x0.
    double WidthNum = 0.0;
    double HeightNum = 0.0;
    const bool bHasWidth  = Params->TryGetNumberField(TEXT("width"), WidthNum);
    const bool bHasHeight = Params->TryGetNumberField(TEXT("height"), HeightNum);

    FVector2D DrawSize(FMath::RoundToInt(WidthNum), FMath::RoundToInt(HeightNum));
    bool bDrewAtDesiredSize = false;
    if (!bHasWidth || !bHasHeight)
    {
        // Without a prepass, GetDesiredSize() returns zero and the image would be 0x0.
        // WARNING: GetDesiredSize() does NOT return an FVector2D but
        // UE::Slate::FDeprecateVector2DResult (SWidget.h); X/Y are read by hand instead of
        // trusting the implicit conversion, which is exactly what that type exists to police.
        SlateWidget->SlatePrepass(DrawScale);
        const auto DesiredRaw = SlateWidget->GetDesiredSize();
        const FVector2D Desired(DesiredRaw.X, DesiredRaw.Y);

        if (!bHasWidth)  { DrawSize.X = Desired.X; }
        if (!bHasHeight) { DrawSize.Y = Desired.Y; }
        bDrewAtDesiredSize = !bHasWidth && !bHasHeight;
    }

    // A widget whose desired size is 0 (an empty CanvasPanel, e.g.) cannot produce an image: it
    // falls back to a 512x512 canvas and says so, instead of writing a 0-byte PNG.
    FString SizeNote;
    if (DrawSize.X < 1.0 || DrawSize.Y < 1.0)
    {
        SizeNote = FString::Printf(
            TEXT("the widget's desired size was %gx%g (nothing to draw): fell back to 512x512. ")
            TEXT("A CanvasPanel root with no explicit slot sizes reports zero -- pass width/height."),
            DrawSize.X, DrawSize.Y);
        DrawSize = FVector2D(512.0, 512.0);
        bDrewAtDesiredSize = false;
    }
    DrawSize.X = FMath::Clamp(DrawSize.X, 1.0, 8192.0);
    DrawSize.Y = FMath::Clamp(DrawSize.Y, 1.0, 8192.0);

    FWidgetRenderer Renderer(/*bUseGammaCorrection=*/ true);
    Renderer.SetIsPrepassNeeded(true);

    UTextureRenderTarget2D* RenderTarget = Renderer.DrawWidget(SlateWidget, DrawSize);
    if (!RenderTarget)
    {
        Preview->ReleaseSlateResources(/*bReleaseChildren=*/ true);
        Resp->SetBoolField(TEXT("rendered"), false);
        Resp->SetStringField(TEXT("error"),
            TEXT("FWidgetRenderer::DrawWidget returned no render target. This is the engine's own ")
            TEXT("CanEverRender gate firing late; the widget itself was built fine."));
        return mcp_common::MakeOkReply(Resp);
    }

    FImage RawImage;
    if (!FImageUtils::GetRenderTargetImage(RenderTarget, RawImage))
    {
        Preview->ReleaseSlateResources(/*bReleaseChildren=*/ true);
        Resp->SetBoolField(TEXT("rendered"), false);
        Resp->SetStringField(TEXT("error"),
            TEXT("the widget rendered but its render target could not be read back "
                 "(FImageUtils::GetRenderTargetImage failed)."));
        return mcp_common::MakeOkReply(Resp);
    }

    // WARNING: the readback can NOT be assumed to be BGRA8. FWidgetRenderer::CreateTargetFor asks
    // for the format Slate recommends (WidgetRenderer.cpp, GetSlateRecommendedColorFormat),
    // which depending on platform and settings can be floating point -- and FImage::AsBGRA8() has
    // a check(Format == BGRA8) that does NOT return an error: it crashes the editor. Always normalize.
    FImage Image;
    RawImage.CopyTo(Image, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

    // Optional opaque alpha. A widget only paints where it paints, so by default the PNG carries
    // real transparency (the render target is cleared to FLinearColor::Transparent) -- correct
    // for compositing, confusing to look at in a viewer with a white background. The same remedy,
    // for the same reason, as capture_viewport.
    bool bOpaque = false;
    Params->TryGetBoolField(TEXT("opaque_background"), bOpaque);
    if (bOpaque)
    {
        TArrayView64<FColor> Pixels = Image.AsBGRA8();
        for (FColor& Px : Pixels)
        {
            Px.A = 255;
        }
    }

    // --- Output path. Same rule as capture_viewport: bare name -> the project's folder,
    // absolute path used as is, .png appended if missing.
    FString OutputPath;
    Params->TryGetStringField(TEXT("output_path"), OutputPath);
    if (OutputPath.IsEmpty())
    {
        OutputPath = WidgetBP->GetName();
    }
    if (!OutputPath.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase))
    {
        OutputPath += TEXT(".png");
    }
    if (FPaths::IsRelative(OutputPath))
    {
        OutputPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP"), TEXT("WidgetPreviews"), OutputPath);
    }
    OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*FPaths::GetPath(OutputPath));

    const bool bWritten = FImageUtils::SaveImageByExtension(*OutputPath, Image);

    Preview->ReleaseSlateResources(/*bReleaseChildren=*/ true);

    Resp->SetBoolField(TEXT("rendered"), bWritten);
    Resp->SetStringField(TEXT("file_path"), OutputPath);
    Resp->SetNumberField(TEXT("width"), Image.SizeX);
    Resp->SetNumberField(TEXT("height"), Image.SizeY);
    Resp->SetNumberField(TEXT("scale"), DrawScale);
    Resp->SetBoolField(TEXT("drew_at_desired_size"), bDrewAtDesiredSize);
    Resp->SetNumberField(TEXT("byte_size"), bWritten ? IFileManager::Get().FileSize(*OutputPath) : 0);
    Resp->SetStringField(TEXT("designer_flags"), DesignerFlagsUsed);
    Resp->SetStringField(TEXT("preview_note"),
        TEXT("design-time preview (what the UMG Designer shows), NOT the live HUD: runtime "
             "bindings, NativeConstruct text and ViewModel data render at their default value."));
    if (!SizeNote.IsEmpty())
    {
        Resp->SetStringField(TEXT("size_note"), SizeNote);
    }
    if (!bWritten)
    {
        Resp->SetStringField(TEXT("error"), FString::Printf(
            TEXT("the widget rendered but the PNG could not be written to '%s' (check the path and "
                 "that the file is not locked)."), *OutputPath));
    }
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// UMG PROPERTY BINDINGS -- the Designer's "Bind" dropdown.
//
// What it is. In the details panel, to the right of almost every widget property, there is a
// "Bind" dropdown that ties it to a function of the Widget Blueprint: instead of a fixed value,
// the property is recomputed every frame by calling that function. It is how the UI of a
// Blueprint project connects to data WITHOUT MVVM.
//
// Why dedicated commands are needed. These bindings do NOT live in the WidgetTree or in the
// graph: they live in UWidgetBlueprint::Bindings, a separate TArray<FDelegateEditorBinding>.
// That property is NOT exposed to Python -- get_editor_property("bindings") throws --,
// so execute_python is not a way out either. Without this there is NO headless route.
//
// THE RULE THAT GOVERNS IT ALL, read from the engine's own validator
// (UMGEditor/Private/WidgetBlueprint.cpp): a property P is bindable if and only if its widget
// declares an FDelegateProperty named "<P>Delegate" -- Text -> TextDelegate, ToolTipText ->
// ToolTipTextDelegate, Visibility -> VisibilityDelegate. And the function must (a) have a
// signature compatible with that delegate -- same return type, ZERO parameters -- and (b) be
// PURE (FUNC_Const or FUNC_BlueprintPure), because it is called while painting.
//
// Those checks are not reimplemented: FDelegateEditorBinding::IsBindingValid is called, which is
// the same code the UMG compiler runs, and its messages are returned verbatim. A validation of our
// own that drifted from the engine's would be worse than none: it would say yes and then the
// asset would not compile.
// ---------------------------------------------------------------------------

namespace
{
    /** The properties of this widget that ACCEPT a binding, by the "<P>Delegate" rule. */
    TArray<FString> ListBindableProperties_Widget(const UWidget* Widget)
    {
        TArray<FString> Out;
        if (!Widget)
        {
            return Out;
        }
        for (TFieldIterator<FDelegateProperty> It(Widget->GetClass()); It; ++It)
        {
            const FString DelegateName = It->GetName();
            if (DelegateName.EndsWith(TEXT("Delegate"), ESearchCase::CaseSensitive))
            {
                const FString PropName = DelegateName.LeftChop(8); // without the "Delegate" suffix
                // It only counts if the companion property really exists: there are delegates
                // that end in "Delegate" without being anyone's companion.
                if (!PropName.IsEmpty() && Widget->GetClass()->FindPropertyByName(FName(*PropName)))
                {
                    Out.AddUnique(PropName);
                }
            }
        }
        Out.Sort();
        return Out;
    }

    /** Dumps the messages of an FCompilerResultsLog into a JSON array of strings. */
    TArray<TSharedPtr<FJsonValue>> CompilerLogToJson_Widget(const FCompilerResultsLog& Log)
    {
        TArray<TSharedPtr<FJsonValue>> Out;
        for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
        {
            Out.Add(MakeShared<FJsonValueString>(Msg->ToText().ToString()));
        }
        return Out;
    }
}

// MCP:list_widget_property_bindings - List the UMG PROPERTY BINDINGS of a Widget Blueprint -- the "Bind" dropdown next to a property in the Designer's details panel, which makes that property recompute every frame by calling a function instead of holding a fixed value. These live in UWidgetBlueprint::Bindings, NOT in the widget tree and NOT in the event graph, so no other reader sees them; and UWidgetBlueprint::Bindings is NOT exposed to Python either (get_editor_property("bindings") throws), so this is the only headless way to read them. Each entry is re-validated live with the engine's own FDelegateEditorBinding::IsBindingValid, so a binding whose function was renamed or made non-pure comes back valid:false WITH the compiler's reason -- that is the difference between "there is a binding" and "there is a binding that works".
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param include_bindable_properties [bool] - Also list, per widget in the tree, which properties ACCEPT a binding at all (default false). A property is bindable only if its widget declares a companion delegate named "<Property>Delegate" -- this saves guessing.
//   @returns { widget_bp_path: string, binding_count: int, bindings: [{ widget_name, property_name, function_name, kind, member_guid, source_property, valid, validation_messages: [string] }], bindable_properties?: {widget_name: [string]} }

TSharedPtr<FJsonObject> HandleListWidgetPropertyBindings(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(Error);
    }

// UWidgetBlueprint::Bindings is WITH_EDITORONLY_DATA (WidgetBlueprint.h). This guard is belt
// and braces: the module depends on UnrealEd, so it is NEVER compiled without the editor and the
// branch above is dead code. That is why the two write handlers below access Bindings without a
// guard -- if that ever stopped being true they would fail to COMPILE, which is a loud and cheap
// failure, not a silent one.
    UClass* GeneratedClass = WidgetBP->GeneratedClass;

    TArray<TSharedPtr<FJsonValue>> Entries;
    for (const FDelegateEditorBinding& Binding : WidgetBP->Bindings)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("widget_name"), Binding.ObjectName);
        Entry->SetStringField(TEXT("property_name"), Binding.PropertyName.ToString());
        Entry->SetStringField(TEXT("function_name"), Binding.FunctionName.ToString());
        Entry->SetStringField(TEXT("kind"), Binding.Kind == EBindingKind::Function ? TEXT("Function") : TEXT("Property"));
        Entry->SetStringField(TEXT("member_guid"), Binding.MemberGuid.ToString());
        Entry->SetStringField(TEXT("source_property"), Binding.SourceProperty.ToString());

        // Re-validate live, with the SAME code the UMG compiler runs. That the entry exists in
        // the array does not mean it still works: renaming the function or removing its purity
        // leaves it in place and dead.
        bool bValid = false;
        TArray<TSharedPtr<FJsonValue>> Messages;
        if (GeneratedClass)
        {
            FCompilerResultsLog Log;
            bValid = Binding.IsBindingValid(GeneratedClass, WidgetBP, Log);
            Messages = CompilerLogToJson_Widget(Log);
        }
        else
        {
            Messages.Add(MakeShared<FJsonValueString>(
                TEXT("the Widget Blueprint has no compiled class: validation was skipped, not passed")));
        }
        Entry->SetBoolField(TEXT("valid"), bValid);
        Entry->SetArrayField(TEXT("validation_messages"), Messages);

        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("widget_bp_path"), WidgetBPPath);
    Resp->SetNumberField(TEXT("binding_count"), Entries.Num());
    Resp->SetArrayField(TEXT("bindings"), Entries);

    bool bIncludeBindable = false;
    Params->TryGetBoolField(TEXT("include_bindable_properties"), bIncludeBindable);
    if (bIncludeBindable && WidgetBP->WidgetTree)
    {
        TArray<UWidget*> AllWidgets;
        WidgetBP->WidgetTree->GetAllWidgets(AllWidgets);

        TSharedPtr<FJsonObject> Bindable = MakeShared<FJsonObject>();
        for (const UWidget* W : AllWidgets)
        {
            if (!W)
            {
                continue;
            }
            Bindable->SetArrayField(W->GetName(), mcp_common::StringsToJson(ListBindableProperties_Widget(W)));
        }
        Resp->SetObjectField(TEXT("bindable_properties"), Bindable);
    }

    return mcp_common::MakeOkReply(Resp);
}

// MCP:add_widget_property_binding - Bind a widget PROPERTY to a function of the Widget Blueprint -- the Designer's "Bind" dropdown, headless. The property then recomputes every frame from that function instead of holding a fixed value (a health bar's Percent, a label's Text, a panel's Visibility). TWO RULES, both enforced by the ENGINE'S OWN validator (FDelegateEditorBinding::IsBindingValid, the same code the UMG compiler runs -- this tool does not second-guess it): the property is bindable only if its widget declares a companion delegate named "<Property>Delegate" (Text -> TextDelegate), and the function must have a COMPATIBLE SIGNATURE (same return type, NO parameters) and be PURE (BlueprintPure or const), because it is called while painting. A refusal names which of the two failed and, when the property is not bindable at all, lists the ones that are. Create the function first with add_blueprint_function + set its return type; one property can hold only ONE binding, so re-binding the same property REPLACES it (reported in replaced_existing).
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - Name of the widget in the tree whose property gets bound (must be a direct variable of the UserWidget)
//   @param property_name - Property to bind, e.g. "Text" on a TextBlock, "Percent" on a ProgressBar, "Visibility" on any widget. Must have a companion "<property_name>Delegate" on the widget's class.
//   @param function_name - Name of the function ON THIS Widget Blueprint that supplies the value. Must be pure and take no parameters, returning the property's type.
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true). The compile is what turns the editor binding into the runtime one, so a false here leaves the binding authored but not live.
//   @returns { widget_bp_path: string, widget_name: string, property_name: string, function_name: string, bound: bool, valid: bool, replaced_existing: bool, member_guid: string, validation_messages: [string], bindable_properties?: [string], saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleAddWidgetPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, WidgetName, PropertyName, FunctionName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), FunctionName)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    UWidget* Widget = WidgetBP ? FindWidgetInBlueprint_Widget(WidgetBP, WidgetName, Error) : nullptr;
    if (!Widget)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Bindability pre-check. IsBindingValid does it too, but there a failure gets confused with a
    // signature failure; here the exact reason can be given AND what CAN be bound.
    const FName CompanionDelegate(*(PropertyName + TEXT("Delegate")));
    const bool bHasCompanion = FindFProperty<FDelegateProperty>(Widget->GetClass(), CompanionDelegate) != nullptr;
    const bool bIsEventStyle  = FindFProperty<FDelegateProperty>(Widget->GetClass(), FName(*PropertyName)) != nullptr;
    if (!bHasCompanion && !bIsEventStyle)
    {
        TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
        Resp->SetStringField(TEXT("property_name"), PropertyName);
        Resp->SetBoolField(TEXT("bound"), false);
        Resp->SetBoolField(TEXT("valid"), false);

        Resp->SetArrayField(TEXT("bindable_properties"), mcp_common::StringsToJson(ListBindableProperties_Widget(Widget)));
        Resp->SetStringField(TEXT("error"), FString::Printf(
            TEXT("'%s' is not bindable on %s (%s): UMG requires a companion delegate named '%s', ")
            TEXT("and that class does not declare one. bindable_properties lists what this widget ")
            TEXT("does accept."),
            *PropertyName, *WidgetName, *Widget->GetClass()->GetName(), *CompanionDelegate.ToString()));
        return mcp_common::MakeOkReply(Resp);
    }

    // The function is looked up on the SKELETON class: it is the one that is up to date right after
    // creating a new function, before a full compile.
    UClass* SkeletonClass = WidgetBP->SkeletonGeneratedClass;
    UClass* GeneratedClass = WidgetBP->GeneratedClass;
    UClass* LookupClass = SkeletonClass ? SkeletonClass : GeneratedClass;
    if (!LookupClass)
    {
        return mcp_common::MakeErrorReply(
            TEXT("the Widget Blueprint has no compiled/skeleton class yet: compile_blueprint first."));
    }
    if (!LookupClass->FindFunctionByName(FName(*FunctionName), EIncludeSuperFlag::IncludeSuper))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("function '%s' not found on %s. Create it first (add_blueprint_function), give it a ")
            TEXT("return value of the property's type and NO parameters, and make it pure."),
            *FunctionName, *WidgetBPPath));
    }

    FDelegateEditorBinding NewBinding;
    NewBinding.ObjectName = WidgetName;
    NewBinding.PropertyName = FName(*PropertyName);
    NewBinding.FunctionName = FName(*FunctionName);
    NewBinding.Kind = EBindingKind::Function;
    // SourcePath is left EMPTY on purpose. With a path, IsBindingValid validates the path and
    // RETURNS; without one it enters the branch that checks signature AND PURITY
    // (WidgetBlueprint.cpp), which is the strict one. The strict validation is preferred: a
    // binding that comes out valid:true here and then does not compile would be a lying "success".

    // The GUID is what makes the binding survive a rename of the function: ToRuntimeBinding
    // prefers it over the name when it is set (WidgetBlueprint.cpp). Without it, a rename breaks it.
    FGuid FunctionGuid;
    if (UBlueprint::GetGuidFromClassByFieldName<UFunction>(LookupClass, FName(*FunctionName), FunctionGuid))
    {
        NewBinding.MemberGuid = FunctionGuid;
    }

    WidgetBP->Modify();

    // A property admits only ONE binding: FDelegateEditorBinding's operator== compares exactly
    // ObjectName+PropertyName, so re-binding replaces instead of duplicating.
    const bool bReplaced = WidgetBP->Bindings.Remove(NewBinding) > 0;
    WidgetBP->Bindings.Add(NewBinding);

    bool bValid = false;
    TArray<TSharedPtr<FJsonValue>> Messages;
    if (GeneratedClass)
    {
        FCompilerResultsLog Log;
        bValid = NewBinding.IsBindingValid(GeneratedClass, WidgetBP, Log);
        Messages = CompilerLogToJson_Widget(Log);
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave,
        TEXT("save=false: the binding is authored IN MEMORY but not compiled, so it is not live yet."));

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("property_name"), PropertyName);
    Resp->SetStringField(TEXT("function_name"), FunctionName);
    Resp->SetBoolField(TEXT("bound"), true);
    Resp->SetBoolField(TEXT("valid"), bValid);
    Resp->SetBoolField(TEXT("replaced_existing"), bReplaced);
    Resp->SetStringField(TEXT("member_guid"), NewBinding.MemberGuid.ToString());
    Resp->SetArrayField(TEXT("validation_messages"), Messages);
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// MCP:remove_widget_property_binding - Remove a UMG property binding (the Designer's "Bind" dropdown set back to none), so the property goes back to holding its authored fixed value. Inverse of add_widget_property_binding and idempotent: a property that was not bound is an honest success with removed:false, never an error.
//   @param widget_bp_path - /Game path of the Widget Blueprint asset
//   @param widget_name - Name of the widget whose binding to drop
//   @param property_name - The bound property, e.g. "Text". Matching is on widget_name + property_name, which is exactly how UMG identifies a binding (one binding per property, max).
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true).
//   @returns { widget_bp_path: string, widget_name: string, property_name: string, removed: bool, already_absent: bool, remaining_binding_count: int, saved: bool, save_error: string }

TSharedPtr<FJsonObject> HandleRemoveWidgetPropertyBinding(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, WidgetName, PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    if (!WidgetBP)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FDelegateEditorBinding Probe;
    Probe.ObjectName = WidgetName;
    Probe.PropertyName = FName(*PropertyName);

    WidgetBP->Modify();
    const int32 RemovedCount = WidgetBP->Bindings.Remove(Probe);
    const bool bRemoved = RemovedCount > 0;

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);

    bool bSaved = false;
    FString SaveErr;
    if (bRemoved)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
        bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave, TEXT("save=false: removed IN MEMORY only."));
    }
    else
    {
        // Do not touch the asset for a call that changed nothing: marking it dirty and recompiling
        // it would be an invisible side effect of an operation that was a no-op.
        SaveErr = TEXT("nothing was bound on that property: no change, so nothing was saved.");
    }

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetStringField(TEXT("property_name"), PropertyName);
    Resp->SetBoolField(TEXT("removed"), bRemoved);
    Resp->SetBoolField(TEXT("already_absent"), !bRemoved);
    Resp->SetNumberField(TEXT("remaining_binding_count"), WidgetBP->Bindings.Num());
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// GAMEPAD NAVIGATION
//
// Why a command is needed and the generic writer is not enough. UWidget::Navigation is
// UPROPERTY(Instanced) and starts out nullptr (Widget.h): it is not an asset with a path that
// a reflection setter can point at, it is a SUBOBJECT that has to be created. Descending through
// "Navigation.Up.Rule" fails with "segment 'Navigation' is null". Without this, a co-op game
// played with a gamepad needs someone opening the editor.
//
// THE EDITOR'S PATH IS COPIED, not the runtime one, and the differences matter:
//   - The subobject is created with RF_Transactional, and with the template flags propagated
//     when the widget is a template -- which is exactly what a widget of a WidgetTree is
//     (UMGEditor/Private/Customizations/WidgetNavigationCustomization.cpp).
//   - WARNING: if all six rules end up on Escape, the editor DISCARDS the subobject
//     (Widget->Navigation = nullptr, same customization :525-530). It is not an optimization:
//     IsDefaultNavigation() is what tells "no navigation authored" apart from "authored
//     navigation that happens to be the default", and leaving the object in place dirties the
//     asset's diff forever. It is replicated here.
// ---------------------------------------------------------------------------

// MCP:set_widget_navigation - Author a widget's GAMEPAD/KEYBOARD navigation rules -- what gets focus when the player pushes up, down, left, right, Tab or Shift+Tab off this widget. Without this a menu cannot be driven with a pad, and it is NOT reachable through set_widget_property: UWidget::Navigation is an Instanced SUBOBJECT that starts null, so there is no asset path a reflection setter could point at and a dotted path fails on a null segment. Follows the editor's own flow, including the two details that matter: the subobject is created RF_Transactional with template flags propagated, and if all six directions end up on Escape the subobject is DISCARDED (that is what tells "no navigation authored" apart from "navigation authored that happens to be the default", and it is what the Designer does). Read the result back with get_widget_blueprint_tree include_navigation=true.
//   @param widget_bp_path [string!] - /Game path of the Widget Blueprint asset
//   @param widget_name [string!] - Name of the widget in the tree whose navigation to author
//   @param rules [object] - Dict of {direction: rule}. Direction is Up|Down|Left|Right|Next|Previous (Next/Previous are Tab/Shift+Tab). The value is either a rule name string ("Wrap", "Stop", "Escape") or an object {"rule":"Explicit","widget_to_focus":"Btn_Confirm"} when the rule needs a target. Rules: Escape (default -- let the search continue outward), Stop (movement dies here), Wrap (cycle inside the container), Explicit (jump to widget_to_focus).
//   @param all_directions - Optional rule name applied to ALL SIX directions before `rules` is processed, so you can say "Stop everywhere, except Down" in one call. Explicit is refused here: it would need six different targets.
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true).
//   @returns { widget_bp_path, widget_name, applied: [{direction, rule, widget_to_focus}], current_navigation: {Up:{rule,widget_to_focus}, ...}, navigation_discarded: bool, saved, save_error } -- navigation_discarded:true means every direction ended on Escape so the subobject was dropped, exactly as the Designer does; current_navigation is then all-Escape.

TSharedPtr<FJsonObject> HandleSetWidgetNavigation(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, WidgetName;
    UWidgetBlueprint* WidgetBP = nullptr;
    UWidget* Widget = nullptr;
    if (TSharedPtr<FJsonObject> Failure = ReadWidgetTarget_Widget(Params, WidgetBPPath, WidgetName, WidgetBP, Widget))
    {
        return Failure;
    }

    // What is going to be written, resolved BEFORE touching anything: a misspelled direction or
    // rule name must be a clean error, not half the navigation applied.
    TArray<FNavWrite_Widget> Writes;

    FString AllRuleStr;
    if (Params->TryGetStringField(TEXT("all_directions"), AllRuleStr) && !AllRuleStr.IsEmpty())
    {
        EUINavigationRule AllRule;
        if (!ParseNavRule_Widget(AllRuleStr, AllRule))
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("all_directions: unknown rule '%s'. Expected Escape|Stop|Wrap|Explicit."), *AllRuleStr));
        if (AllRule == EUINavigationRule::Explicit)
            return mcp_common::MakeErrorReply(
                TEXT("all_directions cannot be Explicit: that rule needs a widget_to_focus per direction. "
                     "Use the rules map instead."));

        for (EUINavigation D : AllNavDirections_Widget)
        {
            Writes.Add({ D, AllRule, NAME_None });
        }
    }

    const TSharedPtr<FJsonObject>* RulesObj = nullptr;
    if (Params->TryGetObjectField(TEXT("rules"), RulesObj) && RulesObj && RulesObj->IsValid())
    {
        for (const auto& Pair : (*RulesObj)->Values)
        {
            const FString DirName(*Pair.Key);
            EUINavigation Dir;
            if (!ParseNavDirection_Widget(DirName, Dir))
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("rules: '%s' is not a direction. Expected Up|Down|Left|Right|Next|Previous."), *DirName));

            FString RuleStr;
            FName FocusTarget = NAME_None;
            if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Object)
            {
                const TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
                Entry->TryGetStringField(TEXT("rule"), RuleStr);
                FString FocusStr;
                if (Entry->TryGetStringField(TEXT("widget_to_focus"), FocusStr) && !FocusStr.IsEmpty())
                {
                    FocusTarget = FName(*FocusStr);
                }
            }
            else if (Pair.Value.IsValid())
            {
                RuleStr = Pair.Value->AsString();
            }

            EUINavigationRule Rule;
            if (RuleStr.IsEmpty() || !ParseNavRule_Widget(RuleStr, Rule))
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("rules['%s']: unknown rule '%s'. Expected Escape|Stop|Wrap|Explicit."), *DirName, *RuleStr));

            if (Rule == EUINavigationRule::Explicit)
            {
                if (FocusTarget.IsNone())
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("rules['%s'] is Explicit but has no widget_to_focus. Pass "
                             "{\"rule\":\"Explicit\",\"widget_to_focus\":\"<widget name>\"}."), *DirName));
                // The target has to exist NOW: an explicit navigation to a widget that is not there
                // is a dead end that does not fail to compile and only shows up while playing.
                if (!WidgetBP->WidgetTree->FindWidget(FocusTarget))
                    return mcp_common::MakeErrorReply(FString::Printf(
                        TEXT("rules['%s'].widget_to_focus '%s' is not a widget of this tree."),
                        *DirName, *FocusTarget.ToString()));
            }

            Writes.Add({ Dir, Rule, FocusTarget });
        }
    }

    if (Writes.Num() == 0)
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to author: pass rules {direction: rule} and/or all_directions."));

    Widget->Modify();

    // Subobject creation, copied from the editor's customization.
    UWidgetNavigation* Nav = Widget->Navigation;
    if (!Nav)
    {
        Nav = NewObject<UWidgetNavigation>(Widget);
        Nav->SetFlags(RF_Transactional);
        if (Widget->IsTemplate())
        {
            const EObjectFlags TemplateFlags =
                Widget->GetMaskedFlags(RF_PropagateToSubObjects) | RF_DefaultSubObject;
            Nav->SetFlags(TemplateFlags);
        }
    }

    TArray<TSharedPtr<FJsonValue>> AppliedArr;
    for (const FNavWrite_Widget& W : Writes)
    {
        FWidgetNavigationData& Data = Nav->GetNavigationData(W.Direction);
        Data.Rule = W.Rule;
        Data.WidgetToFocus = W.Focus;

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("direction"), NavDirectionToString_Widget(W.Direction));
        Entry->SetStringField(TEXT("rule"), NavRuleToString_Widget(W.Rule));
        Entry->SetStringField(TEXT("widget_to_focus"), W.Focus.ToString());
        AppliedArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    // WARNING: what the Designer does: if everything ended up at the default, the subobject is dropped.
    const bool bDiscarded = Nav->IsDefaultNavigation();
    Widget->Navigation = bDiscarded ? nullptr : Nav;

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave, TEXT("save=false: authored IN MEMORY only."));

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("widget_name"), WidgetName);
    Resp->SetArrayField(TEXT("applied"), AppliedArr);
    Resp->SetBoolField(TEXT("navigation_discarded"), bDiscarded);
    Resp->SetObjectField(TEXT("current_navigation"), WidgetNavigationToJson_Widget(Widget));
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// LIVE FOCUS -- the other half of being able to test navigation.
//
// inject_key_input with route="slate" MOVES focus; these two READ it and SET it. Without them,
// gamepad navigation can be authored and pushed, but not checked: "press down three times and
// confirm" is not an assertion, it is a screenshot checked by eye.
//
// The path from the focused SWidget to the UMG UWidget is the engine's own
// (UMG/Private/Extensions/UIComponents/NavigationUIComponent.cpp); its two helpers are
// internal and not exported, so they are replicated. Walking up the parents is essential: the
// SWidget holding focus is usually the internal SButton, not the one that maps 1:1 to the UWidget.
// ---------------------------------------------------------------------------
// MCP:get_pie_focused_widget - Report which UMG widget currently has FOCUS in the running PIE session -- the assertion that makes gamepad navigation testable instead of eyeballed. Resolves FSlateApplication's focused SWidget and maps it back to the UMG UWidget that created it, by walking up to the owning SObjectWidget and matching cached widgets: the same route the engine's own navigation component uses, and it has to walk up because the widget holding focus is usually an internal SButton, not the one that maps 1:1 to a UWidget. Pair it with inject_key_input route="slate", the only input path that moves focus at all: press Down, read this, assert. Returns has_focus:false as an honest success (not an error) when nothing is focused -- which is itself the finding when a menu opens with no focus and every navigation key then appears to do nothing.
//   @param user_index - Optional Slate user index (default 0). Local co-op players are separate Slate users, so player 2 is user 1.
//   @returns { has_focus: bool, slate_widget_type: string, widget_name: string, widget_class: string, owning_user_widget: string, owning_user_widget_class: string, resolved_to_umg: bool, note? } -- resolved_to_umg:false together with has_focus:true means something in Slate holds focus but it is not a UMG widget (the viewport, a native Slate panel); slate_widget_type still names it.
TSharedPtr<FJsonObject> HandleGetPieFocusedWidget(const TSharedPtr<FJsonObject>& Params)
{
    if (!FSlateApplication::IsInitialized())
    {
        return mcp_common::MakeErrorReply(TEXT("Slate is not initialized in this process."));
    }

    double UserIndexNum = 0.0;
    Params->TryGetNumberField(TEXT("user_index"), UserIndexNum);
    const uint32 UserIndex = static_cast<uint32>(FMath::Max(0, FMath::RoundToInt(UserIndexNum)));

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetNumberField(TEXT("user_index"), UserIndex);

    TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(UserIndex);
    if (!Focused.IsValid())
    {
        Resp->SetBoolField(TEXT("has_focus"), false);
        Resp->SetBoolField(TEXT("resolved_to_umg"), false);
        Resp->SetStringField(TEXT("note"),
            TEXT("nothing holds Slate focus for this user. With a menu open that normally means no "
                 "widget was ever focused, and navigation keys have nothing to move FROM: set one "
                 "with set_pie_widget_focus, or give the screen an initial focus target."));
        return mcp_common::MakeOkReply(Resp);
    }

    Resp->SetBoolField(TEXT("has_focus"), true);
    Resp->SetStringField(TEXT("slate_widget_type"), Focused->GetTypeAsString());

    // Walk up to the SObjectWidget that wraps the owning UUserWidget.
    UUserWidget* OwningUserWidget = nullptr;
    {
        TSharedPtr<SWidget> Cursor = Focused;
        while (Cursor.IsValid())
        {
            if (Cursor->GetWidgetClass().GetWidgetType() == SObjectWidget::StaticWidgetClass().GetWidgetType())
            {
                OwningUserWidget = StaticCastSharedPtr<SObjectWidget>(Cursor)->GetWidgetObject();
                break;
            }
            Cursor = Cursor->GetParentWidget();
        }
    }

    if (!OwningUserWidget)
    {
        Resp->SetBoolField(TEXT("resolved_to_umg"), false);
        Resp->SetStringField(TEXT("note"),
            TEXT("the focused Slate widget is not inside any UMG UserWidget: the viewport or a native "
                 "Slate panel holds focus."));
        return mcp_common::MakeOkReply(Resp);
    }

    Resp->SetStringField(TEXT("owning_user_widget"), OwningUserWidget->GetName());
    Resp->SetStringField(TEXT("owning_user_widget_class"), OwningUserWidget->GetClass()->GetName());

    // And from the UUserWidget to the concrete UWidget: compare the cached SWidget walking up
    // parents, and keep the FIRST match, which is the closest to the one that really has focus.
    UWidget* FoundUMG = nullptr;
    if (OwningUserWidget->WidgetTree)
    {
        TSharedPtr<SWidget> Cursor = Focused;
        while (Cursor.IsValid() && !FoundUMG)
        {
            OwningUserWidget->WidgetTree->ForEachWidget([&FoundUMG, &Cursor](UWidget* Candidate)
            {
                if (!FoundUMG && Candidate && Candidate->GetCachedWidget() == Cursor)
                {
                    FoundUMG = Candidate;
                }
            });
            Cursor = Cursor->GetParentWidget();
        }
    }

    Resp->SetBoolField(TEXT("resolved_to_umg"), FoundUMG != nullptr);
    Resp->SetStringField(TEXT("widget_name"), FoundUMG ? FoundUMG->GetName() : FString());
    Resp->SetStringField(TEXT("widget_class"), FoundUMG ? FoundUMG->GetClass()->GetName() : FString());
    if (!FoundUMG)
    {
        Resp->SetStringField(TEXT("note"),
            TEXT("the owning UserWidget was found but no widget of its tree owns the focused SWidget: "
                 "focus sits on an internal Slate widget the UMG tree does not expose."));
    }
    return mcp_common::MakeOkReply(Resp);
}

// MCP:set_pie_widget_focus - Give keyboard/gamepad FOCUS to a live UMG widget in the running PIE session, so navigation has somewhere to start from. Menus routinely open with nothing focused, and then every navigation key does nothing -- which reads exactly like broken navigation and sends you to debug rules that were fine. Resolves the widget the same way click_pie_widget does (widget_name, optionally drilling in with child_name, nested Widget Blueprints included), then calls UWidget::SetKeyboardFocus. Verify with get_pie_focused_widget and drive from there with inject_key_input route="slate".
//   @param widget_name [string!] - Name of a live UserWidget added to the viewport, or of a widget inside one.
//   @param child_name - Optional: after resolving widget_name, drill into this descendant by name (nested Widget Blueprints included).
//   @param player_index - Optional local player index whose viewport widgets are searched (default 0).
//   @param world - Optional world selector, same rules as read_pie_widgets: absent/'pie' | 'server' | 'client' | 'pie:N'.
//   @returns { focused: bool, widget_name: string, widget_class: string, has_keyboard_focus: bool } -- has_keyboard_focus is RE-READ off the widget after the call, so it is evidence and not an echo: false means Slate refused the focus, and the usual cause is that the widget is not focusable (check IsFocusable and its Visibility -- a HitTestInvisible widget cannot take focus).
TSharedPtr<FJsonObject> HandleSetPieWidgetFocus(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_name"), WidgetName)) { return Missing; }

    FString ChildName;
    Params->TryGetStringField(TEXT("child_name"), ChildName);

    int32 PlayerIndex = 0;
    Params->TryGetNumberField(TEXT("player_index"), PlayerIndex);

    FString WorldError;
    UWorld* PieWorld = RequirePieWorld_Widget(Params, TEXT("set_pie_widget_focus"), WorldError);
    if (!PieWorld)
    {
        return mcp_common::MakeErrorReply(WorldError);
    }

    FString ResolveErr;
    UWidget* Target = ResolveLivePieWidget(PieWorld, FName(*WidgetName), PlayerIndex, ChildName, ResolveErr);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("set_pie_widget_focus: %s"), *ResolveErr));
    }

    Target->SetKeyboardFocus();

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetBoolField(TEXT("focused"), true);
    Resp->SetStringField(TEXT("widget_name"), Target->GetName());
    Resp->SetStringField(TEXT("widget_class"), Target->GetClass()->GetName());
    Resp->SetBoolField(TEXT("has_keyboard_focus"), Target->HasKeyboardFocus());
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// NAMED SLOTS OF A NESTED INSTANCE
//
// Why it is needed. The reparent_widget doc says no SetContentForSlot is needed. That is TRUE
// for a UNamedSlot of the widget's own tree -- it is a UContentWidget and reparent fills it --,
// and FALSE for the slot of a NESTED INSTANCE: there the slot is not a panel of your tree, it is
// an entry in the instance's NamedSlotBindings. The "template screen with holes" pattern falls
// exactly into the second case.
//
// WARNING: TWO THINGS THE ENGINE DOES SILENTLY, WATCHED HERE:
//  - UUserWidget::SetContentForSlot returns nothing, and if the slot does not exist it DROPS the
//    binding without warning (UserWidget.cpp). That is why it is re-read here with
//    GetContentForSlot and what gets reported is the RE-READ, not the call.
//  - The content has to live in the WidgetTree of the OUTER WBP (the editor creates it there:
//    SHierarchyViewItem.cpp). A widget from another tree would be accepted and left dangling.
// ---------------------------------------------------------------------------
// MCP:set_named_slot_content - Put a widget into a NAMED SLOT of a NESTED Widget Blueprint instance -- the "template screen with holes" pattern. reparent_widget already fills a UNamedSlot that belongs to your own tree (it is a UContentWidget, so it is just a panel), but the slot of an embedded instance is not a panel in your tree at all: it is an entry in that instance's NamedSlotBindings, and no other tool writes it. WARNING: the engine's SetContentForSlot returns nothing and SILENTLY DROPS the binding when the slot name does not exist, so this re-reads it with GetContentForSlot afterwards and reports the RE-READ, never the call. List what a given instance offers with available_slots in the response, or pass no slot_name at all to just enumerate them.
//   @param widget_bp_path [string!] - /Game path of the OUTER Widget Blueprint (the one that owns both the nested instance and the content).
//   @param host_widget_name [string!] - Name of the nested Widget Blueprint instance in the outer tree -- the widget that OWNS the slot.
//   @param slot_name - Name of the named slot to fill. Omit to only enumerate what the host offers (nothing is written, available_slots is still returned).
//   @param content_widget_name - Name of the widget in the OUTER tree to place in the slot. It must already exist there (add_widget_extended creates it). Omit together with clear=true to empty the slot.
//   @param clear [bool] - Empty the slot instead of filling it (default false). Mutually exclusive with content_widget_name.
//   @param save [bool] - Compile + save the Widget Blueprint when done (default true).
//   @returns { widget_bp_path, host_widget_name, host_class, available_slots: [string], slot_name, content_widget_name, filled: bool, verified_content: string, saved, save_error } -- verified_content is what GetContentForSlot returns AFTER the write: empty when the engine dropped the binding, which is the only way to tell that from a success.
TSharedPtr<FJsonObject> HandleSetNamedSlotContent(const TSharedPtr<FJsonObject>& Params)
{
    FString WidgetBPPath, HostName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("widget_bp_path"), WidgetBPPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("host_widget_name"), HostName)) { return Missing; }

    FString Error;
    UWidgetBlueprint* WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(WidgetBPPath, Error);
    if (!WidgetBP || !GetWidgetTree_Widget(WidgetBP, Error))
        return mcp_common::MakeErrorReply(Error);

    UWidget* HostWidget = WidgetBP->WidgetTree->FindWidget(FName(*HostName));
    if (!HostWidget)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Widget '%s' not found in the widget tree"), *HostName));

    UUserWidget* Host = Cast<UUserWidget>(HostWidget);
    if (!Host)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' is a %s, not a nested Widget Blueprint instance: only a UserWidget has named "
                 "slots to fill. For a UNamedSlot of THIS tree use reparent_widget instead."),
            *HostName, *HostWidget->GetClass()->GetName()));

    // What this instance really offers. It is always returned, also on failure: it is the
    // information that turns a "that slot does not exist" into something actionable.
    TArray<FName> SlotNames;
    Host->GetSlotNames(SlotNames);
    TArray<TSharedPtr<FJsonValue>> SlotsJson;
    for (const FName& N : SlotNames) { SlotsJson.Add(MakeShared<FJsonValueString>(N.ToString())); }

    TSharedPtr<FJsonObject> Resp = StartReply_Widget(WidgetBPPath, TEXT("host_widget_name"), HostName);
    Resp->SetStringField(TEXT("host_class"), Host->GetClass()->GetName());
    Resp->SetArrayField(TEXT("available_slots"), SlotsJson);

    FString SlotName;
    if (!Params->TryGetStringField(TEXT("slot_name"), SlotName) || SlotName.IsEmpty())
    {
        // Enumeration mode: nothing is written, and the response says so.
        Resp->SetBoolField(TEXT("filled"), false);
        Resp->SetStringField(TEXT("note"),
            TEXT("no slot_name given: nothing was written, this is the list of slots the host offers."));
        return mcp_common::MakeOkReply(Resp);
    }

    const FName SlotFName(*SlotName);
    if (!SlotNames.Contains(SlotFName))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' offers no named slot called '%s'. It offers: %s. (A slot only shows up here if "
                 "the nested Widget Blueprint left it EMPTY and did not mark it expose-on-instance-only.)"),
            *HostName, *SlotName,
            SlotNames.Num() ? *FString::JoinBy(SlotNames, TEXT(", "), [](const FName& N){ return N.ToString(); })
                            : TEXT("(none)")));

    bool bClear = false;
    Params->TryGetBoolField(TEXT("clear"), bClear);

    FString ContentName;
    Params->TryGetStringField(TEXT("content_widget_name"), ContentName);

    UWidget* Content = nullptr;
    if (!bClear)
    {
        if (ContentName.IsEmpty())
            return mcp_common::MakeErrorReply(
                TEXT("Pass content_widget_name (the widget to place), or clear=true to empty the slot."));

        Content = WidgetBP->WidgetTree->FindWidget(FName(*ContentName));
        if (!Content)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("content widget '%s' is not in this Widget Blueprint's tree. The content of a named "
                     "slot must live in the OUTER tree -- create it first with add_widget_extended."),
                *ContentName));
    }

    // The editor touches all three: the outer WidgetTree, the host and (via Modify) the transaction.
    WidgetBP->WidgetTree->SetFlags(RF_Transactional);
    WidgetBP->WidgetTree->Modify();
    Host->SetFlags(RF_Transactional);
    Host->Modify();
    if (Content)
    {
        Content->Modify();
        // A widget cannot be in a panel and in a slot at the same time.
        Content->RemoveFromParent();
    }

    Host->SetContentForSlot(SlotFName, Content);

    // THE RE-READ. It is the only oracle: SetContentForSlot returns nothing.
    UWidget* Verified = Host->GetContentForSlot(SlotFName);
    const bool bFilled = bClear ? (Verified == nullptr) : (Verified == Content && Content != nullptr);

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    bool bSave = true;
    Params->TryGetBoolField(TEXT("save"), bSave);
    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave, TEXT("save=false: written IN MEMORY only."));

    Resp->SetStringField(TEXT("slot_name"), SlotName);
    Resp->SetStringField(TEXT("content_widget_name"), ContentName);
    Resp->SetBoolField(TEXT("filled"), bFilled);
    Resp->SetStringField(TEXT("verified_content"), Verified ? Verified->GetName() : FString());
    if (!bFilled)
    {
        Resp->SetStringField(TEXT("note"),
            TEXT("the engine did not keep the binding: SetContentForSlot drops it silently when the "
                 "slot is not really available on this instance."));
    }
    mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// scaffold_menu_screen
// ---------------------------------------------------------------------------
namespace
{
    /** Turns "Load Game" into "LoadGame": a valid widget and function name. */
    FString SlugifyMenuEntry_Scaffold(const FString& In)
    {
        FString Out;
        bool bUpperNext = true;
        for (const TCHAR C : In)
        {
            if (FChar::IsAlnum(C))
            {
                Out.AppendChar(bUpperNext ? FChar::ToUpper(C) : C);
                bUpperNext = false;
            }
            else
            {
                bUpperNext = true;   // separator: dropped, and the next character is capitalized
            }
        }
        // A name cannot start with a digit.
        if (Out.Len() > 0 && FChar::IsDigit(Out[0])) { Out.InsertAt(0, TEXT('N')); }
        return Out;
    }

    /** FLAT colour brush (no ResourceObject) with rounded corners.
     *  Flat on purpose: it is the only kind audit_widget_accessibility can judge. A brush with a
     *  texture turns the colour into a TINT and the contrast becomes impossible to compute. */
    FSlateBrush MakeFlatBrush_Scaffold(const FLinearColor& C, float CornerRadius)
    {
        FSlateBrush B;
        B.DrawAs = ESlateBrushDrawType::RoundedBox;
        B.TintColor = FSlateColor(C);
        B.OutlineSettings = FSlateBrushOutlineSettings(CornerRadius);
        return B;
    }

    /** Reads a colour from the params: "#RRGGBBAA", [r,g,b,a] or {R,G,B,A}. Leaves Out untouched if missing. */
    bool ReadColorParam_Scaffold(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FLinearColor& Out)
    {
        const TSharedPtr<FJsonValue> V = Params->TryGetField(Key);
        if (!V.IsValid()) { return false; }

        FString AsHex;
        if (V->TryGetString(AsHex) && !AsHex.IsEmpty())
        {
            Out = FLinearColor(FColor::FromHex(AsHex));
            return true;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (V->TryGetArray(Arr) && Arr && Arr->Num() >= 3)
        {
            Out = FLinearColor(
                (float)(*Arr)[0]->AsNumber(), (float)(*Arr)[1]->AsNumber(), (float)(*Arr)[2]->AsNumber(),
                Arr->Num() >= 4 ? (float)(*Arr)[3]->AsNumber() : 1.0f);
            return true;
        }
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (V->TryGetObject(Obj) && Obj && Obj->IsValid())
        {
            Out = FLinearColor(
                (float)(*Obj)->GetNumberField(TEXT("R")), (float)(*Obj)->GetNumberField(TEXT("G")),
                (float)(*Obj)->GetNumberField(TEXT("B")),
                (*Obj)->HasField(TEXT("A")) ? (float)(*Obj)->GetNumberField(TEXT("A")) : 1.0f);
            return true;
        }
        return false;
    }
}

// MCP:scaffold_menu_screen - Build a COMPLETE, styled, contrast-safe menu screen in ONE call: the Widget Blueprint, a background panel, a centred title and N labelled buttons, each with its OnClicked already wired to a handler in the event graph. This exists because authoring a menu widget-by-widget costs ~40 MCP calls (add the widget, parent it, set its slot, style it, bind it -- times every button), and that cost, not any missing capability, is why a project ends up with a default-grey UMG screen nobody wants to touch. THE DEFAULTS PASS OUR OWN AUDITOR: the button/text colours are chosen so audit_widget_accessibility reports no contrast issue, and the achieved ratio is returned in the payload as contrast_ratio_normal_text so the claim is checkable rather than promised. Everything is a plain solid brush with no ResourceObject, which is exactly what makes that ratio computable at all. Idempotent at every level: an existing asset is reused, an existing widget name is reused, and an existing OnClicked binding is not duplicated -- so re-running to add one more entry is safe. WARNING: layout is authored, NOT measured -- a label longer than the panel is wide will still clip, and nothing here detects that (render_widget_preview is the cheap oracle). WARNING: text is written inline, so it gets an auto-generated localisation key, not a String Table identity.
//   @param package_path - /Game folder for the new Widget Blueprint (e.g. /Game/MyGame/UI).
//   @param name - Asset name, e.g. WBP_OptionsMenu.
//   @param entries [array] - REQUIRED, the buttons top to bottom. Each item is either a plain string (the label) or an object {label, handler_name?}. handler_name names the generated event-graph function; omitted, it becomes On<Label>Clicked. Order is preserved.
//   @param title - Optional heading text above the buttons. Omitted or empty = no title widget at all (not an empty one).
//   @param parent_class - Parent UUserWidget subclass (default "UUserWidget"). Pass a CommonUI activatable class here to scaffold a screen for a CommonUI stack.
//   @param background [bool] - Draw the background panel (default true). false leaves the buttons floating on nothing -- which is what makes contrast unjudgeable, so it also drops contrast_ratio_normal_text from the payload.
//   @param panel_color - Background panel colour. "#RRGGBBAA", [r,g,b,a] (LINEAR 0..1) or {R,G,B,A}. Default is a near-black panel.
//   @param button_color - Button resting colour, same formats. If you override this, you own the contrast: the returned ratio tells you what you got.
//   @param text_color - Label colour for title and buttons, same formats. Default white.
//   @param title_font_size [number] - Default 36.
//   @param button_font_size [number] - Default 20. Anything under 12 is what audit_widget_accessibility flags as small_text.
//   @param panel_width [number] - Panel width in pixels (default 480).
//   @param panel_height [number] - Panel height in pixels (default 560).
//   @param bind_events [bool] - Wire each button's OnClicked to a handler in the event graph (default true). false builds the visuals only.
//   @param theme_source_bp - Optional /Game path of a Widget Blueprint holding a reference Button whose style is copied onto every generated button INSTEAD of the built-in defaults (same idea as apply_widget_style_preset's theme mode: one widget is the single source of truth).
//   @param theme_source_widget - Name of the reference Button inside theme_source_bp. Required with it.
//   @param save [bool] - Compile + save when done (default true).
//   @returns { widget_bp_path, asset_path, created, already_existed, root_widget, panel_widget, box_widget, title_widget, entries: [{label, button_widget, text_widget, handler_name, bound, already_bound, error}], entry_count, buttons_bound, colors: {panel, button, text}, contrast_ratio_normal_text, contrast_required, contrast_passes, contrast_note, theme_source, compiled, saved, save_error }

TSharedPtr<FJsonObject> HandleScaffoldMenuScreen(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("entries"), EntriesArr) || !EntriesArr || EntriesArr->Num() == 0)
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: entries (a non-empty array of labels, or of {label, handler_name} objects)"));

    // Normalize entries BEFORE touching anything, so a bad entry does not leave half a menu written.
    struct FMenuEntry { FString Label; FString Handler; FString Slug; };
    TArray<FMenuEntry> Entries;
    TSet<FString> SeenSlugs;
    for (int32 i = 0; i < EntriesArr->Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& E = (*EntriesArr)[i];
        FMenuEntry Entry;
        const TSharedPtr<FJsonObject>* AsObj = nullptr;
        if (E->TryGetObject(AsObj) && AsObj && AsObj->IsValid())
        {
            (*AsObj)->TryGetStringField(TEXT("label"), Entry.Label);
            (*AsObj)->TryGetStringField(TEXT("handler_name"), Entry.Handler);
        }
        else
        {
            E->TryGetString(Entry.Label);
        }
        if (Entry.Label.IsEmpty())
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("entries[%d] has no label (give a string, or an object with a non-empty label field)"), i));

        Entry.Slug = SlugifyMenuEntry_Scaffold(Entry.Label);
        if (Entry.Slug.IsEmpty())
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("entries[%d] label '%s' has no alphanumeric character, so it cannot name a widget"), i, *Entry.Label));
        if (SeenSlugs.Contains(Entry.Slug))
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("entries[%d] label '%s' collides with an earlier entry (both reduce to '%s'): widget names must be unique"),
                i, *Entry.Label, *Entry.Slug));
        SeenSlugs.Add(Entry.Slug);

        if (Entry.Handler.IsEmpty()) { Entry.Handler = FString::Printf(TEXT("On%sClicked"), *Entry.Slug); }
        Entries.Add(Entry);
    }

    FString Title;
    Params->TryGetStringField(TEXT("title"), Title);
    FString ParentClassName = TEXT("UUserWidget");
    Params->TryGetStringField(TEXT("parent_class"), ParentClassName);

    bool bBackground = true;       Params->TryGetBoolField(TEXT("background"), bBackground);
    bool bBindEvents = true;       Params->TryGetBoolField(TEXT("bind_events"), bBindEvents);
    bool bSave = true;             Params->TryGetBoolField(TEXT("save"), bSave);

    double TitleFontSize = 36.0;   Params->TryGetNumberField(TEXT("title_font_size"), TitleFontSize);
    double ButtonFontSize = 20.0;  Params->TryGetNumberField(TEXT("button_font_size"), ButtonFontSize);
    double PanelWidth = 480.0;     Params->TryGetNumberField(TEXT("panel_width"), PanelWidth);
    double PanelHeight = 560.0;    Params->TryGetNumberField(TEXT("panel_height"), PanelHeight);

    // Defaults chosen to PASS audit_widget_accessibility, not for aesthetic taste.
    // White (L=1.0) on the button (L~0.109) gives (1.0+0.05)/(0.109+0.05) = 6.6:1, well above
    // WCAG AA's 4.5 for normal text. UMG's default grey (0.4955) gets 1.92:1.
    FLinearColor PanelColor(0.035f, 0.037f, 0.045f, 0.96f);
    FLinearColor ButtonColor(0.10f, 0.11f, 0.13f, 1.0f);
    FLinearColor TextColor(1.0f, 1.0f, 1.0f, 1.0f);
    ReadColorParam_Scaffold(Params, TEXT("panel_color"), PanelColor);
    ReadColorParam_Scaffold(Params, TEXT("button_color"), ButtonColor);
    ReadColorParam_Scaffold(Params, TEXT("text_color"), TextColor);

    // Optional theme: copy the style of a reference button instead of using the defaults.
    FString ThemeBPPath, ThemeWidgetName, ThemeSourceDesc;
    const bool bHasTheme =
        Params->TryGetStringField(TEXT("theme_source_bp"), ThemeBPPath) && !ThemeBPPath.IsEmpty()
        && Params->TryGetStringField(TEXT("theme_source_widget"), ThemeWidgetName) && !ThemeWidgetName.IsEmpty();
    FButtonStyle ThemeButtonStyle;
    bool bThemeStyleResolved = false;
    if (bHasTheme)
    {
        FString ThemeErr;
        UWidgetBlueprint* ThemeBP = UMGWidgetUtils::LoadWidgetBlueprint(ThemeBPPath, ThemeErr);
        if (!ThemeBP || !ThemeBP->WidgetTree)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("theme_source_bp '%s' is not a loadable Widget Blueprint: %s"), *ThemeBPPath, *ThemeErr));
        UWidget* ThemeWidget = ThemeBP->WidgetTree->FindWidget(FName(*ThemeWidgetName));
        if (!ThemeWidget)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("theme_source_widget '%s' not found in %s"), *ThemeWidgetName, *ThemeBPPath));
        UButton* ThemeButton = Cast<UButton>(ThemeWidget);
        if (!ThemeButton)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("theme_source_widget '%s' is a %s, not a Button: the style copied onto the generated buttons has to come from a Button"),
                *ThemeWidgetName, *ThemeWidget->GetClass()->GetName()));
        ThemeButtonStyle = ThemeButton->GetStyle();
        bThemeStyleResolved = true;
        ThemeSourceDesc = FString::Printf(TEXT("%s:%s"), *ThemeBPPath, *ThemeWidgetName);
    }

    // The asset: create or reuse (idempotent, same as create_widget_blueprint).
    UClass* ParentClass = ResolveUserWidgetParentClass(ParentClassName);
    if (!ParentClass)
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("parent_class '%s' could not be resolved to a UUserWidget subclass"), *ParentClassName));

    FString NormalizedPath = PackagePath;
    NormalizedPath.RemoveFromEnd(TEXT("/"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *NormalizedPath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    UWidgetBlueprint* WidgetBP = nullptr;
    bool bAlreadyExisted = false;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        FString LoadErr;
        WidgetBP = UMGWidgetUtils::LoadWidgetBlueprint(FullPath, LoadErr);
        if (!WidgetBP)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("asset exists at %s but is not a loadable Widget Blueprint: %s"), *FullPath, *LoadErr));
        bAlreadyExisted = true;
    }
    else
    {
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
        Factory->ParentClass = ParentClass;
        WidgetBP = Cast<UWidgetBlueprint>(
            AssetToolsModule.Get().CreateAsset(Name, NormalizedPath, UWidgetBlueprint::StaticClass(), Factory));
        if (!WidgetBP)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create Widget Blueprint at %s"), *FullPath));
    }

    UWidgetTree* Tree = WidgetBP->WidgetTree;
    if (!Tree) { return mcp_common::MakeErrorReply(TEXT("Widget Blueprint has no WidgetTree")); }

    // Reuse the existing widget if it is already there; otherwise build it and attach it to the parent.
    auto EnsureWidget = [&](UClass* Cls, const FString& WName, UPanelWidget* Parent) -> UWidget*
    {
        if (UWidget* Found = Tree->FindWidget(FName(*WName))) { return Found; }
        UWidget* W = Tree->ConstructWidget<UWidget>(Cls, FName(*WName));
        if (!W) { return nullptr; }
        if (Parent && !Parent->AddChild(W)) { return nullptr; }
        WidgetBP->OnVariableAdded(W->GetFName());
        return W;
    };

    // Root
    UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(Tree->RootWidget);
    if (!RootCanvas)
    {
        if (Tree->RootWidget)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("the existing root widget '%s' is a %s, not a CanvasPanel: this scaffold anchors its panel to a CanvasPanel root"),
                *Tree->RootWidget->GetName(), *Tree->RootWidget->GetClass()->GetName()));
        RootCanvas = Tree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
        if (!RootCanvas) { return mcp_common::MakeErrorReply(TEXT("Failed to construct root CanvasPanel")); }
        Tree->RootWidget = RootCanvas;
        WidgetBP->OnVariableAdded(RootCanvas->GetFName());
    }

    // Background panel (or the box directly, if background=false)
    UPanelWidget* BoxHost = RootCanvas;
    UBorder* MenuPanel = nullptr;
    if (bBackground)
    {
        MenuPanel = Cast<UBorder>(EnsureWidget(UBorder::StaticClass(), TEXT("MenuPanel"), RootCanvas));
        if (!MenuPanel) { return mcp_common::MakeErrorReply(TEXT("Failed to construct the background Border")); }
        MenuPanel->Background = MakeFlatBrush_Scaffold(PanelColor, 16.0f);
        MenuPanel->SetPadding(FMargin(32.0f, 28.0f));
        BoxHost = MenuPanel;
    }

    UVerticalBox* MenuBox = Cast<UVerticalBox>(EnsureWidget(UVerticalBox::StaticClass(), TEXT("MenuBox"), BoxHost));
    if (!MenuBox) { return mcp_common::MakeErrorReply(TEXT("Failed to construct the VerticalBox")); }

    // The panel (or the box, if there is no background) is anchored to the CENTER of the canvas and sized.
    UWidget* Anchored = bBackground ? static_cast<UWidget*>(MenuPanel) : static_cast<UWidget*>(MenuBox);
    if (UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Anchored->Slot))
    {
        CanvasSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
        CanvasSlot->SetAlignment(FVector2D(0.5f, 0.5f));   // pivot at the center: truly centred
        CanvasSlot->SetSize(FVector2D(PanelWidth, PanelHeight));
        CanvasSlot->SetPosition(FVector2D(0.0f, 0.0f));
    }
    if (UBorderSlot* BSlot = Cast<UBorderSlot>(MenuBox->Slot))
    {
        BSlot->SetHorizontalAlignment(HAlign_Fill);
        BSlot->SetVerticalAlignment(VAlign_Fill);
    }

    // Title (only if requested)
    UTextBlock* TitleWidget = nullptr;
    if (!Title.IsEmpty())
    {
        TitleWidget = Cast<UTextBlock>(EnsureWidget(UTextBlock::StaticClass(), TEXT("TitleText"), MenuBox));
        if (!TitleWidget) { return mcp_common::MakeErrorReply(TEXT("Failed to construct the title TextBlock")); }
        TitleWidget->SetText(FText::FromString(Title));
        TitleWidget->SetColorAndOpacity(FSlateColor(TextColor));
        FSlateFontInfo TitleFont = TitleWidget->GetFont();
        TitleFont.Size = (int32)TitleFontSize;
        TitleWidget->SetFont(TitleFont);
        TitleWidget->SetJustification(ETextJustify::Center);
        if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(TitleWidget->Slot))
        {
            VS->SetHorizontalAlignment(HAlign_Center);
            VS->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 24.0f));   // space between title and buttons
        }
    }

    // Buttons
    // Hover/pressed are derived by scaling ONLY the RGB. `Color * k` on an FLinearColor also
    // multiplies the ALPHA (1.0 * 1.6 = 1.6), which saturates when painting and leaves a style
    // that is not the one this tool claims to return.
    auto ScaleRGB = [](const FLinearColor& C, float K)
    {
        return FLinearColor(C.R * K, C.G * K, C.B * K, C.A);
    };

    FButtonStyle BuiltInStyle;
    BuiltInStyle.SetNormal  (MakeFlatBrush_Scaffold(ButtonColor, 6.0f));
    BuiltInStyle.SetHovered (MakeFlatBrush_Scaffold(ScaleRGB(ButtonColor, 1.6f), 6.0f));
    BuiltInStyle.SetPressed (MakeFlatBrush_Scaffold(ScaleRGB(ButtonColor, 0.7f), 6.0f));
    BuiltInStyle.SetDisabled(MakeFlatBrush_Scaffold(FLinearColor(0.08f, 0.08f, 0.08f, 0.6f), 6.0f));
    BuiltInStyle.SetNormalPadding (FMargin(16.0f, 12.0f));
    BuiltInStyle.SetPressedPadding(FMargin(16.0f, 12.0f));

    TArray<TSharedPtr<FJsonValue>> EntryReports;
    for (const FMenuEntry& Entry : Entries)
    {
        const FString BtnName = FString::Printf(TEXT("Btn_%s"), *Entry.Slug);
        const FString TxtName = FString::Printf(TEXT("Btn_%s_Text"), *Entry.Slug);

        UButton* Btn = Cast<UButton>(EnsureWidget(UButton::StaticClass(), BtnName, MenuBox));
        if (!Btn)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to construct button '%s'"), *BtnName));
        Btn->SetStyle(bThemeStyleResolved ? ThemeButtonStyle : BuiltInStyle);
        if (UVerticalBoxSlot* VS = Cast<UVerticalBoxSlot>(Btn->Slot))
        {
            VS->SetHorizontalAlignment(HAlign_Fill);
            VS->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));   // spacing: without it the buttons touch
        }

        UTextBlock* Txt = Cast<UTextBlock>(EnsureWidget(UTextBlock::StaticClass(), TxtName, Btn));
        if (!Txt)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to construct label '%s'"), *TxtName));
        Txt->SetText(FText::FromString(Entry.Label));
        Txt->SetColorAndOpacity(FSlateColor(TextColor));
        FSlateFontInfo BtnFont = Txt->GetFont();
        BtnFont.Size = (int32)ButtonFontSize;
        Txt->SetFont(BtnFont);
        Txt->SetJustification(ETextJustify::Center);
        if (UButtonSlot* BtnSlot = Cast<UButtonSlot>(Txt->Slot))
        {
            BtnSlot->SetHorizontalAlignment(HAlign_Center);
            BtnSlot->SetVerticalAlignment(VAlign_Center);
        }
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);

    // COMPILING HERE IS NOT OPTIONAL: the bound event resolves the widget as an FObjectProperty of
    // the generated class, so a freshly created widget does NOT EXIST for the binder until compiled.
    FKismetEditorUtilities::CompileBlueprint(WidgetBP, EBlueprintCompileOptions::None);

    // OnClicked of each button
    int32 BoundCount = 0;
    for (int32 i = 0; i < Entries.Num(); ++i)
    {
        const FMenuEntry& Entry = Entries[i];
        TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
        R->SetStringField(TEXT("label"), Entry.Label);
        R->SetStringField(TEXT("button_widget"), FString::Printf(TEXT("Btn_%s"), *Entry.Slug));
        R->SetStringField(TEXT("text_widget"), FString::Printf(TEXT("Btn_%s_Text"), *Entry.Slug));

        if (!bBindEvents)
        {
            R->SetBoolField(TEXT("bound"), false);
            R->SetStringField(TEXT("note"), TEXT("bind_events=false: visuals only, no OnClicked handler"));
            EntryReports.Add(MakeShared<FJsonValueObject>(R));
            continue;
        }

        R->SetStringField(TEXT("handler_name"), Entry.Handler);

        UClass* SearchClass = WidgetBP->SkeletonGeneratedClass ? WidgetBP->SkeletonGeneratedClass : WidgetBP->GeneratedClass;
        const FName BtnFName(*FString::Printf(TEXT("Btn_%s"), *Entry.Slug));
        FObjectProperty* BtnProp = SearchClass ? FindFProperty<FObjectProperty>(SearchClass, BtnFName) : nullptr;
        if (!BtnProp)
        {
            R->SetBoolField(TEXT("bound"), false);
            R->SetStringField(TEXT("error"),
                TEXT("the button did not become a property of the generated class after compiling, so OnClicked could not be bound"));
            EntryReports.Add(MakeShared<FJsonValueObject>(R));
            continue;
        }

        FMulticastDelegateProperty* OnClicked =
            FindFProperty<FMulticastDelegateProperty>(UButton::StaticClass(), TEXT("OnClicked"));
        if (!OnClicked)
        {
            R->SetBoolField(TEXT("bound"), false);
            R->SetStringField(TEXT("error"), TEXT("UButton::OnClicked not found by reflection"));
            EntryReports.Add(MakeShared<FJsonValueObject>(R));
            continue;
        }

        // Idempotent: if it was already bound it is not duplicated, so re-running to add one more
        // entry cannot break the ones that were already there.
        if (const UK2Node_ComponentBoundEvent* Existing =
                FKismetEditorUtilities::FindBoundEventForComponent(WidgetBP, TEXT("OnClicked"), BtnFName))
        {
            R->SetBoolField(TEXT("bound"), true);
            R->SetBoolField(TEXT("already_bound"), true);
            R->SetStringField(TEXT("handler_name"), Existing->CustomFunctionName.ToString());
            ++BoundCount;
            EntryReports.Add(MakeShared<FJsonValueObject>(R));
            continue;
        }

        UEdGraph* Graph = FUEAgentGraphUtils::FindGraphByNameDeep(WidgetBP, FString());
        if (!Graph)
        {
            R->SetBoolField(TEXT("bound"), false);
            R->SetStringField(TEXT("error"), TEXT("no event graph found on the Widget Blueprint"));
            EntryReports.Add(MakeShared<FJsonValueObject>(R));
            continue;
        }

        FGraphNodeCreator<UK2Node_ComponentBoundEvent> Creator(*Graph);
        UK2Node_ComponentBoundEvent* Node = Creator.CreateNode();
        Node->NodePosX = 0;
        Node->NodePosY = i * 220;   // in a column, so the graph reads without rearranging anything
        Node->InitializeComponentBoundEventParams(BtnProp, OnClicked);
        // The name the engine generates is BndEvt__<BP>_<Widget>_K2Node_...__DelegateSignature:
        // unreadable, and with 5 buttons the graph is unmanageable. It is replaced by the requested/derived one.
        Node->CustomFunctionName = FName(*Entry.Handler);
        Creator.Finalize();
        Graph->NotifyGraphChanged();

        EntryReports.Add(MakeShared<FJsonValueObject>(R));
    }

    if (bBindEvents)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
        FKismetEditorUtilities::CompileBlueprint(WidgetBP, EBlueprintCompileOptions::None);

        // The verdict is RE-READ after compiling, never assumed from the node's creation.
        for (int32 i = 0; i < EntryReports.Num(); ++i)
        {
            TSharedPtr<FJsonObject> R = EntryReports[i]->AsObject();
            if (!R.IsValid()) { continue; }
            if (R->HasField(TEXT("already_bound")) || R->HasField(TEXT("error"))) { continue; }
            const FName BtnFName(*FString::Printf(TEXT("Btn_%s"), *Entries[i].Slug));
            const UK2Node_ComponentBoundEvent* Check =
                FKismetEditorUtilities::FindBoundEventForComponent(WidgetBP, TEXT("OnClicked"), BtnFName);
            R->SetBoolField(TEXT("bound"), Check != nullptr);
            if (Check)
            {
                R->SetStringField(TEXT("handler_name"), Check->CustomFunctionName.ToString());
                ++BoundCount;
            }
        }
    }

    FString SaveErr;
    const bool bSaved = SaveWidgetBlueprint(WidgetBP, SaveErr, bSave);

    // Response
    TSharedPtr<FJsonObject> Resp = StartReply_Widget(FullPath, TEXT("asset_path"), WidgetBP->GetPathName());
    Resp->SetBoolField(TEXT("created"), !bAlreadyExisted);
    Resp->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Resp->SetStringField(TEXT("root_widget"), RootCanvas->GetName());
    Resp->SetStringField(TEXT("panel_widget"), MenuPanel ? MenuPanel->GetName() : FString());
    Resp->SetStringField(TEXT("box_widget"), MenuBox->GetName());
    Resp->SetStringField(TEXT("title_widget"), TitleWidget ? TitleWidget->GetName() : FString());
    Resp->SetArrayField(TEXT("entries"), EntryReports);
    Resp->SetNumberField(TEXT("entry_count"), Entries.Num());
    Resp->SetNumberField(TEXT("buttons_bound"), BoundCount);

    TSharedPtr<FJsonObject> Colors = MakeShared<FJsonObject>();
    Colors->SetObjectField(TEXT("panel"), UMGWidgetUtils::ColorToJsonWithHex(PanelColor));
    Colors->SetObjectField(TEXT("button"), UMGWidgetUtils::ColorToJsonWithHex(ButtonColor));
    Colors->SetObjectField(TEXT("text"), UMGWidgetUtils::ColorToJsonWithHex(TextColor));
    Resp->SetObjectField(TEXT("colors"), Colors);

    // The REAL contrast that resulted, not a promise. If someone passes their own
    // button_color/text_color, this tells them whether they just broke their menu's accessibility.
    if (bThemeStyleResolved)
    {
        Resp->SetStringField(TEXT("contrast_note"),
            TEXT("a theme_source style was applied instead of the built-in colours, so the contrast of "
                 "the copied style is NOT computed here -- run audit_widget_accessibility on the result."));
    }
    else
    {
        if (!bBackground)
        {
            // Without a background there is no solid colour behind the text: there is no ratio to give,
            // just as audit_widget_accessibility reports background_source none instead of passing by default.
            Resp->SetStringField(TEXT("contrast_note"),
                TEXT("background=false: the buttons still carry their own solid style, but nothing was "
                     "measured behind the screen itself -- run audit_widget_accessibility on the result."));
        }
        const double Ratio = UMGWidgetUtils::WcagContrast(TextColor, ButtonColor);
        const double Required = (ButtonFontSize >= 18.0) ? 3.0 : 4.5;
        Resp->SetNumberField(TEXT("contrast_ratio_normal_text"), Ratio);
        Resp->SetNumberField(TEXT("contrast_required"), Required);
        Resp->SetBoolField(TEXT("contrast_passes"), Ratio >= Required);
        if (bBackground && Ratio < Required)
        {
            Resp->SetStringField(TEXT("contrast_note"), FString::Printf(
                TEXT("the label colour over the button colour is %.2f:1, below the %.1f:1 WCAG AA floor "
                     "at font size %.0f -- audit_widget_accessibility will report this screen as low_contrast."),
                Ratio, Required, ButtonFontSize));
        }
    }
    if (!ThemeSourceDesc.IsEmpty()) { Resp->SetStringField(TEXT("theme_source"), ThemeSourceDesc); }

    Resp->SetBoolField(TEXT("compiled"),
        WidgetBP->Status == BS_UpToDate || WidgetBP->Status == BS_UpToDateWithWarnings);
    Resp->SetBoolField(TEXT("saved"), bSaved);
    if (bSave && !bSaved) { Resp->SetStringField(TEXT("save_error"), SaveErr); }

    return mcp_common::MakeOutcomeReply(Resp, !bSave || bSaved,
        (!bSave || bSaved) ? FString() : FString(TEXT(
            "scaffold_menu_screen: the screen was built IN MEMORY but NOT saved to disk, so it is lost "
            "when the editor closes. See save_error for the measured cause (usually a read-only file, or one "
            "locked by source control).")));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_widget_animation"), &HandleAddWidgetAnimation },
    { TEXT("add_widget_animation_keyframe"), &HandleAddWidgetAnimationKeyframe },
    { TEXT("add_widget_animation_track"), &HandleAddWidgetAnimationTrack },
    { TEXT("add_widget_extended"), &HandleAddWidgetExtended },
    { TEXT("add_widget_property_binding"), &HandleAddWidgetPropertyBinding },
    { TEXT("add_widget_to_blueprint"), &HandleAddWidgetToBlueprint },
    { TEXT("apply_widget_style_preset"), &HandleApplyWidgetStylePreset },
    { TEXT("click_pie_widget"), &HandleClickPieWidget },
    { TEXT("create_widget_blueprint"), &HandleCreateWidgetBlueprint },
    { TEXT("duplicate_widget"), &HandleDuplicateWidget },
    { TEXT("get_pie_focused_widget"), &HandleGetPieFocusedWidget },
    { TEXT("get_widget_animations"), &HandleGetWidgetAnimations },
    { TEXT("get_widget_blueprint_tree"), &HandleGetWidgetBlueprintTree },
    { TEXT("list_widget_property_bindings"), &HandleListWidgetPropertyBindings },
    { TEXT("list_widget_types"), &HandleListWidgetTypes },
    { TEXT("play_widget_animation"), &HandlePlayWidgetAnimation },
    { TEXT("read_pie_widgets"), &HandleReadPieWidgets },
    { TEXT("remove_widget_animation"), &HandleRemoveWidgetAnimation },
    { TEXT("remove_widget_property_binding"), &HandleRemoveWidgetPropertyBinding },
    { TEXT("rename_widget"), &HandleRenameWidget },
    { TEXT("render_widget_preview"), &HandleRenderWidgetPreview },
    { TEXT("reorder_widget_child"), &HandleReorderWidgetChild },
    { TEXT("reparent_widget"), &HandleReparentWidget },
    { TEXT("scaffold_menu_screen"), &HandleScaffoldMenuScreen },
    { TEXT("set_box_slot_properties"), &HandleSetBoxSlotProperties },
    { TEXT("set_grid_slot_properties"), &HandleSetGridSlotProperties },
    { TEXT("set_named_slot_content"), &HandleSetNamedSlotContent },
    { TEXT("set_pie_widget_focus"), &HandleSetPieWidgetFocus },
    { TEXT("set_pie_widget_value"), &HandleSetPieWidgetValue },
    { TEXT("set_slot_properties"), &HandleSetSlotProperties },
    { TEXT("set_widget_navigation"), &HandleSetWidgetNavigation },
    { TEXT("set_widget_property"), &HandleSetWidgetProperty },
    { TEXT("set_widget_slot_properties"), &HandleSetWidgetSlotProperties },
    { TEXT("stop_widget_animation"), &HandleStopWidgetAnimation },
};
} // namespace UEAgentWidgetCommands
