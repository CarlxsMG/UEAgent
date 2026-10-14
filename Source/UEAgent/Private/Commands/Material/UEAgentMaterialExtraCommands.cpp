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
// set_material_instance_param re-reads through UMaterialEditingLibrary's value
// getters, all of which take an EMaterialParameterAssociation.
#include "Materials/MaterialParameters.h"
#include "Engine/Texture.h"
#include "SceneTypes.h"
#include "UObject/UObjectIterator.h"
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

// MCP:connect_to_material_output - Connect a material expression output to a material property.
//   @param material_path - Material asset path.
//   @param from_node_id - Id of the source expression (from create_material_expression / get_material_expressions).
//   @param output_property - BaseColor, Metallic, Roughness, Normal, Emissive, Opacity, OpacityMask, Specular, ...
//   @param from_pin - Optional source output pin name (default: first available)
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

// MCP:set_material_node_param - Set a value/property on a material expression node (e.g. a Constant value or parameter name).
//   @param material_path - Material asset path
//   @param node_id - Id of the target expression in the material
//   @param param_name - Name of the property to set (e.g. "ParameterName", "R", "DefaultValue")
//   @param value - String value to import (parsed per property type)
//   @returns { material: string, node_id: string, param_name: string, value: string }

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

    Material->Modify();
    Expr->Modify();
    bool bHandled = false;

    if (ParamName.Equals(TEXT("ParameterName"), ESearchCase::IgnoreCase) && Expr->HasAParameterName())
    {
        Expr->SetParameterName(FName(*Value));
        bHandled = true;
    }
    else if (UMaterialExpressionConstant* C = Cast<UMaterialExpressionConstant>(Expr))
    {
        C->R = FCString::Atof(*Value);
        bHandled = true;
    }
    else if (UMaterialExpressionScalarParameter* S = Cast<UMaterialExpressionScalarParameter>(Expr))
    {
        S->DefaultValue = FCString::Atof(*Value);
        bHandled = true;
    }
    else if (UMaterialExpressionConstant3Vector* C3 = Cast<UMaterialExpressionConstant3Vector>(Expr))
    {
        FLinearColor LC; LC.InitFromString(Value); C3->Constant = LC; bHandled = true;
    }
    else if (UMaterialExpressionConstant4Vector* C4 = Cast<UMaterialExpressionConstant4Vector>(Expr))
    {
        FLinearColor LC; LC.InitFromString(Value); C4->Constant = LC; bHandled = true;
    }
    else if (UMaterialExpressionVectorParameter* V = Cast<UMaterialExpressionVectorParameter>(Expr))
    {
        FLinearColor LC; LC.InitFromString(Value); V->DefaultValue = LC; bHandled = true;
    }
    else
    {
        // Reflection fallback: set a named FProperty on the expression by import text.
        if (FProperty* Property = FindFProperty<FProperty>(Expr->GetClass(), FName(*ParamName)))
        {
            void* Addr = Property->ContainerPtrToValuePtr<void>(Expr);
            Property->ImportText_Direct(*Value, Addr, Expr, PPF_None);
            bHandled = true;
        }
    }

    if (!bHandled)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Could not set '%s' on expression of type %s"), *ParamName, *Expr->GetClass()->GetName()));

    Expr->PostEditChange();
    Material->PostEditChange();
    Material->MarkPackageDirty();

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material"), Material->GetName());
    Result->SetStringField(TEXT("node_id"), NodeId);
    Result->SetStringField(TEXT("param_name"), ParamName);
    Result->SetStringField(TEXT("value"), Value);
    return Result;
}

// ===========================================================================
// set_material_instance_param -- two engine traps in one tool
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

// MCP:set_material_instance_param - Set a parameter on a Material Instance Constant, then RE-READ it and report what the asset actually holds. `set` is derived from that re-read, NOT from the engine setter's return value -- UMaterialEditingLibrary::SetMaterialInstance*ParameterValue always returns false (its bResult is never assigned), which would make this tool report a failure it had not had. Vector values are parsed explicitly in three formats and a malformed one is a hard ERROR instead of a silent (0,0,0,1).
//   @param instance_path - Material instance asset path.
//   @param param_name - Parameter name.
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
                    TEXT("set_material_instance_param: %s"), *ParseError));
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

// MCP:compile_material - Recompile/refresh a material asset.
//   @param material_path - Material asset path
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

    // node_count, nodes (with their inputs) and the flattened connections of an expression graph.
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

            // Per-class inputs via GetInput(i)/CountInputs() (GetInputsView() is deprecated).
            TArray<TSharedPtr<FJsonValue>> InputsJson;
            const int32 NumInputs = Expr->CountInputs();
            for (int32 i = 0; i < NumInputs; ++i)
            {
                FExpressionInput* In = Expr->GetInput(i);
                if (!In) { continue; }
                TSharedPtr<FJsonObject> IObj = MakeShared<FJsonObject>();
                IObj->SetNumberField(TEXT("index"), i);
                IObj->SetStringField(TEXT("input_name"), In->InputName.ToString());
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
                    CObj->SetStringField(TEXT("to_input"), In->InputName.ToString());
                    Connections.Add(MakeShared<FJsonValueObject>(CObj));
                }
                InputsJson.Add(MakeShared<FJsonValueObject>(IObj));
            }
            NObj->SetArrayField(TEXT("inputs"), InputsJson);
            NObj->SetNumberField(TEXT("input_count"), NumInputs);
            Nodes.Add(MakeShared<FJsonValueObject>(NObj));
        }

        Result->SetNumberField(TEXT("node_count"), Expressions.Num());
        Result->SetArrayField(TEXT("nodes"), Nodes);
        Result->SetNumberField(TEXT("connection_count"), Connections.Num());
        Result->SetArrayField(TEXT("connections"), Connections);
    }

    // Saves the package Asset lives in; false when it has none or the save fails.
    bool SaveOwningPackage(UObject* Asset)
    {
        FString SaveError;
        UPackage* Package = Asset->GetPackage();
        return Package && mcp_common::SaveAssetPackage(Package, Asset, SaveError);
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

// MCP:create_material_function - Create and save a new Material Function asset (UMaterialFunction).
//   @param package_path - /Game package directory, e.g. '/Game/MyGame'
//   @param name - Asset name for the new material function
//   @param description - Optional description for the material function
//   @returns { function_path: string, name: string, package_path: string, description: string, num_inputs: number, num_outputs: number, created: bool, saved: bool, already_exists: bool? }

TSharedPtr<FJsonObject> HandleCreateMaterialFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, Name, Description;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    Params->TryGetStringField(TEXT("description"), Description);

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
            R->SetStringField(TEXT("name"), Existing->GetName());
            R->SetStringField(TEXT("package_path"), PackagePath);
            R->SetNumberField(TEXT("num_inputs"), NumInputs);
            R->SetNumberField(TEXT("num_outputs"), NumOutputs);
            R->SetBoolField(TEXT("already_exists"), true);
            R->SetBoolField(TEXT("created"), true);
            return R;
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Asset exists but is not a UMaterialFunction: %s"), *FullPath));
    }

    // Create via the real editor factory so the inner objects are set up correctly.
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools");
    IAssetTools& AssetTools = AssetToolsModule.Get();

    UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
    if (!Factory)
        return mcp_common::MakeErrorReply(TEXT("Failed to create UMaterialFunctionFactoryNew"));

    UObject* CreatedAsset = AssetTools.CreateAsset(Name, PackagePath, UMaterialFunction::StaticClass(), Factory);
    UMaterialFunction* Function = Cast<UMaterialFunction>(CreatedAsset);
    if (!Function)
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to create material function: %s"), *FullPath));

    Function->Modify();
    if (!Description.IsEmpty())
    {
        Function->Description = Description;
    }
    // Expose to the material function library so it behaves like an editor-created one.
    Function->bExposeToLibrary = true;
    Function->PostEditChange();
    Function->MarkPackageDirty();

    // Save the package to disk.
    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Function);

    int32 NumInputs = 0, NumOutputs = 0;
    CountFunctionIO(Function, NumInputs, NumOutputs);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("function_path"), Function->GetPathName());
    Result->SetStringField(TEXT("name"), Function->GetName());
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetStringField(TEXT("description"), Function->Description);
    Result->SetNumberField(TEXT("num_inputs"), NumInputs);
    Result->SetNumberField(TEXT("num_outputs"), NumOutputs);
    Result->SetBoolField(TEXT("created"), true);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

// MCP:add_material_function_input - Add a UMaterialExpressionFunctionInput node to a material function.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param input_name - Name of the new input pin
//   @param input_type - One of the 13 EFunctionInputType values: Scalar|Vector2|Vector3|Vector4|Texture2D|TextureCube|Texture2DArray|VolumeTexture|StaticBool|MaterialAttributes|TextureExternal|Bool|Substrate (optional, default "Scalar")
//   @param description - Optional tooltip/description string for the input pin
//   @param preview_value - Optional {x,y,z,w} (FVector4f) preview value used when previewing / as default
//   @param use_preview_value_as_default - Optional bool; if true, the preview value becomes the input's default
//   @param sort_priority - Optional int controlling the input's display order relative to other inputs
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

// MCP:add_material_function_output - Add a UMaterialExpressionFunctionOutput node to a material function.          '/Game/MyGame/MF_Example.MF_Example'
//   @param function_path - Full path to the material function, e.g.
//   @param output_name - Name of the new output pin
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

// MCP:set_material_function_input_props - Set properties on an existing UMaterialExpressionFunctionInput (located by node_id).  Partial update (idempotent): only fields present in Params are written; mirrors everything get_material_function_inputs reads. ValidateName is re-run when input_name changes. Also writes InputType/SortPriority/bUsePreviewValueAsDefault, which the reader exposes.
//   @param function_path - Full path to the material function, e.g. '/Game/MyGame/MF_Example.MF_Example'
//   @param node_id - The input node id (UMaterialExpression name) from add_material_function_input
//   @param input_name - Optional new pin name (re-validated for uniqueness)
//   @param input_type - Optional EFunctionInputType (the 13 values; same parser as add_material_function_input)
//   @param description - Optional tooltip/description string
//   @param preview_value - Optional {x,y,z,w} (FVector4f) preview value
//   @param use_preview_value_as_default - Optional bool
//   @param sort_priority - Optional int display-order
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

// MCP:create_post_process_material - Create a UMaterial configured as a post-process material (MD_PostProcess).
//   @param package_path - /Game package directory, e.g. '/Game/MyGame'
//   @param name - Asset name for the new material
//   @returns { material_path: string, name: string, package_path: string, domain: string, blendable_location: number, is_post_process: bool, created: bool, already_exists: bool, saved: bool }

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

    // Configure as a post-process material.
    Material->PreEditChange(nullptr);
    Material->Modify();
    Material->MaterialDomain = MD_PostProcess;
    Material->BlendableLocation = BL_SceneColorAfterTonemapping;
    Material->PostEditChange();
    UMaterialEditingLibrary::RecompileMaterial(Material);
    Material->MarkPackageDirty();

    // Save the package.
    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Material);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    Result->SetStringField(TEXT("name"), Material->GetName());
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetStringField(TEXT("domain"), MaterialDomainToString((EMaterialDomain)Material->MaterialDomain.GetValue()));
    Result->SetNumberField(TEXT("blendable_location"), (int32)Material->BlendableLocation.GetValue());
    Result->SetBoolField(TEXT("is_post_process"), Material->MaterialDomain == MD_PostProcess);
    Result->SetBoolField(TEXT("created"), true);
    Result->SetBoolField(TEXT("already_exists"), bAlreadyExists);
    Result->SetBoolField(TEXT("saved"), bSaved);
    return Result;
}

// MCP:set_material_domain - Set a material's domain (EMaterialDomain) and recompile it.          'LightFunction', 'Volume'
//   @param material_path - Full path to the material, e.g. '/Game/MyGame/M_Example.M_Example'
//   @param domain - One of 'Surface', 'PostProcess', 'UI', 'DeferredDecal',
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

// MCP:duplicate_material_expression - Duplicate a material expression (in-place) and optionally rename.
//   @param material_path - Full asset path of the UMaterial.
//   @param node_id - Name of the source expression.
//   @param new_node_name - Optional rename for the duplicate (optional)
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

// MCP:get_material_graph_info - Read the full expression graph of a UMaterial (nodes + connections).  Enumerates UMaterial::GetExpressions() and walks each expression's FExpressionInput Inputs[] array. For every connected input, the connection is also flattened into the top-level connections[] list for easy graph-walking.
//   @param material_path - Full asset path of the UMaterial.
//   @returns { material_path: string, node_count: number, nodes: [{ name: string, class: string, inputs: [{ index: number, input_name: string, source?: string, source_output_index?: number }], input_count: number }], connection_count: number, connections: [{ from: string, from_output_index: number, to: string, to_input: string }] }

TSharedPtr<FJsonObject> HandleGetMaterialGraphInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

    UMaterial* Material = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindMaterial(MaterialPath, Material)) { return Error; }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_path"), Material->GetPathName());
    UEAgentMaterialExtraImpl::AddGraphFields(Result, Material->GetExpressions());
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
//   get_material_function_inputs        enumerate UMaterialExpressionFunctionInput.
//   delete_material_function            delete the UMaterialFunction asset.
// ========================================================================== //

// MCP:recompile_material_function - Recompile (UpdateMaterialFunction) a UMaterialFunction and save it.
//   @param material_function_path - Asset path of the UMaterialFunction
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

// MCP:create_material_function_instance - Create a UMaterialFunctionInstance (asset, with parent set) and save it.
//   @param package_path - Target /Game package path (auto-prefixed with /Game/ if missing)
//   @param instance_name - Name for the new UMaterialFunctionInstance asset
//   @param parent_function_path - Asset path of the parent UMaterialFunctionInterface
//   @returns { asset_path: string, instance_name: string, parent_function_path: string, created: bool, already_existed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleCreateMaterialFunctionInstance(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath, InstanceName, ParentPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), PackagePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_name"), InstanceName)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_function_path"), ParentPath)) { return Missing; }

    if (!PackagePath.StartsWith(TEXT("/Game/")))
    {
        PackagePath = TEXT("/Game/") + PackagePath;
    }
    const FString FullPath = PackagePath + TEXT("/") + InstanceName;
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        // Idempotent: return the existing instance, like create_material_function does.
        UMaterialFunctionInstance* Existing = LoadObject<UMaterialFunctionInstance>(nullptr, *FullPath);
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("asset_path"), FullPath);
        Result->SetStringField(TEXT("instance_name"), InstanceName);
        Result->SetBoolField(TEXT("created"), false);
        Result->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(Result);
    }
    if (!UEditorAssetLibrary::DoesDirectoryExist(PackagePath))
    {
        UEditorAssetLibrary::MakeDirectory(PackagePath);
    }

    UMaterialFunctionInterface* Parent = LoadObject<UMaterialFunctionInterface>(nullptr, *ParentPath);
    if (!Parent)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Parent function not found: %s"), *ParentPath));
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

    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(CreatedAsset);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("asset_path"), FullPath);
    Result->SetStringField(TEXT("instance_name"), InstanceName);
    Result->SetStringField(TEXT("parent_function_path"), Parent->GetPathName());
    Result->SetBoolField(TEXT("created"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("create_material_function_instance"));
}

// MCP:set_material_function_instance_parent - Re-parent a UMaterialFunctionInstance and save it.
//   @param instance_path - Asset path of the UMaterialFunctionInstance to re-parent
//   @param parent_function_path - Asset path of the new parent UMaterialFunctionInterface
//   @returns { instance_path: string, old_parent: string, new_parent: string, applied: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetMaterialFunctionInstanceParent(const TSharedPtr<FJsonObject>& Params)
{
    FString InstancePath, ParentPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("instance_path"), InstancePath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parent_function_path"), ParentPath)) { return Missing; }

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
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetMaterialFunctionInstanceParent", "Set Material Function Instance Parent (MCP)"));
    Instance->Modify();
    Instance->SetParent(Parent);

    const bool bSaved = UEAgentMaterialExtraImpl::SaveOwningPackage(Instance);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("instance_path"), Instance->GetPathName());
    Result->SetStringField(TEXT("old_parent"), OldParent);
    Result->SetStringField(TEXT("new_parent"), Instance->Parent ? Instance->Parent->GetPathName() : TEXT("(none)"));
    Result->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_material_function_instance_parent"));
}

// MCP:get_material_function_inputs - Enumerate UMaterialFunctionInput / UMaterialFunctionOutput expressions inside a UMaterialFunction.
//   @param material_function_path - Asset path of the UMaterialFunction
//   @returns { material_function_path: string, input_count: number, output_count: number, inputs: [{ node_id: string, input_name: string, input_type: string, description: string, sort_priority: number, use_preview_value: bool, preview_value: {x,y,z,w} }], outputs: [{ node_id: string, output_name: string, description: string, sort_priority: number }] }
TSharedPtr<FJsonObject> HandleGetMaterialFunctionInputs(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_function_path"), FunctionPath)) { return Missing; }

    UMaterialFunction* Function = LoadObject<UMaterialFunction>(nullptr, *FunctionPath);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function not found: %s"), *FunctionPath));
    }

    TArray<TSharedPtr<FJsonValue>> Inputs;
    TArray<TSharedPtr<FJsonValue>> Outputs;
    for (UMaterialExpression* Expr : Function->GetExpressions())
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
    Result->SetStringField(TEXT("material_function_path"), Function->GetPathName());
    Result->SetNumberField(TEXT("input_count"), Inputs.Num());
    Result->SetNumberField(TEXT("output_count"), Outputs.Num());
    Result->SetArrayField(TEXT("inputs"), Inputs);
    Result->SetArrayField(TEXT("outputs"), Outputs);
    return mcp_common::MakeOkReply(Result);
}

// MCP:delete_material_function - Delete a UMaterialFunction asset from the project.
//   @param material_function_path - Asset path of the UMaterialFunction to delete
//   @returns { material_function_path: string, deleted: bool }
TSharedPtr<FJsonObject> HandleDeleteMaterialFunction(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_function_path"), FunctionPath)) { return Missing; }

    if (!UEditorAssetLibrary::DoesAssetExist(FunctionPath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material function asset not found: %s"), *FunctionPath));
    }

    const bool bDeleted = UEditorAssetLibrary::DeleteAsset(FunctionPath);
    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_function_path"), FunctionPath);
    Result->SetBoolField(TEXT("deleted"), bDeleted);
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
// MCP:create_subsurface_profile - Create a USubsurfaceProfile data asset (NewObject + save).  Idempotent: if the asset already exists it is loaded and returned.
//   @param name - Asset name (e.g. SSP_Example)
//   @param package_path - Target /Game package path (optional, default "/Game/Gameplay")
//   @returns { name: string, asset_path: string, asset_class: string, mean_free_path_distance: number, is_subsurface_profile: bool, already_existed: bool, created: bool }

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

        FString SaveError;
        if (!mcp_common::SaveAssetPackage(Package, Profile, SaveError))
        {
            return mcp_common::MakeErrorReply(SaveError);
        }
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
    Result->SetBoolField(TEXT("created"), true);
    return mcp_common::MakeOkReply(Result);
}

// ========================================================================== //
// MaterialFunction internal graph editing (get + create)
//
// The MaterialFunction handlers above are scoped to Input/Output pins
// (add/delete_material_function_input/output, get_material_function_inputs) or
// wire pins by name (connect_material_function_pins). None reach the
// function's *internal* expression graph (Multiply, Power, Constant,
// VertexNormalWS...) the way get_material_graph_info/create_material_expression
// do for a plain UMaterial. These two commands are the function-scoped twins of
// those two UMaterial commands, same response shape, same conventions:
//   get_material_function_graph          Function->GetExpressions() walk,
//                                         identical to get_material_graph_info.
//   create_material_function_expression  UMaterialEditingLibrary::
//                                         CreateMaterialExpressionInFunction —
//                                         verified generic (any UMaterialExpression
//                                         subclass, not limited to FunctionInput/
//                                         Output; MaterialEditingLibrary.h).
// ========================================================================== //

// MCP:get_material_function_graph - Enumerate a UMaterialFunction's internal expression graph: every UMaterialExpression node plus the FExpressionInput connections between them (same shape as get_material_graph_info, scoped to a function instead of a material).
//   @param material_function_path - Asset path of the UMaterialFunction
//   @returns { material_function_path: string, node_count: number, nodes: [{name, class, inputs:[{index, input_name, source, source_output_index}], input_count}], connection_count: number, connections: [{from, from_output_index, to, to_input}] }
TSharedPtr<FJsonObject> HandleGetMaterialFunctionGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_function_path"), FunctionPath)) { return Missing; }

    UMaterialFunction* Function = nullptr;
    if (TSharedPtr<FJsonObject> Error = UEAgentMaterialExtraImpl::FindFunction(FunctionPath, Function)) { return Error; }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("material_function_path"), Function->GetPathName());
    UEAgentMaterialExtraImpl::AddGraphFields(Result, Function->GetExpressions());
    return Result;
}

namespace UEAgentMaterialExtraImpl
{
    // Resolves an expression class from a short name (a class that is already loaded) or, when the text
    // looks like an object path, by loading it. Classes that do not derive from UMaterialExpression are
    // refused. On failure returns nullptr with OutError holding the reply.
    UClass* ResolveExpressionClass(const FString& ClassName, TSharedPtr<FJsonObject>& OutError)
    {
        UClass* Resolved = FindObject<UClass>(nullptr, *ClassName);
        if (Resolved == nullptr && ClassName.Contains(TEXT("/")))
        {
            Resolved = LoadClass<UMaterialExpression>(nullptr, *ClassName);
        }

        if (Resolved == nullptr)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Material expression class not found: %s. Use full path like '/Script/Engine.MaterialExpressionMultiply'"), *ClassName));
        }
        else if (!Resolved->IsChildOf(UMaterialExpression::StaticClass()))
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Class is not a MaterialExpression: %s"), *ClassName));
            Resolved = nullptr;
        }
        return Resolved;
    }
}

// MCP:create_material_function_expression - Create a new UMaterialExpression node inside a UMaterialFunction's internal graph (same shape as create_material_expression, scoped to a function instead of a material). Use connect_material_function_pins to wire it and recompile_material_function to recompile.
//   @param material_function_path - Asset path of the UMaterialFunction
//   @param expression_class_name - Class name of the UMaterialExpression to create, e.g. 'MaterialExpressionMultiply' or a full path like '/Script/Engine.MaterialExpressionPower'
//   @param node_pos_x - Optional X position in the function's graph editor (default 0)
//   @param node_pos_y - Optional Y position in the function's graph editor (default 0)
//   @returns { material_function_path: string, expression_class_name: string, expression_name: string, node_pos_x: number, node_pos_y: number, success: bool }
TSharedPtr<FJsonObject> HandleCreateMaterialFunctionExpression(const TSharedPtr<FJsonObject>& Params)
{
    FString FunctionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_function_path"), FunctionPath)) { return Missing; }

    FString ExpressionClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("expression_class_name"), ExpressionClassName)) { return Missing; }

    // The function is resolved before the class, so an unknown function is reported first.
    UMaterialFunction* Function = AssetPathUtils::LoadOrLoadSoftPathString<UMaterialFunction>(FunctionPath);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Material function not found: %s. Use full path like '/Game/Materials/MF_MyFunction.MF_MyFunction'"), *FunctionPath));
    }

    TSharedPtr<FJsonObject> ClassError;
    UClass* ExpressionClass = UEAgentMaterialExtraImpl::ResolveExpressionClass(ExpressionClassName, ClassError);
    if (!ExpressionClass)
    {
        return ClassError;
    }

    // Optional placement in the function graph; both coordinates default to the origin.
    FIntPoint Placement(0, 0);
    Params->TryGetNumberField(TEXT("node_pos_x"), Placement.X);
    Params->TryGetNumberField(TEXT("node_pos_y"), Placement.Y);

    // Function-scoped counterpart of UMaterialEditingLibrary::CreateMaterialExpression: it forwards to
    // CreateMaterialExpressionEx with the function as owner, so any expression class can be placed here,
    // not only the function input/output nodes.
    UMaterialExpression* NewNode = UMaterialEditingLibrary::CreateMaterialExpressionInFunction(Function, ExpressionClass, Placement.X, Placement.Y);
    if (!NewNode)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create material expression in function"));
    }
    Function->MarkPackageDirty();

    TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
    Reply->SetBoolField(TEXT("success"), true);
    Reply->SetStringField(TEXT("material_function_path"), Function->GetPathName());
    Reply->SetStringField(TEXT("expression_name"), NewNode->GetName());
    Reply->SetStringField(TEXT("expression_class_name"), ExpressionClassName);
    Reply->SetNumberField(TEXT("node_pos_x"), Placement.X);
    Reply->SetNumberField(TEXT("node_pos_y"), Placement.Y);
    return Reply;
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
    { TEXT("create_material_function_expression"), &HandleCreateMaterialFunctionExpression },
    { TEXT("create_material_function_instance"), &HandleCreateMaterialFunctionInstance },
    { TEXT("create_post_process_material"), &HandleCreatePostProcessMaterial },
    { TEXT("create_subsurface_profile"), &HandleCreateSubsurfaceProfile },
    { TEXT("delete_material_function"), &HandleDeleteMaterialFunction },
    { TEXT("delete_material_function_input"), &HandleDeleteMaterialFunctionInput },
    { TEXT("delete_material_function_output"), &HandleDeleteMaterialFunctionOutput },
    { TEXT("disconnect_to_material_output"), &HandleDisconnectToMaterialOutput },
    { TEXT("duplicate_material_expression"), &HandleDuplicateMaterialExpression },
    { TEXT("get_material_function_graph"), &HandleGetMaterialFunctionGraph },
    { TEXT("get_material_function_inputs"), &HandleGetMaterialFunctionInputs },
    { TEXT("get_material_graph_info"), &HandleGetMaterialGraphInfo },
    { TEXT("recompile_material_function"), &HandleRecompileMaterialFunction },
    { TEXT("set_material_domain"), &HandleSetMaterialDomain },
    { TEXT("set_material_function_input_props"), &HandleSetMaterialFunctionInputProps },
    { TEXT("set_material_function_instance_parent"), &HandleSetMaterialFunctionInstanceParent },
    { TEXT("set_material_function_output_props"), &HandleSetMaterialFunctionOutputProps },
    { TEXT("set_material_instance_param"), &HandleSetMaterialInstanceParam },
    { TEXT("set_material_node_param"), &HandleSetMaterialNodeParam },
};
} // namespace UEAgentMaterialExtraCommands
