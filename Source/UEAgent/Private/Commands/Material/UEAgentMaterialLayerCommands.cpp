#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"
#include "RenderingThread.h"                                  // FlushRenderingCommands

#include "SceneTypes.h"                                       // EMaterialProperty / MP_MaterialAttributes
#include "Materials/Material.h"                               // UMaterial
#include "Materials/MaterialFunction.h"                       // UMaterialFunction (GetInputsAndOutputs / SetMaterialFunctionUsage / bExposeToLibrary)
#include "Materials/MaterialFunctionInterface.h"              // EMaterialFunctionUsage
#include "Materials/MaterialFunctionMaterialLayer.h"          // UMaterialFunctionMaterialLayer
#include "Materials/MaterialFunctionMaterialLayerBlend.h"     // UMaterialFunctionMaterialLayerBlend
#include "Materials/MaterialExpressionMaterialAttributeLayers.h" // UMaterialExpressionMaterialAttributeLayers
#include "Materials/MaterialExpressionMaterialFunctionCall.h" // FFunctionExpressionInput / FFunctionExpressionOutput (GetInputsAndOutputs)
#include "MaterialEditingLibrary.h"                           // UMaterialEditingLibrary (MaterialEditor module)

/**
 * Material Layer / Blend authoring commands.
 *
 * Authors persisted material-layer assets entirely headless, the way the
 * engine's editor factories do (EditorFactories.cpp):
 *   - UMaterialFunctionMaterialLayerFactory       -> NewObject<UMaterialFunctionMaterialLayer>
 *                                                    + SetMaterialFunctionUsage(MaterialLayer)
 *   - UMaterialFunctionMaterialLayerBlendFactory  -> NewObject<UMaterialFunctionMaterialLayerBlend>
 *                                                    + SetMaterialFunctionUsage(MaterialLayerBlend)
 * Neither factory overrides ConfigureProperties(), so the NewObject path is
 * fully headless-safe (no modal picker). The asset is registered with the asset
 * registry and saved to disk.
 *
 * add_material_attribute_layers_node adds a UMaterialExpressionMaterialAttributeLayers
 * node onto an existing UMaterial (via UMaterialEditingLibrary::CreateMaterialExpression),
 * wires it to the material's MaterialAttributes output, and recompiles the material.
 * The recompile is wrapped in FlushRenderingCommands() before/after to avoid the
 * render-thread crash gotcha.
 *
 * get_material_layer_info loads a layer/blend function back and reports its class,
 * usage, input/output counts and is_layer/is_blend flags.
 */
namespace UEAgentMaterialLayerCommands
{

// MCP-DOMAIN: Material

namespace
{
    /** Normalize a package folder path: strip trailing slash. */
    FString MLNormalizeFolder(const FString& In)
    {
        FString Out = In;
        Out.RemoveFromEnd(TEXT("/"));
        return Out;
    }

    /** Map an EMaterialFunctionUsage to a stable label. */
    FString UsageToString(EMaterialFunctionUsage Usage)
    {
        switch (Usage)
        {
            case EMaterialFunctionUsage::MaterialLayer:      return TEXT("MaterialLayer");
            case EMaterialFunctionUsage::MaterialLayerBlend: return TEXT("MaterialLayerBlend");
            default:                                         return TEXT("Default");
        }
    }

    /** The fields a layer-like create reports for Function; bCreated tells whether this call made it. */
    TSharedPtr<FJsonObject> MaterialLayer_CreateReply(UMaterialFunctionInterface* Function, const FString& Name, bool bCreated)
    {
        TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
        Resp->SetStringField(TEXT("asset_path"), Function->GetPathName());
        Resp->SetStringField(TEXT("name"), Name);
        Resp->SetStringField(TEXT("class"), Function->GetClass()->GetName());
        Resp->SetStringField(TEXT("usage"), UsageToString(Function->GetMaterialFunctionUsage()));
        Resp->SetBoolField(TEXT("created"), bCreated);
        Resp->SetBoolField(TEXT("already_existed"), !bCreated);
        return Resp;
    }

    /**
     * Create + persist a specialized material function (layer or blend), replicating
     * exactly what the editor factory does (NewObject + SetMaterialFunctionUsage).
     * Idempotent: if the asset already exists, load it back and return the same
     * stable payload (created=false, already_existed=true).
     */
    TSharedPtr<FJsonObject> CreateLayerLike(
        const TSharedPtr<FJsonObject>& Params,
        UClass* AssetClass,
        EMaterialFunctionUsage Usage,
        const FString& ClassLabel)
    {
        FString PackagePath, Name;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (!PackagePath.StartsWith(TEXT("/")))
            return mcp_common::MakeErrorReply(TEXT("package_path must be a UE long path, e.g. /Game/Foo"));

        const FString Folder = MLNormalizeFolder(PackagePath);
        const FString PackageName = FString::Printf(TEXT("%s/%s"), *Folder, *Name);
        if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
        const FString FullObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);

        // Idempotent: existing asset -> load + return a stable payload.
        if (UEditorAssetLibrary::DoesAssetExist(PackageName))
        {
            UMaterialFunctionInterface* ExistingFn =
                LoadObject<UMaterialFunctionInterface>(nullptr, *FullObjectPath);
            if (ExistingFn)
            {
                return mcp_common::MakeOkReply(MaterialLayer_CreateReply(ExistingFn, Name, /*bCreated=*/false));
            }
        }

        // Create the package + the function object (exactly what the factory's
        // FactoryCreateNew does), then tag its usage.
        if (const FString AssetConflict = mcp_common::AssetNameConflict(PackageName, Name, AssetClass); !AssetConflict.IsEmpty())
            return mcp_common::MakeErrorReply(AssetConflict);
        UPackage* Package = CreatePackage(*PackageName);
        if (!Package)
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create package: %s"), *PackageName));

        UMaterialFunction* Function = NewObject<UMaterialFunction>(
            Package, AssetClass, *Name,
            RF_Public | RF_Standalone | RF_Transactional);
        if (!Function)
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to create %s asset"), *ClassLabel));

        Function->SetMaterialFunctionUsage(Usage);
        // Layer/blend functions are exposed to the material function library by default
        // (matches the editor New Asset behaviour for these specialized functions).
        Function->bExposeToLibrary = true;

        FAssetRegistryModule::AssetCreated(Function);
        Function->MarkPackageDirty();

        FString SaveErr;
        const bool bSaved = mcp_common::SaveAssetPackage(Package, Function, SaveErr);

        TSharedPtr<FJsonObject> Resp = MaterialLayer_CreateReply(Function, Name, /*bCreated=*/true);
        mcp_common::AddSaveOutcome(Resp, bSaved, SaveErr);
        return mcp_common::MakeOkReply(Resp);
    }
}

// ---------------------------------------------------------------------------
// create_material_layer
// ---------------------------------------------------------------------------
// MCP:create_material_layer - Create and save a UMaterialFunctionMaterialLayer asset
//   @param package_path - UE content folder, e.g. /Game/MyGame
//   @param name - Asset name for the new material layer function
//   @returns { asset_path: string, name: string, class: string, usage: string, created: bool, already_existed: bool, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleCreateMaterialLayer(const TSharedPtr<FJsonObject>& Params)
{
    return CreateLayerLike(
        Params,
        UMaterialFunctionMaterialLayer::StaticClass(),
        EMaterialFunctionUsage::MaterialLayer,
        TEXT("UMaterialFunctionMaterialLayer"));
}

// ---------------------------------------------------------------------------
// create_material_layer_blend
// ---------------------------------------------------------------------------
// MCP:create_material_layer_blend - Create and save a UMaterialFunctionMaterialLayerBlend asset
//   @param package_path - UE content folder, e.g. /Game/MyGame
//   @param name - Asset name for the new material layer blend function
//   @returns { asset_path: string, name: string, class: string, usage: string, created: bool, already_existed: bool, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleCreateMaterialLayerBlend(const TSharedPtr<FJsonObject>& Params)
{
    return CreateLayerLike(
        Params,
        UMaterialFunctionMaterialLayerBlend::StaticClass(),
        EMaterialFunctionUsage::MaterialLayerBlend,
        TEXT("UMaterialFunctionMaterialLayerBlend"));
}

namespace UEAgentMaterialLayerImpl
{
    // Loads a UMaterial from a path: a direct LoadObject first and, if that finds nothing and the text
    // parses as a valid soft object path, a soft-path load cast to UMaterial. nullptr when both fail.
    UMaterial* LoadMaterialByPath(const FString& Path)
    {
        if (UMaterial* Direct = LoadObject<UMaterial>(nullptr, *Path))
        {
            return Direct;
        }
        const FSoftObjectPath AsSoftPath(Path);
        return AsSoftPath.IsValid() ? Cast<UMaterial>(AsSoftPath.TryLoad()) : nullptr;
    }
}

// ---------------------------------------------------------------------------
// add_material_attribute_layers_node
// ---------------------------------------------------------------------------
// MCP:add_material_attribute_layers_node - Add a MaterialAttributeLayers expression node to an existing UMaterial
//   @param material_path - Object path to the UMaterial, e.g. /Game/MyGame/M_Example
//   @param node_name - Optional advisory node name
//   @returns { material_path: string, expression_added: bool, expression_class: string, connected_material_attributes: bool, use_material_attributes: bool, num_expressions: int, recompiled: bool, saved: bool, node_name: string, save_error: string }
TSharedPtr<FJsonObject> HandleAddMaterialAttributeLayersNode(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Material = UEAgentMaterialLayerImpl::LoadMaterialByPath(MaterialPath);
    if (!Material)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not load UMaterial at: %s"), *MaterialPath));

    // Advisory label only: it is echoed back when given and does not name anything in the graph.
    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);

    // Create the MaterialAttributeLayers expression on the material's graph.
    UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(
        Material,
        UMaterialExpressionMaterialAttributeLayers::StaticClass(),
        /*NodePosX*/ -400, /*NodePosY*/ 0);
    if (!Expr)
        return mcp_common::MakeErrorReply(
            TEXT("CreateMaterialExpression returned null for MaterialAttributeLayers"));

    // Switch the material to attribute-driven output and wire the layer stack's
    // default (index 0) output into the MaterialAttributes property.
    Material->bUseMaterialAttributes = true;
    const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(
        Expr, FString(), MP_MaterialAttributes);

    // Recompile so the change is reflected. Wrap in render-command flushes to avoid
    // a render-thread race while shaders for the material are rebuilt.
    FlushRenderingCommands();
    UMaterialEditingLibrary::RecompileMaterial(Material);
    FlushRenderingCommands();

    const int32 NumExpr = UMaterialEditingLibrary::GetNumMaterialExpressions(Material);

    // Persist the edited material package.
    FString SaveErr;
    const bool bSaved = mcp_common::SaveAssetPackage(Material->GetOutermost(), Material, SaveErr);

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("material_path"), Material->GetPathName());
    Resp->SetBoolField(TEXT("expression_added"), true);
    Resp->SetStringField(TEXT("expression_class"), Expr->GetClass()->GetName());
    Resp->SetBoolField(TEXT("connected_material_attributes"), bConnected);
    Resp->SetBoolField(TEXT("use_material_attributes"), Material->bUseMaterialAttributes);
    Resp->SetNumberField(TEXT("num_expressions"), NumExpr);
    Resp->SetBoolField(TEXT("recompiled"), true);
    if (!NodeName.IsEmpty())
    {
        Resp->SetStringField(TEXT("node_name"), NodeName);
    }
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_material_attribute_layers_node"), SaveErr);
}

// ---------------------------------------------------------------------------
// get_material_layer_info
// ---------------------------------------------------------------------------
// MCP:get_material_layer_info - Read a material layer or blend function's metadata
//   @param layer_path - Object path to a UMaterialFunctionMaterialLayer or UMaterialFunctionMaterialLayerBlend, e.g. /Game/MyGame/M_Layer_Example
//   @returns { asset_path: string, class: string, usage: string, num_inputs: int, num_outputs: int, num_expressions: int, is_layer: bool, is_blend: bool }
TSharedPtr<FJsonObject> HandleGetMaterialLayerInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString LayerPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("layer_path"), LayerPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterialFunctionInterface* Fn = LoadObject<UMaterialFunctionInterface>(nullptr, *LayerPath);
    if (!Fn)
    {
        FSoftObjectPath SoftPath(LayerPath);
        if (SoftPath.IsValid())
        {
            Fn = Cast<UMaterialFunctionInterface>(SoftPath.TryLoad());
        }
    }
    if (!Fn)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not load material function (layer/blend) at: %s"), *LayerPath));

    TArray<FFunctionExpressionInput> Inputs;
    TArray<FFunctionExpressionOutput> Outputs;
    Fn->GetInputsAndOutputs(Inputs, Outputs);

    const EMaterialFunctionUsage Usage = Fn->GetMaterialFunctionUsage();
    const bool bIsLayer = Fn->IsA<UMaterialFunctionMaterialLayer>() ||
                          Usage == EMaterialFunctionUsage::MaterialLayer;
    const bool bIsBlend = Fn->IsA<UMaterialFunctionMaterialLayerBlend>() ||
                          Usage == EMaterialFunctionUsage::MaterialLayerBlend;

    int32 NumExpressions = 0;
    if (UMaterialFunction* AsFunction = Cast<UMaterialFunction>(Fn))
    {
        NumExpressions = AsFunction->GetExpressions().Num();
    }

    TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
    Resp->SetStringField(TEXT("asset_path"), Fn->GetPathName());
    Resp->SetStringField(TEXT("class"), Fn->GetClass()->GetName());
    Resp->SetStringField(TEXT("usage"), UsageToString(Usage));
    Resp->SetNumberField(TEXT("num_inputs"), Inputs.Num());
    Resp->SetNumberField(TEXT("num_outputs"), Outputs.Num());
    Resp->SetNumberField(TEXT("num_expressions"), NumExpressions);
    Resp->SetBoolField(TEXT("is_layer"), bIsLayer);
    Resp->SetBoolField(TEXT("is_blend"), bIsBlend);
    return mcp_common::MakeOkReply(Resp);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_material_attribute_layers_node"), &HandleAddMaterialAttributeLayersNode },
    { TEXT("create_material_layer"), &HandleCreateMaterialLayer },
    { TEXT("create_material_layer_blend"), &HandleCreateMaterialLayerBlend },
    { TEXT("get_material_layer_info"), &HandleGetMaterialLayerInfo },
};
} // namespace UEAgentMaterialLayerCommands
