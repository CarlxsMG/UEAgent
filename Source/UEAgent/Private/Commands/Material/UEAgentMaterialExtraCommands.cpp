#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Material/AssetPathUtils.h"

#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionConstant4Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionVectorParameter.h"
// set_material_instance_parameter re-reads through UMaterialEditingLibrary's value
// getters, all of which take an EMaterialParameterAssociation.
#include "Materials/MaterialParameters.h"
#include "Engine/Texture.h"
#include "SceneTypes.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"            // FProperty import/export (set_material_expression_property)
#include "Misc/StringOutputDevice.h"       // captures ImportText_Direct's own error text
#include "RenderingThread.h"   // FlushRenderingCommands — serialize material recompile vs render thread

// Material functions / post-process materials
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "MaterialDomain.h"
#include "Engine/BlendableInterface.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Factories/MaterialFactoryNew.h"
#include "UObject/SavePackage.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"

// MaterialFunction authoring advanced
#include "Materials/MaterialFunctionInterface.h"     // UMaterialFunctionInterface (parent)
#include "Materials/MaterialFunctionInstance.h"      // UMaterialFunctionInstance + SetParent
#include "Factories/MaterialFunctionInstanceFactory.h" // UMaterialFunctionInstanceFactoryNew

// SubsurfaceProfile data asset.
#include "Engine/SubsurfaceProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"                    // get_material_function_info soft-path load
#include "Materials/MaterialFunctionMaterialLayer.h"      // create_material_function usage MaterialLayer
#include "Materials/MaterialFunctionMaterialLayerBlend.h" // create_material_function usage MaterialLayerBlend

/** Additional material commands: connect an expression to a material output property, set
 *  expression/instance parameters, recompile a material and set its domain, plus material functions
 *  (authoring, instances, internal graph), post-process materials, subsurface profiles and material
 *  graph introspection. */
namespace UEAgentMaterialExtraCommands
{

// MCP-DOMAIN: Material

namespace
{
    UMaterialExpression* FindExpressionByName(UMaterial* Material, const FString& NodeId)
    {
        for (TObjectIterator<UMaterialExpression> It; It; ++It)
        {
            UMaterialExpression* Expression = *It;
            if (Expression && Expression->GetOuter() == Material && Expression->GetName() == NodeId)
            {
                return Expression;
            }
        }
        return nullptr;
    }

    bool ResolveMaterialProperty(const FString& PropertyName, EMaterialProperty& Out)
    {
        const FString P = PropertyName.ToLower();
        if (P == TEXT("basecolor") || P == TEXT("base_color")) { Out = MP_BaseColor; return true; }
        if (P == TEXT("metallic")) { Out = MP_Metallic; return true; }
        if (P == TEXT("specular")) { Out = MP_Specular; return true; }
        if (P == TEXT("roughness")) { Out = MP_Roughness; return true; }
        if (P == TEXT("anisotropy")) { Out = MP_Anisotropy; return true; }
        if (P == TEXT("normal")) { Out = MP_Normal; return true; }
        if (P == TEXT("tangent")) { Out = MP_Tangent; return true; }
        if (P == TEXT("emissive") || P == TEXT("emissivecolor") || P == TEXT("emissive_color")) { Out = MP_EmissiveColor; return true; }
        if (P == TEXT("opacity")) { Out = MP_Opacity; return true; }
        if (P == TEXT("opacitymask") || P == TEXT("opacity_mask")) { Out = MP_OpacityMask; return true; }
        if (P == TEXT("worldpositionoffset") || P == TEXT("wpo")) { Out = MP_WorldPositionOffset; return true; }
        if (P == TEXT("ambientocclusion") || P == TEXT("ao")) { Out = MP_AmbientOcclusion; return true; }
        if (P == TEXT("refraction")) { Out = MP_Refraction; return true; }
        if (P == TEXT("pixeldepthoffset")) { Out = MP_PixelDepthOffset; return true; }
        if (P == TEXT("subsurfacecolor") || P == TEXT("subsurface")) { Out = MP_SubsurfaceColor; return true; }
        return false;
    }

    // Defined further down, next to set_material_instance_parameter, its first user.
    bool ParseLinearColorForMaterialInstanceParam(
        const FString& In, FLinearColor& Out, FString& OutFormat, FString& OutError);

    // Writes Text into Property of Expression, all or nothing: the text is parsed into a scratch
    // copy of the current value, and only a value that parsed is copied onto the node.
    //  - FLinearColor (the main value of Constant3Vector, Constant4Vector and VectorParameter)
    //    goes through ParseLinearColorForMaterialInstanceParam, which refuses what it cannot
    //    read; FLinearColor::InitFromString would turn that into black without a word.
    //  - Numbers go through LexTryParseString: ImportText_Direct cannot fail on a float
    //    property ("abc" imports as 0 and returns a non-null cursor).
    //  - Every other type goes through ImportText_Direct, whose null return means rejected.
    bool ImportExpressionProperty(UMaterialExpression* Expression, FProperty* Property, const FString& Text, FString& OutError)
    {
        void* Dest = Property->ContainerPtrToValuePtr<void>(Expression);
        void* Scratch = Property->AllocateAndInitializeValue();
        Property->CopyCompleteValue(Scratch, Dest);

        bool bParsed = false;
        const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
        const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property);
        if (StructProperty && StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
        {
            FLinearColor Color;
            FString Format;
            bParsed = ParseLinearColorForMaterialInstanceParam(Text, Color, Format, OutError);
            if (bParsed)
            {
                *static_cast<FLinearColor*>(Scratch) = Color;
            }
        }
        else if (NumericProperty && !NumericProperty->IsEnum())
        {
            const FString Trimmed = Text.TrimStartAndEnd();
            if (NumericProperty->IsFloatingPoint())
            {
                double Number = 0.0;
                bParsed = LexTryParseString(Number, *Trimmed);
                if (bParsed)
                {
                    NumericProperty->SetFloatingPointPropertyValue(Scratch, Number);
                }
            }
            else
            {
                int64 Number = 0;
                bParsed = LexTryParseString(Number, *Trimmed);
                if (bParsed)
                {
                    NumericProperty->SetIntPropertyValue(Scratch, Number);
                }
            }
            if (!bParsed)
            {
                OutError = FString::Printf(TEXT("'%s' is not a number (property %s is %s)"),
                    *Text, *Property->GetName(), *Property->GetCPPType());
            }
        }
        else
        {
            FStringOutputDevice ImportErrors;
            bParsed = Property->ImportText_Direct(*Text, Scratch, Expression, PPF_None, &ImportErrors) != nullptr;
            if (!bParsed)
            {
                OutError = ImportErrors.IsEmpty()
                    ? FString::Printf(TEXT("'%s' is not a valid value for property %s (%s)"), *Text, *Property->GetName(), *Property->GetCPPType())
                    : FString::Printf(TEXT("'%s' rejected by property %s (%s): %s"), *Text, *Property->GetName(), *Property->GetCPPType(), *ImportErrors);
            }
        }

        if (bParsed)
        {
            Expression->Modify();
            Property->CopyCompleteValue(Dest, Scratch);
        }
        Property->DestroyAndFreeValue(Scratch);
        return bParsed;
    }
}

namespace UEAgentMaterialExtraImpl
{
    // The material at MaterialPath; the error reply when it does not load.
    TSharedPtr<FJsonObject> FindMaterial(const FString& MaterialPath, UMaterial*& OutMaterial)
    {
        OutMaterial = AssetPathUtils::LoadOrLoadSoftPathString<UMaterial>(MaterialPath);
        if (!OutMaterial)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
        }
        return nullptr;
    }

    // The material function at FunctionPath; the error reply when it does not load.
    TSharedPtr<FJsonObject> FindFunction(const FString& FunctionPath, UMaterialFunction*& OutFunction)
    {
        OutFunction = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(FunctionPath);
        if (!OutFunction)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material function not found: %s"), *FunctionPath));
        }
        return nullptr;
    }
}

// MCP:connect_to_material_output - Wire one output of an expression node into a main input of a UMaterial asset (BaseColor, Roughness, Normal...). The edit stays in memory: no recompile, no dirty flag, no save, so follow with compile_material and save_asset. Node-to-node wiring is connect_material_expressions.
//   @param material_path - UMaterial asset path, e.g. "/Game/Materials/M_Rock"; a material instance path fails with Material not found.
//   @param from_node_id - Id of the source expression (from create_material_expression / get_material_graph_info).
//   @param output_property - Material input to feed, case-insensitive: BaseColor, Metallic, Specular, Roughness, Anisotropy, Normal, Tangent, EmissiveColor (or Emissive), Opacity, OpacityMask, WorldPositionOffset (WPO), AmbientOcclusion (AO), Refraction, PixelDepthOffset, SubsurfaceColor.
//   @param from_pin - Output pin name on the source node, e.g. "RGB" or "R"; omitted or empty uses its first output.
//   @returns { material: string, from_node_id: string, output_property: string, connected: bool }

TSharedPtr<FJsonObject> HandleConnectToMaterialOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath, FromNodeId, OutputProperty, FromPin;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_node_id"), FromNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_property"), OutputProperty, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("from_pin"), FromPin);

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    UMaterialExpression* FromExpression = FindExpressionByName(Material, FromNodeId);
    if (!FromExpression)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Expression not found: %s"), *FromNodeId));

    EMaterialProperty Property;
    if (!ResolveMaterialProperty(OutputProperty, Property))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown output_property: %s"), *OutputProperty));

    const bool bConnected = UMaterialEditingLibrary::ConnectMaterialProperty(FromExpression, FromPin, Property);
    if (!bConnected)
        return mcp_common::MakeErrorReply(TEXT("ConnectMaterialProperty failed (incompatible output or property)"));

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetStringField(TEXT("from_node_id"), FromNodeId);
    Result->SetStringField(TEXT("output_property"), OutputProperty);
    Result->SetBoolField(TEXT("connected"), true);
    return Result;
}

// MCP:disconnect_to_material_output - Disconnect a material output property (inverse of connect_to_material_output).  Idempotent: an already-disconnected property returns already_disconnected=true (still success — the output slot always resolves to a valid input, so false is unambiguously "nothing was connected", never an error).
//   @param material_path - Material asset path.
//   @param output_property - BaseColor, Metallic, Roughness, Normal, Emissive, Opacity, OpacityMask, Specular, ... (same strings as connect_to_material_output)
//   @returns { material: string, output_property: string, disconnected: bool, already_disconnected: bool }

TSharedPtr<FJsonObject> HandleDisconnectToMaterialOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath, OutputProperty;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_property"), OutputProperty, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    EMaterialProperty Property;
    if (!ResolveMaterialProperty(OutputProperty, Property))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown output_property: %s"), *OutputProperty));

    // DisconnectMaterialProperty returns false when the property input was already
    // disconnected. The enum always resolves to a valid material input, so false is
    // unambiguously "nothing was connected" (already_disconnected), NOT an error.
    const bool bDisconnected = UMaterialEditingLibrary::DisconnectMaterialProperty(Material, Property);

    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetStringField(TEXT("output_property"), OutputProperty);
    Result->SetBoolField(TEXT("disconnected"), bDisconnected);
    Result->SetBoolField(TEXT("already_disconnected"), !bDisconnected);
    return Result;
}

// MCP:set_material_expression_property - Write one property of an expression node of a UMaterial asset (not an instance), found by its object name node_id. param_name names the property on every node type (R on Constant, DefaultValue or SliderMax on ScalarParameter, Constant on Constant3Vector and Constant4Vector, DefaultValue on VectorParameter...); ParameterName renames a node that has a parameter name. A value that does not parse for the property is refused and nothing is written. No undo, no save: it runs PostEditChange and marks the material dirty (persist with save_asset). value_after is the property read back. Instances: set_material_instance_parameter.
//   @param material_path - Material asset path
//   @param node_id - Id of the target expression in the material
//   @param param_name - Name of the property to set (e.g. "ParameterName", "R", "DefaultValue")
//   @param value - Text to write: a number for a numeric property, "(R=1,G=0.5,B=0,A=1)", "[1,0.5,0,1]" or "1,0.5,0" for a linear color, the new name for ParameterName, else UE import text for the property; omitted means empty text.
//   @returns { material: string, node_id: string, param_name: string, value: string, value_after: string }

TSharedPtr<FJsonObject> HandleSetMaterialNodeParam(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath, NodeId, ParamName, Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("value"), Value);

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    UMaterialExpression* Expr = FindExpressionByName(Material, NodeId);
    if (!Expr)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Expression not found: %s"), *NodeId));

    FString ValueAfter;
    if (ParamName.Equals(TEXT("ParameterName"), ESearchCase::IgnoreCase) && Expr->HasAParameterName())
    {
        // SetParameterName is the node's own way to rename its parameter. An empty name would
        // leave the parameter named None.
        const FString NewName = Value.TrimStartAndEnd();
        if (NewName.IsEmpty())
            return mcp_common::MakeErrorReply(TEXT("value must be the new parameter name; an empty name is refused"));
        Material->Modify();
        Expr->Modify();
        Expr->SetParameterName(FName(*NewName));
        ValueAfter = Expr->GetParameterName().ToString();
    }
    else
    {
        // Every other name is a property of the node, found by reflection, so param_name always
        // decides what is written.
        FProperty* Property = FindFProperty<FProperty>(Expr->GetClass(), FName(*ParamName));
        if (!Property)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Expression %s (%s) has no property '%s'"),
                *NodeId, *Expr->GetClass()->GetName(), *ParamName));

        // Parse first, write after: a value the property rejects leaves the node untouched.
        FString ImportError;
        if (!ImportExpressionProperty(Expr, Property, Value, ImportError))
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not set '%s' on expression %s: %s"),
                *ParamName, *NodeId, *ImportError));
        Material->Modify();
        Property->ExportText_Direct(ValueAfter, Property->ContainerPtrToValuePtr<void>(Expr), nullptr, Expr, PPF_None);
    }

    Expr->PostEditChange();
    Material->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("param_name"), ParamName);
    Result->SetStringField(TEXT("value"), Value);
    Result->SetStringField(TEXT("value_after"), ValueAfter);
    return Result;
}

// ===========================================================================
// set_material_instance_parameter -- two engine traps in one tool
//
// (1) The setter's return value is meaningless. The four
//     UMaterialEditingLibrary::SetMaterialInstance*ParameterValue functions are written
//     like this (MaterialEditingLibrary.cpp):
//         bool bResult = false;
//         if (Instance) { Instance->Set...EditorOnly(...); UpdateMaterialInstance(...); }
//         return bResult;              // <-- bResult is NEVER assigned
//     i.e. the engine ALWAYS returns false, whether it worked or not -- the reverse of the
//     usual `success:true` lie, and worse, because it invites repeating the operation or
//     declaring a working feature broken. => `set` is derived from a RE-READ through the
//     value getters (which do return the value), never from the setter's return.
//
// (2) An unvalidated vector FORMAT writes ZEROS IN SILENCE. FLinearColor::InitFromString
//     parses with FParse::Value(TEXT("R="), ...) and, on failure, leaves the color at
//     (0,0,0,1) while returning false: "0.17,0.18,0.24,0.5" would write black. => the sane
//     formats are parsed EXPLICITLY, and anything else FAILS LOUDLY naming the formats it
//     accepts.
// ===========================================================================

namespace
{
    // Parse the `value` of a VECTOR material-instance parameter. Accepts,
    // explicitly and verifiably, the three forms a caller actually writes:
    //   "(R=0.17,G=0.18,B=0.24,A=0.5)"  -- UE struct export text (the only one
    //                                      FLinearColor::InitFromString handles)
    //   "[0.17,0.18,0.24,0.5]"          -- JSON array, bracketed
    //   "0.17,0.18,0.24"                -- bare CSV (A defaults to 1)
    // The CSV/array forms are parsed component-by-component HERE instead of being
    // handed to InitFromString, which collapses them to (0,0,0,1) and says nothing.
    // Every component must be numeric: that check is the whole point -- it is what
    // turns "silently wrote black" into an error message.
    bool ParseLinearColorForMaterialInstanceParam(
        const FString& In, FLinearColor& Out, FString& OutFormat, FString& OutError)
    {
        const FString AcceptedForms = TEXT(
            "accepted vector formats: UE struct '(R=..,G=..,B=..,A=..)', "
            "JSON array '[r,g,b,a]' or '[r,g,b]', or bare CSV 'r,g,b,a' / 'r,g,b' (A defaults to 1)");

        FString S = In.TrimStartAndEnd();
        if (S.IsEmpty())
        {
            OutError = FString::Printf(TEXT("empty vector value; %s"), *AcceptedForms);
            return false;
        }

        // Form 1: UE struct export text. This is the ONLY form InitFromString can
        // read, and its bool return is honest -- so we honor it instead of ignoring it.
        if (S.StartsWith(TEXT("(")))
        {
            FLinearColor LC;
            if (LC.InitFromString(S))
            {
                Out = LC;
                OutFormat = TEXT("ue_struct");
                return true;
            }
            OutError = FString::Printf(
                TEXT("value '%s' looks like a UE struct but FLinearColor::InitFromString rejected it "
                     "(it needs R=/G=/B= keys). Writing it anyway would store (0,0,0,1). %s"),
                *In, *AcceptedForms);
            return false;
        }

        // Forms 2 and 3: bracketed JSON array or bare CSV.
        FString Body = S;
        bool bBracketed = false;
        if (Body.StartsWith(TEXT("[")) && Body.EndsWith(TEXT("]")))
        {
            Body.MidInline(1, Body.Len() - 2);
            Body.TrimStartAndEndInline();
            bBracketed = true;
        }

        TArray<FString> Parts;
        // CullEmpty=false on purpose: "1,2,," must be an ERROR, not a silently
        // truncated 2-component color.
        Body.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty*/ false);
        for (FString& Part : Parts)
        {
            Part.TrimStartAndEndInline();
        }

        if (Parts.Num() != 3 && Parts.Num() != 4)
        {
            OutError = FString::Printf(
                TEXT("value '%s' has %d component(s); a vector needs 3 or 4. %s"),
                *In, Parts.Num(), *AcceptedForms);
            return false;
        }
        for (int32 i = 0; i < Parts.Num(); ++i)
        {
            // IsNumeric accepts a leading sign, digits and ONE dot (no exponent).
            // Anything else -- an empty slot, "R=0.17", "1e-3" -- fails here rather
            // than becoming a zero nobody asked for.
            if (Parts[i].IsEmpty() || !Parts[i].IsNumeric())
            {
                OutError = FString::Printf(
                    TEXT("value '%s' component %d ('%s') is not a plain number. %s"),
                    *In, i, *Parts[i], *AcceptedForms);
                return false;
            }
        }

        Out.R = FCString::Atof(*Parts[0]);
        Out.G = FCString::Atof(*Parts[1]);
        Out.B = FCString::Atof(*Parts[2]);
        Out.A = (Parts.Num() == 4) ? FCString::Atof(*Parts[3]) : 1.0f;
        OutFormat = bBracketed ? TEXT("json_array") : TEXT("csv");
        return true;
    }
}

// MCP:set_material_instance_parameter - Set a scalar, vector, texture or static-switch global parameter on a Material Instance Constant asset and re-read it: set comes from the re-read (the engine setter always returns false), plus value_after and overridden_after. Malformed vectors are rejected. Marks the package dirty; does not save.
//   @param instance_path - Asset path of the UMaterialInstanceConstant, e.g. "/Game/Materials/MI_Rock".
//   @param param_name - Parameter name as defined in the parent material, e.g. "BaseTint"; only global parameters are reached, not layer parameters.
//   @param value - Value. Scalar: a number (JSON number or numeric string). Vector: '(R=..,G=..,B=..,A=..)', '[r,g,b,a]', '[r,g,b]', 'r,g,b,a', 'r,g,b', or a real JSON array of 3-4 numbers; anything else is REJECTED (never written as black). Texture: an asset path. Static switch: a bool or 'true'/'false'.
//   @param param_type - "scalar" | "vector" | "texture" | "static_switch" (optional, inferred from the value: a JSON array => vector, a JSON bool => static_switch, a JSON number => scalar, a string starting with '/' => texture, with '(' or '[' => vector, else scalar. A bool passed AS A STRING still reads as scalar, so pass param_type explicitly in that case)
//   @returns { instance: string, instance_path: string, param_name: string, param_type: string, value_format?: string, set: bool, value_after: number|object|string|bool|null, overridden_after: bool, value_requested: number|object|string|bool, requested: {...} }

TSharedPtr<FJsonObject> HandleSetMaterialInstanceParam(const TSharedPtr<FJsonObject>& Params)
{
    FString InstancePath, ParamName, ParamType;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_path"), InstancePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("param_type"), ParamType);

    // `value` may be a STRING or one of the natural JSON forms; a string is parsed
    // byte for byte as given.
    const TSharedPtr<FJsonValue> RawValue = Params->TryGetField(TEXT("value"));
    if (!RawValue.IsValid() || RawValue->Type == EJson::None || RawValue->Type == EJson::Null)
    {
        return mcp_common::MissingParameterReply(TEXT("value"));
    }

    FString Value;                      // the string form that is parsed below
    FString KindFromJsonShape;          // inference contributed by a non-string JSON value
    bool bJsonVectorParsed = false;
    FLinearColor JsonVector(0.f, 0.f, 0.f, 1.f);
    bool bJsonBool = false;
    switch (RawValue->Type)
    {
    case EJson::String:
        Value = RawValue->AsString();
        break;
    case EJson::Number:
        Value = FString::SanitizeFloat(RawValue->AsNumber());
        KindFromJsonShape = TEXT("scalar");
        break;
    case EJson::Boolean:
        bJsonBool = RawValue->AsBool();
        Value = bJsonBool ? TEXT("true") : TEXT("false");
        KindFromJsonShape = TEXT("static_switch");
        break;
    case EJson::Array:
    {
        const TArray<TSharedPtr<FJsonValue>>& Arr = RawValue->AsArray();
        if (Arr.Num() != 3 && Arr.Num() != 4)
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("value array has %d element(s); a vector parameter needs 3 or 4 numbers"), Arr.Num()));
        double Comp[4] = { 0.0, 0.0, 0.0, 1.0 };
        for (int32 i = 0; i < Arr.Num(); ++i)
        {
            if (!Arr[i].IsValid() || Arr[i]->Type != EJson::Number)
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("value array element %d is not a number"), i));
            Comp[i] = Arr[i]->AsNumber();
        }
        JsonVector = FLinearColor(
            static_cast<float>(Comp[0]), static_cast<float>(Comp[1]),
            static_cast<float>(Comp[2]), static_cast<float>(Comp[3]));
        bJsonVectorParsed = true;
        Value = JsonVector.ToString();
        KindFromJsonShape = TEXT("vector");
        break;
    }
    default:
        return mcp_common::MakeErrorReply(
            TEXT("value must be a string, a number, a bool, or an array of 3-4 numbers"));
    }

    UMaterialInstanceConstant* Instance = LoadObject<UMaterialInstanceConstant>(nullptr, *InstancePath);
    if (!Instance)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material instance not found: %s"), *InstancePath));

    // Infer the parameter kind from param_type, then from the JSON shape, then
    // from the string format.
    FString Kind = ParamType.ToLower();
    if (Kind.IsEmpty()) { Kind = KindFromJsonShape; }
    if (Kind.IsEmpty())
    {
        if (Value.StartsWith(TEXT("/"))) { Kind = TEXT("texture"); }
        else if (Value.StartsWith(TEXT("(")) || Value.StartsWith(TEXT("["))) { Kind = TEXT("vector"); }
        else { Kind = TEXT("scalar"); }
    }
    // A native JSON array is only meaningful for a vector. Refusing here beats
    // silently Atof-ing "(R=..,G=..)" into a 0 for a scalar parameter.
    if (bJsonVectorParsed && Kind != TEXT("vector"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("value was given as a JSON array (a vector) but param_type is '%s'"), *Kind));
    }

    const FName PName(*ParamName);
    const EMaterialParameterAssociation Assoc = EMaterialParameterAssociation::GlobalParameter;

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("instance"), Instance->GetName());
    Result->SetStringField(TEXT("instance_path"), Instance->GetPathName());
    Result->SetStringField(TEXT("param_name"), ParamName);
    Result->SetStringField(TEXT("param_type"), Kind);

    // Every branch: WRITE -> RE-READ -> compare. The setter's bool is discarded on
    // purpose; it is a constant false in the engine.
    bool bSet = false;
    if (Kind == TEXT("scalar"))
    {
        const float Requested = FCString::Atof(*Value);
        UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(Instance, PName, Requested, Assoc);
        const float After = UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(Instance, PName, Assoc);
        // Relative tolerance: these values run from 0.01 to 262144, so a fixed
        // epsilon would call a correct large write a failure.
        const float Tolerance = FMath::Max(UE_KINDA_SMALL_NUMBER, FMath::Abs(Requested) * 1.e-5f);
        bSet = FMath::IsNearlyEqual(After, Requested, Tolerance);
        Result->SetNumberField(TEXT("value_requested"), Requested);
        Result->SetNumberField(TEXT("value_after"), After);
    }
    else if (Kind == TEXT("vector"))
    {
        FLinearColor Requested(0.f, 0.f, 0.f, 1.f);
        FString Format;
        if (bJsonVectorParsed)
        {
            Requested = JsonVector;
            Format = TEXT("json_array_native");
        }
        else
        {
            FString ParseError;
            if (!ParseLinearColorForMaterialInstanceParam(Value, Requested, Format, ParseError))
            {
                // The whole reason this branch exists: refuse, because writing an
                // unparsed vector stores (0,0,0,1) and says nothing.
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("set_material_instance_parameter: %s"), *ParseError));
            }
        }
        UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(Instance, PName, Requested, Assoc);
        const FLinearColor After = UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(Instance, PName, Assoc);
        bSet = After.Equals(Requested, 1.e-4f);
        Result->SetStringField(TEXT("value_format"), Format);
        Result->SetObjectField(TEXT("value_requested"), mcp_common::LinearColorToJsonObject(Requested));
        Result->SetObjectField(TEXT("value_after"), mcp_common::LinearColorToJsonObject(After));
    }
    else if (Kind == TEXT("texture"))
    {
        UTexture* Texture = LoadObject<UTexture>(nullptr, *Value);
        if (!Texture)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Texture not found: %s"), *Value));
        UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, PName, Texture, Assoc);
        UTexture* After = UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(Instance, PName, Assoc);
        bSet = (After == Texture);
        Result->SetStringField(TEXT("value_requested"), Texture->GetPathName());
        if (After) { Result->SetStringField(TEXT("value_after"), After->GetPathName()); }
        else       { Result->SetField(TEXT("value_after"), MakeShared<FJsonValueNull>()); }
    }
    else if (Kind == TEXT("static_switch"))
    {
        const bool bSwitch = (RawValue->Type == EJson::Boolean) ? bJsonBool : Value.ToBool();
        UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(Instance, PName, bSwitch, Assoc);
        const bool bAfter = UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(Instance, PName, Assoc);
        bSet = (bAfter == bSwitch);
        Result->SetBoolField(TEXT("value_requested"), bSwitch);
        Result->SetBoolField(TEXT("value_after"), bAfter);
    }
    else
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown param_type: %s (scalar/vector/texture/static_switch)"), *ParamType));
    }

    Instance->MarkPackageDirty();

    // Second, independent witness: a value that matches by accident (because the
    // parent already had it) is NOT an override.
    const bool bOverriddenAfter =
        UMaterialEditingLibrary::IsMaterialInstanceParameterOverridden(Instance, PName, Assoc);
    Result->SetBoolField(TEXT("overridden_after"), bOverriddenAfter);
    Result->SetBoolField(TEXT("set"), bSet);

    mcp_common::AddRequestedEcho(Result, Params,
        { TEXT("instance_path"), TEXT("param_name"), TEXT("value"), TEXT("param_type") });

    FString Error;
    if (!bSet)
    {
        const FString OverriddenState = bOverriddenAfter ? TEXT("true") : TEXT("false");
        const FString InstanceName = Instance->GetName();
        Error = FString::Printf(
            TEXT("re-read of '%s' on %s does not match what was written (param_type=%s, overridden=%s). "
                 "The parameter may not exist on this instance, or may be of a different type."),
            *ParamName, *InstanceName, *Kind, *OverriddenState);
    }
    return mcp_common::MakeOutcomeReply(Result, bSet, Error);
}

// MCP:compile_material - Recompile a UMaterial asset through UMaterialEditingLibrary::RecompileMaterial: refreshes its child material instances and the viewports and marks the package dirty, without saving. recompiled is always true and does not report shader errors. Material functions use recompile_material_function.
//   @param material_path - UMaterial asset path, e.g. "/Game/Materials/M_Rock"; a material instance or function path fails with Material not found.
//   @returns { material: string, recompiled: bool }

TSharedPtr<FJsonObject> HandleCompileMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    UMaterialEditingLibrary::RecompileMaterial(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetBoolField(TEXT("recompiled"), true);
    return Result;
}

// ===========================================================================
// Material functions / post-process materials
// ===========================================================================

namespace
{
    // Stable label of an EMaterialFunctionUsage, as create_material_function and get_material_function_info report it.
    const TCHAR* MaterialExtra_FunctionUsageName(EMaterialFunctionUsage Usage)
    {
        switch (Usage)
        {
        case EMaterialFunctionUsage::MaterialLayer:      return TEXT("MaterialLayer");
        case EMaterialFunctionUsage::MaterialLayerBlend: return TEXT("MaterialLayerBlend");
        default:                                         return TEXT("Default");
        }
    }

    // create_material_function's usage parameter -> EMaterialFunctionUsage (case-insensitive); false for any other name.
    bool MaterialExtra_ParseFunctionUsage(const FString& Name, EMaterialFunctionUsage& OutUsage)
    {
        if (Name.Equals(TEXT("Default"), ESearchCase::IgnoreCase))            { OutUsage = EMaterialFunctionUsage::Default;            return true; }
        if (Name.Equals(TEXT("MaterialLayer"), ESearchCase::IgnoreCase))      { OutUsage = EMaterialFunctionUsage::MaterialLayer;      return true; }
        if (Name.Equals(TEXT("MaterialLayerBlend"), ESearchCase::IgnoreCase)) { OutUsage = EMaterialFunctionUsage::MaterialLayerBlend; return true; }
        return false;
    }

    // Count function input/output expressions currently inside a material function.
    void CountFunctionIO(const UMaterialFunction* Function, int32& OutInputs, int32& OutOutputs)
    {
        OutInputs = 0;
        OutOutputs = 0;
        if (!Function)
        {
            return;
        }
        for (const TObjectPtr<UMaterialExpression>& ExprPtr : Function->GetExpressions())
        {
            UMaterialExpression* Expr = ExprPtr.Get();
            if (!Expr)
            {
                continue;
            }
            if (Expr->IsA<UMaterialExpressionFunctionInput>())  { ++OutInputs; }
            else if (Expr->IsA<UMaterialExpressionFunctionOutput>()) { ++OutOutputs; }
        }
    }

    // Parse a string to EFunctionInputType — all 13 EFunctionInputType values,
    // case-insensitive (EXPECTED_NUM_INPUT_TYPES=13, excludes the FunctionInput_MAX
    // sentinel). Returns false on an unknown name. SHARED by add_material_function_input
    // and set_material_function_input_props (one parser).
    bool ParseFunctionInputType(const FString& In, EFunctionInputType& Out)
    {
        const FString T = In.ToLower();
        if (T == TEXT("scalar"))             { Out = FunctionInput_Scalar;             return true; }
        if (T == TEXT("vector2"))            { Out = FunctionInput_Vector2;            return true; }
        if (T == TEXT("vector3"))            { Out = FunctionInput_Vector3;            return true; }
        if (T == TEXT("vector4"))            { Out = FunctionInput_Vector4;            return true; }
        if (T == TEXT("texture2d"))          { Out = FunctionInput_Texture2D;          return true; }
        if (T == TEXT("texturecube"))        { Out = FunctionInput_TextureCube;        return true; }
        if (T == TEXT("texture2darray"))     { Out = FunctionInput_Texture2DArray;     return true; }
        if (T == TEXT("volumetexture"))      { Out = FunctionInput_VolumeTexture;      return true; }
        if (T == TEXT("staticbool"))         { Out = FunctionInput_StaticBool;         return true; }
        if (T == TEXT("materialattributes")) { Out = FunctionInput_MaterialAttributes; return true; }
        if (T == TEXT("textureexternal"))    { Out = FunctionInput_TextureExternal;    return true; }
        if (T == TEXT("bool"))               { Out = FunctionInput_Bool;               return true; }
        if (T == TEXT("substrate"))          { Out = FunctionInput_Substrate;          return true; }
        return false;
    }

    const TCHAR* FunctionInputTypeToString(EFunctionInputType Type)
    {
        switch (Type)
        {
        case FunctionInput_Scalar:             return TEXT("Scalar");
        case FunctionInput_Vector2:            return TEXT("Vector2");
        case FunctionInput_Vector3:            return TEXT("Vector3");
        case FunctionInput_Vector4:            return TEXT("Vector4");
        case FunctionInput_Texture2D:          return TEXT("Texture2D");
        case FunctionInput_TextureCube:        return TEXT("TextureCube");
        case FunctionInput_Texture2DArray:     return TEXT("Texture2DArray");
        case FunctionInput_VolumeTexture:      return TEXT("VolumeTexture");
        case FunctionInput_StaticBool:         return TEXT("StaticBool");
        case FunctionInput_MaterialAttributes: return TEXT("MaterialAttributes");
        case FunctionInput_TextureExternal:    return TEXT("TextureExternal");
        case FunctionInput_Bool:               return TEXT("Bool");
        case FunctionInput_Substrate:          return TEXT("Substrate");
        default:                               return TEXT("Scalar");
        }
    }

    // Locate a function input/output node by its node_id (== UMaterialExpression::GetName()).
    UMaterialExpression* FindFunctionExpressionByNodeId(UMaterialFunction* Function, const FString& NodeId)
    {
        if (!Function) { return nullptr; }
        for (const TObjectPtr<UMaterialExpression>& ExprPtr : Function->GetExpressions())
        {
            UMaterialExpression* Expr = ExprPtr.Get();
            if (Expr && Expr->GetName() == NodeId)
            {
                return Expr;
            }
        }
        return nullptr;
    }

    // Apply the optional shared input props present in Params onto a function-input node
    // (description, preview_value FVector4f, use_preview_value_as_default bitfield,
    // sort_priority). Idempotent: only fields present in Params are written. Shared by
    // add_material_function_input and set_material_function_input_props.
    void ApplyOptionalInputProps(UMaterialExpressionFunctionInput* In, const TSharedPtr<FJsonObject>& Params)
    {
        FString Desc; double Num; bool bVal;
        if (Params->TryGetStringField(TEXT("description"), Desc))                 { In->Description = Desc; }
        if (Params->TryGetNumberField(TEXT("sort_priority"), Num))               { In->SortPriority = static_cast<int32>(Num); }
        if (Params->TryGetBoolField(TEXT("use_preview_value_as_default"), bVal))  { In->bUsePreviewValueAsDefault = bVal ? 1 : 0; }
        const TSharedPtr<FJsonObject>* PV = nullptr;
        if (Params->TryGetObjectField(TEXT("preview_value"), PV) && PV && *PV)
        {
            FVector4f V = In->PreviewValue;  // start from current so a partial {x} keeps y/z/w
            double C;
            if ((*PV)->TryGetNumberField(TEXT("x"), C)) { V.X = static_cast<float>(C); }
            if ((*PV)->TryGetNumberField(TEXT("y"), C)) { V.Y = static_cast<float>(C); }
            if ((*PV)->TryGetNumberField(TEXT("z"), C)) { V.Z = static_cast<float>(C); }
            if ((*PV)->TryGetNumberField(TEXT("w"), C)) { V.W = static_cast<float>(C); }
            In->PreviewValue = V;
        }
    }

    // Map a string domain name to EMaterialDomain. Returns false if unknown.
    bool ResolveMaterialDomain(const FString& DomainName, EMaterialDomain& Out)
    {
        const FString D = DomainName.ToLower();
        if (D == TEXT("surface"))       { Out = MD_Surface;        return true; }
        if (D == TEXT("postprocess") || D == TEXT("post_process") || D == TEXT("post process")) { Out = MD_PostProcess; return true; }
        if (D == TEXT("ui") || D == TEXT("userinterface") || D == TEXT("user interface")) { Out = MD_UI; return true; }
        if (D == TEXT("deferreddecal") || D == TEXT("deferred_decal") || D == TEXT("deferred decal") || D == TEXT("decal")) { Out = MD_DeferredDecal; return true; }
        if (D == TEXT("lightfunction") || D == TEXT("light_function") || D == TEXT("light function")) { Out = MD_LightFunction; return true; }
        if (D == TEXT("volume"))        { Out = MD_Volume;         return true; }
        return false;
    }

    const TCHAR* MaterialDomainToString(EMaterialDomain Domain)
    {
        switch (Domain)
        {
        case MD_Surface:        return TEXT("Surface");
        case MD_DeferredDecal:  return TEXT("DeferredDecal");
        case MD_LightFunction:  return TEXT("LightFunction");
        case MD_Volume:         return TEXT("Volume");
        case MD_PostProcess:    return TEXT("PostProcess");
        case MD_UI:             return TEXT("UI");
        default:                return TEXT("Unknown");
        }
    }
}

namespace UEAgentMaterialExtraImpl
{
    // Reads the required function_path and node_id and loads that function; the error reply when one fails.
    TSharedPtr<FJsonObject> ReadFunctionAndNodeId(const TSharedPtr<FJsonObject>& Params, FString& OutFunctionPath, FString& OutNodeId,
        UMaterialFunction*& OutFunction)
    {
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_path"), OutFunctionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), OutNodeId, mcp_common::EAllowEmpty::Yes)) { return Missing; }
        return FindFunction(OutFunctionPath, OutFunction);
    }

    // PostEditChange on Node, then UpdateMaterialFunction, PostEditChange and MarkPackageDirty on its Function.
    void CommitNodeEdit(UMaterialFunction* Function, UMaterialExpression* Node)
    {
        Node->PostEditChange();

        UMaterialEditingLibrary::UpdateMaterialFunction(Function, /*PreviewMaterial*/ nullptr);
        Function->PostEditChange();
        Function->MarkPackageDirty();
    }

    // Deletes Node from Function, recompiles the function and recounts its inputs and outputs.
    void DeleteFunctionNode(UMaterialFunction* Function, UMaterialExpression* Node, int32& OutInputs, int32& OutOutputs)
    {
        Function->Modify();
        UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Node);

        // UpdateMaterialFunction forces render recompiles; serialize game<->render.
        FlushRenderingCommands();
        UMaterialEditingLibrary::UpdateMaterialFunction(Function, /*PreviewMaterial*/ nullptr);
        FlushRenderingCommands();
        Function->PostEditChange();
        Function->MarkPackageDirty();

        CountFunctionIO(Function, OutInputs, OutOutputs);
    }

    // node_count, nodes (with their inputs) and the flattened connections of an expression graph. Empty
    // slots in Expressions are skipped, so node_count counts the nodes listed. An input's name is
    // GetInputName(i): FExpressionInput::InputName is set only by the few nodes that rename an input,
    // and is None on the rest, whose inputs are named after their FExpressionInput property.
    void AddGraphFields(const TSharedPtr<FJsonObject>& Result, TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions)
    {
        TArray<TSharedPtr<FJsonValue>> Nodes;
        TArray<TSharedPtr<FJsonValue>> Connections;
        for (UMaterialExpression* Expr : Expressions)
        {
            if (!Expr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
            NObj->SetStringField(TEXT("name"), Expr->GetName());
            NObj->SetStringField(TEXT("class"), Expr->GetClass()->GetName());
            NObj->SetStringField(TEXT("class_path"), Expr->GetClass()->GetPathName());
            NObj->SetStringField(TEXT("description"), Expr->GetDescription());

            // Per-class inputs via GetInput(i)/CountInputs() (GetInputsView() is deprecated).
            TArray<TSharedPtr<FJsonValue>> InputsJson;
            const int32 NumInputs = Expr->CountInputs();
            for (int32 i = 0; i < NumInputs; ++i)
            {
                FExpressionInput* In = Expr->GetInput(i);
                if (!In) { continue; }
                const FString ReportedInputName = Expr->GetInputName(i).ToString();
                TSharedPtr<FJsonObject> IObj = MakeShared<FJsonObject>();
                IObj->SetNumberField(TEXT("index"), i);
                IObj->SetStringField(TEXT("input_name"), ReportedInputName);
                if (In->Expression)
                {
                    // The source output is identified by an OutputIndex into
                    // Expression's FExpressionOutput[]; we do not have a
                    // OutputName on FExpressionInput itself. Record the
                    // OutputIndex so callers can resolve the name downstream.
                    IObj->SetStringField(TEXT("source"), In->Expression->GetName());
                    IObj->SetNumberField(TEXT("source_output_index"), In->OutputIndex);
                    // Record the connection at the connection-list level too.
                    TSharedPtr<FJsonObject> CObj = MakeShared<FJsonObject>();
                    CObj->SetStringField(TEXT("from"), In->Expression->GetName());
                    CObj->SetNumberField(TEXT("from_output_index"), In->OutputIndex);
                    CObj->SetStringField(TEXT("to"), Expr->GetName());
                    CObj->SetStringField(TEXT("to_input"), ReportedInputName);
                    Connections.Add(MakeShared<FJsonValueObject>(CObj));
                }
                InputsJson.Add(MakeShared<FJsonValueObject>(IObj));
            }
            NObj->SetArrayField(TEXT("inputs"), InputsJson);
            NObj->SetNumberField(TEXT("input_count"), NumInputs);
            Nodes.Add(MakeShared<FJsonValueObject>(NObj));
        }

        Result->SetNumberField(TEXT("node_count"), Nodes.Num());
        Result->SetArrayField(TEXT("nodes"), Nodes);
        Result->SetNumberField(TEXT("connection_count"), Connections.Num());
        Result->SetArrayField(TEXT("connections"), Connections);
    }

    // expression_count and orphan_nodes: every live expression object whose outer is Owner (the material or function),
    // and those of them its expression collection does not list. A partial bulk delete or a duplicate can leave such
    // nodes behind; they still count, so expression_count is the oracle for deleting nodes one by one.
    void AddOuterOwnedCensus(const TSharedPtr<FJsonObject>& Result, const UObject* Owner, TConstArrayView<TObjectPtr<UMaterialExpression>> Listed)
    {
        int32 OwnedCount = 0;
        TArray<TSharedPtr<FJsonValue>> Orphans;
        for (TObjectIterator<UMaterialExpression> NodeIt; NodeIt; ++NodeIt)
        {
            UMaterialExpression* Node = *NodeIt;
            if (Node == nullptr || Node->GetOuter() != Owner)
            {
                continue;
            }
            ++OwnedCount;
            if (!Listed.Contains(Node))
            {
                TSharedPtr<FJsonObject> Orphan = MakeShared<FJsonObject>();
                Orphan->SetStringField(TEXT("name"), Node->GetName());
                Orphan->SetStringField(TEXT("class"), Node->GetClass()->GetName());
                Orphans.Add(MakeShared<FJsonValueObject>(Orphan));
            }
        }
        Result->SetNumberField(TEXT("expression_count"), OwnedCount);
        Result->SetNumberField(TEXT("orphan_count"), Orphans.Num());
        Result->SetArrayField(TEXT("orphan_nodes"), Orphans);
    }

    // Saves the package Asset lives in; false, with OutError saying why, when it has none or the save fails.
    bool SaveOwningPackage(UObject* Asset, FString& OutError)
    {
        UPackage* Package = Asset->GetPackage();
        if (!Package)
        {
            OutError = FString::Printf(TEXT("%s has no package to save"), *Asset->GetPathName());
            return false;
        }
        return mcp_common::SaveAssetPackage(Package, Asset, OutError);
    }

    // SaveOwningPackage for a caller that does not report why the save failed.
    bool SaveOwningPackage(UObject* Asset)
    {
        FString SaveError;
        return SaveOwningPackage(Asset, SaveError);
    }

    // The FExpressionInput property of Expr whose name equals InputName (FString comparison), or null.
    FExpressionInput* FindExpressionInput(UMaterialExpression* Expr, const FString& InputName)
    {
        for (TFieldIterator<FProperty> It(Expr->GetClass()); It; ++It)
        {
            FProperty* Prop = *It;
            if (Prop && Prop->GetName() == InputName)
            {
                if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
                {
                    if (StructProp->Struct->GetFName() == TEXT("ExpressionInput"))
                    {
                        return StructProp->ContainerPtrToValuePtr<FExpressionInput>(Expr);
                    }
                }
            }
        }
        return nullptr;
    }
}

// MCP:create_material_function - Create an empty material function asset named name in package_path -- a plain UMaterialFunction through the editor factory, or with usage MaterialLayer / MaterialLayerBlend a material layer or layer blend function, the way the editor's factories make them -- exposed to the material function library, with the optional description stored as its tooltip, then save it. Idempotent: an existing material function at that path, whatever its class or usage, is returned untouched with already_exists=true, already_existed=true, created=false and saved=false, and nothing is saved (description is ignored then and comes back as the function's own); another asset class there, or an invalid package path, is an error. A failed save (PIE running, read-only file) makes success false with created=true, saved=false and save_error, the function kept in memory. Then add pins with add_material_function_input and add_material_function_output and nodes with create_material_expression (material_function_path); read it back with get_material_graph_info; a variant with overrides: create_material_function_instance; use it in a material: add_material_function_call (a layer or blend: set_material_layer_stack); inverse: delete_asset.
//   @param package_path - /Game package directory, e.g. '/Game/MyGame'; a trailing slash is ignored
//   @param name - Asset name for the new material function
//   @param description - Text stored as the function Description, the tooltip shown wherever the function is used; default empty. Ignored when the function already exists.
//   @param usage - Optional "Default" (a plain UMaterialFunction, the default), "MaterialLayer" (a UMaterialFunctionMaterialLayer) or "MaterialLayerBlend" (a UMaterialFunctionMaterialLayerBlend), any case. Any other value is an error
//   @returns { function_path: string, asset_path: string, name: string, package_path: string, class: string, usage: string, description: string, num_inputs: number, num_outputs: number, created: bool, already_existed: bool, saved: bool, save_error?: string, already_exists?: bool }

TSharedPtr<FJsonObject> HandleCreateMaterialFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name, Description;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("description"), Description);
    FString UsageName;
    Params->TryGetStringField(TEXT("usage"), UsageName);
    EMaterialFunctionUsage Usage = EMaterialFunctionUsage::Default;
    if (!UsageName.IsEmpty() && !MaterialExtra_ParseFunctionUsage(UsageName, Usage))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown usage: %s (expected Default, MaterialLayer or MaterialLayerBlend)"), *UsageName));
    }
    while (PackagePath.EndsWith(TEXT("/")))
    {
        PackagePath.LeftChopInline(1, EAllowShrinking::No);
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: if it already exists, load and return it.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UMaterialFunction* Existing = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(FullPath);
        if (Existing)
        {
            int32 NumInputs = 0, NumOutputs = 0;
            CountFunctionIO(Existing, NumInputs, NumOutputs);
            TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
            R->SetStringField(TEXT("function_path"), Existing->GetPathName());
            R->SetStringField(TEXT("asset_path"), Existing->GetPathName());
            R->SetStringField(TEXT("name"), Existing->GetName());
            R->SetStringField(TEXT("package_path"), PackagePath);
            R->SetStringField(TEXT("class"), Existing->GetClass()->GetName());
            R->SetStringField(TEXT("usage"), MaterialExtra_FunctionUsageName(Existing->GetMaterialFunctionUsage()));
            R->SetStringField(TEXT("description"), Existing->Description);
            R->SetNumberField(TEXT("num_inputs"), NumInputs);
            R->SetNumberField(TEXT("num_outputs"), NumOutputs);
            R->SetBoolField(TEXT("already_exists"), true);
            R->SetBoolField(TEXT("already_existed"), true);
            R->SetBoolField(TEXT("created"), false);
            R->SetBoolField(TEXT("saved"), false);
            return mcp_common::MakeOkReply(R);
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset exists but is not a UMaterialFunction: %s"), *FullPath));
    }

    UMaterialFunction* Function = nullptr;
    if (Usage == EMaterialFunctionUsage::Default)
    {
        // Create via the real editor factory so the inner objects are set up correctly.
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        IAssetTools& AssetTools = AssetToolsModule.Get();

        UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
        if (!Factory)
            return mcp_common::MakeErrorReply(TEXT("Failed to create UMaterialFunctionFactoryNew"));

        UObject* CreatedAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterialFunction::StaticClass(), Factory);
        Function = Cast<UMaterialFunction>(CreatedAsset);
        if (!Function)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create material function: %s"), *FullPath));
    }
    else
    {
        // A layer or blend: what the editor's layer factories do (neither overrides ConfigureProperties, so this is
        // headless-safe): a new object of the specialised class, tagged with its usage, registered with the registry.
        UClass* const LayerClass = Usage == EMaterialFunctionUsage::MaterialLayer
            ? UMaterialFunctionMaterialLayer::StaticClass()
            : UMaterialFunctionMaterialLayerBlend::StaticClass();
        if (const FString AssetConflict = mcp_common::AssetNameConflict(FullPath, Name, LayerClass); !AssetConflict.IsEmpty())
            return mcp_common::MakeErrorReply(AssetConflict);
        UPackage* Package = CreatePackage(*FullPath);
        if (!Package)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create package: %s"), *FullPath));
        Function = NewObject<UMaterialFunction>(Package, LayerClass, *Name, RF_Public | RF_Standalone | RF_Transactional);
        if (!Function)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create %s asset"), *LayerClass->GetName()));
        Function->SetMaterialFunctionUsage(Usage);
        FAssetRegistryModule::AssetCreated(Function);
    }

    Function->Modify();
    if (!Description.IsEmpty())
    {
        Function->Description = Description;
    }
    // Expose to the material function library so it behaves like an editor-created one.
    Function->bExposeToLibrary = true;
    Function->PostEditChange();
    Function->MarkPackageDirty();

    // Save the package to disk. A failed save keeps the new function in memory and fails the call.
    FString SaveError;
    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Function, SaveError);

    int32 NumInputs = 0, NumOutputs = 0;
    CountFunctionIO(Function, NumInputs, NumOutputs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("asset_path"), Function->GetPathName());
    Result->SetStringField(TEXT("name"), Function->GetName());
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetStringField(TEXT("class"), Function->GetClass()->GetName());
    Result->SetStringField(TEXT("usage"), MaterialExtra_FunctionUsageName(Function->GetMaterialFunctionUsage()));
    Result->SetStringField(TEXT("description"), Function->Description);
    Result->SetNumberField(TEXT("num_inputs"), NumInputs);
    Result->SetNumberField(TEXT("num_outputs"), NumOutputs);
    Result->SetBoolField(TEXT("created"), true);
    Result->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_material_function"), SaveError);
}

// MCP:add_material_function_input - Add a FunctionInput node (a new input pin) to a UMaterialFunction asset with the optional props applied, then recompile the function and mark it dirty without saving. Returns node_id, the handle for set_material_function_input_props and delete_material_function_input, plus num_inputs and added.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param input_name - Name of the new input pin, stored as given: a name another input already uses is not made unique.
//   @param input_type - One of the 13 EFunctionInputType values: Scalar|Vector2|Vector3|Vector4|Texture2D|TextureCube|Texture2DArray|VolumeTexture|StaticBool|MaterialAttributes|TextureExternal|Bool|Substrate (optional, default "Scalar")
//   @param description - Optional tooltip/description string for the input pin
//   @param preview_value [object] - JSON object {"x":0,"y":0,"z":0,"w":0} (FVector4f), any subset; missing components keep their current value. Used for previews, and as the default with use_preview_value_as_default.
//   @param use_preview_value_as_default [bool] - Optional bool; if true, the preview value becomes the input's default
//   @param sort_priority [number] - Optional int controlling the input's display order relative to other inputs
//   @returns { function_path: string, input_name: string, input_type: string, node_id: string, num_inputs: number, added: bool }

TSharedPtr<FJsonObject> HandleAddMaterialFunctionInput(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, InputName, InputTypeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_path"), FunctionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), InputName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("input_type"), InputTypeStr))
        InputTypeStr = TEXT("Scalar");

    EFunctionInputType InputType = FunctionInput_Scalar;
    if (!ParseFunctionInputType(InputTypeStr, InputType))
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown input_type: %s (Scalar/Vector2/Vector3/Vector4/Texture2D/TextureCube/"
                 "Texture2DArray/VolumeTexture/StaticBool/MaterialAttributes/TextureExternal/Bool/Substrate)"),
            *InputTypeStr));

    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindFunction(FunctionPath, Function)) { return Error; }

    int32 BeforeInputs = 0, BeforeOutputs = 0;
    CountFunctionIO(Function, BeforeInputs, BeforeOutputs);

    Function->Modify();
    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
        Function, UMaterialExpressionFunctionInput::StaticClass(), /*X*/ -400, /*Y*/ BeforeInputs * 100);
    UMaterialExpressionFunctionInput* InputExpr = Cast<UMaterialExpressionFunctionInput>(NewExpr);
    if (!InputExpr)
        return mcp_common::MakeErrorReply(TEXT("Failed to create UMaterialExpressionFunctionInput"));

    InputExpr->Modify();
    InputExpr->InputName = FName(*InputName);
    InputExpr->InputType = InputType;
    ApplyOptionalInputProps(InputExpr, Params);  // optional description/preview_value/default-flag/sort_priority
    InputExpr->ValidateName();
    UEAgentMaterialExtraImpl::CommitNodeEdit(Function, InputExpr);

    int32 AfterInputs = 0, AfterOutputs = 0;
    CountFunctionIO(Function, AfterInputs, AfterOutputs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("input_name"), InputName);
    Result->SetStringField(TEXT("input_type"), FunctionInputTypeToString(InputType));
    Result->SetStringField(TEXT("node_id"), InputExpr->GetName());
    Result->SetNumberField(TEXT("num_inputs"), AfterInputs);
    Result->SetBoolField(TEXT("added"), AfterInputs == BeforeInputs + 1);
    return Result;
}

// MCP:add_material_function_output - Add a FunctionOutput node, the output pin named output_name, to a UMaterialFunction asset loaded from function_path, then recompile the function (loaded materials that use it recompile too) and mark it dirty, without saving and without an undo transaction. Each call adds another node, so it is not idempotent, and output_name is stored as given: a name another output already has is not made unique. The new output starts unwired, placed right of the graph below the existing outputs, and node_id is its handle; added confirms the output count rose by one. A function that does not load is an error. Wire it with connect_material_function_pins, edit it with set_material_function_output_props, and its inverse is delete_material_function_output; inputs are add_material_function_input; list pins with get_material_function_info; persist with save_asset.
//   @param function_path - Full path to the material function, e.g. "/Game/MyGame/MF_Example.MF_Example"
//   @param output_name - Name of the new output pin, stored as given: a name another output already uses is not made unique
//   @returns { function_path: string, output_name: string, node_id: string, num_outputs: number, added: bool }

TSharedPtr<FJsonObject> HandleAddMaterialFunctionOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, OutputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_path"), FunctionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("output_name"), OutputName, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindFunction(FunctionPath, Function)) { return Error; }

    int32 BeforeInputs = 0, BeforeOutputs = 0;
    CountFunctionIO(Function, BeforeInputs, BeforeOutputs);

    Function->Modify();
    UMaterialExpression* NewExpr = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
        Function, UMaterialExpressionFunctionOutput::StaticClass(), /*X*/ 400, /*Y*/ BeforeOutputs * 100);
    UMaterialExpressionFunctionOutput* OutputExpr = Cast<UMaterialExpressionFunctionOutput>(NewExpr);
    if (!OutputExpr)
        return mcp_common::MakeErrorReply(TEXT("Failed to create UMaterialExpressionFunctionOutput"));

    OutputExpr->Modify();
    OutputExpr->OutputName = FName(*OutputName);
    OutputExpr->ValidateName();
    UEAgentMaterialExtraImpl::CommitNodeEdit(Function, OutputExpr);

    int32 AfterInputs = 0, AfterOutputs = 0;
    CountFunctionIO(Function, AfterInputs, AfterOutputs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("output_name"), OutputName);
    Result->SetStringField(TEXT("node_id"), OutputExpr->GetName());
    Result->SetNumberField(TEXT("num_outputs"), AfterOutputs);
    Result->SetBoolField(TEXT("added"), AfterOutputs == BeforeOutputs + 1);
    return Result;
}

// MCP:set_material_function_input_props - Edit an existing FunctionInput node of a UMaterialFunction asset, found by node_id: only the fields you pass are written (name, type, description, preview value, default flag, sort priority), then the function is recompiled and marked dirty without saving. Read it back with get_material_function_info.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param node_id - The input node id (UMaterialExpression name) from add_material_function_input
//   @param input_name - New pin name, stored as given: it is not made unique (the engine ValidateName only acts on nodes owned by a material).
//   @param input_type - Optional EFunctionInputType (the 13 values; same parser as add_material_function_input)
//   @param description - New tooltip text of the input pin.
//   @param preview_value [object] - JSON object {"x":0,"y":0,"z":0,"w":0} (FVector4f), any subset; missing components keep their current value.
//   @param use_preview_value_as_default [bool] - Bool: true makes the preview value the default of the input when its pin is left unconnected.
//   @param sort_priority [number] - Integer that orders this input pin relative to the other inputs of the function.
//   @returns { function_path: string, node_id: string, input_name: string, input_type: string, applied: bool }

TSharedPtr<FJsonObject> HandleSetMaterialFunctionInputProps(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, NodeId;
    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::ReadFunctionAndNodeId(Params, FunctionPath, NodeId, Function)) { return Error; }

    UMaterialExpressionFunctionInput* In = Cast<UMaterialExpressionFunctionInput>(FindFunctionExpressionByNodeId(Function, NodeId));
    if (!In)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Function input node '%s' not found in %s"), *NodeId, *FunctionPath));

    Function->Modify();
    In->Modify();

    // input_type (optional) via the shared parser — honest error on an unknown name.
    FString InputTypeStr;
    if (Params->TryGetStringField(TEXT("input_type"), InputTypeStr))
    {
        EFunctionInputType InputType = FunctionInput_Scalar;
        if (!ParseFunctionInputType(InputTypeStr, InputType))
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Unknown input_type: %s (Scalar/Vector2/Vector3/Vector4/Texture2D/TextureCube/"
                     "Texture2DArray/VolumeTexture/StaticBool/MaterialAttributes/TextureExternal/Bool/Substrate)"),
                *InputTypeStr));
        In->InputType = InputType;
    }

    // input_name (optional) -> must re-validate uniqueness.
    bool bNameChanged = false;
    FString NewName;
    if (Params->TryGetStringField(TEXT("input_name"), NewName))
    {
        In->InputName = FName(*NewName);
        bNameChanged = true;
    }

    ApplyOptionalInputProps(In, Params);

    if (bNameChanged) { In->ValidateName(); }
    UEAgentMaterialExtraImpl::CommitNodeEdit(Function, In);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("node_id"), In->GetName());
    Result->SetStringField(TEXT("input_name"), In->InputName.ToString());
    Result->SetStringField(TEXT("input_type"), FunctionInputTypeToString(In->InputType));
    Result->SetBoolField(TEXT("applied"), true);
    return Result;
}

// MCP:set_material_function_output_props - Set properties on an existing UMaterialExpressionFunctionOutput (located by node_id).  Partial update (idempotent). ValidateName is re-run when output_name changes.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param node_id - The output node id (UMaterialExpression name) from add_material_function_output
//   @param output_name - Optional new pin name (re-validated for uniqueness)
//   @param description - Optional tooltip/description string
//   @param sort_priority - Optional int display-order
//   @returns { function_path: string, node_id: string, output_name: string, applied: bool }

TSharedPtr<FJsonObject> HandleSetMaterialFunctionOutputProps(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, NodeId;
    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::ReadFunctionAndNodeId(Params, FunctionPath, NodeId, Function)) { return Error; }

    UMaterialExpressionFunctionOutput* Out = Cast<UMaterialExpressionFunctionOutput>(FindFunctionExpressionByNodeId(Function, NodeId));
    if (!Out)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Function output node '%s' not found in %s"), *NodeId, *FunctionPath));

    Function->Modify();
    Out->Modify();

    bool bNameChanged = false;
    FString NewName, Desc;
    double Num;
    if (Params->TryGetStringField(TEXT("output_name"), NewName)) { Out->OutputName = FName(*NewName); bNameChanged = true; }
    if (Params->TryGetStringField(TEXT("description"), Desc))    { Out->Description = Desc; }
    if (Params->TryGetNumberField(TEXT("sort_priority"), Num))   { Out->SortPriority = static_cast<int32>(Num); }

    if (bNameChanged) { Out->ValidateName(); }
    UEAgentMaterialExtraImpl::CommitNodeEdit(Function, Out);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("node_id"), Out->GetName());
    Result->SetStringField(TEXT("output_name"), Out->OutputName.ToString());
    Result->SetBoolField(TEXT("applied"), true);
    return Result;
}

// MCP:create_post_process_material - Create a UMaterial named name in package_path through the editor factory, configured as a post-process material (domain PostProcess, blended after tonemapping), then recompile and save it. Idempotent create or convert: an existing UMaterial at that path has those two settings overwritten, is recompiled and saved, and answers created=false, already_exists=true and a note with its old domain; one that already has them is left as it is (changed=false, saved=false, a note), nothing recompiled or saved. Another asset class there, or an invalid path, is an error. A failed save makes success false with saved=false and save_error, the material kept in memory. Add nodes with create_material_expression and wire them with connect_to_material_output; another domain: set_material_domain.
//   @param package_path - /Game package directory, e.g. '/Game/MyGame'
//   @param name - Asset name for the new material
//   @returns { material_path: string, name: string, package_path: string, domain: string, blendable_location: number, is_post_process: bool, created: bool, already_exists: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleCreatePostProcessMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    UMaterial* Material = nullptr;
    bool bAlreadyExists = false;
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        Material = AssetPathUtils::LoadOrLoadSoftPathString<UMaterial>(FullPath);
        if (!Material)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset exists but is not a UMaterial: %s"), *FullPath));
        bAlreadyExists = true;
    }
    else
    {
        FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
        IAssetTools& AssetTools = AssetToolsModule.Get();

        UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
        if (!Factory)
            return mcp_common::MakeErrorReply(TEXT("Failed to create UMaterialFactoryNew"));

        UObject* CreatedAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterial::StaticClass(), Factory);
        Material = Cast<UMaterial>(CreatedAsset);
        if (!Material)
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create material: %s"), *FullPath));
    }

    // An existing material that already has the settings below is left as it is: nothing is modified, recompiled or
    // saved. One with other settings is converted to a post-process material, as a new one is configured.
    const EMaterialDomain PreviousDomain = (EMaterialDomain)Material->MaterialDomain.GetValue();
    const bool bAlreadyConfigured = bAlreadyExists && Material->MaterialDomain == MD_PostProcess
        && Material->BlendableLocation == BL_SceneColorAfterTonemapping;

    bool bSaved = false;
    FString SaveError;
    if (!bAlreadyConfigured)
    {
        // Configure as a post-process material.
        Material->PreEditChange(nullptr);
        Material->Modify();
        Material->MaterialDomain = MD_PostProcess;
        Material->BlendableLocation = BL_SceneColorAfterTonemapping;
        Material->PostEditChange();
        UMaterialEditingLibrary::RecompileMaterial(Material);
        Material->MarkPackageDirty();

        // Save the package.
        bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Material, SaveError);
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("name"), Material->GetName());
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetStringField(TEXT("domain"), MaterialDomainToString((EMaterialDomain)Material->MaterialDomain.GetValue()));
    Result->SetNumberField(TEXT("blendable_location"), (int32)Material->BlendableLocation.GetValue());
    Result->SetBoolField(TEXT("is_post_process"), Material->MaterialDomain == MD_PostProcess);
    Result->SetBoolField(TEXT("created"), !bAlreadyExists);
    Result->SetBoolField(TEXT("already_exists"), bAlreadyExists);
    Result->SetBoolField(TEXT("changed"), !bAlreadyConfigured);
    if (bAlreadyConfigured)
    {
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetStringField(TEXT("note"), TEXT("The material is already a post-process material blended after tonemapping; nothing was changed or saved."));
        return mcp_common::MakeOkReply(Result);
    }
    if (bAlreadyExists)
    {
        Result->SetStringField(TEXT("note"), FString::Printf(
            TEXT("The existing material was converted to a post-process material blended after tonemapping (its domain was %s)."),
            MaterialDomainToString(PreviousDomain)));
    }
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_post_process_material"), SaveError);
}

// MCP:set_material_domain - Set the material domain (EMaterialDomain) of a UMaterial asset, then recompile it and mark its package dirty, without saving and without an undo transaction. domain is matched without case and also accepts spellings such as post_process, decal or light function; an unknown domain is an error before the material loads, and a material instance or a path that does not load is an error (Material not found). Setting the domain it already has still recompiles. previous_domain and domain are read back, and domain_set confirms the change took. Persist with save_asset; a new post-process material is create_post_process_material; a recompile alone is compile_material.
//   @param material_path - Full path to the material, e.g. '/Game/MyGame/M_Example.M_Example'; a UMaterial, not a material instance
//   @param domain - One of 'Surface', 'PostProcess', 'UI', 'DeferredDecal', 'LightFunction', 'Volume', matched without case; post_process, userinterface, decal, deferred_decal and light_function (or the same words with spaces) work too
//   @returns { material: string, material_path: string, previous_domain: string, requested_domain: string, domain: string, domain_set: bool }

TSharedPtr<FJsonObject> HandleSetMaterialDomain(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath, DomainStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("domain"), DomainStr, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    EMaterialDomain Domain;
    if (!ResolveMaterialDomain(DomainStr, Domain))
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown domain: %s (Surface/PostProcess/UI/DeferredDecal/LightFunction/Volume)"), *DomainStr));

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    const EMaterialDomain Previous = (EMaterialDomain)Material->MaterialDomain.GetValue();

    // A material domain change forces a full FMaterialResource rebuild on the render
    // thread. Serialize game<->render around it (flush before mutating so the render
    // thread isn't mid-flight on the resources, and after each step so the rebuild
    // completes before we touch the material again / return) — without this, a deferred
    // render command can dereference a reallocated resource -> RenderCore access violation.
    FlushRenderingCommands();
    Material->PreEditChange(nullptr);
    Material->Modify();
    Material->MaterialDomain = Domain;
    Material->PostEditChange();
    FlushRenderingCommands();
    UMaterialEditingLibrary::RecompileMaterial(Material);
    FlushRenderingCommands();
    Material->MarkPackageDirty();

    // Read the value back to confirm the change actually took.
    const EMaterialDomain ReadBack = (EMaterialDomain)Material->MaterialDomain.GetValue();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("previous_domain"), MaterialDomainToString(Previous));
    Result->SetStringField(TEXT("requested_domain"), DomainStr);
    Result->SetStringField(TEXT("domain"), MaterialDomainToString(ReadBack));
    Result->SetBoolField(TEXT("domain_set"), ReadBack == Domain);
    return Result;
}

// ===========================================================================
// Material graph authoring: function-call insertion, expression duplication and
// full graph introspection.
//
// Every RecompileMaterial here is wrapped in FlushRenderingCommands() (before AND
// after), the render-thread mitigation set_material_domain uses, so the asset-level
// mutation is serialized against any in-flight render work.
// ===========================================================================

// MCP:add_material_function_call - Insert a call to a UMaterialFunction into a UMaterial graph: a MaterialFunctionCall node bound to the function, so the function's inputs and outputs become the node's pins. Refused, with the node removed again, when the function cannot be called from this material.
//   @param material_path - Full asset path of the UMaterial (e.g. '/Game/MyGame/M_Example')
//   @param function_path - Full asset path of the UMaterialFunction to call (e.g. '/Game/Functions/MF_Foo.MF_Foo')
//   @param node_name - Optional name for the new node; a name another object of the material already has gets a unique variant
//   @returns { material_path: string, function_path: string, node_name: string, node_class: string, expression_count: number }

TSharedPtr<FJsonObject> HandleAddMaterialFunctionCall(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath, FunctionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_path"), FunctionPath)) { return Missing; }
    FString NodeName;
    Params->TryGetStringField(TEXT("node_name"), NodeName);

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    UMaterialFunctionInterface* Function = LoadObject<UMaterialFunctionInterface>(nullptr, *FunctionPath);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function not found: %s"), *FunctionPath));
    }

    // SetMaterialFunction binds the function and builds the node's inputs and outputs from it; it
    // refuses a function the material cannot call (a recursive reference, for one).
    UMaterialExpressionMaterialFunctionCall* NewNode = Cast<UMaterialExpressionMaterialFunctionCall>(
        UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionMaterialFunctionCall::StaticClass()));
    if (!NewNode)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create material expression"));
    }
    if (!NewNode->SetMaterialFunction(Function))
    {
        UMaterialEditingLibrary::DeleteMaterialExpression(Material, NewNode);
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Material function %s cannot be called from %s"), *Function->GetPathName(), *Material->GetPathName()));
    }

    if (!NodeName.IsEmpty())
    {
        mcp_common::RenameToFreeName(NewNode, NodeName, Material);
    }

    Material->Modify();
    Material->PostEditChange();
    FlushRenderingCommands();
    UMaterialEditingLibrary::RecompileMaterial(Material);
    FlushRenderingCommands();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("node_name"), NewNode->GetName());
    Result->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
    Result->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
    return Result;
}

// MCP:duplicate_material_expression - Copy one expression node of a UMaterial asset into the same material, optionally renamed, then recompile the material and mark it dirty without saving. The copy keeps the source position and settings (a parameter keeps its name) and its output is unwired. Returns duplicate_node and expression_count.
//   @param material_path - Full asset path of the UMaterial.
//   @param node_id - Object name of the expression to copy, as listed by get_material_graph_info (nodes[].name).
//   @param new_node_name - Object name for the copy; when taken, a unique variant is used (read duplicate_node). Default: a name the engine picks.
//   @returns { material_path: string, source_node: string, duplicate_node: string, duplicate_class: string, expression_count: number }

TSharedPtr<FJsonObject> HandleDuplicateMaterialExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath, NodeId;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("node_id"), NodeId)) { return Missing; }
    FString NewNodeName;
    Params->TryGetStringField(TEXT("new_node_name"), NewNodeName);

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }
    UMaterialExpression* Source = FindExpressionByName(Material, NodeId);
    if (!Source)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Expression not found: %s"), *NodeId));
    }

    Material->Modify();
    UMaterialExpression* Dup = UMaterialEditingLibrary::DuplicateMaterialExpression(
        Material,
        /*InFunction*/ nullptr,
        Source);
    if (!Dup)
    {
        return mcp_common::MakeErrorReply(TEXT("DuplicateMaterialExpression returned null"));
    }
    // Rename if requested.
    if (!NewNodeName.IsEmpty())
    {
        Dup->Modify();
        mcp_common::RenameToFreeName(Dup, NewNodeName, Material);
    }
    Material->PostEditChange();
    FlushRenderingCommands();
    UMaterialEditingLibrary::RecompileMaterial(Material);
    FlushRenderingCommands();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("source_node"), NodeId);
    Result->SetStringField(TEXT("duplicate_node"), Dup->GetName());
    Result->SetStringField(TEXT("duplicate_class"), Dup->GetClass()->GetName());
    Result->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
    return Result;
}

// MCP:get_material_graph_info - Read the expression graph of a UMaterial asset -- or of a UMaterialFunction / material layer's internal graph -- without changing it: every expression node with its object name (the node_id other material tools take), class, class path and description, its inputs by index, and the node-to-node connections flattened into connections[], each with the source output index. Only wiring between expressions is listed -- what feeds the material's main outputs (BaseColor, Normal...) is not -- and node positions and property values are not read. input_name and to_input are each input's name as the node reports it (GetInputName, e.g. A, B, Alpha), and node_count counts the nodes listed. expression_count counts every expression object the asset owns, including orphan_nodes its expression collection does not list (a partial bulk delete or a duplicate leaves them behind, and duplicate parameter names hide there): it is the count to watch when deleting nodes one by one until 0. A material instance or a path that does not load is an error. Shader stats: get_material_stats; wiring: connect_material_expressions, connect_to_material_output, connect_material_function_pins.
//   @param material_path - UMaterial asset path, e.g. "/Game/Materials/M_Rock" or "/Game/Materials/M_Rock.M_Rock"; a UMaterialFunction path is read as a function graph; a material instance is not accepted
//   @param material_function_path - Alias of material_path that addresses a UMaterialFunction (or material layer / layer blend)
//   @returns { material_path?: string (a material), material_name?: string, material_function_path?: string (a function), expression_count: number, orphan_count: number, orphan_nodes: [{ name: string, class: string }], node_count: number, nodes: [{ name: string, class: string, class_path: string, description: string, inputs: [{ index: number, input_name: string, source?: string, source_output_index?: number }], input_count: number }], connection_count: number, connections: [{ from: string, from_output_index: number, to: string, to_input: string }] }

TSharedPtr<FJsonObject> HandleGetMaterialGraphInfo(const TSharedPtr<FJsonObject>& Params)
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

    UMaterial* Material = nullptr;
    UMaterialFunction* Function = nullptr;
    if (bFunctionKey)
    {
        Function = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(GraphPath);
    }
    if (Function == nullptr)
    {
        Material = AssetPathUtils::LoadOrLoadSoftPathString<UMaterial>(GraphPath);
    }
    if (Material == nullptr && Function == nullptr && !bFunctionKey)
    {
        Function = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(GraphPath);
    }
    if (Material == nullptr && Function == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material or material function not found: %s"), *GraphPath));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    if (Function != nullptr)
    {
        Result->SetStringField(TEXT("material_function_path"), Function->GetPathName());
        UEAgentMaterialExtraImpl::AddOuterOwnedCensus(Result, Function, Function->GetExpressions());
        UEAgentMaterialExtraImpl::AddGraphFields(Result, Function->GetExpressions());
    }
    else
    {
        Result->SetStringField(TEXT("material_path"), Material->GetPathName());
        Result->SetStringField(TEXT("material_name"), Material->GetName());
        UEAgentMaterialExtraImpl::AddOuterOwnedCensus(Result, Material, Material->GetExpressions());
        UEAgentMaterialExtraImpl::AddGraphFields(Result, Material->GetExpressions());
    }
    return Result;
}
// ========================================================================== //
// Material function authoring advanced
//
// Function-graph I/O, instance support, recompile and lifecycle ops for material
// functions. Creation and expression-graph authoring (`create_material_function`,
// `add_material_function_input`, `add_material_function_output`,
// `add_material_function_call`) sit earlier in this file.
//   recompile_material_function         UpdateMaterialFunction on a UMaterialFunctionInterface.
//   connect_material_function_pins      ConnectMaterialExpressions on a function's
//                                       expression list (vs the material-level
//                                       connect_material_expressions).
//   create_material_function_instance    UMaterialFunctionInstance via
//                                       UMaterialFunctionInstanceFactoryNew.
//   set_material_function_instance_parent  set Parent on the instance.
//   get_material_function_info        enumerate UMaterialExpressionFunctionInput.
// ========================================================================== //

// MCP:recompile_material_function - Recompile a material function, layer or function instance (UpdateMaterialFunction): loaded materials using it recompile and dependent function instances are marked dirty but not saved, then the function itself is saved; success requires that save. recompiled is always true, not a shader-error report.
//   @param material_function_path - Asset path of the UMaterialFunctionInterface (function, layer or function instance), e.g. "/Game/Materials/MF_Blend".
//   @returns { material_function_path: string, function_class: string, recompiled: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRecompileMaterialFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_function_path"), FunctionPath)) { return Missing; }

    UMaterialFunctionInterface* Function = LoadObject<UMaterialFunctionInterface>(nullptr, *FunctionPath);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function not found: %s"), *FunctionPath));
    }

    // UMF recompile: same as material recompile, except the target is the
    // function (and any preview material is derived from it). Signature:
    // UpdateMaterialFunction(UMaterialFunctionInterface*, UMaterial* PreviewMaterial=nullptr).
    // FlushRenderingCommands() before+after to serialize against the render thread
    // (same mitigation as compile_material / set_material_domain).
    FlushRenderingCommands();
    UMaterialEditingLibrary::UpdateMaterialFunction(Function);
    FlushRenderingCommands();

    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Function);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("function_class"), Function->GetClass()->GetName());
    Result->SetBoolField(TEXT("recompiled"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("recompile_material_function"));
}

// MCP:connect_material_function_pins - Wire two material expression nodes inside a UMaterialFunction graph by name.
//   @param material_function_path - Asset path of the UMaterialFunction
//   @param from_node_id - Name of the source expression
//   @param to_node_id - Name of the destination expression
//   @param from_pin - Optional source output pin name (default: first available)
//   @param to_input - Optional destination FExpressionInput name (e.g. "A"); if omitted, picks the first empty input
//   @returns { material_function_path: string, from_node_id: string, to_node_id: string, from_pin: string, to_input: string, connected: bool, saved: bool }
TSharedPtr<FJsonObject> HandleConnectMaterialFunctionPins(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, FromNodeId, FromPinName, ToNodeId, ToInputName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_function_path"), FunctionPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("from_node_id"), FromNodeId)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("to_node_id"), ToNodeId)) { return Missing; }
    Params->TryGetStringField(TEXT("from_pin"), FromPinName);
    Params->TryGetStringField(TEXT("to_input"), ToInputName);

// ConnectMaterialExpressions expects a UMaterial*, not a UMaterialFunction.
    // The function-level equivalent uses the function's own GetExpressions() list
    // (a UMaterialFunction has a parallel expression array under FunctionExpressions
    // via UMaterialFunction::GetExpressionCollection()).
    UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *FunctionPath);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function not found: %s"), *FunctionPath));
    }

    // UMaterialFunction::GetExpressions() returns TConstArrayView<TObjectPtr<UMaterialExpression>>.
    TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions = Function->GetExpressions();
    UMaterialExpression* FromExpr = nullptr;
    UMaterialExpression* ToExpr = nullptr;
    for (const TObjectPtr<UMaterialExpression>& E : Expressions)
    {
        if (!E) { continue; }
        const FString Name = E->GetName();
        if (!FromExpr && Name == FromNodeId) { FromExpr = E; }
        if (!ToExpr && Name == ToNodeId)   { ToExpr = E; }
    }
    if (!FromExpr)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("from_node_id '%s' not found in function"), *FromNodeId));
    }
    if (!ToExpr)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("to_node_id '%s' not found in function"), *ToNodeId));
    }

    // Find a matching input on the destination expression and wire it.
    // The most common pattern in UMaterialFunctions: wire FromExpr's default
    // output pin into ToExpr's matching FExpressionInput (resolved by name).
    FExpressionInput* TargetInput = nullptr;
    if (!ToInputName.IsEmpty())
    {
        // Find the input by reflection name (e.g. "A", "B", "Input").
        TargetInput = UEAgentMaterialExtraImpl::FindExpressionInput(ToExpr, ToInputName);
        if (!TargetInput)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("to_input '%s' not an FExpressionInput on %s"), *ToInputName, *ToNodeId));
        }
    }
    else
    {
        // Auto-pick input A while nothing is wired into it.
        FExpressionInput* InputA = UEAgentMaterialExtraImpl::FindExpressionInput(ToExpr, TEXT("A"));
        if (InputA && !InputA->Expression)
        {
            TargetInput = InputA;
        }
    }
    if (!TargetInput)
    {
        return mcp_common::MakeErrorReply(
            TEXT("No suitable empty FExpressionInput found on destination; pass to_input explicitly"));
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ConnectMaterialFunctionPins", "Connect Material Function Pins (MCP)"));
    Function->Modify();
    TargetInput->Expression = FromExpr;

    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Function);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("from_node_id"), FromNodeId);
    Result->SetStringField(TEXT("to_node_id"), ToNodeId);
    Result->SetStringField(TEXT("from_pin"), FromPinName);
    Result->SetStringField(TEXT("to_input"), ToInputName.IsEmpty() ? TEXT("A") : ToInputName);
    Result->SetBoolField(TEXT("connected"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("connect_material_function_pins"));
}

// MCP:create_material_function_instance - Create a UMaterialFunctionInstance asset named instance_name in package_path through the editor factory, parented to the material function or function instance at parent_function_path, then save it. package_path is any long package path, such as /Game/Materials or a plugin's /MyPlugin/Funcs; a relative folder is read under /Game, a missing folder is made, and an unmounted or malformed path is an error. Idempotent: an instance already at that path is returned unchanged with already_existed=true, created=false and saved=false, its own parent in parent_function_path (the requested one is ignored) and nothing saved; another asset class there is an error. A parent that does not load is an error and nothing, not even the folder, is created; a failed save (PIE running, read-only file) makes success false with saved=false and save_error, the new instance kept in memory. Re-parent later: set_material_function_instance_parent; a new base function: create_material_function.
//   @param package_path - Target folder as a long package path, e.g. /Game/Materials or /MyPlugin/Funcs; a relative folder is read under /Game
//   @param instance_name - Name for the new UMaterialFunctionInstance asset
//   @param parent_function_path - Asset path of the parent UMaterialFunctionInterface
//   @returns { asset_path: string, instance_name: string, parent_function_path: string, created: bool, already_existed: bool, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleCreateMaterialFunctionInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, InstanceName, ParentPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_name"), InstanceName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_function_path"), ParentPath)) { return Missing; }

    // package_path is a folder: any long package path is taken as it is (/Game, /Game/Funcs, a plugin's
    // /MyPlugin/Funcs), without trailing slashes; a relative folder is read under /Game.
    PackagePath = mcp_common::NormalizeFolder(PackagePath, TEXT("/Game"));
    if (!PackagePath.StartsWith(TEXT("/")))
    {
        PackagePath = TEXT("/Game/") + PackagePath;
    }
    const FString FullPath = PackagePath + TEXT("/") + InstanceName;
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
    FText InvalidReason;
    if (!FPackageName::IsValidLongPackageName(FullPath, /*bIncludeReadOnlyRoots=*/ false, &InvalidReason))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Invalid asset path '%s': %s. Nothing was created."), *FullPath, *InvalidReason.ToString()));
    }

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        // Idempotent: return the existing instance unchanged and unsaved, like create_material_function does.
        UMaterialFunctionInstance* Existing = LoadObject<UMaterialFunctionInstance>(nullptr, *FullPath);
        if (!Existing)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Asset exists but is not a UMaterialFunctionInstance: %s"), *FullPath));
        }
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), FullPath);
        Result->SetStringField(TEXT("instance_name"), InstanceName);
        Result->SetStringField(TEXT("parent_function_path"), Existing->Parent ? Existing->Parent->GetPathName() : FString());
        Result->SetBoolField(TEXT("created"), false);
        Result->SetBoolField(TEXT("already_existed"), true);
        Result->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Result);
    }

    // The parent is checked before anything is made, so a bad parent leaves no new folder behind.
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(ParentPath)) { return Invalid; }
    UMaterialFunctionInterface* Parent = LoadObject<UMaterialFunctionInterface>(nullptr, *ParentPath);
    if (!Parent)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Parent function not found: %s"), *ParentPath));
    }

    if (!UEditorAssetLibrary::DoesDirectoryExist(PackagePath))
    {
        UEditorAssetLibrary::MakeDirectory(PackagePath);
    }

    UMaterialFunctionInstanceFactory* Factory = NewObject<UMaterialFunctionInstanceFactory>();
    if (!Factory)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to spawn UMaterialFunctionInstanceFactory"));
    }
    Factory->InitialParent = Parent;

    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    UObject* CreatedAsset = AssetToolsModule.Get().CreateAsset(
        InstanceName, PackagePath, UMaterialFunctionInstance::StaticClass(), Factory);
    if (!CreatedAsset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create material function instance %s"), *FullPath));
    }

    FString SaveError;
    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(CreatedAsset, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), FullPath);
    Result->SetStringField(TEXT("instance_name"), InstanceName);
    Result->SetStringField(TEXT("parent_function_path"), Parent->GetPathName());
    Result->SetBoolField(TEXT("created"), true);
    Result->SetBoolField(TEXT("already_existed"), false);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_material_function_instance"), SaveError);
}

// MCP:set_material_function_instance_parent - Re-parent a UMaterialFunctionInstance asset loaded from instance_path to the material function or function instance at parent_function_path, in one undo step, then save it. old_parent and new_parent are read back from the asset ("(none)" when empty), and applied is true once the parent is set. Success requires the save: a failed save (PIE running, read-only file) makes success false with saved=false and save_error, the new parent kept in memory. A path containing // is refused before anything loads, and an instance or parent that does not load is an error; nothing changes then. The parent it already has is a success with applied=false, saved=false and a note, nothing changed or saved. Create an instance with create_material_function_instance; recompile it with recompile_material_function.
//   @param instance_path - Asset path of the UMaterialFunctionInstance to re-parent
//   @param parent_function_path - Asset path of the new parent UMaterialFunctionInterface
//   @returns { instance_path: string, old_parent: string, new_parent: string, applied: bool, saved: bool, save_error?: string, note?: string }
TSharedPtr<FJsonObject> HandleSetMaterialFunctionInstanceParent(const TSharedPtr<FJsonObject>& Params)
{
    FString InstancePath, ParentPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_path"), InstancePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_function_path"), ParentPath)) { return Missing; }

    // Both paths are checked before the first load: LoadObject creates the package a path names, and creating one
    // whose name holds "//" is a Fatal error.
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(InstancePath)) { return Invalid; }
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(ParentPath)) { return Invalid; }

    UMaterialFunctionInstance* Instance = LoadObject<UMaterialFunctionInstance>(nullptr, *InstancePath);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function instance not found: %s"), *InstancePath));
    }
    UMaterialFunctionInterface* Parent = LoadObject<UMaterialFunctionInterface>(nullptr, *ParentPath);
    if (!Parent)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Parent function not found: %s"), *ParentPath));
    }

    const FString OldParent = Instance->Parent ? Instance->Parent->GetPathName() : TEXT("(none)");

    // The parent it already has: nothing is modified or saved.
    if (Instance->Parent == Parent)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("instance_path"), Instance->GetPathName());
        Result->SetStringField(TEXT("old_parent"), OldParent);
        Result->SetStringField(TEXT("new_parent"), OldParent);
        Result->SetBoolField(TEXT("applied"), false);
        Result->SetBoolField(TEXT("saved"), false);
        Result->SetStringField(TEXT("note"), TEXT("The instance already has that parent; nothing was changed or saved."));
        return mcp_common::MakeOkReply(Result);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetMaterialFunctionInstanceParent", "Set Material Function Instance Parent (MCP)"));
    Instance->Modify();
    Instance->SetParent(Parent);

    FString SaveError;
    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Instance, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("instance_path"), Instance->GetPathName());
    Result->SetStringField(TEXT("old_parent"), OldParent);
    Result->SetStringField(TEXT("new_parent"), Instance->Parent ? Instance->Parent->GetPathName() : TEXT("(none)"));
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_material_function_instance_parent"), SaveError);
}

// MCP:get_material_function_info - Describe a material function asset -- a plain function, a material layer, a layer blend or a function instance -- without changing it: its class and usage ("Default", "MaterialLayer" or "MaterialLayerBlend"), is_layer / is_blend, the input and output pin counts it exposes (num_inputs, num_outputs; an instance reports its parent's) and num_expressions, plus, for a UMaterialFunction, its FunctionInput and FunctionOutput nodes: node_id, name, type, description, sort priority and preview value for inputs; name, description and sort priority for outputs (a function instance has no nodes of its own, so its lists are empty). For the whole node graph use get_material_graph_info.
//   @param material_function_path - Asset path of the material function, layer, layer blend or function instance, e.g. "/Game/Materials/MF_Blend".
//   @param layer_path - Alias of material_function_path
//   @returns { material_function_path: string, asset_path: string, class: string, usage: string, is_layer: bool, is_blend: bool, num_inputs: number, num_outputs: number, num_expressions: number, input_count: number, output_count: number, inputs: [{ node_id: string, input_name: string, input_type: string, description: string, sort_priority: number, use_preview_value: bool, preview_value: {x,y,z,w} }], outputs: [{ node_id: string, output_name: string, description: string, sort_priority: number }] }
TSharedPtr<FJsonObject> HandleGetMaterialFunctionInputs(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath;
    if (!Params->TryGetStringField(TEXT("material_function_path"), FunctionPath) && !Params->TryGetStringField(TEXT("layer_path"), FunctionPath))
    {
        return mcp_common::MissingParameterReply(TEXT("material_function_path"));
    }

    // Any function interface (function, layer, blend or function instance), by path or soft path.
    UMaterialFunctionInterface* FunctionInterface = LoadObject<UMaterialFunctionInterface>(nullptr, *FunctionPath);
    if (!FunctionInterface)
    {
        const FSoftObjectPath SoftPath(FunctionPath);
        if (SoftPath.IsValid())
        {
            FunctionInterface = Cast<UMaterialFunctionInterface>(SoftPath.TryLoad());
        }
    }
    if (!FunctionInterface)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function not found: %s"), *FunctionPath));
    }

    TArray<FFunctionExpressionInput> PinInputs;
    TArray<FFunctionExpressionOutput> PinOutputs;
    FunctionInterface->GetInputsAndOutputs(PinInputs, PinOutputs);
    const EMaterialFunctionUsage Usage = FunctionInterface->GetMaterialFunctionUsage();

    TArray<TSharedPtr<FJsonValue>> Inputs;
    TArray<TSharedPtr<FJsonValue>> Outputs;
    UMaterialFunction* Function = Cast<UMaterialFunction>(FunctionInterface);
    const TConstArrayView<TObjectPtr<UMaterialExpression>> OwnNodes = Function != nullptr
        ? Function->GetExpressions() : TConstArrayView<TObjectPtr<UMaterialExpression>>();
    for (UMaterialExpression* Expr : OwnNodes)
    {
        if (!Expr) { continue; }
        if (UMaterialExpressionFunctionInput* In = Cast<UMaterialExpressionFunctionInput>(Expr))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("node_id"), In->GetName());
            Obj->SetStringField(TEXT("input_name"), In->InputName.ToString());
            Obj->SetStringField(TEXT("input_type"), FunctionInputTypeToString(In->InputType));
            Obj->SetStringField(TEXT("description"), In->Description);
            Obj->SetNumberField(TEXT("sort_priority"), In->SortPriority);
            Obj->SetBoolField(TEXT("use_preview_value"), In->bUsePreviewValueAsDefault);
            TSharedPtr<FJsonObject> PV = MakeShared<FJsonObject>();
            PV->SetNumberField(TEXT("x"), In->PreviewValue.X);
            PV->SetNumberField(TEXT("y"), In->PreviewValue.Y);
            PV->SetNumberField(TEXT("z"), In->PreviewValue.Z);
            PV->SetNumberField(TEXT("w"), In->PreviewValue.W);
            Obj->SetObjectField(TEXT("preview_value"), PV);
            Inputs.Add(MakeShared<FJsonValueObject>(Obj));
        }
        else if (UMaterialExpressionFunctionOutput* Out = Cast<UMaterialExpressionFunctionOutput>(Expr))
        {
            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("node_id"), Out->GetName());
            Obj->SetStringField(TEXT("output_name"), Out->OutputName.ToString());
            Obj->SetStringField(TEXT("description"), Out->Description);
            Obj->SetNumberField(TEXT("sort_priority"), Out->SortPriority);
            Outputs.Add(MakeShared<FJsonValueObject>(Obj));
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_function_path"), FunctionInterface->GetPathName());
    Result->SetStringField(TEXT("asset_path"), FunctionInterface->GetPathName());
    Result->SetStringField(TEXT("class"), FunctionInterface->GetClass()->GetName());
    Result->SetStringField(TEXT("usage"), MaterialExtra_FunctionUsageName(Usage));
    Result->SetBoolField(TEXT("is_layer"), FunctionInterface->IsA<UMaterialFunctionMaterialLayer>() || Usage == EMaterialFunctionUsage::MaterialLayer);
    Result->SetBoolField(TEXT("is_blend"), FunctionInterface->IsA<UMaterialFunctionMaterialLayerBlend>() || Usage == EMaterialFunctionUsage::MaterialLayerBlend);
    Result->SetNumberField(TEXT("num_inputs"), PinInputs.Num());
    Result->SetNumberField(TEXT("num_outputs"), PinOutputs.Num());
    Result->SetNumberField(TEXT("num_expressions"), OwnNodes.Num());
    Result->SetNumberField(TEXT("input_count"), Inputs.Num());
    Result->SetNumberField(TEXT("output_count"), Outputs.Num());
    Result->SetArrayField(TEXT("inputs"), Inputs);
    Result->SetArrayField(TEXT("outputs"), Outputs);
    return mcp_common::MakeOkReply(Result);
}

// ========================================================================== //
// Delete a function input/output node (inverse of the add twins)
//
// DeleteMaterialExpressionInFunction(UMaterialFunction*, UMaterialExpression*)
// (MaterialEditingLibrary.h, UE_API). The engine impl SILENTLY no-ops
// unless Expression->GetOuter() == MaterialFunction, so the node is always
// located via the function's own expression list (FindFunctionExpressionByNodeId,
// never a global TObjectIterator). The node_id is the stable per-function
// UMaterialExpression name the add handlers return.
//
// Idempotent: an absent node_id is success ({ deleted:false, already_absent:true })
// — never an error, never {}. Both handlers Modify() before, recompile via
// UpdateMaterialFunction (wrapped in FlushRenderingCommands() before AND after,
// same render-thread mitigation as set_material_domain), then
// PostEditChange()/MarkPackageDirty(). `deleted` is asserted by the count drop.
// ========================================================================== //

// MCP:delete_material_function_input - Delete a UMaterialExpressionFunctionInput node from a material function (inverse of add_material_function_input).  Located by node_id via the function's own expression list. Idempotent: an already-absent node_id returns { deleted:false, already_absent:true } as success, not an error.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param node_id - The input node id (UMaterialExpression name) from add_material_function_input
//   @returns { function_path: string, node_id: string, deleted: bool, already_absent: bool, num_inputs: number, num_outputs: number }

TSharedPtr<FJsonObject> HandleDeleteMaterialFunctionInput(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, NodeId;
    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::ReadFunctionAndNodeId(Params, FunctionPath, NodeId, Function)) { return Error; }

    int32 BeforeInputs = 0, BeforeOutputs = 0;
    CountFunctionIO(Function, BeforeInputs, BeforeOutputs);

    // Locate strictly via the function's own list — the engine delete no-ops
    // unless GetOuter() == Function, and a global iterator could return a
    // same-named node from a different function.
    UMaterialExpression* Expr = FindFunctionExpressionByNodeId(Function, NodeId);
    if (!Expr)
    {
        // Idempotent absence: not an error.
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] delete_material_function_input: node_id '%s' already absent in %s (inputs=%d outputs=%d)"),
            *NodeId, *FunctionPath, BeforeInputs, BeforeOutputs);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("function_path"), Function->GetPathName());
        Result->SetStringField(TEXT("node_id"), NodeId);
        Result->SetBoolField(TEXT("deleted"), false);
        Result->SetBoolField(TEXT("already_absent"), true);
        Result->SetNumberField(TEXT("num_inputs"), BeforeInputs);
        Result->SetNumberField(TEXT("num_outputs"), BeforeOutputs);
        return Result;
    }

    // Reject a wrong-kind node_id (e.g. an output or a plain expression) cleanly.
    UMaterialExpressionFunctionInput* InputExpr = Cast<UMaterialExpressionFunctionInput>(Expr);
    if (!InputExpr)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("node_id '%s' is not a function input"), *NodeId));

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] delete_material_function_input: deleting node_id '%s' in %s (inputs=%d)"),
        *NodeId, *FunctionPath, BeforeInputs);

    int32 AfterInputs = 0, AfterOutputs = 0;
    UEAgentMaterialExtraImpl::DeleteFunctionNode(Function, InputExpr, AfterInputs, AfterOutputs);

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] delete_material_function_input: node_id '%s' -> inputs %d->%d"),
        *NodeId, BeforeInputs, AfterInputs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetBoolField(TEXT("deleted"), AfterInputs == BeforeInputs - 1);
    Result->SetBoolField(TEXT("already_absent"), false);
    Result->SetNumberField(TEXT("num_inputs"), AfterInputs);
    Result->SetNumberField(TEXT("num_outputs"), AfterOutputs);
    return Result;
}

// MCP:delete_material_function_output - Delete a UMaterialExpressionFunctionOutput node from a material function (inverse of add_material_function_output).  Located by node_id via the function's own expression list. Idempotent: an already-absent node_id returns { deleted:false, already_absent:true } as success, not an error.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param node_id - The output node id (UMaterialExpression name) from add_material_function_output
//   @returns { function_path: string, node_id: string, deleted: bool, already_absent: bool, num_inputs: number, num_outputs: number }

TSharedPtr<FJsonObject> HandleDeleteMaterialFunctionOutput(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath, NodeId;
    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::ReadFunctionAndNodeId(Params, FunctionPath, NodeId, Function)) { return Error; }

    int32 BeforeInputs = 0, BeforeOutputs = 0;
    CountFunctionIO(Function, BeforeInputs, BeforeOutputs);

    UMaterialExpression* Expr = FindFunctionExpressionByNodeId(Function, NodeId);
    if (!Expr)
    {
        // Idempotent absence: not an error.
        UE_LOG(LogTemp, Verbose, TEXT("[MCP] delete_material_function_output: node_id '%s' already absent in %s (inputs=%d outputs=%d)"),
            *NodeId, *FunctionPath, BeforeInputs, BeforeOutputs);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("function_path"), Function->GetPathName());
        Result->SetStringField(TEXT("node_id"), NodeId);
        Result->SetBoolField(TEXT("deleted"), false);
        Result->SetBoolField(TEXT("already_absent"), true);
        Result->SetNumberField(TEXT("num_inputs"), BeforeInputs);
        Result->SetNumberField(TEXT("num_outputs"), BeforeOutputs);
        return Result;
    }

    UMaterialExpressionFunctionOutput* OutputExpr = Cast<UMaterialExpressionFunctionOutput>(Expr);
    if (!OutputExpr)
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("node_id '%s' is not a function output"), *NodeId));

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] delete_material_function_output: deleting node_id '%s' in %s (outputs=%d)"),
        *NodeId, *FunctionPath, BeforeOutputs);

    int32 AfterInputs = 0, AfterOutputs = 0;
    UEAgentMaterialExtraImpl::DeleteFunctionNode(Function, OutputExpr, AfterInputs, AfterOutputs);

    UE_LOG(LogTemp, Verbose, TEXT("[MCP] delete_material_function_output: node_id '%s' -> outputs %d->%d"),
        *NodeId, BeforeOutputs, AfterOutputs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetBoolField(TEXT("deleted"), AfterOutputs == BeforeOutputs - 1);
    Result->SetBoolField(TEXT("already_absent"), false);
    Result->SetNumberField(TEXT("num_inputs"), AfterInputs);
    Result->SetNumberField(TEXT("num_outputs"), AfterOutputs);
    return Result;
}

// create_subsurface_profile
// ---------------------------------------------------------------------------- //
// MCP:create_subsurface_profile - Create a USubsurfaceProfile asset named name in package_path (default /Game/Gameplay; trailing slashes trimmed) with the engine's default settings, then save it; mean_free_path_distance reads one of those settings back. Idempotent: an asset at that path that loads as a subsurface profile is returned as it is, with already_existed=true, created=false and saved=false, and nothing is saved; one of another class is an error, and so is an invalid package path. A failed save (PIE running, read-only file) makes success false with saved=false and save_error, but the new profile stays registered in memory, so a repeat call answers already_existed=true for an asset that is not on disk: persist it with save_asset. Edit its Settings struct with set_properties on the asset path, then save_asset.
//   @param name - Asset name (e.g. SSP_Example)
//   @param package_path - Target /Game package path (optional, default "/Game/Gameplay")
//   @returns { name: string, asset_path: string, asset_class: string, mean_free_path_distance: number, is_subsurface_profile: bool, already_existed: bool, created: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleCreateSubsurfaceProfile(const TSharedPtr<FJsonObject>& Params)
{
    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString RequestedFolder;
    Params->TryGetStringField(TEXT("package_path"), RequestedFolder);
    const FString PackagePath = mcp_common::NormalizeFolder(RequestedFolder, TEXT("/Game/Gameplay"));
    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    bool bAlreadyExisted = false;
    bool bSaved = false;
    FString SaveError;
    USubsurfaceProfile* Profile = nullptr;

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        Profile = LoadObject<USubsurfaceProfile>(nullptr, *FullPath);
        if (!Profile)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Asset exists but failed to load as SubsurfaceProfile: %s"), *FullPath));
        }
        bAlreadyExisted = true;
    }
    else
    {
        FString PackageError;
        UPackage* Package = mcp_common::CreateAssetPackage(*FullPath, *Name, USubsurfaceProfile::StaticClass(), PackageError);
        if (!Package)
        {
            return mcp_common::MakeErrorReply(PackageError);
        }

        // NewObject via StaticClass() (USubsurfaceProfile is MinimalAPI).
        Profile = NewObject<USubsurfaceProfile>(Package, USubsurfaceProfile::StaticClass(), *Name,
                                                RF_Public | RF_Standalone | RF_Transactional);
        if (!Profile)
        {
            return mcp_common::MakeErrorReply(TEXT("NewObject<USubsurfaceProfile> returned null"));
        }

        FAssetRegistryModule::AssetCreated(Profile);
        Profile->MarkPackageDirty();

        // A failed save keeps the new profile in memory; the reply below says so with saved=false and save_error.
        bSaved = mcp_common::SaveAssetPackage(Package, Profile, SaveError);
    }

    const bool bIsSubsurfaceProfile = Profile->IsA(USubsurfaceProfile::StaticClass());

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("name"), Name);
    Result->SetStringField(TEXT("asset_path"), FullPath);
    Result->SetStringField(TEXT("asset_class"), Profile->GetClass()->GetName());
    // Surface one inspectable default from FSubsurfaceProfileStruct Settings so the
    // payload reflects a real, read-back property rather than an opaque handle.
    Result->SetNumberField(TEXT("mean_free_path_distance"), Profile->Settings.MeanFreePathDistance);
    Result->SetBoolField(TEXT("is_subsurface_profile"), bIsSubsurfaceProfile);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !bAlreadyExisted);
    if (bAlreadyExisted)
    {
        // The existing profile is returned as it is: nothing was saved.
        Result->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Result);
    }
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_subsurface_profile"), SaveError);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_material_function_call"), &HandleAddMaterialFunctionCall },
    { TEXT("add_material_function_input"), &HandleAddMaterialFunctionInput },
    { TEXT("add_material_function_output"), &HandleAddMaterialFunctionOutput },
    { TEXT("compile_material"), &HandleCompileMaterial },
    { TEXT("connect_material_function_pins"), &HandleConnectMaterialFunctionPins },
    { TEXT("connect_to_material_output"), &HandleConnectToMaterialOutput },
    { TEXT("create_material_function"), &HandleCreateMaterialFunction },
    { TEXT("create_material_function_instance"), &HandleCreateMaterialFunctionInstance },
    { TEXT("create_post_process_material"), &HandleCreatePostProcessMaterial },
    { TEXT("create_subsurface_profile"), &HandleCreateSubsurfaceProfile },
    { TEXT("delete_material_function_input"), &HandleDeleteMaterialFunctionInput },
    { TEXT("delete_material_function_output"), &HandleDeleteMaterialFunctionOutput },
    { TEXT("disconnect_to_material_output"), &HandleDisconnectToMaterialOutput },
    { TEXT("duplicate_material_expression"), &HandleDuplicateMaterialExpression },
    { TEXT("get_material_function_info"), &HandleGetMaterialFunctionInputs },
    { TEXT("get_material_graph_info"), &HandleGetMaterialGraphInfo },
    { TEXT("recompile_material_function"), &HandleRecompileMaterialFunction },
    { TEXT("set_material_domain"), &HandleSetMaterialDomain },
    { TEXT("set_material_function_input_props"), &HandleSetMaterialFunctionInputProps },
    { TEXT("set_material_function_instance_parent"), &HandleSetMaterialFunctionInstanceParent },
    { TEXT("set_material_function_output_props"), &HandleSetMaterialFunctionOutputProps },
    { TEXT("set_material_instance_parameter"), &HandleSetMaterialInstanceParam },
    { TEXT("set_material_expression_property"), &HandleSetMaterialNodeParam },
};
} // namespace UEAgentMaterialExtraCommands
