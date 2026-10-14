// HONEST JSON -> FProperty applier, via reflection.
//
// It exists because the engine path (FJsonObjectConverter, which mcp_common::SetObjectProperty
// relies on) iterates THE STRUCT'S PROPERTIES and looks each one up in the JSON: if the JSON
// carries names that do not match, it returns true having written ZERO fields. Here the
// direction is inverted -- the JSON KEYS are iterated -- so a key that matches no FProperty is
// a reported fact (Ignored), never a silence.
// The failure that motivates it is documented in the .cpp; see also
// Private/Commands/UI/UEAgentWidgetCommands.cpp.
//
// The Slate/UMG includes (SlateColor, SlateBrush, SlateFontInfo, Margin) live in the .cpp on
// purpose: this header is public and must not drag SlateCore into every TU that includes it.
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FProperty;
class UObject;
class UStruct;

namespace mcp_common
{
    /**
     * What actually got written. Dotted paths, absolute from the root of the call.
     *
     * The honesty contract: if `Applied` is empty, NOTHING was written, and no function in
     * this file returns true without having added at least one entry to `Applied`.
     * `Ignored` lists the JSON keys that matched no FProperty -- the engine converter's
     * silent no-op, made visible.
     */
    struct FJsonApplyReport
    {
        TArray<FString> Applied;                  // written paths
        TArray<FString> Ignored;                  // JSON keys with no matching FProperty
        TArray<TPair<FString, FString>> Errors;   // path -> reason
        bool HasFailures() const { return Ignored.Num() > 0 || Errors.Num() > 0; }
    };

    /**
     * Resolves "WidgetStyle.Normal.TintColor" (or "Items[2].Name") to the leaf FProperty + its pointer.
     *
     * At each segment: descends into the UScriptStruct of an FStructProperty, FOLLOWS the pointer
     * of an FObjectProperty (a null pointer is an error naming the segment that was null), and
     * enters the element of an FArrayProperty with an index (out of range is an error).
     * Name lookup is lenient: case-insensitive, and accepts the `b` variant of booleans
     * ("IsEnabled" finds `bIsEnabled`).
     *
     * @param OwnerStruct  UStruct/UClass that owns `Container` (e.g. Object->GetClass()).
     * @param Container    Pointer to the container (the UObject, or the parent struct).
     * @param OutError     The exact segment that failed and, when it helps, the names available there.
     * @return true only if OutLeafProp and OutLeafPtr are resolved.
     */
    UEAGENT_API bool ResolvePropertyPath(UStruct* OwnerStruct, void* Container, const FString& Path,
                                           FProperty*& OutLeafProp, void*& OutLeafPtr, FString& OutError);

    /**
     * Writes Value into the property pointed to by ValuePtr. PathPrefix only feeds the report.
     *
     * Dispatches on the FProperty type and, for the Slate/UMG structs the engine converter cannot
     * read in their natural form (FSlateColor as {R,G,B,A}, FMargin as a bare number,
     * FSlateBrush as an asset path...), applies convenience shortcuts AT ANY DEPTH.
     * Anything that fits no shortcut falls through to the generic recursion over the JSON keys.
     *
     * @param ValuePtr   Pointer TO THE VALUE of the property (not to the container).
     * @param PathPrefix Absolute path from the root of the call; when empty the property name
     *                   is used, so `Applied` never contains an empty entry.
     * @return true only if something was written; in that case Report.Applied grew.
     */
    UEAGENT_API bool ApplyJsonToProperty(FProperty* Property, void* ValuePtr,
                                           const TSharedPtr<FJsonValue>& Value,
                                           const FString& PathPrefix, FJsonApplyReport& Report);

    /**
     * Combines the two above: resolves the path on Object and applies.
     *
     * Does not mark the package dirty nor call PostEditChangeProperty: persistence and editor
     * notification belong to the caller, which knows whether this is part of a larger batch.
     *
     * @param OutError Reason when it returns false -- the path-resolution error, or a
     *                 summary of what this particular call put into Ignored/Errors.
     */
    UEAGENT_API bool ApplyJsonPathToObject(UObject* Object, const FString& DottedPath,
                                             const TSharedPtr<FJsonValue>& Value,
                                             FJsonApplyReport& Report, FString& OutError);
}
