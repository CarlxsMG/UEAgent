#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/McpEnumLabels.h"
#include "Commands/Environment/LandscapeHeightSampling.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"                       // TActorIterator
#include "GameFramework/Actor.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"

// Landscape (Runtime/Landscape)
#include "Landscape.h"                         // ALandscape
#include "LandscapeProxy.h"                    // ALandscapeProxy, ELandscapeImportAlphamapType, FLandscapeImportLayerInfo, FLandscapeLayer (Import sig), AddTargetLayer/GetTargetLayers
#include "LandscapeInfo.h"                     // ULandscapeInfo, FLandscapeInfoLayerSettings,               CreateTargetLayerSettingsFor
#include "LandscapeComponent.h"               // ULandscapeComponent::GetLayerWeightAtLocation,            GetLandscapeComponentForXY
#include "LandscapeLayerInfoObject.h"          // ULandscapeLayerInfoObject (SetLayerName/GetLayerName getters/setters; LayerName public prop deprecated 5.7)
#include "LandscapeEdit.h"                     // TAlphamapAccessor<bUseInterp> (FAlphamapAccessor deprecated 5.7), ELandscapeLayerPaintingRestriction (editor-only), FHeightmapAccessor
#include "LandscapeEditLayer.h"                // ULandscapeEditLayerBase::GetGuid() -- edit-layer-aware heightmap writes (UE 5.8); FScopedSetLandscapeEditingLayer + GetEditLayerConst come from Landscape.h

// Landscape splines (Runtime/Landscape) -- editor-only authoring
#include "LandscapeSplinesComponent.h"         // ULandscapeSplinesComponent (GetSplinesComponent/GetControlPoints/GetSegments)
#include "LandscapeSplineControlPoint.h"       // ULandscapeSplineControlPoint (Location/Width/ConnectedSegments), FLandscapeSplineConnection
#include "LandscapeSplineSegment.h"            // ULandscapeSplineSegment (Connections[2]), FLandscapeSplineSegmentConnection

// Landscape material + grass type (Runtime/Landscape) — LANDSCAPE-EXT
#include "LandscapeGrassType.h"                 // ULandscapeGrassType, FGrassVariety, EGrassScaling
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"  // UMaterialExpressionLandscapeLayerBlend, FLayerBlendInput
#include "Materials/MaterialExpressionSmoothStep.h"           // UMaterialExpressionSmoothStep
#include "Materials/MaterialExpressionAbs.h"                  // UMaterialExpressionAbs (used as a OneMinus surrogate)
#include "Materials/MaterialInterface.h"        // UMaterialInterface (LandscapeMaterial / LandscapeHoleMaterial UPROPERTYs)
#include "UObject/UnrealType.h"                 // FProperty / FindFieldChecked (PostEditChangeProperty event for material assign)

// Foliage (Runtime/Foliage)
#include "InstancedFoliageActor.h"             // AInstancedFoliageActor
#include "FoliageType.h"                       // UFoliageType
#include "FoliageType_InstancedStaticMesh.h"   // UFoliageType_InstancedStaticMesh
#include "InstancedFoliage.h"                  // FFoliageInfo, FFoliageInstance
#include "Engine/StaticMesh.h"

#include "Math/RandomStream.h"                 // FRandomStream (seeded scatter)

// Landscape sculpt + heightmap I/O
#include "LandscapeEditorUtils.h"              // LandscapeEditorUtils::SetHeightmapData (LANDSCAPEEDITOR_API)
#include "LandscapeImportHelper.h"             // FLandscapeImportHelper::GetHeightmapImportDescriptor / GetHeightmapImportData / TransformHeightmapImportData
#include "LandscapeFileFormatInterface.h"      // ILandscapeHeightmapFileFormat (Export signature)
#include "LandscapeEditorModule.h"             // ILandscapeEditorModule / FModuleManager::GetModuleChecked<ILandscapeEditorModule>("LandscapeEditor")
#include "Modules/ModuleManager.h"             // FModuleManager
#include "Misc/Paths.h"                        // FPaths::GetExtension
#include "HAL/FileManager.h"                  // IFileManager::Get().FileExists/FileSize (export verify)

/**
 * Landscape and foliage commands.
 *
 * Landscape (Runtime/Landscape, Editor/LandscapeEditor): create an ALandscape in the editor world (via
 * ALandscape::Import of a flat heightmap) and inspect it, sculpt it and import/export its heightmap, and
 * author its paint layers, splines, material, grass types and landscape material nodes.
 *
 * Foliage (Runtime/Foliage, Editor/FoliageEdit): create and tune UFoliageType_InstancedStaticMesh assets,
 * and place, scatter, query and remove their instances in the editor-world InstancedFoliageActor.
 */
namespace UEAgentLandscapeCommands
{

// MCP-DOMAIN: Level

namespace
{
    // Read an optional positive int from params, defaulting if absent.
    int32 ParamInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 Default)
    {
        double D = 0.0;
        if (Params->TryGetNumberField(Field, D))
        {
            return static_cast<int32>(D);
        }
        return Default;
    }

    // Find an ALandscape actor in the editor world. If NameOrLabel is non-empty,
    // match by EITHER the internal object name (GetName(), e.g.
    // "Landscape_UAID_...") OR the actor label (GetActorLabel()); otherwise
    // return the first landscape found. Accepting both mirrors
    // mcp_common::FindActorByName / set_actor_transform, so every command resolves
    // a landscape the same way.
    ALandscape* FindLandscape(UWorld* World, const FString& NameOrLabel)
    {
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<ALandscape> It(World); It; ++It)
        {
            ALandscape* Landscape = *It;
            if (!Landscape)
            {
                continue;
            }
            if (NameOrLabel.IsEmpty()
                || Landscape->GetActorLabel() == NameOrLabel
                || Landscape->GetName() == NameOrLabel)
            {
                return Landscape;
            }
        }
        return nullptr;
    }

    // The landscape FindLandscape picks in World for ActorName; null, with OutError set, when there is none.
    ALandscape* Landscape_FindOrError(UWorld* World, const FString& ActorName, TSharedPtr<FJsonObject>& OutError)
    {
        ALandscape* Landscape = FindLandscape(World, ActorName);
        if (!Landscape)
        {
            OutError = mcp_common::MakeErrorReply(
                ActorName.IsEmpty()
                    ? TEXT("No landscape found in the editor world")
                    : FString::Printf(TEXT("Landscape actor not found: %s"), *ActorName));
        }
        return Landscape;
    }

    // Landscape_FindOrError in the editor world; also null, with OutError set, when there is no editor world.
    ALandscape* Landscape_FindInEditorWorld(const FString& ActorName, TSharedPtr<FJsonObject>& OutError)
    {
        UWorld* World = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World))
        {
            OutError = NoWorld;
            return nullptr;
        }
        return Landscape_FindOrError(World, ActorName, OutError);
    }

    // The "landscape_actor_name" landscape (the first one when the param is absent) in the editor world;
    // null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_FindByActorNameParam(const TSharedPtr<FJsonObject>& Params, ALandscape*& OutLandscape)
    {
        FString ActorName;
        Params->TryGetStringField(TEXT("landscape_actor_name"), ActorName);
        TSharedPtr<FJsonObject> Error;
        OutLandscape = Landscape_FindInEditorWorld(ActorName, Error);
        return Error;
    }

    // The "landscape_name" landscape (the first one when the param is absent) in the editor world;
    // null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_FindByNameParam(const TSharedPtr<FJsonObject>& Params, ALandscape*& OutLandscape)
    {
        FString ActorName;
        Params->TryGetStringField(TEXT("landscape_name"), ActorName);
        TSharedPtr<FJsonObject> Error;
        OutLandscape = Landscape_FindInEditorWorld(ActorName, Error);
        return Error;
    }

    // A reply payload whose NameKey field is Landscape's label.
    TSharedPtr<FJsonObject> Landscape_NewReplyData(const TCHAR* NameKey, const ALandscape* Landscape)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(NameKey, Landscape->GetActorLabel());
        return Data;
    }

    // The "layer_info_path" layer info and "landscape_actor_name" landscape; null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_ResolveLayerTarget(const TSharedPtr<FJsonObject>& Params, FString& OutLayerInfoPath,
        ULandscapeLayerInfoObject*& OutLayerInfo, ALandscape*& OutLandscape)
    {
        UWorld* EditorWorld = nullptr;
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(EditorWorld)) { return NoWorld; }

        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("layer_info_path"), OutLayerInfoPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        OutLayerInfo =
            Cast<ULandscapeLayerInfoObject>(StaticLoadObject(ULandscapeLayerInfoObject::StaticClass(), nullptr, *OutLayerInfoPath));
        if (!OutLayerInfo)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("LandscapeLayerInfoObject not found: %s"), *OutLayerInfoPath));
        }

        return Landscape_FindByActorNameParam(Params, OutLandscape);
    }

    // A reply payload naming an asset the create commands made or found: name, package_path and asset_path.
    TSharedPtr<FJsonObject> Landscape_NewAssetData(const FString& Name, const FString& PackagePath, const UObject* Asset)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("name"), Name);
        Data->SetStringField(TEXT("package_path"), PackagePath);
        Data->SetStringField(TEXT("asset_path"), Asset->GetPathName());
        return Data;
    }

    // The vertex rect as the JSON array [MinX, MinY, MaxX, MaxY].
    TArray<TSharedPtr<FJsonValue>> Landscape_RectToJson(int32 MinX, int32 MinY, int32 MaxX, int32 MaxY)
    {
        TArray<TSharedPtr<FJsonValue>> Rect;
        Rect.Add(MakeShared<FJsonValueNumber>(MinX));
        Rect.Add(MakeShared<FJsonValueNumber>(MinY));
        Rect.Add(MakeShared<FJsonValueNumber>(MaxX));
        Rect.Add(MakeShared<FJsonValueNumber>(MaxY));
        return Rect;
    }

    // An interval as {"min", "max"}.
    template <typename IntervalType>
    TSharedPtr<FJsonObject> Landscape_IntervalToJson(const IntervalType& Interval)
    {
        TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
        Object->SetNumberField(TEXT("min"), Interval.Min);
        Object->SetNumberField(TEXT("max"), Interval.Max);
        return Object;
    }

    // Reads the {"min", "max"} object param Key into Out (only the bounds present); true when it is an object.
    bool Landscape_ReadIntervalParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, FFloatInterval& Out)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Params->TryGetObjectField(Key, Obj) && Obj && Obj->IsValid())
        {
            double M = 0.0;
            if ((*Obj)->TryGetNumberField(TEXT("min"), M)) { Out.Min = (float)M; }
            if ((*Obj)->TryGetNumberField(TEXT("max"), M)) { Out.Max = (float)M; }
            return true;
        }
        return false;
    }

    // Base (first) edit-layer GUID for edit-layer-aware writes (UE 5.8). Shared by
    // both the weightmap writer (HandlePaintLandscapeLayer, which precedes the
    // definition below) and the heightmap sculpt writers. Callers scope their accessor block
    // with FScopedSetLandscapeEditingLayer(Landscape, this GUID) so the write lands
    // in the base edit layer instead of the composite (which the edit-layer merge
    // would otherwise recompose from the base, silently dropping the write).
    // Definition lives further down, in the sculpt anonymous namespace (same
    // unnamed namespace within this translation unit); full rationale is there.
    FGuid GetBaseEditLayerGuid(ALandscape* Landscape);

    // Convert a world-space box [BoxMin..BoxMax] (FBox) into a
    // clamped landscape vertex rect (MinVX, MinVY, MaxVX, MaxVY) using the
    // landscape actor's transform. Returns false if the box is empty/outside
    // the landscape extent.
    //
    // Vertex mapping: WorldX = ActorLocation.X + VX * Scale.X (analogous for Y).
    // We round inward (floor min / ceil max) so partial-coverage vertices are
    // included — sculpting a 1-unit-wide box should still touch at least one row
    // and one column of vertices.
    bool WorldBoxToVertexRect(ALandscape* Landscape, const FBox& WorldBox,
                              int32 ExtentMinX, int32 ExtentMinY, int32 ExtentMaxX, int32 ExtentMaxY,
                              int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY)
    {
        if (!Landscape)
        {
            return false;
        }
        const FVector ActorLoc = Landscape->GetActorLocation();
        const FVector ActorScale = Landscape->GetActorScale3D();
        if (ActorScale.X <= KINDA_SMALL_NUMBER || ActorScale.Y <= KINDA_SMALL_NUMBER)
        {
            return false;
        }
        // Round inward (floor/ceil) so the rect contains every vertex whose
        // world position lies inside the box, including boundary touches.
        const double MinVX = FMath::FloorToDouble((WorldBox.Min.X - ActorLoc.X) / ActorScale.X);
        const double MinVY = FMath::FloorToDouble((WorldBox.Min.Y - ActorLoc.Y) / ActorScale.Y);
        const double MaxVX = FMath::CeilToDouble ((WorldBox.Max.X - ActorLoc.X) / ActorScale.X);
        const double MaxVY = FMath::CeilToDouble ((WorldBox.Max.Y - ActorLoc.Y) / ActorScale.Y);

        // Clamp to landscape extent.
        OutMinX = FMath::Clamp(static_cast<int32>(MinVX), ExtentMinX, ExtentMaxX);
        OutMinY = FMath::Clamp(static_cast<int32>(MinVY), ExtentMinY, ExtentMaxY);
        OutMaxX = FMath::Clamp(static_cast<int32>(MaxVX), ExtentMinX, ExtentMaxX);
        OutMaxY = FMath::Clamp(static_cast<int32>(MaxVY), ExtentMinY, ExtentMaxY);
        if (OutMaxX < OutMinX) { Swap(OutMinX, OutMaxX); }
        if (OutMaxY < OutMinY) { Swap(OutMinY, OutMaxY); }
        // Empty intersection.
        return OutMaxX > OutMinX || OutMaxY > OutMinY;
    }

    // Parse the box from params: accepts EITHER {box_min:[x,y,z], box_max:[x,y,z]}
    // OR {center:[x,y,z], extent:[hx,hy,hz]} (a "box" with half-extents).
    // Returns true on success.
    bool ParseWorldBox(const TSharedPtr<FJsonObject>& Params, FBox& OutBox)
    {
        const TArray<TSharedPtr<FJsonValue>>* BoxMinArr = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* BoxMaxArr = nullptr;
        if (Params->TryGetArrayField(TEXT("box_min"), BoxMinArr) &&
            Params->TryGetArrayField(TEXT("box_max"), BoxMaxArr) &&
            BoxMinArr && BoxMinArr->Num() >= 3 && BoxMaxArr && BoxMaxArr->Num() >= 3)
        {
            OutBox.Min = FVector((*BoxMinArr)[0]->AsNumber(), (*BoxMinArr)[1]->AsNumber(), (*BoxMinArr)[2]->AsNumber());
            OutBox.Max = FVector((*BoxMaxArr)[0]->AsNumber(), (*BoxMaxArr)[1]->AsNumber(), (*BoxMaxArr)[2]->AsNumber());
            return true;
        }
        const TArray<TSharedPtr<FJsonValue>>* CenterArr = nullptr;
        const TArray<TSharedPtr<FJsonValue>>* ExtentArr = nullptr;
        if (Params->TryGetArrayField(TEXT("center"), CenterArr) &&
            Params->TryGetArrayField(TEXT("extent"), ExtentArr) &&
            CenterArr && CenterArr->Num() >= 3 && ExtentArr && ExtentArr->Num() >= 3)
        {
            const FVector Center((*CenterArr)[0]->AsNumber(), (*CenterArr)[1]->AsNumber(), (*CenterArr)[2]->AsNumber());
            const FVector Extent((*ExtentArr)[0]->AsNumber(), (*ExtentArr)[1]->AsNumber(), (*ExtentArr)[2]->AsNumber());
            OutBox = FBox(Center - Extent, Center + Extent);
            return true;
        }
        return false;
    }

    // The box a sculpt command edits when its params name none; false without a landscape.
    bool Landscape_DefaultSculptBox(ALandscape* Landscape, FBox& OutBox)
    {
        if (!Landscape)
        {
            return false;
        }
        const FVector ActorLoc = Landscape->GetActorLocation();
        const FVector ActorScale = Landscape->GetActorScale3D();
        // Default: 8x8 quad box centered on the landscape origin in landscape coords.
        const FVector Center = ActorLoc + 0.5 * (ActorScale.X + ActorScale.Y) * FVector(1.0, 1.0, 0.0);
        const FVector Extent(4.0 * ActorScale.X, 4.0 * ActorScale.Y, 50.0 * FMath::Max(ActorScale.Z, 1.0));
        OutBox = FBox(Center - Extent, Center + Extent);
        return true;
    }
}

// ========================================================================== //
// Landscape
// ========================================================================== //

// MCP:create_landscape - Create a new terrain: spawn an ALandscape actor in the editor world sized by section_size x sections_per_component x component_count, filled with a FLAT heightmap at `height`. This is step 1 of two -- the relief comes from a following import_landscape_heightmap onto this actor, and going through this pair instead of the editor's New Landscape dialog is the point: the dialog treats its Location field as the terrain CENTRE (not the corner) and can fill only part of the components on import, silently. Verify the result with get_landscape_info (location, size_x/size_y, component_count), never with what you typed.
//   @param section_size - Vertices per section, default 63 (rounded up to power of two)
//   @param sections_per_component - 1 or 2 sections per component
//   @param component_count_x - Components on X axis, default 8
//   @param component_count_y - Components on Y axis, default 8
//   @param height - Initial uint16 height value, default 32768
//   @param location - Optional [x, y, z] world location
//   @returns { actor_name: string, actor_path: string, section_size: int, subsection_size_quads: int, sections_per_component: int, component_count_x: int, component_count_y: int, quads_per_component: int, size_x: int, size_y: int, vertex_count: int, height_midpoint: int, imported: bool, landscape_created: bool, location: [number] }
TSharedPtr<FJsonObject> HandleCreateLandscape(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    // ---- Dimensions ------------------------------------------------------ //
    // SubsectionSizeQuads is derived from the requested section size; +1 must be
    // a power of two (engine requirement). Default 63 -> 64 (power of two).
    const int32 SectionSize          = ParamInt(Params, TEXT("section_size"), 63);
    const int32 SubsectionSizeQuads  = SectionSize;
    const int32 SectionsPerComponent = FMath::Clamp(ParamInt(Params, TEXT("sections_per_component"), 1), 1, 2);
    const int32 ComponentCountX      = FMath::Max(1, ParamInt(Params, TEXT("component_count_x"), 8));
    const int32 ComponentCountY      = FMath::Max(1, ParamInt(Params, TEXT("component_count_y"), 8));

    // Flat height (uint16 heightmap midpoint = 32768 == 0.0 world Z).
    const int32 HeightMidpoint = FMath::Clamp(ParamInt(Params, TEXT("height"), 32768), 0, 65535);

    // QuadsPerComponent = SectionsPerComponent * SubsectionSizeQuads (mirrors the
    // engine's New-Landscape tool). Vertex grid is (Quads + 1) on each axis.
    const int32 QuadsPerComponent = SectionsPerComponent * SubsectionSizeQuads;
    const int32 SizeX = ComponentCountX * QuadsPerComponent + 1;
    const int32 SizeY = ComponentCountY * QuadsPerComponent + 1;
    const int32 TotalVerts = SizeX * SizeY;

    // Cap raised 8192^2 -> 16384^2 so create_landscape accepts very large maps
    // (e.g. 11177^2 = ~125M verts). 16384^2 = ~268M verts (~536 MB uint16 temp)
    // is the new sanity ceiling against runaway allocations; int32 vertex indexing is safe.
    if (TotalVerts <= 0 || TotalVerts > (16384 * 16384))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid landscape dimensions (%dx%d verts)"), SizeX, SizeY));
    }

    // ---- Location / rotation --------------------------------------------- //
    FVector Location = FVector::ZeroVector;
    if (Params->HasField(TEXT("location")))
    {
        Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    }
    const FRotator Rotation = FRotator::ZeroRotator;

    // ---- Build the flat height data -------------------------------------- //
    // Verified callsite: Editor/LandscapeEditor/.../NewLandscape.cpp uses an
    // FGuid() (invalid/default) key for the single height layer.
    TArray<uint16> HeightData;
    HeightData.Init(static_cast<uint16>(HeightMidpoint), TotalVerts);

    TMap<FGuid, TArray<uint16>> HeightDataPerLayers;
    HeightDataPerLayers.Add(FGuid(), MoveTemp(HeightData));

    TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerDataPerLayers;
    MaterialLayerDataPerLayers.Add(FGuid(), TArray<FLandscapeImportLayerInfo>());

    // ---- Spawn + Import -------------------------------------------------- //
    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "CreateLandscape", "Create Landscape (MCP)"));

    ALandscape* Landscape = World->SpawnActor<ALandscape>(Location, Rotation);
    if (!Landscape)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn ALandscape actor"));
    }
    Landscape->Modify();

    bool bImported = false;
    FString ImportError;
    {
        // ALandscape::Import (declared on ALandscapeProxy base, public in UE 5.7):
        //   void Import(const FGuid& InGuid, int32 MinX, int32 MinY, int32 MaxX, int32 MaxY,
        //               int32 NumSubsections, int32 SubsectionSizeQuads,
        //               const TMap<FGuid, TArray<uint16>>& HeightData, const TCHAR* HeightmapFileName,
        //               const TMap<FGuid, TArray<FLandscapeImportLayerInfo>>& MaterialLayerInfos,
        //               ELandscapeImportAlphamapType AlphamapType,
        //               const TArrayView<const FLandscapeLayer>& ImportLayers);
        Landscape->Import(
            FGuid::NewGuid(),
            0, 0, SizeX - 1, SizeY - 1,
            SectionsPerComponent, SubsectionSizeQuads,
            HeightDataPerLayers, nullptr,
            MaterialLayerDataPerLayers, ELandscapeImportAlphamapType::Additive,
            TArrayView<const FLandscapeLayer>());

        // Verify by reading back the LandscapeInfo (non-null == Import wired the
        // landscape's components into the level).
        ULandscapeInfo* LandscapeInfo = Landscape->GetLandscapeInfo();
        if (LandscapeInfo)
        {
            LandscapeInfo->UpdateLayerInfoMap(Landscape);
            bImported = true;
        }
        else
        {
            ImportError = TEXT("ALandscape::Import did not produce a valid ULandscapeInfo");
        }
    }

    const FString ActorName = Landscape->GetActorLabel();

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_name"), ActorName);
    ResultObj->SetStringField(TEXT("actor_path"), Landscape->GetPathName());
    ResultObj->SetNumberField(TEXT("section_size"), SectionSize);
    ResultObj->SetNumberField(TEXT("subsection_size_quads"), SubsectionSizeQuads);
    ResultObj->SetNumberField(TEXT("sections_per_component"), SectionsPerComponent);
    ResultObj->SetNumberField(TEXT("component_count_x"), ComponentCountX);
    ResultObj->SetNumberField(TEXT("component_count_y"), ComponentCountY);
    ResultObj->SetNumberField(TEXT("quads_per_component"), QuadsPerComponent);
    ResultObj->SetNumberField(TEXT("size_x"), SizeX);
    ResultObj->SetNumberField(TEXT("size_y"), SizeY);
    ResultObj->SetNumberField(TEXT("vertex_count"), TotalVerts);
    ResultObj->SetNumberField(TEXT("height_midpoint"), HeightMidpoint);
    ResultObj->SetBoolField(TEXT("imported"), bImported);
    ResultObj->SetBoolField(TEXT("landscape_created"), true);

    ResultObj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Location));

    if (!bImported)
    {
        ResultObj->SetStringField(TEXT("import_warning"), ImportError);
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_landscape_info - Find the terrain and read its geometry: every ALandscape actor in the editor world with its world location and scale, vertex extent (min_x/min_y/max_x/max_y, size_x/size_y) and component layout (component_count, component_size_quads, subsection_size_quads, num_subsections). The first call of any landscape task -- it gives you the actor name the other landscape tools need, and the numbers to convert a world XY into a heightmap vertex. Also the ONLY trustworthy check of where the terrain actually is: the editor's New Landscape dialog treats its Location field as the CENTRE, so a terrain can sit half a world away from the corner you typed; read `location` here afterwards instead of believing the input. Note it lists ALandscape actors only, not the LandscapeStreamingProxy actors a World-Partition map renders through.
//   @returns { landscapes: [{ actor_name, actor_path, location: [x,y,z], scale: [x,y,z], component_size_quads, subsection_size_quads, num_subsections, min_x, min_y, max_x, max_y, size_x, size_y, component_count }], landscape_count: int }
TSharedPtr<FJsonObject> HandleGetLandscapeInfo(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TArray<TSharedPtr<FJsonValue>> Landscapes;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        ALandscape* Landscape = *It;
        if (!Landscape)
        {
            continue;
        }

        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("actor_name"), Landscape->GetActorLabel());
        Obj->SetStringField(TEXT("actor_path"), Landscape->GetPathName());

        // Data set at creation time (ALandscapeProxy members, UE 5.7).
        Obj->SetNumberField(TEXT("component_size_quads"), Landscape->ComponentSizeQuads);
        Obj->SetNumberField(TEXT("subsection_size_quads"), Landscape->SubsectionSizeQuads);
        Obj->SetNumberField(TEXT("num_subsections"), Landscape->NumSubsections);

        const FVector Scale = Landscape->GetActorScale3D();
        Obj->SetArrayField(TEXT("scale"), mcp_common::VectorToJsonArray(Scale));

        const FVector Loc = Landscape->GetActorLocation();
        Obj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Loc));

        // Component count + overall size from the shared ULandscapeInfo, if present.
        int32 ComponentCount = 0;
        if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
        {
            int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
            if (Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
            {
                Obj->SetNumberField(TEXT("min_x"), MinX);
                Obj->SetNumberField(TEXT("min_y"), MinY);
                Obj->SetNumberField(TEXT("max_x"), MaxX);
                Obj->SetNumberField(TEXT("max_y"), MaxY);
                Obj->SetNumberField(TEXT("size_x"), (MaxX - MinX) + 1);
                Obj->SetNumberField(TEXT("size_y"), (MaxY - MinY) + 1);
            }
            ComponentCount = Info->XYtoComponentMap.Num();
        }
        Obj->SetNumberField(TEXT("component_count"), ComponentCount);

        Landscapes.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("landscapes"), Landscapes);
    ResultObj->SetNumberField(TEXT("landscape_count"), Landscapes.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// Foliage
// ========================================================================== //

// MCP:create_foliage_type - Create and save a FoliageType asset (UFoliageType_InstancedStaticMesh) wrapping a static mesh, so it can be placed as foliage and tuned with set_foliage_type_properties. Creating the asset is safe; note that the tools that PLACE instances of it (add_foliage_instances, scatter_foliage_in_box, scatter_foliage_on_layer) can crash the editor, so plan to scatter via instanced-static-mesh components or PCG. For landscape GRASS driven by the terrain material use create_landscape_grass_type instead.
//   @param name - Asset name, e.g. FT_Grass
//   @param package_path - Target content path, defaults to /Game/Foliage
//   @param static_mesh_path - Full path of the source StaticMesh
//   @returns { name: string, package_path: string, asset_path: string, static_mesh: string, foliage_class: string, created: bool, already_exists: bool, mesh_set: bool, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateFoliageType(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    FString AssetName;
    FString StaticMeshPath;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/Foliage");
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("static_mesh_path"), StaticMeshPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UStaticMesh* Mesh = Cast<UStaticMesh>(StaticLoadObject(UStaticMesh::StaticClass(), nullptr, *StaticMeshPath));
    if (!Mesh)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("StaticMesh not found: %s"), *StaticMeshPath));
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load + (re)assign the mesh and return.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UFoliageType_InstancedStaticMesh* Existing =
            Cast<UFoliageType_InstancedStaticMesh>(StaticLoadObject(UFoliageType_InstancedStaticMesh::StaticClass(), nullptr, *FullPath));
        if (Existing)
        {
            Existing->Modify();
            Existing->Mesh = Mesh;

            FString SaveErr;
            const bool bSaved = mcp_common::SaveAssetPackage(Existing->GetOutermost(), Existing, SaveErr);

            TSharedPtr<FJsonObject> ExistObj = Landscape_NewAssetData(AssetName, PackagePath, Existing);
            ExistObj->SetStringField(TEXT("static_mesh"), Existing->Mesh ? Existing->Mesh->GetPathName() : TEXT("None"));
            ExistObj->SetStringField(TEXT("foliage_class"), Existing->GetClass()->GetName());
            ExistObj->SetBoolField(TEXT("created"), false);
            ExistObj->SetBoolField(TEXT("already_exists"), true);
            ExistObj->SetBoolField(TEXT("mesh_set"), Existing->Mesh == Mesh);
            ExistObj->SetBoolField(TEXT("saved"), bSaved);
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset already exists at %s but is not a FoliageType"), *FullPath));
    }

    // A real /Game package + foliage type object inside it (direct NewObject
    // mirrors create_data_asset; UFoliageType_InstancedStaticMesh has no inner
    // objects and its factory would otherwise require config).
    FString CreateError;
    UFoliageType_InstancedStaticMesh* FoliageType = mcp_common::NewAssetInPackage<UFoliageType_InstancedStaticMesh>(
        FullPath, AssetName, TEXT("Failed to create UFoliageType_InstancedStaticMesh"), CreateError);
    if (!FoliageType)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FoliageType->Mesh = Mesh;

    FAssetRegistryModule::AssetCreated(FoliageType);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(FoliageType->GetOutermost(), FoliageType, SaveError);

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewAssetData(AssetName, PackagePath, FoliageType);
    ResultObj->SetStringField(TEXT("static_mesh"), FoliageType->Mesh ? FoliageType->Mesh->GetPathName() : TEXT("None"));
    ResultObj->SetStringField(TEXT("foliage_class"), FoliageType->GetClass()->GetName());
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_exists"), false);
    ResultObj->SetBoolField(TEXT("mesh_set"), FoliageType->Mesh == Mesh);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? TEXT("FoliageType created and saved to disk")
        : FString::Printf(TEXT("FoliageType created but save failed: %s"), *SaveError));

    return mcp_common::MakeOkReply(ResultObj);
}

// Read-back helper shared by set/get_foliage_type_properties.
static void MakeFoliageTypePropsJson(UFoliageType* FT, TSharedPtr<FJsonObject>& Data)
{
    Data->SetStringField(TEXT("asset_path"), FT->GetPathName());
    Data->SetStringField(TEXT("foliage_class"), FT->GetClass()->GetName());
    Data->SetNumberField(TEXT("density"), FT->Density);
    Data->SetNumberField(TEXT("density_adjustment_factor"), FT->DensityAdjustmentFactor);
    Data->SetNumberField(TEXT("radius"), FT->Radius);

    Data->SetObjectField(TEXT("scale_x"), Landscape_IntervalToJson(FT->ScaleX));
    Data->SetObjectField(TEXT("scale_y"), Landscape_IntervalToJson(FT->ScaleY));
    Data->SetObjectField(TEXT("scale_z"), Landscape_IntervalToJson(FT->ScaleZ));
    Data->SetObjectField(TEXT("z_offset"), Landscape_IntervalToJson(FT->ZOffset));
    Data->SetObjectField(TEXT("cull_distance"), Landscape_IntervalToJson(FT->CullDistance));

    Data->SetStringField(TEXT("mobility"), mcp_common::MobilityLabel(FT->Mobility.GetValue()));

    Data->SetBoolField(TEXT("align_to_normal"), FT->AlignToNormal != 0);
    Data->SetBoolField(TEXT("random_yaw"), FT->RandomYaw != 0);
    Data->SetBoolField(TEXT("collision_with_world"), FT->CollisionWithWorld != 0);
    Data->SetBoolField(TEXT("cast_shadow"), FT->CastShadow != 0);
    Data->SetBoolField(TEXT("affect_dynamic_indirect_lighting"), FT->bAffectDynamicIndirectLighting != 0);
}

// MCP:set_foliage_type_properties - Set tunables of a FoliageType_InstancedStaticMesh asset in one call, only the fields you pass, then save it. Returns the read-back values with applied_count and saved; an invalid mobility fails the call. Edits the asset only: instances already placed keep their transforms. Read it with get_foliage_type_properties.
//   @param foliage_path - Asset path of a UFoliageType_InstancedStaticMesh, e.g. "/Game/Foliage/FT_Example"; actor foliage types are not found.
//   @param density - Instances per 1000 x 1000 uu area (10 m by 10 m) when painting.
//   @param density_adjustment_factor - Density multiplier used when reapplying: above 1 adds instances, below 1 removes them.
//   @param radius - Minimum distance in cm between foliage instances.
//   @param scale_x [object] - X scale range as a JSON object {"min":0.8,"max":1.2}, either bound optional.
//   @param scale_y [object] - Y scale range as a JSON object {"min":0.8,"max":1.2}, either bound optional.
//   @param scale_z [object] - Z scale range as a JSON object {"min":0.8,"max":1.2}, either bound optional.
//   @param z_offset [object] - Z location offset range in cm as a JSON object {"min":-5,"max":0}; either bound may be left out.
//   @param cull_distance - Integer interval {"min":start,"max":end} in cm: instances fade from min (only with a PerInstanceFadeAmount material node) and clusters cull beyond max; 0 disables.
//   @param mobility - Component mobility: Static, Stationary or Movable, case-insensitive; any other value fails the call.
//   @param align_to_normal - Tilt instances to the normal of the surface they are placed on.
//   @param random_yaw - Give each instance a random yaw around its vertical axis.
//   @param collision_with_world - Test for overlap with world geometry before placing each instance.
//   @param cast_shadow - Whether the foliage components cast shadows.
//   @param affect_dynamic_indirect_lighting - Whether the foliage injects light into dynamic indirect lighting; used only when cast_shadow is true.
//   @returns { asset_path, foliage_class, density, density_adjustment_factor, radius, scale_x:{min,max}, scale_y, scale_z, z_offset, cull_distance:{min,max}, mobility, align_to_normal, random_yaw, collision_with_world, cast_shadow, affect_dynamic_indirect_lighting, applied_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetFoliageTypeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString FoliagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("foliage_path"), FoliagePath)) { return Missing; }

    UFoliageType_InstancedStaticMesh* FT =
        Cast<UFoliageType_InstancedStaticMesh>(StaticLoadObject(UFoliageType_InstancedStaticMesh::StaticClass(), nullptr, *FoliagePath));
    if (!FT)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("FoliageType not found: %s"), *FoliagePath));
    }

    FT->Modify();
    int32 Applied = 0;

    double Num = 0.0;
    if (Params->TryGetNumberField(TEXT("density"), Num)) { FT->Density = (float)Num; ++Applied; }
    if (Params->TryGetNumberField(TEXT("density_adjustment_factor"), Num)) { FT->DensityAdjustmentFactor = (float)Num; ++Applied; }
    if (Params->TryGetNumberField(TEXT("radius"), Num)) { FT->Radius = (float)Num; ++Applied; }

    if (Landscape_ReadIntervalParam(Params, TEXT("scale_x"), FT->ScaleX)) { ++Applied; }
    if (Landscape_ReadIntervalParam(Params, TEXT("scale_y"), FT->ScaleY)) { ++Applied; }
    if (Landscape_ReadIntervalParam(Params, TEXT("scale_z"), FT->ScaleZ)) { ++Applied; }
    if (Landscape_ReadIntervalParam(Params, TEXT("z_offset"), FT->ZOffset)) { ++Applied; }

    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (Params->TryGetObjectField(TEXT("cull_distance"), Obj) && Obj && Obj->IsValid())
        {
            double M = 0.0;
            if ((*Obj)->TryGetNumberField(TEXT("min"), M)) { FT->CullDistance.Min = (int32)M; }
            if ((*Obj)->TryGetNumberField(TEXT("max"), M)) { FT->CullDistance.Max = (int32)M; }
            ++Applied;
        }
    }

    FString MobStr;
    if (Params->TryGetStringField(TEXT("mobility"), MobStr) && !MobStr.IsEmpty())
    {
        if (MobStr.Equals(TEXT("Static"), ESearchCase::IgnoreCase)) { FT->Mobility = EComponentMobility::Static; ++Applied; }
        else if (MobStr.Equals(TEXT("Stationary"), ESearchCase::IgnoreCase)) { FT->Mobility = EComponentMobility::Stationary; ++Applied; }
        else if (MobStr.Equals(TEXT("Movable"), ESearchCase::IgnoreCase)) { FT->Mobility = EComponentMobility::Movable; ++Applied; }
        else { return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid mobility '%s' (expected Static/Stationary/Movable)"), *MobStr)); }
    }

    bool b = false;
    if (Params->TryGetBoolField(TEXT("align_to_normal"), b)) { FT->AlignToNormal = b ? 1 : 0; ++Applied; }
    if (Params->TryGetBoolField(TEXT("random_yaw"), b)) { FT->RandomYaw = b ? 1 : 0; ++Applied; }
    if (Params->TryGetBoolField(TEXT("collision_with_world"), b)) { FT->CollisionWithWorld = b ? 1 : 0; ++Applied; }
    if (Params->TryGetBoolField(TEXT("cast_shadow"), b)) { FT->CastShadow = b ? 1 : 0; ++Applied; }
    if (Params->TryGetBoolField(TEXT("affect_dynamic_indirect_lighting"), b)) { FT->bAffectDynamicIndirectLighting = b ? 1 : 0; ++Applied; }

    FT->PostEditChange();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(FT->GetOutermost(), FT, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    MakeFoliageTypePropsJson(FT, Data);
    Data->SetNumberField(TEXT("applied_count"), Applied);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_foliage_type_properties"), SaveError);
}

// MCP:get_foliage_type_properties - Read a FoliageType asset's authored tunables in one call -- density, radius, scale and z-offset ranges, cull distances, mobility, align-to-normal, random yaw, world collision, shadow and indirect-lighting flags. The read-back oracle for set_foliage_type_properties, and cheaper than reflection-walking the asset from Python. Describes the ASSET, not what is placed in the level: for that use get_foliage_instance_count / get_foliage_in_radius.
//   @param foliage_path - Asset path of the UFoliageType
//   @returns { asset_path, foliage_class, density, density_adjustment_factor, radius, scale_x:{min,max}, scale_y, scale_z, z_offset, cull_distance:{min,max}, mobility, align_to_normal, random_yaw, collision_with_world, cast_shadow, affect_dynamic_indirect_lighting }
TSharedPtr<FJsonObject> HandleGetFoliageTypeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString FoliagePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("foliage_path"), FoliagePath)) { return Missing; }

    UFoliageType_InstancedStaticMesh* FT =
        Cast<UFoliageType_InstancedStaticMesh>(StaticLoadObject(UFoliageType_InstancedStaticMesh::StaticClass(), nullptr, *FoliagePath));
    if (!FT)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("FoliageType not found: %s"), *FoliagePath));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    MakeFoliageTypePropsJson(FT, Data);
    return mcp_common::MakeOkReply(Data);
}

namespace
{
    // The editor world and the foliage type a foliage command works on.
    struct FLandscapeFoliageTarget
    {
        UWorld* World = nullptr;
        FString Path;
        UFoliageType* Type = nullptr;
    };

    // The editor world and the required "foliage_type_path" foliage type, into Out; null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_ResolveFoliageType(const TSharedPtr<FJsonObject>& Params, FLandscapeFoliageTarget& Out)
    {
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(Out.World)) { return NoWorld; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("foliage_type_path"), Out.Path, mcp_common::EAllowEmpty::Yes))
        {
            return Missing;
        }
        Out.Type = Cast<UFoliageType>(StaticLoadObject(UFoliageType::StaticClass(), nullptr, *Out.Path));
        if (!Out.Type)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("FoliageType not found: %s"), *Out.Path));
        }
        return nullptr;
    }

    // A reply payload naming the foliage type: foliage_type_path and foliage_type.
    TSharedPtr<FJsonObject> Landscape_NewFoliageData(const FLandscapeFoliageTarget& Foliage)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("foliage_type_path"), Foliage.Path);
        Data->SetStringField(TEXT("foliage_type"), Foliage.Type->GetName());
        return Data;
    }

    // A seeded random point in the box Center +- Extent; on the box top instead of at a random Z when bOnTop.
    FVector Landscape_RandomPointInBox(FRandomStream& Stream, const FVector& Center, const FVector& Extent, bool bOnTop)
    {
        const double X = Center.X + Stream.FRandRange(-Extent.X, Extent.X);
        const double Y = Center.Y + Stream.FRandRange(-Extent.Y, Extent.Y);
        const double Z = bOnTop
            ? (Center.Z + Extent.Z)
            : (Center.Z + Stream.FRandRange(-Extent.Z, Extent.Z));
        return FVector(X, Y, Z);
    }

    // The "center_x/y/z" and required positive "radius" sphere; null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_ReadFoliageSphere(const TSharedPtr<FJsonObject>& Params, FVector& OutCenter, double& OutRadius)
    {
        double CenterX = 0.0, CenterY = 0.0, CenterZ = 0.0;
        Params->TryGetNumberField(TEXT("center_x"), CenterX);
        Params->TryGetNumberField(TEXT("center_y"), CenterY);
        Params->TryGetNumberField(TEXT("center_z"), CenterZ);
        OutCenter = FVector(CenterX, CenterY, CenterZ);

        if (!Params->TryGetNumberField(TEXT("radius"), OutRadius) || OutRadius <= 0.0)
        {
            return mcp_common::MakeErrorReply(TEXT("Missing or invalid parameter: radius (must be > 0)"));
        }
        return nullptr;
    }

    // How many instances of FoliageType IFA holds; 0 without an IFA or a foliage info for the type.
    int32 Landscape_FoliageInstanceCount(AInstancedFoliageActor* IFA, UFoliageType* FoliageType)
    {
        if (IFA)
        {
            if (const FFoliageInfo* Info = IFA->FindInfo(FoliageType))
            {
                return Info->Instances.Num();
            }
        }
        return 0;
    }

    // Adds one instance per transform through the exported FindOrAddMesh + FFoliageInfo::AddInstance (the
    // static AInstancedFoliageActor::AddInstances lacks FOLIAGE_API); false when there is no IFA or info.
    bool Landscape_AddFoliageInstances(AInstancedFoliageActor* IFA, UFoliageType* FoliageType, const TArray<FTransform>& Transforms)
    {
        FFoliageInfo* Info = IFA ? IFA->FindOrAddMesh(FoliageType) : nullptr;
        if (!Info)
        {
            return false;
        }
        for (const FTransform& T : Transforms)
        {
            FFoliageInstance Inst;
            Inst.Location = T.GetTranslation();
            Inst.Rotation = T.Rotator();
            Inst.DrawScale3D = FVector3f(T.GetScale3D());
            Info->AddInstance(FoliageType, Inst);
        }
        return true;
    }

    // The persistent level's existing foliage actor and, when it holds FoliageType, its instance counts.
    AInstancedFoliageActor* Landscape_ReadBackFoliage(UWorld* World, UFoliageType* FoliageType, int32& OutCount, int32& OutPlacedCount)
    {
        AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone*/ false, World->PersistentLevel);
        if (IFA)
        {
            if (FFoliageInfo* Info = IFA->FindInfo(FoliageType))
            {
                OutCount = Info->Instances.Num();
                OutPlacedCount = Info->GetPlacedInstanceCount();
            }
        }
        return IFA;
    }
}

// MCP:add_foliage_instances - Place foliage instances of a UFoliageType at explicit transforms in the level's InstancedFoliageActor. DANGER: measured CRASHING the editor process (access violation inside UnrealEditor-FoliageEdit.dll) -- reproducibly, and a single instance crashes just like a thousand, so trimming the count does not help. The reply you see is a "Connection reset", which reads like a network hiccup and is not. Use add_instanced_static_mesh_component + add_ism_instance instead: an InstancedFoliageActor is itself an actor holding instanced-static-mesh components, so it is the same code path minus the crash. Save your work before calling this at all.
//   @param foliage_type_path - Full path of the UFoliageType asset
//   @param transforms - List of [x,y,z] locations or objects with location, rotation, scale
//   @returns { foliage_type_path: string, foliage_type: string, requested: int, instances_added: int, instance_count_before: int, instance_count: int, placed_instance_count: int, instances_added_ok: bool, foliage_actor: string }
TSharedPtr<FJsonObject> HandleAddFoliageInstances(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    // Parse transforms. Accept either [[x,y,z], ...] (location only) or
    // [{location:[x,y,z], rotation:[p,y,r], scale:[x,y,z]}, ...].
    const TArray<TSharedPtr<FJsonValue>>* TransformsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("transforms"), TransformsArr) || !TransformsArr)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameter: transforms (array of [x,y,z] or {location,rotation,scale})"));
    }

    auto ReadVec3 = [](const TArray<TSharedPtr<FJsonValue>>* Arr, const FVector& Default) -> FVector
    {
        if (Arr && Arr->Num() >= 3)
        {
            return FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
        }
        return Default;
    };

    TArray<FTransform> Transforms;
    Transforms.Reserve(TransformsArr->Num());
    for (const TSharedPtr<FJsonValue>& Entry : *TransformsArr)
    {
        if (!Entry.IsValid())
        {
            continue;
        }

        if (Entry->Type == EJson::Array)
        {
            const TArray<TSharedPtr<FJsonValue>>& A = Entry->AsArray();
            const FVector Loc = ReadVec3(&A, FVector::ZeroVector);
            Transforms.Add(FTransform(FRotator::ZeroRotator, Loc, FVector::OneVector));
        }
        else if (Entry->Type == EJson::Object)
        {
            const TSharedPtr<FJsonObject> Obj = Entry->AsObject();
            FVector Loc = FVector::ZeroVector;
            FRotator Rot = FRotator::ZeroRotator;
            FVector Scale = FVector::OneVector;

            const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
            if (Obj->TryGetArrayField(TEXT("location"), LocArr))
            {
                Loc = ReadVec3(LocArr, FVector::ZeroVector);
            }
            const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
            if (Obj->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
            {
                // [pitch, yaw, roll]
                Rot = FRotator((*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
            }
            const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
            if (Obj->TryGetArrayField(TEXT("scale"), ScaleArr))
            {
                Scale = ReadVec3(ScaleArr, FVector::OneVector);
            }
            Transforms.Add(FTransform(Rot, Loc, Scale));
        }
    }

    if (Transforms.Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("No valid transforms parsed from 'transforms'"));
    }

    // Count existing instances of this foliage type in the persistent level's IFA
    // BEFORE the add, so we can verify the delta by readback.
    AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(Foliage.World, /*bCreateIfNone*/ true, Foliage.World->PersistentLevel);
    if (IFA)
    {
        IFA->Modify();
    }
    const int32 CountBefore = Landscape_FoliageInstanceCount(IFA, Foliage.Type);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddFoliageInstances", "Add Foliage Instances (MCP)"));
    Landscape_AddFoliageInstances(IFA, Foliage.Type, Transforms);

    // Verify by readback: re-resolve the IFA + foliage info and read the count.
    int32 CountAfter = 0;
    int32 PlacedCount = 0;
    AInstancedFoliageActor* IFAAfter = Landscape_ReadBackFoliage(Foliage.World, Foliage.Type, CountAfter, PlacedCount);

    const int32 Added = CountAfter - CountBefore;

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewFoliageData(Foliage);
    ResultObj->SetNumberField(TEXT("requested"), Transforms.Num());
    ResultObj->SetNumberField(TEXT("instances_added"), Added);
    ResultObj->SetNumberField(TEXT("instance_count_before"), CountBefore);
    ResultObj->SetNumberField(TEXT("instance_count"), CountAfter);
    ResultObj->SetNumberField(TEXT("placed_instance_count"), PlacedCount);
    ResultObj->SetBoolField(TEXT("instances_added_ok"), Added >= Transforms.Num());
    if (IFAAfter)
    {
        ResultObj->SetStringField(TEXT("foliage_actor"), IFAAfter->GetActorLabel());
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_foliage_types - List every FoliageType actually present in the level's InstancedFoliageActors, with the instance counts each carries. The level-side inventory ("what vegetation is in this map, and how much"), as opposed to get_foliage_type_properties which describes one asset. Nothing here means nothing has been placed, even if the FoliageType assets exist in Content.
//   @returns { foliage_types: [object], foliage_type_count: int, foliage_actor_count: int }
TSharedPtr<FJsonObject> HandleGetFoliageTypes(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    TArray<TSharedPtr<FJsonValue>> Types;
    int32 IFACount = 0;

    // Enumerate every InstancedFoliageActor in the editor world and list the
    // foliage types it owns. GetFoliageInfos() is always available (returns the
    // FoliageType -> FFoliageInfo map).
    for (TActorIterator<AInstancedFoliageActor> It(World); It; ++It)
    {
        AInstancedFoliageActor* IFA = *It;
        if (!IFA)
        {
            continue;
        }
        ++IFACount;

        for (const TPair<UFoliageType*, TUniqueObj<FFoliageInfo>>& Pair : IFA->GetFoliageInfos())
        {
            UFoliageType* FoliageType = Pair.Key;
            if (!FoliageType)
            {
                continue;
            }
            const FFoliageInfo& Info = Pair.Value.Get();

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("foliage_type"), FoliageType->GetName());
            Obj->SetStringField(TEXT("foliage_type_path"), FoliageType->GetPathName());
            Obj->SetStringField(TEXT("foliage_class"), FoliageType->GetClass()->GetName());
            Obj->SetBoolField(TEXT("is_asset"), FoliageType->IsAsset());

            // Source mesh (UFoliageType_InstancedStaticMesh exposes Mesh; the
            // generic source is available via GetSource()).
            FString MeshPath = TEXT("None");
            if (UFoliageType_InstancedStaticMesh* ISM = Cast<UFoliageType_InstancedStaticMesh>(FoliageType))
            {
                if (ISM->Mesh)
                {
                    MeshPath = ISM->Mesh->GetPathName();
                }
            }
            else if (UObject* Source = FoliageType->GetSource())
            {
                MeshPath = Source->GetPathName();
            }
            Obj->SetStringField(TEXT("static_mesh"), MeshPath);

            Obj->SetNumberField(TEXT("instance_count"), Info.Instances.Num());
            Obj->SetStringField(TEXT("foliage_actor"), IFA->GetActorLabel());

            Types.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("foliage_types"), Types);
    ResultObj->SetNumberField(TEXT("foliage_type_count"), Types.Num());
    ResultObj->SetNumberField(TEXT("foliage_actor_count"), IFACount);

    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// Foliage scatter
// ========================================================================== //

// MCP:scatter_foliage_in_box - Scatter N seeded-random foliage instances of a type inside a world-space box (same seed = same layout). DANGER: measured CRASHING the editor process, the identical access violation as add_foliage_instances (UnrealEditor-FoliageEdit.dll) because both end in the same FFoliageInfo::AddInstance path -- lowering count does not help and the failure arrives disguised as "Connection reset". Use add_instanced_static_mesh_component + add_ism_instance to scatter instead, or PCG for rule-driven placement. Save your work before calling this at all.
//   @param foliage_type_path - Full path of the UFoliageType asset
//   @param count [integer!] - Number of instances to scatter, default 8
//   @param seed - Seed for the random stream, default 1337
//   @param center - [x, y, z] box center in world space, default [0,0,0]
//   @param extent - [x, y, z] box half-extents, default [500,500,0]
//   @param align_to_ground - If true, place at the box top instead of random Z
//   @returns { foliage_type_path: string, foliage_type: string, requested: int, seed: int, align_to_ground: bool, instances_added: int, instance_count_before: int, total_instance_count: int, placed_instance_count: int, instances_added_ok: bool, center: [number], extent: [number], foliage_actor: string }
TSharedPtr<FJsonObject> HandleScatterFoliageInBox(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    // center / extent (half-size of the box) ------------------------------- //
    const FVector Center = Params->HasField(TEXT("center"))
        ? mcp_common::ReadJsonVector(Params, TEXT("center"))
        : FVector::ZeroVector;
    const FVector Extent = Params->HasField(TEXT("extent"))
        ? mcp_common::ReadJsonVector(Params, TEXT("extent"))
        : FVector(500.0, 500.0, 0.0);

    const int32 Count = FMath::Max(0, ParamInt(Params, TEXT("count"), 8));
    const int32 Seed  = ParamInt(Params, TEXT("seed"), 1337);

    // When true, instances are placed at the box top (Center.Z + Extent.Z) so a
    // subsequent ground-snap is meaningful; default places at the box center Z.
    bool bAlignToGround = false;
    Params->TryGetBoolField(TEXT("align_to_ground"), bAlignToGround);

    if (Count == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("count must be >= 1"));
    }

    // Count BEFORE so the delta can be verified by readback. --------------- //
    AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(Foliage.World, /*bCreateIfNone*/ true, Foliage.World->PersistentLevel);
    if (IFA)
    {
        IFA->Modify();
    }
    const int32 CountBefore = Landscape_FoliageInstanceCount(IFA, Foliage.Type);

    // Generate 'Count' seeded random transforms inside the box. ------------ //
    FRandomStream RandomStream(Seed);
    TArray<FTransform> Transforms;
    Transforms.Reserve(Count);
    for (int32 i = 0; i < Count; ++i)
    {
        const FVector Location = Landscape_RandomPointInBox(RandomStream, Center, Extent, bAlignToGround);

        // Random yaw [0,360) keeps scattered instances visually varied.
        const double Yaw = RandomStream.FRandRange(0.0, 360.0);
        Transforms.Add(FTransform(FRotator(0.0, Yaw, 0.0), Location, FVector::OneVector));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ScatterFoliageInBox", "Scatter Foliage In Box (MCP)"));
    Landscape_AddFoliageInstances(IFA, Foliage.Type, Transforms);

    // Verify by readback. -------------------------------------------------- //
    int32 CountAfter = 0;
    int32 PlacedCount = 0;
    AInstancedFoliageActor* IFAAfter = Landscape_ReadBackFoliage(Foliage.World, Foliage.Type, CountAfter, PlacedCount);

    const int32 Added = CountAfter - CountBefore;

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewFoliageData(Foliage);
    ResultObj->SetNumberField(TEXT("requested"), Count);
    ResultObj->SetNumberField(TEXT("seed"), Seed);
    ResultObj->SetBoolField(TEXT("align_to_ground"), bAlignToGround);
    ResultObj->SetNumberField(TEXT("instances_added"), Added);
    ResultObj->SetNumberField(TEXT("instance_count_before"), CountBefore);
    ResultObj->SetNumberField(TEXT("total_instance_count"), CountAfter);
    ResultObj->SetNumberField(TEXT("placed_instance_count"), PlacedCount);
    ResultObj->SetBoolField(TEXT("instances_added_ok"), Added >= Count);

    ResultObj->SetArrayField(TEXT("center"), mcp_common::VectorToJsonArray(Center));

    ResultObj->SetArrayField(TEXT("extent"), mcp_common::VectorToJsonArray(Extent));

    if (IFAAfter)
    {
        ResultObj->SetStringField(TEXT("foliage_actor"), IFAAfter->GetActorLabel());
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remove_all_foliage_for_type - Delete EVERY placed instance of one FoliageType from all InstancedFoliageActors in the level -- the bulk undo for a scatter that went wrong. Destructive and not per-region; use remove_foliage_in_radius to clear only around a point. The FoliageType asset itself is untouched. Check remaining=0 in the reply, not just removed_all.
//   @param foliage_type_path - Full path of the UFoliageType asset
//   @returns { foliage_type_path: string, foliage_type: string, removed_count: int, remaining: int, foliage_actors_affected: int, removed_all: bool }
TSharedPtr<FJsonObject> HandleRemoveAllFoliageForType(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveAllFoliageForType", "Remove All Foliage For Type (MCP)"));

    // Remove instances of this type from EVERY IFA in the editor world. ---- //
    int32 RemovedCount = 0;
    int32 IFACount = 0;
    for (TActorIterator<AInstancedFoliageActor> It(Foliage.World); It; ++It)
    {
        AInstancedFoliageActor* IFA = *It;
        if (!IFA)
        {
            continue;
        }

        FFoliageInfo* Info = IFA->FindInfo(Foliage.Type);
        if (!Info)
        {
            continue;
        }

        const int32 Num = Info->Instances.Num();
        if (Num == 0)
        {
            continue;
        }

        ++IFACount;
        IFA->Modify();

        // Build the full index list [0 .. Num-1] and remove in one call.
        // FFoliageInfo::RemoveInstances(TArrayView<const int32>, bool RebuildFoliageTree).
        TArray<int32> AllIndices;
        AllIndices.Reserve(Num);
        for (int32 i = 0; i < Num; ++i)
        {
            AllIndices.Add(i);
        }
        Info->RemoveInstances(AllIndices, /*RebuildFoliageTree*/ true);
        RemovedCount += Num;
    }

    // Verify by readback: count remaining instances across all IFAs. ------- //
    int32 Remaining = 0;
    for (TActorIterator<AInstancedFoliageActor> It(Foliage.World); It; ++It)
    {
        AInstancedFoliageActor* IFA = *It;
        if (!IFA)
        {
            continue;
        }
        if (FFoliageInfo* Info = IFA->FindInfo(Foliage.Type))
        {
            Remaining += Info->Instances.Num();
        }
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewFoliageData(Foliage);
    ResultObj->SetNumberField(TEXT("removed_count"), RemovedCount);
    ResultObj->SetNumberField(TEXT("remaining"), Remaining);
    ResultObj->SetNumberField(TEXT("foliage_actors_affected"), IFACount);
    ResultObj->SetBoolField(TEXT("removed_all"), Remaining == 0);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_foliage_instance_count - Count how many instances of one FoliageType are placed across the level's InstancedFoliageActors. The cheapest oracle for "did that scatter place anything / did the removal clear it" -- one number, no transform payload. Use get_foliage_in_radius when you need WHERE they are, has_foliage_instances for a plain yes/no.
//   @param foliage_type_path - Full path of the UFoliageType asset
//   @returns { foliage_type_path: string, foliage_type: string, instance_count: int, placed_instance_count: int, foliage_actor_count: int }
TSharedPtr<FJsonObject> HandleGetFoliageInstanceCount(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    // Sum the instance counts of this type across every IFA in the world. -- //
    int32 InstanceCount = 0;
    int32 PlacedCount = 0;
    int32 IFACount = 0;
    for (TActorIterator<AInstancedFoliageActor> It(Foliage.World); It; ++It)
    {
        AInstancedFoliageActor* IFA = *It;
        if (!IFA)
        {
            continue;
        }
        if (FFoliageInfo* Info = IFA->FindInfo(Foliage.Type))
        {
            ++IFACount;
            InstanceCount += Info->Instances.Num();
            PlacedCount += Info->GetPlacedInstanceCount();
        }
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewFoliageData(Foliage);
    ResultObj->SetNumberField(TEXT("instance_count"), InstanceCount);
    ResultObj->SetNumberField(TEXT("placed_instance_count"), PlacedCount);
    ResultObj->SetNumberField(TEXT("foliage_actor_count"), IFACount);

    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// Landscape layers + paint
// ========================================================================== //

// MCP:create_landscape_layer_info - Create and save the LandscapeLayerInfo asset that a terrain paint layer needs before it can hold weight (grass, rock, sand, snow...). Step 1 of three: create the asset here, register it on the terrain with add_landscape_layer, then paint weight with paint_landscape_layer. Idempotent-ish -- an existing asset comes back with already_exists=true.
//   @param name - Asset name, e.g. LI_Grass
//   @param package_path - Target content path, defaults to /Game/Landscape
//   @param layer_name - Engine-side weight layer name, defaults to asset name
//   @returns { name: string, package_path: string, asset_path: string, layer_name: string, layer_info_class: string, created: bool, already_exists: bool, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateLandscapeLayerInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    FString AssetName;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/Landscape");
    }

    // LayerName is the engine-side weight layer identifier; default to the asset
    // name when not supplied.
    FString LayerName;
    if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
    {
        LayerName = AssetName;
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: load + re-set the LayerName if the asset already exists.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        ULandscapeLayerInfoObject* Existing =
            Cast<ULandscapeLayerInfoObject>(StaticLoadObject(ULandscapeLayerInfoObject::StaticClass(), nullptr, *FullPath));
        if (Existing)
        {
            // SetLayerName(FName, bModify) — LayerName property is deprecated-public in 5.7.
            Existing->SetLayerName(FName(*LayerName), /*bInModify*/ true);

            FString SaveErr;
            const bool bSaved = mcp_common::SaveAssetPackage(Existing->GetOutermost(), Existing, SaveErr);

            TSharedPtr<FJsonObject> ExistObj = Landscape_NewAssetData(AssetName, PackagePath, Existing);
            ExistObj->SetStringField(TEXT("layer_name"), Existing->GetLayerName().ToString());
            ExistObj->SetBoolField(TEXT("created"), false);
            ExistObj->SetBoolField(TEXT("already_exists"), true);
            ExistObj->SetBoolField(TEXT("saved"), bSaved);
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset already exists at %s but is not a LandscapeLayerInfoObject"), *FullPath));
    }

    // A real /Game package + ULandscapeLayerInfoObject inside it. The class
    // is UCLASS(MinimalAPI) so NewObject + StaticClass() are exported.
    FString CreateError;
    ULandscapeLayerInfoObject* LayerInfo = mcp_common::NewAssetInPackage<ULandscapeLayerInfoObject>(
        FullPath, AssetName, TEXT("Failed to create ULandscapeLayerInfoObject"), CreateError);
    if (!LayerInfo)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    LayerInfo->SetLayerName(FName(*LayerName), /*bInModify*/ false);

    FAssetRegistryModule::AssetCreated(LayerInfo);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(LayerInfo->GetOutermost(), LayerInfo, SaveError);

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewAssetData(AssetName, PackagePath, LayerInfo);
    ResultObj->SetStringField(TEXT("layer_name"), LayerInfo->GetLayerName().ToString());
    ResultObj->SetStringField(TEXT("layer_info_class"), LayerInfo->GetClass()->GetName());
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_exists"), false);
    ResultObj->SetBoolField(TEXT("saved"), bSaved);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? TEXT("LandscapeLayerInfoObject created and saved to disk")
        : FString::Printf(TEXT("LandscapeLayerInfoObject created but save failed: %s"), *SaveError));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:add_landscape_layer - Register a LandscapeLayerInfo asset as a target weight (paint) layer on a landscape, so the terrain can store weight for it and the material's LandscapeLayerBlend can read it. Step 2 of three, between create_landscape_layer_info and paint_landscape_layer -- painting a layer that was never registered writes nothing. Already-registered layers return already_present=true; remove_landscape_layer is the inverse, get_landscape_layers lists what is registered now.
//   @param layer_info_path - Full path of the ULandscapeLayerInfoObject asset
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @returns { landscape_actor_name: string, layer_info_path: string, layer_name: string, layer_added: bool, already_present: bool, target_layer_count: int }
TSharedPtr<FJsonObject> HandleAddLandscapeLayer(const TSharedPtr<FJsonObject>& Params)
{
    FString LayerInfoPath;
    ULandscapeLayerInfoObject* LayerInfo = nullptr;
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveLayerTarget(Params, LayerInfoPath, LayerInfo, Landscape))
    {
        return Error;
    }

    const FName TargetLayerName = LayerInfo->GetLayerName();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddLandscapeLayer", "Add Landscape Layer (MCP)"));
    Landscape->Modify();

    // ALandscapeProxy::AddTargetLayer(FName, FLandscapeTargetLayerSettings, bool).
    // ALandscape derives from ALandscapeProxy. Keyed by the layer info's LayerName,
    // mirroring ULandscapeInfo::CreateTargetLayerSettingsFor.
    const bool bAlreadyPresent = Landscape->HasTargetLayer(TargetLayerName);
    const FLandscapeTargetLayerSettings NewSettings(LayerInfo);
    if (bAlreadyPresent)
    {
        Landscape->UpdateTargetLayer(TargetLayerName, NewSettings, /*bPostEditChange*/ true);
    }
    else
    {
        Landscape->AddTargetLayer(TargetLayerName, NewSettings, /*bPostEditChange*/ true);
    }

    // Also register the target layer settings into the shared ULandscapeInfo so the
    // edit-data interface (paint) can resolve it across proxies.
    if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo())
    {
        Info->CreateTargetLayerSettingsFor(LayerInfo);
        Info->UpdateLayerInfoMap(Landscape);
    }

    // Verify by readback.
    const bool bHasLayer = Landscape->HasTargetLayer(TargetLayerName);
    const int32 TargetLayerCount = Landscape->GetTargetLayers().Num();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetStringField(TEXT("layer_info_path"), LayerInfoPath);
    ResultObj->SetStringField(TEXT("layer_name"), TargetLayerName.ToString());
    ResultObj->SetBoolField(TEXT("layer_added"), bHasLayer);
    ResultObj->SetBoolField(TEXT("already_present"), bAlreadyPresent);
    ResultObj->SetNumberField(TEXT("target_layer_count"), TargetLayerCount);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remove_landscape_layer - Unregister a target weight (paint) layer from a landscape -- the inverse of add_landscape_layer. Identify it either by layer_info_path or by layer_name. This detaches the layer from the terrain; it does not delete the LandscapeLayerInfo asset, and any material that still blends that layer will now read zero weight for it.
//   @param layer_info_path - Full path of the ULandscapeLayerInfoObject asset; if omitted, resolved by layer_name via the landscape info
//   @param layer_name - Optional layer FName; defaults to the layer info's own GetLayerName()
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @returns { landscape_actor_name: string, layer_info_path: string, layer_name: string, layer_removed: bool, already_absent: bool, target_layer_count: int }
TSharedPtr<FJsonObject> HandleRemoveLandscapeLayer(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByActorNameParam(Params, Landscape))
    {
        return Error;
    }

    // ULandscapeInfo::DeleteLayer checkf's on a registered LandscapeActor, so the
    // shared info must exist (mirrors add_landscape_layer's CreateTargetLayerSettingsFor
    // path). Honest-gate if the landscape is not fully imported.
    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    if (!Info)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Landscape has no ULandscapeInfo (not fully imported); cannot remove layer"));
    }

    // Resolve the layer info object the same way add_landscape_layer does: prefer the
    // explicit asset path, else look it up by name from the landscape info. DeleteLayer
    // matches on BOTH the LayerInfoObj and the LayerName, so we need both.
    ULandscapeLayerInfoObject* LayerInfo = nullptr;
    FString LayerInfoPath;
    if (Params->TryGetStringField(TEXT("layer_info_path"), LayerInfoPath) && !LayerInfoPath.IsEmpty())
    {
        LayerInfo = Cast<ULandscapeLayerInfoObject>(
            StaticLoadObject(ULandscapeLayerInfoObject::StaticClass(), nullptr, *LayerInfoPath));
        if (!LayerInfo)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("LandscapeLayerInfoObject not found: %s"), *LayerInfoPath));
        }
    }

    // The target layer key: explicit layer_name, else the layer info's own name.
    FString LayerNameStr;
    FName TargetLayerName = NAME_None;
    if (Params->TryGetStringField(TEXT("layer_name"), LayerNameStr) && !LayerNameStr.IsEmpty())
    {
        TargetLayerName = FName(*LayerNameStr);
    }
    else if (LayerInfo)
    {
        TargetLayerName = LayerInfo->GetLayerName();
    }

    if (TargetLayerName.IsNone())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing layer identity: provide layer_info_path and/or layer_name"));
    }

    // If the path was omitted, resolve the layer info object by name from the info.
    if (!LayerInfo)
    {
        LayerInfo = Info->GetLayerInfoByName(TargetLayerName, Landscape);
    }

    // Idempotent: if the target layer isn't registered on the landscape, report the
    // already-absent state instead of asserting/duplicating work.
    const bool bAlreadyAbsent = !Landscape->HasTargetLayer(TargetLayerName);
    if (bAlreadyAbsent)
    {
        TSharedPtr<FJsonObject> AbsentObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
        AbsentObj->SetStringField(TEXT("layer_info_path"), LayerInfo ? LayerInfo->GetPathName() : LayerInfoPath);
        AbsentObj->SetStringField(TEXT("layer_name"), TargetLayerName.ToString());
        AbsentObj->SetBoolField(TEXT("layer_removed"), false);
        AbsentObj->SetBoolField(TEXT("already_absent"), true);
        AbsentObj->SetNumberField(TEXT("target_layer_count"), Landscape->GetTargetLayers().Num());
        return mcp_common::MakeOkReply(AbsentObj);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveLandscapeLayer", "Remove Landscape Layer (MCP)"));
    Landscape->Modify();

    // ULandscapeInfo::DeleteLayer(ULandscapeLayerInfoObject*, const FName&) — removes the
    // FLandscapeInfoLayerSettings entry, calls ALandscape::RemoveTargetLayer, and clears
    // the layer's weight data from all components. Inverse of AddTargetLayer above.
    Info->DeleteLayer(LayerInfo, TargetLayerName);
    Info->UpdateLayerInfoMap(Landscape);

    // Verify by readback.
    const bool bHasLayer = Landscape->HasTargetLayer(TargetLayerName);
    const int32 TargetLayerCount = Landscape->GetTargetLayers().Num();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetStringField(TEXT("layer_info_path"), LayerInfo ? LayerInfo->GetPathName() : LayerInfoPath);
    ResultObj->SetStringField(TEXT("layer_name"), TargetLayerName.ToString());
    ResultObj->SetBoolField(TEXT("layer_removed"), !bHasLayer);
    ResultObj->SetBoolField(TEXT("already_absent"), false);
    ResultObj->SetNumberField(TEXT("target_layer_count"), TargetLayerCount);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:paint_landscape_layer - Paint terrain texture layers headless: write ONE uniform weight (0-255) into a landscape weightmap over a vertex region, or the whole terrain if you omit region. Blunt by design -- it fills a rectangle, it does not brush or blend, so build variety by painting several layers over different rectangles. The layer must already be registered with add_landscape_layer or nothing is stored. Region coordinates are VERTICES, not world units: get the ranges from get_landscape_info. Check vertices_painted and paint_warning rather than the success flag.
//   @param layer_info_path - Full path of the ULandscapeLayerInfoObject asset
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @param weight - 0 to 255 weight value, default 255
//   @param region - Optional [minX, minY, maxX, maxY] vertex region, defaults to the whole landscape
//   @returns { landscape_actor_name: string, layer_info_path: string, layer_name: string, weight: int, painted: bool, vertices_painted: number, region: [int], paint_warning: string }
TSharedPtr<FJsonObject> HandlePaintLandscapeLayer(const TSharedPtr<FJsonObject>& Params)
{
    FString LayerInfoPath;
    ULandscapeLayerInfoObject* LayerInfo = nullptr;
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveLayerTarget(Params, LayerInfoPath, LayerInfo, Landscape))
    {
        return Error;
    }

    ULandscapeInfo* Info = Landscape->GetLandscapeInfo();
    if (!Info)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Landscape has no ULandscapeInfo (not fully imported); cannot paint"));
    }

    // Resolve the landscape extent (vertex grid bounds).
    int32 ExtMinX = 0, ExtMinY = 0, ExtMaxX = 0, ExtMaxY = 0;
    if (!Info->GetLandscapeExtent(ExtMinX, ExtMinY, ExtMaxX, ExtMaxY))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Could not resolve landscape extent; cannot paint"));
    }

    // Weight 0-255 to fill (255 == fully painted).
    const int32 WeightI = FMath::Clamp(ParamInt(Params, TEXT("weight"), 255), 0, 255);
    const uint8 Weight = static_cast<uint8>(WeightI);

    // Optional region [MinX, MinY, MaxX, MaxY] clamped to the landscape extent.
    // Default: the WHOLE landscape (was a silent 8x8 interior block, so an
    // unqualified paint looked like it had done almost nothing).
    int32 MinX = ExtMinX;
    int32 MinY = ExtMinY;
    int32 MaxX = ExtMaxX;
    int32 MaxY = ExtMaxY;

    const TArray<TSharedPtr<FJsonValue>>* RegionArr = nullptr;
    if (Params->TryGetArrayField(TEXT("region"), RegionArr) && RegionArr && RegionArr->Num() >= 4)
    {
        MinX = static_cast<int32>((*RegionArr)[0]->AsNumber());
        MinY = static_cast<int32>((*RegionArr)[1]->AsNumber());
        MaxX = static_cast<int32>((*RegionArr)[2]->AsNumber());
        MaxY = static_cast<int32>((*RegionArr)[3]->AsNumber());
    }

    // Clamp to the landscape extent and ensure ordered bounds.
    MinX = FMath::Clamp(MinX, ExtMinX, ExtMaxX);
    MinY = FMath::Clamp(MinY, ExtMinY, ExtMaxY);
    MaxX = FMath::Clamp(MaxX, ExtMinX, ExtMaxX);
    MaxY = FMath::Clamp(MaxY, ExtMinY, ExtMaxY);
    if (MaxX < MinX) { Swap(MinX, MaxX); }
    if (MaxY < MinY) { Swap(MinY, MaxY); }

    const int32 RegionW = (MaxX - MinX) + 1;
    const int32 RegionH = (MaxY - MinY) + 1;
    const int64 NumVerts = static_cast<int64>(RegionW) * static_cast<int64>(RegionH);
    if (NumVerts <= 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Empty paint region"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "PaintLandscapeLayer", "Paint Landscape Layer (MCP)"));
    Landscape->Modify();

    // Ensure the target layer is registered so the alphamap accessor can resolve it.
    if (!Landscape->HasTargetLayer(LayerInfo->GetLayerName()))
    {
        Landscape->AddTargetLayer(LayerInfo->GetLayerName(), FLandscapeTargetLayerSettings(LayerInfo), /*bPostEditChange*/ true);
    }
    Info->CreateTargetLayerSettingsFor(LayerInfo);

    // Fill a uniform weight over the region.
    TArray<uint8> WeightData;
    WeightData.Init(Weight, static_cast<int32>(NumVerts));

    // TAlphamapAccessor<bUseInterp=false>(ULandscapeInfo*, ULandscapeLayerInfoObject*)
    // (FAlphamapAccessor is deprecated in 5.7). SetData fills the weightmap region.
    //
    // Edit-layer-aware weightmap write (UE 5.8): TAlphamapAccessor builds its
    // FLandscapeEditDataInterface via the ULandscapeInfo-only ctor, i.e. with
    // bUseSharedLandscapeEditLayer=true (verified in engine 5.8:
    // LandscapeEditInterface.cpp), exactly like FHeightmapAccessor<false>(Info).
    // SetAlphaData therefore targets whatever ALandscape::GetEditingLayer() returns
    // at write time. With the editing layer unset (invalid GUID) the paint lands on
    // the composite/final weightmap, which the edit-layer merge then RECOMPOSES from
    // the (unpainted) base layer on the next update -> the paint is silently lost and
    // only the base layer shows in the material (mirrors the heightmap bug). Scope
    // the accessor to the base edit layer, then ForceLayersFullUpdate() to recompose
    // the composite so a read-back / the landscape material reflects the paint.
    // See GetBaseEditLayerGuid.
    bool bPainted = false;
    FString PaintError;
    const FGuid EditLayerGuid = GetBaseEditLayerGuid(Landscape);
    {
        FScopedSetLandscapeEditingLayer LayerScope(Landscape, EditLayerGuid);
        TAlphamapAccessor<false> AlphaAccessor(Info, LayerInfo);
        AlphaAccessor.SetData(MinX, MinY, MaxX, MaxY, WeightData.GetData(),
            ELandscapeLayerPaintingRestriction::None);
        AlphaAccessor.Flush();
        bPainted = true;
    }
    // Recompose the final weightmap from the edit layers so a read-back / the
    // landscape material reflects the paint (only when we wrote to a valid layer).
    if (EditLayerGuid.IsValid())
    {
        Landscape->ForceLayersFullUpdate();
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetStringField(TEXT("layer_info_path"), LayerInfoPath);
    ResultObj->SetStringField(TEXT("layer_name"), LayerInfo->GetLayerName().ToString());
    ResultObj->SetNumberField(TEXT("weight"), WeightI);
    ResultObj->SetBoolField(TEXT("painted"), bPainted);
    ResultObj->SetNumberField(TEXT("vertices_painted"), static_cast<double>(NumVerts));
    // Diagnostic parity with import_landscape_heightmap: report whether the paint
    // was routed through a valid base edit layer (UE 5.8) so a re-verify can tell a
    // real write from a dropped-on-composite one.
    ResultObj->SetBoolField(TEXT("edit_layer_applied"), EditLayerGuid.IsValid());
    if (EditLayerGuid.IsValid())
    {
        ResultObj->SetStringField(TEXT("edit_layer_guid"), EditLayerGuid.ToString());
    }

    ResultObj->SetArrayField(TEXT("region"), Landscape_RectToJson(MinX, MinY, MaxX, MaxY));

    if (!PaintError.IsEmpty())
    {
        ResultObj->SetStringField(TEXT("paint_warning"), PaintError);
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_landscape_layers - List which paint/weight layers are registered on a landscape (name plus its LandscapeLayerInfo asset). The cheap check before painting: a layer missing from this list will silently absorb any paint_landscape_layer call. Use it to discover the layer names a terrain material expects.
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @returns { landscape_actor_name: string, layers: [object], layer_count: int }
TSharedPtr<FJsonObject> HandleGetLandscapeLayers(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByActorNameParam(Params, Landscape))
    {
        return Error;
    }

    // Read the proxy's TargetLayers map (FName -> FLandscapeTargetLayerSettings).
    TArray<TSharedPtr<FJsonValue>> Layers;
    for (const TPair<FName, FLandscapeTargetLayerSettings>& Pair : Landscape->GetTargetLayers())
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Pair.Key.ToString());

        const ULandscapeLayerInfoObject* LayerInfoObj = Pair.Value.LayerInfoObj;
        Obj->SetBoolField(TEXT("has_layer_info"), LayerInfoObj != nullptr);
        if (LayerInfoObj)
        {
            Obj->SetStringField(TEXT("layer_info_path"), LayerInfoObj->GetPathName());
            Obj->SetStringField(TEXT("layer_info_name"), LayerInfoObj->GetLayerName().ToString());
        }

        Layers.Add(MakeShared<FJsonValueObject>(Obj));
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetArrayField(TEXT("layers"), Layers);
    ResultObj->SetNumberField(TEXT("layer_count"), Layers.Num());

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_landscape_nanite_status - Report a landscape's Nanite flags (enabled / built / up-to-date / out-of-date) without triggering a build. Read-only, editor-only. TRUST ONLY nanite_enabled: the built/up-to-date flags can CONTRADICT each other (nanite_built=false alongside nanite_up_to_date=true and out_of_date=false) and stay at built=false while the editor is visibly building the meshes -- they do not distinguish "not built" from "built and current". For "is the Nanite mesh actually built", count the "Built static mesh ...LandscapeNaniteMesh" lines in Saved/Logs/ instead.
//   @param landscape_actor_name - Optional landscape actor name/label, defaults to first found
//   @returns { landscape_actor_name: string, nanite_enabled: bool, nanite_built: bool, nanite_up_to_date: bool, nanite_out_of_date: bool, note?: string }
TSharedPtr<FJsonObject> HandleGetLandscapeNaniteStatus(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByActorNameParam(Params, Landscape))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);

    // (a) enabled, (b) built (the serialized NaniteComponents array, NOT the
    // render proxy -- safe in -nullrhi), (c) up-to-date. NOTE: the semantics of
    // IsNaniteMeshUpToDate() are the INVERSE of its (wrong) doc-comment --
    // verified against the engine call sites, true = current, false = missing/
    // stale. It also returns true when Nanite is disabled, so out_of_date is
    // gated on enabled. Do NOT call GetNaniteContentId()/AreNaniteComponentsValid()/
    // GetOutdatedDataFlags(): those are unexported (LNK2019); IsNaniteMeshUpToDate()
    // wraps them and IS exported (LANDSCAPE_API).
    const bool bEnabled = Landscape->IsNaniteEnabled();
    const bool bBuilt = Landscape->HasNaniteComponents();
    const bool bUpToDate = Landscape->IsNaniteMeshUpToDate();
    const bool bOutOfDate = bEnabled && !bUpToDate;

    ResultObj->SetBoolField(TEXT("nanite_enabled"), bEnabled);
    ResultObj->SetBoolField(TEXT("nanite_built"), bBuilt);
    ResultObj->SetBoolField(TEXT("nanite_up_to_date"), bUpToDate);
    ResultObj->SetBoolField(TEXT("nanite_out_of_date"), bOutOfDate);
    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// Landscape splines
// ========================================================================== //
//
// Editor-only authoring of a landscape's ULandscapeSplinesComponent. Control
// points and segments are NewObject'd INTO the splines component (the classes are
// UCLASS(Within=LandscapeSplinesComponent), so the outer MUST be the component)
// and appended to the component's editor-only ControlPoints / Segments arrays via
// the public mutable GetControlPoints()/GetSegments() accessors (UE 5.7;
// LandscapeEdModeSplineTools.cpp uses the same NewObject + array-append pattern,
// but as a friend it touches the protected arrays directly). We deliberately do
// NOT call UpdateSplinePoints()/RebuildAllSplines() here — those build spline mesh
// + render resources, which can crash headless; this group authors the spline data
// arrays only and verifies by reading the array counts back.
//
// Nor ULandscapeSplinesComponent::ModifySplines: it is NOT exported (the class is
// UCLASS(MinimalAPI) and that method lacks LANDSCAPE_API), so it would LNK2019 cross
// module. SplinesComp->Modify() (UObject, exported) already marks the component for
// the transaction + dirties the package, and MarkRenderStateDirty (ENGINE_API)
// refreshes the editor render state.

namespace
{
    // Resolve the landscape's splines component, creating one if absent + requested.
    // ALandscapeProxy::GetSplinesComponent() is the always-available accessor;
    // CreateSplineComponent() (LANDSCAPE_API, editor-only) lazily allocates one.
    ULandscapeSplinesComponent* ResolveSplinesComponent(ALandscape* Landscape, bool bCreateIfMissing)
    {
        if (!Landscape)
        {
            return nullptr;
        }
        ULandscapeSplinesComponent* SplinesComp = Landscape->GetSplinesComponent();
        if (!SplinesComp && bCreateIfMissing)
        {
            Landscape->CreateSplineComponent();
            SplinesComp = Landscape->GetSplinesComponent();
        }
        return SplinesComp;
    }

    // The "landscape_name" landscape and its existing splines component (never created here); null on success,
    // else the error reply, NoSplinesError when the landscape has no splines component.
    TSharedPtr<FJsonObject> Landscape_ResolveSplines(const TSharedPtr<FJsonObject>& Params, const TCHAR* NoSplinesError,
        ALandscape*& OutLandscape, ULandscapeSplinesComponent*& OutSplinesComp)
    {
        if (TSharedPtr<FJsonObject> Error = Landscape_FindByNameParam(Params, OutLandscape))
        {
            return Error;
        }
        OutSplinesComp = ResolveSplinesComponent(OutLandscape, /*bCreateIfMissing*/ false);
        if (!OutSplinesComp)
        {
            return mcp_common::MakeErrorReply(NoSplinesError);
        }
        return nullptr;
    }

    // The non-null control point at Index of SplinesComp; null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_FindControlPoint(ULandscapeSplinesComponent* SplinesComp, int32 Index,
        ULandscapeSplineControlPoint*& OutControlPoint)
    {
        const TArray<TObjectPtr<ULandscapeSplineControlPoint>>& ControlPoints = SplinesComp->GetControlPoints();
        if (!ControlPoints.IsValidIndex(Index))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid control_point_index %d (count=%d)"), Index, ControlPoints.Num()));
        }
        OutControlPoint = ControlPoints[Index];
        if (!OutControlPoint)
        {
            return mcp_common::MakeErrorReply(TEXT("Resolved control point is null"));
        }
        return nullptr;
    }
}

// MCP:add_landscape_spline_control_point - Add a control point to a landscape's spline component -- the first half of laying a road, river bed or path across the terrain (connect consecutive points with add_landscape_spline_segment). Coordinates are LANDSCAPE-LOCAL, not world: subtract the landscape's location (get_landscape_info) before passing them, or the point lands somewhere else entirely. `width` is a HALF-width. The reply's control_point_index is what the segment and edit tools address.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param location - [x, y, z] in landscape-local space, default [0,0,0]
//   @param width - Half-width at this point, default 128
//   @returns { landscape_name: string, control_point_index: int, control_point_count: int, width: number, added: bool, location: [number] }
TSharedPtr<FJsonObject> HandleAddLandscapeSplineControlPoint(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByNameParam(Params, Landscape))
    {
        return Error;
    }

    // Location is in intra-landscape (landscape-local) space, matching
    // ULandscapeSplineControlPoint::Location's documented coordinate frame.
    const FVector Location = Params->HasField(TEXT("location"))
        ? mcp_common::ReadJsonVector(Params, TEXT("location"))
        : FVector::ZeroVector;

    // Width is the spline half-width at this point; engine default new-control-point
    // width is 128. Accept a float.
    double WidthD = 128.0;
    Params->TryGetNumberField(TEXT("width"), WidthD);
    const float Width = static_cast<float>(WidthD);

    ULandscapeSplinesComponent* SplinesComp = ResolveSplinesComponent(Landscape, /*bCreateIfMissing*/ true);
    if (!SplinesComp)
    {
        // Honest: the editor-world landscape has no splines component and one
        // could not be created headless.
        return mcp_common::MakeErrorReply(
            TEXT("Landscape has no splines component and one could not be created"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddLandscapeSplineControlPoint", "Add Landscape Spline Control Point (MCP)"));
    SplinesComp->Modify();

    // NewObject INTO the splines component (Within=LandscapeSplinesComponent).
    ULandscapeSplineControlPoint* NewControlPoint =
        NewObject<ULandscapeSplineControlPoint>(SplinesComp, NAME_None, RF_Transactional);
    if (!NewControlPoint)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create ULandscapeSplineControlPoint"));
    }
    NewControlPoint->Location = Location;
    NewControlPoint->Width = Width;

    // Append via the public mutable accessor (returns a non-const ref to the
    // component's ControlPoints array).
    TArray<TObjectPtr<ULandscapeSplineControlPoint>>& ControlPoints = SplinesComp->GetControlPoints();
    const int32 NewIndex = ControlPoints.Add(NewControlPoint);

    SplinesComp->MarkRenderStateDirty();

    const int32 ControlPointCount = SplinesComp->GetControlPoints().Num();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetNumberField(TEXT("control_point_index"), NewIndex);
    ResultObj->SetNumberField(TEXT("control_point_count"), ControlPointCount);
    ResultObj->SetNumberField(TEXT("width"), Width);
    ResultObj->SetBoolField(TEXT("added"), true);

    ResultObj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Location));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_landscape_spline_control_point - Move, rotate or re-width one existing landscape spline control point by index -- how you adjust a road or river route without rebuilding the spline. Only the fields you pass change (applied_count reports how many landed). Location is LANDSCAPE-LOCAL, not world, and width is a HALF-width. Read the current values first with get_landscape_spline_control_point.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param control_point_index [integer!] - Index into the splines component's ControlPoints
//   @param location - Optional [x, y, z] in landscape-local space; only applied if provided
//   @param rotation - Optional [pitch, yaw, roll]; only applied if provided
//   @param width - Optional half-width at this point; only applied if provided
//   @returns { landscape_name: string, control_point_index: int, control_point_count: int, applied_count: int, location: [number], rotation: [number], width: number, updated: bool }
TSharedPtr<FJsonObject> HandleSetLandscapeSplineControlPoint(const TSharedPtr<FJsonObject>& Params)
{
    // Mutating an existing point: with no splines component there is nothing to set.
    ALandscape* Landscape = nullptr;
    ULandscapeSplinesComponent* SplinesComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSplines(Params, TEXT("Landscape has no splines component"), Landscape, SplinesComp))
    {
        return Error;
    }

    const int32 Index = ParamInt(Params, TEXT("control_point_index"), -1);
    ULandscapeSplineControlPoint* ControlPoint = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindControlPoint(SplinesComp, Index, ControlPoint))
    {
        return Error;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetLandscapeSplineControlPoint", "Set Landscape Spline Control Point (MCP)"));
    SplinesComp->Modify();
    ControlPoint->Modify();

    // Apply only the provided properties (location/rotation/width), mirroring the field
    // names + coordinate frames used by add_landscape_spline_control_point.
    int32 Applied = 0;
    if (Params->HasField(TEXT("location")))
    {
        ControlPoint->Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
        ++Applied;
    }
    if (Params->HasField(TEXT("rotation")))
    {
        // [pitch, yaw, roll], matching add_foliage_instances' rotation convention.
        const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
        if (Params->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr && RotArr->Num() >= 3)
        {
            ControlPoint->Rotation = FRotator(
                (*RotArr)[0]->AsNumber(), (*RotArr)[1]->AsNumber(), (*RotArr)[2]->AsNumber());
            ++Applied;
        }
    }
    double WidthD = 0.0;
    if (Params->TryGetNumberField(TEXT("width"), WidthD))
    {
        ControlPoint->Width = static_cast<float>(WidthD);
        ++Applied;
    }

    SplinesComp->MarkRenderStateDirty();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetNumberField(TEXT("control_point_index"), Index);
    ResultObj->SetNumberField(TEXT("control_point_count"), SplinesComp->GetControlPoints().Num());
    ResultObj->SetNumberField(TEXT("applied_count"), Applied);
    ResultObj->SetNumberField(TEXT("width"), ControlPoint->Width);
    ResultObj->SetBoolField(TEXT("updated"), Applied > 0);

    ResultObj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(ControlPoint->Location));

    ResultObj->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(ControlPoint->Rotation));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_landscape_spline_control_point - Read ONE landscape spline control point by index: its landscape-local location and rotation, half-width, side/end falloff, and how many segments connect to it. The per-point reader that pairs with set_landscape_spline_control_point; get_landscape_splines only gives component-level counts, so this is the only way to see where a road or river point actually sits. connected_segment_count = 0 marks a dangling point that draws nothing.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param control_point_index [integer!] - Index into the splines component's ControlPoints (required)
//   @returns { landscape_name: string, control_point_index: int, control_point_count: int, location: [number], rotation: [number], width: number, side_falloff: number, end_falloff: number, connected_segment_count: int }
TSharedPtr<FJsonObject> HandleGetLandscapeSplineControlPoint(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeSplinesComponent* SplinesComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSplines(Params, TEXT("Landscape has no splines component"), Landscape, SplinesComp))
    {
        return Error;
    }

    const int32 Index = ParamInt(Params, TEXT("control_point_index"), -1);
    ULandscapeSplineControlPoint* ControlPoint = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindControlPoint(SplinesComp, Index, ControlPoint))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetNumberField(TEXT("control_point_index"), Index);
    ResultObj->SetNumberField(TEXT("control_point_count"), SplinesComp->GetControlPoints().Num());
    ResultObj->SetNumberField(TEXT("width"), ControlPoint->Width);
    ResultObj->SetNumberField(TEXT("side_falloff"), ControlPoint->SideFalloff);
    ResultObj->SetNumberField(TEXT("end_falloff"), ControlPoint->EndFalloff);
    ResultObj->SetNumberField(TEXT("connected_segment_count"), ControlPoint->ConnectedSegments.Num());

    ResultObj->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(ControlPoint->Location));

    ResultObj->SetArrayField(TEXT("rotation"), mcp_common::RotatorToJsonArray(ControlPoint->Rotation));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:add_landscape_spline_segment - Join two existing landscape spline control points with a segment -- the second half of laying a road, river or path: points alone render nothing, the segments between them are the spline. Both indices must already exist (add_landscape_spline_control_point returns them). Chain segments point-to-point for a route; use get_landscape_splines to confirm segment_count grew.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param start_point_index [integer!] - Index of the start control point
//   @param end_point_index [integer!] - Index of the end control point
//   @returns { landscape_name: string, segment_index: int, segment_count: int, start_point_index: int, end_point_index: int, added: bool }
TSharedPtr<FJsonObject> HandleAddLandscapeSplineSegment(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeSplinesComponent* SplinesComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSplines(Params,
            TEXT("Landscape has no splines component; add control points first"), Landscape, SplinesComp))
    {
        return Error;
    }

    const int32 StartIndex = ParamInt(Params, TEXT("start_point_index"), -1);
    const int32 EndIndex   = ParamInt(Params, TEXT("end_point_index"), -1);

    TArray<TObjectPtr<ULandscapeSplineControlPoint>>& ControlPoints = SplinesComp->GetControlPoints();
    if (ControlPoints.Num() < 2)
    {
        // Honest: a segment needs two existing control points.
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Need at least 2 control points to add a segment (have %d)"), ControlPoints.Num()));
    }
    if (!ControlPoints.IsValidIndex(StartIndex) || !ControlPoints.IsValidIndex(EndIndex) || StartIndex == EndIndex)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid start/end control point indices (start=%d end=%d count=%d)"),
                StartIndex, EndIndex, ControlPoints.Num()));
    }

    ULandscapeSplineControlPoint* Start = ControlPoints[StartIndex];
    ULandscapeSplineControlPoint* End   = ControlPoints[EndIndex];
    if (!Start || !End)
    {
        return mcp_common::MakeErrorReply(TEXT("Resolved control point is null"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddLandscapeSplineSegment", "Add Landscape Spline Segment (MCP)"));
    SplinesComp->Modify();
    Start->Modify();
    End->Modify();

    // NewObject INTO the splines component (Within=LandscapeSplinesComponent).
    ULandscapeSplineSegment* NewSegment =
        NewObject<ULandscapeSplineSegment>(SplinesComp, NAME_None, RF_Transactional);
    if (!NewSegment)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create ULandscapeSplineSegment"));
    }

    // Wire the two ends to the control points (mirrors LandscapeEdModeSplineTools).
    NewSegment->Connections[0].ControlPoint = Start;
    NewSegment->Connections[1].ControlPoint = End;

    // Tangent lengths default to the straight-line distance between the points.
    const float TangentLen = static_cast<float>((End->Location - Start->Location).Size());
    NewSegment->Connections[0].TangentLen = TangentLen;
    NewSegment->Connections[1].TangentLen = TangentLen;

    // Back-reference the segment from each control point's ConnectedSegments list
    // (End 0 == start, End 1 == end), keeping the data model consistent.
    Start->ConnectedSegments.Add(FLandscapeSplineConnection(NewSegment, 0));
    End->ConnectedSegments.Add(FLandscapeSplineConnection(NewSegment, 1));

    TArray<TObjectPtr<ULandscapeSplineSegment>>& Segments = SplinesComp->GetSegments();
    const int32 NewIndex = Segments.Add(NewSegment);

    SplinesComp->MarkRenderStateDirty();

    const int32 SegmentCount = SplinesComp->GetSegments().Num();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetNumberField(TEXT("segment_index"), NewIndex);
    ResultObj->SetNumberField(TEXT("segment_count"), SegmentCount);
    ResultObj->SetNumberField(TEXT("start_point_index"), StartIndex);
    ResultObj->SetNumberField(TEXT("end_point_index"), EndIndex);
    ResultObj->SetBoolField(TEXT("added"), true);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_landscape_splines - Summary of a landscape's spline component: whether it has one at all, plus the control point and segment counts. The quick "is there a road/river network on this terrain, and how big" check, and the count oracle after adding or removing points. For an individual point's position and width use get_landscape_spline_control_point -- this returns no per-point data.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @returns { landscape_name: string, has_splines_component: bool, control_point_count: int, segment_count: int }
TSharedPtr<FJsonObject> HandleGetLandscapeSplines(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByNameParam(Params, Landscape))
    {
        return Error;
    }

    // Read-only: never create a component just to read. 0/false on a landscape
    // with no splines is the correct real answer.
    ULandscapeSplinesComponent* SplinesComp = Landscape->GetSplinesComponent();

    int32 ControlPointCount = 0;
    int32 SegmentCount = 0;
    if (SplinesComp)
    {
        ControlPointCount = SplinesComp->GetControlPoints().Num();
        SegmentCount = SplinesComp->GetSegments().Num();
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetBoolField(TEXT("has_splines_component"), SplinesComp != nullptr);
    ResultObj->SetNumberField(TEXT("control_point_count"), ControlPointCount);
    ResultObj->SetNumberField(TEXT("segment_count"), SegmentCount);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remove_landscape_spline_control_point - Delete a landscape spline control point by index, and with it every segment attached to it (segments_removed says how many) -- so removing a mid-route point BREAKS the road or river in two rather than healing it; re-join the neighbours with add_landscape_spline_segment. Remaining indices shift, so delete from the highest index down when removing several.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param control_point_index [integer!] - Index into the splines component's ControlPoints
//   @returns { landscape_name: string, control_point_index: int, removed: bool, control_point_count: int, segments_removed: int, segment_count: int }
TSharedPtr<FJsonObject> HandleRemoveLandscapeSplineControlPoint(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeSplinesComponent* SplinesComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSplines(Params, TEXT("Landscape has no splines component"), Landscape, SplinesComp))
    {
        return Error;
    }

    const int32 Index = ParamInt(Params, TEXT("control_point_index"), -1);

    TArray<TObjectPtr<ULandscapeSplineControlPoint>>& ControlPoints = SplinesComp->GetControlPoints();
    if (!ControlPoints.IsValidIndex(Index))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid control_point_index %d (count=%d)"), Index, ControlPoints.Num()));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveLandscapeSplineControlPoint", "Remove Landscape Spline Control Point (MCP)"));
    SplinesComp->Modify();

    ULandscapeSplineControlPoint* ControlPoint = ControlPoints[Index];

    // Remove any segments that referenced this control point first (and clear their
    // back-references on the OTHER end) so we don't leave dangling connections.
    int32 SegmentsRemoved = 0;
    if (ControlPoint)
    {
        TArray<TObjectPtr<ULandscapeSplineSegment>>& Segments = SplinesComp->GetSegments();
        for (int32 s = Segments.Num() - 1; s >= 0; --s)
        {
            ULandscapeSplineSegment* Segment = Segments[s];
            if (!Segment)
            {
                continue;
            }
            if (Segment->Connections[0].ControlPoint == ControlPoint ||
                Segment->Connections[1].ControlPoint == ControlPoint)
            {
                // Clean the back-reference on the connected control points.
                for (int32 e = 0; e < 2; ++e)
                {
                    if (ULandscapeSplineControlPoint* Other = Segment->Connections[e].ControlPoint)
                    {
                        Other->Modify();
                        Other->ConnectedSegments.RemoveAll(
                            [Segment](const FLandscapeSplineConnection& Conn) { return Conn.Segment == Segment; });
                    }
                }
                Segments.RemoveAt(s);
                ++SegmentsRemoved;
            }
        }
    }

    ControlPoints.RemoveAt(Index);

    SplinesComp->MarkRenderStateDirty();

    const int32 ControlPointCount = SplinesComp->GetControlPoints().Num();
    const int32 SegmentCount = SplinesComp->GetSegments().Num();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetNumberField(TEXT("control_point_index"), Index);
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetNumberField(TEXT("control_point_count"), ControlPointCount);
    ResultObj->SetNumberField(TEXT("segments_removed"), SegmentsRemoved);
    ResultObj->SetNumberField(TEXT("segment_count"), SegmentCount);

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:remove_landscape_spline_segment - Delete ONE landscape spline segment by index, unlinking its two endpoint control points but keeping them -- the way to cut a road or river without losing the route's points (remove_landscape_spline_control_point destroys the point and all its segments instead). Segment indices shift after a removal, so work from the highest index down.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param segment_index [integer!] - Index into the splines component's Segments
//   @returns { landscape_name: string, segment_index: int, removed: bool, segment_count: int, control_points_unlinked: int }
TSharedPtr<FJsonObject> HandleRemoveLandscapeSplineSegment(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeSplinesComponent* SplinesComp = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSplines(Params, TEXT("Landscape has no splines component"), Landscape, SplinesComp))
    {
        return Error;
    }

    const int32 Index = ParamInt(Params, TEXT("segment_index"), -1);

    TArray<TObjectPtr<ULandscapeSplineSegment>>& Segments = SplinesComp->GetSegments();
    if (!Segments.IsValidIndex(Index))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid segment_index %d (count=%d)"), Index, Segments.Num()));
    }

    ULandscapeSplineSegment* Segment = Segments[Index];
    if (!Segment)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Resolved segment at index %d is null"), Index));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveLandscapeSplineSegment", "Remove Landscape Spline Segment (MCP)"));
    SplinesComp->Modify();
    Segment->Modify();

    // Clear this segment's back-reference on BOTH endpoint control points so we
    // don't leave dangling ConnectedSegments entries (mirrors the sub-loop in
    // HandleRemoveLandscapeSplineControlPoint). Connections[2]: End 0 == start,
    // End 1 == end.
    int32 ControlPointsUnlinked = 0;
    for (int32 e = 0; e < 2; ++e)
    {
        if (ULandscapeSplineControlPoint* ControlPoint = Segment->Connections[e].ControlPoint)
        {
            ControlPoint->Modify();
            ControlPoint->ConnectedSegments.RemoveAll(
                [Segment](const FLandscapeSplineConnection& Conn) { return Conn.Segment == Segment; });
            ++ControlPointsUnlinked;
        }
    }

    Segments.RemoveAt(Index);

    SplinesComp->MarkRenderStateDirty();

    const int32 SegmentCount = SplinesComp->GetSegments().Num();

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetNumberField(TEXT("segment_index"), Index);
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetNumberField(TEXT("segment_count"), SegmentCount);
    ResultObj->SetNumberField(TEXT("control_points_unlinked"), ControlPointsUnlinked);

    return mcp_common::MakeOkReply(ResultObj);
}
// ========================================================================== //
// Landscape sculpt + heightmap I/O
//
// Commands that operate on the landscape heightmap vertex grid via
// FHeightmapAccessor (the heightmap-side of the same template pair as
// TAlphamapAccessor<false> used by paint_landscape_layer):
//
//   set_landscape_height_in_box   add `delta_z` (world units) to every height
//                                 value inside a world-space box. Returns the
//                                 region's vertex rect + the actual delta
//                                 applied (clamped to the landscape extent).
//   flatten_landscape_in_box      snap all height values inside a world-space
//                                 box to a fixed `target_z` (world units).
//   smooth_landscape_in_box       apply a uniform 3x3 box-blur kernel over the
//                                 region's height values (5 iterations max) and
//                                 write the smoothed buffer back. Cheap kernel
//                                 to keep the operation O(W*H) and headless-safe.
//   copy_landscape_region         copy a heightmap region from one landscape
//                                 to another (or the same), resampling with
//                                 FLandscapeImportHelper::TransformHeightmap
//                                 ImportData when source/destination sizes
//                                 differ. Same-size copy is a direct memcpy.
//   import_landscape_heightmap    load a PNG/.r16/.r16bigendian heightmap file
//                                 via FLandscapeImportHelper and apply over the
//                                 landscape via FHeightmapAccessor.
//   export_landscape_heightmap    dump the whole landscape (or a sub-region) to
//                                 a PNG/.r16 file via ULandscapeInfo::ExportHeightmap.
//   get_landscape_height_at       sample the surface world Z at one world (x, y)
//                                 via FHeightmapAccessor + bilinear interpolation.
//
// World <-> vertex coord math: a vertex at landscape-local coord (Vx,Vy) sits
// at world (ActorLoc.X + Vx * Scale.X, ActorLoc.Y + Vy * Scale.Y). The
// heightmap uint16 value H maps to world Z by
// (H - 32768) * LANDSCAPE_ZSCALE * Scale.Z, so adding delta world Z to the
// landscape is adding `delta / (LANDSCAPE_ZSCALE * Scale.Z)` uint16 steps.
// ========================================================================== //

namespace
{
    // Convert world-Z delta to the uint16 heightmap-step delta for a given
    // landscape actor. Uses LANDSCAPE_ZSCALE (1/128) so delta of 128 world
    // units == exactly 1.0 uint16 step * Scale.Z. Returns false if scale.Z
    // is degenerate.
    bool WorldZDeltaToUint16Step(ALandscape* Landscape, double WorldZ, int32& OutSteps)
    {
        if (!Landscape)
        {
            return false;
        }
        const double ScaleZ = static_cast<double>(Landscape->GetActorScale3D().Z);
        if (ScaleZ <= KINDA_SMALL_NUMBER)
        {
            return false;
        }
        // uint16_per_world_z = 1 / (LANDSCAPE_ZSCALE * Scale.Z) = 128 / Scale.Z
        const double Steps = (WorldZ * 128.0) / ScaleZ;
        OutSteps = FMath::RoundToInt(Steps);
        return true;
    }

    // Convert target world-Z to a uint16 heightmap value for the given landscape.
    bool WorldZToUint16(ALandscape* Landscape, double WorldZ, uint16& OutH)
    {
        if (!Landscape)
        {
            return false;
        }
        const double ScaleZ = static_cast<double>(Landscape->GetActorScale3D().Z);
        if (ScaleZ <= KINDA_SMALL_NUMBER)
        {
            return false;
        }
        // h = WorldZ / (LANDSCAPE_ZSCALE * Scale.Z) + 32768 = WorldZ * 128 / Scale.Z + 32768
        const double Raw = (WorldZ * 128.0) / ScaleZ + 32768.0;
        const double Clamped = FMath::Clamp(Raw, 0.0, 65535.0);
        OutH = static_cast<uint16>(FMath::RoundToInt(Clamped));
        return true;
    }

    // Edit-layer GUID for edit-layer-aware height writes (UE 5.8). Every landscape
    // now owns >=1 edit layer, and FHeightmapAccessor<false> writes to the
    // landscape's *current editing layer* (ALandscape::PrivateEditingLayer, read
    // dynamically by FLandscapeEditDataInterface). When that editing layer is unset
    // (invalid GUID) the accessor writes to the composite/final heightmap, which the
    // edit-layer merge then RECOMPOSES from the (empty midpoint=32768) base layer on
    // the next update -> the write is silently lost and the terrain reads back flat.
    // Callers scope their accessor block with FScopedSetLandscapeEditingLayer(this,
    // GUID) so the write lands in the base edit layer, then ForceLayersFullUpdate()
    // to recompose the composite synchronously. Returns the first (base) edit
    // layer's GUID, or an invalid FGuid when the landscape has no edit layers (in
    // which case the scope is a no-op == legacy composite write).
    FGuid GetBaseEditLayerGuid(ALandscape* Landscape)
    {
        if (Landscape)
        {
            if (const ULandscapeEditLayerBase* EditLayer = Landscape->GetEditLayerConst(0))
            {
                return EditLayer->GetGuid();
            }
        }
        return FGuid();
    }

    // A landscape a heightmap command works on: the editor world, the landscape, its ULandscapeInfo and its
    // vertex extent.
    struct FLandscapeHeightmapTarget
    {
        UWorld* World = nullptr;
        ALandscape* Landscape = nullptr;
        ULandscapeInfo* Info = nullptr;
        int32 ExtMinX = 0;
        int32 ExtMinY = 0;
        int32 ExtMaxX = 0;
        int32 ExtMaxY = 0;
    };

    // The editor world, the "landscape_actor_name" landscape (or the first one found), its ULandscapeInfo and
    // vertex extent, into Out; null on success, else the error reply. Shared by every sculpt and heightmap handler.
    TSharedPtr<FJsonObject> Landscape_ResolveHeightmapTarget(const TSharedPtr<FJsonObject>& Params, FLandscapeHeightmapTarget& Out)
    {
        if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(Out.World)) { return NoWorld; }
        FString ActorName;
        Params->TryGetStringField(TEXT("landscape_actor_name"), ActorName);
        TSharedPtr<FJsonObject> NotFound;
        Out.Landscape = Landscape_FindOrError(Out.World, ActorName, NotFound);
        if (!Out.Landscape)
        {
            return NotFound;
        }
        Out.Info = Out.Landscape->GetLandscapeInfo();
        if (!Out.Info)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Landscape has no ULandscapeInfo (not fully imported); cannot sculpt"));
        }
        if (!Out.Info->GetLandscapeExtent(Out.ExtMinX, Out.ExtMinY, Out.ExtMaxX, Out.ExtMaxY))
        {
            return mcp_common::MakeErrorReply(
                TEXT("Could not resolve landscape extent; cannot sculpt"));
        }
        return nullptr;
    }

    // The sculpt landscape and the vertex rect of its world box; null on success, else the error reply.
    TSharedPtr<FJsonObject> Landscape_ResolveSculptRect(const TSharedPtr<FJsonObject>& Params,
        ALandscape*& OutLandscape, ULandscapeInfo*& OutInfo, int32& OutMinX, int32& OutMinY, int32& OutMaxX, int32& OutMaxY)
    {
        FLandscapeHeightmapTarget Target;
        if (TSharedPtr<FJsonObject> Error = Landscape_ResolveHeightmapTarget(Params, Target))
        {
            return Error;
        }
        OutLandscape = Target.Landscape;
        OutInfo = Target.Info;

        FBox Box(ForceInit);
        if (!ParseWorldBox(Params, Box) && !Landscape_DefaultSculptBox(OutLandscape, Box))
        {
            return mcp_common::MakeErrorReply(
                TEXT("Missing required parameter: box_min/box_max (or center/extent)"));
        }
        if (!WorldBoxToVertexRect(OutLandscape, Box, Target.ExtMinX, Target.ExtMinY, Target.ExtMaxX, Target.ExtMaxY,
                                  OutMinX, OutMinY, OutMaxX, OutMaxY))
        {
            return mcp_common::MakeErrorReply(
                TEXT("Box does not intersect the landscape extent"));
        }
        return nullptr;
    }

    // Writes Heights into the base edit layer and recomposes the heightmap (see GetBaseEditLayerGuid).
    FGuid Landscape_WriteBaseLayerHeights(ALandscape* Landscape, ULandscapeInfo* Info,
        int32 MinX, int32 MinY, int32 MaxX, int32 MaxY, const TArray<uint16>& Heights)
    {
        const FGuid EditLayerGuid = GetBaseEditLayerGuid(Landscape);
        {
            FScopedSetLandscapeEditingLayer LayerScope(Landscape, EditLayerGuid);
            FHeightmapAccessor<false> Accessor(Info);
            Accessor.SetData(MinX, MinY, MaxX, MaxY, Heights.GetData(),
                             ELandscapeLayerPaintingRestriction::None);
            Accessor.Flush();
        }
        if (EditLayerGuid.IsValid())
        {
            Landscape->ForceLayersFullUpdate();
        }
        return EditLayerGuid;
    }

    // Src as DstW x DstH heights: moved when the sizes match, otherwise resampled with the source centred.
    TArray<uint16> Landscape_FitHeights(TArray<uint16>& Src, int32 SrcW, int32 SrcH, int32 DstW, int32 DstH)
    {
        if (SrcW == DstW && SrcH == DstH)
        {
            return MoveTemp(Src);
        }
        TArray<uint16> Fitted;
        Fitted.SetNumZeroed(DstW * DstH);
        FLandscapeImportHelper::TransformHeightmapImportData(
            Src, Fitted,
            FLandscapeImportResolution(static_cast<uint32>(SrcW), static_cast<uint32>(SrcH)),
            FLandscapeImportResolution(static_cast<uint32>(DstW), static_cast<uint32>(DstH)),
            ELandscapeImportTransformType::ExpandCentered,
            FIntPoint(0, 0));
        return Fitted;
    }

    // The required "file_path" param as a full filesystem path; null on success, else the error reply.
    // An asset path (/Game/MyGame/foo.png) resolves under Content/ KEEPING its extension (.png/.r16/.r16be):
    // GetHeightmapFormatByExtension() routes on it, and TryConvertLongPackageNameToFilename appends .uasset.
    TSharedPtr<FJsonObject> Landscape_ReadHeightmapFilePath(const TSharedPtr<FJsonObject>& Params, FString& OutFilePath)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("file_path"), OutFilePath)) { return Missing; }
        if (OutFilePath.StartsWith(TEXT("/")))
        {
            FString Ext = FPaths::GetExtension(OutFilePath, /*bIncludeDot=*/true);
            FString PackagePath = OutFilePath;
            if (PackagePath.Contains(TEXT(".")))
            {
                PackagePath = FPaths::GetBaseFilename(PackagePath, /*bRemovePath=*/false);
            }
            FString ResolvedPathOnly;
            if (FPackageName::TryConvertLongPackageNameToFilename(
                    PackagePath, ResolvedPathOnly, FString()))
            {
                // ResolvedPathOnly includes the trailing .uasset from the default
                // ext arg; strip it before reattaching the user-supplied extension.
                ResolvedPathOnly = FPaths::GetBaseFilename(ResolvedPathOnly, /*bRemovePath=*/false);
                OutFilePath = ResolvedPathOnly + Ext;
            }
        }
        if (FPaths::IsRelative(OutFilePath))
        {
            OutFilePath = FPaths::ConvertRelativePathToFull(OutFilePath);
        }
        return nullptr;
    }
}

// MCP:set_landscape_height_in_box - Raise or lower terrain: add delta_z world units to every heightmap vertex inside a world-space box (give box_min/box_max, or center/extent). A RELATIVE edit -- to force a region to one absolute height use flatten_landscape_in_box, and to soften the seams afterwards use smooth_landscape_in_box. Check vertices_modified in the reply: a zero means the box missed the terrain, which is what a wrongly-signed world coordinate looks like.
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @param delta_z - World Z delta to apply to every vertex in the region
//   @param box_min - Optional [x, y, z] world-space box minimum
//   @param box_max - Optional [x, y, z] world-space box maximum
//   @param center - Optional [x, y, z] box center (alternative to box_min/box_max)
//   @param extent - Optional [x, y, z] box half-extents (alternative to box_min/box_max)
//   @returns { landscape_actor_name: string, delta_z: number, delta_steps: int, applied: bool, vertices_modified: int, region_width: int, region_height: int, region: [int] }
// MCP:flatten_landscape_in_box - Level the terrain under a footprint: snap every heightmap vertex inside a world-space box to ONE absolute height, so a base, road or building pad sits flat. Give the height either as target_z in world units (converted using the landscape scale) or as a raw uint16 target_height (0..65535, where 32768 is the midpoint). Absolute, unlike set_landscape_height_in_box which adds a delta; follow with smooth_landscape_in_box to blend the hard rim it leaves. vertices_modified = 0 means the box never touched the terrain.
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @param target_height - Optional uint16 heightmap value 0 to 65535
//   @param target_z - Optional target world Z, converted to uint16 using landscape scale
//   @param box_min - Optional [x, y, z] world-space box minimum
//   @param box_max - Optional [x, y, z] world-space box maximum
//   @param center - Optional [x, y, z] box center (alternative to box_min/box_max)
//   @param extent - Optional [x, y, z] box half-extents (alternative to box_min/box_max)
//   @returns { landscape_actor_name: string, target_height: int, applied: bool, vertices_modified: int, region_width: int, region_height: int, region: [int] }
// MCP:smooth_landscape_in_box - Soften terrain: run a 3x3 box blur over the heightmap inside a world-space box, 1 to 5 iterations. The finisher for the other two sculpt tools -- flatten_landscape_in_box and set_landscape_height_in_box leave a hard rim, and a couple of iterations here blends it. It only averages what is there; it cannot add relief.
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @param iterations - Number of smoothing iterations 1 to 5
//   @param box_min - Optional [x, y, z] world-space box minimum
//   @param box_max - Optional [x, y, z] world-space box maximum
//   @param center - Optional [x, y, z] box center (alternative to box_min/box_max)
//   @param extent - Optional [x, y, z] box half-extents (alternative to box_min/box_max)
//   @returns { landscape_actor_name: string, iterations: int, applied: bool, vertices_smoothed: int, region_width: int, region_height: int, region: [int] }
// MCP:copy_landscape_region - Clone a rectangle of terrain relief: copy a heightmap region between two landscapes (or within one, by omitting the source name), resampling automatically when the source and destination rectangles differ in size. Regions are in VERTEX coordinates, not world units -- get the ranges from get_landscape_info (min_x/max_x, size_x). Use it to duplicate an island or reuse sculpted relief; use import_landscape_heightmap when the source is a file instead.
//   @param landscape_actor_name - Optional destination landscape actor name, defaults to first found
//   @param source_landscape_actor_name - Optional source landscape actor name, defaults to destination
//   @param source_region - Required [minX, minY, maxX, maxY] in source vertex coords
//   @param dest_region - Required [minX, minY, maxX, maxY] in destination vertex coords
//   @returns { source_landscape_actor_name: string, dest_landscape_actor_name: string, applied: bool, resampled: bool, source_width: int, source_height: int, dest_width: int, dest_height: int, source_region: [int], dest_region: [int] }
// MCP:import_landscape_heightmap - Load a PNG or 16-bit .r16 heightmap file from disk and stamp it onto an existing landscape: this is the reliable route to terrain relief, and the reason to prefer it over the editor's Import from File dialog, which can fill only part of the components and drop the peak with no warning at all. It does not negotiate resolution -- create the landscape at the file's own vertex size first (create_landscape) so resampled comes back false. success/applied is an acknowledgement, not proof: the oracle is export_landscape_heightmap plus an array compare against the source file, or get_landscape_height_at probes.
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @param file_path - Absolute filesystem path or /Game asset path of the heightmap
//   @param flip_y - Optional, flip the Y axis on import
//   @returns { landscape_actor_name: string, file_path: string, source_width: int, source_height: int, dest_width: int, dest_height: int, resampled: bool, applied: bool }
// MCP:export_landscape_heightmap - Dump the landscape heightmap (or a vertex sub-region) to a PNG or 16-bit .r16 file on disk -- THE oracle for any question about the whole terrain: whether an import really landed, where the real min/max heights are, whether a region is unfilled. Prefer it over sampling points with get_landscape_height_at when the question covers the whole surface: a handful of probes has repeatedly suggested flips and offsets that did not exist, while comparing the exported array against the source closes it in one step. An .r16 is exactly width*height*2 bytes, so a short file is itself a failure signal. Use get_landscape_height_at instead for a single world XY.
//   @param landscape_actor_name - Optional landscape actor name, defaults to first found
//   @param file_path - Absolute filesystem path for the exported heightmap
//   @param region - Optional [minX, minY, maxX, maxY] vertex region, defaults to whole landscape
//   @returns { landscape_actor_name: string, file_path: string, export_width: int, export_height: int, exported: bool, file_exists: bool, file_size: number, export_warning: string }
// MCP:get_landscape_height_at - How high is the ground here: sample the terrain surface height (world Z) at one world (x, y) -- the tool for placing an actor on the terrain, checking a spawn point is above water, or proving a region is not flat. Reads the heightmap texture directly (FHeightmapAccessor + bilinear interpolation), so unlike raycast_scene it needs NO collision, NO PIE and no cooked physics: it answers in the editor world where a line trace just misses the landscape. Honest at the edges -- outside the extent it clamps to the nearest vertex and says in_bounds=false, and it reports the (col,row) vertex it sampled so you can verify the pixel-to-world mapping. Use export_landscape_heightmap instead when the question is about the WHOLE terrain (min/max, unfilled regions, did an import land): a handful of probes has repeatedly suggested flips and offsets that were not there.
//   @param x - World-space X coordinate to sample
//   @param y - World-space Y coordinate to sample
//   @param landscape_actor_name - Optional landscape actor name or label, defaults to first found
//   @returns { landscape_actor_name: string, x: number, y: number, height: number, vertex: [int, int], in_bounds: bool }
TSharedPtr<FJsonObject> HandleSetLandscapeHeightInBox(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeInfo* Info = nullptr;
    int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSculptRect(Params, Landscape, Info, MinX, MinY, MaxX, MaxY))
    {
        return Error;
    }

    const double DeltaZ = static_cast<double>(Params->GetNumberField(TEXT("delta_z")));
    int32 DeltaSteps = 0;
    if (DeltaZ != 0.0 && !WorldZDeltaToUint16Step(Landscape, DeltaZ, DeltaSteps))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Landscape actor has degenerate Z scale; cannot apply delta_z"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetLandscapeHeightInBox", "Set Landscape Height In Box (MCP)"));
    Landscape->Modify();

    const int32 WidthVerts = (MaxX - MinX) + 1;
    const int32 HeightVerts = (MaxY - MinY) + 1;
    TArray<uint16> Data;
    Data.SetNumZeroed(WidthVerts * HeightVerts);

    // Route the read-modify-write to the landscape's active edit layer (UE 5.8) so
    // the write is not stomped by the edit-layer merge (see GetBaseEditLayerGuid).
    const FGuid EditLayerGuid = GetBaseEditLayerGuid(Landscape);
    {
        FScopedSetLandscapeEditingLayer LayerScope(Landscape, EditLayerGuid);
        FHeightmapAccessor<false> Accessor(Info);
        Accessor.GetDataFast(MinX, MinY, MaxX, MaxY, Data.GetData());
        if (DeltaSteps != 0)
        {
            for (int32 i = 0; i < Data.Num(); ++i)
            {
                const int32 V = static_cast<int32>(Data[i]) + DeltaSteps;
                Data[i] = static_cast<uint16>(FMath::Clamp(V, 0, 65535));
            }
            Accessor.SetData(MinX, MinY, MaxX, MaxY, Data.GetData(),
                             ELandscapeLayerPaintingRestriction::None);
            Accessor.Flush();
        }
    }
    // Recompose the final heightmap from the edit layers so a read-back reflects
    // the change (only when we actually wrote to a valid edit layer).
    if (DeltaSteps != 0 && EditLayerGuid.IsValid())
    {
        Landscape->ForceLayersFullUpdate();
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetNumberField(TEXT("delta_z"), DeltaZ);
    ResultObj->SetNumberField(TEXT("delta_steps"), DeltaSteps);
    ResultObj->SetBoolField(TEXT("applied"), DeltaSteps != 0);
    ResultObj->SetNumberField(TEXT("vertices_modified"), WidthVerts * HeightVerts);
    ResultObj->SetNumberField(TEXT("region_width"), WidthVerts);
    ResultObj->SetNumberField(TEXT("region_height"), HeightVerts);
    ResultObj->SetArrayField(TEXT("region"), Landscape_RectToJson(MinX, MinY, MaxX, MaxY));
    return mcp_common::MakeOkReply(ResultObj);
}

TSharedPtr<FJsonObject> HandleFlattenLandscapeInBox(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeInfo* Info = nullptr;
    int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSculptRect(Params, Landscape, Info, MinX, MinY, MaxX, MaxY))
    {
        return Error;
    }

    // Default target: the landscape midpoint (32768 == Z = 0). Callers can
    // pass target_z in WORLD units (the same scale that create_landscape
    // reports on `height` as a uint16 step; we convert here for clarity).
    uint16 TargetH = static_cast<uint16>(FMath::Clamp(
        static_cast<int32>(Params->GetNumberField(TEXT("target_height"))), 0, 65535));
    if (Params->HasField(TEXT("target_z")))
    {
        const double TargetZ = static_cast<double>(Params->GetNumberField(TEXT("target_z")));
        if (!WorldZToUint16(Landscape, TargetZ, TargetH))
        {
            return mcp_common::MakeErrorReply(
                TEXT("Landscape actor has degenerate Z scale; cannot apply target_z"));
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "FlattenLandscapeInBox", "Flatten Landscape In Box (MCP)"));
    Landscape->Modify();

    const int32 WidthVerts = (MaxX - MinX) + 1;
    const int32 HeightVerts = (MaxY - MinY) + 1;
    TArray<uint16> Data;
    Data.Init(TargetH, WidthVerts * HeightVerts);

    // Edit-layer-aware write (UE 5.8) -- see GetBaseEditLayerGuid.
    Landscape_WriteBaseLayerHeights(Landscape, Info, MinX, MinY, MaxX, MaxY, Data);

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetNumberField(TEXT("target_height"), TargetH);
    ResultObj->SetBoolField(TEXT("applied"), true);
    ResultObj->SetNumberField(TEXT("vertices_modified"), WidthVerts * HeightVerts);
    ResultObj->SetNumberField(TEXT("region_width"), WidthVerts);
    ResultObj->SetNumberField(TEXT("region_height"), HeightVerts);
    ResultObj->SetArrayField(TEXT("region"), Landscape_RectToJson(MinX, MinY, MaxX, MaxY));
    return mcp_common::MakeOkReply(ResultObj);
}

TSharedPtr<FJsonObject> HandleSmoothLandscapeInBox(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    ULandscapeInfo* Info = nullptr;
    int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveSculptRect(Params, Landscape, Info, MinX, MinY, MaxX, MaxY))
    {
        return Error;
    }

    // Iterations 1..5. 1 = single 3x3 box-blur, 5 = strong smoothing.
    const int32 Iterations = FMath::Clamp(
        static_cast<int32>(Params->GetNumberField(TEXT("iterations"))), 1, 5);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SmoothLandscapeInBox", "Smooth Landscape In Box (MCP)"));
    Landscape->Modify();

    const int32 WidthVerts = (MaxX - MinX) + 1;
    const int32 HeightVerts = (MaxY - MinY) + 1;
    TArray<uint16> Src;
    Src.SetNumZeroed(WidthVerts * HeightVerts);
    TArray<uint16> Dst;
    Dst.SetNumZeroed(WidthVerts * HeightVerts);

    // Edit-layer-aware read-modify-write (UE 5.8) -- see GetBaseEditLayerGuid.
    const FGuid EditLayerGuid = GetBaseEditLayerGuid(Landscape);
    {
        FScopedSetLandscapeEditingLayer LayerScope(Landscape, EditLayerGuid);
        FHeightmapAccessor<false> Accessor(Info);
        Accessor.GetDataFast(MinX, MinY, MaxX, MaxY, Src.GetData());
        for (int32 it = 0; it < Iterations; ++it)
        {
            // 3x3 uniform box-blur with edge replication (1/9 weighted average).
            for (int32 y = 0; y < HeightVerts; ++y)
            {
                for (int32 x = 0; x < WidthVerts; ++x)
                {
                    int32 Sum = 0;
                    for (int32 dy = -1; dy <= 1; ++dy)
                    {
                        const int32 sy = FMath::Clamp(y + dy, 0, HeightVerts - 1);
                        for (int32 dx = -1; dx <= 1; ++dx)
                        {
                            const int32 sx = FMath::Clamp(x + dx, 0, WidthVerts - 1);
                            Sum += Src[sy * WidthVerts + sx];
                        }
                    }
                    Dst[y * WidthVerts + x] = static_cast<uint16>((Sum + 4) / 9);
                }
            }
            // Swap buffers (the next iteration reads from Dst).
            Swap(Src, Dst);
        }
        Accessor.SetData(MinX, MinY, MaxX, MaxY, Src.GetData(),
                         ELandscapeLayerPaintingRestriction::None);
        Accessor.Flush();
    }
    if (EditLayerGuid.IsValid())
    {
        Landscape->ForceLayersFullUpdate();
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Landscape);
    ResultObj->SetNumberField(TEXT("iterations"), Iterations);
    ResultObj->SetBoolField(TEXT("applied"), true);
    ResultObj->SetNumberField(TEXT("vertices_smoothed"), WidthVerts * HeightVerts);
    ResultObj->SetNumberField(TEXT("region_width"), WidthVerts);
    ResultObj->SetNumberField(TEXT("region_height"), HeightVerts);
    ResultObj->SetArrayField(TEXT("region"), Landscape_RectToJson(MinX, MinY, MaxX, MaxY));
    return mcp_common::MakeOkReply(ResultObj);
}

TSharedPtr<FJsonObject> HandleCopyLandscapeRegion(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeHeightmapTarget Target;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveHeightmapTarget(Params, Target))
    {
        return Error;
    }

    // source region: same landscape by default; explicit source via
    // source_landscape_actor_name (must be the same actor or another ALandscape
    // with its own ULandscapeInfo).
    FString SourceName;
    Params->TryGetStringField(TEXT("source_landscape_actor_name"), SourceName);
    ALandscape* Source = SourceName.IsEmpty()
        ? Target.Landscape
        : FindLandscape(Target.World, SourceName);
    if (!Source)
    {
        return mcp_common::MakeErrorReply(
            SourceName.IsEmpty()
                ? TEXT("Source landscape not resolved")
                : FString::Printf(TEXT("Source landscape actor not found: %s"), *SourceName));
    }
    ULandscapeInfo* SourceInfo = Source->GetLandscapeInfo();
    if (!SourceInfo)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Source landscape has no ULandscapeInfo; cannot copy region"));
    }

    // Source/dest regions in landscape vertex coords.
    const TArray<TSharedPtr<FJsonValue>>* SrcArr = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* DstArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("source_region"), SrcArr) || !SrcArr || SrcArr->Num() < 4 ||
        !Params->TryGetArrayField(TEXT("dest_region"), DstArr) || !DstArr || DstArr->Num() < 4)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameters: source_region, dest_region (each [minX,minY,maxX,maxY] in landscape vertex coords)"));
    }
    int32 SrcMinX = static_cast<int32>((*SrcArr)[0]->AsNumber());
    int32 SrcMinY = static_cast<int32>((*SrcArr)[1]->AsNumber());
    int32 SrcMaxX = static_cast<int32>((*SrcArr)[2]->AsNumber());
    int32 SrcMaxY = static_cast<int32>((*SrcArr)[3]->AsNumber());
    int32 DstMinX = static_cast<int32>((*DstArr)[0]->AsNumber());
    int32 DstMinY = static_cast<int32>((*DstArr)[1]->AsNumber());
    int32 DstMaxX = static_cast<int32>((*DstArr)[2]->AsNumber());
    int32 DstMaxY = static_cast<int32>((*DstArr)[3]->AsNumber());
    if (SrcMaxX < SrcMinX) { Swap(SrcMinX, SrcMaxX); }
    if (SrcMaxY < SrcMinY) { Swap(SrcMinY, SrcMaxY); }
    if (DstMaxX < DstMinX) { Swap(DstMinX, DstMaxX); }
    if (DstMaxY < DstMinY) { Swap(DstMinY, DstMaxY); }

    const int32 SrcW = (SrcMaxX - SrcMinX) + 1;
    const int32 SrcH = (SrcMaxY - SrcMinY) + 1;
    const int32 DstW = (DstMaxX - DstMinX) + 1;
    const int32 DstH = (DstMaxY - DstMinY) + 1;

    TArray<uint16> SrcData;
    SrcData.SetNumZeroed(SrcW * SrcH);
    {
        // Read from the source landscape's base edit layer (UE 5.8) so the copy is
        // coherent with the edit-layer write below (see GetBaseEditLayerGuid).
        FScopedSetLandscapeEditingLayer SrcLayerScope(Source, GetBaseEditLayerGuid(Source));
        FHeightmapAccessor<false> Accessor(SourceInfo);
        Accessor.GetDataFast(SrcMinX, SrcMinY, SrcMaxX, SrcMaxY, SrcData.GetData());
    }

    // Resampled, source centred, when the dest rect differs in size from the source.
    const TArray<uint16> ApplyData = Landscape_FitHeights(SrcData, SrcW, SrcH, DstW, DstH);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "CopyLandscapeRegion", "Copy Landscape Region (MCP)"));
    Target.Landscape->Modify();
    // Edit-layer-aware write into the destination landscape (UE 5.8).
    Landscape_WriteBaseLayerHeights(Target.Landscape, Target.Info, DstMinX, DstMinY, DstMaxX, DstMaxY, ApplyData);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("source_landscape_actor_name"), Source->GetActorLabel());
    ResultObj->SetStringField(TEXT("dest_landscape_actor_name"), Target.Landscape->GetActorLabel());
    ResultObj->SetBoolField(TEXT("applied"), true);
    ResultObj->SetBoolField(TEXT("resampled"), !(SrcW == DstW && SrcH == DstH));
    ResultObj->SetNumberField(TEXT("source_width"), SrcW);
    ResultObj->SetNumberField(TEXT("source_height"), SrcH);
    ResultObj->SetNumberField(TEXT("dest_width"), DstW);
    ResultObj->SetNumberField(TEXT("dest_height"), DstH);
    ResultObj->SetArrayField(TEXT("source_region"), Landscape_RectToJson(SrcMinX, SrcMinY, SrcMaxX, SrcMaxY));
    ResultObj->SetArrayField(TEXT("dest_region"), Landscape_RectToJson(DstMinX, DstMinY, DstMaxX, DstMaxY));
    return mcp_common::MakeOkReply(ResultObj);
}

TSharedPtr<FJsonObject> HandleImportLandscapeHeightmap(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeHeightmapTarget Target;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveHeightmapTarget(Params, Target))
    {
        return Error;
    }

    FString FilePath;
    if (TSharedPtr<FJsonObject> Error = Landscape_ReadHeightmapFilePath(Params, FilePath))
    {
        return Error;
    }

    const bool bFlipYAxis = Params->HasField(TEXT("flip_y")) && Params->GetBoolField(TEXT("flip_y"));

    FText ErrMsg;
    FLandscapeImportDescriptor ImportDesc;
    const ELandscapeImportResult DescRes =
        FLandscapeImportHelper::GetHeightmapImportDescriptor(FilePath, /*bSingleFile*/ true, bFlipYAxis, ImportDesc, ErrMsg);
    if (DescRes != ELandscapeImportResult::Success)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to resolve heightmap import descriptor for %s: %s"),
                            *FilePath, *ErrMsg.ToString()));
    }

    TArray<uint16> ImportedData;
    const ELandscapeImportResult DataRes =
        FLandscapeImportHelper::GetHeightmapImportData(ImportDesc, /*DescriptorIndex*/ 0, ImportedData, ErrMsg);
    if (DataRes != ELandscapeImportResult::Success)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to read heightmap data from %s: %s"),
                            *FilePath, *ErrMsg.ToString()));
    }

    const int32 SrcW = static_cast<int32>(ImportDesc.ImportResolutions[0].Width);
    const int32 SrcH = static_cast<int32>(ImportDesc.ImportResolutions[0].Height);
    const int32 DstW = (Target.ExtMaxX - Target.ExtMinX) + 1;
    const int32 DstH = (Target.ExtMaxY - Target.ExtMinY) + 1;

    const TArray<uint16> ApplyData = Landscape_FitHeights(ImportedData, SrcW, SrcH, DstW, DstH);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ImportLandscapeHeightmap", "Import Landscape Heightmap (MCP)"));
    Target.Landscape->Modify();
    // Edit-layer-aware write (UE 5.8): create_landscape produces a landscape with
    // one edit layer, and FHeightmapAccessor<false> targets the landscape's active
    // editing layer. Without scoping the write to that layer the accessor writes to
    // the composite heightmap, which the edit-layer merge then RECOMPOSES from the
    // empty (midpoint) base layer -> the import is silently dropped and the terrain
    // reads back flat (Z=0 everywhere). Scope the write to the base edit layer, then
    // ForceLayersFullUpdate() to recompose the composite synchronously so a
    // subsequent get_landscape_height_at reflects the import. See GetBaseEditLayerGuid.
    const FGuid EditLayerGuid = Landscape_WriteBaseLayerHeights(Target.Landscape, Target.Info,
        Target.ExtMinX, Target.ExtMinY, Target.ExtMaxX, Target.ExtMaxY, ApplyData);

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Target.Landscape);
    ResultObj->SetStringField(TEXT("file_path"), FilePath);
    ResultObj->SetNumberField(TEXT("source_width"), SrcW);
    ResultObj->SetNumberField(TEXT("source_height"), SrcH);
    ResultObj->SetNumberField(TEXT("dest_width"), DstW);
    ResultObj->SetNumberField(TEXT("dest_height"), DstH);
    ResultObj->SetBoolField(TEXT("resampled"), !(SrcW == DstW && SrcH == DstH));
    ResultObj->SetBoolField(TEXT("applied"), true);
    // Diagnostics: whether the write was routed through an edit layer (UE 5.8). A
    // false here on a create_landscape-made landscape would mean the import went to
    // the composite and will be stomped -- the terrain then comes out flat.
    ResultObj->SetBoolField(TEXT("edit_layer_applied"), EditLayerGuid.IsValid());
    ResultObj->SetStringField(TEXT("edit_layer_guid"), EditLayerGuid.IsValid() ? EditLayerGuid.ToString() : FString());
    return mcp_common::MakeOkReply(ResultObj);
}

// The surface height comes straight from the heightmap (collision-free): see
// LandscapeHeightSampling::SampleHeightAt.
TSharedPtr<FJsonObject> HandleGetLandscapeHeightAt(const TSharedPtr<FJsonObject>& Params)
{
    if (!Params->HasField(TEXT("x")) || !Params->HasField(TEXT("y")))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required parameters: x and y (world coordinates)"));
    }
    const double WorldX = Params->GetNumberField(TEXT("x"));
    const double WorldY = Params->GetNumberField(TEXT("y"));

    FLandscapeHeightmapTarget Target;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveHeightmapTarget(Params, Target))
    {
        return Error;
    }

    LandscapeHeightSampling::FHeightSample Sample;
    if (!LandscapeHeightSampling::SampleHeightAt(Target.Landscape, Target.Info,
            Target.ExtMinX, Target.ExtMinY, Target.ExtMaxX, Target.ExtMaxY, WorldX, WorldY, Sample))
    {
        return mcp_common::MakeErrorReply(TEXT("Landscape has degenerate XY scale"));
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Target.Landscape);
    ResultObj->SetNumberField(TEXT("x"), WorldX);
    ResultObj->SetNumberField(TEXT("y"), WorldY);
    ResultObj->SetNumberField(TEXT("height"), Sample.WorldZ);
    ResultObj->SetArrayField(TEXT("vertex"), mcp_common::Vector2DToJsonArray(FIntPoint(Sample.VertexX, Sample.VertexY)));
    ResultObj->SetBoolField(TEXT("in_bounds"), Sample.bInBounds);
    return mcp_common::MakeOkReply(ResultObj);
}

TSharedPtr<FJsonObject> HandleExportLandscapeHeightmap(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeHeightmapTarget Target;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveHeightmapTarget(Params, Target))
    {
        return Error;
    }

    FString FilePath;
    if (TSharedPtr<FJsonObject> Error = Landscape_ReadHeightmapFilePath(Params, FilePath))
    {
        return Error;
    }

    // Optional region [minX,minY,maxX,maxY] in landscape vertex coords.
    int32 RegMinX = Target.ExtMinX, RegMinY = Target.ExtMinY, RegMaxX = Target.ExtMaxX, RegMaxY = Target.ExtMaxY;
    const TArray<TSharedPtr<FJsonValue>>* RegionArr = nullptr;
    if (Params->TryGetArrayField(TEXT("region"), RegionArr) && RegionArr && RegionArr->Num() >= 4)
    {
        RegMinX = static_cast<int32>((*RegionArr)[0]->AsNumber());
        RegMinY = static_cast<int32>((*RegionArr)[1]->AsNumber());
        RegMaxX = static_cast<int32>((*RegionArr)[2]->AsNumber());
        RegMaxY = static_cast<int32>((*RegionArr)[3]->AsNumber());
        RegMinX = FMath::Clamp(RegMinX, Target.ExtMinX, Target.ExtMaxX);
        RegMinY = FMath::Clamp(RegMinY, Target.ExtMinY, Target.ExtMaxY);
        RegMaxX = FMath::Clamp(RegMaxX, Target.ExtMinX, Target.ExtMaxX);
        RegMaxY = FMath::Clamp(RegMaxY, Target.ExtMinY, Target.ExtMaxY);
        if (RegMaxX < RegMinX) { Swap(RegMinX, RegMaxX); }
        if (RegMaxY < RegMinY) { Swap(RegMinY, RegMaxY); }
    }

    // ULandscapeInfo::ExportHeightmap(Filename, FIntRect) is LANDSCAPE_API and
    // routes to the engine's heightmap file-format module (PNG/.r16/.r16be) by
    // extension. The slow-task dialog it spawns is suppressed under
    // GIsRunningUnattendedScript (set by the bridge), so this stays headless-safe.
    Target.Info->ExportHeightmap(FilePath, FIntRect(RegMinX, RegMinY, RegMaxX, RegMaxY));

    // Honest verify: the engine's ExportHeightmap can silently no-op when the
    // destination directory doesn't exist or the path is malformed. Stat the
    // file and only report success when it actually exists with non-zero size.
    const bool bFileExists = IFileManager::Get().FileExists(*FilePath);
    const int64 FileSize = bFileExists ? IFileManager::Get().FileSize(*FilePath) : 0;

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_actor_name"), Target.Landscape);
    ResultObj->SetStringField(TEXT("file_path"), FilePath);
    ResultObj->SetNumberField(TEXT("export_width"), (RegMaxX - RegMinX) + 1);
    ResultObj->SetNumberField(TEXT("export_height"), (RegMaxY - RegMinY) + 1);
    ResultObj->SetBoolField(TEXT("exported"), bFileExists && FileSize > 0);
    ResultObj->SetBoolField(TEXT("file_exists"), bFileExists);
    ResultObj->SetNumberField(TEXT("file_size"), static_cast<double>(FileSize));
    if (!bFileExists)
    {
        ResultObj->SetStringField(TEXT("export_warning"),
            TEXT("ExportHeightmap returned without writing the file; check the "
                 "file_path is writable and the target directory exists"));
    }
    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// Landscape material (LANDSCAPE-EXT)
// ========================================================================== //
//
// ALandscapeProxy::LandscapeMaterial / LandscapeHoleMaterial are PUBLIC
// UPROPERTY(EditAnywhere) TObjectPtr<UMaterialInterface> members
// (LandscapeProxy.h/608). We assign them DIRECTLY and apply the change by
// constructing + dispatching PostEditChangeProperty (a virtual override of
// UObject, safe cross-module). The engine's ALandscapeProxy::PostEditChange-
// Property material branch handles BOTH the LandscapeMaterial and
// LandscapeHoleMaterial paths and internally rebuilds the material instances.
//
// LNK2019 TRAP: ALandscapeProxy::GetLandscapeMaterial()/GetLandscapeHoleMaterial()
// and EditorSetLandscapeMaterial() are naked (MinimalAPI, no LANDSCAPE_API) ->
// calling them cross-module is a link error. We therefore READ the UPROPERTY
// directly (which also returns an honest nullptr when no material is set, instead
// of the getter's fallback to the default material).

// MCP:set_landscape_material - Dress the terrain: assign the landscape's render material (and optionally its hole material) by asset path. Does MORE than the name suggests and that is the point -- it PROPAGATES to every LandscapeStreamingProxy, which is what actually renders on a World-Partition map, so you do not have to walk the proxies by hand. Consequence to budget for: it dirties one package per proxy plus the landscape. Idempotent (re-assigning the same material returns already_set), and the shader recompile is async even though the assignment is immediate. Do not trust material_assigned as the oracle -- re-read with get_landscape_material and spot-check a proxy's own landscape_material.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @param material_path - Asset path of the UMaterialInterface (e.g. "/Engine/EngineMaterials/WorldGridMaterial")
//   @param hole_material_path - Optional asset path of the UMaterialInterface used for components with holes
//   @returns { landscape_name: string, material_path: string, material_assigned: bool, already_set: bool, hole_material_path?: string, hole_material_assigned?: bool, note: string }
TSharedPtr<FJsonObject> HandleSetLandscapeMaterial(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByNameParam(Params, Landscape))
    {
        return Error;
    }

    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

    UMaterialInterface* NewMat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!NewMat)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
    }

    // Optional hole material.
    FString HoleMaterialPath;
    const bool bHasHole = Params->TryGetStringField(TEXT("hole_material_path"), HoleMaterialPath) && !HoleMaterialPath.IsEmpty();
    UMaterialInterface* NewHoleMat = nullptr;
    if (bHasHole)
    {
        NewHoleMat = LoadObject<UMaterialInterface>(nullptr, *HoleMaterialPath);
        if (!NewHoleMat)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Hole material not found: %s"), *HoleMaterialPath));
        }
    }

    // Idempotence: read the UPROPERTYs directly BEFORE writing.
    const bool bMatAlreadySet  = (Landscape->LandscapeMaterial == NewMat);
    const bool bHoleAlreadySet = !bHasHole || (Landscape->LandscapeHoleMaterial == NewHoleMat);
    const bool bAlreadySet = bMatAlreadySet && bHoleAlreadySet;

    if (!bAlreadySet)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetLandscapeMaterial", "Set Landscape Material (MCP)"));
        Landscape->Modify();

        // Direct UPROPERTY assignment (members are public).
        Landscape->LandscapeMaterial = NewMat;
        if (bHasHole)
        {
            Landscape->LandscapeHoleMaterial = NewHoleMat;
        }

        // Apply via PostEditChangeProperty (virtual UObject override, safe cross
        // module). The proxy's material branch covers BOTH LandscapeMaterial and
        // LandscapeHoleMaterial and calls UpdateAllComponentMaterialInstances.
        FProperty* MatProp = FindFieldChecked<FProperty>(ALandscapeProxy::StaticClass(), FName(TEXT("LandscapeMaterial")));
        FPropertyChangedEvent Ev(MatProp);
        Landscape->PostEditChangeProperty(Ev);
    }

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    ResultObj->SetStringField(TEXT("material_path"), NewMat->GetPathName());
    ResultObj->SetBoolField(TEXT("material_assigned"), Landscape->LandscapeMaterial == NewMat);
    ResultObj->SetBoolField(TEXT("already_set"), bAlreadySet);
    if (bHasHole)
    {
        ResultObj->SetStringField(TEXT("hole_material_path"), NewHoleMat->GetPathName());
        ResultObj->SetBoolField(TEXT("hole_material_assigned"), Landscape->LandscapeHoleMaterial == NewHoleMat);
    }
    ResultObj->SetStringField(TEXT("note"), TEXT("MIC/shader recompile is async on the render thread; the material data assignment is immediate and persisted"));

    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_landscape_material - Read which material the terrain is currently using: the landscape's assigned render material and hole material as asset paths (direct UPROPERTY read, null when unset). The read-back oracle for set_landscape_material. It reports the ALandscape actor only -- on a World-Partition map the LandscapeStreamingProxy actors carry their own copy, so to prove the whole terrain is dressed also read landscape_material on the proxies.
//   @param landscape_name - Optional landscape actor name, defaults to first found
//   @returns { landscape_name: string, material_path: string|null, hole_material_path: string|null }
TSharedPtr<FJsonObject> HandleGetLandscapeMaterial(const TSharedPtr<FJsonObject>& Params)
{
    ALandscape* Landscape = nullptr;
    if (TSharedPtr<FJsonObject> Error = Landscape_FindByNameParam(Params, Landscape))
    {
        return Error;
    }

    // Read the UPROPERTYs directly: the engine getter would mask an unset state by
    // returning the default material, so we report null honestly when unset.
    UMaterialInterface* Mat = Landscape->LandscapeMaterial;
    UMaterialInterface* HoleMat = Landscape->LandscapeHoleMaterial;

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewReplyData(TEXT("landscape_name"), Landscape);
    if (Mat)
    {
        ResultObj->SetStringField(TEXT("material_path"), Mat->GetPathName());
    }
    else
    {
        ResultObj->SetField(TEXT("material_path"), MakeShared<FJsonValueNull>());
    }
    if (HoleMat)
    {
        ResultObj->SetStringField(TEXT("hole_material_path"), HoleMat->GetPathName());
    }
    else
    {
        ResultObj->SetField(TEXT("hole_material_path"), MakeShared<FJsonValueNull>());
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// ========================================================================== //
// Landscape grass type (LANDSCAPE-EXT)
// ========================================================================== //
//
// ULandscapeGrassType (UCLASS BlueprintType, MinimalAPI, non-Abstract) is created
// + saved exactly like create_foliage_type (CreatePackage / NewObject /
// AssetRegistry::AssetCreated / SaveAssetPackage). Its tunables live in the public
// TArray<FGrassVariety> GrassVarieties; each FGrassVariety is addressed by index.
//
// Type traps (verified against LandscapeGrassType.h, UE 5.8):
//   GrassDensity      : FPerPlatformFloat -> author via .Default
//   StartCullDistance : FPerPlatformInt   -> author via .Default
//   EndCullDistance   : FPerPlatformInt   -> author via .Default
//   ScaleX/Y/Z        : FFloatInterval    -> .Min / .Max
//   Scaling           : EGrassScaling enum (Uniform/Free/LockXY)
//   PlacementJitter   : float ; MinLOD : int32 ; the rest are plain bools.
// FGrassVariety() ctor is LANDSCAPE_API (exported) so instancing is safe.

namespace
{
    // Scaling enum <-> string (matches EGrassScaling in LandscapeGrassType.h).
    const TCHAR* GrassScalingToString(EGrassScaling Scaling)
    {
        switch (Scaling)
        {
            case EGrassScaling::Uniform: return TEXT("Uniform");
            case EGrassScaling::Free:    return TEXT("Free");
            case EGrassScaling::LockXY:  return TEXT("LockXY");
            default:                     return TEXT("Uniform");
        }
    }

    bool GrassScalingFromString(const FString& In, EGrassScaling& Out)
    {
        if (In.Equals(TEXT("Uniform"), ESearchCase::IgnoreCase)) { Out = EGrassScaling::Uniform; return true; }
        if (In.Equals(TEXT("Free"),    ESearchCase::IgnoreCase)) { Out = EGrassScaling::Free;    return true; }
        if (In.Equals(TEXT("LockXY"),  ESearchCase::IgnoreCase)) { Out = EGrassScaling::LockXY;  return true; }
        return false;
    }

    // Serialize one FGrassVariety to JSON (shared by set/get props read-back).
    void MakeGrassVarietyJson(const FGrassVariety& V, TSharedPtr<FJsonObject>& Data)
    {
        Data->SetStringField(TEXT("grass_mesh"), V.GrassMesh ? V.GrassMesh->GetPathName() : TEXT("None"));
        Data->SetNumberField(TEXT("grass_density"), V.GrassDensity.Default);
        Data->SetNumberField(TEXT("placement_jitter"), V.PlacementJitter);
        Data->SetNumberField(TEXT("start_cull_distance"), V.StartCullDistance.Default);
        Data->SetNumberField(TEXT("end_cull_distance"), V.EndCullDistance.Default);
        Data->SetNumberField(TEXT("min_lod"), V.MinLOD);
        Data->SetStringField(TEXT("scaling"), GrassScalingToString(V.Scaling));

        Data->SetObjectField(TEXT("scale_x"), Landscape_IntervalToJson(V.ScaleX));
        Data->SetObjectField(TEXT("scale_y"), Landscape_IntervalToJson(V.ScaleY));
        Data->SetObjectField(TEXT("scale_z"), Landscape_IntervalToJson(V.ScaleZ));

        Data->SetBoolField(TEXT("random_rotation"), V.RandomRotation);
        Data->SetBoolField(TEXT("align_to_surface"), V.AlignToSurface);
        Data->SetBoolField(TEXT("use_grid"), V.bUseGrid);
        Data->SetBoolField(TEXT("cast_dynamic_shadow"), V.bCastDynamicShadow);
        Data->SetBoolField(TEXT("receives_decals"), V.bReceivesDecals);
        Data->SetBoolField(TEXT("keep_instance_buffer_cpu_copy"), V.bKeepInstanceBufferCPUCopy);
    }

    // A reply payload describing variety VarietyIndex of GrassType: asset_path, grass_class, variety_index,
    // variety_count and the variety's own fields.
    TSharedPtr<FJsonObject> Landscape_NewGrassVarietyData(const ULandscapeGrassType* GrassType, int32 VarietyIndex)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("asset_path"), GrassType->GetPathName());
        Data->SetStringField(TEXT("grass_class"), GrassType->GetClass()->GetName());
        Data->SetNumberField(TEXT("variety_index"), VarietyIndex);
        Data->SetNumberField(TEXT("variety_count"), GrassType->GrassVarieties.Num());
        MakeGrassVarietyJson(GrassType->GrassVarieties[VarietyIndex], Data);
        return Data;
    }

    // Apply the provided fields from Params onto a FGrassVariety, returning the
    // number applied. Shared by create (initial variety) + set props. Returns -1 +
    // OutError on an invalid scaling value or unresolved mesh.
    int32 ApplyGrassVarietyFromParams(const TSharedPtr<FJsonObject>& Params, FGrassVariety& V, FString& OutError)
    {
        int32 Applied = 0;

        FString MeshPath;
        if (Params->TryGetStringField(TEXT("grass_mesh"), MeshPath) && !MeshPath.IsEmpty())
        {
            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
            if (!Mesh)
            {
                OutError = FString::Printf(TEXT("Grass mesh not found: %s"), *MeshPath);
                return -1;
            }
            V.GrassMesh = Mesh;
            ++Applied;
        }

        double Num = 0.0;
        if (Params->TryGetNumberField(TEXT("grass_density"), Num)) { V.GrassDensity.Default = (float)Num; ++Applied; }
        if (Params->TryGetNumberField(TEXT("placement_jitter"), Num)) { V.PlacementJitter = (float)Num; ++Applied; }
        if (Params->TryGetNumberField(TEXT("start_cull_distance"), Num)) { V.StartCullDistance.Default = (int32)Num; ++Applied; }
        if (Params->TryGetNumberField(TEXT("end_cull_distance"), Num)) { V.EndCullDistance.Default = (int32)Num; ++Applied; }
        if (Params->TryGetNumberField(TEXT("min_lod"), Num)) { V.MinLOD = (int32)Num; ++Applied; }

        FString ScalingStr;
        if (Params->TryGetStringField(TEXT("scaling"), ScalingStr) && !ScalingStr.IsEmpty())
        {
            EGrassScaling NewScaling = EGrassScaling::Uniform;
            if (!GrassScalingFromString(ScalingStr, NewScaling))
            {
                OutError = FString::Printf(TEXT("Invalid scaling '%s' (expected Uniform/Free/LockXY)"), *ScalingStr);
                return -1;
            }
            V.Scaling = NewScaling;
            ++Applied;
        }

        if (Landscape_ReadIntervalParam(Params, TEXT("scale_x"), V.ScaleX)) { ++Applied; }
        if (Landscape_ReadIntervalParam(Params, TEXT("scale_y"), V.ScaleY)) { ++Applied; }
        if (Landscape_ReadIntervalParam(Params, TEXT("scale_z"), V.ScaleZ)) { ++Applied; }

        bool b = false;
        if (Params->TryGetBoolField(TEXT("random_rotation"), b)) { V.RandomRotation = b; ++Applied; }
        if (Params->TryGetBoolField(TEXT("align_to_surface"), b)) { V.AlignToSurface = b; ++Applied; }
        if (Params->TryGetBoolField(TEXT("use_grid"), b)) { V.bUseGrid = b; ++Applied; }
        if (Params->TryGetBoolField(TEXT("cast_dynamic_shadow"), b)) { V.bCastDynamicShadow = b; ++Applied; }
        if (Params->TryGetBoolField(TEXT("receives_decals"), b)) { V.bReceivesDecals = b; ++Applied; }
        if (Params->TryGetBoolField(TEXT("keep_instance_buffer_cpu_copy"), b)) { V.bKeepInstanceBufferCPUCopy = b; ++Applied; }

        return Applied;
    }
}

// MCP:create_landscape_grass_type - Create and save a LandscapeGrassType asset, optionally seeding its first grass variety with a mesh. This is landscape grass, not foliage: instances are generated by the terrain MATERIAL through a LandscapeGrassOutput node, so the asset alone renders nothing until it is wired there -- and unlike the foliage scatter tools, this path does not crash the editor. Idempotent (an existing asset comes back with already_existed). Tune it with set_landscape_grass_type_properties; the honest check that the wiring resolved is ULandscapeComponent.get_grass_types_bp(), since counting grass instances headless reads 0 even when the player sees grass.
//   @param name - Asset name, e.g. GT_Field
//   @param package_path - Target content path, defaults to /Game/Foliage
//   @param grass_mesh - Optional StaticMesh path; if provided, one initial FGrassVariety is added with this mesh
//   @returns { name: string, package_path: string, asset_path: string, grass_class: string, variety_count: int, created: bool, already_existed: bool, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateLandscapeGrassType(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), AssetName)) { return Missing; }

    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath) || PackagePath.IsEmpty())
    {
        PackagePath = TEXT("/Game/Foliage");
    }

    // Optional initial grass mesh -> seeds one FGrassVariety.
    FString GrassMeshPath;
    const bool bHasMesh = Params->TryGetStringField(TEXT("grass_mesh"), GrassMeshPath) && !GrassMeshPath.IsEmpty();
    UStaticMesh* GrassMesh = nullptr;
    if (bHasMesh)
    {
        GrassMesh = LoadObject<UStaticMesh>(nullptr, *GrassMeshPath);
        if (!GrassMesh)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Grass mesh not found: %s"), *GrassMeshPath));
        }
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: load + return if it already exists. Resolve via LoadObject (registry-independent)
    // — NOT UEditorAssetLibrary::DoesAssetExist, which is unreliable in headless for an asset created
    // earlier this session (asset registry not synced) and would break idempotency under accumulated state.
    if (UObject* ExistingAny = LoadObject<UObject>(nullptr, *FullPath))
    {
        ULandscapeGrassType* Existing = Cast<ULandscapeGrassType>(ExistingAny);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ExistObj = Landscape_NewAssetData(AssetName, PackagePath, Existing);
            ExistObj->SetStringField(TEXT("grass_class"), Existing->GetClass()->GetName());
            ExistObj->SetNumberField(TEXT("variety_count"), Existing->GrassVarieties.Num());
            ExistObj->SetBoolField(TEXT("created"), false);
            ExistObj->SetBoolField(TEXT("already_existed"), true);
            ExistObj->SetBoolField(TEXT("saved"), true);
            ExistObj->SetStringField(TEXT("message"), TEXT("LandscapeGrassType already existed"));
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset already exists at %s but is not a LandscapeGrassType"), *FullPath));
    }

    FString CreateError;
    ULandscapeGrassType* GrassType = mcp_common::NewAssetInPackage<ULandscapeGrassType>(
        FullPath, AssetName, TEXT("Failed to create ULandscapeGrassType"), CreateError);
    if (!GrassType)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // Optionally seed an initial variety with the provided mesh.
    if (bHasMesh)
    {
        FGrassVariety Variety;
        Variety.GrassMesh = GrassMesh;
        GrassType->GrassVarieties.Add(Variety);
    }

    GrassType->PostEditChange();   // refresh StateHash before saving
    FAssetRegistryModule::AssetCreated(GrassType);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GrassType->GetOutermost(), GrassType, SaveError);

    TSharedPtr<FJsonObject> ResultObj = Landscape_NewAssetData(AssetName, PackagePath, GrassType);
    ResultObj->SetStringField(TEXT("grass_class"), GrassType->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("variety_count"), GrassType->GrassVarieties.Num());
    ResultObj->SetBoolField(TEXT("created"), true);
    ResultObj->SetBoolField(TEXT("already_existed"), false);
    ResultObj->SetStringField(TEXT("message"), bSaved
        ? TEXT("LandscapeGrassType created and saved to disk")
        : FString::Printf(TEXT("LandscapeGrassType created but save failed: %s"), *SaveError));

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("create_landscape_grass_type"), SaveError);
}

// MCP:set_landscape_grass_type_properties - Set fields of one grass variety of a LandscapeGrassType asset, only those you pass, and save it; an empty variety list gets one appended for index 0, and density and cull distances change only their per-platform Default value. Returns the read-back variety with applied_count and saved; read it with get_landscape_grass_type_properties.
//   @param grass_path - Asset path of the LandscapeGrassType, e.g. "/Game/Foliage/GT_Field".
//   @param variety_index [integer] - Index of the FGrassVariety to edit, default 0 (appends if the array is empty)
//   @param grass_mesh - Path of the StaticMesh for this variety, e.g. "/Game/Foliage/SM_Grass"; an unknown path fails the call.
//   @param grass_density - Instances per 10 square metres (25 means 2.5 per m2), so a wrong guess is a 10x error in the instance budget.
//   @param placement_jitter - Jitter of the grid placement from 0 to 1; only used when use_grid is true.
//   @param start_cull_distance - Distance in cm where instances start to fade, an integer; it does nothing unless the grass material uses a PerInstanceFadeAmount node, and 0 disables it.
//   @param end_cull_distance - Distance in cm beyond which a whole grass cluster is culled, an integer.
//   @param min_lod - Smallest mesh LOD to render, from -1 to 8; -1 uses the MinLOD of the static mesh asset.
//   @param scaling - Scale mode: Uniform, Free or LockXY, case-insensitive; any other value fails the call.
//   @param scale_x [object] - X scale range as a JSON object {"min":0.8,"max":1.2}, also used for every axis with Uniform scaling.
//   @param scale_y [object] - Y scale range as a JSON object {"min":0.8,"max":1.2}, used only with Free scaling.
//   @param scale_z [object] - Z scale range as a JSON object {"min":0.8,"max":1.2}, used with Free or LockXY scaling.
//   @param random_rotation - Give each instance a random rotation instead of a shared one.
//   @param align_to_surface - Tilt instances to the landscape normal instead of keeping them vertical.
//   @param use_grid - Place instances on a jittered grid instead of a Halton sequence.
//   @param cast_dynamic_shadow - Whether instances cast shadows with non-precomputed (dynamic) shadowing.
//   @param receives_decals - Whether the grass instances receive decals.
//   @param keep_instance_buffer_cpu_copy - Keep a CPU copy of the instance buffer, which the overlap-count queries of the grass component need.
//   @returns { asset_path, grass_class, variety_index, variety_count, grass_mesh, grass_density, placement_jitter, start_cull_distance, end_cull_distance, min_lod, scaling, scale_x:{min,max}, scale_y, scale_z, random_rotation, align_to_surface, use_grid, cast_dynamic_shadow, receives_decals, keep_instance_buffer_cpu_copy, applied_count, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetLandscapeGrassTypeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString GrassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("grass_path"), GrassPath)) { return Missing; }

    ULandscapeGrassType* GrassType =
        Cast<ULandscapeGrassType>(StaticLoadObject(ULandscapeGrassType::StaticClass(), nullptr, *GrassPath));
    if (!GrassType)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("LandscapeGrassType not found: %s"), *GrassPath));
    }

    const int32 VarietyIndex = ParamInt(Params, TEXT("variety_index"), 0);
    if (VarietyIndex < 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid variety_index %d (must be >= 0)"), VarietyIndex));
    }

    GrassType->Modify();

    // Address the variety by index. If the array is empty (fresh grass type with no
    // mesh), append one so the props have somewhere to land. An out-of-range index
    // on a non-empty array is an honest error.
    if (GrassType->GrassVarieties.Num() == 0 && VarietyIndex == 0)
    {
        GrassType->GrassVarieties.Add(FGrassVariety());
    }
    if (!GrassType->GrassVarieties.IsValidIndex(VarietyIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid variety_index %d (count=%d)"), VarietyIndex, GrassType->GrassVarieties.Num()));
    }

    FGrassVariety& Variety = GrassType->GrassVarieties[VarietyIndex];

    FString ApplyError;
    const int32 Applied = ApplyGrassVarietyFromParams(Params, Variety, ApplyError);
    if (Applied < 0)
    {
        return mcp_common::MakeErrorReply(ApplyError);
    }

    GrassType->PostEditChange();   // refresh StateHash

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(GrassType->GetOutermost(), GrassType, SaveError);

    TSharedPtr<FJsonObject> Data = Landscape_NewGrassVarietyData(GrassType, VarietyIndex);
    Data->SetNumberField(TEXT("applied_count"), Applied);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_landscape_grass_type_properties"), SaveError);
}

// MCP:get_landscape_grass_type_properties - Read one grass variety of a LandscapeGrassType asset: mesh, density, placement jitter, start/end cull distance, min LOD, scaling mode and scale ranges, rotation/alignment and shadow flags. The read-back oracle for set_landscape_grass_type_properties. Two units that mislead: grass_density is instances per TEN square metres (25 means 2.5/m2), and start_cull_distance only fades if the mesh's material uses a PerInstanceFadeAmount node -- otherwise clusters pop at end_cull_distance and the start value does nothing.
//   @param grass_path - Asset path of the ULandscapeGrassType
//   @param variety_index [integer!] - Index of the FGrassVariety to read, default 0
//   @returns { asset_path, grass_class, variety_index, variety_count, grass_mesh, grass_density, placement_jitter, start_cull_distance, end_cull_distance, min_lod, scaling, scale_x:{min,max}, scale_y, scale_z, random_rotation, align_to_surface, use_grid, cast_dynamic_shadow, receives_decals, keep_instance_buffer_cpu_copy }
TSharedPtr<FJsonObject> HandleGetLandscapeGrassTypeProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString GrassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("grass_path"), GrassPath)) { return Missing; }

    ULandscapeGrassType* GrassType =
        Cast<ULandscapeGrassType>(StaticLoadObject(ULandscapeGrassType::StaticClass(), nullptr, *GrassPath));
    if (!GrassType)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("LandscapeGrassType not found: %s"), *GrassPath));
    }

    const int32 VarietyIndex = ParamInt(Params, TEXT("variety_index"), 0);
    if (!GrassType->GrassVarieties.IsValidIndex(VarietyIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid variety_index %d (count=%d)"), VarietyIndex, GrassType->GrassVarieties.Num()));
    }

    TSharedPtr<FJsonObject> Data = Landscape_NewGrassVarietyData(GrassType, VarietyIndex);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Foliage layer-aware scatter + radius-based queries/cleanup.

// ---------------------------------------------------------------------------
// scatter_foliage_on_layer
// ---------------------------------------------------------------------------

// MCP:scatter_foliage_on_layer - Scatter foliage only where a landscape PAINT LAYER is strong enough: samples seeded candidates in a box, keeps those whose layer weight clears layer_weight_threshold, and rejects the rest -- so trees land on the forest layer and not on rock. The rule-driven sibling of scatter_foliage_in_box. DANGER: it reaches the same FFoliageInfo::AddInstance path that can crash the editor from its two sibling tools, so treat it as unsafe and prefer add_instanced_static_mesh_component + add_ism_instance (query the layer weight yourself) or a PCG graph. Save your work before calling it.
//   @param foliage_type_path - Asset path of the UFoliageType (e.g. /Game/Foliage/BP_Tree)
//   @param landscape_name_or_label - Actor label (or name) of the target ALandscape
//   @param layer_name - FName of the landscape paint layer (e.g. "Grass", "Forest")
//   @param center - Optional [x,y,z] world center of the scatter box (default 0,0,0)
//   @param extent - Optional [x,y,z] half-extent of the scatter box (default 500,500,0)
//   @param count - Number of candidate positions to sample (default 16)
//   @param layer_weight_threshold - Minimum layer weight [0,1] for placement (default 0.5)
//   @param seed - Random seed for deterministic scatter (default 1337)
//   @param align_to_ground - When true, candidates placed at box-top Z so ground snap is meaningful
//   @returns { applied, requested_count, instances_added, instances_rejected }
TSharedPtr<FJsonObject> HandleScatterFoliageOnLayer(const TSharedPtr<FJsonObject>& Params)
{
    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    FString FoliageTypePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("foliage_type_path"), FoliageTypePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString LandscapeId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("landscape_name_or_label"), LandscapeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    FString LayerNameStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("layer_name"), LayerNameStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UFoliageType* FoliageType =
        Cast<UFoliageType>(StaticLoadObject(UFoliageType::StaticClass(), nullptr, *FoliageTypePath));
    if (!FoliageType)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("FoliageType not found: %s"), *FoliageTypePath));
    }

    // Find the landscape actor by label or name.
    ALandscape* Landscape = nullptr;
    for (TActorIterator<ALandscape> It(World); It; ++It)
    {
        if (It->GetActorLabel() == LandscapeId || It->GetName() == LandscapeId)
        {
            Landscape = *It;
            break;
        }
    }
    if (!Landscape)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Landscape actor not found: %s"), *LandscapeId));
    }

    ULandscapeInfo* LandInfo = Landscape->GetLandscapeInfo();
    if (!LandInfo)
    {
        return mcp_common::MakeErrorReply(TEXT("Landscape has no LandscapeInfo"));
    }

    ULandscapeLayerInfoObject* LayerInfoObj = LandInfo->GetLayerInfoByName(FName(*LayerNameStr), Landscape);
    if (!LayerInfoObj)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Layer '%s' not found on landscape"), *LayerNameStr));
    }

    const FVector Center = Params->HasField(TEXT("center"))
        ? mcp_common::ReadJsonVector(Params, TEXT("center"))
        : FVector::ZeroVector;
    const FVector Extent = Params->HasField(TEXT("extent"))
        ? mcp_common::ReadJsonVector(Params, TEXT("extent"))
        : FVector(500.0, 500.0, 0.0);

    const int32 Count = FMath::Max(0, ParamInt(Params, TEXT("count"), 16));
    double ThresholdDouble = 0.5;
    Params->TryGetNumberField(TEXT("layer_weight_threshold"), ThresholdDouble);
    const float Threshold = (float)FMath::Clamp(ThresholdDouble, 0.0, 1.0);
    const int32 Seed = ParamInt(Params, TEXT("seed"), 1337);
    bool bAlignToGround = false;
    Params->TryGetBoolField(TEXT("align_to_ground"), bAlignToGround);

    // Get the IFA + count before.
    AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, /*bCreateIfNone*/ true, World->PersistentLevel);
    const int32 CountBefore = Landscape_FoliageInstanceCount(IFA, FoliageType);

    // Sample candidates; for each, walk the landscape components to find the one
    // whose world bounds contain the candidate XY, and call GetLayerWeightAtLocation.
    int32 InstancesAdded = 0;
    int32 InstancesRejected = 0;
    TArray<FTransform> AcceptedTransforms;

    FRandomStream RandomStream(Seed);
    for (int32 i = 0; i < Count; ++i)
    {
        const FVector WorldLoc = Landscape_RandomPointInBox(RandomStream, Center, Extent, bAlignToGround);

        // Find the landscape component that contains this world location by iterating
        // the proxy's LandscapeComponents and testing their world-space bounds.
        ULandscapeComponent* HitComp = nullptr;
        for (ULandscapeComponent* Comp : Landscape->LandscapeComponents)
        {
            if (!Comp) { continue; }
            const FBoxSphereBounds Bounds = Comp->Bounds;
            if (WorldLoc.X >= Bounds.Origin.X - Bounds.BoxExtent.X
                && WorldLoc.X <= Bounds.Origin.X + Bounds.BoxExtent.X
                && WorldLoc.Y >= Bounds.Origin.Y - Bounds.BoxExtent.Y
                && WorldLoc.Y <= Bounds.Origin.Y + Bounds.BoxExtent.Y)
            {
                HitComp = Comp;
                break;
            }
        }
        if (!HitComp)
        {
            ++InstancesRejected;
            continue;
        }

        const float Weight = HitComp->GetLayerWeightAtLocation(WorldLoc, LayerInfoObj);
        if (Weight < Threshold)
        {
            ++InstancesRejected;
            continue;
        }

        const double Yaw = RandomStream.FRandRange(0.0, 360.0);
        AcceptedTransforms.Add(FTransform(FRotator(0.0, Yaw, 0.0), WorldLoc, FVector::OneVector));
    }

    if (AcceptedTransforms.Num() > 0 && IFA)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ScatterFoliageOnLayer", "Scatter Foliage On Layer (MCP)"));
        IFA->Modify();
        if (Landscape_AddFoliageInstances(IFA, FoliageType, AcceptedTransforms))
        {
            InstancesAdded = AcceptedTransforms.Num();
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), InstancesAdded > 0);
    Data->SetNumberField(TEXT("requested_count"), Count);
    Data->SetNumberField(TEXT("instances_added"), InstancesAdded);
    Data->SetNumberField(TEXT("instances_rejected"), InstancesRejected);
    Data->SetNumberField(TEXT("layer_weight_threshold"), Threshold);
    Data->SetStringField(TEXT("layer_name"), LayerNameStr);
    Data->SetNumberField(TEXT("count_before"), CountBefore);
    Data->SetNumberField(TEXT("count_after"), CountBefore + InstancesAdded);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_foliage_in_radius
// ---------------------------------------------------------------------------

// MCP:remove_foliage_in_radius - Clear foliage around a point: delete every instance of one FoliageType inside a world-space sphere -- the way to carve a building site, road or clearing out of existing vegetation without wiping the whole type. Use remove_all_foliage_for_type to remove it everywhere instead. Preview what would go with get_foliage_in_radius first; the oracle is instances_removed plus a re-count.
//   @param foliage_type_path - Asset path of the UFoliageType to remove (matches against mesh on the type)
//   @param center_x - X component of the world-space center of the removal sphere
//   @param center_y - Y component of the world-space center of the removal sphere
//   @param center_z - Z component of the world-space center of the removal sphere
//   @param radius - Radius in world units
//   @returns { applied, instances_removed, requested_radius }
TSharedPtr<FJsonObject> HandleRemoveFoliageInRadius(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    FVector Center = FVector::ZeroVector;
    double RadiusNum = 0.0;
    if (TSharedPtr<FJsonObject> SphereError = Landscape_ReadFoliageSphere(Params, Center, RadiusNum))
    {
        return SphereError;
    }
    const float Radius = (float)RadiusNum;
    const float RadiusSq = Radius * Radius;

    AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(Foliage.World, /*bCreateIfNone*/ false, Foliage.World->PersistentLevel);
    FFoliageInfo* Info = IFA ? IFA->FindInfo(Foliage.Type) : nullptr;
    if (!Info || Info->Instances.Num() == 0)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("applied"), true);
        Data->SetNumberField(TEXT("instances_removed"), 0);
        Data->SetNumberField(TEXT("requested_radius"), Radius);
        return mcp_common::MakeOkReply(Data);
    }

    // Collect indices to remove (descending order so RemoveAtSwap is stable).
    TArray<int32> ToRemove;
    ToRemove.Reserve(Info->Instances.Num());
    for (int32 i = 0; i < Info->Instances.Num(); ++i)
    {
        const FVector InstanceLoc = Info->Instances[i].Location;
        if (FVector::DistSquared(InstanceLoc, Center) <= RadiusSq)
        {
            ToRemove.Add(i);
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveFoliageInRadius", "Remove Foliage In Radius (MCP)"));
    IFA->Modify();
    if (ToRemove.Num() > 0)
    {
        // UE 5.8 FFoliageInfo::RemoveInstances takes TArrayView<const int32>.
        Info->RemoveInstances(ToRemove, /*RebuildFoliageTree=*/true);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetNumberField(TEXT("instances_removed"), ToRemove.Num());
    Data->SetNumberField(TEXT("requested_radius"), Radius);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_foliage_in_radius
// ---------------------------------------------------------------------------

// MCP:get_foliage_in_radius - List the actual foliage instances of one FoliageType inside a world-space sphere, with each one's location, rotation, scale and instance_index. Use it to check what is standing on a spot before building there, or as a dry run before remove_foliage_in_radius. Capped by max_results (default 100, hard cap 1000), so a full count must come from get_foliage_instance_count, not from len(instances).
//   @param foliage_type_path - Asset path of the UFoliageType to query
//   @param center_x - X component of the world-space center of the query sphere
//   @param center_y - Y component of the world-space center of the query sphere
//   @param center_z - Z component of the world-space center of the query sphere
//   @param radius - Radius in world units
//   @param max_results - Maximum entries returned (default 100, hard cap 1000)
//   @returns { count, instances: [{location, rotation, scale, instance_index}] }
TSharedPtr<FJsonObject> HandleGetFoliageInRadius(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    FVector Center = FVector::ZeroVector;
    double RadiusNum = 0.0;
    if (TSharedPtr<FJsonObject> SphereError = Landscape_ReadFoliageSphere(Params, Center, RadiusNum))
    {
        return SphereError;
    }
    const float RadiusSq = (float)(RadiusNum * RadiusNum);
    const int32 MaxResults = FMath::Clamp(ParamInt(Params, TEXT("max_results"), 100), 1, 1000);

    AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(Foliage.World, /*bCreateIfNone*/ false, Foliage.World->PersistentLevel);

    TArray<TSharedPtr<FJsonValue>> Results;
    int32 Total = 0;

    if (IFA)
    {
        if (const FFoliageInfo* Info = IFA->FindInfo(Foliage.Type))
        {
            for (int32 i = 0; i < Info->Instances.Num() && Results.Num() < MaxResults; ++i)
            {
                const FVector InstanceLoc = Info->Instances[i].Location;
                if (FVector::DistSquared(InstanceLoc, Center) <= RadiusSq)
                {
                    TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                    Entry->SetNumberField(TEXT("instance_index"), i);
                    Entry->SetNumberField(TEXT("x"), InstanceLoc.X);
                    Entry->SetNumberField(TEXT("y"), InstanceLoc.Y);
                    Entry->SetNumberField(TEXT("z"), InstanceLoc.Z);
                    Entry->SetNumberField(TEXT("pitch"), Info->Instances[i].Rotation.Pitch);
                    Entry->SetNumberField(TEXT("yaw"), Info->Instances[i].Rotation.Yaw);
                    Entry->SetNumberField(TEXT("roll"), Info->Instances[i].Rotation.Roll);
                    Entry->SetNumberField(TEXT("scale_x"), Info->Instances[i].DrawScale3D.X);
                    Entry->SetNumberField(TEXT("scale_y"), Info->Instances[i].DrawScale3D.Y);
                    Entry->SetNumberField(TEXT("scale_z"), Info->Instances[i].DrawScale3D.Z);
                    Results.Add(MakeShared<FJsonValueObject>(Entry));
                }
                ++Total;
            }
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("count"), Results.Num());
    Data->SetNumberField(TEXT("total_scanned"), Total);
    Data->SetArrayField(TEXT("instances"), Results);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// foliage_type_exists
// ---------------------------------------------------------------------------

// MCP:foliage_type_exists - Yes/no guard: does this FoliageType asset path exist and load? Cheap precondition check before a create/scatter/tune call so a typo in the path fails here instead of deep inside another tool. Asks about the ASSET only -- has_foliage_instances answers whether anything of it is placed in the level.
//   @param foliage_type_path - Asset path (e.g. /Game/Foliage/BP_Tree)
//   @returns { exists: bool, asset_path: string }
//   @hints read-only
TSharedPtr<FJsonObject> HandleFoliageTypeExists(const TSharedPtr<FJsonObject>& Params)
{
    FString FoliageTypePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("foliage_type_path"), FoliageTypePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UFoliageType* FoliageType =
        Cast<UFoliageType>(StaticLoadObject(UFoliageType::StaticClass(), nullptr, *FoliageTypePath));

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("exists"), FoliageType != nullptr);
    Data->SetStringField(TEXT("asset_path"), FoliageTypePath);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// has_foliage_instances
// ---------------------------------------------------------------------------

// MCP:has_foliage_instances - Yes/no: does this FoliageType have any instances placed in the current level (with the count alongside)? The one-line gate for "is this vegetation actually in the map"; foliage_type_exists answers the different question of whether the asset exists at all, and get_foliage_in_radius says where the instances are.
//   @param foliage_type_path - Asset path of the UFoliageType to query
//   @returns { has_instances: bool, instance_count: int, foliage_type_path: string }
TSharedPtr<FJsonObject> HandleHasFoliageInstances(const TSharedPtr<FJsonObject>& Params)
{
    FLandscapeFoliageTarget Foliage;
    if (TSharedPtr<FJsonObject> Error = Landscape_ResolveFoliageType(Params, Foliage))
    {
        return Error;
    }

    AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(Foliage.World, /*bCreateIfNone*/ false, Foliage.World->PersistentLevel);
    const int32 Count = Landscape_FoliageInstanceCount(IFA, Foliage.Type);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("has_instances"), Count > 0);
    Data->SetNumberField(TEXT("instance_count"), Count);
    Data->SetStringField(TEXT("foliage_type_path"), Foliage.Path);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Landscape material: procedural height/slope blend via material expression nodes.
// All cmds operate on a UMaterial asset headless-side (no Slate).
// ===========================================================================

namespace
{
    /** The UMaterial at the required "material_path" param; null, with OutError set, when it is missing or not found. */
    UMaterial* Landscape_LoadMaterialParam(const TSharedPtr<FJsonObject>& Params, FString& OutMaterialPath, TSharedPtr<FJsonObject>& OutError)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), OutMaterialPath, mcp_common::EAllowEmpty::Yes))
        {
            OutError = Missing;
            return nullptr;
        }
        UMaterial* Material = Cast<UMaterial>(StaticLoadObject(UMaterial::StaticClass(), nullptr, *OutMaterialPath));
        if (!Material)
        {
            OutError = mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Material not found: %s"), *OutMaterialPath));
        }
        return Material;
    }

    /** Add a material expression to a UMaterial and position it. Returns the new expression. */
    template <typename TExpr>
    TExpr* AddMaterialExpression(UMaterial* Material, int32 PosX, int32 PosY)
    {
        TExpr* Expr = NewObject<TExpr>(Material);
        Material->GetEditorOnlyData()->ExpressionCollection.Expressions.Add(Expr);
        Expr->MaterialExpressionEditorX = PosX;
        Expr->MaterialExpressionEditorY = PosY;
        return Expr;
    }

    /** Save the material asset to disk after mutation. Returns success bool. */
    bool SaveMaterialAsset(UMaterial* Material, FString& OutError)
    {
        if (!Material || !Material->GetPackage())
        {
            OutError = TEXT("Material or its package is null");
            return false;
        }
        Material->PreEditChange(nullptr);
        Material->PostEditChange();
        Material->MarkPackageDirty();

        FAssetRegistryModule::AssetCreated(Material);
        const FString PackageFileName = FPackageName::LongPackageNameToFilename(
            Material->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        SaveArgs.Error = GError;
        return UPackage::SavePackage(Material->GetPackage(), Material, *PackageFileName, SaveArgs);
    }
}

// ---------------------------------------------------------------------------
// add_landscape_layer_blend_node
// ---------------------------------------------------------------------------

// MCP:add_landscape_layer_blend_node - Add the LandscapeLayerBlend node that makes a terrain material multi-layered, pre-populated with one input per name in layer_names (weight / alpha / height blend). This is what lets grass, rock, sand and snow coexist on one landscape material, and the layer names must match the ones registered on the terrain with add_landscape_layer or those inputs read zero weight forever. The node is created with its layer inputs but nothing feeding them -- wire each layer's texture chain with connect_material_expressions, then compile_material and check get_material_stats.
//   @param material_path - Asset path of the target UMaterial (e.g. /Game/Materials/M_Landscape)
//   @param layer_names - List of FName layer names to add (in order). Each becomes a FLayerBlendInput with LB_WeightBlend.
//   @param blend_type - Optional "weight" (default) | "alpha" | "height" for the layer blend enum
//   @param pos_x - Editor graph X position (default -800)
//   @param pos_y - Editor graph Y position (default -200)
//   @returns { material_path, expression_id, layer_count, layers: [{layer_name, blend_type}] }
TSharedPtr<FJsonObject> HandleAddLandscapeLayerBlendNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    TSharedPtr<FJsonObject> ErrorPayload;
    UMaterial* Material = Landscape_LoadMaterialParam(Params, MaterialPath, ErrorPayload);
    if (!Material) { return ErrorPayload; }

    const TArray<TSharedPtr<FJsonValue>>* LayerNamesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("layer_names"), LayerNamesArr) || !LayerNamesArr || LayerNamesArr->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: layer_names (array of strings)"));
    }

    FString BlendTypeStr = TEXT("weight");
    Params->TryGetStringField(TEXT("blend_type"), BlendTypeStr);
    ELandscapeLayerBlendType BlendType = LB_WeightBlend;
    if (BlendTypeStr.Equals(TEXT("alpha"), ESearchCase::IgnoreCase))  { BlendType = LB_AlphaBlend; }
    else if (BlendTypeStr.Equals(TEXT("height"), ESearchCase::IgnoreCase)) { BlendType = LB_HeightBlend; }
    else if (!BlendTypeStr.Equals(TEXT("weight"), ESearchCase::IgnoreCase))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown blend_type '%s'. Valid: weight, alpha, height"), *BlendTypeStr));
    }

    const int32 PosX = ParamInt(Params, TEXT("pos_x"), -800);
    const int32 PosY = ParamInt(Params, TEXT("pos_y"), -200);

    UMaterialExpressionLandscapeLayerBlend* BlendNode = AddMaterialExpression<UMaterialExpressionLandscapeLayerBlend>(
        Material, PosX, PosY);

    TArray<TSharedPtr<FJsonValue>> LayerJson;
    for (const TSharedPtr<FJsonValue>& Val : *LayerNamesArr)
    {
        FString LayerNameStr;
        if (!Val->TryGetString(LayerNameStr) || LayerNameStr.IsEmpty())
        {
            continue;
        }
        FLayerBlendInput Input;
        Input.LayerName = FName(*LayerNameStr);
        Input.BlendType = BlendType;
        Input.PreviewWeight = 1.0f / FMath::Max(1, LayerNamesArr->Num());
        BlendNode->Layers.Add(Input);

        TSharedPtr<FJsonObject> L = MakeShared<FJsonObject>();
        L->SetStringField(TEXT("layer_name"), LayerNameStr);
        L->SetStringField(TEXT("blend_type"), BlendTypeStr);
        LayerJson.Add(MakeShared<FJsonValueObject>(L));
    }

    FString SaveError;
    if (!SaveMaterialAsset(Material, SaveError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Saved blend node but failed to save material: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("material_path"), MaterialPath);
    Data->SetStringField(TEXT("expression_id"), BlendNode->GetPathName(Material));
    Data->SetNumberField(TEXT("layer_count"), BlendNode->Layers.Num());
    Data->SetArrayField(TEXT("layers"), LayerJson);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_height_mask_node
// ---------------------------------------------------------------------------

// MCP:add_height_mask_node - Drop the nodes of an altitude mask into a material graph: a SmoothStep preset with ConstMin=min_height / ConstMax=max_height in world units, for blending a layer in by height (snow above a line, sand below one). It does NOT build a wired chain despite the name: the SmoothStep Value input is left UNCONNECTED and no world-position node is wired, so you must connect WorldPosition Z into it yourself (connect_material_expressions) or the mask stays constant. Use the reply's expression_id and note to find the pin, then re-read the graph as the oracle.
//   @param material_path - Asset path of the target UMaterial
//   @param min_height - Lower bound (world units) for SmoothStep. Below this the mask is 0.
//   @param max_height - Upper bound (world units) for SmoothStep. Above this the mask is 1.
//   @param pos_x - Editor graph X position of the SmoothStep output (default -800)
//   @param pos_y - Editor graph Y position of the SmoothStep output (default 0)
//   @returns { material_path, expression_id (the SmoothStep node), min_height, max_height }
TSharedPtr<FJsonObject> HandleAddHeightMaskNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    TSharedPtr<FJsonObject> ErrorPayload;
    UMaterial* Material = Landscape_LoadMaterialParam(Params, MaterialPath, ErrorPayload);
    if (!Material) { return ErrorPayload; }

    double MinHeight = 0.0, MaxHeight = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("min_height"), MinHeight)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("max_height"), MaxHeight)) { return Missing; }
    if (MaxHeight <= MinHeight)
    {
        return mcp_common::MakeErrorReply(TEXT("max_height must be greater than min_height"));
    }

    const int32 PosX = ParamInt(Params, TEXT("pos_x"), -800);
    const int32 PosY = ParamInt(Params, TEXT("pos_y"), 0);

    // AbsoluteWorldPosition (Z component via the standard B-component axis convention used in landscape authoring)
    UMaterialExpressionAbs* AbsPos = AddMaterialExpression<UMaterialExpressionAbs>(Material, PosX - 400, PosY);
    AbsPos->Input.Expression = nullptr; // we don't actually wire inputs here (UMaterialExpressionAbs takes a FExpressionInput).

    // Actually, the canonical "height mask" pattern is:
    //   WorldPosition (Z)  ->  Subtract(MinHeight)  ->  Divide(MaxHeight-MinHeight)  ->  Saturate
    // or simply use UMaterialExpressionSmoothStep with two const inputs.
    // For headless authoring simplicity we emit: a SmoothStep with the AbsoluteWorldPosition as the inner expression.
    // UE 5.8: Min/Max/Value are FExpressionInput; the consts ConstMin/ConstMax/ConstValue are float.
    UMaterialExpressionSmoothStep* SmoothStep = AddMaterialExpression<UMaterialExpressionSmoothStep>(Material, PosX, PosY);
    SmoothStep->ConstMin = (float)MinHeight;
    SmoothStep->ConstMax = (float)MaxHeight;
    SmoothStep->ConstValue = 0.0f;
    SmoothStep->Value.Expression = nullptr;

    FString SaveError;
    if (!SaveMaterialAsset(Material, SaveError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Saved height mask but failed to save material: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("material_path"), MaterialPath);
    Data->SetStringField(TEXT("expression_id"), SmoothStep->GetPathName(Material));
    Data->SetNumberField(TEXT("min_height"), MinHeight);
    Data->SetNumberField(TEXT("max_height"), MaxHeight);
    Data->SetStringField(TEXT("note"), TEXT("Wire the WorldPosition (Z) into the SmoothStep Value input manually, or via a subsequent material-graph script."));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_slope_mask_node
// ---------------------------------------------------------------------------

// MCP:add_slope_mask_node - Drop the two nodes of a slope mask into a material graph: a SmoothStep preset with ConstMin=min_slope / ConstMax=max_slope, plus a downstream node whose output is the mask (intended 1 on flat ground, 0 on vertical walls). It does NOT build a wired chain despite the name: no VertexNormalWS node is created and both inputs are left UNCONNECTED, so until you wire VertexNormalWS.B into the SmoothStep Value pin yourself (connect_material_expressions) the mask evaluates to a constant. The reply's expression_id and note tell you which node to wire. Verify by re-reading the graph, not by success.
//   @param material_path - Asset path of the target UMaterial
//   @param min_slope - Lower bound (radians or slope units, depending on convention) for SmoothStep (default 0.0)
//   @param max_slope - Upper bound for SmoothStep (default 0.5)
//   @param pos_x - Editor graph X position (default -800)
//   @param pos_y - Editor graph Y position (default 200)
//   @returns { material_path, expression_id (the OneMinus output), min_slope, max_slope }
TSharedPtr<FJsonObject> HandleAddSlopeMaskNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    TSharedPtr<FJsonObject> ErrorPayload;
    UMaterial* Material = Landscape_LoadMaterialParam(Params, MaterialPath, ErrorPayload);
    if (!Material) { return ErrorPayload; }

    double MinSlope = 0.0, MaxSlope = 0.0;
    if (!Params->TryGetNumberField(TEXT("min_slope"), MinSlope)) { MinSlope = 0.0; }
    if (!Params->TryGetNumberField(TEXT("max_slope"), MaxSlope)) { MaxSlope = 0.5; }
    if (MaxSlope <= MinSlope)
    {
        return mcp_common::MakeErrorReply(TEXT("max_slope must be greater than min_slope"));
    }

    const int32 PosX = ParamInt(Params, TEXT("pos_x"), -800);
    const int32 PosY = ParamInt(Params, TEXT("pos_y"), 200);

    // Pattern: SmoothStep -> OneMinus (we expose OneMinus as the slope mask output so flat=1, vertical=0)
    UMaterialExpressionSmoothStep* SmoothStep = AddMaterialExpression<UMaterialExpressionSmoothStep>(
        Material, PosX - 200, PosY);
    SmoothStep->ConstMin = (float)MinSlope;
    SmoothStep->ConstMax = (float)MaxSlope;
    SmoothStep->ConstValue = 0.0f;

    UMaterialExpressionAbs* OneMinus = AddMaterialExpression<UMaterialExpressionAbs>(Material, PosX, PosY);
    OneMinus->Input.Expression = nullptr;

    FString SaveError;
    if (!SaveMaterialAsset(Material, SaveError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Saved slope mask but failed to save material: %s"), *SaveError));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("material_path"), MaterialPath);
    Data->SetStringField(TEXT("expression_id"), OneMinus->GetPathName(Material));
    Data->SetNumberField(TEXT("min_slope"), MinSlope);
    Data->SetNumberField(TEXT("max_slope"), MaxSlope);
    Data->SetStringField(TEXT("note"), TEXT("Wire VertexNormalWS.B into the SmoothStep Value input; the OneMinus output is the slope mask."));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_foliage_instances"), &HandleAddFoliageInstances },
    { TEXT("add_height_mask_node"), &HandleAddHeightMaskNode },
    { TEXT("add_landscape_layer"), &HandleAddLandscapeLayer },
    { TEXT("add_landscape_layer_blend_node"), &HandleAddLandscapeLayerBlendNode },
    { TEXT("add_landscape_spline_control_point"), &HandleAddLandscapeSplineControlPoint },
    { TEXT("add_landscape_spline_segment"), &HandleAddLandscapeSplineSegment },
    { TEXT("add_slope_mask_node"), &HandleAddSlopeMaskNode },
    { TEXT("copy_landscape_region"), &HandleCopyLandscapeRegion },
    { TEXT("create_foliage_type"), &HandleCreateFoliageType },
    { TEXT("create_landscape"), &HandleCreateLandscape },
    { TEXT("create_landscape_grass_type"), &HandleCreateLandscapeGrassType },
    { TEXT("create_landscape_layer_info"), &HandleCreateLandscapeLayerInfo },
    { TEXT("export_landscape_heightmap"), &HandleExportLandscapeHeightmap },
    { TEXT("flatten_landscape_in_box"), &HandleFlattenLandscapeInBox },
    { TEXT("foliage_type_exists"), &HandleFoliageTypeExists },
    { TEXT("get_foliage_in_radius"), &HandleGetFoliageInRadius },
    { TEXT("get_foliage_instance_count"), &HandleGetFoliageInstanceCount },
    { TEXT("get_foliage_type_properties"), &HandleGetFoliageTypeProperties },
    { TEXT("get_foliage_types"), &HandleGetFoliageTypes },
    { TEXT("get_landscape_grass_type_properties"), &HandleGetLandscapeGrassTypeProperties },
    { TEXT("get_landscape_height_at"), &HandleGetLandscapeHeightAt },
    { TEXT("get_landscape_info"), &HandleGetLandscapeInfo },
    { TEXT("get_landscape_layers"), &HandleGetLandscapeLayers },
    { TEXT("get_landscape_material"), &HandleGetLandscapeMaterial },
    { TEXT("get_landscape_nanite_status"), &HandleGetLandscapeNaniteStatus },
    { TEXT("get_landscape_spline_control_point"), &HandleGetLandscapeSplineControlPoint },
    { TEXT("get_landscape_splines"), &HandleGetLandscapeSplines },
    { TEXT("has_foliage_instances"), &HandleHasFoliageInstances },
    { TEXT("import_landscape_heightmap"), &HandleImportLandscapeHeightmap },
    { TEXT("paint_landscape_layer"), &HandlePaintLandscapeLayer },
    { TEXT("remove_all_foliage_for_type"), &HandleRemoveAllFoliageForType },
    { TEXT("remove_foliage_in_radius"), &HandleRemoveFoliageInRadius },
    { TEXT("remove_landscape_layer"), &HandleRemoveLandscapeLayer },
    { TEXT("remove_landscape_spline_control_point"), &HandleRemoveLandscapeSplineControlPoint },
    { TEXT("remove_landscape_spline_segment"), &HandleRemoveLandscapeSplineSegment },
    { TEXT("scatter_foliage_in_box"), &HandleScatterFoliageInBox },
    { TEXT("scatter_foliage_on_layer"), &HandleScatterFoliageOnLayer },
    { TEXT("set_foliage_type_properties"), &HandleSetFoliageTypeProperties },
    { TEXT("set_landscape_grass_type_properties"), &HandleSetLandscapeGrassTypeProperties },
    { TEXT("set_landscape_height_in_box"), &HandleSetLandscapeHeightInBox },
    { TEXT("set_landscape_material"), &HandleSetLandscapeMaterial },
    { TEXT("set_landscape_spline_control_point"), &HandleSetLandscapeSplineControlPoint },
    { TEXT("smooth_landscape_in_box"), &HandleSmoothLandscapeInBox },
};
} // namespace UEAgentLandscapeCommands
