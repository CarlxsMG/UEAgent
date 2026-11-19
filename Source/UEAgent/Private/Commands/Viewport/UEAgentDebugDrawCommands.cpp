// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Engine/World.h"
#include "Engine/EngineTypes.h"            // ESceneDepthPriorityGroup
#include "Components/LineBatchComponent.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "UObject/UnrealType.h"            // FArrayProperty / FScriptArrayHelper: reading AHUD::DebugTextList
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Debug drawing commands: lines, boxes, spheres, arrows, points and text drawn into the editor world or a PIE world,
 * for an agent to show where something is (a trace, a bound, a spawn point) in the viewport.
 *
 * Shapes go straight into the world's persistent line batchers (the ones DrawDebugLine uses for a lifetime or
 * persistent line) under a batch ID of this family, so clear_debug_shapes can take out only what was drawn here, and
 * every count in a reply is read back from the batcher's arrays. Text has no batcher: it goes to the debug text list
 * of each local player's HUD, as DrawDebugString does, so it exists only in a PIE world with a HUD.
 */
namespace UEAgentDebugDrawCommands
{

// MCP-DOMAIN: Editor

namespace
{
    /** Batch ID of every line and point this family draws ('UEAD'); 0 is the batcher's "no batch". */
    constexpr uint32 DebugDraw_BatchId = 0x55454144u;

    /** Most shapes one call may draw. */
    constexpr int32 DebugDraw_MaxShapes = 2000;

    /** The world persistent line batcher, or its foreground twin (drawn over the scene). */
    ULineBatchComponent* DebugDraw_Batcher(UWorld* World, bool bForeground)
    {
        return World ? World->GetLineBatcher(bForeground
            ? UWorld::ELineBatcherType::ForegroundPersistent
            : UWorld::ELineBatcherType::WorldPersistent) : nullptr;
    }

    /** Lines plus points of BatchId in Batcher (every one when BatchId is 0). */
    int32 DebugDraw_CountPrimitives(const ULineBatchComponent* Batcher, uint32 BatchId)
    {
        if (!Batcher)
        {
            return 0;
        }
        if (BatchId == 0)
        {
            return Batcher->BatchedLines.Num() + Batcher->BatchedPoints.Num() + Batcher->BatchedMeshes.Num();
        }
        int32 Count = 0;
        for (const FBatchedLine& Line : Batcher->BatchedLines)
        {
            Count += Line.BatchID == BatchId ? 1 : 0;
        }
        for (const FBatchedPoint& Point : Batcher->BatchedPoints)
        {
            Count += Point.BatchID == BatchId ? 1 : 0;
        }
        for (const FBatchedMesh& Mesh : Batcher->BatchedMeshes)
        {
            Count += Mesh.BatchID == BatchId ? 1 : 0;
        }
        return Count;
    }

    /** The HUDs DrawDebugString writes to: each local player controller's, in World. */
    TArray<AHUD*> DebugDraw_Huds(UWorld* World)
    {
        TArray<AHUD*> Huds;
        if (!World)
        {
            return Huds;
        }
        for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
        {
            APlayerController* PlayerController = It->Get();
            if (PlayerController && PlayerController->MyHUD && PlayerController->Player)
            {
                Huds.Add(PlayerController->MyHUD);
            }
        }
        return Huds;
    }

    /** Entries of the HUD's DebugTextList (a protected UPROPERTY, read by reflection); 0 when it cannot be read. */
    int32 DebugDraw_HudTextCount(AHUD* Hud)
    {
        static const FName DebugTextListName(TEXT("DebugTextList"));
        const FArrayProperty* ListProperty = Hud ? FindFProperty<FArrayProperty>(AHUD::StaticClass(), DebugTextListName) : nullptr;
        if (!ListProperty)
        {
            return 0;
        }
        FScriptArrayHelper Helper(ListProperty, ListProperty->ContainerPtrToValuePtr<void>(Hud));
        return Helper.Num();
    }

    int32 DebugDraw_TextCount(const TArray<AHUD*>& Huds)
    {
        int32 Count = 0;
        for (AHUD* Hud : Huds)
        {
            Count += DebugDraw_HudTextCount(Hud);
        }
        return Count;
    }

    /**
     * Reads the [x, y, z] array Key of Shape into Out. Null when it reads; else an error naming the shape's index and
     * the key. A missing key is an error only when bRequired, and then Out is left alone.
     */
    TSharedPtr<FJsonObject> DebugDraw_ReadVector(const TSharedPtr<FJsonObject>& Shape, int32 Index, const TCHAR* Key,
        bool bRequired, FVector& Out)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Shape->TryGetArrayField(Key, Values) || Values == nullptr)
        {
            if (Shape->HasField(Key) || bRequired)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("shapes[%d].%s must be an array of 3 numbers [x, y, z]; nothing was drawn"), Index, Key));
            }
            return nullptr;
        }
        double Components[3] = { 0.0, 0.0, 0.0 };
        if (Values->Num() != 3)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("shapes[%d].%s has %d elements, expected 3 [x, y, z]; nothing was drawn"), Index, Key, Values->Num()));
        }
        for (int32 Axis = 0; Axis < 3; ++Axis)
        {
            if (!(*Values)[Axis].IsValid() || !(*Values)[Axis]->TryGetNumber(Components[Axis]) || !FMath::IsFinite(Components[Axis]))
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("shapes[%d].%s[%d] is not a finite number; nothing was drawn"), Index, Key, Axis));
            }
        }
        Out = FVector(Components[0], Components[1], Components[2]);
        return nullptr;
    }

    /** Reads the optional number Key of Shape, else Default, into Out; an error when it is not a finite number. */
    TSharedPtr<FJsonObject> DebugDraw_ReadNumber(const TSharedPtr<FJsonObject>& Shape, int32 Index, const TCHAR* Key,
        double Default, double& Out)
    {
        Out = Default;
        if (!Shape->HasField(Key))
        {
            return nullptr;
        }
        if (!Shape->TryGetNumberField(Key, Out) || !FMath::IsFinite(Out))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("shapes[%d].%s must be a finite number; nothing was drawn"), Index, Key));
        }
        return nullptr;
    }

    /** One validated shape, ready to draw. */
    struct FDebugDrawShape
    {
        FString Type;
        FVector A = FVector::ZeroVector;     // start / center / location
        FVector B = FVector::ZeroVector;     // end / extent
        FRotator Rotation = FRotator::ZeroRotator;
        double Radius = 0.0;
        double Size = 0.0;                   // point size in pixels, arrow head size
        int32 Segments = 16;
        FString Text;
        FLinearColor Color = FLinearColor::Green;
        float Thickness = 0.0f;
    };

    /**
     * Validates shapes[Index] into Out, its color and thickness defaulting to the call's. Null when valid; otherwise
     * the error reply (nothing is drawn when any shape is invalid).
     */
    TSharedPtr<FJsonObject> DebugDraw_ParseShape(const TSharedPtr<FJsonObject>& Shape, int32 Index,
        const FLinearColor& DefaultColor, float DefaultThickness, FDebugDrawShape& Out)
    {
        if (!Shape.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("shapes[%d] is not an object; nothing was drawn"), Index));
        }
        if (!Shape->TryGetStringField(TEXT("type"), Out.Type) || Out.Type.TrimStartAndEnd().IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("shapes[%d].type is missing: one of line, box, sphere, arrow, point, text; nothing was drawn"), Index));
        }
        Out.Type = Out.Type.TrimStartAndEnd().ToLower();

        if (TSharedPtr<FJsonObject> Error = mcp_common::ReadJsonLinearColor(Shape, TEXT("color"), DefaultColor, Out.Color))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("shapes[%d]: %s"), Index,
                *Error->GetStringField(TEXT("error"))));
        }
        double Thickness = DefaultThickness;
        if (TSharedPtr<FJsonObject> Error = DebugDraw_ReadNumber(Shape, Index, TEXT("thickness"), DefaultThickness, Thickness))
        {
            return Error;
        }
        Out.Thickness = static_cast<float>(FMath::Clamp(Thickness, 0.0, 1000.0));

        TSharedPtr<FJsonObject> Error;
        if (Out.Type == TEXT("line") || Out.Type == TEXT("arrow"))
        {
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("start"), true, Out.A))) { return Error; }
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("end"), true, Out.B))) { return Error; }
            if (Out.Type == TEXT("arrow"))
            {
                if ((Error = DebugDraw_ReadNumber(Shape, Index, TEXT("size"), 20.0, Out.Size))) { return Error; }
                Out.Size = FMath::Clamp(Out.Size, 0.0, 100000.0);
            }
        }
        else if (Out.Type == TEXT("box"))
        {
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("center"), true, Out.A))) { return Error; }
            Out.B = FVector(50.0);
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("extent"), false, Out.B))) { return Error; }
            Out.B = Out.B.GetAbs();
            if (Shape->HasField(TEXT("rotation")))
            {
                Out.Rotation = mcp_common::ReadJsonRotator(Shape, TEXT("rotation"));
            }
        }
        else if (Out.Type == TEXT("sphere"))
        {
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("center"), true, Out.A))) { return Error; }
            if ((Error = DebugDraw_ReadNumber(Shape, Index, TEXT("radius"), 50.0, Out.Radius))) { return Error; }
            double Segments = 16.0;
            if ((Error = DebugDraw_ReadNumber(Shape, Index, TEXT("segments"), 16.0, Segments))) { return Error; }
            Out.Radius = FMath::Clamp(FMath::Abs(Out.Radius), 0.0, 1.0e7);
            Out.Segments = FMath::Clamp(FMath::RoundToInt(Segments), 4, 64);
        }
        else if (Out.Type == TEXT("point"))
        {
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("location"), true, Out.A))) { return Error; }
            if ((Error = DebugDraw_ReadNumber(Shape, Index, TEXT("size"), 10.0, Out.Size))) { return Error; }
            Out.Size = FMath::Clamp(Out.Size, 1.0, 1000.0);
        }
        else if (Out.Type == TEXT("text"))
        {
            if ((Error = DebugDraw_ReadVector(Shape, Index, TEXT("location"), true, Out.A))) { return Error; }
            if (!Shape->TryGetStringField(TEXT("text"), Out.Text) || Out.Text.IsEmpty())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("shapes[%d].text is missing or empty; nothing was drawn"), Index));
            }
            if ((Error = DebugDraw_ReadNumber(Shape, Index, TEXT("size"), 1.0, Out.Size))) { return Error; }
            Out.Size = FMath::Clamp(Out.Size, 0.1, 20.0);
        }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("shapes[%d].type '%s' is not one of line, box, sphere, arrow, point, text; nothing was drawn"), Index, *Out.Type));
        }
        return nullptr;
    }

    /** The world the `world` param selects (editor by default), or the error reply. */
    TSharedPtr<FJsonObject> DebugDraw_ResolveWorld(const TSharedPtr<FJsonObject>& Params, UWorld*& OutWorld, FString& OutSelector)
    {
        FString Error;
        OutWorld = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), OutSelector, Error);
        if (!OutWorld)
        {
            return mcp_common::MakeErrorReply(Error.IsEmpty() ? TEXT("No world could be resolved") : Error);
        }
        return nullptr;
    }
}

// MCP:draw_debug_shapes - Draw debug lines, boxes, spheres, arrows, points and text into the editor world (default) or a PIE world, to show a location, a bound, a path or a trace in the viewport. Each shape lasts duration seconds, or until clear_debug_shapes with persistent=true; nothing is saved, selectable or undoable.
//   @para shapes is a list of objects with a type and its fields, positions in world units: line {start, end}; arrow {start, end, size = head size, default 20}; box {center, extent = half size, default [50,50,50], rotation = {pitch, yaw, roll} degrees}; sphere {center, radius (default 50), segments (4..64, default 16)}; point {location, size in pixels (default 10)}; text {location, text, size = font scale (default 1)}. Any shape may carry its own color and thickness. Text goes to the HUD of each local player, so it is drawn only in a PIE world with a HUD; in the editor world it is skipped and the reply says so. An invalid shape is an error and nothing is drawn.
//   @param shapes [array] - Required list of shape objects as described above, at most 2000.
//   @param color [array] - Optional default color [R, G, B] or [R, G, B, A] in 0..1 (default green [0, 1, 0, 1]); a shape's own color wins.
//   @param thickness - Optional default line thickness in world units (default 0 = one pixel, clamped 0..1000); a shape's own thickness wins.
//   @param duration - Optional seconds the shapes stay (default 10, clamped 0.01..86400); ignored when persistent is true. The editor world counts them down while the editor ticks.
//   @param persistent - Optional bool (default false): keep the shapes until clear_debug_shapes.
//   @param foreground - Optional bool (default false): draw lines and points over the scene instead of depth-tested in it.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { shapes_requested: int, drawn: int, lines_added: int, texts_added: int, skipped: [{ index: int, type: string, reason: string }], by_type: { line?: int, box?: int, sphere?: int, arrow?: int, point?: int, text?: int }, lifetime_seconds: float (-1 = until cleared), batcher: string, requested?: object, world_name, world_package, world_type, net_mode, pie_instance }
//   @hints additive not-idempotent
TSharedPtr<FJsonObject> HandleDrawDebugShapes(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* ShapesJson = nullptr;
    if (!Params->TryGetArrayField(TEXT("shapes"), ShapesJson) || ShapesJson == nullptr)
    {
        return mcp_common::MissingParameterReply(TEXT("shapes"));
    }
    if (ShapesJson->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("shapes is empty; nothing was drawn"));
    }
    if (ShapesJson->Num() > DebugDraw_MaxShapes)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("shapes holds %d entries, at most %d per call; nothing was drawn"), ShapesJson->Num(), DebugDraw_MaxShapes));
    }

    FLinearColor DefaultColor = FLinearColor::Green;
    if (TSharedPtr<FJsonObject> Error = mcp_common::ReadJsonLinearColor(Params, TEXT("color"), FLinearColor::Green, DefaultColor))
    {
        return Error;
    }
    double Thickness = 0.0;
    Params->TryGetNumberField(TEXT("thickness"), Thickness);
    const float DefaultThickness = FMath::IsFinite(Thickness) ? static_cast<float>(FMath::Clamp(Thickness, 0.0, 1000.0)) : 0.0f;
    double Duration = 10.0;
    Params->TryGetNumberField(TEXT("duration"), Duration);
    Duration = FMath::IsFinite(Duration) ? FMath::Clamp(Duration, 0.01, 86400.0) : 10.0;
    bool bPersistent = false;
    Params->TryGetBoolField(TEXT("persistent"), bPersistent);
    bool bForeground = false;
    Params->TryGetBoolField(TEXT("foreground"), bForeground);

    TArray<FDebugDrawShape> Shapes;
    Shapes.Reserve(ShapesJson->Num());
    for (int32 Index = 0; Index < ShapesJson->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Value = (*ShapesJson)[Index];
        const TSharedPtr<FJsonObject>* ShapeObject = nullptr;
        if (!Value.IsValid() || !Value->TryGetObject(ShapeObject) || ShapeObject == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("shapes[%d] is not an object; nothing was drawn"), Index));
        }
        if (TSharedPtr<FJsonObject> Error = DebugDraw_ParseShape(*ShapeObject, Index, DefaultColor, DefaultThickness, Shapes.AddDefaulted_GetRef()))
        {
            return Error;
        }
    }

    UWorld* World = nullptr;
    FString Selector;
    if (TSharedPtr<FJsonObject> Error = DebugDraw_ResolveWorld(Params, World, Selector))
    {
        return Error;
    }
    ULineBatchComponent* Batcher = DebugDraw_Batcher(World, bForeground);
    if (!Batcher)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The %s world has no line batcher (it is not initialized for rendering); nothing was drawn"), *Selector));
    }
    const uint8 DepthPriority = bForeground ? SDPG_Foreground : SDPG_World;
    const float LifeTime = bPersistent ? -1.0f : static_cast<float>(Duration);
    const TArray<AHUD*> Huds = DebugDraw_Huds(World);
    AActor* TextBase = World->GetWorldSettings();

    const int32 LinesBefore = DebugDraw_CountPrimitives(Batcher, DebugDraw_BatchId);
    const int32 TextsBefore = DebugDraw_TextCount(Huds);
    int32 Drawn = 0;
    TMap<FString, int32> ByType;
    TArray<TSharedPtr<FJsonValue>> Skipped;
    auto Skip = [&Skipped](int32 Index, const FString& Type, const FString& Reason)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("type"), Type);
        Entry->SetStringField(TEXT("reason"), Reason);
        Skipped.Add(MakeShared<FJsonValueObject>(Entry));
    };

    for (int32 Index = 0; Index < Shapes.Num(); ++Index)
    {
        const FDebugDrawShape& Shape = Shapes[Index];
        if (Shape.Type == TEXT("text"))
        {
            if (Huds.Num() == 0)
            {
                Skip(Index, Shape.Type, TEXT("no local player HUD in this world: text draws only in a PIE world with a HUD"));
                continue;
            }
            const int32 Before = DebugDraw_TextCount(Huds);
            for (AHUD* Hud : Huds)
            {
                Hud->AddDebugText(Shape.Text, TextBase, bPersistent ? -1.0f : static_cast<float>(Duration), Shape.A, Shape.A,
                    Shape.Color.ToFColor(true), /*bSkipOverwriteCheck=*/true, /*bAbsoluteLocation=*/true,
                    /*bKeepAttachedToActor=*/false, nullptr, static_cast<float>(Shape.Size), /*bDrawShadow=*/true);
            }
            if (DebugDraw_TextCount(Huds) > Before)
            {
                ++Drawn;
                ByType.FindOrAdd(Shape.Type)++;
            }
            else
            {
                Skip(Index, Shape.Type, TEXT("the HUD did not add the text"));
            }
            continue;
        }

        const int32 Before = DebugDraw_CountPrimitives(Batcher, DebugDraw_BatchId);
        if (Shape.Type == TEXT("line"))
        {
            Batcher->DrawLine(Shape.A, Shape.B, Shape.Color, DepthPriority, Shape.Thickness, LifeTime, DebugDraw_BatchId);
        }
        else if (Shape.Type == TEXT("arrow"))
        {
            Batcher->DrawDirectionalArrow(Shape.A, Shape.B, static_cast<float>(Shape.Size), Shape.Color, LifeTime, DepthPriority,
                Shape.Thickness, DebugDraw_BatchId);
        }
        else if (Shape.Type == TEXT("box"))
        {
            Batcher->DrawBox(Shape.A, Shape.B, Shape.Rotation.Quaternion(), Shape.Color, LifeTime, DepthPriority, Shape.Thickness,
                DebugDraw_BatchId);
        }
        else if (Shape.Type == TEXT("sphere"))
        {
            Batcher->DrawSphere(Shape.A, static_cast<float>(Shape.Radius), Shape.Segments, Shape.Color, LifeTime, DepthPriority,
                Shape.Thickness, DebugDraw_BatchId);
        }
        else if (Shape.Type == TEXT("point"))
        {
            Batcher->DrawPoint(Shape.A, Shape.Color, static_cast<float>(Shape.Size), DepthPriority, LifeTime, DebugDraw_BatchId);
        }
        if (DebugDraw_CountPrimitives(Batcher, DebugDraw_BatchId) > Before)
        {
            ++Drawn;
            ByType.FindOrAdd(Shape.Type)++;
        }
        else
        {
            Skip(Index, Shape.Type, TEXT("the line batcher added nothing for this shape"));
        }
    }

    TSharedPtr<FJsonObject> ByTypeJson = MakeShared<FJsonObject>();
    for (const TPair<FString, int32>& Pair : ByType)
    {
        ByTypeJson->SetNumberField(Pair.Key, Pair.Value);
    }
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("shapes_requested"), Shapes.Num());
    Data->SetNumberField(TEXT("drawn"), Drawn);
    Data->SetNumberField(TEXT("lines_added"), DebugDraw_CountPrimitives(Batcher, DebugDraw_BatchId) - LinesBefore);
    Data->SetNumberField(TEXT("texts_added"), DebugDraw_TextCount(Huds) - TextsBefore);
    Data->SetArrayField(TEXT("skipped"), Skipped);
    Data->SetObjectField(TEXT("by_type"), ByTypeJson);
    Data->SetNumberField(TEXT("lifetime_seconds"), LifeTime);
    Data->SetStringField(TEXT("batcher"), bForeground ? TEXT("foreground") : TEXT("world"));
    mcp_common::AddRequestedEcho(Data, Params, { TEXT("persistent"), TEXT("duration"), TEXT("foreground"), TEXT("world") });
    mcp_common::AddWorldEvidence(Data, World);
    return mcp_common::MakeOutcomeReply(Data, Drawn == Shapes.Num(), Drawn == Shapes.Num() ? FString() : FString::Printf(
        TEXT("draw_debug_shapes: drew %d of %d shapes; skipped lists why the others were not"), Drawn, Shapes.Num()));
}

// MCP:clear_debug_shapes - Remove debug shapes from the editor world (default) or a PIE world: by default only what draw_debug_shapes drew (its lines, points and text), with all=true every persistent debug line, point and string in that world, the engine's and other tools' included. Reports how many were there before and after.
//   @param all - Optional bool (default false): also clear the debug lines and strings that something other than draw_debug_shapes drew.
//   @param world - Optional world selector: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie').
//   @returns { cleared_lines: int, cleared_texts: int, remaining_lines: int, remaining_texts: int, requested?: object, world_name, world_package, world_type, net_mode, pie_instance }
//   @hints idempotent
TSharedPtr<FJsonObject> HandleClearDebugShapes(const TSharedPtr<FJsonObject>& Params)
{
    bool bAll = false;
    Params->TryGetBoolField(TEXT("all"), bAll);

    UWorld* World = nullptr;
    FString Selector;
    if (TSharedPtr<FJsonObject> Error = DebugDraw_ResolveWorld(Params, World, Selector))
    {
        return Error;
    }

    ULineBatchComponent* Batchers[] = { DebugDraw_Batcher(World, false), DebugDraw_Batcher(World, true) };
    const uint32 CountId = bAll ? 0u : DebugDraw_BatchId;
    const TArray<AHUD*> Huds = DebugDraw_Huds(World);
    AActor* TextBase = World->GetWorldSettings();

    int32 LinesBefore = 0;
    for (ULineBatchComponent* Batcher : Batchers)
    {
        LinesBefore += DebugDraw_CountPrimitives(Batcher, CountId);
    }
    const int32 TextsBefore = DebugDraw_TextCount(Huds);

    for (ULineBatchComponent* Batcher : Batchers)
    {
        if (Batcher)
        {
            if (bAll)
            {
                Batcher->Flush();
            }
            else
            {
                Batcher->ClearBatch(DebugDraw_BatchId);
            }
        }
    }
    for (AHUD* Hud : Huds)
    {
        if (bAll)
        {
            Hud->RemoveAllDebugStrings();
        }
        else
        {
            // Text drawn here is anchored to the world settings actor, as DrawDebugString's absolute text is.
            Hud->RemoveDebugText(TextBase, /*bLeaveDurationText=*/false);
        }
    }

    int32 LinesAfter = 0;
    for (ULineBatchComponent* Batcher : Batchers)
    {
        LinesAfter += DebugDraw_CountPrimitives(Batcher, CountId);
    }
    const int32 TextsAfter = DebugDraw_TextCount(Huds);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("cleared_lines"), LinesBefore - LinesAfter);
    Data->SetNumberField(TEXT("cleared_texts"), TextsBefore - TextsAfter);
    Data->SetNumberField(TEXT("remaining_lines"), LinesAfter);
    Data->SetNumberField(TEXT("remaining_texts"), TextsAfter);
    mcp_common::AddRequestedEcho(Data, Params, { TEXT("all"), TEXT("world") });
    mcp_common::AddWorldEvidence(Data, World);
    const bool bCleared = LinesAfter == 0 && (!bAll || TextsAfter == 0);
    return mcp_common::MakeOutcomeReply(Data, bCleared, bCleared ? FString() : FString::Printf(
        TEXT("clear_debug_shapes: %d line(s) and %d text(s) are still there after clearing"), LinesAfter, TextsAfter));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("draw_debug_shapes"), &HandleDrawDebugShapes },
    { TEXT("clear_debug_shapes"), &HandleClearDebugShapes },
};
} // namespace UEAgentDebugDrawCommands
