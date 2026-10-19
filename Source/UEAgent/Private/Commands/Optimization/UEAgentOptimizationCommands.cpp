#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/StreamableRenderAsset.h"
#include "ScopedTransaction.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "ContentStreaming.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Blueprint/BlueprintSupport.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"
#include "Engine/Selection.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"

namespace UEAgentOptimizationCommands
{

// MCP-DOMAIN: Optimization

// ---------------------------------------------------------------------------
// Building blocks shared by the analysis commands (analyze / memory / check /
// suggest) and the three setters (compression / LOD count / streaming).
// ---------------------------------------------------------------------------
namespace UEAgentOptimizationImpl
{
    constexpr int64 OneMiB = 1024 * 1024;

    // -- Labels -----------------------------------------------------------------

    /**
     * Name of a texture compression preset. Only the presets listed below are named; any other
     * value, including presets added to the engine later, reports as "Unknown".
     */
    FString CompressionSettingsName(const TEnumAsByte<TextureCompressionSettings> Setting)
    {
        struct FNamedPreset
        {
            TextureCompressionSettings Preset;
            const TCHAR* Label;
        };
        static const FNamedPreset NamedPresets[] =
        {
            { TC_Default,               TEXT("TC_Default") },
            { TC_Normalmap,             TEXT("TC_Normalmap") },
            { TC_Masks,                 TEXT("TC_Masks") },
            { TC_Grayscale,             TEXT("TC_Grayscale") },
            { TC_Displacementmap,       TEXT("TC_Displacementmap") },
            { TC_VectorDisplacementmap, TEXT("TC_VectorDisplacementmap") },
            { TC_HDR,                   TEXT("TC_HDR") },
            { TC_EditorIcon,            TEXT("TC_EditorIcon") },
            { TC_Alpha,                 TEXT("TC_Alpha") },
            { TC_DistanceFieldFont,     TEXT("TC_DistanceFieldFont") },
            { TC_HDR_Compressed,        TEXT("TC_HDR_Compressed") },
            { TC_BC7,                   TEXT("TC_BC7") },
            { TC_HalfFloat,             TEXT("TC_HalfFloat") },
        };
        for (const FNamedPreset& Named : NamedPresets)
        {
            if (Named.Preset == Setting.GetValue())
            {
                return Named.Label;
            }
        }
        return TEXT("Unknown");
    }

    /** "PixelFormat_<n>" read from the texture's platform data; "Unknown" without texture or data. */
    FString PixelFormatLabel(UTexture2D* Texture)
    {
        const FTexturePlatformData* Platform = (Texture != nullptr) ? Texture->GetPlatformData() : nullptr;
        if (Platform == nullptr)
        {
            return TEXT("Unknown");
        }
        return FString::Printf(TEXT("PixelFormat_%d"), static_cast<int32>(Platform->PixelFormat));
    }

    /** Name of a compression quality level, for reporting the value read back after a change. */
    FString CompressionQualityName(const TEnumAsByte<ETextureCompressionQuality> Quality)
    {
        switch (Quality.GetValue())
        {
        case TCQ_Default: return TEXT("TCQ_Default");
        case TCQ_Lowest:  return TEXT("TCQ_Lowest");
        case TCQ_Low:     return TEXT("TCQ_Low");
        case TCQ_Medium:  return TEXT("TCQ_Medium");
        case TCQ_High:    return TEXT("TCQ_High");
        case TCQ_Highest: return TEXT("TCQ_Highest");
        default:          break;
        }
        return TEXT("Unknown");
    }

    /** set_asset_compression accepts exactly High / Medium / Low, in any letter case. */
    bool QualityFromToken(const FString& Token, TEnumAsByte<ETextureCompressionQuality>& OutQuality)
    {
        if (Token.Equals(TEXT("High"), ESearchCase::IgnoreCase))
        {
            OutQuality = TCQ_High;
            return true;
        }
        if (Token.Equals(TEXT("Medium"), ESearchCase::IgnoreCase))
        {
            OutQuality = TCQ_Medium;
            return true;
        }
        if (Token.Equals(TEXT("Low"), ESearchCase::IgnoreCase))
        {
            OutQuality = TCQ_Low;
            return true;
        }
        return false;
    }

    // -- Registry, disk and JSON helpers -------------------------------------------

    /**
     * Registry lookup followed by a load. On failure returns the error answer to send back
     * ("Asset not found" / "Failed to load asset"); on success returns null and fills the outputs.
     */
    TSharedPtr<FJsonObject> LoadAssetForCommand(const FString& ObjectPath, FAssetData& OutEntry, UObject*& OutAsset)
    {
        OutAsset = nullptr;
        OutEntry = mcp_common::FindAssetDataByPath(ObjectPath);
        if (!OutEntry.IsValid())
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found: %s"), *ObjectPath));
        }
        OutAsset = OutEntry.GetAsset();
        if (OutAsset == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *ObjectPath));
        }
        return nullptr;
    }

    /** Size of the package file behind an asset (.uasset or .umap), or 0 when it is not on disk. */
    int64 PackageBytesOnDisk(const FAssetData& Entry)
    {
        FString PackageFile;
        if (!mcp_common::FindPackageFileOnDisk(Entry.PackageName.ToString(), PackageFile))
        {
            return 0;
        }
        return FMath::Max<int64>(0, FPlatformFileManager::Get().GetPlatformFile().FileSize(*PackageFile));
    }

    double InMegabytes(const int64 Bytes)
    {
        return Bytes / (1024.0 * 1024.0);
    }

    void AppendLines(TArray<FString>& Into, std::initializer_list<const TCHAR*> Lines)
    {
        for (const TCHAR* Line : Lines)
        {
            Into.Add(Line);
        }
    }

    /** asset_path / asset_class / file_size_bytes / file_size_mb, the head of every analysis answer. */
    void PutAssetIdentity(const TSharedPtr<FJsonObject>& Out, const FString& AssetPath, const FAssetData& Entry, const int64 DiskBytes)
    {
        Out->SetStringField(TEXT("asset_path"), AssetPath);
        Out->SetStringField(TEXT("asset_class"), Entry.AssetClassPath.ToString());
        Out->SetNumberField(TEXT("file_size_bytes"), DiskBytes);
        Out->SetNumberField(TEXT("file_size_mb"), InMegabytes(DiskBytes));
    }

    /**
     * The asset_path asset looked up and loaded, its package size, and Report's identity fields
     * (PutAssetIdentity). Null on success, else the error reply.
     */
    TSharedPtr<FJsonObject> StartAssetReport(const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonObject>& Report, UObject*& OutAsset, int64& OutDiskBytes)
    {
        FString RequestedAsset;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }

        FAssetData Entry;
        if (TSharedPtr<FJsonObject> LookupFailure = LoadAssetForCommand(RequestedAsset, Entry, OutAsset))
        {
            return LookupFailure;
        }

        OutDiskBytes = PackageBytesOnDisk(Entry);
        PutAssetIdentity(Report, RequestedAsset, Entry, OutDiskBytes);
        return nullptr;
    }

    // -- Per-class statistics ---------------------------------------------------------

    struct FMeshLodTotals
    {
        int32 LodCount = 0;
        int32 Triangles = 0;
        int32 Vertices = 0;
        int32 Sections = 0;
        /** Rough geometry footprint over every LOD: three vectors per triangle plus one per vertex. */
        int64 GeometryBytes = 0;
    };

    /** Sums the render LODs of a static mesh (GetNumLODs is 0 when there is no render data). */
    FMeshLodTotals SumStaticMeshLods(UStaticMesh& Mesh)
    {
        FMeshLodTotals Totals;
        Totals.LodCount = Mesh.GetNumLODs();
        for (int32 Lod = 0; Lod < Totals.LodCount; ++Lod)
        {
            const FStaticMeshLODResources& Resources = Mesh.GetRenderData()->LODResources[Lod];
            const int32 LodTriangles = Resources.GetNumTriangles();
            const int32 LodVertices = Resources.GetNumVertices();
            Totals.Triangles += LodTriangles;
            Totals.Vertices += LodVertices;
            Totals.Sections += Resources.Sections.Num();
            const int64 LodBytes = (LodTriangles * 3 * sizeof(FVector)) + (LodVertices * sizeof(FVector));
            Totals.GeometryBytes += LodBytes;
        }
        return Totals;
    }

    struct FTextureFacts
    {
        int32 Width = 0;
        int32 Height = 0;
        int32 Mips = 0;
        FString Compression;
        FString PixelFormat;
    };

    FTextureFacts ReadTextureFacts(UTexture2D& Texture)
    {
        FTextureFacts Facts;
        Facts.Width = Texture.GetSizeX();
        Facts.Height = Texture.GetSizeY();
        Facts.Mips = Texture.GetNumMips();
        Facts.Compression = CompressionSettingsName(Texture.CompressionSettings);
        Facts.PixelFormat = PixelFormatLabel(&Texture);
        return Facts;
    }

    void PutTextureFacts(const TSharedPtr<FJsonObject>& Out, const FTextureFacts& Facts)
    {
        Out->SetNumberField(TEXT("width"), Facts.Width);
        Out->SetNumberField(TEXT("height"), Facts.Height);
        Out->SetNumberField(TEXT("mip_count"), Facts.Mips);
        Out->SetStringField(TEXT("compression_settings"), Facts.Compression);
        Out->SetStringField(TEXT("pixel_format"), Facts.PixelFormat);
    }

    /** Uncompressed size of the whole mip chain at 4 bytes per texel (each product in 64 bits: an 8K
     *  mip is 268,435,456 bytes, and wider textures overflow a 32-bit product). */
    int64 EstimateMipChainBytes(const FTextureFacts& Facts)
    {
        int64 Sum = 0;
        for (int32 Mip = 0; Mip < Facts.Mips; ++Mip)
        {
            const int32 MipWidth = FMath::Max(1, Facts.Width >> Mip);
            const int32 MipHeight = FMath::Max(1, Facts.Height >> Mip);
            const int64 MipBytes = static_cast<int64>(MipWidth) * MipHeight * 4;
            Sum += MipBytes;
        }
        return Sum;
    }

    struct FMaterialParamTally
    {
        int32 Total = 0;
        /** Texture parameters; the tools also use this as the texture sample count. */
        int32 Textures = 0;
    };

    using FParameterInfoQuery = void (UMaterialInterface::*)(TArray<FMaterialParameterInfo>&, TArray<FGuid>&) const;

    int32 CountParameters(const UMaterialInterface& Material, const FParameterInfoQuery Query)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Ids;
        (Material.*Query)(Infos, Ids);
        return Infos.Num();
    }

    FMaterialParamTally TallyMaterialParameters(const UMaterialInterface& Material)
    {
        const int32 Scalars = CountParameters(Material, &UMaterialInterface::GetAllScalarParameterInfo);
        const int32 Vectors = CountParameters(Material, &UMaterialInterface::GetAllVectorParameterInfo);
        const int32 Textures = CountParameters(Material, &UMaterialInterface::GetAllTextureParameterInfo);

        FMaterialParamTally Tally;
        Tally.Textures = Textures;
        Tally.Total = Scalars + Vectors + Textures;
        return Tally;
    }

    // -- analyze_asset_performance --------------------------------------------------

    /** Good / Fair / Poor plus the issues behind it. */
    struct FPerformanceGrade
    {
        FString Rating = TEXT("Good");
        TArray<FString> Issues;

        /** The first tier that applies sets the rating and records its issue. */
        void Tier(const bool bPoor, const TCHAR* PoorIssue, const bool bFair, const TCHAR* FairIssue)
        {
            if (bPoor)
            {
                Rating = TEXT("Poor");
                Issues.Add(PoorIssue);
            }
            else if (bFair)
            {
                Rating = TEXT("Fair");
                Issues.Add(FairIssue);
            }
        }

        /** Records an issue without touching the rating. */
        void NoteIf(const bool bApplies, const TCHAR* Issue)
        {
            if (bApplies)
            {
                Issues.Add(Issue);
            }
        }
    };

    /**
     * Per-asset body shared by analyze_asset_performance and analyze_assets_performance_batch.
     * The caller has already checked that AssetData is valid; loading, the class-specific
     * statistics and the grade happen here. Not a command of its own, so it carries no tool
     * annotation.
     */
    TSharedPtr<FJsonObject> AnalyzeAssetPerformanceCore(const FString& AssetPath, const FAssetData& AssetData)
    {
        UObject* Loaded = AssetData.GetAsset();
        if (Loaded == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
        }

        const int64 DiskBytes = PackageBytesOnDisk(AssetData);
        TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
        PutAssetIdentity(Report, AssetPath, AssetData, DiskBytes);

        FPerformanceGrade Grade;
        TArray<FString> Advice;
        if (UStaticMesh* Mesh = Cast<UStaticMesh>(Loaded))
        {
            const FMeshLodTotals Totals = SumStaticMeshLods(*Mesh);
            Report->SetStringField(TEXT("optimization_type"), TEXT("StaticMesh"));
            Report->SetNumberField(TEXT("lod_count"), Totals.LodCount);
            Report->SetNumberField(TEXT("triangle_count"), Totals.Triangles);
            Report->SetNumberField(TEXT("vertex_count"), Totals.Vertices);
            Report->SetNumberField(TEXT("section_count"), Totals.Sections);

            Grade.Tier(Totals.Triangles > 100000, TEXT("High triangle count (>100k)"),
                Totals.Triangles > 50000, TEXT("Moderate triangle count (>50k)"));
            Grade.NoteIf(Totals.LodCount < 2, TEXT("No LOD settings configured"));
            Grade.NoteIf(DiskBytes > 50 * OneMiB, TEXT("Large file size (>50MB)"));
            AppendLines(Advice, {
                TEXT("Enable LOD settings for better performance"),
                TEXT("Consider mesh compression"),
                TEXT("Check polygon count and complexity"),
                TEXT("Use appropriate collision complexity") });
        }
        else if (UTexture2D* Texture = Cast<UTexture2D>(Loaded))
        {
            const FTextureFacts Facts = ReadTextureFacts(*Texture);
            Report->SetStringField(TEXT("optimization_type"), TEXT("Texture2D"));
            PutTextureFacts(Report, Facts);

            Grade.Tier(Facts.Width > 4096 || Facts.Height > 4096, TEXT("Very high resolution texture (>4K)"),
                Facts.Width > 2048 || Facts.Height > 2048, TEXT("High resolution texture (>2K)"));
            Grade.NoteIf(Facts.Mips < 2, TEXT("No mip maps configured"));
            Grade.NoteIf(DiskBytes > 50 * OneMiB, TEXT("Large file size (>50MB)"));
            AppendLines(Advice, {
                TEXT("Use appropriate texture compression"),
                TEXT("Enable mip maps for better performance"),
                TEXT("Consider texture resolution"),
                TEXT("Use texture streaming if needed") });
        }
        else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Loaded))
        {
            const FMaterialParamTally ParamTally = TallyMaterialParameters(*Material);
            Report->SetStringField(TEXT("optimization_type"), TEXT("Material"));
            Report->SetNumberField(TEXT("parameter_count"), ParamTally.Total);
            Report->SetNumberField(TEXT("texture_sample_count"), ParamTally.Textures);
            // Shader instruction counts are not gathered; the field is always 0.
            Report->SetNumberField(TEXT("instruction_count"), 0);

            Grade.Tier(ParamTally.Total > 50, TEXT("High parameter count (>50)"),
                ParamTally.Total > 25, TEXT("Moderate parameter count (>25)"));
            Grade.NoteIf(ParamTally.Textures > 10, TEXT("High texture sample count (>10)"));
            AppendLines(Advice, {
                TEXT("Simplify material complexity"),
                TEXT("Use material instances when possible"),
                TEXT("Check shader complexity"),
                TEXT("Consider material parameter collections") });
        }
        else
        {
            // Unclassified assets: the same shape as every other class (arrays, as the annotation
            // promises), with the one note there is.
            Report->SetStringField(TEXT("optimization_type"), TEXT("Other"));
            Report->SetStringField(TEXT("performance_rating"), TEXT("Unknown"));
            Report->SetArrayField(TEXT("performance_issues"), TArray<TSharedPtr<FJsonValue>>());
            Report->SetArrayField(TEXT("suggestions"), mcp_common::StringsToJson({ TEXT("Asset type not analyzed") }));
            return Report;
        }

        Report->SetStringField(TEXT("performance_rating"), Grade.Rating);
        Report->SetArrayField(TEXT("performance_issues"), mcp_common::StringsToJson(Grade.Issues));
        Report->SetArrayField(TEXT("suggestions"), mcp_common::StringsToJson(Advice));
        return Report;
    }

    // -- get_asset_memory_usage ---------------------------------------------------------

    /** "Very High" / "High" / "Medium" / "Low" from three descending byte thresholds (strictly above). */
    const TCHAR* FootprintBand(const int64 Bytes, const int64 VeryHighAbove, const int64 HighAbove, const int64 MediumAbove)
    {
        if (Bytes > VeryHighAbove)
        {
            return TEXT("Very High");
        }
        if (Bytes > HighAbove)
        {
            return TEXT("High");
        }
        return Bytes > MediumAbove ? TEXT("Medium") : TEXT("Low");
    }

    // -- check_asset_optimization --------------------------------------------------------

    struct FFinding
    {
        const TCHAR* Issue;
        const TCHAR* Advice;
    };

    /**
     * Outcome of check_asset_optimization. Every finding overwrites the status, so the LAST
     * finding decides it (a later Fair can follow an earlier Poor), while bOptimized only ever
     * goes from true to false.
     */
    struct FOptimizationVerdict
    {
        bool bOptimized = true;
        FString Status = TEXT("Good");
        TArray<FString> Issues;
        TArray<FString> Recommendations;

        void Record(const FFinding& Finding)
        {
            Issues.Add(Finding.Issue);
            Recommendations.Add(Finding.Advice);
        }

        /**
         * Severe level: unoptimized and Poor. Mild level: Fair, unless an earlier tier already said
         * Poor -- the status is the worst of all tiers, so a later mild tier never turns Poor into
         * Fair. Neither: nothing recorded.
         */
        void Tier(const bool bSevere, const FFinding& Severe, const bool bMild, const FFinding& Mild)
        {
            if (bSevere)
            {
                bOptimized = false;
                Status = TEXT("Poor");
                Record(Severe);
            }
            else if (bMild)
            {
                if (Status != TEXT("Poor"))
                {
                    Status = TEXT("Fair");
                }
                Record(Mild);
            }
        }

        /** The package-size tier, worded the same for every class except for its advice. */
        void PackageSizeTier(const int64 DiskBytes, const TCHAR* SevereAdvice, const TCHAR* MildAdvice)
        {
            Tier(DiskBytes > 50 * OneMiB, { TEXT("File size too large (>50MB)"), SevereAdvice },
                DiskBytes > 10 * OneMiB, { TEXT("File size could be optimized (<10MB recommended)"), MildAdvice });
        }
    };

    // -- set_asset_lod_settings ------------------------------------------------------------

    /** Grows or shrinks the source model list to Wanted entries, never going below one. */
    void MatchSourceModelCount(UStaticMesh& Mesh, const int32 Wanted)
    {
        while (Mesh.GetNumSourceModels() < Wanted)
        {
            Mesh.AddSourceModel();
        }
        for (int32 Count = Mesh.GetNumSourceModels(); Count > Wanted && Count > 1; Count = Mesh.GetNumSourceModels())
        {
            Mesh.RemoveSourceModel(Count - 1);
        }
    }

    /** One row per LOD of the mesh's current render data, plus triangle/vertex sums over them. */
    TArray<TSharedPtr<FJsonValue>> RenderLodRows(UStaticMesh& Mesh, int32& OutTriangles, int32& OutVertices)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        OutTriangles = 0;
        OutVertices = 0;

        FStaticMeshRenderData* RenderData = Mesh.GetRenderData();
        const int32 LodTotal = (RenderData != nullptr) ? RenderData->LODResources.Num() : 0;
        for (int32 Lod = 0; Lod < LodTotal; ++Lod)
        {
            const FStaticMeshLODResources& Resources = RenderData->LODResources[Lod];
            const int32 Triangles = Resources.GetNumTriangles();
            const int32 Vertices = Resources.GetNumVertices();
            OutTriangles += Triangles;
            OutVertices += Vertices;

            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("lod_index"), Lod);
            Row->SetNumberField(TEXT("triangle_count"), Triangles);
            Row->SetNumberField(TEXT("vertex_count"), Vertices);
            Row->SetNumberField(TEXT("screen_size"), RenderData->ScreenSize[Lod].Default);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return Rows;
    }

    /** Read-only LOD rows of a skeletal mesh; LODs without info are skipped. */
    TArray<TSharedPtr<FJsonValue>> SkeletalLodRows(USkeletalMesh& Mesh, const int32 LodTotal)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        for (int32 Lod = 0; Lod < LodTotal; ++Lod)
        {
            const FSkeletalMeshLODInfo* Info = Mesh.GetLODInfo(Lod);
            if (Info == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetNumberField(TEXT("lod_index"), Lod);
            Row->SetNumberField(TEXT("screen_size"), Info->ScreenSize.Default);
            Row->SetNumberField(TEXT("lod_hysteresis"), Info->LODHysteresis);
            Row->SetBoolField(TEXT("is_reduction_active"), Info->bHasBeenSimplified);
            Rows.Add(MakeShared<FJsonValueObject>(Row));
        }
        return Rows;
    }
}

// Cost analysis

// MCP:analyze_asset_performance - Grade one asset Good, Fair or Poor against fixed thresholds, without changing it: the path is looked up in the asset registry and the asset loaded synchronously. Static meshes are graded on triangles summed over all render LODs (above 50k Fair, above 100k Poor), Texture2D on resolution (above 2K, above 4K), materials and instances on parameter count (above 25, above 50); fewer than 2 LODs or mips, a mesh or texture package over 50 MB and over 10 texture parameters only add performance_issues. Other classes get performance_rating Unknown. suggestions is fixed advice per class, not derived from the numbers; texture_sample_count counts texture parameters and instruction_count is always 0. A path missing from the registry or failing to load is an error. Many assets: analyze_assets_performance_batch; a verdict: check_asset_optimization; shader stats: get_material_stats; memory: get_asset_memory_usage.
//   @param asset_path - Object path ("/Game/Meshes/SM_Rock.SM_Rock") or package path ("/Game/Meshes/SM_Rock") of an asset the asset registry knows
//   @returns { asset_path: string, asset_class: string, file_size_bytes: number, file_size_mb: number, optimization_type: string, lod_count?: number, triangle_count?: number, vertex_count?: number, section_count?: number, performance_rating: string, performance_issues: [string], suggestions: [string], width?: number, height?: number, mip_count?: number, compression_settings?: string, pixel_format?: string, parameter_count?: number, texture_sample_count?: number, instruction_count?: number }

TSharedPtr<FJsonObject> HandleAnalyzeAssetPerformance(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    FString RequestedAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Only the registry lookup happens here. Loading and grading live in the shared core, so the
    // batch command reports exactly the same per-asset payload.
    const FAssetData Entry = mcp_common::FindAssetDataByPath(RequestedAsset);
    if (!Entry.IsValid())
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset not found: %s"), *RequestedAsset));
    }
    return Impl::AnalyzeAssetPerformanceCore(RequestedAsset, Entry);
}

// MCP:analyze_assets_performance_batch - Batch version of analyze_asset_performance. Resolves and analyzes multiple assets by path in one call (avoids callers looping analyze_asset_performance one-by-one). Forces a synchronous load per asset, so max_results defaults conservatively.
//   @param asset_paths - Required array of asset paths to analyze.
//   @param max_results - Optional cap on how many of asset_paths to process (default 200, clamped 1..2000). Entries beyond the cap are not processed.
//   @returns { requested_count: number, processed_count: number, results_truncated: bool, results: [{ asset_path: string, found: bool, asset_class?: string, file_size_bytes?: number, file_size_mb?: number, optimization_type?: string, performance_rating?: string, performance_issues?: [string], suggestions?: [string], ...same per-asset fields as analyze_asset_performance's data payload, error?: string }] }

TSharedPtr<FJsonObject> HandleAnalyzeAssetsPerformanceBatch(const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("asset_paths"), AssetPathsArray) || !AssetPathsArray || AssetPathsArray->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("'asset_paths' array is required and must not be empty"));
    }

    int32 MaxResults = 200;
    if (Params->TryGetNumberField(TEXT("max_results"), MaxResults))
    {
        MaxResults = FMath::Clamp(MaxResults, 1, 2000);
    }

    const int32 RequestedCount = AssetPathsArray->Num();
    const int32 ProcessedCount = FMath::Min(RequestedCount, MaxResults);
    const bool bTruncated = RequestedCount > ProcessedCount;

    TArray<TSharedPtr<FJsonValue>> ResultsArray;
    ResultsArray.Reserve(ProcessedCount);

    for (int32 Index = 0; Index < ProcessedCount; ++Index)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();

        FString AssetPath;
        if (!(*AssetPathsArray)[Index].IsValid() || !(*AssetPathsArray)[Index]->TryGetString(AssetPath))
        {
            Entry->SetBoolField(TEXT("found"), false);
            Entry->SetStringField(TEXT("error"), TEXT("asset_paths entry is not a string"));
            ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        Entry->SetStringField(TEXT("asset_path"), AssetPath);

        FAssetData AssetData = mcp_common::FindAssetDataByPath(AssetPath);
        if (!AssetData.IsValid())
        {
            Entry->SetBoolField(TEXT("found"), false);
            ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
            continue;
        }

        Entry->SetBoolField(TEXT("found"), true);

        // Merge every field the shared per-asset core produced (same payload analyze_asset_performance
        // returns for a single asset -- including its own "error" field on a load failure).
        TSharedPtr<FJsonObject> PerAssetResult = UEAgentOptimizationImpl::AnalyzeAssetPerformanceCore(AssetPath, AssetData);
        if (PerAssetResult.IsValid())
        {
            for (const auto& Pair : PerAssetResult->Values)
            {
                Entry->SetField(Pair.Key, Pair.Value);
            }
        }

        ResultsArray.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("requested_count"), RequestedCount);
    ResultObj->SetNumberField(TEXT("processed_count"), ProcessedCount);
    ResultObj->SetBoolField(TEXT("results_truncated"), bTruncated);
    ResultObj->SetArrayField(TEXT("results"), ResultsArray);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:get_asset_memory_usage - Estimate the memory one asset needs from simple formulas, not a measurement, without changing it; the asset is found in the asset registry and loaded. Static meshes count 24-byte vectors, three per triangle plus one per vertex, over all render LODs; Texture2D 4 bytes per texel over the whole mip chain, whatever the compression; materials and instances 1 KB per parameter; any other class reports its package size. memory_usage bands the estimate per class (Low, Medium, High, Very High; Unknown for other classes) and optimization_tips is one fixed line per class. file_size_bytes is the package file on disk, 0 when there is none. A path missing from the registry or failing to load is an error. The disk size alone is get_asset_file_info, a graded report analyze_asset_performance, the whole editor's memory get_memory_usage.
//   @param asset_path - Object path ("/Game/Meshes/SM_Rock.SM_Rock") or package path ("/Game/Meshes/SM_Rock") of an asset the asset registry knows
//   @returns { asset_path: string, asset_class: string, file_size_bytes: number, file_size_mb: number, memory_type: string, lod_count?: number, triangle_count?: number, vertex_count?: number, section_count?: number, estimated_memory_usage_bytes: number, estimated_memory_usage_mb: number, memory_usage: string, optimization_tips: string, width?: number, height?: number, mip_count?: number, compression_settings?: string, pixel_format?: string, parameter_count?: number, texture_sample_count?: number }

TSharedPtr<FJsonObject> HandleGetAssetMemoryUsage(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    TSharedPtr<FJsonObject> Footprint = MakeShared<FJsonObject>();
    UObject* Target = nullptr;
    int64 DiskBytes = 0;
    if (TSharedPtr<FJsonObject> Error = Impl::StartAssetReport(Params, Footprint, Target, DiskBytes)) { return Error; }

    // Each class adds its own statistics and picks an estimate, a band and a tip; the four
    // shared fields are written once at the end.
    int64 EstimatedBytes = 0;
    const TCHAR* Band = TEXT("Low");
    const TCHAR* Tip = TEXT("");
    if (UStaticMesh* Mesh = Cast<UStaticMesh>(Target))
    {
        const Impl::FMeshLodTotals Totals = Impl::SumStaticMeshLods(*Mesh);
        Footprint->SetStringField(TEXT("memory_type"), TEXT("StaticMesh"));
        Footprint->SetNumberField(TEXT("lod_count"), Totals.LodCount);
        Footprint->SetNumberField(TEXT("triangle_count"), Totals.Triangles);
        Footprint->SetNumberField(TEXT("vertex_count"), Totals.Vertices);
        Footprint->SetNumberField(TEXT("section_count"), Totals.Sections);
        EstimatedBytes = Totals.GeometryBytes;
        Band = Impl::FootprintBand(EstimatedBytes, 100 * Impl::OneMiB, 50 * Impl::OneMiB, 10 * Impl::OneMiB);
        Tip = TEXT("Consider LOD settings and mesh compression");
    }
    else if (UTexture2D* Texture = Cast<UTexture2D>(Target))
    {
        const Impl::FTextureFacts Facts = Impl::ReadTextureFacts(*Texture);
        Footprint->SetStringField(TEXT("memory_type"), TEXT("Texture2D"));
        Impl::PutTextureFacts(Footprint, Facts);
        EstimatedBytes = Impl::EstimateMipChainBytes(Facts);
        Band = Impl::FootprintBand(EstimatedBytes, 200 * Impl::OneMiB, 100 * Impl::OneMiB, 50 * Impl::OneMiB);
        Tip = TEXT("Consider texture compression and resolution");
    }
    else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Target))
    {
        const Impl::FMaterialParamTally ParamTally = Impl::TallyMaterialParameters(*Material);
        Footprint->SetStringField(TEXT("memory_type"), TEXT("Material"));
        Footprint->SetNumberField(TEXT("parameter_count"), ParamTally.Total);
        Footprint->SetNumberField(TEXT("texture_sample_count"), ParamTally.Textures);
        // A flat kilobyte per parameter; only very parameter-heavy materials leave the Low band.
        EstimatedBytes = ParamTally.Total * 1024;
        Band = (ParamTally.Total > 100) ? TEXT("Medium") : TEXT("Low");
        Tip = TEXT("Consider material complexity");
    }
    else
    {
        // Unanalyzed classes report their package size as the estimate.
        Footprint->SetStringField(TEXT("memory_type"), TEXT("Other"));
        EstimatedBytes = DiskBytes;
        Band = TEXT("Unknown");
        Tip = TEXT("Asset type not analyzed");
    }

    Footprint->SetNumberField(TEXT("estimated_memory_usage_bytes"), EstimatedBytes);
    Footprint->SetNumberField(TEXT("estimated_memory_usage_mb"), Impl::InMegabytes(EstimatedBytes));
    Footprint->SetStringField(TEXT("memory_usage"), Band);
    Footprint->SetStringField(TEXT("optimization_tips"), Tip);
    return Footprint;
}

// MCP:check_asset_optimization - Check one asset against fixed optimization thresholds and return a verdict, without changing it; the asset is found in the asset registry and loaded. Static meshes: LOD count (under 2 severe, under 3 mild), triangles over all render LODs (above 100k, above 50k) and package size (above 50 MB, above 10 MB). Texture2D: resolution (above 4K, above 2K), mips (under 2, under 4), package size and an advisory note for the TC_Default preset. Materials and instances: parameters (above 50, above 25) and texture parameters (above 10, above 5). Other classes: package size only. is_optimized turns false on any severe finding, optimization_status is the worst tier (Good, Fair, Poor), and issues pair one to one with recommendations. A path missing from the registry or failing to load is an error. Graded stats: analyze_asset_performance; more advice: suggest_asset_optimizations; fixes: set_asset_lod_settings, set_asset_compression.
//   @param asset_path - Object path ("/Game/Meshes/SM_Rock.SM_Rock") or package path ("/Game/Meshes/SM_Rock") of an asset the asset registry knows
//   @returns { asset_path: string, asset_class: string, file_size_bytes: number, file_size_mb: number, is_optimized: bool, optimization_status: string, issues: [string], recommendations: [string], lod_count?: number, triangle_count?: number, vertex_count?: number, width?: number, height?: number, mip_count?: number, compression_settings?: string, pixel_format?: string, parameter_count?: number, texture_sample_count?: number }

TSharedPtr<FJsonObject> HandleCheckAssetOptimization(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    TSharedPtr<FJsonObject> Findings = MakeShared<FJsonObject>();
    UObject* Target = nullptr;
    int64 DiskBytes = 0;
    if (TSharedPtr<FJsonObject> Error = Impl::StartAssetReport(Params, Findings, Target, DiskBytes)) { return Error; }

    // Every applicable tier is recorded; the status is the worst of them.
    Impl::FOptimizationVerdict Verdict;
    if (UStaticMesh* Mesh = Cast<UStaticMesh>(Target))
    {
        const Impl::FMeshLodTotals Totals = Impl::SumStaticMeshLods(*Mesh);
        Findings->SetNumberField(TEXT("lod_count"), Totals.LodCount);
        Findings->SetNumberField(TEXT("triangle_count"), Totals.Triangles);
        Findings->SetNumberField(TEXT("vertex_count"), Totals.Vertices);

        Verdict.Tier(Totals.LodCount < 2, { TEXT("No LOD settings configured"), TEXT("Enable LOD settings for better performance") },
            Totals.LodCount < 3, { TEXT("Limited LOD settings"), TEXT("Consider adding more LOD levels") });
        Verdict.Tier(Totals.Triangles > 100000, { TEXT("High triangle count (>100k)"), TEXT("Reduce triangle count or use LOD settings") },
            Totals.Triangles > 50000, { TEXT("Moderate triangle count (>50k)"), TEXT("Consider reducing triangle count") });
        Verdict.PackageSizeTier(DiskBytes, TEXT("Consider mesh compression or splitting"), TEXT("Consider mesh compression"));
    }
    else if (UTexture2D* Texture = Cast<UTexture2D>(Target))
    {
        const Impl::FTextureFacts Facts = Impl::ReadTextureFacts(*Texture);
        Impl::PutTextureFacts(Findings, Facts);

        Verdict.Tier(Facts.Width > 4096 || Facts.Height > 4096, { TEXT("Very high resolution texture (>4K)"), TEXT("Consider reducing texture resolution") },
            Facts.Width > 2048 || Facts.Height > 2048, { TEXT("High resolution texture (>2K)"), TEXT("Consider reducing texture resolution") });
        Verdict.Tier(Facts.Mips < 2, { TEXT("No mip maps configured"), TEXT("Enable mip maps for better performance") },
            Facts.Mips < 4, { TEXT("Limited mip maps"), TEXT("Consider adding more mip levels") });
        // Advisory only: neither the status nor is_optimized changes for the default preset.
        if (Facts.Compression == TEXT("TC_Default"))
        {
            Verdict.Record({ TEXT("Default compression settings"), TEXT("Consider using appropriate compression settings") });
        }
        Verdict.PackageSizeTier(DiskBytes, TEXT("Consider texture compression or resolution reduction"), TEXT("Consider texture compression"));
    }
    else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Target))
    {
        const Impl::FMaterialParamTally ParamTally = Impl::TallyMaterialParameters(*Material);
        Findings->SetNumberField(TEXT("parameter_count"), ParamTally.Total);
        Findings->SetNumberField(TEXT("texture_sample_count"), ParamTally.Textures);

        Verdict.Tier(ParamTally.Total > 50, { TEXT("High parameter count (>50)"), TEXT("Simplify material complexity") },
            ParamTally.Total > 25, { TEXT("Moderate parameter count (>25)"), TEXT("Consider simplifying material") });
        Verdict.Tier(ParamTally.Textures > 10, { TEXT("High texture sample count (>10)"), TEXT("Reduce texture samples") },
            ParamTally.Textures > 5, { TEXT("Moderate texture sample count (>5)"), TEXT("Consider reducing texture samples") });
    }
    else
    {
        Verdict.PackageSizeTier(DiskBytes, TEXT("Consider compression or splitting"), TEXT("Consider compression"));
    }

    Findings->SetBoolField(TEXT("is_optimized"), Verdict.bOptimized);
    Findings->SetStringField(TEXT("optimization_status"), Verdict.Status);
    Findings->SetArrayField(TEXT("issues"), mcp_common::StringsToJson(Verdict.Issues));
    Findings->SetArrayField(TEXT("recommendations"), mcp_common::StringsToJson(Verdict.Recommendations));
    return Findings;
}

// MCP:suggest_asset_optimizations - List optimization advice for one asset, without changing it; the asset is found in the asset registry and loaded. critical_suggestions come from thresholds: a static mesh with fewer than 2 render LODs or over 100k triangles across them, a Texture2D above 4K or with fewer than 2 mips, a material or instance with over 50 parameters or over 10 texture parameters, and any package over 50 MB. performance_suggestions and memory_suggestions are fixed advice per class plus package-size notes above 10 MB, and suggestions joins all of them with eight generic lines every asset gets; other classes get a not-analyzed note instead of class advice. A path missing from the registry or failing to load is an error. For a pass/fail verdict use check_asset_optimization, for graded stats analyze_asset_performance.
//   @param asset_path - Object path ("/Game/Meshes/SM_Rock.SM_Rock") or package path ("/Game/Meshes/SM_Rock") of an asset the asset registry knows
//   @returns { asset_path: string, asset_class: string, file_size_bytes: number, file_size_mb: number, suggestions: [string], critical_suggestions: [string], performance_suggestions: [string], memory_suggestions: [string], lod_count?: number, triangle_count?: number, vertex_count?: number, width?: number, height?: number, mip_count?: number, compression_settings?: string, pixel_format?: string, parameter_count?: number, texture_sample_count?: number }

TSharedPtr<FJsonObject> HandleSuggestAssetOptimizations(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
    UObject* Target = nullptr;
    int64 DiskBytes = 0;
    if (TSharedPtr<FJsonObject> Error = Impl::StartAssetReport(Params, Plan, Target, DiskBytes)) { return Error; }

    // Three themed buckets, merged at the end behind any leading note and ahead of the
    // generic advice that every asset gets.
    TArray<FString> LeadingNotes;
    TArray<FString> Critical;
    TArray<FString> Performance;
    TArray<FString> Memory;

    if (UStaticMesh* Mesh = Cast<UStaticMesh>(Target))
    {
        const Impl::FMeshLodTotals Totals = Impl::SumStaticMeshLods(*Mesh);
        Plan->SetNumberField(TEXT("lod_count"), Totals.LodCount);
        Plan->SetNumberField(TEXT("triangle_count"), Totals.Triangles);
        Plan->SetNumberField(TEXT("vertex_count"), Totals.Vertices);

        if (Totals.LodCount < 2)
        {
            Impl::AppendLines(Critical, {
                TEXT("CRITICAL: Enable LOD settings for better performance"),
                TEXT("CRITICAL: Configure at least 2-3 LOD levels") });
        }
        if (Totals.Triangles > 100000)
        {
            Impl::AppendLines(Critical, {
                TEXT("CRITICAL: High triangle count (>100k) - consider reducing"),
                TEXT("CRITICAL: Use LOD settings to reduce triangle count at distance") });
        }
        Impl::AppendLines(Performance, {
            TEXT("Enable LOD settings for better performance"),
            TEXT("Consider mesh compression"),
            TEXT("Check polygon count and complexity"),
            TEXT("Use appropriate collision complexity"),
            TEXT("Consider mesh simplification"),
            TEXT("Use instanced rendering for repeated meshes") });
        Impl::AppendLines(Memory, {
            TEXT("Consider mesh compression to reduce memory usage"),
            TEXT("Use texture streaming for large meshes"),
            TEXT("Consider mesh splitting for very large meshes") });
    }
    else if (UTexture2D* Texture = Cast<UTexture2D>(Target))
    {
        const Impl::FTextureFacts Facts = Impl::ReadTextureFacts(*Texture);
        Impl::PutTextureFacts(Plan, Facts);

        if (Facts.Width > 4096 || Facts.Height > 4096)
        {
            Impl::AppendLines(Critical, {
                TEXT("CRITICAL: Very high resolution texture (>4K) - consider reducing"),
                TEXT("CRITICAL: Use texture streaming for large textures") });
        }
        if (Facts.Mips < 2)
        {
            Critical.Add(TEXT("CRITICAL: No mip maps configured - enable for better performance"));
        }
        Impl::AppendLines(Performance, {
            TEXT("Use appropriate texture compression"),
            TEXT("Enable mip maps for better performance"),
            TEXT("Consider texture resolution"),
            TEXT("Use texture streaming if needed"),
            TEXT("Consider texture atlasing for small textures"),
            TEXT("Use texture compression formats (DXT, BC, etc.)") });
        Impl::AppendLines(Memory, {
            TEXT("Use texture compression to reduce memory usage"),
            TEXT("Consider texture resolution reduction"),
            TEXT("Use texture streaming for large textures"),
            TEXT("Consider texture format optimization") });
    }
    else if (UMaterialInterface* Material = Cast<UMaterialInterface>(Target))
    {
        const Impl::FMaterialParamTally ParamTally = Impl::TallyMaterialParameters(*Material);
        Plan->SetNumberField(TEXT("parameter_count"), ParamTally.Total);
        Plan->SetNumberField(TEXT("texture_sample_count"), ParamTally.Textures);

        if (ParamTally.Total > 50)
        {
            Impl::AppendLines(Critical, {
                TEXT("CRITICAL: High parameter count (>50) - simplify material"),
                TEXT("CRITICAL: Consider breaking material into multiple instances") });
        }
        if (ParamTally.Textures > 10)
        {
            Critical.Add(TEXT("CRITICAL: High texture sample count (>10) - reduce texture samples"));
        }
        Impl::AppendLines(Performance, {
            TEXT("Simplify material complexity"),
            TEXT("Use material instances when possible"),
            TEXT("Check shader complexity"),
            TEXT("Consider material parameter collections"),
            TEXT("Use material functions for reusable logic"),
            TEXT("Optimize material node connections") });
        Impl::AppendLines(Memory, {
            TEXT("Use material instances to reduce memory usage"),
            TEXT("Consider material parameter collections"),
            TEXT("Use material functions for code reuse") });
    }
    else
    {
        LeadingNotes.Add(TEXT("Asset type not analyzed for specific optimizations"));
    }

    // Package size advice applies to every class.
    if (DiskBytes > 50 * Impl::OneMiB)
    {
        Critical.Add(TEXT("CRITICAL: File size is very large (>50MB) - consider splitting or compressing"));
        Impl::AppendLines(Memory, {
            TEXT("Consider asset splitting for very large files"),
            TEXT("Use compression to reduce file size") });
    }
    else if (DiskBytes > 10 * Impl::OneMiB)
    {
        Performance.Add(TEXT("File size could be optimized (<10MB recommended) - consider compression"));
        Memory.Add(TEXT("Consider compression to reduce file size"));
    }

    TArray<FString> Everything = LeadingNotes;
    Everything.Append(Critical);
    Everything.Append(Performance);
    Everything.Append(Memory);
    Impl::AppendLines(Everything, {
        TEXT("Consider asset streaming for large assets"),
        TEXT("Use appropriate asset compression settings"),
        TEXT("Consider asset LOD settings"),
        TEXT("Use asset optimization tools"),
        TEXT("Consider asset splitting for very large assets"),
        TEXT("Use asset compression formats"),
        TEXT("Consider asset streaming settings"),
        TEXT("Use asset optimization profiles") });

    Plan->SetArrayField(TEXT("suggestions"), mcp_common::StringsToJson(Everything));
    Plan->SetArrayField(TEXT("critical_suggestions"), mcp_common::StringsToJson(Critical));
    Plan->SetArrayField(TEXT("performance_suggestions"), mcp_common::StringsToJson(Performance));
    Plan->SetArrayField(TEXT("memory_suggestions"), mcp_common::StringsToJson(Memory));
    return Plan;
}

// Asset settings

// MCP:set_asset_compression - Sets the REAL texture compression quality (UTexture::CompressionQuality) on a texture asset and saves it to disk. Only UTexture targets are supported (no engine-level "compression" concept exists for meshes/materials); non-texture assets fail honestly with success:false.
//   @param asset_path - The path to the asset in the asset registry.
//   @param compression_type - The compression quality to apply: 'High', 'Medium', or 'Low' (case-insensitive). Maps to TCQ_High / TCQ_Medium / TCQ_Low. Any other value is a hard error.
//   @returns { asset_path: string, compression_type: string, asset_type: string, original_file_size_bytes: number, original_file_size_mb: number, is_texture: bool, compression_quality_applied?: string, current_compression_settings?: string, pixel_format?: string, width?: number, height?: number, mip_count?: number, saved: bool, success: bool, message: string, error?: string }

TSharedPtr<FJsonObject> HandleSetAssetCompression(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    FString RequestedAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString QualityToken;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("compression_type"), QualityToken, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The token is validated before the asset is even looked up.
    TEnumAsByte<ETextureCompressionQuality> WantedQuality = TCQ_Default;
    if (!Impl::QualityFromToken(QualityToken, WantedQuality))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid 'compression_type' \"%s\" -- must be one of: High, Medium, Low (case-insensitive)"), *QualityToken));
    }

    FAssetData Entry;
    UObject* Target = nullptr;
    if (TSharedPtr<FJsonObject> LookupFailure = Impl::LoadAssetForCommand(RequestedAsset, Entry, Target))
    {
        return LookupFailure;
    }

    // The package size is context only; nothing here predicts a compressed size.
    const int64 DiskBytes = Impl::PackageBytesOnDisk(Entry);
    const FString ClassName = Target->GetClass()->GetName();
    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("asset_path"), RequestedAsset);
    Outcome->SetStringField(TEXT("compression_type"), QualityToken);
    Outcome->SetStringField(TEXT("asset_type"), ClassName);
    Outcome->SetNumberField(TEXT("original_file_size_bytes"), DiskBytes);
    Outcome->SetNumberField(TEXT("original_file_size_mb"), Impl::InMegabytes(DiskBytes));

    // CompressionQuality only exists on UTexture; every other class is refused, unchanged.
    UTexture* Texture = Cast<UTexture>(Target);
    if (Texture == nullptr)
    {
        const FString Refusal = FString::Printf(
            TEXT("'%s' is a %s, not a UTexture -- this tool only mutates real compression on textures (UTexture::CompressionQuality). No engine-level compression concept applies to this asset type."),
            *RequestedAsset, *ClassName);
        Outcome->SetBoolField(TEXT("is_texture"), false);
        Outcome->SetBoolField(TEXT("saved"), false);
        Outcome->SetBoolField(TEXT("success"), false);
        Outcome->SetStringField(TEXT("message"), Refusal);
        Outcome->SetStringField(TEXT("error"), Refusal);
        return Outcome;
    }
    Outcome->SetBoolField(TEXT("is_texture"), true);

    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetAssetCompression", "Set Asset Compression Quality"));
        Texture->Modify();
        Texture->CompressionQuality = WantedQuality;
        Texture->PostEditChange();
        Texture->MarkPackageDirty();
    }

    // Everything below is read back from the texture after PostEditChange.
    const FString QualityNow = Impl::CompressionQualityName(Texture->CompressionQuality);
    Outcome->SetStringField(TEXT("compression_quality_applied"), QualityNow);
    Outcome->SetStringField(TEXT("current_compression_settings"), Impl::CompressionSettingsName(Texture->CompressionSettings));
    UTexture2D* FlatTexture = Cast<UTexture2D>(Texture);
    Outcome->SetStringField(TEXT("pixel_format"), Impl::PixelFormatLabel(FlatTexture));
    if (FlatTexture != nullptr)
    {
        Outcome->SetNumberField(TEXT("width"), FlatTexture->GetSizeX());
        Outcome->SetNumberField(TEXT("height"), FlatTexture->GetSizeY());
        Outcome->SetNumberField(TEXT("mip_count"), FlatTexture->GetNumMips());
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Texture->GetPackage(), Texture, SaveError);
    Outcome->SetBoolField(TEXT("saved"), bSaved);
    Outcome->SetBoolField(TEXT("success"), bSaved);
    if (!bSaved)
    {
        const FString SaveFailure = FString::Printf(
            TEXT("CompressionQuality set to %s in memory but the asset FAILED to save: %s"), *QualityNow, *SaveError);
        Outcome->SetStringField(TEXT("message"), SaveFailure);
        Outcome->SetStringField(TEXT("error"), SaveFailure);
        return Outcome;
    }
    Outcome->SetStringField(TEXT("message"), FString::Printf(TEXT("CompressionQuality set to %s and saved to disk."), *QualityNow));
    return Outcome;
}

// MCP:set_asset_lod_settings - Sets the REAL LOD count on a UStaticMesh (adds/removes FStaticMeshSourceModel entries via AddSourceModel/RemoveSourceModel, rebuilds via PostEditChange, saves to disk). SkeletalMesh LOD-count mutation is NOT supported (would require the FLODUtilities mesh-reduction subsystem, out of scope) -- read-only LOD info is still reported honestly. Non-mesh assets fail with success:false.
//   @param asset_path - The path to the asset in the asset registry.
//   @param lod_count - The desired number of LOD levels (1-8). Must be an integer. Only mutates for UStaticMesh; reported but not applied for SkeletalMesh/other.
//   @returns { asset_path: string, requested_lod_count: number, asset_type: string, current_lod_count?: number, total_triangles?: number, total_vertices?: number, lod_data?: [object], saved?: bool, success: bool, message: string, error?: string, final_lod_count: number, lod_efficiency?: number, lod_recommendations?: [string] }

TSharedPtr<FJsonObject> HandleSetAssetLodSettings(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    FString RequestedAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    int32 WantedLods = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("lod_count"), WantedLods)) { return Missing; }

    FAssetData Entry;
    UObject* Target = nullptr;
    if (TSharedPtr<FJsonObject> LookupFailure = Impl::LoadAssetForCommand(RequestedAsset, Entry, Target))
    {
        return LookupFailure;
    }

    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("asset_path"), RequestedAsset);
    Outcome->SetNumberField(TEXT("requested_lod_count"), WantedLods);

    // Only static meshes are changed: their LOD set is the list of source models.
    if (UStaticMesh* Mesh = Cast<UStaticMesh>(Target))
    {
        Outcome->SetStringField(TEXT("asset_type"), TEXT("StaticMesh"));
        const int32 LodsBefore = Mesh->GetNumSourceModels();
        Outcome->SetNumberField(TEXT("current_lod_count"), LodsBefore);

        int32 TriangleSum = 0;
        int32 VertexSum = 0;
        if (WantedLods < 1 || WantedLods > 8)
        {
            // Rejected before touching the mesh; lod_data still describes it as it is now.
            Outcome->SetArrayField(TEXT("lod_data"), Impl::RenderLodRows(*Mesh, TriangleSum, VertexSum));
            const FString Rejection = TEXT("Invalid lod_count. Must be between 1 and 8 -- no mutation attempted.");
            Outcome->SetBoolField(TEXT("success"), false);
            Outcome->SetBoolField(TEXT("saved"), false);
            Outcome->SetStringField(TEXT("message"), Rejection);
            Outcome->SetStringField(TEXT("error"), Rejection);
            Outcome->SetNumberField(TEXT("final_lod_count"), LodsBefore);
            return Outcome;
        }

        {
            FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetAssetLodSettings", "Set Static Mesh LOD Count"));
            Mesh->Modify();
            Impl::MatchSourceModelCount(*Mesh, WantedLods);
            Mesh->PostEditChange(); // rebuilds the render data for the new LOD set
            Mesh->MarkPackageDirty();
        }

        // Counts and per-LOD rows come from the rebuilt mesh.
        const int32 LodsAfter = Mesh->GetNumSourceModels();
        Outcome->SetNumberField(TEXT("final_lod_count"), LodsAfter);
        Outcome->SetArrayField(TEXT("lod_data"), Impl::RenderLodRows(*Mesh, TriangleSum, VertexSum));
        Outcome->SetNumberField(TEXT("total_triangles"), TriangleSum);
        Outcome->SetNumberField(TEXT("total_vertices"), VertexSum);

        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Mesh->GetPackage(), Mesh, SaveError);
        Outcome->SetBoolField(TEXT("saved"), bSaved);
        Outcome->SetBoolField(TEXT("success"), bSaved);
        if (bSaved)
        {
            Outcome->SetStringField(TEXT("message"), FString::Printf(
                TEXT("LOD count changed from %d to %d and saved to disk."), LodsBefore, LodsAfter));
        }
        else
        {
            const FString SaveFailure = FString::Printf(
                TEXT("LOD count changed from %d to %d in memory but the asset FAILED to save: %s"), LodsBefore, LodsAfter, *SaveError);
            Outcome->SetStringField(TEXT("message"), SaveFailure);
            Outcome->SetStringField(TEXT("error"), SaveFailure);
        }

        // Display-only advice derived from the counts after the change.
        const float LodRatio = (LodsBefore > 0) ? (static_cast<float>(LodsAfter) / static_cast<float>(LodsBefore)) : 0.0f;
        Outcome->SetNumberField(TEXT("lod_efficiency"), LodRatio);
        TArray<FString> LodAdvice;
        if (LodsAfter < 3)
        {
            LodAdvice.Add(TEXT("Consider adding more LODs for better performance"));
        }
        if (LodsAfter > 6)
        {
            LodAdvice.Add(TEXT("Consider reducing LOD count to avoid memory overhead"));
        }
        if (TriangleSum > 100000)
        {
            LodAdvice.Add(TEXT("High triangle count detected - LODs are essential"));
        }
        Outcome->SetArrayField(TEXT("lod_recommendations"), mcp_common::StringsToJson(LodAdvice));
        return Outcome;
    }

    // Skeletal meshes would need the mesh-reduction subsystem to change their LOD count, so
    // they are reported read-only and the request is refused.
    if (USkeletalMesh* SkinnedMesh = Cast<USkeletalMesh>(Target))
    {
        Outcome->SetStringField(TEXT("asset_type"), TEXT("SkeletalMesh"));
        const int32 LodsNow = SkinnedMesh->GetLODNum();
        Outcome->SetNumberField(TEXT("current_lod_count"), LodsNow);
        Outcome->SetArrayField(TEXT("lod_data"), Impl::SkeletalLodRows(*SkinnedMesh, LodsNow));

        const FString Unsupported = TEXT("Skeletal mesh LOD count is not supported by this tool (would require the FLODUtilities/IMeshReduction mesh-reduction subsystem -- out of scope). Read-only LOD info reported above; no change was made.");
        Outcome->SetBoolField(TEXT("success"), false);
        Outcome->SetBoolField(TEXT("saved"), false);
        Outcome->SetStringField(TEXT("message"), Unsupported);
        Outcome->SetStringField(TEXT("error"), Unsupported);
        Outcome->SetNumberField(TEXT("final_lod_count"), LodsNow);
        return Outcome;
    }

    // Any other class has no LOD concept here (and, unlike the branches above, reports no "saved").
    const FString NotApplicable = TEXT("LOD settings not applicable to this asset type");
    Outcome->SetStringField(TEXT("asset_type"), TEXT("Other"));
    Outcome->SetBoolField(TEXT("success"), false);
    Outcome->SetStringField(TEXT("message"), NotApplicable);
    Outcome->SetStringField(TEXT("error"), NotApplicable);
    Outcome->SetNumberField(TEXT("final_lod_count"), 0);
    return Outcome;
}

// MCP:configure_asset_streaming - Sets the REAL NeverStream flag (UStreamableRenderAsset::NeverStream) on a texture or static mesh and saves it to disk. enable_streaming=true clears NeverStream (asset streams on demand); enable_streaming=false sets it (asset stays fully resident). Assets with no such knob (e.g. Material) fail honestly with success:false.
//   @param asset_path - The path to the asset in the asset registry.
//   @param enable_streaming - Whether to enable streaming for the asset (maps to NeverStream = !enable_streaming).
//   @returns { asset_path: string, enable_streaming: bool, asset_type: string, file_size_bytes: number, file_size_mb: number, is_streamable: bool, never_stream?: bool, saved?: bool, success: bool, message: string, error?: string }

TSharedPtr<FJsonObject> HandleConfigureAssetStreaming(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentOptimizationImpl;

    FString RequestedAsset;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), RequestedAsset, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bStreamingWanted = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("enable_streaming"), bStreamingWanted)) { return Missing; }

    FAssetData Entry;
    UObject* Target = nullptr;
    if (TSharedPtr<FJsonObject> LookupFailure = Impl::LoadAssetForCommand(RequestedAsset, Entry, Target))
    {
        return LookupFailure;
    }

    const int64 DiskBytes = Impl::PackageBytesOnDisk(Entry);
    const FString ClassName = Target->GetClass()->GetName();
    TSharedPtr<FJsonObject> Outcome = MakeShared<FJsonObject>();
    Outcome->SetStringField(TEXT("asset_path"), RequestedAsset);
    Outcome->SetBoolField(TEXT("enable_streaming"), bStreamingWanted);
    Outcome->SetStringField(TEXT("asset_type"), ClassName);
    Outcome->SetNumberField(TEXT("file_size_bytes"), DiskBytes);
    Outcome->SetNumberField(TEXT("file_size_mb"), Impl::InMegabytes(DiskBytes));

    // NeverStream is declared on UStreamableRenderAsset, the common base of textures and static
    // meshes; any other class has no such flag and is refused unchanged.
    UStreamableRenderAsset* Streamable = Cast<UStreamableRenderAsset>(Target);
    if (Streamable == nullptr)
    {
        const FString Refusal = FString::Printf(
            TEXT("'%s' is a %s -- streaming (NeverStream) only applies to UStreamableRenderAsset (Texture/StaticMesh); no such knob exists on this asset type at the engine level."),
            *RequestedAsset, *ClassName);
        Outcome->SetBoolField(TEXT("is_streamable"), false);
        Outcome->SetBoolField(TEXT("saved"), false);
        Outcome->SetBoolField(TEXT("success"), false);
        Outcome->SetStringField(TEXT("message"), Refusal);
        Outcome->SetStringField(TEXT("error"), Refusal);
        return Outcome;
    }
    Outcome->SetBoolField(TEXT("is_streamable"), true);

    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ConfigureAssetStreaming", "Configure Asset Streaming"));
        Streamable->Modify();
        Streamable->NeverStream = !bStreamingWanted;
        Streamable->PostEditChange();
        Streamable->MarkPackageDirty();
    }

    // The reported flag is read back after PostEditChange, not echoed from the request.
    const bool bNeverStreamNow = (Streamable->NeverStream != 0);
    Outcome->SetBoolField(TEXT("never_stream"), bNeverStreamNow);
    const TCHAR* FlagWord = bNeverStreamNow ? TEXT("true") : TEXT("false");

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Streamable->GetPackage(), Streamable, SaveError);
    Outcome->SetBoolField(TEXT("saved"), bSaved);
    Outcome->SetBoolField(TEXT("success"), bSaved);
    if (!bSaved)
    {
        const FString SaveFailure = FString::Printf(
            TEXT("NeverStream set to %s in memory but the asset FAILED to save: %s"), FlagWord, *SaveError);
        Outcome->SetStringField(TEXT("message"), SaveFailure);
        Outcome->SetStringField(TEXT("error"), SaveFailure);
        return Outcome;
    }
    Outcome->SetStringField(TEXT("message"), FString::Printf(
        TEXT("NeverStream set to %s (streaming %s) and saved to disk."),
        FlagWord, bStreamingWanted ? TEXT("enabled") : TEXT("disabled")));
    return Outcome;
}

// ---------------------------------------------------------------------------
// Read-side inverses of the 3 setters above.
// ---------------------------------------------------------------------------

// MCP:get_asset_compression - Read a TEXTURE's compression settings (inverse of set_asset_compression): TC_* enum names plus pixel format and LOD group. Accepts UTexture2D / UTexture2DArray / UTextureCube / UTextureRenderTarget2D; hand it anything else and it does not error -- it answers is_texture=false and reports asset_class, so check that flag before reading the rest.
//   @param asset_path - Full /Game/... asset path
//   @returns { asset_path, asset_class, is_texture, compression_settings?, pixel_format?, lod_group?, source_format?, srgb?, never_stream?, note? }

TSharedPtr<FJsonObject> HandleGetAssetCompression(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    FAssetData AD;
    UObject* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentOptimizationImpl::LoadAssetForCommand(AssetPath, AD, Asset)) { return Error; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

    UTexture* Tex = Cast<UTexture>(Asset);
    if (!Tex)
    {
        Data->SetBoolField(TEXT("is_texture"), false);
        Data->SetStringField(TEXT("note"),
            TEXT("asset is not a UTexture -- compression_settings only applies to textures"));
        return mcp_common::MakeOkReply(Data);
    }

    Data->SetBoolField(TEXT("is_texture"), true);
    Data->SetStringField(TEXT("compression_settings"), UEAgentOptimizationImpl::CompressionSettingsName(Tex->CompressionSettings));
    Data->SetStringField(TEXT("pixel_format"), UEAgentOptimizationImpl::PixelFormatLabel(Cast<UTexture2D>(Tex)));
    Data->SetStringField(TEXT("lod_group"), UTexture::GetTextureGroupString(Tex->LODGroup.GetValue()));
    Data->SetNumberField(TEXT("lod_group_int"), Tex->LODGroup.GetValue());
    Data->SetBoolField(TEXT("srgb"), Tex->SRGB);
    Data->SetBoolField(TEXT("use_virtual_texture_streaming_priority"), Tex->bUseVirtualTextureStreamingPriority);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_asset_lod_settings - Read a mesh's LOD config (inverse of set_asset_lod_settings): per-LOD triangle count, vertex count and screen size for UStaticMesh or USkeletalMesh, plus LODGroup, which exists on StaticMesh only. There is NO lod-bias field in the response -- see the note below before you go looking for it.
//   @param asset_path - Full /Game/... asset path
//   @returns { asset_path, asset_class, lod_count, lod_group?, lods:[{ lod_index, screen_size?, triangle_count, vertex_count }] }
//   NOTE: there is no `current_lod_bias` field -- UStaticMesh has no direct 1:1 "LOD bias" property in the Engine
//   module (the closest real concepts, MinLOD / MinQualityLevelLOD, are a different thing: a LOD
//   floor, not a bias/offset) and USkeletalMesh has no per-asset LODGroup/bias concept at all.

TSharedPtr<FJsonObject> HandleGetAssetLodSettings(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    FAssetData AD;
    UObject* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentOptimizationImpl::LoadAssetForCommand(AssetPath, AD, Asset)) { return Error; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

    if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
    {
        const int32 LODCount = StaticMesh->GetNumLODs();
        Data->SetNumberField(TEXT("lod_count"), LODCount);
        // GetLODGroup() is the real, non-deprecated accessor for the LOD group this mesh belongs
        // to (direct member access is deprecated and unavailable).
        const FName LODGroupName = StaticMesh->GetLODGroup();
        if (!LODGroupName.IsNone())
        {
            Data->SetStringField(TEXT("lod_group"), LODGroupName.ToString());
        }

        TArray<TSharedPtr<FJsonValue>> LodsJson;
        if (StaticMesh->GetRenderData() && StaticMesh->GetRenderData()->LODResources.Num() > 0)
        {
            for (int32 LODIndex = 0; LODIndex < LODCount; LODIndex++)
            {
                FStaticMeshLODResources& LODRes = StaticMesh->GetRenderData()->LODResources[LODIndex];
                TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                E->SetNumberField(TEXT("lod_index"), LODIndex);
                E->SetNumberField(TEXT("triangle_count"), LODRes.GetNumTriangles());
                E->SetNumberField(TEXT("vertex_count"), LODRes.GetNumVertices());
                LodsJson.Add(MakeShared<FJsonValueObject>(E));
            }
        }
        Data->SetArrayField(TEXT("lods"), LodsJson);
        return mcp_common::MakeOkReply(Data);
    }

    if (USkeletalMesh* SkMesh = Cast<USkeletalMesh>(Asset))
    {
        const int32 LODCount = SkMesh->GetLODNum();
        Data->SetNumberField(TEXT("lod_count"), LODCount);

        TArray<TSharedPtr<FJsonValue>> LodsJson;
        for (int32 LODIndex = 0; LODIndex < LODCount; LODIndex++)
        {
            FSkeletalMeshLODInfo* LODInfo = SkMesh->GetLODInfo(LODIndex);
            TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
            E->SetNumberField(TEXT("lod_index"), LODIndex);
            if (LODInfo)
            {
                E->SetNumberField(TEXT("screen_size"), LODInfo->ScreenSize.Default);
                E->SetNumberField(TEXT("lod_hysteresis"), LODInfo->LODHysteresis);
            }
            LodsJson.Add(MakeShared<FJsonValueObject>(E));
        }
        Data->SetArrayField(TEXT("lods"), LodsJson);
        return mcp_common::MakeOkReply(Data);
    }

    Data->SetNumberField(TEXT("lod_count"), 0);
    Data->SetStringField(TEXT("note"),
        TEXT("asset is not a UStaticMesh or USkeletalMesh -- LOD config only applies to mesh assets"));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_asset_streaming_config - Read the streaming knobs (inverse of configure_asset_streaming). Works at two levels: with NO asset_path it reports the GLOBAL texture-streaming pool size (only when the editor runs a render-asset streaming manager -- an editor started with -nullrhi has none, and then streaming_manager_available is false and the pool size is omitted); with one, it adds that asset's NeverStream flag (which applies to any UStreamableRenderAsset -- textures AND static meshes) plus the texture-only LODGroup and VirtualTextureStreamingPriority. There is NO `streaming_priority` field -- see the note below.
//   @param asset_path - Optional /Game/... asset path; if omitted, returns global streaming config only
//   @returns { streaming_manager_available: bool, global_pool_size?: int, asset_path?, asset_class?, is_streamable?: bool, never_stream?: bool, lod_group?: string, lod_group_int?: number, use_virtual_texture_streaming_priority?: bool, note? }
//   NOTE: there is no `streaming_priority` field -- UTexture::StreamingPriority does not exist
//   (Texture.h); the closest knob, VirtualTextureStreamingPriority, is reported as
//   `use_virtual_texture_streaming_priority`.

TSharedPtr<FJsonObject> HandleGetAssetStreamingConfig(const TSharedPtr<FJsonObject>& Params)
{
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    // Global pool size from the render-asset streaming manager. That manager only exists when the
    // RHI streams textures and meshes (not under -nullrhi), and its accessor check()s when it is
    // missing, so ask first; IsRenderAssetStreamingEnabled(None) is true exactly when it exists.
    // Without it there is no pool to read, and the size is left out rather than reported as 0.
    FStreamingManagerCollection& Streaming = IStreamingManager::Get();
    const bool bHasStreamingManager = Streaming.IsRenderAssetStreamingEnabled(EStreamableRenderAssetType::None);
    Data->SetBoolField(TEXT("streaming_manager_available"), bHasStreamingManager);
    if (bHasStreamingManager)
    {
        Data->SetNumberField(TEXT("global_pool_size"), Streaming.GetRenderAssetStreamingManager().GetPoolSize());
    }

    FString AssetPath;
    Params->TryGetStringField(TEXT("asset_path"), AssetPath);
    if (AssetPath.IsEmpty())
    {
        Data->SetStringField(TEXT("note"), TEXT("asset_path omitted -- global streaming config only"));
        return mcp_common::MakeOkReply(Data);
    }

    FAssetData AD;
    UObject* Asset = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentOptimizationImpl::LoadAssetForCommand(AssetPath, AD, Asset)) { return Error; }

    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

    // never_stream (UStreamableRenderAsset::NeverStream) is the real inverse of
    // configure_asset_streaming's mutation -- it applies to both UTexture AND UStaticMesh.
    UStreamableRenderAsset* Streamable = Cast<UStreamableRenderAsset>(Asset);
    if (Streamable)
    {
        Data->SetBoolField(TEXT("is_streamable"), true);
        Data->SetBoolField(TEXT("never_stream"), Streamable->NeverStream != 0);
    }
    else
    {
        Data->SetBoolField(TEXT("is_streamable"), false);
    }

    if (UTexture* Tex = Cast<UTexture>(Asset))
    {
        // UTexture has no StreamingPriority. Its texture-only knobs are:
        //   - LODGroup (TextureGroup enum) — drives memory budget per pool
        //   - bUseVirtualTextureStreamingPriority (bool) + VirtualTextureStreamingPriority (EVTProducerPriority)
        Data->SetStringField(TEXT("lod_group"), UTexture::GetTextureGroupString(Tex->LODGroup.GetValue()));
        Data->SetNumberField(TEXT("lod_group_int"), Tex->LODGroup.GetValue());
        Data->SetBoolField(TEXT("use_virtual_texture_streaming_priority"), Tex->bUseVirtualTextureStreamingPriority);
    }
    else if (!Streamable)
    {
        Data->SetStringField(TEXT("note"),
            TEXT("asset is not a UStreamableRenderAsset (Texture/StaticMesh) -- never_stream and the "
                 "texture-only knobs (LODGroup / VirtualTextureStreamingPriority) do not apply; "
                 "global_pool_size is still reported whenever the editor has a streaming manager"));
    }
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("analyze_asset_performance"), &HandleAnalyzeAssetPerformance },
    { TEXT("analyze_assets_performance_batch"), &HandleAnalyzeAssetsPerformanceBatch },
    { TEXT("check_asset_optimization"), &HandleCheckAssetOptimization },
    { TEXT("configure_asset_streaming"), &HandleConfigureAssetStreaming },
    { TEXT("get_asset_compression"), &HandleGetAssetCompression },
    { TEXT("get_asset_lod_settings"), &HandleGetAssetLodSettings },
    { TEXT("get_asset_memory_usage"), &HandleGetAssetMemoryUsage },
    { TEXT("get_asset_streaming_config"), &HandleGetAssetStreamingConfig },
    { TEXT("set_asset_compression"), &HandleSetAssetCompression },
    { TEXT("set_asset_lod_settings"), &HandleSetAssetLodSettings },
    { TEXT("suggest_asset_optimizations"), &HandleSuggestAssetOptimizations },
};
} // namespace UEAgentOptimizationCommands
