// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Asset/AssetCommandHelpers.h"

#include "Misc/PackageName.h"                 // FPackageName::TryConvertLongPackageNameToFilename / ObjectPathToPackageName
#include "HAL/FileManager.h"                  // IFileManager::Get().FindFilesRecursive
#include "Misc/App.h"                         // FApp::CanEverRender
#include "RHI.h"                              // GIsRHIInitialized

#include "AssetRegistry/AssetRegistryModule.h" // FAssetRegistryModule / IAssetRegistry
#include "AssetRegistry/IAssetRegistry.h"      // IAssetRegistry::GetDependencies
#include "Misc/AssetRegistryInterface.h"       // UE::AssetRegistry::EDependencyCategory

#include "UObject/Package.h"                  // UPackage
#include "ObjectTools.h"                      // ThumbnailTools::* (UnrealEd)
#include "Misc/ObjectThumbnail.h"             // FObjectThumbnail

#include "Curves/CurveFloat.h"                // UCurveFloat::GetFloatValue
#include "Curves/CurveVector.h"               // UCurveVector::GetVectorValue
#include "Curves/CurveLinearColor.h"          // UCurveLinearColor::GetLinearColorValue
#include "Curves/CurveBase.h"                 // UCurveBase

// rename_assets via IAssetTools.
#include "IAssetTools.h"
#include "AssetToolsModule.h"

// rename_assets guards (readonly roots / PIE / CDO-modal pre-detection /
// per-item disk verification / dirty-referencer classification).
#include "Editor.h"                                          // GEditor
#include "Editor/LevelEditor/Public/LevelEditorSubsystem.h"   // ULevelEditorSubsystem::IsInPlayInEditor (same include form as UEAgentSystemCommands.cpp)
#include "UObject/UObjectIterator.h"                          // TObjectIterator<UClass> (CDO sweep)
#include "UObject/UnrealType.h"                               // TFieldIterator<FObjectProperty> / TPropertyValueIterator<FSoftObjectProperty>
#include "UObject/SoftObjectPath.h"                           // FSoftObjectPath::GetWithoutSubPath
#include "UObject/SoftObjectPtr.h"                            // FSoftObjectPtr::ToSoftObjectPath
#include "UObject/ObjectRedirector.h"                         // UObjectRedirector (redirector_left probe)
#include "Engine/Blueprint.h"                                 // UBlueprint::GeneratedClass (engine's indirect-BP soft-ref case)
#include "Engine/World.h"                                     // UWorld::StaticClass()->GetClassPathName() (world-owned package test)
#include "CoreGlobals.h"                                      // GIsRunningUnattendedScript
#include "Templates/UnrealTemplate.h"                         // TGuardValue

// Cross-project / migrate / diff utilities.
#include "HAL/PlatformFileManager.h"          // IPlatformFile (copy dest dirs, file stat)
#include "Misc/FileHelper.h"                   // FFileHelper::LoadFileToArray (binary .uasset scan)
#include "Misc/Paths.h"                        // FPaths path math
#include "Misc/DateTime.h"                     // FDateTime::ParseIso8601 / Parse
#include "Misc/SecureHash.h"                   // FMD5Hash::HashFile (stable content hash)
#include "Internationalization/Regex.h"        // FRegexPattern / FRegexMatcher (optional pattern filter)
#include "AssetRegistry/ARFilter.h"            // FARFilter (redirector sweep after ingest)

/**
 * Low-level asset utilities: on-disk size, recursive dependencies, thumbnails and curve evaluation; a
 * guarded bulk rename (IAssetTools::RenameAssets behind PIE / read-only-root / CDO-modal guards, verified
 * item by item on disk); and cross-project ingest, scan, compare and diff over the filesystem and the
 * AssetRegistry graph.
 */
namespace UEAgentAssetUtilCommands
{

// MCP-DOMAIN: Asset

namespace
{
    /** Normalize an MCP asset_path (object path or package path) to a long package name. */
    FString ToLongPackageName(const FString& InAssetPath)
    {
        // ObjectPathToPackageName strips a trailing ".AssetName" object delimiter if present,
        // and returns the input unchanged for a plain package path. Safe for both forms.
        return FPackageName::ObjectPathToPackageName(InAssetPath);
    }

    /** The object path of an MCP asset_path: as given when it names an object, else "<Package>.<AssetName>". */
    FString ToObjectPath_AssetUtil(const FString& InAssetPath)
    {
        if (InAssetPath.Contains(TEXT(".")))
        {
            return InAssetPath;
        }
        const FString PackageName = ToLongPackageName(InAssetPath);
        const FString LeafName = FPackageName::GetLongPackageAssetName(PackageName);
        return FString::Printf(TEXT("%s.%s"), *PackageName, *LeafName);
    }

    /**
     * Resolve an optional dependency_type ("hard"|"soft"|"all", case-insensitive) into the
     * IAssetRegistry FDependencyQuery to pass alongside EDependencyCategory::Package. Mirrors the
     * proven hard/soft split from UEAgentAnalysisCommands::HandleGetAssetReferencers (the
     * 4-arg GetDependencies/GetReferencers overload). "all" (default, and anything unrecognized)
     * resolves to a default-constructed FDependencyQuery -- i.e. no hard/soft filtering.
     */
    UE::AssetRegistry::FDependencyQuery ResolveDependencyQuery_AssetUtil(const FString& DependencyType)
    {
        if (DependencyType.Equals(TEXT("hard"), ESearchCase::IgnoreCase))
        {
            return UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Hard);
        }
        if (DependencyType.Equals(TEXT("soft"), ESearchCase::IgnoreCase))
        {
            return UE::AssetRegistry::FDependencyQuery(UE::AssetRegistry::EDependencyQuery::Soft);
        }
        return UE::AssetRegistry::FDependencyQuery();
    }
}

// -------------------------------------------------------------------------- //
// get_asset_dependencies_recursive
// -------------------------------------------------------------------------- //
// MCP:get_asset_dependencies_recursive - Recursively walk an asset's package dependencies via the Asset Registry.  Breadth-first, deduplicated, capped at max_depth (default 3). Each returned dependency records the depth at which it was first reached (1 == direct).
//   @param asset_path - Object path or package path of the root asset.
//   @param max_depth - Maximum dependency depth to traverse (1..32, default 3).
//   @param dependency_type - Optional "hard"|"soft"|"all" (case-insensitive, default "all"). Filters every depth level of the walk via the AssetRegistry hard/soft FDependencyQuery split; "all" leaves the walk unfiltered.
//   @returns { asset_path: string, package_name: string, max_depth: int, dependency_type: string, dependencies: [{path, depth}], count: int }

TSharedPtr<FJsonObject> HandleGetAssetDependenciesRecursive(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    int32 MaxDepth = 3;
    double MaxDepthD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_depth"), MaxDepthD))
    {
        MaxDepth = FMath::Clamp(static_cast<int32>(MaxDepthD), 1, 32);
    }

    FString DependencyType = TEXT("all");
    Params->TryGetStringField(TEXT("dependency_type"), DependencyType);
    const UE::AssetRegistry::FDependencyQuery DepQuery = ResolveDependencyQuery_AssetUtil(DependencyType);

    const FString RootPackage = ToLongPackageName(AssetPath);
    if (!FPackageName::IsValidLongPackageName(RootPackage))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Not a valid long package name: %s"), *RootPackage));
    }

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    // Breadth-first walk with dedup. Record each newly-discovered dependency with the
    // depth at which it was first reached (1 == direct dependency of the root).
    TSet<FName> Seen;
    Seen.Add(FName(*RootPackage));

    TArray<TSharedPtr<FJsonValue>> DepArray;

    // Current frontier of package names to expand at the next depth level.
    TArray<FName> Frontier;
    Frontier.Add(FName(*RootPackage));

    for (int32 Depth = 1; Depth <= MaxDepth && Frontier.Num() > 0; ++Depth)
    {
        TArray<FName> NextFrontier;
        for (const FName& Pkg : Frontier)
        {
            TArray<FName> Dependencies;
            AssetRegistry.GetDependencies(Pkg, Dependencies, UE::AssetRegistry::EDependencyCategory::Package, DepQuery);
            for (const FName& Dep : Dependencies)
            {
                if (Seen.Contains(Dep))
                {
                    continue;
                }
                Seen.Add(Dep);

                TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("path"), Dep.ToString());
                Entry->SetNumberField(TEXT("depth"), Depth);
                DepArray.Add(MakeShared<FJsonValueObject>(Entry));

                NextFrontier.Add(Dep);
            }
        }
        Frontier = MoveTemp(NextFrontier);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("package_name"), RootPackage);
    Result->SetNumberField(TEXT("max_depth"), MaxDepth);
    Result->SetStringField(TEXT("dependency_type"), DependencyType);
    Result->SetArrayField(TEXT("dependencies"), DepArray);
    Result->SetNumberField(TEXT("count"), DepArray.Num());
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// generate_asset_thumbnail
// -------------------------------------------------------------------------- //
// MCP:generate_asset_thumbnail - Render and cache an asset's thumbnail (or report the existing cached one).  When the editor can render (FApp::CanEverRender() and the RHI is initialized), this generates a fresh thumbnail via ThumbnailTools and caches it into the package's thumbnail map. When rendering is unavailable, or the object type has no thumbnail renderer, it honestly reports the existing cached thumbnail state.
//   @param asset_path - Object path or package path of the asset.
//   @returns { asset_path: string, object_path: string, asset_class: string, rendered: bool, has_thumbnail: bool, width: int, height: int, compressed_bytes: int, message: string }

TSharedPtr<FJsonObject> HandleGenerateAssetThumbnail(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Load the object so there is a real UObject to render/inspect.
    const FString ObjectPath = ToObjectPath_AssetUtil(AssetPath);
    UObject* Asset = mcp_common::ResolveObjectByPath(ObjectPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset not found / could not be loaded: %s"), *ObjectPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), AssetPath);
    Result->SetStringField(TEXT("object_path"), Asset->GetPathName());
    Result->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetName());

    // Render only when the editor genuinely has a rendering device. RenderThumbnail
    // check()s GIsRHIInitialized and early-returns when !FApp::CanEverRender(), so we
    // mirror those preconditions and otherwise honestly report the cached state.
    const bool bCanRender = FApp::CanEverRender() && GIsRHIInitialized;

    if (bCanRender)
    {
        // Generates a fresh thumbnail (GPU render) and caches it into the package's
        // thumbnail map. Returns nullptr for object types that have no thumbnail renderer.
        FObjectThumbnail* Generated = ThumbnailTools::GenerateThumbnailForObjectToSaveToDisk(Asset);
        if (Generated && !Generated->IsEmpty())
        {
            Asset->MarkPackageDirty();
            Result->SetBoolField(TEXT("rendered"), true);
            Result->SetBoolField(TEXT("has_thumbnail"), true);
            Result->SetNumberField(TEXT("width"), Generated->GetImageWidth());
            Result->SetNumberField(TEXT("height"), Generated->GetImageHeight());
            Result->SetNumberField(TEXT("compressed_bytes"), Generated->GetCompressedDataSize());
            Result->SetStringField(TEXT("message"), TEXT("Thumbnail rendered and cached into package thumbnail map"));
            return mcp_common::MakeOkReply(Result);
        }

        // No renderer for this type (e.g. data-only assets). Fall through to report the
        // existing cached thumbnail honestly rather than faking a render.
        Result->SetBoolField(TEXT("rendered"), false);
        Result->SetStringField(TEXT("message"),
            TEXT("Object type has no thumbnail renderer; reporting existing cached thumbnail"));
    }
    else
    {
        Result->SetBoolField(TEXT("rendered"), false);
        Result->SetStringField(TEXT("message"),
            TEXT("Editor cannot render (headless / RHI not initialized); reporting existing cached thumbnail"));
    }

    // Honest fallback: read any existing in-memory cached thumbnail for this object.
    FObjectThumbnail* Existing = ThumbnailTools::GetThumbnailForObject(Asset);
    const bool bHasThumb = (Existing != nullptr) && !Existing->IsEmpty();
    Result->SetBoolField(TEXT("has_thumbnail"), bHasThumb);
    if (bHasThumb)
    {
        Result->SetNumberField(TEXT("width"), Existing->GetImageWidth());
        Result->SetNumberField(TEXT("height"), Existing->GetImageHeight());
        Result->SetNumberField(TEXT("compressed_bytes"), Existing->GetCompressedDataSize());
    }
    else
    {
        Result->SetNumberField(TEXT("width"), 0);
        Result->SetNumberField(TEXT("height"), 0);
        Result->SetNumberField(TEXT("compressed_bytes"), 0);
    }
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// evaluate_curve_at_time
// -------------------------------------------------------------------------- //
// MCP:evaluate_curve_at_time - Evaluate a curve asset at a given time.  Loads a UCurveFloat / UCurveVector / UCurveLinearColor and evaluates it at the requested time. For multi-channel curves (Vector=3, LinearColor=4) the optional channel selects which component the scalar 'value' field reports; the full component array is always returned in 'values'.
//   @param curve_path - Object path or package path of the curve asset.
//   @param time - The time (X) to evaluate at.
//   @param channel - Optional channel index for multi-channel curves (Vector/LinearColor) to populate 'value'.
//   @returns { curve_path: string, curve_class: string, time: float, curve_type: string, num_channels: int, channel: int, value: float, values: [float], vector: {x,y,z}, color: {r,g,b,a} }

TSharedPtr<FJsonObject> HandleEvaluateCurveAtTime(const TSharedPtr<FJsonObject>& Params)
{
    FString CurvePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("curve_path"), CurvePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    double TimeD = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("time"), TimeD)) { return Missing; }
    const float Time = static_cast<float>(TimeD);

    int32 Channel = 0;
    double ChannelD = 0.0;
    if (Params->TryGetNumberField(TEXT("channel"), ChannelD))
    {
        Channel = static_cast<int32>(ChannelD);
    }

    const FString ObjectPath = ToObjectPath_AssetUtil(CurvePath);
    UObject* CurveObj = StaticLoadObject(UCurveBase::StaticClass(), nullptr, *ObjectPath);
    if (!CurveObj)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Curve asset not found / not a UCurveBase: %s"), *ObjectPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("curve_path"), CurvePath);
    Result->SetStringField(TEXT("curve_class"), CurveObj->GetClass()->GetName());
    Result->SetNumberField(TEXT("time"), Time);

    if (UCurveFloat* CF = Cast<UCurveFloat>(CurveObj))
    {
        const float Value = CF->GetFloatValue(Time);
        Result->SetStringField(TEXT("curve_type"), TEXT("Float"));
        Result->SetNumberField(TEXT("num_channels"), 1);
        Result->SetNumberField(TEXT("channel"), 0);
        Result->SetNumberField(TEXT("value"), Value);

        TArray<TSharedPtr<FJsonValue>> ValuesArr;
        ValuesArr.Add(MakeShared<FJsonValueNumber>(Value));
        Result->SetArrayField(TEXT("values"), ValuesArr);
        return mcp_common::MakeOkReply(Result);
    }

    // values[] of a multi-channel curve, then the channel the scalar `value` reports: the requested
    // one, or 0 when it is out of range.
    auto AddChannelValues = [&Result, Channel](const TArray<TSharedPtr<FJsonValue>>& Values)
    {
        const int32 Reported = (Channel >= 0 && Channel < Values.Num()) ? Channel : 0;
        Result->SetArrayField(TEXT("values"), Values);
        Result->SetNumberField(TEXT("channel"), Reported);
        Result->SetNumberField(TEXT("value"), Values[Reported]->AsNumber());
    };

    if (UCurveVector* CV = Cast<UCurveVector>(CurveObj))
    {
        const FVector V = CV->GetVectorValue(Time);
        Result->SetStringField(TEXT("curve_type"), TEXT("Vector"));
        Result->SetNumberField(TEXT("num_channels"), 3);
        AddChannelValues(mcp_common::VectorToJsonArray(V));
        Result->SetObjectField(TEXT("vector"), mcp_common::VectorToJsonObject(V));
        return mcp_common::MakeOkReply(Result);
    }

    if (UCurveLinearColor* CC = Cast<UCurveLinearColor>(CurveObj))
    {
        const FLinearColor Color = CC->GetLinearColorValue(Time);
        Result->SetStringField(TEXT("curve_type"), TEXT("LinearColor"));
        Result->SetNumberField(TEXT("num_channels"), 4);
        AddChannelValues(mcp_common::ColorToJsonArray(Color));
        Result->SetObjectField(TEXT("color"), mcp_common::LinearColorToJsonObject(Color));
        return mcp_common::MakeOkReply(Result);
    }

    return mcp_common::MakeErrorReply(
        FString::Printf(TEXT("Unsupported curve class '%s' (expected UCurveFloat/UCurveVector/UCurveLinearColor)"),
            *CurveObj->GetClass()->GetName()));
}

namespace
{
    /**
     * Extract the folder (package path) from a full asset path.
     * e.g. "/Game/Characters/Hero" -> "/Game/Characters"
     */
    FString GetFolderFromAssetPath(const FString& AssetPath)
    {
        int32 LastSlash;
        if (AssetPath.FindLastChar(TEXT('/'), LastSlash))
        {
            return AssetPath.Left(LastSlash);
        }
        return TEXT("/Game");
    }

    /**
     * Extract the asset name from a full asset path.
     * e.g. "/Game/Characters/Hero" -> "Hero"
     */
    FString GetNameFromAssetPath(const FString& AssetPath)
    {
        int32 LastSlash;
        if (AssetPath.FindLastChar(TEXT('/'), LastSlash))
        {
            return AssetPath.Mid(LastSlash + 1);
        }
        return AssetPath;
    }
}

// ===========================================================================
// RENAME GUARDS of rename_assets. Why each guard exists is right below
// (all three are measured behaviors of IAssetTools::RenameAssets, not
// defensive guesses).
// ===========================================================================

// Why each guard exists. rename_assets, rename_asset and move_asset (UEAgentManagementCommands.cpp)
// all end up in the same IAssetTools::RenameAssets / FAssetRenameManager path, so they share three
// exposures:
//   1. PIE. RenameAssets re-saves referencing packages; doing that while a PIE session is live is the same
//      hazard fixup_redirectors refuses on (UEAgentSystemCommands.cpp).
//   2. Read-only roots. A vendored library root (e.g. /Game/VendorPack) is upstream; a rename of, or INTO,
//      it violates that. Applied to the SOURCE and the DESTINATION.
//   3. CDO references. FAssetRenameManager::PerformAssetRename opens an FMessageDialog when a NATIVE CDO
//      references an asset being renamed, and RenameAssets has no bWithDialog knob to turn that off.
//      FAssetRenameManager::FindCDOReferences is private, so FindCdoReferencesToAssets below reimplements
//      it from public API.

/**
 * True when a Play-In-Editor session is live (ULevelEditorSubsystem::IsInPlayInEditor). False when there
 * is no GEditor / no subsystem -- i.e. "cannot tell" is reported as "not in PIE", the same shape as the
 * fixup_redirectors guard in UEAgentSystemCommands.cpp.
 */
bool IsPlayInEditorActive()
{
    if (!GEditor)
    {
        return false;
    }
    ULevelEditorSubsystem* LevelEditorSubsystem = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
    return LevelEditorSubsystem && LevelEditorSubsystem->IsInPlayInEditor();
}

/**
 * Reimplementation of the PRIVATE FAssetRenameManager::FindCDOReferences from public API only:
 *   - hard refs: TObjectIterator<UClass> (skipping ClassGeneratedBy != nullptr,
 *     CLASS_Deprecated | CLASS_NewerVersionExists, and invalid CDOs) + TFieldIterator<FObjectProperty>,
 *     the engine's own loop;
 *   - soft refs: a deep property walk (TPropertyValueIterator<FSoftObjectProperty> with FullRecursion over
 *     the CDO) instead of the engine's private FSoftObjectPathRenameSerializer, compared against each
 *     asset's object path (plus a UBlueprint's GeneratedClass path, the engine's "indirect blueprint refs
 *     to their generated class counterparts" case).
 * The soft half is an APPROXIMATION of the engine's serializer-based scan: it reaches nested structs and
 * containers, but NOT soft paths that only exist inside instanced subobjects of the CDO. Consumers must
 * report it as such (rename_assets emits `soft_scan_is_approximate`), never as a guarantee that no
 * modal will appear.
 *
 * @return one JSON object per asset that has at least one CDO reference:
 *         { asset_path, hard_referencing_classes: [string], soft_referencing_classes: [string] }
 */
TArray<TSharedPtr<FJsonObject>> FindCdoReferencesToAssets(const TArray<UObject*>& Assets)
{
    TArray<TSharedPtr<FJsonObject>> Out;
    if (Assets.Num() == 0)
    {
        return Out;
    }

    // Lookups mirroring the engine's two checklists.
    TMap<const UObject*, int32> HardLookup;
    TMap<FString, int32>        SoftPathLookup; // lowercased object path (no subpath) -> asset index
    for (int32 Index = 0; Index < Assets.Num(); ++Index)
    {
        const UObject* Asset = Assets[Index];
        if (!Asset)
        {
            continue;
        }
        HardLookup.Add(Asset, Index);
        SoftPathLookup.Add(Asset->GetPathName().ToLower(), Index);
        // Engine parity: "indirect blueprint refs to their generated class
        // counterparts" (AssetRenameManager.cpp) -- a CDO that soft-refs
        // BP_Foo_C is a reference to the BP_Foo asset being renamed.
        if (const UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
        {
            if (Blueprint->GeneratedClass)
            {
                SoftPathLookup.Add(Blueprint->GeneratedClass->GetPathName().ToLower(), Index);
            }
        }
    }

    TMap<int32, TSet<FString>> HardRefsByAsset;
    TMap<int32, TSet<FString>> SoftRefsByAsset;

    for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
    {
        UClass* Cls = *ClassIt;
        // Engine parity: Blueprint-generated classes are skipped outright, since the modal is
        // only about NATIVE CDOs (source/config/text needing find-replace).
        UObject* CDO = AssetCommandHelpers::NativeClassDefault(Cls);
        if (!CDO)
        {
            continue;
        }

        // -- hard references: engine's TFieldIterator<FObjectProperty> loop ----
        for (TFieldIterator<FObjectProperty> PropIt(Cls); PropIt; ++PropIt)
        {
            const UObject* Value = PropIt->GetPropertyValue(PropIt->ContainerPtrToValuePtr<UObject>(CDO));
            if (!Value)
            {
                continue;
            }
            if (const int32* FoundIndex = HardLookup.Find(Value))
            {
                HardRefsByAsset.FindOrAdd(*FoundIndex).Add(Cls->GetPathName());
            }
        }

        // -- soft references: deep property walk over the CDO ------------------
        for (TPropertyValueIterator<FSoftObjectProperty> SoftIt(
                 Cls, CDO,
                 EPropertyValueIteratorFlags::FullRecursion,
                 EFieldIteratorFlags::ExcludeDeprecated);
             SoftIt; ++SoftIt)
        {
            const FSoftObjectProperty* SoftProp = SoftIt.Key();
            const void* ValuePtr = SoftIt.Value();
            if (!SoftProp || !ValuePtr)
            {
                continue;
            }
            const FSoftObjectPath SoftPath = SoftProp->GetPropertyValue(ValuePtr).ToSoftObjectPath().GetWithoutSubPath();
            if (SoftPath.IsNull())
            {
                continue;
            }
            if (const int32* FoundIndex = SoftPathLookup.Find(SoftPath.ToString().ToLower()))
            {
                SoftRefsByAsset.FindOrAdd(*FoundIndex).Add(Cls->GetPathName());
            }
        }
    }

    auto ClassPathsJson = [](const TSet<FString>* ClassPaths)
    {
        return ClassPaths ? mcp_common::StringsToJson(ClassPaths->Array()) : TArray<TSharedPtr<FJsonValue>>();
    };
    for (int32 Index = 0; Index < Assets.Num(); ++Index)
    {
        const TSet<FString>* HardSet = HardRefsByAsset.Find(Index);
        const TSet<FString>* SoftSet = SoftRefsByAsset.Find(Index);
        if (!HardSet && !SoftSet)
        {
            continue;
        }
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("asset_path"), Assets[Index] ? Assets[Index]->GetPathName() : FString());
        Entry->SetArrayField(TEXT("hard_referencing_classes"), ClassPathsJson(HardSet));
        Entry->SetArrayField(TEXT("soft_referencing_classes"), ClassPathsJson(SoftSet));
        Out.Add(Entry);
    }

    return Out;
}

/**
 * On-disk verification of ONE completed rename: the new object resolves in the registry, its package file
 * exists, and a probe tells whether the source package was left as a redirector.
 * @return { new_object_path, registry_valid, new_package_name,
 *           new_file_exists, new_file_size_bytes, old_file_exists,
 *           redirector_left, verified }
 *         `verified` == registry_valid && new_file_exists && size > 0.
 */
TSharedPtr<FJsonObject> VerifyRenamedAssetOnDisk(
    const FString& OldPackageName, const FString& NewObjectPath)
{
    TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
    Out->SetStringField(TEXT("new_object_path"), NewObjectPath);

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    const FAssetData NewAssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(NewObjectPath));
    const bool bRegistryValid = NewAssetData.IsValid();
    Out->SetBoolField(TEXT("registry_valid"), bRegistryValid);
    Out->SetStringField(TEXT("new_package_name"), bRegistryValid ? NewAssetData.PackageName.ToString() : FString());

    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

    // The new package on disk. Resolve from the registry's package name when we
    // have it, otherwise from the requested object path -- so a failed rename
    // still reports "no file" instead of silently reporting nothing.
    const FString NewPackageName = bRegistryValid
        ? NewAssetData.PackageName.ToString()
        : FPackageName::ObjectPathToPackageName(NewObjectPath);

    bool  bNewFileExists = false;
    int64 NewFileSize    = 0;
    {
        FString NewBase;
        if (FPackageName::TryConvertLongPackageNameToFilename(NewPackageName, NewBase))
        {
            for (const TCHAR* Ext : { TEXT(".uasset"), TEXT(".umap") })
            {
                const FString Candidate = NewBase + Ext;
                if (PlatformFile.FileExists(*Candidate))
                {
                    bNewFileExists = true;
                    NewFileSize = PlatformFile.FileSize(*Candidate);
                    break;
                }
            }
        }
    }
    Out->SetBoolField(TEXT("new_file_exists"), bNewFileExists);
    Out->SetNumberField(TEXT("new_file_size_bytes"), static_cast<double>(NewFileSize));

    // The source package. Still there == a redirector was left behind (the normal,
    // desired outcome) OR the rename did not happen at all -- distinguished by
    // probing for an actual UObjectRedirector at the source object path.
    bool bOldFileExists = false;
    {
        FString OldBase;
        if (FPackageName::TryConvertLongPackageNameToFilename(OldPackageName, OldBase))
        {
            for (const TCHAR* Ext : { TEXT(".uasset"), TEXT(".umap") })
            {
                if (PlatformFile.FileExists(*(OldBase + Ext))) { bOldFileExists = true; break; }
            }
        }
    }
    Out->SetBoolField(TEXT("old_file_exists"), bOldFileExists);

    bool bRedirectorLeft = false;
    if (bOldFileExists)
    {
        // In-memory probe first (RenameAssets creates the redirector object
        // immediately); registry second, in case it has already been rescanned.
        const FString OldAssetName = GetNameFromAssetPath(OldPackageName);
        const FString OldObjectPath = OldPackageName + TEXT(".") + OldAssetName;
        if (FindObject<UObjectRedirector>(nullptr, *OldObjectPath) != nullptr)
        {
            bRedirectorLeft = true;
        }
        else
        {
            TArray<FAssetData> OldPackageAssets;
            AssetRegistry.GetAssetsByPackageName(FName(*OldPackageName), OldPackageAssets, /*bIncludeOnlyOnDiskAssets=*/true);
            const FTopLevelAssetPath RedirectorClassPath = UObjectRedirector::StaticClass()->GetClassPathName();
            for (const FAssetData& PackageAsset : OldPackageAssets)
            {
                if (PackageAsset.AssetClassPath == RedirectorClassPath) { bRedirectorLeft = true; break; }
            }
        }
    }
    Out->SetBoolField(TEXT("redirector_left"), bRedirectorLeft);

    Out->SetBoolField(TEXT("verified"), bRegistryValid && bNewFileExists && NewFileSize > 0);
    return Out;
}

/**
 * Package names that reference any of `PackageNames` (package-level
 * dependencies, hard + soft), excluding the input packages themselves.
 * Registry-only: nothing is loaded.
 */
TArray<FString> GatherReferencerPackages(const TArray<FString>& PackageNames)
{
    TArray<FString> Out;
    if (PackageNames.Num() == 0)
    {
        return Out;
    }

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    const UE::AssetRegistry::FDependencyQuery DependencyQuery; // hard + soft, like fixup_redirectors

    TSet<FName> SelfPackages;
    for (const FString& PackageName : PackageNames)
    {
        SelfPackages.Add(FName(*PackageName));
    }

    TSet<FName> Referencers;
    for (const FString& PackageName : PackageNames)
    {
        TArray<FName> PackageReferencers;
        AssetRegistry.GetReferencers(FName(*PackageName), PackageReferencers,
            UE::AssetRegistry::EDependencyCategory::Package, DependencyQuery);
        for (const FName& Referencer : PackageReferencers)
        {
            if (!SelfPackages.Contains(Referencer))
            {
                Referencers.Add(Referencer);
            }
        }
    }

    Out.Reserve(Referencers.Num());
    for (const FName& Referencer : Referencers)
    {
        Out.Add(Referencer.ToString());
    }
    return Out;
}

/**
 * Of `PackageNames`, the ones that are RESIDENT in memory and DIRTY,
 * each classified ofpa/world/normal as UEAgentSystemCommands.cpp does.
 * Never loads a package -- loading an OFPA external-actor package is the
 * orphan-resurrection footgun (it can bring a deleted actor back).
 */
TArray<TSharedPtr<FJsonObject>> ClassifyDirtyPackages(const TArray<FString>& PackageNames)
{
    TArray<TSharedPtr<FJsonObject>> Out;
    if (PackageNames.Num() == 0)
    {
        return Out;
    }

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();

    const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();

    for (const FString& PackageNameStr : PackageNames)
    {
        // Only RESIDENT packages can be dirty; loading one to ask is the OFPA
        // orphan-resurrection footgun, so we never do.
        UPackage* ResidentPackage = FindPackage(nullptr, *PackageNameStr);
        if (!ResidentPackage || !ResidentPackage->IsDirty())
        {
            continue;
        }

        FString Kind = TEXT("normal");
        if (PackageNameStr.Contains(TEXT("/__ExternalActors__/")) ||
            PackageNameStr.Contains(TEXT("/__ExternalObjects__/")))
        {
            Kind = TEXT("ofpa");
        }
        else
        {
            TArray<FAssetData> PackageAssets;
            AssetRegistry.GetAssetsByPackageName(FName(*PackageNameStr), PackageAssets, /*bIncludeOnlyOnDiskAssets=*/true);
            for (const FAssetData& PackageAsset : PackageAssets)
            {
                // Exact test from UEAgentSystemCommands.cpp.
                if (!PackageAsset.GetOptionalOuterPathName().IsNone())
                {
                    Kind = TEXT("ofpa");
                    break;
                }
                if (PackageAsset.AssetClassPath == WorldClassPath)
                {
                    Kind = TEXT("world");
                    break;
                }
            }
        }

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("package"), PackageNameStr);
        Entry->SetStringField(TEXT("kind"), Kind);
        Entry->SetBoolField(TEXT("is_dirty"), true);
        Out.Add(Entry);
    }

    return Out;
}

// ---------------------------------------------------------------------------
// 1. rename_assets
// ---------------------------------------------------------------------------
//   (a) per-row results come from disk verification, not from RenameAssets' single
//       batch bool;
//   (b) readonly_roots: renaming a vendored, read-only content root (e.g.
//       /Game/VendorPack) is refused;
//   (c) a PIE guard, like fixup_redirectors, which re-saves the same kind of
//       referencing packages;
//   (d) pre-detection of the CDO-reference modal.
//
// The modal does NOT hang a call arriving through this plugin:
// UUEAgentBridge::DispatchSingleCommand wraps EVERY command in
// TGuardValue<bool>(GIsRunningUnattendedScript, true), and FMessageDialog::Open
// returns its DefaultValue without showing UI when that flag is set
// (MessageDialog.cpp). The engine passes EAppReturnType::Cancel as the
// default (AssetRenameManager.cpp), so the symptom is RenameAssets returning
// FALSE and the whole batch silently not happening -- the pre-detection turns that
// silent no-op into a refusal with its cause. A local TGuardValue is kept too, so
// the handler is safe if ever called off-bridge.
// ---------------------------------------------------------------------------
// MCP:rename_assets - Rename/move multiple assets in one IAssetTools::RenameAssets batch, with per-item disk verification instead of the batch bool. Guards: refuses while PIE is active; refuses paths under readonly_roots (default: the project's Read Only Content Roots setting) on BOTH the source and the destination; pre-detects the native-CDO reference case that makes the engine's FMessageDialog auto-cancel the whole batch (would_block_on_modal) and does NOT call RenameAssets in that case. Also reports referencer packages left DIRTY -- PerformAssetRename discards its FailedPackages list unless UE source control is connected, so a referencer (typically OFPA) can be left unwritten while RenameAssets still returns true.
//   @param assets - List of rename descriptors: { old_path (required), new_name (optional, default = current name), new_folder (optional, default = current folder) }.
//   @param dry_run - Optional bool (default false). Plan + run every guard and the CDO scan, but never call RenameAssets.
//   @param readonly_roots - Optional array of long paths that must not be renamed FROM or INTO (default: the project's Read Only Content Roots setting). Pass [] to disable the guard.
//   @param allow_cdo_referenced - Optional bool (default false). When false, a native-CDO reference to any asset in the batch refuses the batch instead of letting it silently no-op.
//   @returns { dry_run: bool, readonly_roots: [string], readonly_roots_explicit: bool, planned_count: int, renamed_count: int, failed_count: int, skipped_count: int, batch_api_result: bool|null, would_block_on_modal: bool, cdo_references: [object], soft_scan_is_approximate: bool, referencer_packages_dirty: [object], results: [object] }
TSharedPtr<FJsonObject> HandleBulkRenameAssets(
    const TSharedPtr<FJsonObject>& Params)
{
    const TArray<TSharedPtr<FJsonValue>>* AssetsArray = nullptr;
    if (!Params->TryGetArrayField(TEXT("assets"), AssetsArray) || !AssetsArray || AssetsArray->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("assets array is required and must not be empty"));
    }

    if (!FAssetToolsModule::IsModuleLoaded())
    {
        return mcp_common::MakeErrorReply(TEXT("AssetTools module is not loaded"));
    }

    bool bDryRun = false;
    Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

    bool bAllowCdoReferenced = false;
    Params->TryGetBoolField(TEXT("allow_cdo_referenced"), bAllowCdoReferenced);

    // Literal field read so the codegen catalog sees `readonly_roots` as a
    // parameter, AND so the response can say whether the guard is the default or
    // the caller's. The canonical parse itself is mcp_common::ParseReadonlyRoots
    // (default: the project's Read Only Content Roots setting; an explicit [] disables the guard).
    const TArray<TSharedPtr<FJsonValue>>* ReadOnlyRootsField = nullptr;
    const bool bReadOnlyRootsExplicit = Params->TryGetArrayField(TEXT("readonly_roots"), ReadOnlyRootsField);
    const TArray<FString> ReadOnlyRoots = mcp_common::ParseReadonlyRoots(Params);

    const TArray<TSharedPtr<FJsonValue>> ReadOnlyRootsJson = mcp_common::StringsToJson(ReadOnlyRoots);

    // -- guard 1: PIE. Refuse rather than stop PIE for the caller (same policy
    //    as fixup_redirectors): this can re-save an unbounded number of packages.
    if (IsPlayInEditorActive())
    {
        return mcp_common::MakeErrorReply(TEXT(
            "rename_assets refuses to run while Play-In-Editor is active: IAssetTools::RenameAssets "
            "re-saves every referencing package, which is unreliable during PIE. Stop PIE "
            "(stop_play_in_editor) and call again."));
    }

    IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

    // Per-accepted-item bookkeeping, index-aligned with RenameDataArray.
    TArray<FAssetRenameData>        RenameDataArray;
    TArray<UObject*>                AcceptedAssets;
    TArray<FString>                 AcceptedOldPackages;
    TArray<FString>                 AcceptedNewObjectPaths;
    TArray<TSharedPtr<FJsonObject>> AcceptedEntries;

    TArray<TSharedPtr<FJsonObject>> ResultEntries; // every row, accepted or not
    int32 FailedCount  = 0;
    int32 SkippedCount = 0;

    for (const TSharedPtr<FJsonValue>& Entry : *AssetsArray)
    {
        const TSharedPtr<FJsonObject>* EntryObj = nullptr;
        if (!Entry.IsValid() || !Entry->TryGetObject(EntryObj) || !EntryObj || !(*EntryObj).IsValid())
        {
            TSharedPtr<FJsonObject> BadEntry = MakeShared<FJsonObject>();
            BadEntry->SetStringField(TEXT("old_path"), TEXT(""));
            BadEntry->SetStringField(TEXT("new_path"), TEXT(""));
            BadEntry->SetBoolField(TEXT("success"), false);
            BadEntry->SetStringField(TEXT("error"), TEXT("entry is not a JSON object"));
            ResultEntries.Add(BadEntry);
            ++FailedCount;
            continue;
        }

        FString OldPath;
        FString NewName;
        FString NewFolder;
        (*EntryObj)->TryGetStringField(TEXT("old_path"), OldPath);
        (*EntryObj)->TryGetStringField(TEXT("new_name"), NewName);
        (*EntryObj)->TryGetStringField(TEXT("new_folder"), NewFolder);

        TSharedPtr<FJsonObject> ResultEntry = MakeShared<FJsonObject>();
        ResultEntry->SetStringField(TEXT("old_path"), OldPath);

        if (OldPath.IsEmpty())
        {
            ResultEntry->SetBoolField(TEXT("success"), false);
            ResultEntry->SetStringField(TEXT("error"), TEXT("old_path is empty"));
            ResultEntry->SetStringField(TEXT("new_path"), TEXT(""));
            ResultEntries.Add(ResultEntry);
            ++FailedCount;
            continue;
        }

        // Default new_folder = current folder, default new_name = current name.
        // Computed BEFORE the load so the read-only destination check can run
        // even on an asset that fails to load.
        const FString OldPackageName = FPackageName::ObjectPathToPackageName(OldPath);
        if (NewFolder.IsEmpty())
        {
            NewFolder = GetFolderFromAssetPath(OldPackageName);
        }
        while (NewFolder.Len() > 1 && NewFolder.EndsWith(TEXT("/"))) { NewFolder.LeftChopInline(1); }
        if (NewName.IsEmpty())
        {
            NewName = GetNameFromAssetPath(OldPackageName);
        }
        const FString NewPackageName = NewFolder / NewName;
        const FString NewObjectPath  = NewPackageName + TEXT(".") + NewName;
        ResultEntry->SetStringField(TEXT("new_path"), NewPackageName);

        // The load makes a package name of old_path and RenameAssets creates the joined new package and renames the
        // asset's objects after the new object path: a name past the FName limit aborts the editor.
        FString InvalidPath = mcp_common::InvalidPackageName(OldPath);
        if (InvalidPath.IsEmpty())
        {
            InvalidPath = mcp_common::InvalidPackageName(NewObjectPath);
        }
        if (!InvalidPath.IsEmpty())
        {
            ResultEntry->SetBoolField(TEXT("success"), false);
            ResultEntry->SetStringField(TEXT("error"), InvalidPath);
            ResultEntries.Add(ResultEntry);
            ++FailedCount;
            continue;
        }

        // -- guard 2: read-only roots, source AND destination -----------------
        if (mcp_common::IsUnderReadOnlyRoot(OldPath, ReadOnlyRoots))
        {
            ResultEntry->SetBoolField(TEXT("success"), false);
            ResultEntry->SetStringField(TEXT("error"), FString::Printf(
                TEXT("refused: source is under a read-only root (%s). Pass readonly_roots=[] to override."), *OldPath));
            ResultEntry->SetStringField(TEXT("refusal_code"), TEXT("readonly_root_source"));
            ResultEntries.Add(ResultEntry);
            ++SkippedCount;
            continue;
        }
        if (mcp_common::IsUnderReadOnlyRoot(NewPackageName, ReadOnlyRoots))
        {
            ResultEntry->SetBoolField(TEXT("success"), false);
            ResultEntry->SetStringField(TEXT("error"), FString::Printf(
                TEXT("refused: destination is under a read-only root (%s). Pass readonly_roots=[] to override."), *NewPackageName));
            ResultEntry->SetStringField(TEXT("refusal_code"), TEXT("readonly_root_destination"));
            ResultEntries.Add(ResultEntry);
            ++SkippedCount;
            continue;
        }

        UObject* AssetObj = LoadObject<UObject>(nullptr, *OldPath);
        if (!AssetObj)
        {
            ResultEntry->SetBoolField(TEXT("success"), false);
            ResultEntry->SetStringField(TEXT("error"), FString::Printf(TEXT("Cannot load asset: %s"), *OldPath));
            ResultEntries.Add(ResultEntry);
            ++FailedCount;
            continue;
        }
        // The rename carries the asset's objects under the new object path: its deepest object, measured now that it
        // is loaded, must stay under the FName limit there.
        if (const FString TooLong = AssetCommandHelpers::CopiedObjectPathsTooLong(AssetObj, NewObjectPath); !TooLong.IsEmpty())
        {
            ResultEntry->SetBoolField(TEXT("success"), false);
            ResultEntry->SetStringField(TEXT("error"), TooLong);
            ResultEntries.Add(ResultEntry);
            ++FailedCount;
            continue;
        }

        RenameDataArray.Emplace(MakeWeakObjectPtr(AssetObj), NewFolder, NewName);
        AcceptedAssets.Add(AssetObj);
        AcceptedOldPackages.Add(AssetObj->GetPackage() ? AssetObj->GetPackage()->GetName() : OldPackageName);
        AcceptedNewObjectPaths.Add(NewObjectPath);
        AcceptedEntries.Add(ResultEntry);
        ResultEntries.Add(ResultEntry);
    }

    // -- guard 3: native-CDO references -> the engine's FMessageDialog --------
    const TArray<TSharedPtr<FJsonObject>> CdoRefs = FindCdoReferencesToAssets(AcceptedAssets);
    TArray<TSharedPtr<FJsonValue>> CdoRefsJson;
    for (const TSharedPtr<FJsonObject>& Ref : CdoRefs) { CdoRefsJson.Add(MakeShared<FJsonValueObject>(Ref)); }
    const bool bWouldBlockOnModal = CdoRefs.Num() > 0;

    auto BuildData = [&](bool bBatchAttempted, bool bBatchResult, int32 RenamedCount,
                         const TArray<TSharedPtr<FJsonObject>>& DirtyReferencers) -> TSharedPtr<FJsonObject>
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("dry_run"), bDryRun);
        Data->SetArrayField(TEXT("readonly_roots"), ReadOnlyRootsJson);
        Data->SetBoolField(TEXT("readonly_roots_explicit"), bReadOnlyRootsExplicit);
        Data->SetNumberField(TEXT("planned_count"), AcceptedEntries.Num());
        Data->SetNumberField(TEXT("renamed_count"), RenamedCount);
        Data->SetNumberField(TEXT("failed_count"), FailedCount);
        Data->SetNumberField(TEXT("skipped_count"), SkippedCount);
        if (bBatchAttempted)
        {
            Data->SetBoolField(TEXT("batch_api_result"), bBatchResult);
        }
        else
        {
            // Honest: RenameAssets was never called, so there IS no batch bool.
            Data->SetField(TEXT("batch_api_result"), MakeShared<FJsonValueNull>());
        }
        Data->SetBoolField(TEXT("would_block_on_modal"), bWouldBlockOnModal);
        Data->SetArrayField(TEXT("cdo_references"), CdoRefsJson);
        Data->SetBoolField(TEXT("soft_scan_is_approximate"), true);

        TArray<TSharedPtr<FJsonValue>> DirtyJson;
        for (const TSharedPtr<FJsonObject>& Obj : DirtyReferencers) { DirtyJson.Add(MakeShared<FJsonValueObject>(Obj)); }
        Data->SetArrayField(TEXT("referencer_packages_dirty"), DirtyJson);

        TArray<TSharedPtr<FJsonValue>> ResultValues;
        ResultValues.Reserve(ResultEntries.Num());
        for (const TSharedPtr<FJsonObject>& Obj : ResultEntries) { ResultValues.Add(MakeShared<FJsonValueObject>(Obj)); }
        Data->SetArrayField(TEXT("results"), ResultValues);
        return Data;
    };

    if (bWouldBlockOnModal && !bAllowCdoReferenced)
    {
        for (const TSharedPtr<FJsonObject>& AcceptedEntry : AcceptedEntries)
        {
            AcceptedEntry->SetBoolField(TEXT("success"), false);
            AcceptedEntry->SetStringField(TEXT("error"), TEXT(
                "not attempted: a native CDO references an asset in this batch, so "
                "IAssetTools::RenameAssets would hit FAssetRenameManager's FMessageDialog, which is "
                "auto-answered CANCEL under the MCP bridge -> the whole batch would silently no-op. "
                "Pass allow_cdo_referenced=true to attempt it anyway."));
            AcceptedEntry->SetStringField(TEXT("refusal_code"), TEXT("cdo_referenced"));
        }
        const int32 Refused = AcceptedEntries.Num();
        SkippedCount += Refused;
        TSharedPtr<FJsonObject> Data = BuildData(/*bBatchAttempted=*/false, /*bBatchResult=*/false, /*RenamedCount=*/0, {});
        // dry_run asked for a report and got one -> that is a success. A real
        // call asked for renames and performed none -> that is not.
        return mcp_common::MakeOutcomeReply(Data, /*bSuccess=*/bDryRun,
            bDryRun ? FString() : FString::Printf(
                TEXT("refused: %d asset(s) in this batch are referenced by a native CDO (see cdo_references)."), Refused));
    }

    if (bDryRun)
    {
        for (const TSharedPtr<FJsonObject>& AcceptedEntry : AcceptedEntries)
        {
            AcceptedEntry->SetBoolField(TEXT("success"), false);
            AcceptedEntry->SetBoolField(TEXT("would_rename"), true);
        }
        TSharedPtr<FJsonObject> Data = BuildData(/*bBatchAttempted=*/false, /*bBatchResult=*/false, /*RenamedCount=*/0, {});
        return mcp_common::MakeOutcomeReply(Data, /*bSuccess=*/true);
    }

    if (RenameDataArray.Num() == 0)
    {
        TSharedPtr<FJsonObject> Data = BuildData(/*bBatchAttempted=*/false, /*bBatchResult=*/false, /*RenamedCount=*/0, {});
        return mcp_common::MakeOutcomeReply(Data, /*bSuccess=*/false,
            TEXT("no rename was attempted: every entry was rejected or refused (see results[])."));
    }

    // Snapshot the referencer set BEFORE the rename: afterwards those edges are
    // repointed at the NEW package names, so asking the registry again would
    // under-report exactly the packages we care about. Also record which of them
    // were ALREADY dirty, so a pre-existing dirty package is not blamed on us.
    const TArray<FString> ReferencerCandidates = GatherReferencerPackages(AcceptedOldPackages);
    TSet<FString> DirtyBeforeSet;
    for (const TSharedPtr<FJsonObject>& Obj : ClassifyDirtyPackages(ReferencerCandidates))
    {
        FString PackageName;
        if (Obj->TryGetStringField(TEXT("package"), PackageName)) { DirtyBeforeSet.Add(PackageName); }
    }

    bool bBatchResult = false;
    {
        // Redundant with UEAgentBridge.cpp (which already sets this
        // for every dispatched command) -- kept so the handler cannot hang the
        // game thread on a modal if it is ever driven from somewhere else.
        TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
        bBatchResult = AssetTools.RenameAssets(RenameDataArray);
    }

    // -- per-item verification: the disk, not the batch bool ------------------
    int32 RenamedCount = 0;
    for (int32 Index = 0; Index < AcceptedEntries.Num(); ++Index)
    {
        TSharedPtr<FJsonObject> VerifyObj = VerifyRenamedAssetOnDisk(AcceptedOldPackages[Index], AcceptedNewObjectPaths[Index]);
        const bool bVerified = VerifyObj->GetBoolField(TEXT("verified"));

        AcceptedEntries[Index]->SetObjectField(TEXT("verification"), VerifyObj);
        AcceptedEntries[Index]->SetBoolField(TEXT("success"), bVerified);
        AcceptedEntries[Index]->SetBoolField(TEXT("redirector_left"), VerifyObj->GetBoolField(TEXT("redirector_left")));
        if (bVerified)
        {
            ++RenamedCount;
        }
        else
        {
            AcceptedEntries[Index]->SetStringField(TEXT("error"), FString::Printf(
                TEXT("rename NOT verified on disk (registry_valid=%s, new_file_exists=%s). "
                     "IAssetTools::RenameAssets returned %s for the batch."),
                VerifyObj->GetBoolField(TEXT("registry_valid")) ? TEXT("true") : TEXT("false"),
                VerifyObj->GetBoolField(TEXT("new_file_exists")) ? TEXT("true") : TEXT("false"),
                bBatchResult ? TEXT("true") : TEXT("false")));
            ++FailedCount;
        }
    }

    // -- the silent one: referencers left dirty (unwritten) ------------------
    TArray<TSharedPtr<FJsonObject>> DirtyAfter = ClassifyDirtyPackages(ReferencerCandidates);
    for (const TSharedPtr<FJsonObject>& Obj : DirtyAfter)
    {
        FString PackageName;
        const bool bWasDirtyBefore = Obj->TryGetStringField(TEXT("package"), PackageName)
            && DirtyBeforeSet.Contains(PackageName);
        // Distinguishes "this rename left it unwritten" from "it was already
        // dirty before we touched anything" -- without this the tool would
        // blame the rename for the caller's own unsaved edits.
        Obj->SetBoolField(TEXT("was_dirty_before_rename"), bWasDirtyBefore);
    }

    TSharedPtr<FJsonObject> Data = BuildData(/*bBatchAttempted=*/true, bBatchResult, RenamedCount, DirtyAfter);

    const bool bOverallOk = (RenamedCount == AcceptedEntries.Num()) && FailedCount == 0 && SkippedCount == 0;
    FString ErrorMessage;
    if (!bOverallOk)
    {
        ErrorMessage = FString::Printf(
            TEXT("%d/%d renames verified on disk (%d failed, %d refused). RenameAssets returned %s -- that bool "
                 "is the BATCH result and is not evidence for any individual row."),
            RenamedCount, AcceptedEntries.Num(), FailedCount, SkippedCount,
            bBatchResult ? TEXT("true") : TEXT("false"));
    }
    return mcp_common::MakeOutcomeReply(Data, bOverallOk, ErrorMessage);
}

// ==========================================================================
// Cross-project / migrate / diff utilities.
// ==========================================================================
namespace
{
    // ---- Lightweight BINARY .uasset ref scan (no editor load) ------
    // Tokenize the raw package bytes (what `grep -aoE` does by hand) and count
    // reference-path tokens (/Game//Engine//Script) + optional-pattern matches.
    // An incomplete copy shows far fewer ref tokens than the full one.
    struct FUAssetRefScan_AssetUtil
    {
        bool      bExists = false;
        int64     FileSize = 0;
        FDateTime MTime = FDateTime(0);
        int32     RefPathUnique = 0;        // distinct /Game|/Engine|/Script tokens
        int64     RefPathOccurrences = 0;   // total occurrences of those tokens
        int32     MatchUnique = 0;          // distinct tokens matching `pattern`
        int64     MatchOccurrences = 0;     // total occurrences matching `pattern`
        TArray<TPair<FString, int32>> TopRefPaths;
        TArray<TPair<FString, int32>> TopMatches;
    };

    FORCEINLINE bool IsRefTokenChar_AssetUtil(uint8 C)
    {
        return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
               (C >= '0' && C <= '9') || C == '_' || C == '/' || C == '.';
    }

    void ScanBytesForRefs_AssetUtil(const TArray<uint8>& Bytes, const FString& Pattern, FUAssetRefScan_AssetUtil& Out)
    {
        TMap<FString, int32> RefPathCounts;
        TMap<FString, int32> MatchCounts;

        const bool bHasPattern = !Pattern.IsEmpty();
        // FRegexPattern never throws; an invalid pattern simply never matches.
        FRegexPattern Rx(bHasPattern ? Pattern : FString(TEXT("$a^")));

        FString Cur;
        bool bHasLetter = false;
        auto Flush = [&]()
        {
            if (Cur.Len() >= 3 && bHasLetter)
            {
                if (Cur.Contains(TEXT("/Game/")) || Cur.Contains(TEXT("/Engine/")) || Cur.Contains(TEXT("/Script/")))
                {
                    RefPathCounts.FindOrAdd(Cur)++;
                    Out.RefPathOccurrences++;
                }
                if (bHasPattern)
                {
                    FRegexMatcher M(Rx, Cur);
                    if (M.FindNext())
                    {
                        MatchCounts.FindOrAdd(Cur)++;
                        Out.MatchOccurrences++;
                    }
                }
            }
            Cur.Reset();
            bHasLetter = false;
        };

        for (uint8 C : Bytes)
        {
            if (IsRefTokenChar_AssetUtil(C))
            {
                Cur.AppendChar(static_cast<TCHAR>(C));
                if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z')) { bHasLetter = true; }
                if (Cur.Len() > 512) { Flush(); } // guard against pathological runs
            }
            else
            {
                Flush();
            }
        }
        Flush();

        Out.RefPathUnique = RefPathCounts.Num();
        Out.MatchUnique = MatchCounts.Num();

        auto TopN = [](const TMap<FString, int32>& In, TArray<TPair<FString, int32>>& OutArr, int32 N)
        {
            OutArr.Reserve(In.Num());
            for (const TPair<FString, int32>& KV : In) { OutArr.Add(KV); }
            OutArr.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
            if (OutArr.Num() > N) { OutArr.SetNum(N); }
        };
        TopN(RefPathCounts, Out.TopRefPaths, 200);
        TopN(MatchCounts, Out.TopMatches, 200);
    }

    bool ScanFileForRefs_AssetUtil(const FString& FilePath, const FString& Pattern, FUAssetRefScan_AssetUtil& Out)
    {
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        if (!PF.FileExists(*FilePath)) { Out.bExists = false; return false; }
        Out.bExists = true;
        Out.FileSize = PF.FileSize(*FilePath);
        Out.MTime = PF.GetTimeStamp(*FilePath);
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *FilePath)) { return false; }
        ScanBytesForRefs_AssetUtil(Bytes, Pattern, Out);
        return true;
    }

    // Serialize a scan's ref-path/match token lists to a JSON array of {token, count}.
    TArray<TSharedPtr<FJsonValue>> TokenListToJson_AssetUtil(const TArray<TPair<FString, int32>>& In)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const TPair<FString, int32>& KV : In)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("token"), KV.Key);
            Entry->SetNumberField(TEXT("count"), KV.Value);
            Arr.Add(MakeShared<FJsonValueObject>(Entry));
        }
        return Arr;
    }

    // ---- Locate the "Content" ancestor of an external source dir ---
    // The relative path is preserved RELATIVE to this Content root (so a file at
    // <Src>/Content/Characters/Foo.uasset ingests as <dest_root>/Characters/Foo).
    // If no "Content" component exists, the source_dir itself is the root.
    FString FindContentRootDir_AssetUtil(const FString& AbsDir)
    {
        FString Norm = AbsDir;
        Norm.ReplaceInline(TEXT("\\"), TEXT("/"));
        Norm.RemoveFromEnd(TEXT("/"));
        TArray<FString> Parts;
        Norm.ParseIntoArray(Parts, TEXT("/"), /*InCullEmpty=*/true);
        int32 Idx = INDEX_NONE;
        for (int32 i = Parts.Num() - 1; i >= 0; --i)
        {
            if (Parts[i].Equals(TEXT("Content"), ESearchCase::IgnoreCase)) { Idx = i; break; }
        }
        if (Idx == INDEX_NONE) { return Norm; }
        FString Root;
        for (int32 i = 0; i <= Idx; ++i)
        {
            if (i > 0) { Root += TEXT("/"); }
            Root += Parts[i];
        }
        return Root;
    }

    // Resolve a scan target that may be a disk path OR a /Game|/Engine mount path
    // to an on-disk .uasset/.umap filename.
    FString ResolveScanFilePath_AssetUtil(const FString& In)
    {
        if (In.StartsWith(TEXT("/Game")) || In.StartsWith(TEXT("/Engine")))
        {
            FString Base;
            if (FPackageName::TryConvertLongPackageNameToFilename(FPackageName::ObjectPathToPackageName(In), Base))
            {
                for (const TCHAR* Ext : { TEXT(".uasset"), TEXT(".umap") })
                {
                    const FString Candidate = Base + Ext;
                    if (FPaths::FileExists(Candidate)) { return Candidate; }
                }
            }
        }
        return In;
    }

    // Resolve tree_a/tree_b (disk dir OR /Game mount) to an absolute on-disk dir.
    bool ResolveTreeDir_AssetUtil(const FString& In, FString& OutDir)
    {
        if (In.StartsWith(TEXT("/Game")) || In.StartsWith(TEXT("/Engine")))
        {
            if (!FPackageName::TryConvertLongPackageNameToFilename(In, OutDir)) { return false; }
        }
        else
        {
            OutDir = In;
        }
        OutDir = FPaths::ConvertRelativePathToFull(OutDir);
        OutDir.ReplaceInline(TEXT("\\"), TEXT("/"));
        OutDir.RemoveFromEnd(TEXT("/"));
        return !OutDir.IsEmpty();
    }

    // Enumerate every .uasset/.umap under RootDir -> map of lowercased forward-slash
    // rel-path -> { original rel-path (for display), absolute path }.
    struct FTreeEntry_AssetUtil { FString Rel; FString Abs; };
    void EnumerateTree_AssetUtil(const FString& RootDir, TMap<FString, FTreeEntry_AssetUtil>& Out, int32 MaxFiles)
    {
        TArray<FString> Files;
        IFileManager::Get().FindFilesRecursive(Files, *RootDir, TEXT("*.uasset"), /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/true);
        IFileManager::Get().FindFilesRecursive(Files, *RootDir, TEXT("*.umap"),   /*Files=*/true, /*Directories=*/false, /*bClearFileNames=*/false);
        const int32 Prefix = RootDir.Len() + 1;
        for (FString& F : Files)
        {
            if (Out.Num() >= MaxFiles) { break; }
            FString Full = F;
            Full.ReplaceInline(TEXT("\\"), TEXT("/"));
            if (Full.Len() <= Prefix || !Full.StartsWith(RootDir, ESearchCase::IgnoreCase)) { continue; }
            FString Rel = Full.RightChop(Prefix);
            FString Key = Rel.ToLower();
            Out.Add(Key, FTreeEntry_AssetUtil{ Rel, Full });
        }
    }
}

// Cross-project / migrate / diff commands. They operate on the filesystem + AssetRegistry graph and never
// load external editors.

// -------------------------------------------------------------------------- //
// ingest_external_uassets
// -------------------------------------------------------------------------- //
// MCP:ingest_external_uassets - Copy .uasset/.uexp/.ubulk/.umap from an EXTERNAL Content tree into THIS project, preserving each file's path relative to its Content root, then register the new packages with IAssetRegistry::ScanPathsSynchronous(bForceRescan) and sweep object redirectors under the destination. Fills the gap that migrate_assets only EXPORTS and import_texture_file is image-only. HEAVY: touches project Content on disk + mutates the registry -- run with confirmation.
//   @param source_dir - External source directory (its Content ancestor defines the preserved relative path; if none, source_dir itself is the root).
//   @param dest_root - Destination content mount path (optional, default '/Game'); e.g. '/Game/MyGame/Imported'.
//   @param paths - Optional list of relative sub-paths (relative to the Content root) to restrict the copy to; empty/absent = whole tree.
//   @param overwrite - Optional bool (default false). When false, destination files that already exist are skipped.
//   @param fixup_redirectors - Optional bool (default true). Resolve + leave-fixed object redirectors under dest_root after the copy.
//   @returns { source_dir: string, content_root: string, dest_root: string, dest_dir: string, files_copied: int, files_skipped: int, files_failed: int, scanned_paths: [string], redirectors_fixed: int, copied: [{src, dest, package_path}], message: string }
TSharedPtr<FJsonObject> HandleIngestExternalUAssets(const TSharedPtr<FJsonObject>& Params)
{
    FString SourceDir;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_dir"), SourceDir)) { return Missing; }

    FString DestRoot = TEXT("/Game");
    Params->TryGetStringField(TEXT("dest_root"), DestRoot);
    DestRoot.RemoveFromEnd(TEXT("/"));
    if (!DestRoot.StartsWith(TEXT("/")))
    {
        return mcp_common::MakeErrorReply(TEXT("dest_root must be a content mount path (e.g. /Game/...)"));
    }

    bool bOverwrite = false;
    Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);
    bool bFixup = true;
    Params->TryGetBoolField(TEXT("fixup_redirectors"), bFixup);

    // Optional relative sub-path filter (each match by prefix, forward-slash normalized).
    TArray<FString> PathFilters;
    const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *PathsArr)
        {
            FString S;
            if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
            {
                S.ReplaceInline(TEXT("\\"), TEXT("/"));
                S.RemoveFromStart(TEXT("/"));
                PathFilters.Add(S);
            }
        }
    }

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    FString SourceFull = FPaths::ConvertRelativePathToFull(SourceDir);
    SourceFull.ReplaceInline(TEXT("\\"), TEXT("/"));
    SourceFull.RemoveFromEnd(TEXT("/"));
    if (!PF.DirectoryExists(*SourceFull))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("source_dir does not exist: %s"), *SourceFull));
    }

    const FString ContentRoot = FindContentRootDir_AssetUtil(SourceFull);

    // Destination base directory on disk from the mount path.
    FString DestBaseDir;
    if (!FPackageName::TryConvertLongPackageNameToFilename(DestRoot, DestBaseDir))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("dest_root is not a mounted content path: %s"), *DestRoot));
    }
    DestBaseDir.ReplaceInline(TEXT("\\"), TEXT("/"));
    DestBaseDir.RemoveFromEnd(TEXT("/"));

    // Enumerate the external package files (and their on-disk companions).
    TArray<FString> SrcFiles;
    IFileManager::Get().FindFilesRecursive(SrcFiles, *SourceFull, TEXT("*.uasset"), true, false, /*bClearFileNames=*/true);
    IFileManager::Get().FindFilesRecursive(SrcFiles, *SourceFull, TEXT("*.uexp"),   true, false, /*bClearFileNames=*/false);
    IFileManager::Get().FindFilesRecursive(SrcFiles, *SourceFull, TEXT("*.ubulk"),  true, false, /*bClearFileNames=*/false);
    IFileManager::Get().FindFilesRecursive(SrcFiles, *SourceFull, TEXT("*.umap"),   true, false, /*bClearFileNames=*/false);

    const int32 ContentRootLen = ContentRoot.Len();
    int32 Copied = 0, Skipped = 0, Failed = 0;
    TSet<FString> ScanPackageDirs;   // long package PATHS (folders) to rescan
    TArray<TSharedPtr<FJsonValue>> CopiedArr;

    // Whether SrcFile is one to ingest; OutSrcNorm is it with forward slashes and OutRel its path relative to the
    // Content root, e.g. "Characters/Foo/SK.uasset".
    const auto ToIngest = [&ContentRoot, ContentRootLen, &PathFilters](const FString& SrcFile, FString& OutSrcNorm, FString& OutRel)
    {
        OutSrcNorm = SrcFile;
        OutSrcNorm.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (!OutSrcNorm.StartsWith(ContentRoot)) { return false; }

        OutRel = OutSrcNorm.RightChop(ContentRootLen);
        OutRel.RemoveFromStart(TEXT("/"));
        if (OutRel.IsEmpty()) { return false; }

        if (PathFilters.Num() > 0)
        {
            bool bMatch = false;
            for (const FString& Filter : PathFilters)
            {
                if (OutRel.StartsWith(Filter, ESearchCase::IgnoreCase)) { bMatch = true; break; }
            }
            if (!bMatch) { return false; }
        }
        return true;
    };

    // Each copy becomes the package dest_root/<relative path>, and the registry scan below makes an FName of it: one
    // past the FName limit would abort the editor there, and again at every start while the file stays. The whole
    // set is checked before anything is copied.
    for (const FString& SrcFile : SrcFiles)
    {
        FString SrcNorm, Rel;
        if (ToIngest(SrcFile, SrcNorm, Rel))
        {
            if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(DestRoot / FPaths::GetBaseFilename(Rel, /*bRemovePath=*/false)))
            {
                return Invalid;
            }
        }
    }

    for (FString& SrcFile : SrcFiles)
    {
        FString SrcNorm, Rel;
        if (!ToIngest(SrcFile, SrcNorm, Rel)) { continue; }

        const FString DestFile = DestBaseDir / Rel;
        if (!bOverwrite && PF.FileExists(*DestFile)) { ++Skipped; continue; }

        PF.CreateDirectoryTree(*FPaths::GetPath(DestFile));
        const uint32 CopyResult = IFileManager::Get().Copy(*DestFile, *SrcFile, /*Replace=*/bOverwrite, /*EvenIfReadOnly=*/true, /*Attributes=*/false);
        if (CopyResult != COPY_OK)
        {
            ++Failed;
            continue;
        }
        ++Copied;

        // Record + collect the destination package folder (mount path) for the rescan.
        const FString RelDir = FPaths::GetPath(Rel);   // "Characters/Foo"
        FString DestPkgFolder = DestRoot;
        if (!RelDir.IsEmpty()) { DestPkgFolder = DestRoot + TEXT("/") + RelDir; }
        ScanPackageDirs.Add(DestPkgFolder);

        if (CopiedArr.Num() < 500)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("src"), SrcNorm);
            Entry->SetStringField(TEXT("dest"), DestFile);
            // Package path for the asset (drop extension) so callers can address it as /Game/...
            FString RelNoExt = Rel;
            const FString Ext = FPaths::GetExtension(Rel, /*bIncludeDot=*/true);
            if (!Ext.IsEmpty()) { RelNoExt.RemoveFromEnd(Ext); }
            Entry->SetStringField(TEXT("package_path"), DestRoot + TEXT("/") + RelNoExt);
            CopiedArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    // Register the freshly-copied packages with the Asset Registry.
    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();
    TArray<FString> ScanPaths = ScanPackageDirs.Array();
    if (ScanPaths.Num() > 0)
    {
        AssetRegistry.ScanPathsSynchronous(ScanPaths, /*bForceRescan=*/true);
    }

    // Redirector sweep under dest_root (best-effort; matches fixup_redirectors' unattended path).
    int32 RedirectorsFixed = 0;
    if (bFixup && Copied > 0)
    {
        FARFilter Filter;
        Filter.PackagePaths.Add(FName(*DestRoot));
        Filter.bRecursivePaths = true;
        Filter.ClassPaths.Add(UObjectRedirector::StaticClass()->GetClassPathName());
        Filter.bRecursiveClasses = true;

        TArray<FAssetData> RedirectorAssets;
        AssetRegistry.GetAssets(Filter, RedirectorAssets);

        TArray<UObjectRedirector*> Redirectors;
        for (const FAssetData& AD : RedirectorAssets)
        {
            if (UObjectRedirector* R = Cast<UObjectRedirector>(AD.GetAsset())) { Redirectors.Add(R); }
        }
        RedirectorsFixed = Redirectors.Num();
        if (Redirectors.Num() > 0 && FAssetToolsModule::IsModuleLoaded())
        {
            IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
            AssetTools.FixupReferencers(Redirectors, /*bCheckoutDialogPrompt=*/false, ERedirectFixupMode::LeaveFixedUpRedirectors);
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source_dir"), SourceFull);
    Result->SetStringField(TEXT("content_root"), ContentRoot);
    Result->SetStringField(TEXT("dest_root"), DestRoot);
    Result->SetStringField(TEXT("dest_dir"), DestBaseDir);
    Result->SetNumberField(TEXT("files_copied"), Copied);
    Result->SetNumberField(TEXT("files_skipped"), Skipped);
    Result->SetNumberField(TEXT("files_failed"), Failed);
    Result->SetNumberField(TEXT("redirectors_fixed"), RedirectorsFixed);
    Result->SetArrayField(TEXT("scanned_paths"), mcp_common::StringsToJson(ScanPaths));
    Result->SetArrayField(TEXT("copied"), CopiedArr);
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("Ingested %d file(s) (%d skipped, %d failed) from %s into %s; rescanned %d path(s)."),
        Copied, Skipped, Failed, *ContentRoot, *DestRoot, ScanPaths.Num()));
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// scan_uasset_references
// -------------------------------------------------------------------------- //
// MCP:scan_uasset_references - Lightweight BINARY scan of a .uasset/.umap on disk (NO editor load): tokenizes the raw package bytes and reports the referenced-asset-path tokens (/Game//Engine//Script) plus, optionally, tokens matching a regex `pattern`. A quick way to compare copies of one asset: an incomplete (stub) copy shows far fewer reference tokens than the complete one. Accepts a disk path OR a /Game|/Engine mount path.
//   @param package_file - Absolute .uasset/.umap path on disk, or a /Game|/Engine mount path.
//   @param pattern - Optional regex (ECMAScript); tokens containing a match are counted in the 'matches' fields.
//   @returns { package_file: string, resolved_path: string, exists: bool, file_size_bytes: number, modified: string, pattern: string, ref_path_unique: int, ref_path_occurrences: number, ref_paths: [{token, count}], match_unique: int, match_occurrences: number, matches: [{token, count}] }
TSharedPtr<FJsonObject> HandleScanUAssetRefs(const TSharedPtr<FJsonObject>& Params)
{
    FString PackageFile;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_file"), PackageFile)) { return Missing; }
    FString Pattern;
    Params->TryGetStringField(TEXT("pattern"), Pattern);

    const FString Resolved = ResolveScanFilePath_AssetUtil(PackageFile);
    FUAssetRefScan_AssetUtil Scan;
    const bool bScanned = ScanFileForRefs_AssetUtil(Resolved, Pattern, Scan);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("package_file"), PackageFile);
    Result->SetStringField(TEXT("resolved_path"), Resolved);
    Result->SetBoolField(TEXT("exists"), Scan.bExists);
    Result->SetStringField(TEXT("pattern"), Pattern);
    if (!Scan.bExists)
    {
        Result->SetStringField(TEXT("message"), FString::Printf(TEXT("Package file not found: %s"), *Resolved));
        return mcp_common::MakeOkReply(Result);
    }
    if (!bScanned)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to read package bytes: %s"), *Resolved));
    }

    Result->SetNumberField(TEXT("file_size_bytes"), static_cast<double>(Scan.FileSize));
    Result->SetStringField(TEXT("modified"), Scan.MTime.ToString());
    Result->SetNumberField(TEXT("ref_path_unique"), Scan.RefPathUnique);
    Result->SetNumberField(TEXT("ref_path_occurrences"), static_cast<double>(Scan.RefPathOccurrences));
    Result->SetArrayField(TEXT("ref_paths"), TokenListToJson_AssetUtil(Scan.TopRefPaths));
    Result->SetNumberField(TEXT("match_unique"), Scan.MatchUnique);
    Result->SetNumberField(TEXT("match_occurrences"), static_cast<double>(Scan.MatchOccurrences));
    Result->SetArrayField(TEXT("matches"), TokenListToJson_AssetUtil(Scan.TopMatches));
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// compare_asset_versions
// -------------------------------------------------------------------------- //
// MCP:compare_asset_versions - Apply the binary ref-scan of scan_uasset_references to the SAME asset (rel_path) across N external Content roots, reporting ref-count / size / mtime (and optional pattern-match count) per root so you can tell an incomplete (stub) copy from the complete one at a glance.
//   @param rel_path - Asset path relative to each root's Content dir, e.g. 'Characters/Mannequin/ABP_Manny.uasset' (a missing extension defaults to .uasset).
//   @param roots - List of external Content-root directories to compare across.
//   @param pattern - Optional regex (ECMAScript) forwarded to the scan; when set, roots rank by pattern-match count instead of ref-path count.
//   @returns { rel_path: string, pattern: string, roots: [{root, full_path, exists, size_bytes, modified, ref_path_unique, ref_path_occurrences, match_unique, match_occurrences}], most_complete_root: string, likely_stub_root: string, message: string }
TSharedPtr<FJsonObject> HandleCompareAssetVersions(const TSharedPtr<FJsonObject>& Params)
{
    FString RelPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("rel_path"), RelPath)) { return Missing; }
    const TArray<TSharedPtr<FJsonValue>>* RootsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("roots"), RootsArr) || !RootsArr || RootsArr->Num() == 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: roots (non-empty array of Content-root dirs)"));
    }
    FString Pattern;
    Params->TryGetStringField(TEXT("pattern"), Pattern);
    const bool bRankByPattern = !Pattern.IsEmpty();

    FString RelNorm = RelPath;
    RelNorm.ReplaceInline(TEXT("\\"), TEXT("/"));
    RelNorm.RemoveFromStart(TEXT("/"));
    if (FPaths::GetExtension(RelNorm).IsEmpty()) { RelNorm += TEXT(".uasset"); }

    TArray<TSharedPtr<FJsonValue>> RootsJson;
    FString MostCompleteRoot;
    FString LikelyStubRoot;
    int64 BestScore = -1;
    int64 WorstScore = -1;

    for (const TSharedPtr<FJsonValue>& V : *RootsArr)
    {
        FString RootDir;
        if (!V.IsValid() || !V->TryGetString(RootDir) || RootDir.IsEmpty()) { continue; }
        FString RootNorm = FPaths::ConvertRelativePathToFull(RootDir);
        RootNorm.ReplaceInline(TEXT("\\"), TEXT("/"));
        RootNorm.RemoveFromEnd(TEXT("/"));
        const FString FullPath = RootNorm / RelNorm;

        FUAssetRefScan_AssetUtil Scan;
        ScanFileForRefs_AssetUtil(FullPath, Pattern, Scan);

        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("root"), RootNorm);
        Entry->SetStringField(TEXT("full_path"), FullPath);
        Entry->SetBoolField(TEXT("exists"), Scan.bExists);
        Entry->SetNumberField(TEXT("size_bytes"), static_cast<double>(Scan.FileSize));
        Entry->SetStringField(TEXT("modified"), Scan.bExists ? Scan.MTime.ToString() : FString());
        Entry->SetNumberField(TEXT("ref_path_unique"), Scan.RefPathUnique);
        Entry->SetNumberField(TEXT("ref_path_occurrences"), static_cast<double>(Scan.RefPathOccurrences));
        Entry->SetNumberField(TEXT("match_unique"), Scan.MatchUnique);
        Entry->SetNumberField(TEXT("match_occurrences"), static_cast<double>(Scan.MatchOccurrences));
        RootsJson.Add(MakeShared<FJsonValueObject>(Entry));

        if (Scan.bExists)
        {
            const int64 Score = bRankByPattern ? Scan.MatchOccurrences : static_cast<int64>(Scan.RefPathUnique);
            if (BestScore < 0 || Score > BestScore) { BestScore = Score; MostCompleteRoot = RootNorm; }
            if (WorstScore < 0 || Score < WorstScore) { WorstScore = Score; LikelyStubRoot = RootNorm; }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("rel_path"), RelNorm);
    Result->SetStringField(TEXT("pattern"), Pattern);
    Result->SetArrayField(TEXT("roots"), RootsJson);
    Result->SetStringField(TEXT("most_complete_root"), MostCompleteRoot);
    Result->SetStringField(TEXT("likely_stub_root"), LikelyStubRoot);
    Result->SetStringField(TEXT("message"), MostCompleteRoot.IsEmpty()
        ? TEXT("Asset not found under any provided root.")
        : FString::Printf(TEXT("Most complete: %s (score %lld); likely stub: %s (score %lld)."),
            *MostCompleteRoot, BestScore, *LikelyStubRoot, WorstScore));
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// list_assets_modified_since
// -------------------------------------------------------------------------- //
// MCP:list_assets_modified_since - List package files (.uasset/.umap) under a content path whose ON-DISK mtime is newer than a timestamp -- the `find -newermt` recipe for isolating an UE Migrate set or a session's changes, but returning /Game package names. Enumerates the filesystem (so it catches just-migrated assets the registry may not have indexed yet).
//   @param timestamp - Cutoff time; ISO-8601 (e.g. '2024-07-15T00:00:00Z') or an FDateTime-parseable string. Assets with mtime strictly greater are returned.
//   @param path - Content path to scan (optional, default '/Game'), e.g. '/Game/MyGame'.
//   @param limit - Max entries to return, newest first (optional, default 500; clamped 1..5000).
//   @returns { path: string, dir: string, since: string, count: int, truncated: bool, total_matched: int, assets: [{package_name, file_path, modified, size_bytes}], message: string }
TSharedPtr<FJsonObject> HandleListAssetsModifiedSince(const TSharedPtr<FJsonObject>& Params)
{
    FString TsStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("timestamp"), TsStr)) { return Missing; }
    FDateTime Since;
    if (!FDateTime::ParseIso8601(*TsStr, Since) && !FDateTime::Parse(TsStr, Since))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not parse timestamp: %s (use ISO-8601 or 'YYYY.MM.DD-HH.MM.SS')"), *TsStr));
    }

    FString PathFilter = TEXT("/Game");
    Params->TryGetStringField(TEXT("path"), PathFilter);
    PathFilter.RemoveFromEnd(TEXT("/"));

    int32 Limit = 500;
    double LimitD = 0.0;
    if (Params->TryGetNumberField(TEXT("limit"), LimitD)) { Limit = FMath::Clamp(static_cast<int32>(LimitD), 1, 5000); }

    FString Dir;
    if (!FPackageName::TryConvertLongPackageNameToFilename(PathFilter, Dir))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("path is not a mounted content path: %s"), *PathFilter));
    }

    TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.uasset"), true, false, /*bClearFileNames=*/true);
    IFileManager::Get().FindFilesRecursive(Files, *Dir, TEXT("*.umap"),   true, false, /*bClearFileNames=*/false);

    struct FHit { FString Package; FString File; FDateTime MTime; int64 Size; };
    TArray<FHit> Hits;
    IFileManager& FM = IFileManager::Get();
    for (const FString& F : Files)
    {
        const FDateTime MTime = FM.GetTimeStamp(*F);
        if (MTime <= Since) { continue; }
        FString Package;
        if (!FPackageName::TryConvertFilenameToLongPackageName(F, Package)) { Package = F; }
        Hits.Add(FHit{ Package, F, MTime, FM.FileSize(*F) });
    }

    const int32 TotalMatched = Hits.Num();
    Hits.Sort([](const FHit& A, const FHit& B) { return A.MTime > B.MTime; });
    const bool bTruncated = Hits.Num() > Limit;
    if (bTruncated) { Hits.SetNum(Limit); }

    TArray<TSharedPtr<FJsonValue>> AssetsArr;
    for (const FHit& H : Hits)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("package_name"), H.Package);
        Entry->SetStringField(TEXT("file_path"), H.File);
        Entry->SetStringField(TEXT("modified"), H.MTime.ToString());
        Entry->SetNumberField(TEXT("size_bytes"), static_cast<double>(H.Size));
        AssetsArr.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("path"), PathFilter);
    Result->SetStringField(TEXT("dir"), Dir);
    Result->SetStringField(TEXT("since"), Since.ToString());
    Result->SetNumberField(TEXT("count"), AssetsArr.Num());
    Result->SetNumberField(TEXT("total_matched"), TotalMatched);
    Result->SetBoolField(TEXT("truncated"), bTruncated);
    Result->SetArrayField(TEXT("assets"), AssetsArr);
    Result->SetStringField(TEXT("message"), FString::Printf(TEXT("%d package(s) modified after %s under %s."), TotalMatched, *Since.ToString(), *PathFilter));
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// diff_content_trees
// -------------------------------------------------------------------------- //
// MCP:diff_content_trees - Compare two Content folders (project vs external, fork vs source) WITHOUT loading the editor: reports assets added/removed/changed keyed by relative path, using file size and (optionally) a whole-file MD5 content hash. Verifies a Migrate or audits a fork on disk. Each tree may be a disk directory OR a /Game|/Engine mount path.
//   @param tree_a - First Content folder: absolute disk dir or /Game|/Engine mount path.
//   @param tree_b - Second Content folder: absolute disk dir or /Game|/Engine mount path.
//   @param hash - Optional bool (default true). When true, same-size files are compared by MD5 to detect content changes; when false, only size is compared.
//   @returns { tree_a: string, tree_b: string, dir_a: string, dir_b: string, only_in_a: [string], only_in_b: [string], changed: [{rel, size_a, size_b, hash_a, hash_b, reason}], identical_count: int, only_in_a_count: int, only_in_b_count: int, changed_count: int, message: string }
TSharedPtr<FJsonObject> HandleDiffContentTrees(const TSharedPtr<FJsonObject>& Params)
{
    FString TreeA, TreeB;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_a"), TreeA)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("tree_b"), TreeB)) { return Missing; }
    bool bHash = true;
    Params->TryGetBoolField(TEXT("hash"), bHash);

    FString DirA, DirB;
    if (!ResolveTreeDir_AssetUtil(TreeA, DirA))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve tree_a: %s"), *TreeA));
    }
    if (!ResolveTreeDir_AssetUtil(TreeB, DirB))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not resolve tree_b: %s"), *TreeB));
    }

    const int32 MaxFiles = 20000;
    TMap<FString, FTreeEntry_AssetUtil> A, B;
    EnumerateTree_AssetUtil(DirA, A, MaxFiles);
    EnumerateTree_AssetUtil(DirB, B, MaxFiles);

    IFileManager& FM = IFileManager::Get();
    auto HashOf = [&](const FString& Path) -> FString
    {
        const FMD5Hash H = FMD5Hash::HashFile(*Path);
        return H.IsValid() ? LexToString(H) : FString();
    };

    TArray<TSharedPtr<FJsonValue>> OnlyA, OnlyB;
    TArray<TSharedPtr<FJsonValue>> Changed;
    int32 IdenticalCount = 0;

    for (const TPair<FString, FTreeEntry_AssetUtil>& KV : A)
    {
        const FTreeEntry_AssetUtil* InB = B.Find(KV.Key);
        if (!InB)
        {
            OnlyA.Add(MakeShared<FJsonValueString>(KV.Value.Rel));
            continue;
        }
        const int64 SizeA = FM.FileSize(*KV.Value.Abs);
        const int64 SizeB = FM.FileSize(*InB->Abs);
        bool bChanged = false;
        FString Reason;
        FString HashA, HashB;
        if (SizeA != SizeB)
        {
            bChanged = true;
            Reason = TEXT("size");
        }
        else if (bHash)
        {
            HashA = HashOf(KV.Value.Abs);
            HashB = HashOf(InB->Abs);
            if (HashA != HashB) { bChanged = true; Reason = TEXT("hash"); }
        }
        if (bChanged)
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("rel"), KV.Value.Rel);
            Entry->SetNumberField(TEXT("size_a"), static_cast<double>(SizeA));
            Entry->SetNumberField(TEXT("size_b"), static_cast<double>(SizeB));
            Entry->SetStringField(TEXT("hash_a"), HashA);
            Entry->SetStringField(TEXT("hash_b"), HashB);
            Entry->SetStringField(TEXT("reason"), Reason);
            Changed.Add(MakeShared<FJsonValueObject>(Entry));
        }
        else
        {
            ++IdenticalCount;
        }
    }
    for (const TPair<FString, FTreeEntry_AssetUtil>& KV : B)
    {
        if (!A.Contains(KV.Key)) { OnlyB.Add(MakeShared<FJsonValueString>(KV.Value.Rel)); }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("tree_a"), TreeA);
    Result->SetStringField(TEXT("tree_b"), TreeB);
    Result->SetStringField(TEXT("dir_a"), DirA);
    Result->SetStringField(TEXT("dir_b"), DirB);
    Result->SetArrayField(TEXT("only_in_a"), OnlyA);
    Result->SetArrayField(TEXT("only_in_b"), OnlyB);
    Result->SetArrayField(TEXT("changed"), Changed);
    Result->SetNumberField(TEXT("identical_count"), IdenticalCount);
    Result->SetNumberField(TEXT("only_in_a_count"), OnlyA.Num());
    Result->SetNumberField(TEXT("only_in_b_count"), OnlyB.Num());
    Result->SetNumberField(TEXT("changed_count"), Changed.Num());
    Result->SetStringField(TEXT("message"), FString::Printf(
        TEXT("only_in_a=%d, only_in_b=%d, changed=%d, identical=%d (hash=%s)."),
        OnlyA.Num(), OnlyB.Num(), Changed.Num(), IdenticalCount, bHash ? TEXT("on") : TEXT("off")));
    return mcp_common::MakeOkReply(Result);
}

// -------------------------------------------------------------------------- //
// get_reference_path
// -------------------------------------------------------------------------- //
// MCP:get_reference_path - Find the shortest reference CHAIN through the AssetRegistry dependency graph: with `from`, the shortest from -> ... -> target (walking dependencies); without `from`, the shortest chain from a root (a package nothing references) DOWN to target (walking referencers) -- i.e. 'what keeps target alive / why is it undeletable'. get_asset_referencers gives the direct edges; this gives the PATH.
//   @param target - The asset whose reference chain to trace (object path or /Game package path).
//   @param from - Optional starting hub/root asset; when set, traces from -> target via dependencies instead of the referencer-root chain.
//   @param max_depth - Max chain depth to search (optional, default 16; clamped 1..64).
//   @param dependency_type - Optional "hard"|"soft"|"all" (case-insensitive, default "all"). Filters BOTH BFS directions (the from->target dependency walk and the referencer-root walk) and the always-computed direct_referencers list via the AssetRegistry hard/soft FDependencyQuery split; "all" leaves them unfiltered.
//   @returns { target: string, target_package: string, from: string, dependency_type: string, found: bool, reached_root: bool, chain: [string], chain_length: int, direct_referencers: [string], direct_referencer_count: int, message: string }
TSharedPtr<FJsonObject> HandleGetReferencePath(const TSharedPtr<FJsonObject>& Params)
{
    FString Target;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("target"), Target)) { return Missing; }
    FString From;
    Params->TryGetStringField(TEXT("from"), From);

    int32 MaxDepth = 16;
    double MaxDepthD = 0.0;
    if (Params->TryGetNumberField(TEXT("max_depth"), MaxDepthD)) { MaxDepth = FMath::Clamp(static_cast<int32>(MaxDepthD), 1, 64); }

    FString DependencyType = TEXT("all");
    Params->TryGetStringField(TEXT("dependency_type"), DependencyType);
    const UE::AssetRegistry::FDependencyQuery DepQuery = ResolveDependencyQuery_AssetUtil(DependencyType);

    const FName TargetPkg(*ToLongPackageName(Target));
    const bool bHasFrom = !From.IsEmpty();
    const FName FromPkg = bHasFrom ? FName(*ToLongPackageName(From)) : NAME_None;

    IAssetRegistry& AssetRegistry = mcp_common::AssetRegistry();
    const UE::AssetRegistry::EDependencyCategory Cat = UE::AssetRegistry::EDependencyCategory::Package;

    const int32 MaxVisited = 20000;
    TArray<FName> Chain;   // built root-first, ending at target
    bool bFound = false;
    bool bReachedRoot = false;

    if (bHasFrom)
    {
        // Forward BFS over dependencies from `from` until we reach target.
        TMap<FName, FName> Parent;  // node -> the node we expanded to reach it (toward `from`)
        TSet<FName> Visited;
        TArray<FName> Frontier;
        Frontier.Add(FromPkg);
        Visited.Add(FromPkg);
        for (int32 Depth = 0; Depth < MaxDepth && Frontier.Num() > 0 && !bFound; ++Depth)
        {
            TArray<FName> Next;
            for (const FName& Node : Frontier)
            {
                if (Node == TargetPkg) { bFound = true; break; }
                TArray<FName> Deps;
                AssetRegistry.GetDependencies(Node, Deps, Cat, DepQuery);
                for (const FName& Dep : Deps)
                {
                    if (Visited.Contains(Dep)) { continue; }
                    Visited.Add(Dep);
                    Parent.Add(Dep, Node);
                    if (Dep == TargetPkg) { bFound = true; break; }
                    Next.Add(Dep);
                    if (Visited.Num() > MaxVisited) { break; }
                }
                if (bFound) { break; }
            }
            if (bFound) { break; }
            Frontier = MoveTemp(Next);
        }
        if (FromPkg == TargetPkg) { bFound = true; }
        if (bFound)
        {
            // Reconstruct target -> ... -> from, then reverse to from -> ... -> target.
            TArray<FName> Rev;
            FName Cur = TargetPkg;
            Rev.Add(Cur);
            while (Cur != FromPkg)
            {
                const FName* P = Parent.Find(Cur);
                if (!P) { break; }
                Cur = *P;
                Rev.Add(Cur);
            }
            for (int32 i = Rev.Num() - 1; i >= 0; --i) { Chain.Add(Rev[i]); }
        }
    }
    else
    {
        // Backward BFS over referencers from target until we hit a root (0 referencers).
        TMap<FName, FName> Parent;   // node -> node closer to target (the one it references)
        TSet<FName> Visited;
        TArray<FName> Frontier;
        Frontier.Add(TargetPkg);
        Visited.Add(TargetPkg);
        Parent.Add(TargetPkg, NAME_None);
        FName RootNode = NAME_None;
        FName DeepestNode = TargetPkg;
        for (int32 Depth = 0; Depth < MaxDepth && Frontier.Num() > 0 && RootNode.IsNone(); ++Depth)
        {
            TArray<FName> Next;
            for (const FName& Node : Frontier)
            {
                TArray<FName> Refs;
                AssetRegistry.GetReferencers(Node, Refs, Cat, DepQuery);
                if (Node != TargetPkg && Refs.Num() == 0)
                {
                    RootNode = Node;   // BFS => shortest root chain
                    break;
                }
                for (const FName& Ref : Refs)
                {
                    if (Visited.Contains(Ref)) { continue; }
                    Visited.Add(Ref);
                    Parent.Add(Ref, Node);
                    DeepestNode = Ref;
                    Next.Add(Ref);
                    if (Visited.Num() > MaxVisited) { break; }
                }
            }
            if (!RootNode.IsNone()) { break; }
            Frontier = MoveTemp(Next);
        }

        FName StartNode = RootNode;
        if (RootNode.IsNone())
        {
            // No zero-referencer root within depth: fall back to the farthest referencer found.
            StartNode = DeepestNode;
        }
        else
        {
            bReachedRoot = true;
        }
        if (StartNode != TargetPkg || RootNode == TargetPkg)
        {
            // Reconstruct StartNode -> ... -> target via Parent (each points toward target).
            FName Cur = StartNode;
            Chain.Add(Cur);
            int32 Guard = 0;
            while (Cur != TargetPkg && Guard++ < MaxDepth + 2)
            {
                const FName* P = Parent.Find(Cur);
                if (!P || P->IsNone()) { break; }
                Cur = *P;
                Chain.Add(Cur);
            }
            bFound = (Chain.Num() > 0) && (Chain.Last() == TargetPkg);
        }
    }

    // Direct referencers of the target for context.
    TArray<FName> DirectRefs;
    AssetRegistry.GetReferencers(TargetPkg, DirectRefs, Cat, DepQuery);

    TArray<TSharedPtr<FJsonValue>> ChainJson;
    for (const FName& N : Chain) { ChainJson.Add(MakeShared<FJsonValueString>(N.ToString())); }
    TArray<TSharedPtr<FJsonValue>> DirectJson;
    for (const FName& N : DirectRefs) { DirectJson.Add(MakeShared<FJsonValueString>(N.ToString())); }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("target"), Target);
    Result->SetStringField(TEXT("target_package"), TargetPkg.ToString());
    Result->SetStringField(TEXT("from"), From);
    Result->SetStringField(TEXT("dependency_type"), DependencyType);
    Result->SetBoolField(TEXT("found"), bFound);
    Result->SetBoolField(TEXT("reached_root"), bReachedRoot);
    Result->SetArrayField(TEXT("chain"), ChainJson);
    Result->SetNumberField(TEXT("chain_length"), ChainJson.Num());
    Result->SetArrayField(TEXT("direct_referencers"), DirectJson);
    Result->SetNumberField(TEXT("direct_referencer_count"), DirectRefs.Num());
    Result->SetStringField(TEXT("message"), bFound
        ? FString::Printf(TEXT("Chain of %d node(s)%s."), ChainJson.Num(), bReachedRoot ? TEXT(" from a root") : TEXT(""))
        : (bHasFrom ? TEXT("No dependency path from `from` to target within max_depth.")
                    : TEXT("No referencer chain found (target may be an orphan / not referenced).")));
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("rename_assets"), &HandleBulkRenameAssets },
    { TEXT("compare_asset_versions"), &HandleCompareAssetVersions },
    { TEXT("diff_content_trees"), &HandleDiffContentTrees },
    { TEXT("evaluate_curve_at_time"), &HandleEvaluateCurveAtTime },
    { TEXT("generate_asset_thumbnail"), &HandleGenerateAssetThumbnail },
    { TEXT("get_asset_dependencies_recursive"), &HandleGetAssetDependenciesRecursive },
    { TEXT("get_reference_path"), &HandleGetReferencePath },
    { TEXT("ingest_external_uassets"), &HandleIngestExternalUAssets },
    { TEXT("list_assets_modified_since"), &HandleListAssetsModifiedSince },
    { TEXT("scan_uasset_references"), &HandleScanUAssetRefs },
};
} // namespace UEAgentAssetUtilCommands
