// Colour parameters of the property handlers: the *_properties commands that
// Tools/codegen_property_handlers.py generates, and the hand-written post-process ones.
// Replies write colours back with mcp_common::ColorToJsonArray.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Math/Color.h"
#include "Math/Vector4.h"

// Parse a color param: [R, G, B] (W defaults) or [R, G, B, W].
inline bool ParseColorVec4(const TArray<TSharedPtr<FJsonValue>>* Arr, double DefaultW, FVector4& Out)
{
    if (!Arr || Arr->Num() < 3)
    {
        return false;
    }
    const double X = (*Arr)[0]->AsNumber();
    const double Y = (*Arr)[1]->AsNumber();
    const double Z = (*Arr)[2]->AsNumber();
    const double W = Arr->Num() >= 4 ? (*Arr)[3]->AsNumber() : DefaultW;
    Out = FVector4(X, Y, Z, W);
    return true;
}
