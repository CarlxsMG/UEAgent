// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Material/AssetPathUtils.h"
#include "MaterialEditingLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Engine/Texture.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Editor.h"
#include "UObject/UObjectIterator.h"
#include "RHIShaderPlatform.h"
#include "MaterialShared.h"
#include "ShaderCompiler.h"            // GShaderCompilingManager->FinishAllCompilation (get_material_stats)
#include "Misc/App.h"                  // FApp::CanEverRender (honest no-RHI gate)
#include "Templates/Function.h"        // TFunctionRef (graph node visitor)
#include "Misc/EngineVersionComparison.h"
#if UE_VERSION_OLDER_THAN(5, 8, 0)
#include "IMaterialEditor.h"                          // NotifyExternalMaterialChange (5.7 disconnect)
#include "Subsystems/AssetEditorSubsystem.h"
#include "MaterialGraph/MaterialGraphNode.h"          // GetShortenPinName (5.7 disconnect)
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Particles/ParticleSystemComponent.h"        // 5.7 usage-flag recompile
#include "EditorSupportDelegates.h"
#endif

namespace UEAgentMaterialCommands
{

// MCP-DOMAIN: Material

// ---------------------------------------------------------------------------------------------
// Plumbing shared by the material handlers.
// ---------------------------------------------------------------------------------------------
namespace UEAgentMaterialCommandsImpl
{

    /** "<What> not found: <Path>" followed by the hint that a full object path is expected. */
    TSharedPtr<FJsonObject> NotFoundByPath(const TCHAR* What, const FString& Path)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s not found: %s. Use full path like '/Game/Materials/MyMaterial.MyMaterial'"), What, *Path));
    }

    /** Reply for a usage name that does not map onto an EMaterialUsage value. */
    TSharedPtr<FJsonObject> UnknownUsage(const FString& Requested)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown or invalid material usage: %s. Expected an EMaterialUsage name like 'SkeletalMesh' or 'MATUSAGE_SkeletalMesh'."),
            *Requested));
    }

    /** The UMaterial (graph owner) behind Path into OutMaterial; the standard not-found reply when it does not load. */
    TSharedPtr<FJsonObject> LoadBaseMaterial(const FString& Path, UMaterial*& OutMaterial)
    {
        OutMaterial = AssetPathUtils::LoadOrLoadSoftPathString<UMaterial>(Path);
        if (OutMaterial == nullptr)
        {
            return NotFoundByPath(TEXT("Material"), Path);
        }
        return nullptr;
    }

    /**
     * Calls Visitor for every expression node whose outer is Material, in object-iterator order,
     * and stops as soon as Visitor returns false.
     */
    void VisitGraphNodes(UMaterial* Material, TFunctionRef<bool(UMaterialExpression*)> Visitor)
    {
        for (TObjectIterator<UMaterialExpression> NodeIt; NodeIt; ++NodeIt)
        {
            UMaterialExpression* Node = *NodeIt;
            if (Node == nullptr || Node->GetOuter() != Material)
            {
                continue;
            }
            if (!Visitor(Node))
            {
                return;
            }
        }
    }

    /** The node of Material called NodeName (object names are unique per outer), or nullptr. */
    UMaterialExpression* FindGraphNode(UMaterial* Material, const FString& NodeName)
    {
        UMaterialExpression* Match = nullptr;
        VisitGraphNodes(Material, [&Match, &NodeName](UMaterialExpression* Node)
        {
            if (Node->GetName() == NodeName)
            {
                Match = Node;
            }
            return Match == nullptr;
        });
        return Match;
    }

    /**
     * "SkeletalMesh" or "MATUSAGE_SkeletalMesh" -> EMaterialUsage, through reflection. Names the
     * enum does not know are refused, and so is the MATUSAGE_MAX sentinel.
     */
    bool ParseUsageFlag(const FString& Requested, EMaterialUsage& OutUsage, FString& OutCanonicalName)
    {
        const UEnum* UsageEnum = StaticEnum<EMaterialUsage>();
        if (UsageEnum == nullptr)
        {
            return false;
        }
        const FString Prefix(TEXT("MATUSAGE_"));
        const FString Qualified = Requested.StartsWith(Prefix) ? Requested : Prefix + Requested;
        const int64 RawValue = UsageEnum->GetValueByNameString(Qualified);
        if (RawValue == INDEX_NONE || RawValue == static_cast<int64>(MATUSAGE_MAX))
        {
            return false;
        }
        OutUsage = static_cast<EMaterialUsage>(RawValue);
        OutCanonicalName = UsageEnum->GetNameStringByValue(RawValue);
        return true;
    }

    /**
     * Writes Material's package to disk: callers check by reloading, which a dirty flag alone would
     * not satisfy. False, with OutError, when the material has no package or the save fails.
     */
    bool SaveMaterialPackage(UMaterial* Material, FString& OutError)
    {
        if (UPackage* OwningPackage = Material->GetOutermost())
        {
            return mcp_common::SaveAssetPackage(OwningPackage, Material, OutError);
        }
        OutError = TEXT("Could not get package from material");
        return false;
    }

    /** The editor's IAssetTools; loading the module is a no-op once it is up. */
    IAssetTools& AssetToolsApi()
    {
        return FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    }

    /**
     * Finds a material asset by NAME. The name is first tried as a package name; failing that, the
     * material assets under /Game are scanned for the first one whose asset name matches
     * (case-insensitively).
     */
    bool FindMaterialAssetByName(IAssetRegistry& Registry, const FString& Name, FAssetData& OutAsset)
    {
        TArray<FAssetData> InPackage;
        Registry.GetAssetsByPackageName(FName(*Name), InPackage);
        if (InPackage.Num() > 0)
        {
            OutAsset = InPackage[0];
            return true;
        }

        FARFilter MaterialsUnderGame;
        MaterialsUnderGame.ClassPaths.Add(UMaterialInterface::StaticClass()->GetClassPathName());
        MaterialsUnderGame.ClassPaths.Add(UMaterial::StaticClass()->GetClassPathName());
        MaterialsUnderGame.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());
        MaterialsUnderGame.bRecursivePaths = true;
        MaterialsUnderGame.PackagePaths.Add(TEXT("/Game"));

        TArray<FAssetData> Candidates;
        Registry.GetAssets(MaterialsUnderGame, Candidates);
        for (const FAssetData& Candidate : Candidates)
        {
            if (Candidate.AssetName.ToString() == Name)
            {
                OutAsset = Candidate;
                return true;
            }
        }
        return false;
    }

    /** material_class / material_category / material_tooltip / material_display_name ("Unknown" x4 without a material). */
    void WriteMaterialClassInfo(const TSharedPtr<FJsonObject>& Target, UMaterialInterface* Material)
    {
        if (Material == nullptr)
        {
            Target->SetStringField(TEXT("material_class"), TEXT("Unknown"));
            Target->SetStringField(TEXT("material_category"), TEXT("Unknown"));
            Target->SetStringField(TEXT("material_tooltip"), TEXT("Unknown"));
            Target->SetStringField(TEXT("material_display_name"), TEXT("Unknown"));
            return;
        }
        const UClass* MaterialClass = Material->GetClass();
        Target->SetStringField(TEXT("material_class"), MaterialClass->GetName());
        Target->SetStringField(TEXT("material_category"), MaterialClass->GetMetaData(TEXT("Category")));
        Target->SetStringField(TEXT("material_tooltip"), MaterialClass->GetMetaData(TEXT("Tooltip")));
        Target->SetStringField(TEXT("material_display_name"), MaterialClass->GetMetaData(TEXT("DisplayName")));
    }

    /** Display name of the blend modes these commands report. */
    const TCHAR* BlendModeName(EBlendMode Mode)
    {
        switch (Mode)
        {
        case BLEND_Opaque:      return TEXT("Opaque");
        case BLEND_Masked:      return TEXT("Masked");
        case BLEND_Translucent: return TEXT("Translucent");
        case BLEND_Additive:    return TEXT("Additive");
        case BLEND_Modulate:    return TEXT("Modulate");
        default:                return TEXT("Unknown");
        }
    }

    /**
     * Two-sided and masked (asked of base materials only), the translucent / opaque flags and the
     * blend mode name. Without a material this reads as an opaque, single-sided, unmasked surface.
     */
    void WriteSurfaceTraits(const TSharedPtr<FJsonObject>& Target, UMaterialInterface* Material)
    {
        UMaterial* AsBase = Cast<UMaterial>(Material);
        const EBlendMode Mode = Material != nullptr ? Material->GetBlendMode() : BLEND_Opaque;
        const bool bTranslucentFamily = Mode == BLEND_Translucent || Mode == BLEND_Additive || Mode == BLEND_Modulate;
        Target->SetBoolField(TEXT("material_is_two_sided"), AsBase != nullptr && AsBase->IsTwoSided());
        Target->SetBoolField(TEXT("material_is_masked"), AsBase != nullptr && AsBase->IsMasked());
        Target->SetBoolField(TEXT("material_is_translucent"), bTranslucentFamily);
        Target->SetBoolField(TEXT("material_is_opaque"), Mode == BLEND_Opaque);
        Target->SetStringField(TEXT("blend_mode"), BlendModeName(Mode));
    }

    /** Per-kind counters for the packages that reference a material. */
    struct FReferencerTally
    {
        // StaticMesh, SkeletalMesh, Blueprint, Level, then everything else.
        int32 ByKind[5] = { 0, 0, 0, 0, 0 };

        void WriteTo(const TSharedPtr<FJsonObject>& Target) const
        {
            Target->SetNumberField(TEXT("static_mesh_usage_count"), ByKind[0]);
            Target->SetNumberField(TEXT("skeletal_mesh_usage_count"), ByKind[1]);
            Target->SetNumberField(TEXT("blueprint_usage_count"), ByKind[2]);
            Target->SetNumberField(TEXT("level_usage_count"), ByKind[3]);
            Target->SetNumberField(TEXT("other_usage_count"), ByKind[4]);
        }
    };

    /**
     * One usage row for a referencing package. The package name is handed to the object-path
     * lookup as it is; when that finds nothing the row is reported as Unknown.
     */
    TSharedPtr<FJsonObject> DescribeReferencer(IAssetRegistry& Registry, const FName& PackageName, FReferencerTally& Tally)
    {
        static const TCHAR* const KindTokens[4] = { TEXT("StaticMesh"), TEXT("SkeletalMesh"), TEXT("Blueprint"), TEXT("Level") };
        static const TCHAR* const KindCategories[4] = { TEXT("Mesh"), TEXT("Mesh"), TEXT("Blueprint"), TEXT("Level") };
        constexpr int32 OtherKind = 4;

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        const FString PackageText = PackageName.ToString();
        Row->SetStringField(TEXT("package_name"), PackageText);

        // The referencer is a PACKAGE: its assets are listed by package name (asking for the package
        // name as an object path found nothing, so most rows came out "Unknown"). The package's
        // primary asset -- the one named after it -- describes it; otherwise its first asset.
        TArray<FAssetData> InPackage;
        Registry.GetAssetsByPackageName(PackageName, InPackage);
        if (InPackage.Num() == 0)
        {
            Row->SetStringField(TEXT("usage_type"), TEXT("Unknown"));
            Row->SetStringField(TEXT("usage_category"), TEXT("Unknown"));
            ++Tally.ByKind[OtherKind];
            return Row;
        }
        const FString ShortName = FPackageName::GetShortName(PackageText);
        const FAssetData* Primary = InPackage.FindByPredicate(
            [&ShortName](const FAssetData& Candidate) { return Candidate.AssetName.ToString() == ShortName; });
        const FAssetData& Referencer = Primary != nullptr ? *Primary : InPackage[0];

        const FString ClassText = Referencer.AssetClassPath.ToString();
        Row->SetStringField(TEXT("asset_name"), Referencer.AssetName.ToString());
        Row->SetStringField(TEXT("asset_class"), ClassText);
        Row->SetStringField(TEXT("asset_path"), Referencer.GetObjectPathString());

        // A map (UWorld) or an actor saved in its own package (One File Per Actor) is a level use;
        // otherwise the first kind whose token occurs in the class path (case-insensitive) wins.
        int32 Kind = OtherKind;
        if (ClassText == TEXT("/Script/Engine.World") || PackageText.Contains(TEXT("/__ExternalActors__/")))
        {
            Kind = 3;
        }
        for (int32 Index = 0; Kind == OtherKind && Index < OtherKind; ++Index)
        {
            if (ClassText.Contains(KindTokens[Index]))
            {
                Kind = Index;
            }
        }
        Row->SetStringField(TEXT("usage_type"), Kind == OtherKind ? TEXT("Other") : KindTokens[Kind]);
        Row->SetStringField(TEXT("usage_category"), Kind == OtherKind ? TEXT("Other") : KindCategories[Kind]);
        ++Tally.ByKind[Kind];

        // Registry tags of the referencing asset; a tag it does not carry is reported as "".
        struct FTagField
        {
            const TCHAR* Tag;
            const TCHAR* Field;
        };
        static const FTagField TagFields[] = {
            { TEXT("Category"), TEXT("asset_category") },
            { TEXT("Tooltip"), TEXT("asset_tooltip") },
            { TEXT("DisplayName"), TEXT("asset_display_name") },
        };
        for (const FTagField& TagField : TagFields)
        {
            FString TagValue;
            Referencer.GetTagValue(TagField.Tag, TagValue);
            Row->SetStringField(TagField.Field, TagValue);
        }
        return Row;
    }

    /**
     * True when the instance (through its parent chain) exposes a global parameter of that type and
     * name. The UMaterialEditingLibrary setters below cannot answer it: they write any name -- an
     * orphan override for a parameter the parent does not have -- and always return false.
     */
    bool InstanceExposesParameter(const UMaterialInstanceConstant* Instance, EMaterialParameterType Type, FName ParamName)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Ids;
        Instance->GetAllParameterInfoOfType(Type, Infos, Ids);
        return Infos.ContainsByPredicate([ParamName](const FMaterialParameterInfo& Info)
        {
            return Info.Name == ParamName && Info.Association == EMaterialParameterAssociation::GlobalParameter;
        });
    }

    /**
     * Applies one parameter_overrides entry: a number sets a scalar, a bool a static switch and an
     * object {r, g, b[, a]} a vector (alpha defaults to 1). Returns the kind that was applied, or
     * nullptr when the value has another JSON type or the parent exposes no global parameter of that
     * type and name (nothing is written then, so no orphan override is left on the instance).
     */
    const TCHAR* ApplyParameterOverride(UMaterialInstanceConstant* Instance, const FString& ParamName, const FJsonValue& Value)
    {
        const FName Name(*ParamName);
        switch (Value.Type)
        {
        case EJson::Number:
        {
            if (!InstanceExposesParameter(Instance, EMaterialParameterType::Scalar, Name))
            {
                return nullptr;
            }
            UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, Name, static_cast<float>(Value.AsNumber()));
            return TEXT("scalar");
        }
        case EJson::Boolean:
            if (!InstanceExposesParameter(Instance, EMaterialParameterType::StaticSwitch, Name))
            {
                return nullptr;
            }
            UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, Name, Value.AsBool());
            return TEXT("static_switch");
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject> Channels = Value.AsObject();
            if (!Channels.IsValid() || !InstanceExposesParameter(Instance, EMaterialParameterType::Vector, Name))
            {
                return nullptr;
            }
            const float Red = static_cast<float>(Channels->GetNumberField(TEXT("r")));
            const float Green = static_cast<float>(Channels->GetNumberField(TEXT("g")));
            const float Blue = static_cast<float>(Channels->GetNumberField(TEXT("b")));
            const float Alpha = Channels->HasField(TEXT("a")) ? static_cast<float>(Channels->GetNumberField(TEXT("a"))) : 1.0f;
            UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, Name, FLinearColor(Red, Green, Blue, Alpha));
            return TEXT("vector");
        }
        default:
            return nullptr;
        }
    }

#if UE_VERSION_OLDER_THAN(5, 8, 0)
    // UE 5.7: UMaterialEditingLibrary::DisconnectMaterialExpressions and ::SetBaseMaterialUsage arrived in 5.8. These
    // do the same work with what 5.7 exposes (5.8's bodies, their file-private helpers written out).

    /** Refresh an open Material Editor of Material, as 5.8's library does after editing its graph. */
    void NotifyMaterialEditorOfChange57(UMaterialInterface* Material)
    {
        if (Material == nullptr || GEditor == nullptr || Material->IsA(UMaterialInstanceDynamic::StaticClass()))
        {
            return;
        }
        if (IAssetEditorInstance* Editor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Material, false))
        {
            static_cast<IMaterialEditor*>(Editor)->NotifyExternalMaterialChange();
        }
    }

    /** 5.8's DisconnectMaterialExpressions: break the link into ToExpression's input named ToInputName (first input
     *  when empty). True when a link was broken; false when the input does not exist or was not connected. */
    bool DisconnectMaterialExpressions57(UMaterialExpression* ToExpression, const FString& ToInputName)
    {
        if (ToExpression == nullptr)
        {
            return false;
        }
        const FName InputName(*ToInputName);
        FExpressionInput* Input = nullptr;
        if (InputName.IsNone())
        {
            Input = ToExpression->GetInput(0);
        }
        else
        {
            for (FExpressionInputIterator It{ ToExpression }; It; ++It)
            {
                // A function call's pin is compared without its type postfix; any other pin by its shortened name.
                FName TestName;
                if (UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(ToExpression))
                {
                    TestName = FuncCall->GetInputNameWithType(It.Index, false);
                }
                else
                {
                    TestName = UMaterialGraphNode::GetShortenPinName(ToExpression->GetInputName(It.Index));
                }
                if (TestName == InputName)
                {
                    Input = It.Input;
                    break;
                }
            }
        }
        if (Input == nullptr || !Input->IsConnected())
        {
            return false;
        }
        Input->Expression = nullptr;
        Input->OutputIndex = 0;
        NotifyMaterialEditorOfChange57(Cast<UMaterial>(ToExpression->GetOuter()));
        return true;
    }

    /**
     * UMaterialInstance::UpdateParameterNames is protected in 5.7 (5.8's library calls it as a friend of the class).
     * This using-declaration names that same engine function publicly; it is only ever used through the member
     * pointer it yields, which is of type void (UMaterialInstance::*)(). The type is never instantiated.
     */
    struct FMaterialInstanceUpdateParameterNames57 : public UMaterialInstance
    {
        using UMaterialInstance::UpdateParameterNames;
    };

    /**
     * 5.8's SetBaseMaterialUsage. 5.7's UMaterial::SetUsageByFlag is private, so the public bUsedWith* bit it switches
     * over is written here; then the recompile 5.8 runs (UMaterialEditingLibrary::RecompileMaterialInternal, private in
     * 5.7, written out: the public RecompileMaterial would also rebuild texture streaming data behind a modal dialog)
     * and the same broadcast. Returns early when the flag already holds bValue.
     */
    void SetBaseMaterialUsage57(UMaterial* Material, EMaterialUsage Usage, bool bValue)
    {
        if (Material == nullptr || Material->GetUsageByFlag(Usage) == bValue)
        {
            return;
        }
        switch (Usage)
        {
        case MATUSAGE_SkeletalMesh:            Material->bUsedWithSkeletalMesh = bValue; break;
        case MATUSAGE_ParticleSprites:         Material->bUsedWithParticleSprites = bValue; break;
        case MATUSAGE_BeamTrails:              Material->bUsedWithBeamTrails = bValue; break;
        case MATUSAGE_MeshParticles:           Material->bUsedWithMeshParticles = bValue; break;
        case MATUSAGE_NiagaraSprites:          Material->bUsedWithNiagaraSprites = bValue; break;
        case MATUSAGE_NiagaraRibbons:          Material->bUsedWithNiagaraRibbons = bValue; break;
        case MATUSAGE_NiagaraMeshParticles:    Material->bUsedWithNiagaraMeshParticles = bValue; break;
        case MATUSAGE_StaticLighting:          Material->bUsedWithStaticLighting = bValue; break;
        case MATUSAGE_MorphTargets:            Material->bUsedWithMorphTargets = bValue; break;
        case MATUSAGE_SplineMesh:              Material->bUsedWithSplineMeshes = bValue; break;
        case MATUSAGE_InstancedStaticMeshes:   Material->bUsedWithInstancedStaticMeshes = bValue; break;
        case MATUSAGE_GeometryCollections:     Material->bUsedWithGeometryCollections = bValue; break;
        case MATUSAGE_Clothing:                Material->bUsedWithClothing = bValue; break;
        case MATUSAGE_GeometryCache:           Material->bUsedWithGeometryCache = bValue; break;
        case MATUSAGE_Water:                   Material->bUsedWithWater = bValue; break;
        case MATUSAGE_HairStrands:             Material->bUsedWithHairStrands = bValue; break;
        case MATUSAGE_LidarPointCloud:         Material->bUsedWithLidarPointCloud = bValue; break;
        case MATUSAGE_VirtualHeightfieldMesh:  Material->bUsedWithVirtualHeightfieldMesh = bValue; break;
        case MATUSAGE_Nanite:                  Material->bUsedWithNanite = bValue; break;
        case MATUSAGE_Voxels:                  Material->bUsedWithVoxels = bValue; break;
        case MATUSAGE_VolumetricCloud:         Material->bUsedWithVolumetricCloud = bValue; break;
        case MATUSAGE_HeterogeneousVolumes:    Material->bUsedWithHeterogeneousVolumes = bValue; break;
        case MATUSAGE_StaticMesh:              Material->bUsedWithStaticMesh = bValue; break;
        default:                               return;
        }

        {
            FMaterialUpdateContext UpdateContext;
            UpdateContext.AddMaterial(Material);
            Material->PreEditChange(nullptr);
            Material->PostEditChange();
            Material->MarkPackageDirty();
            // Particle components re-evaluate their view relevance; child instances refresh their parameter names.
            for (TObjectIterator<UParticleSystemComponent> It(RF_ClassDefaultObject, true, EInternalObjectFlags::Garbage); It; ++It)
            {
                It->bIsViewRelevanceDirty = true;
            }
            void (UMaterialInstance::*UpdateNames)() = &FMaterialInstanceUpdateParameterNames57::UpdateParameterNames;
            for (TObjectIterator<UMaterialInstance> It(RF_ClassDefaultObject, true, EInternalObjectFlags::Garbage); It; ++It)
            {
                if (It->Parent == Material)
                {
                    UMaterialInstance* ChildInstance = *It;
                    (ChildInstance->*UpdateNames)();
                }
            }
        }
        UMaterialEditingLibrary::RebuildMaterialInstanceEditors(Material);

        FEditorSupportDelegates::MaterialUsageFlagsChanged.Broadcast(Material, Usage);
    }
#endif
}

// MCP:create_material - Create a new base material asset (UMaterial) -- the graph-owning master whose parameters and static switches every instance of it inherits. Prefer create_material_instance when you only need a re-skin of something that exists: an instance costs no new shader permutations and leaves the original updatable. After creating, build the graph with create_material_expression + connect_material_expressions, then verify with get_material_stats (compile_errors empty and stats_valid true) -- compile_material only acknowledges, it does not report errors, and a landscape material that fails to compile silently falls back to the default material.
//   @param material_path - Content FOLDER the material is created in (e.g., '/Game/Materials')
//   @param material_name - Name of the material asset to create
//   @returns { material_path: string (object path of the new material, '/Game/Materials/M_X.M_X'), material_name: string, package_path: string, success: bool } -- an existing asset of that name is an error, never overwritten

TSharedPtr<FJsonObject> HandleCreateMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString FolderPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), FolderPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString AssetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_name"), AssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Only the validity of the long package name matters; the filename it maps to is not used.
    FString MappedFilename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(FolderPath, MappedFilename))
    {
        return mcp_common::MakeErrorReply(TEXT("Invalid material path. Use format like '/Game/Materials/MyMaterial'"));
    }

    // The new asset's own path: "<folder>/<name>.<name>" (not "<folder>.<name>", which names
    // nothing): an existing material must be found here, or CreateAsset can raise a modal
    // overwrite prompt in the editor.
    FolderPath.RemoveFromEnd(TEXT("/"));
    const FString PackageName = FolderPath / AssetName;
    const FString ReportedPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
    if (UEditorAssetLibrary::DoesAssetExist(PackageName))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material already exists at path: %s"), *ReportedPath));
    }

    IAssetTools& Tools = UEAgentMaterialCommandsImpl::AssetToolsApi();
    UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
    if (Factory == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create MaterialFactory"));
    }

    UObject* NewAsset = Tools.CreateAsset(AssetName, FolderPath, UMaterial::StaticClass(), Factory);
    if (NewAsset == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create material asset"));
    }
    UMaterial* NewMaterial = Cast<UMaterial>(NewAsset);
    if (NewMaterial == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Created asset is not a UMaterial"));
    }
    NewMaterial->MarkPackageDirty();

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), ReportedPath);
    Reply->SetStringField(TEXT("material_name"), AssetName);
    Reply->SetStringField(TEXT("package_path"), FolderPath);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:create_material_instance - Create a material instance (MIC) from a parent material -- the cheap, non-destructive way to re-skin a vendor or engine material: the parent stays untouched and updatable while only your overrides are stored, and no new shader is compiled -- optionally applying a dict of parameter overrides in the same call. Idempotent (already_exists=true when it is already there; parameter_overrides are then not applied and note says so). The parent may be omitted (a parentless instance) and may be given as a bare material NAME instead of a path; resolve an ambiguous name first. Check applied_overrides_count against what you sent and re-read with get_material_parameters: a name the parent does not expose is simply not applied. Tune it later with set_material_instance_parameter, and remember static switches inherit from the parent and are set separately.
//   @param instance_path - Content folder for the material instance (e.g., '/Game/Materials'); defaults to /Game/MCP. The asset is created at <instance_path>/<instance_name>
//   @param package_path - Alias of instance_path
//   @param instance_name - Name of the material instance to create
//   @param name - Alias of instance_name
//   @param parent_material_path - Optional parent material: a full path ('/Game/Materials/MyMaterial.MyMaterial') or a bare material asset name looked up under /Game; omit it for a parentless instance
//   @param parameter_overrides - Optional dict {param_name: scalar_number | {r,g,b[,a]} vector object | static_switch_bool} applied to the new instance before it is saved
//   @returns { instance_path: string, instance_name: string, material_instance_path: string, material_instance_name: string, parent_material?: string, parent_material_path: string, parent_material_name: string, created: bool, already_existed: bool, applied_overrides: [{name, type}], applied_overrides_count: int, success: bool, already_exists?: bool, note?: string, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateMaterialInstance(const TSharedPtr<FJsonObject>& Params)
{
    // Folder: instance_path, else its alias package_path, else the default folder.
    FString TargetFolder;
    if (!Params->TryGetStringField(TEXT("instance_path"), TargetFolder) && !Params->TryGetStringField(TEXT("package_path"), TargetFolder))
    {
        TargetFolder = mcp_common::DefaultAssetFolder();
    }
    while (TargetFolder.EndsWith(TEXT("/")))
    {
        TargetFolder.LeftChopInline(1, EAllowShrinking::No);
    }
    FString NewAssetName;
    if (!Params->TryGetStringField(TEXT("instance_name"), NewAssetName) && !Params->TryGetStringField(TEXT("name"), NewAssetName))
    {
        return mcp_common::MissingParameterReply(TEXT("instance_name"));
    }
    FString ParentPath;
    Params->TryGetStringField(TEXT("parent_material_path"), ParentPath);
    const TSharedPtr<FJsonObject>* Overrides = nullptr;
    const bool bHasOverrides = Params->TryGetObjectField(TEXT("parameter_overrides"), Overrides) && Overrides != nullptr && Overrides->IsValid();

    // The parent is optional; given, it is a path or, failing that, a bare material asset name.
    UMaterialInterface* Parent = nullptr;
    if (!ParentPath.IsEmpty())
    {
        Parent = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialInterface>(ParentPath);
        FAssetData NamedParent;
        if (Parent == nullptr && !ParentPath.Contains(TEXT("/"))
            && UEAgentMaterialCommandsImpl::FindMaterialAssetByName(mcp_common::AssetRegistry(), ParentPath, NamedParent))
        {
            Parent = Cast<UMaterialInterface>(NamedParent.GetAsset());
        }
        if (Parent == nullptr)
        {
            return UEAgentMaterialCommandsImpl::NotFoundByPath(TEXT("Parent material"), ParentPath);
        }
    }

    // CreateAsset names the package <folder>/<name>, so the new object lives at <folder>/<name>.<name>.
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *TargetFolder, *NewAssetName, *NewAssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(ObjectPath)) { return Invalid; }
    if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
    {
        // Idempotent create: an instance that is already there is reported, not recreated or edited.
        const UMaterialInstanceConstant* Existing = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
        if (Existing == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance already exists at path: %s"), *ObjectPath));
        }
        const FString ExistingParentPath = Existing->Parent ? Existing->Parent->GetPathName() : FString();
        TSharedPtr<FJsonObject> ExistingInfo = MakeShared<FJsonObject>();
        ExistingInfo->SetStringField(TEXT("instance_path"), ObjectPath);
        ExistingInfo->SetStringField(TEXT("instance_name"), NewAssetName);
        ExistingInfo->SetStringField(TEXT("material_instance_path"), Existing->GetPathName());
        ExistingInfo->SetStringField(TEXT("material_instance_name"), Existing->GetName());
        ExistingInfo->SetStringField(TEXT("parent_material"), ExistingParentPath);
        ExistingInfo->SetStringField(TEXT("parent_material_path"), Existing->Parent ? ExistingParentPath : FString(TEXT("None")));
        ExistingInfo->SetStringField(TEXT("parent_material_name"), Existing->Parent ? Existing->Parent->GetName() : FString(TEXT("None")));
        ExistingInfo->SetBoolField(TEXT("created"), false);
        ExistingInfo->SetBoolField(TEXT("already_existed"), true);
        ExistingInfo->SetBoolField(TEXT("already_exists"), true);
        ExistingInfo->SetArrayField(TEXT("applied_overrides"), TArray<TSharedPtr<FJsonValue>>());
        ExistingInfo->SetNumberField(TEXT("applied_overrides_count"), 0);
        if (bHasOverrides)
        {
            ExistingInfo->SetStringField(TEXT("note"), TEXT("The instance already existed: it was left unchanged and parameter_overrides were not applied."));
        }
        return mcp_common::MakeOkReply(ExistingInfo);
    }

    IAssetTools& Tools = UEAgentMaterialCommandsImpl::AssetToolsApi();
    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    if (Factory == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create MaterialInstanceConstantFactory"));
    }
    Factory->InitialParent = Parent;

    UObject* NewAsset = Tools.CreateAsset(NewAssetName, TargetFolder, UMaterialInstanceConstant::StaticClass(), Factory);
    if (NewAsset == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create material instance asset"));
    }
    UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(NewAsset);
    if (Instance == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Created asset is not a UMaterialInstanceConstant"));
    }

    // Optional overrides {name: number | bool | {r,g,b[,a]}}. Only what the engine accepted is
    // listed back; a name the parent does not expose is simply not applied.
    TArray<TSharedPtr<FJsonValue>> AppliedRows;
    if (bHasOverrides)
    {
        for (const auto& Entry : (*Overrides)->Values)
        {
            if (!Entry.Value.IsValid())
            {
                continue;
            }
            const FString OverrideName = FString::ConstructFromPtrSize(*Entry.Key, Entry.Key.Len());
            const TCHAR* AppliedKind = UEAgentMaterialCommandsImpl::ApplyParameterOverride(Instance, OverrideName, *Entry.Value);
            if (AppliedKind == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> AppliedRow = MakeShared<FJsonObject>();
            AppliedRow->SetStringField(TEXT("name"), OverrideName);
            AppliedRow->SetStringField(TEXT("type"), AppliedKind);
            AppliedRows.Add(MakeShared<FJsonValueObject>(AppliedRow));
        }
        // Refresh the instance (static permutation included) before it is written.
        UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
        Instance->PostEditChange();
    }

    // Write it to disk right away: a dirty package alone would report success for an asset that
    // does not outlive the editor session.
    Instance->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Instance->GetOutermost(), Instance, SaveError);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("instance_path"), ObjectPath);
    Reply->SetStringField(TEXT("instance_name"), NewAssetName);
    Reply->SetStringField(TEXT("material_instance_path"), Instance->GetPathName());
    Reply->SetStringField(TEXT("material_instance_name"), Instance->GetName());
    // A parent given by path is echoed as sent; one found by bare name is reported by its resolved path.
    const FString ReportedParentPath = Parent == nullptr ? FString(TEXT("None"))
        : (ParentPath.Contains(TEXT("/")) ? ParentPath : Parent->GetPathName());
    Reply->SetStringField(TEXT("parent_material_path"), ReportedParentPath);
    Reply->SetStringField(TEXT("parent_material_name"), Parent != nullptr ? Parent->GetName() : FString(TEXT("None")));
    Reply->SetBoolField(TEXT("created"), true);
    Reply->SetBoolField(TEXT("already_existed"), false);
    Reply->SetArrayField(TEXT("applied_overrides"), AppliedRows);
    Reply->SetNumberField(TEXT("applied_overrides_count"), AppliedRows.Num());
    Reply->SetBoolField(TEXT("success"), true);
    mcp_common::AddSaveOutcome(Reply, bSaved, SaveError);
    return Reply;
}

// MCP:create_material_expression - Add ONE node to a material graph -- or to a material function's internal graph -- by expression class path (e.g. /Script/Engine.MaterialExpressionConstant, ...TextureSample, ...ScalarParameter), at a graph position. The building block of headless material authoring: create the nodes, then wire them with connect_material_expressions and connect_to_material_output (inside a function: connect_material_function_pins, then recompile_material_function). The node arrives UNCONNECTED, so nothing renders differently until it is wired -- verify with get_material_graph_info and get_material_stats, not with the success flag.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial'); a UMaterialFunction path is accepted too and creates the node inside that function
//   @param material_function_path - Alias of material_path that addresses a UMaterialFunction (or material layer): the node is created inside the function's graph
//   @param expression_class_name - Material expression class name (e.g., '/Script/Engine.MaterialExpressionConstant')
//   @param node_pos_x - Optional graph editor X position; defaults to 0
//   @param node_pos_y - Optional graph editor Y position; defaults to 0
//   @returns { material_path: string, material_function_path?: string (the function's object path, when the node went into a function), expression_class_name: string, expression_name: string, node_pos_x: int, node_pos_y: int, success: bool }

TSharedPtr<FJsonObject> HandleCreateMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    // material_path, else material_function_path; which key was used decides what is tried first.
    FString GraphPath;
    bool bFunctionKey = false;
    if (!Params->TryGetStringField(TEXT("material_path"), GraphPath))
    {
        if (!Params->TryGetStringField(TEXT("material_function_path"), GraphPath))
        {
            return mcp_common::MissingParameterReply(TEXT("material_path"));
        }
        bFunctionKey = true;
    }
    FString NodeClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("expression_class_name"), NodeClassName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // The graph owner: a UMaterial, or a UMaterialFunction (tried first when material_function_path was sent).
    UMaterial* Graph = nullptr;
    UMaterialFunction* FunctionGraph = nullptr;
    if (bFunctionKey)
    {
        FunctionGraph = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(GraphPath);
    }
    if (FunctionGraph == nullptr)
    {
        Graph = AssetPathUtils::LoadOrLoadSoftPathString<UMaterial>(GraphPath);
    }
    if (Graph == nullptr && FunctionGraph == nullptr && !bFunctionKey)
    {
        FunctionGraph = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(GraphPath);
    }
    if (Graph == nullptr && FunctionGraph == nullptr)
    {
        return UEAgentMaterialCommandsImpl::NotFoundByPath(TEXT("Material or material function"), GraphPath);
    }

    // In-memory lookup first; a class that is not loaded yet is only loaded when given as a path.
    UClass* NodeClass = FindObject<UClass>(nullptr, *NodeClassName);
    if (NodeClass == nullptr && NodeClassName.Contains(TEXT("/")))
    {
        NodeClass = LoadClass<UMaterialExpression>(nullptr, *NodeClassName);
    }
    if (NodeClass == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Material expression class not found: %s. Use full path like '/Script/Engine.MaterialExpressionConstant'"), *NodeClassName));
    }
    if (!NodeClass->IsChildOf(UMaterialExpression::StaticClass()))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Class is not a MaterialExpression: %s"), *NodeClassName));
    }

    // Graph-editor coordinates are optional and default to the origin.
    int32 GraphX = 0;
    int32 GraphY = 0;
    Params->TryGetNumberField(TEXT("node_pos_x"), GraphX);
    Params->TryGetNumberField(TEXT("node_pos_y"), GraphY);

    // Inside a function the function-scoped creator is used: it makes the function the node's owner,
    // so any expression class can be placed there, not only the function input/output nodes.
    UMaterialExpression* NewNode = FunctionGraph != nullptr
        ? UMaterialEditingLibrary::CreateMaterialExpressionInFunction(FunctionGraph, NodeClass, GraphX, GraphY)
        : UMaterialEditingLibrary::CreateMaterialExpression(Graph, NodeClass, GraphX, GraphY);
    if (NewNode == nullptr)
    {
        return mcp_common::MakeErrorReply(FunctionGraph != nullptr
            ? TEXT("Failed to create material expression in function")
            : TEXT("Failed to create material expression"));
    }
    if (FunctionGraph != nullptr)
    {
        FunctionGraph->MarkPackageDirty();
    }
    else
    {
        Graph->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), GraphPath);
    if (FunctionGraph != nullptr)
    {
        Reply->SetStringField(TEXT("material_function_path"), FunctionGraph->GetPathName());
    }
    Reply->SetStringField(TEXT("expression_class_name"), NodeClassName);
    Reply->SetStringField(TEXT("expression_name"), NewNode->GetName());
    Reply->SetNumberField(TEXT("node_pos_x"), GraphX);
    Reply->SetNumberField(TEXT("node_pos_y"), GraphY);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:delete_material_expression - Delete ONE node from a material graph by name. Deliberately one at a time, and that is the safe way to empty a graph: the engine's bulk delete_all_material_expressions can leave half the nodes behind with no error, and a rebuild on top then produces duplicate parameter names that no parameter listing reveals (duplicates collapse by name). Loop this instead, re-reading get_material_graph_info until its expression_count reaches 0. The underlying engine call also returns false while having done the work, so count the nodes rather than trusting the flag.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param expression_name - Name of the material expression to delete
//   @returns { material_path: string, expression_name: string, success: bool }

TSharedPtr<FJsonObject> HandleDeleteMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), GraphPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NodeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("expression_name"), NodeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Graph = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(GraphPath, Graph)) { return LoadError; }

    // The node collection of a material is not public API, so the node is found by walking the
    // live expression objects and keeping the one owned by this material.
    UMaterialExpression* Doomed = UEAgentMaterialCommandsImpl::FindGraphNode(Graph, NodeName);
    if (Doomed == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material expression not found: %s"), *NodeName));
    }

    UMaterialEditingLibrary::DeleteMaterialExpression(Graph, Doomed);
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), GraphPath);
    Reply->SetStringField(TEXT("expression_name"), NodeName);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:connect_material_expressions - Wire one material node's output pin into another node's input pin -- the call that turns loose expressions into a working graph, and the only headless route to some pins. Input names go WITHOUT spaces (BaseColor and AmbientOcclusion work, "Base Color" does not), and MakeMaterialAttributes accepts named pins the graph walker reports as unnamed, including Displacement. A true return only means the pin NAME resolved: confirm the connection landed by re-reading get_material_graph_info, then compile and check get_material_stats.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param from_expression_name - Name of the source material expression
//   @param from_output_name - Name of the output pin on the source expression
//   @param to_expression_name - Name of the destination material expression
//   @param to_input_name - Name of the input pin on the destination expression
//   @returns { material_path: string, from_expression_name: string, from_output_name: string, to_expression_name: string, to_input_name: string, success: bool }

TSharedPtr<FJsonObject> HandleConnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), GraphPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString SourceNodeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_expression_name"), SourceNodeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString SourcePin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_output_name"), SourcePin, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetNodeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_expression_name"), TargetNodeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetPin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_input_name"), TargetPin, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Graph = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(GraphPath, Graph)) { return LoadError; }

    // One walk over the material's nodes resolves both ends; it stops once both are known.
    UMaterialExpression* SourceNode = nullptr;
    UMaterialExpression* TargetNode = nullptr;
    UEAgentMaterialCommandsImpl::VisitGraphNodes(Graph, [&](UMaterialExpression* Node)
    {
        const FString NodeName = Node->GetName();
        if (NodeName == SourceNodeName)
        {
            SourceNode = Node;
        }
        if (NodeName == TargetNodeName)
        {
            TargetNode = Node;
        }
        return SourceNode == nullptr || TargetNode == nullptr;
    });

    if (SourceNode == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("From expression not found: %s"), *SourceNodeName));
    }
    if (TargetNode == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("To expression not found: %s"), *TargetNodeName));
    }

    // The engine call only tells whether both pin NAMES resolved; that is the failure surfaced here.
    if (!UMaterialEditingLibrary::ConnectMaterialExpressions(SourceNode, SourcePin, TargetNode, TargetPin))
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to connect material expressions. Check output and input names are correct."));
    }
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), GraphPath);
    Reply->SetStringField(TEXT("from_expression_name"), SourceNodeName);
    Reply->SetStringField(TEXT("from_output_name"), SourcePin);
    Reply->SetStringField(TEXT("to_expression_name"), TargetNodeName);
    Reply->SetStringField(TEXT("to_input_name"), TargetPin);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:disconnect_material_expressions - Disconnect an input of a material expression (inverse of connect_material_expressions).  Idempotent: an input that is already disconnected returns already_disconnected=true (still success — the expression exists, there simply was no connection to break).
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param to_expression_name - Name of the destination material expression whose input is disconnected
//   @param to_input_name - Name of the input pin to disconnect (empty = first input)
//   @returns { material_path: string, to_expression_name: string, to_input_name: string, disconnected: bool, already_disconnected: bool, success: bool }

TSharedPtr<FJsonObject> HandleDisconnectMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), GraphPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetNodeName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_expression_name"), TargetNodeName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetPin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_input_name"), TargetPin, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Same resolution as connect_material_expressions: direct load, soft-path retry, then the node.
    UMaterial* Graph = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(GraphPath, Graph)) { return LoadError; }
    UMaterialExpression* TargetNode = UEAgentMaterialCommandsImpl::FindGraphNode(Graph, TargetNodeName);
    if (TargetNode == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("To expression not found: %s"), *TargetNodeName));
    }

    // A false return means the pin had no link to break. The node exists, so that is reported as
    // already_disconnected instead of an error. No recompile here, matching the connect command.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    const bool bLinkBroken = UEAgentMaterialCommandsImpl::DisconnectMaterialExpressions57(TargetNode, TargetPin);
#else
    const bool bLinkBroken = UMaterialEditingLibrary::DisconnectMaterialExpressions(TargetNode, TargetPin);
#endif
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), GraphPath);
    Reply->SetStringField(TEXT("to_expression_name"), TargetNodeName);
    Reply->SetStringField(TEXT("to_input_name"), TargetPin);
    Reply->SetBoolField(TEXT("disconnected"), bLinkBroken);
    Reply->SetBoolField(TEXT("already_disconnected"), !bLinkBroken);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:set_material_physical_material - Link (or clear) the UPhysicalMaterial on a UMaterial asset and persist it to disk.  Idempotent: re-setting the same phys-mat returns already_set=true.  An empty/omitted physical_material_path CLEARS the link (sets it to null).
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param physical_material_path - Path to the UPhysicalMaterial asset to link, or "" / omitted to clear the link
//   @returns { material_path: string, physical_material_path: string, already_set: bool, saved: bool, save_error?: string, success: bool }

TSharedPtr<FJsonObject> HandleSetMaterialPhysicalMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(AssetPath, Material)) { return LoadError; }

    // Absent or "" means "clear the link"; anything else has to resolve to a physical material.
    FString SurfacePath;
    Params->TryGetStringField(TEXT("physical_material_path"), SurfacePath);
    UPhysicalMaterial* Surface = nullptr;
    if (!SurfacePath.IsEmpty())
    {
        Surface = mcp_common::LoadAssetOrSoftPath<UPhysicalMaterial>(SurfacePath);
        if (Surface == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Physical material not found: %s"), *SurfacePath));
        }
    }

    // Compare against the authored field itself; GetPhysicalMaterial() would substitute the engine default.
    const bool bAlreadySet = Material->PhysMaterial == Surface;
    Material->Modify();
    Material->PhysMaterial = Surface;
    Material->MarkPackageDirty();

    UE_LOG(LogTemp, Verbose, TEXT("MCP set_material_physical_material: %s -> %s (already_set=%d)"),
           *AssetPath, Surface ? *Surface->GetPathName() : TEXT("<null>"), bAlreadySet ? 1 : 0);

    FString SaveError;
    const bool bSaved = UEAgentMaterialCommandsImpl::SaveMaterialPackage(Material, SaveError);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), AssetPath);
    Reply->SetStringField(TEXT("physical_material_path"), Material->PhysMaterial ? Material->PhysMaterial->GetPathName() : FString());
    Reply->SetBoolField(TEXT("already_set"), bAlreadySet);
    mcp_common::AddSaveOutcome(Reply, bSaved, SaveError);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:get_material_physical_material - Which physical material a material is linked to -- what decides footstep sounds, impact effects, friction and surface type when something hits this surface. Reads the RAW field with no engine-default fallback, so an empty path means "nothing authored here" and the engine default applies at runtime; that distinction is exactly what you want when auditing why a surface sounds wrong. set_material_physical_material writes it.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @returns { material_path: string, physical_material_path: string, success: bool }

TSharedPtr<FJsonObject> HandleGetMaterialPhysicalMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(AssetPath, Material)) { return LoadError; }

    // The authored field, deliberately not GetPhysicalMaterial(): "" has to mean "nothing set here".
    const FString LinkedPath = Material->PhysMaterial ? Material->PhysMaterial->GetPathName() : FString();

    UE_LOG(LogTemp, Verbose, TEXT("MCP get_material_physical_material: %s -> %s"),
           *AssetPath, LinkedPath.IsEmpty() ? TEXT("<null>") : *LinkedPath);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), AssetPath);
    Reply->SetStringField(TEXT("physical_material_path"), LinkedPath);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:set_material_usage_flag - Enable or disable a usage flag (EMaterialUsage) on a UMaterial asset and persist it to disk.  Idempotent: the engine setter no-ops when the flag already matches (already_set=true).  Recompiles shaders for the affected usage.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param usage - Usage flag name, with or without the MATUSAGE_ prefix (e.g., 'SkeletalMesh' or 'MATUSAGE_SkeletalMesh')
//   @param enabled - Whether to enable (true, default) or disable (false) the flag
//   @returns { material_path: string, usage: string, enabled: bool, already_set: bool, saved: bool, save_error?: string, success: bool }

TSharedPtr<FJsonObject> HandleSetMaterialUsageFlag(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString UsageText;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("usage"), UsageText, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bWantEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bWantEnabled);

    // The usage name is validated before the material is even loaded.
    EMaterialUsage Usage = MATUSAGE_MAX;
    FString UsageLabel;
    if (!UEAgentMaterialCommandsImpl::ParseUsageFlag(UsageText, Usage, UsageLabel))
    {
        return UEAgentMaterialCommandsImpl::UnknownUsage(UsageText);
    }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(AssetPath, Material)) { return LoadError; }

    const bool bAlreadySet = Material->GetUsageByFlag(Usage) == bWantEnabled;
    Material->Modify();
    // The engine setter returns early by itself when the flag already holds the requested value.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
    UEAgentMaterialCommandsImpl::SetBaseMaterialUsage57(Material, Usage, bWantEnabled);
#else
    UMaterialEditingLibrary::SetBaseMaterialUsage(Material, Usage, bWantEnabled);
#endif
    Material->MarkPackageDirty();

    UE_LOG(LogTemp, Verbose, TEXT("MCP set_material_usage_flag: %s %s=%d (already_set=%d)"),
           *AssetPath, *UsageLabel, bWantEnabled ? 1 : 0, bAlreadySet ? 1 : 0);

    FString SaveError;
    const bool bSaved = UEAgentMaterialCommandsImpl::SaveMaterialPackage(Material, SaveError);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), AssetPath);
    Reply->SetStringField(TEXT("usage"), UsageLabel);
    Reply->SetBoolField(TEXT("enabled"), bWantEnabled);
    Reply->SetBoolField(TEXT("already_set"), bAlreadySet);
    mcp_common::AddSaveOutcome(Reply, bSaved, SaveError);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:get_material_usage_flag - Is this material allowed to be used on that kind of geometry? Reads one EMaterialUsage flag (SkeletalMesh, InstancedStaticMeshes, Niagara, SplineMesh, ...) on a base material. The check to make when a material renders fine on a static mesh and comes out as the default grey checker on a skeletal mesh or an instanced mesh -- a missing usage flag is exactly that symptom. Accepts the name with or without the MATUSAGE_ prefix; set_material_usage_flag enables it (and forces a shader recompile).
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param usage - Usage flag name, with or without the MATUSAGE_ prefix (e.g., 'SkeletalMesh' or 'MATUSAGE_SkeletalMesh')
//   @returns { material_path: string, usage: string, enabled: bool, success: bool }

TSharedPtr<FJsonObject> HandleGetMaterialUsageFlag(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString UsageText;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("usage"), UsageText, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    EMaterialUsage Usage = MATUSAGE_MAX;
    FString UsageLabel;
    if (!UEAgentMaterialCommandsImpl::ParseUsageFlag(UsageText, Usage, UsageLabel))
    {
        return UEAgentMaterialCommandsImpl::UnknownUsage(UsageText);
    }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(AssetPath, Material)) { return LoadError; }

    const bool bFlagOn = Material->GetUsageByFlag(Usage);
    UE_LOG(LogTemp, Verbose, TEXT("MCP get_material_usage_flag: %s %s=%d"),
           *AssetPath, *UsageLabel, bFlagOn ? 1 : 0);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), AssetPath);
    Reply->SetStringField(TEXT("usage"), UsageLabel);
    Reply->SetBoolField(TEXT("enabled"), bFlagOn);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:get_material_referencers - What would I break if I changed this material: every asset that references it, bucketed by kind (static mesh, skeletal mesh, blueprint, level, other) with counts, plus the material's own blend mode and two-sided / masked / translucent / opaque flags. The impact check before editing or deleting a shared material. Note it takes a material NAME, not a path, so an ambiguous name can resolve to the wrong asset -- confirm with resolve_asset_by_name first. Nothing to do with get_material_usage_flag, which is about permitted geometry types.
//   @param material_name - Name of the material asset
//   @returns { material_name: string, material_class: string, material_category: string, material_tooltip: string, material_display_name: string, usage: [{package_name, asset_name, asset_class, asset_path, usage_type, usage_category, asset_category, asset_tooltip, asset_display_name}], usage_count: int, static_mesh_usage_count: int, skeletal_mesh_usage_count: int, blueprint_usage_count: int, level_usage_count: int, other_usage_count: int, material_has_referencers: bool, material_is_two_sided: bool, material_is_masked: bool, material_is_translucent: bool, material_is_opaque: bool, blend_mode: string }

TSharedPtr<FJsonObject> HandleGetMaterialUsage(const TSharedPtr<FJsonObject>& Params)
{
    FString LookupName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_name"), LookupName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (LookupName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'material_name' cannot be empty"));
    }

    IAssetRegistry& Registry = mcp_common::AssetRegistry();
    FAssetData MaterialAsset;
    if (!UEAgentMaterialCommandsImpl::FindMaterialAssetByName(Registry, LookupName, MaterialAsset))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material not found: %s"), *LookupName));
    }

    // Every package the registry knows to depend on the material's package.
    TArray<FName> ReferencingPackages;
    Registry.GetReferencers(MaterialAsset.PackageName, ReferencingPackages);

    UEAgentMaterialCommandsImpl::FReferencerTally Tally;
    TArray<TSharedPtr<FJsonValue>> UsageRows;
    for (const FName& ReferencingPackage : ReferencingPackages)
    {
        UsageRows.Add(MakeShared<FJsonValueObject>(
            UEAgentMaterialCommandsImpl::DescribeReferencer(Registry, ReferencingPackage, Tally)));
    }

    UMaterialInterface* Material = Cast<UMaterialInterface>(MaterialAsset.GetAsset());

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_name"), LookupName);
    UEAgentMaterialCommandsImpl::WriteMaterialClassInfo(Reply, Material);
    Reply->SetArrayField(TEXT("usage"), UsageRows);
    Reply->SetNumberField(TEXT("usage_count"), UsageRows.Num());
    Tally.WriteTo(Reply);
    // "Used" is answered by the registry: a material counts as used when anything references it.
    Reply->SetBoolField(TEXT("material_has_referencers"), ReferencingPackages.Num() > 0);
    UEAgentMaterialCommandsImpl::WriteSurfaceTraits(Reply, Material);
    return Reply;
}

// MCP:get_material_stats - Read PER-ASSET shader complexity for a material or material instance (instruction counts, sampler/texture-sample counts) -- the numbers the Material Editor's Stats panel shows, unlike get_shader_info (project-wide/global). Read-only: does not Modify()/dirty the asset. Render-adjacent: forces synchronous shader compilation on the FIRST call for a given material (UMaterialEditingLibrary::GetStatistics blocks on FMaterialResource::FinishCompilation); subsequent calls are fast once the shader map is cached. Works for both UMaterial and UMaterialInstance (UMaterialInterface's GetMaterialResource is virtual/overridden on both).
//   @param material_path - Full path to the material or material instance (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @returns { material_path, material_class, num_vertex_shader_instructions, num_pixel_shader_instructions, num_samplers, num_vertex_texture_samples, num_pixel_texture_samples, num_virtual_texture_samples, num_uv_scalars, num_interpolator_scalars, compile_errors: string[], stats_valid: bool }
TSharedPtr<FJsonObject> HandleGetMaterialStats(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterialInterface* Target = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialInterface>(AssetPath);
    if (Target == nullptr)
    {
        return UEAgentMaterialCommandsImpl::NotFoundByPath(TEXT("Material"), AssetPath);
    }

    // The statistics come from the game-thread shader map, and a material never compiled for this
    // platform just reads as zeros. A process that cannot render never compiles shaders at all, so
    // it gets an explicit refusal rather than a page of zeros.
    if (!FApp::CanEverRender() || GShaderCompilingManager == nullptr)
    {
        return mcp_common::MakeErrorReply(
            TEXT("get_material_stats requires shader compilation (real RHI); this process cannot render (-nullrhi/server)"));
    }

    // Queue the recompile in Background mode and then block on the compiling manager: draining the
    // queue is the step that assigns the finished maps to the material. A Synchronous recompile runs
    // outside that queue, and the game-thread map was observed to stay unassigned afterwards.
    Target->ForceRecompileForRendering(EMaterialShaderPrecompileMode::Background);
    GShaderCompilingManager->FinishAllCompilation();

    const FMaterialResource* Resource = Target->GetMaterialResource(GMaxRHIShaderPlatform);
    const bool bShaderMapComplete = Resource != nullptr
        && Resource->GetGameThreadShaderMap() != nullptr
        && Resource->IsGameThreadShaderMapComplete();

    const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Target);

    TArray<TSharedPtr<FJsonValue>> ErrorList;
    if (Resource != nullptr)
    {
        ErrorList = mcp_common::StringsToJson(Resource->GetCompileErrors());
    }

    struct FStatField
    {
        const TCHAR* Name;
        int32 Value;
    };
    const FStatField StatFields[] = {
        { TEXT("num_vertex_shader_instructions"), Stats.NumVertexShaderInstructions },
        { TEXT("num_pixel_shader_instructions"), Stats.NumPixelShaderInstructions },
        { TEXT("num_samplers"), Stats.NumSamplers },
        { TEXT("num_vertex_texture_samples"), Stats.NumVertexTextureSamples },
        { TEXT("num_pixel_texture_samples"), Stats.NumPixelTextureSamples },
        { TEXT("num_virtual_texture_samples"), Stats.NumVirtualTextureSamples },
        { TEXT("num_uv_scalars"), Stats.NumUVScalars },
        { TEXT("num_interpolator_scalars"), Stats.NumInterpolatorScalars },
    };

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), AssetPath);
    Reply->SetStringField(TEXT("material_class"), Target->GetClass()->GetName());
    for (const FStatField& Field : StatFields)
    {
        Reply->SetNumberField(Field.Name, Field.Value);
    }
    // Valid only with a COMPLETE shader map and no compile errors; NumSamplers >= 0 alone is not
    // enough, since it also holds for the all-zero reading taken off a missing resource.
    const bool bHasCompileErrors = ErrorList.Num() > 0;
    Reply->SetArrayField(TEXT("compile_errors"), ErrorList);
    Reply->SetBoolField(TEXT("stats_valid"), bShaderMapComplete && !bHasCompileErrors && Stats.NumSamplers >= 0);
    return Reply;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("connect_material_expressions"), &HandleConnectMaterialExpressions },
    { TEXT("create_material"), &HandleCreateMaterial },
    { TEXT("create_material_expression"), &HandleCreateMaterialExpression },
    { TEXT("create_material_instance"), &HandleCreateMaterialInstance },
    { TEXT("delete_material_expression"), &HandleDeleteMaterialExpression },
    { TEXT("disconnect_material_expressions"), &HandleDisconnectMaterialExpressions },
    { TEXT("get_material_physical_material"), &HandleGetMaterialPhysicalMaterial },
    { TEXT("get_material_stats"), &HandleGetMaterialStats },
    { TEXT("get_material_referencers"), &HandleGetMaterialUsage },
    { TEXT("get_material_usage_flag"), &HandleGetMaterialUsageFlag },
    { TEXT("set_material_physical_material"), &HandleSetMaterialPhysicalMaterial },
    { TEXT("set_material_usage_flag"), &HandleSetMaterialUsageFlag },
};
} // namespace UEAgentMaterialCommands
