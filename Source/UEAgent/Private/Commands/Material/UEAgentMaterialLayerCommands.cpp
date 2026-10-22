// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "Editor.h"

#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "RenderingThread.h"                                  // FlushRenderingCommands

#include "SceneTypes.h"                                       // EMaterialProperty / MP_MaterialAttributes
#include "Materials/Material.h"                               // UMaterial
#include "Materials/MaterialExpressionMaterialAttributeLayers.h" // UMaterialExpressionMaterialAttributeLayers
#include "MaterialEditingLibrary.h"                           // UMaterialEditingLibrary (MaterialEditor module)

/**
 * Material Layer authoring commands. (Layer and layer-blend function assets are created by
 * create_material_function with usage MaterialLayer / MaterialLayerBlend, and read back by
 * get_material_function_info, in the material extra family.)
 *
 * add_material_attribute_layers_node adds a UMaterialExpressionMaterialAttributeLayers
 * node onto an existing UMaterial (via UMaterialEditingLibrary::CreateMaterialExpression),
 * wires it to the material's MaterialAttributes output, and recompiles the material.
 * The recompile is wrapped in FlushRenderingCommands() before/after to avoid the
 * render-thread crash gotcha.
 */
namespace UEAgentMaterialLayerCommands
{

// MCP-DOMAIN: Material

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
// MCP:add_material_attribute_layers_node - Adds a new MaterialAttributeLayers expression to a UMaterial asset, turns on bUseMaterialAttributes (the separate pins such as Base Color stop driving the material), wires the node into MaterialAttributes, recompiles and saves. Not idempotent: each call adds another node. Fails when the save fails; returns connected_material_attributes and num_expressions. Unwire: disconnect_pin node_id=@output, pin_name=MaterialAttributes.
//   @param material_path - Object path to the UMaterial, e.g. /Game/MyGame/M_Example
//   @param node_name - Label echoed back in the reply only; it does not name or find any node in the graph
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

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_material_attribute_layers_node"), &HandleAddMaterialAttributeLayersNode },
};
} // namespace UEAgentMaterialLayerCommands
