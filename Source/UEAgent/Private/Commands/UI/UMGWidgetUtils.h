#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/SlateWrapperTypes.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"
#include "Engine/LocalPlayer.h"
#include "WidgetBlueprint.h"
#include "Animation/WidgetAnimation.h"
#include "Components/Widget.h"

// UMG helpers shared by the widget, MVVM, level-widget and audit command families.
namespace UMGWidgetUtils
{
    /**
     * Gives every widget and animation of WidgetBP that has no variable GUID one, through
     * UWidgetBlueprint::OnVariableAdded as the designer does when it adds a widget. The widget compiler fixes a
     * missing GUID up itself, but behind an ensure (FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids),
     * so a widget put in the tree without one (by a script, or by a tool before it registered its widgets) raised an
     * ensure on every compile. An empty map is left alone: the compiler fills it without an ensure.
     */
    inline void RegisterUntrackedWidgetVariables(UWidgetBlueprint* WidgetBP)
    {
        if (!WidgetBP || WidgetBP->WidgetVariableNameToGuidMap.IsEmpty())
        {
            return;
        }
        WidgetBP->ForEachSourceWidget([WidgetBP](UWidget* Widget)
        {
            if (Widget && !WidgetBP->WidgetVariableNameToGuidMap.Contains(Widget->GetFName()))
            {
                WidgetBP->OnVariableAdded(Widget->GetFName());
            }
        });
        for (UWidgetAnimation* Animation : WidgetBP->Animations)
        {
            if (Animation && !WidgetBP->WidgetVariableNameToGuidMap.Contains(Animation->GetFName()))
            {
                WidgetBP->OnVariableAdded(Animation->GetFName());
            }
        }
    }

    /** The Widget Blueprint asset at the /Game path WidgetBPPath, or null with OutError saying why. */
    inline UWidgetBlueprint* LoadWidgetBlueprint(const FString& WidgetBPPath, FString& OutError)
    {
        if (WidgetBPPath.IsEmpty())
        {
            OutError = TEXT("widget_bp_path cannot be empty");
            return nullptr;
        }
        if (!UEditorAssetLibrary::DoesAssetExist(WidgetBPPath))
        {
            OutError = FString::Printf(TEXT("Widget Blueprint not found at path: %s"), *WidgetBPPath);
            return nullptr;
        }
        UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(WidgetBPPath));
        if (!WidgetBP)
        {
            OutError = FString::Printf(TEXT("Asset at %s is not a Widget Blueprint"), *WidgetBPPath);
        }
        return WidgetBP;
    }

    /** The name of an ESlateVisibility value; anything unexpected reads as "Unknown". */
    inline const TCHAR* SlateVisibilityName(ESlateVisibility Visibility)
    {
        switch (Visibility)
        {
        case ESlateVisibility::Visible:              return TEXT("Visible");
        case ESlateVisibility::Collapsed:            return TEXT("Collapsed");
        case ESlateVisibility::Hidden:               return TEXT("Hidden");
        case ESlateVisibility::HitTestInvisible:     return TEXT("HitTestInvisible");
        case ESlateVisibility::SelfHitTestInvisible: return TEXT("SelfHitTestInvisible");
        default:                                     return TEXT("Unknown");
        }
    }

    /**
     * Whether UserWidget is live UI of PieWorld: a real instance (TObjectIterator also visits class-default and
     * archetype objects) that belongs to that world and is added to the viewport. With PlayerIndex >= 0, a widget
     * owned by another local player does not count.
     */
    inline bool IsLiveViewportWidget(const UUserWidget* UserWidget, const UWorld* PieWorld, int32 PlayerIndex = INDEX_NONE)
    {
        if (!IsValid(UserWidget)
            || UserWidget->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)
            || UserWidget->GetWorld() != PieWorld
            || !UserWidget->IsInViewport())
        {
            return false;
        }
        if (PlayerIndex >= 0)
        {
            if (const ULocalPlayer* OwningPlayer = UserWidget->GetOwningLocalPlayer())
            {
                const int32 OwnerIndex = OwningPlayer->GetLocalPlayerIndex();
                if (OwnerIndex != INDEX_NONE && OwnerIndex != PlayerIndex)
                {
                    return false;
                }
            }
        }
        return true;
    }

    /**
     * WCAG 2.x relative luminance: 0.2126R + 0.7152G + 0.0722B over LINEAR values, which is what an FLinearColor
     * holds. FLinearColor::GetLuminance is not this: it uses Rec.601 coefficients.
     */
    inline double WcagLuminance(const FLinearColor& Color)
    {
        return 0.2126 * Color.R + 0.7152 * Color.G + 0.0722 * Color.B;
    }

    /** WCAG 2.x contrast ratio of two colours: (Llighter + 0.05) / (Ldarker + 0.05). */
    inline double WcagContrast(const FLinearColor& A, const FLinearColor& B)
    {
        const double LuminanceA = WcagLuminance(A);
        const double LuminanceB = WcagLuminance(B);
        return (FMath::Max(LuminanceA, LuminanceB) + 0.05) / (FMath::Min(LuminanceA, LuminanceB) + 0.05);
    }

    /** A colour as {"R", "G", "B", "A", "hex"}: its linear components and its sRGB hex. */
    inline TSharedPtr<FJsonObject> ColorToJsonWithHex(const FLinearColor& Color)
    {
        TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetNumberField(TEXT("R"), Color.R);
        Out->SetNumberField(TEXT("G"), Color.G);
        Out->SetNumberField(TEXT("B"), Color.B);
        Out->SetNumberField(TEXT("A"), Color.A);
        Out->SetStringField(TEXT("hex"), Color.ToFColor(/*bSRGB=*/ true).ToHex());
        return Out;
    }
}
