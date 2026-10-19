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

    /** Which typed default a get_material_default_parameter request asks for. */
    enum class EDefaultValueKind : uint8
    {
        Scalar,
        Vector,
        Texture,
        StaticSwitch,
        Unsupported
    };

    /** Case-insensitive param_type -> EDefaultValueKind. */
    EDefaultValueKind ParseDefaultValueKind(const FString& Text)
    {
        struct FKindName
        {
            const TCHAR* Name;
            EDefaultValueKind Kind;
        };
        static const FKindName KnownKinds[] = {
            { TEXT("scalar"), EDefaultValueKind::Scalar },
            { TEXT("vector"), EDefaultValueKind::Vector },
            { TEXT("texture"), EDefaultValueKind::Texture },
            { TEXT("static_switch"), EDefaultValueKind::StaticSwitch },
        };
        for (const FKindName& Candidate : KnownKinds)
        {
            if (Text.Equals(Candidate.Name, ESearchCase::IgnoreCase))
            {
                return Candidate.Kind;
            }
        }
        return EDefaultValueKind::Unsupported;
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

    /** {r, g, b, a} for a linear colour. */
    TSharedPtr<FJsonObject> ColorToJson(const FLinearColor& Color)
    {
        TSharedPtr<FJsonObject> Channels = mcp_common::LinearColorToJsonObject(Color);
        return Channels;
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

    /** How many parameters of each kind a material exposes. */
    struct FParameterCensus
    {
        int32 Scalars = 0;
        int32 Vectors = 0;
        int32 Textures = 0;
        int32 StaticSwitches = 0;
    };

    using FParameterInfoGetter = void (UMaterialInterface::*)(TArray<FMaterialParameterInfo>&, TArray<FGuid>&) const;

    int32 CountParameterInfos(const UMaterialInterface* Material, FParameterInfoGetter Getter)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Guids;
        (Material->*Getter)(Infos, Guids);
        return Infos.Num();
    }

    FParameterCensus TakeParameterCensus(const UMaterialInterface* Material)
    {
        FParameterCensus Census;
        Census.Scalars = CountParameterInfos(Material, &UMaterialInterface::GetAllScalarParameterInfo);
        Census.Vectors = CountParameterInfos(Material, &UMaterialInterface::GetAllVectorParameterInfo);
        Census.Textures = CountParameterInfos(Material, &UMaterialInterface::GetAllTextureParameterInfo);
        Census.StaticSwitches = CountParameterInfos(Material, &UMaterialInterface::GetAllStaticSwitchParameterInfo);
        return Census;
    }

    /** A variant suffix becomes part of an asset name: it may not be empty or hold separators. */
    bool IsUsableVariantSuffix(const FString& Suffix)
    {
        return !Suffix.IsEmpty()
            && !Suffix.Contains(TEXT(" "))
            && !Suffix.Contains(TEXT("."))
            && !Suffix.Contains(TEXT("/"));
    }

    /**
     * Applies one parameter_overrides entry: a number sets a scalar, a bool a static switch and an
     * object {r, g, b[, a]} a vector (alpha defaults to 1). Returns the kind that was applied, or
     * nullptr when the value has another JSON type or the engine refused the parameter.
     */
    const TCHAR* ApplyParameterOverride(UMaterialInstanceConstant* Instance, const FString& ParamName, const FJsonValue& Value)
    {
        switch (Value.Type)
        {
        case EJson::Number:
        {
            const float Scalar = static_cast<float>(Value.AsNumber());
            return UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, FName(*ParamName), Scalar)
                ? TEXT("scalar") : nullptr;
        }
        case EJson::Boolean:
            return UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, FName(*ParamName), Value.AsBool())
                ? TEXT("static_switch") : nullptr;
        case EJson::Object:
        {
            const TSharedPtr<FJsonObject> Channels = Value.AsObject();
            if (!Channels.IsValid())
            {
                return nullptr;
            }
            const float Red = static_cast<float>(Channels->GetNumberField(TEXT("r")));
            const float Green = static_cast<float>(Channels->GetNumberField(TEXT("g")));
            const float Blue = static_cast<float>(Channels->GetNumberField(TEXT("b")));
            const float Alpha = Channels->HasField(TEXT("a")) ? static_cast<float>(Channels->GetNumberField(TEXT("a"))) : 1.0f;
            return UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, FName(*ParamName), FLinearColor(Red, Green, Blue, Alpha))
                ? TEXT("vector") : nullptr;
        }
        default:
            return nullptr;
        }
    }
}

// MCP:create_material - Create a new base material asset (UMaterial) -- the graph-owning master whose parameters and static switches every instance of it inherits. Prefer create_material_instance / create_material_instance_constant when you only need a re-skin of something that exists: an instance costs no new shader permutations and leaves the original updatable. After creating, build the graph with create_material_expression + connect_material_expressions, then verify with get_material_stats (compile_errors empty and stats_valid true) -- compile_material only acknowledges, it does not report errors, and a landscape material that fails to compile silently falls back to the default material.
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

// MCP:create_material_instance - Create a material instance (MIC) from a parent material -- the cheap, non-destructive way to re-skin a vendor or engine material: the parent stays untouched and updatable while only your overrides are stored, and no new shader is compiled. Idempotent (already_exists=true when it is already there). Same operation as create_material_instance_constant, which takes a single full asset path instead of separate folder and name. Tune it with set_material_instance_param_override, and remember static switches inherit from the parent and are set separately.
//   @param instance_path - Package path for the material instance (e.g., '/Game/Materials/MyInstance')
//   @param instance_name - Name of the material instance to create
//   @param parent_material_path - Full path to the parent material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @returns { instance_path: string, instance_name: string, parent_material?: string, parent_material_path: string, parent_material_name: string, success: bool, already_exists?: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateMaterialInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString TargetFolder;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_path"), TargetFolder, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NewAssetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_name"), NewAssetName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ParentPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_material_path"), ParentPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterialInterface* Parent = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialInterface>(ParentPath);
    if (Parent == nullptr)
    {
        return UEAgentMaterialCommandsImpl::NotFoundByPath(TEXT("Parent material"), ParentPath);
    }

    // CreateAsset names the package <folder>/<name>, so the new object lives at <folder>/<name>.<name>.
    const FString ObjectPath = FString::Printf(TEXT("%s/%s.%s"), *TargetFolder, *NewAssetName, *NewAssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(ObjectPath)) { return Invalid; }
    if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
    {
        // Idempotent create: an instance that is already there is reported, not recreated.
        const UMaterialInstanceConstant* Existing = LoadObject<UMaterialInstanceConstant>(nullptr, *ObjectPath);
        if (Existing == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance already exists at path: %s"), *ObjectPath));
        }
        TSharedPtr<FJsonObject> ExistingInfo = MakeShared<FJsonObject>();
        ExistingInfo->SetStringField(TEXT("instance_path"), ObjectPath);
        ExistingInfo->SetStringField(TEXT("instance_name"), NewAssetName);
        ExistingInfo->SetStringField(TEXT("parent_material"), Existing->Parent ? Existing->Parent->GetPathName() : FString());
        ExistingInfo->SetBoolField(TEXT("already_exists"), true);
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

    // Write it to disk right away (as create_material_instance_constant does): a dirty package
    // alone would report success for an asset that does not outlive the editor session.
    Instance->MarkPackageDirty();
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Instance->GetOutermost(), Instance, SaveError);

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("instance_path"), ObjectPath);
    Reply->SetStringField(TEXT("instance_name"), NewAssetName);
    Reply->SetStringField(TEXT("parent_material_path"), ParentPath);
    Reply->SetStringField(TEXT("parent_material_name"), Parent->GetName());
    Reply->SetBoolField(TEXT("success"), true);
    mcp_common::AddSaveOutcome(Reply, bSaved, SaveError);
    return Reply;
}

// MCP:create_material_expression - Add ONE node to a material graph, by expression class path (e.g. /Script/Engine.MaterialExpressionConstant, ...TextureSample, ...ScalarParameter), at a graph position. The building block of headless material authoring: create the nodes, then wire them with connect_material_expressions and connect_to_material_output. The node arrives UNCONNECTED, so nothing renders differently until it is wired -- verify with get_material_expressions and get_material_stats, not with the success flag.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param expression_class_name - Material expression class name (e.g., '/Script/Engine.MaterialExpressionConstant')
//   @param node_pos_x - Optional graph editor X position; defaults to 0
//   @param node_pos_y - Optional graph editor Y position; defaults to 0
//   @returns { material_path: string, expression_class_name: string, expression_name: string, node_pos_x: int, node_pos_y: int, success: bool }

TSharedPtr<FJsonObject> HandleCreateMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), GraphPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NodeClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("expression_class_name"), NodeClassName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Graph = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(GraphPath, Graph)) { return LoadError; }

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

    UMaterialExpression* NewNode = UMaterialEditingLibrary::CreateMaterialExpression(Graph, NodeClass, GraphX, GraphY);
    if (NewNode == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create material expression"));
    }
    Graph->MarkPackageDirty();

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), GraphPath);
    Reply->SetStringField(TEXT("expression_class_name"), NodeClassName);
    Reply->SetStringField(TEXT("expression_name"), NewNode->GetName());
    Reply->SetNumberField(TEXT("node_pos_x"), GraphX);
    Reply->SetNumberField(TEXT("node_pos_y"), GraphY);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:delete_material_expression - Delete ONE node from a material graph by name. Deliberately one at a time, and that is the safe way to empty a graph: the engine's bulk delete_all_material_expressions can leave half the nodes behind with no error, and a rebuild on top then produces duplicate parameter names that no parameter listing reveals (duplicates collapse by name). Loop this instead, re-reading get_material_expressions until the count reaches 0. The underlying engine call also returns false while having done the work, so count the nodes rather than trusting the flag.
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

// MCP:connect_material_expressions - Wire one material node's output pin into another node's input pin -- the call that turns loose expressions into a working graph, and the only headless route to some pins. Input names go WITHOUT spaces (BaseColor and AmbientOcclusion work, "Base Color" does not), and MakeMaterialAttributes accepts named pins the graph walker reports as unnamed, including Displacement. A true return only means the pin NAME resolved: confirm the connection landed by re-reading get_material_expressions / get_material_graph_info, then compile and check get_material_stats.
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
    const bool bLinkBroken = UMaterialEditingLibrary::DisconnectMaterialExpressions(TargetNode, TargetPin);
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

// MCP:get_material_expressions - List every node in a material's graph with its name, class and description -- the census that tells you what a material is built from, and the count oracle when deleting nodes one by one (delete until it reaches 0). Also the way to spot the duplicate parameter nodes a partial bulk-delete leaves behind, which no parameter listing shows because duplicates collapse by name. Nodes only: for the WIRING between them use get_material_graph_info, and note this works on a UMaterial, not a UMaterialFunction (the Python equivalent throws on one).
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @returns { material_path: string, material_name: string, expressions: [{ name, class, class_path, description }], expression_count: int, success: bool }

TSharedPtr<FJsonObject> HandleGetMaterialExpressions(const TSharedPtr<FJsonObject>& Params)
{
    FString GraphPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), GraphPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Graph = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(GraphPath, Graph)) { return LoadError; }

    TArray<TSharedPtr<FJsonValue>> NodeList;
    UEAgentMaterialCommandsImpl::VisitGraphNodes(Graph, [&NodeList](UMaterialExpression* Node)
    {
        const UClass* NodeClass = Node->GetClass();
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Node->GetName());
        Entry->SetStringField(TEXT("class"), NodeClass->GetName());
        Entry->SetStringField(TEXT("class_path"), NodeClass->GetPathName());
        Entry->SetStringField(TEXT("description"), Node->GetDescription());
        NodeList.Add(MakeShared<FJsonValueObject>(Entry));
        return true;
    });

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_path"), GraphPath);
    Reply->SetStringField(TEXT("material_name"), Graph->GetName());
    Reply->SetNumberField(TEXT("expression_count"), NodeList.Num());
    Reply->SetArrayField(TEXT("expressions"), NodeList);
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
    UMaterialEditingLibrary::SetBaseMaterialUsage(Material, Usage, bWantEnabled);
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

// MCP:get_material_default_parameter - Read the RESOLVED default value of a named parameter on the base UMaterial graph, with an honest found flag.  Pairs with set_material_node_param (ParameterName + DefaultValue) which is a writer-without-reader.  The engine getters return a default (0 / black / null / false) BOTH when the parameter holds that value AND when it does not exist; this command uses the bool-returning UMaterial::Get*ParameterDefaultValue family to distinguish them, returning found=false (NOT a masquerading default) when the parameter is absent.
//   @param material_path - Full path to the material (e.g., '/Game/Materials/MyMaterial.MyMaterial')
//   @param param_name - Name of the parameter to resolve (the ScalarParameter/VectorParameter/etc. ParameterName)
//   @param param_type - Parameter type to read: 'scalar' | 'vector' | 'texture' | 'static_switch'
//   @returns { material_path: string, param_name: string, param_type: string, found: bool, value?: (float | {r,g,b,a} | string texture_path | bool), success: bool }

TSharedPtr<FJsonObject> HandleGetMaterialDefaultParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), AssetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ParamName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ParamType;
    if (!Params->TryGetStringField(TEXT("param_type"), ParamType))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: param_type. Expected one of: scalar | vector | texture | static_switch"));
    }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> LoadError = UEAgentMaterialCommandsImpl::LoadBaseMaterial(AssetPath, Material)) { return LoadError; }

    // FHashedMaterialParameterInfo converts implicitly from an FName, so the bare name addresses
    // the global (non-layer) parameter.
    const FName ParamKey(*ParamName);

    using UEAgentMaterialCommandsImpl::EDefaultValueKind;
    const EDefaultValueKind Kind = UEAgentMaterialCommandsImpl::ParseDefaultValueKind(ParamType);
    if (Kind == EDefaultValueKind::Unsupported)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown param_type: %s. Expected one of: scalar | vector | texture | static_switch"), *ParamType));
    }

    // Each getter's bool is the honest "found" flag: it tells a default that happens to be zero /
    // black / null / false apart from a parameter that does not exist at all.
    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    bool bFound = false;
    switch (Kind)
    {
    case EDefaultValueKind::Scalar:
    {
        float Scalar = 0.0f;
        bFound = Material->GetScalarParameterDefaultValue(ParamKey, Scalar);
        if (bFound)
        {
            Reply->SetNumberField(TEXT("value"), Scalar);
        }
        break;
    }
    case EDefaultValueKind::Vector:
    {
        FLinearColor Color = FLinearColor::Black;
        bFound = Material->GetVectorParameterDefaultValue(ParamKey, Color);
        if (bFound)
        {
            Reply->SetObjectField(TEXT("value"), UEAgentMaterialCommandsImpl::ColorToJson(Color));
        }
        break;
    }
    case EDefaultValueKind::Texture:
    {
        UTexture* Texture = nullptr;
        bFound = Material->GetTextureParameterDefaultValue(ParamKey, Texture);
        if (bFound)
        {
            // A texture parameter may legitimately default to no texture at all; that reads as "".
            Reply->SetStringField(TEXT("value"), Texture ? Texture->GetPathName() : FString());
        }
        break;
    }
    case EDefaultValueKind::StaticSwitch:
    {
        bool bSwitch = false;
        FGuid SwitchGuid;
        bFound = Material->GetStaticSwitchParameterDefaultValue(ParamKey, bSwitch, SwitchGuid);
        if (bFound)
        {
            Reply->SetBoolField(TEXT("value"), bSwitch);
        }
        break;
    }
    default:
        break;
    }

    UE_LOG(LogTemp, Verbose, TEXT("MCP get_material_default_parameter: %s '%s' (%s) found=%d"),
           *AssetPath, *ParamName, *ParamType, bFound ? 1 : 0);

    Reply->SetStringField(TEXT("material_path"), AssetPath);
    Reply->SetStringField(TEXT("param_name"), ParamName);
    Reply->SetStringField(TEXT("param_type"), ParamType);
    Reply->SetBoolField(TEXT("found"), bFound);
    Reply->SetBoolField(TEXT("success"), true);
    return Reply;
}

// MCP:get_material_usage - What would I break if I changed this material: every asset that references it, bucketed by kind (static mesh, skeletal mesh, blueprint, level, other) with counts, plus the material's own blend mode and two-sided / masked / translucent / opaque flags. The impact check before editing or deleting a shared material. Note it takes a material NAME, not a path, so an ambiguous name can resolve to the wrong asset -- confirm with resolve_asset_by_name first. Nothing to do with get_material_usage_flag, which is about permitted geometry types.
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

// MCP:create_material_variant - Create a material instance from a base material AND apply a dict of parameter overrides in the same call -- one shot for "same material, different colour/roughness/switch", instead of create_material_instance followed by N set calls. The overrides dict accepts scalars, vector objects and static switch booleans. Addresses the base by NAME rather than path, so resolve an ambiguous name first; check applied_overrides_count against what you sent, and re-read with get_material_all_parameters, since a name that does not exist on the parent is simply not applied.
//   @param material_name - Name of the base material asset
//   @param variant_name - Name for the new variant (must not be empty and cannot contain spaces, dots, or slashes)
//   @param parameter_overrides - Optional dict {param_name: scalar_number | vector_object | static_switch_bool} applied to the variant
//   @returns { material_name: string, variant_name: string, variant_path: string, variant_asset_name: string, variant_class: string, parent_material: string, success: bool, variant_saved: bool, applied_overrides_count: int, applied_overrides: [{name, type}], message: string, material_class: string, material_category: string, material_tooltip: string, material_display_name: string, parameters_count: int, scalar_parameters_count: int, vector_parameters_count: int, texture_parameters_count: int, static_switch_parameters_count: int, material_is_two_sided: bool, material_is_masked: bool, material_is_translucent: bool, material_is_opaque: bool, blend_mode: string }

TSharedPtr<FJsonObject> HandleCreateMaterialVariant(const TSharedPtr<FJsonObject>& Params)
{
    FString BaseName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_name"), BaseName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString Suffix;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("variant_name"), Suffix, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (BaseName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'material_name' cannot be empty"));
    }
    if (Suffix.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'variant_name' cannot be empty"));
    }

    IAssetRegistry& Registry = mcp_common::AssetRegistry();
    FAssetData BaseAsset;
    if (!UEAgentMaterialCommandsImpl::FindMaterialAssetByName(Registry, BaseName, BaseAsset))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material not found: %s"), *BaseName));
    }
    UMaterialInterface* Parent = Cast<UMaterialInterface>(BaseAsset.GetAsset());
    if (Parent == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to load material: %s"), *BaseName));
    }
    if (!UEAgentMaterialCommandsImpl::IsUsableVariantSuffix(Suffix))
    {
        return mcp_common::MakeErrorReply(TEXT("Invalid variant name. Must not be empty and cannot contain spaces, dots, or slashes"));
    }

    // The variant is named "<material>_<variant>"; refuse when a package of that name is registered.
    const FString VariantAssetName = FString::Printf(TEXT("%s_%s"), *BaseName, *Suffix);
    TArray<FAssetData> NameClashes;
    Registry.GetAssetsByPackageName(FName(*VariantAssetName), NameClashes);
    if (NameClashes.Num() > 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material variant already exists: %s"), *VariantAssetName));
    }

    // Parameter census of the parent, reported alongside the variant.
    const UEAgentMaterialCommandsImpl::FParameterCensus Census = UEAgentMaterialCommandsImpl::TakeParameterCensus(Parent);

    // The variant is a UMaterialInstanceConstant saved next to its parent (under /Game when the
    // parent's folder is unknown), the same kind of asset create_material_instance makes.
    FString TargetFolder = BaseAsset.PackagePath.ToString();
    if (TargetFolder.IsEmpty())
    {
        TargetFolder = TEXT("/Game");
    }
    const FString VariantObjectPath = FString::Printf(TEXT("%s/%s.%s"), *TargetFolder, *VariantAssetName, *VariantAssetName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(VariantObjectPath)) { return Invalid; }
    if (UEditorAssetLibrary::DoesAssetExist(VariantObjectPath))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material variant already exists: %s"), *VariantObjectPath));
    }

    IAssetTools& Tools = UEAgentMaterialCommandsImpl::AssetToolsApi();
    UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
    if (Factory == nullptr)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create UMaterialInstanceConstantFactoryNew"));
    }
    Factory->InitialParent = Parent;

    UMaterialInstanceConstant* Variant = Cast<UMaterialInstanceConstant>(
        Tools.CreateAsset(VariantAssetName, TargetFolder, UMaterialInstanceConstant::StaticClass(), Factory));
    if (Variant == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create material variant asset at: %s"), *VariantObjectPath));
    }
    // CreateAsset already honours InitialParent; setting it again keeps the parent link explicit.
    UMaterialEditingLibrary::SetMaterialInstanceParent(Variant, Parent);

    // Optional overrides {name: number | bool | {r,g,b[,a]}}. Only what the engine accepted is
    // listed back; a name the parent does not expose is simply not applied.
    TArray<TSharedPtr<FJsonValue>> AppliedRows;
    const TSharedPtr<FJsonObject>* Overrides = nullptr;
    if (Params->TryGetObjectField(TEXT("parameter_overrides"), Overrides) && Overrides != nullptr && Overrides->IsValid())
    {
        for (const auto& Entry : (*Overrides)->Values)
        {
            if (!Entry.Value.IsValid())
            {
                continue;
            }
            const FString OverrideName = FString::ConstructFromPtrSize(*Entry.Key, Entry.Key.Len());
            const TCHAR* AppliedKind = UEAgentMaterialCommandsImpl::ApplyParameterOverride(Variant, OverrideName, *Entry.Value);
            if (AppliedKind == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> AppliedRow = MakeShared<FJsonObject>();
            AppliedRow->SetStringField(TEXT("name"), OverrideName);
            AppliedRow->SetStringField(TEXT("type"), AppliedKind);
            AppliedRows.Add(MakeShared<FJsonValueObject>(AppliedRow));
        }
    }

    // Refresh the instance, then write it to disk.
    UMaterialEditingLibrary::UpdateMaterialInstance(Variant);
    Variant->PostEditChange();
    Variant->MarkPackageDirty();
    const bool bVariantSaved = UEditorAssetLibrary::SaveLoadedAsset(Variant, /*bOnlyIfIsDirty=*/false);
    const bool bVariantExists = Variant != nullptr;

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetStringField(TEXT("material_name"), BaseName);
    Reply->SetStringField(TEXT("variant_name"), Suffix);
    Reply->SetStringField(TEXT("variant_path"), VariantObjectPath);
    Reply->SetStringField(TEXT("variant_asset_name"), VariantAssetName);
    Reply->SetStringField(TEXT("variant_class"), Variant->GetClass()->GetName());
    Reply->SetStringField(TEXT("parent_material"), Parent->GetName());
    Reply->SetBoolField(TEXT("success"), bVariantExists);
    Reply->SetBoolField(TEXT("variant_saved"), bVariantSaved);
    Reply->SetNumberField(TEXT("applied_overrides_count"), AppliedRows.Num());
    Reply->SetArrayField(TEXT("applied_overrides"), AppliedRows);
    Reply->SetStringField(TEXT("message"), bVariantSaved
        ? TEXT("Material variant created and saved successfully")
        : TEXT("Material variant created but failed to save to disk"));
    UEAgentMaterialCommandsImpl::WriteMaterialClassInfo(Reply, Parent);
    Reply->SetNumberField(TEXT("parameters_count"), Census.Scalars + Census.Vectors + Census.Textures + Census.StaticSwitches);
    Reply->SetNumberField(TEXT("scalar_parameters_count"), Census.Scalars);
    Reply->SetNumberField(TEXT("vector_parameters_count"), Census.Vectors);
    Reply->SetNumberField(TEXT("texture_parameters_count"), Census.Textures);
    Reply->SetNumberField(TEXT("static_switch_parameters_count"), Census.StaticSwitches);
    UEAgentMaterialCommandsImpl::WriteSurfaceTraits(Reply, Parent);
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
    { TEXT("create_material_variant"), &HandleCreateMaterialVariant },
    { TEXT("delete_material_expression"), &HandleDeleteMaterialExpression },
    { TEXT("disconnect_material_expressions"), &HandleDisconnectMaterialExpressions },
    { TEXT("get_material_default_parameter"), &HandleGetMaterialDefaultParameter },
    { TEXT("get_material_expressions"), &HandleGetMaterialExpressions },
    { TEXT("get_material_physical_material"), &HandleGetMaterialPhysicalMaterial },
    { TEXT("get_material_stats"), &HandleGetMaterialStats },
    { TEXT("get_material_usage"), &HandleGetMaterialUsage },
    { TEXT("get_material_usage_flag"), &HandleGetMaterialUsageFlag },
    { TEXT("set_material_physical_material"), &HandleSetMaterialPhysicalMaterial },
    { TEXT("set_material_usage_flag"), &HandleSetMaterialUsageFlag },
};
} // namespace UEAgentMaterialCommands
