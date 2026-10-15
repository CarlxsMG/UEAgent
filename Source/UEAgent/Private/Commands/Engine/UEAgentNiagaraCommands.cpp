#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

#include "JsonObjectConverter.h"                             // FJsonObjectConverter::UStructToJsonObject

// Niagara runtime
#include "NiagaraSystem.h"                                  // UNiagaraSystem
#include "NiagaraEmitter.h"                                 // UNiagaraEmitter (for AddEmitterHandle source)
#include "NiagaraComponent.h"                               // UNiagaraComponent
#include "NiagaraFunctionLibrary.h"                         // UNiagaraFunctionLibrary::SpawnSystemAtLocation
#include "NiagaraTypes.h"                                   // FNiagaraVariable / FNiagaraTypeDefinition
#include "NiagaraUserRedirectionParameterStore.h"           // FNiagaraUserRedirectionParameterStore
#include "NiagaraCommon.h"                                  // common Niagara types
#include "NiagaraComponentPoolMethodEnum.h"                 // ENCPoolMethod::None

// Niagara data-interface content authoring (curve keys + array values, via reflection)
#include "Curves/RichCurve.h"                               // FRichCurve (curve DIs: Curve / Red|Green|Blue|AlphaCurve / X|Y|ZCurve)
#include "NiagaraDataInterfaceCurveBase.h"                  // UNiagaraDataInterfaceCurveBase::UpdateLUT / UpdateTimeRanges
#include "UObject/UnrealType.h"                             // FStructProperty / FArrayProperty / FNumericProperty / FScriptArrayHelper

// Niagara editor
#include "NiagaraSystemFactoryNew.h"                        // UNiagaraSystemFactoryNew::InitializeSystem

// Niagara simulation advanced
#include "NiagaraSimulationStageBase.h"                     // UNiagaraSimulationStageGeneric
#include "NiagaraScript.h"                                  // UNiagaraScript (sim stage needs a Script)
#include "NiagaraEffectType.h"                              // UNiagaraEffectType

// Emitter from template + module-to-stack
#include "NiagaraEditorUtilities.h"                         // FNiagaraEditorUtilities::AddEmitterToSystem
#include "NiagaraScriptSource.h"                            // UNiagaraScriptSource (NodeGraph)
#include "NiagaraGraph.h"                                   // UNiagaraGraph::FindOutputNode
#include "NiagaraNodeOutput.h"                              // UNiagaraNodeOutput
#include "NiagaraNodeFunctionCall.h"                        // UNiagaraNodeFunctionCall
#include "ViewModels/Stack/NiagaraStackGraphUtilities.h"    // FNiagaraStackGraphUtilities::AddScriptModuleToStack

// Niagara module input override (rapid-iteration parameter store)
#include "NiagaraParameterMapHistory.h"                     // FCompileConstantResolver (inline)
#include "NiagaraParameterStore.h"                          // FNiagaraParameterStore (RapidIterationParameters)
#include "ViewModels/Stack/NiagaraParameterHandle.h"        // FNiagaraParameterHandle::CreateAliasedModuleParameterHandle

// Niagara renderers (Sprite / Ribbon / Mesh) + data interface bindings
#include "NiagaraRendererProperties.h"                      // UNiagaraRendererProperties (base)
#include "NiagaraSpriteRendererProperties.h"                 // UNiagaraSpriteRendererProperties
#include "NiagaraRibbonRendererProperties.h"                 // UNiagaraRibbonRendererProperties
#include "NiagaraMeshRendererProperties.h"                   // UNiagaraMeshRendererProperties
#include "NiagaraLightRendererProperties.h"                  // UNiagaraLightRendererProperties
#include "NiagaraDecalRendererProperties.h"                  // UNiagaraDecalRendererProperties
#include "Materials/MaterialInterface.h"                    // UMaterialInterface for set_niagara_decal_renderer_material
// TObjectIterator<UClass> for list_niagara_renderer_classes.
#include "UObject/UObjectIterator.h"
#include "NiagaraMeshRendererMeshProperties.h"               // FNiagaraMeshRendererMeshProperties (.Mesh slot)
#include "NiagaraDataInterface.h"                            // UNiagaraDataInterface (base)
#include "Engine/StaticMesh.h"                               // UStaticMesh (mesh assignment)

// Engine / editor
#include "Editor.h"                                         // GEditor
#include "GameFramework/Actor.h"
#include "EngineUtils.h"                                    // TActorIterator
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/PackageName.h"

/**
 * Niagara VFX commands on UNiagaraSystem assets (their emitters, modules, renderers, data interfaces and
 * simulation stages) and on the UNiagaraComponent instances spawned in the editor world.
 *
 * Requires the Niagara plugin (runtime module Niagara, editor module NiagaraEditor).
 */
namespace UEAgentNiagaraCommands
{

// MCP-DOMAIN: Engine

namespace
{
    // Resolve a UNiagaraComponent in the editor world by its owner actor. The
    // identifier may match either the owner's internal object name (AActor::GetName,
    // which is what delete_actor uses) or its editor label (AActor::GetActorLabel).
    // Returns the first UNiagaraComponent found on that actor, or nullptr.
    UNiagaraComponent* FindNiagaraComponentByOwner(const FString& ActorIdentifier, AActor*& OutOwner)
    {
        OutOwner = nullptr;
        if (ActorIdentifier.IsEmpty())
        {
            return nullptr;
        }
        UWorld* World = mcp_common::GetEditorWorld();
        if (!World)
        {
            return nullptr;
        }
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (!Actor)
            {
                continue;
            }
            if (Actor->GetName() == ActorIdentifier || Actor->GetActorLabel() == ActorIdentifier)
            {
                UNiagaraComponent* Comp = Actor->FindComponentByClass<UNiagaraComponent>();
                if (Comp)
                {
                    OutOwner = Actor;
                    return Comp;
                }
            }
        }
        return nullptr;
    }

    // True when the caller passed a non-empty `system_path`, read into OutSystemPath; the caller replies otherwise.
    // Read with ReadRequiredString so that the catalog publishes system_path as required: every caller fails without it.
    bool ExtractNiagaraSystemPath(const TSharedPtr<FJsonObject>& Params, FString& OutSystemPath)
    {
        return !mcp_common::ReadRequiredString(Params, TEXT("system_path"), OutSystemPath).IsValid();
    }

    // The system at SystemPath; null with OutError set when there is none.
    UNiagaraSystem* Niagara_ResolveSystem(const FString& SystemPath, FString& OutError)
    {
        UNiagaraSystem* System = mcp_common::ResolveAsset<UNiagaraSystem>(SystemPath);
        if (!System)
        {
            OutError = FString::Printf(TEXT("Niagara system not found: %s"), *SystemPath);
        }
        return System;
    }

    // The system named by the required `system_path` param; null on success, else the error reply.
    TSharedPtr<FJsonObject> Niagara_LoadSystem(const TSharedPtr<FJsonObject>& Params, UNiagaraSystem*& OutSystem)
    {
        FString SystemPath;
        if (!ExtractNiagaraSystemPath(Params, SystemPath))
        {
            return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
        }
        FString Error;
        OutSystem = Niagara_ResolveSystem(SystemPath, Error);
        if (!OutSystem)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    // Reads the required `system_path`, then `parameter_name`; false with OutError set when one is missing.
    bool Niagara_ReadSystemAndParameterNames(const TSharedPtr<FJsonObject>& Params,
        FString& OutSystemPath, FString& OutParameterName, FString& OutError)
    {
        // Read here rather than through ExtractNiagaraSystemPath: the catalog generator follows calls two
        // deep, and this helper is already the second one under Niagara_LoadParameterSystem.
        if (mcp_common::ReadRequiredString(Params, TEXT("system_path"), OutSystemPath).IsValid())
        {
            OutError = TEXT("Missing required 'system_path' parameter");
            return false;
        }
        if (!Params->TryGetStringField(TEXT("parameter_name"), OutParameterName) || OutParameterName.IsEmpty())
        {
            OutError = TEXT("Missing required 'parameter_name' parameter");
            return false;
        }
        return true;
    }

    // Niagara_ReadSystemAndParameterNames, then Niagara_ResolveSystem; null on success, else the error reply.
    TSharedPtr<FJsonObject> Niagara_LoadParameterSystem(const TSharedPtr<FJsonObject>& Params, FString& OutParameterName,
        UNiagaraSystem*& OutSystem)
    {
        FString SystemPath;
        FString Error;
        if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, OutParameterName, Error))
        {
            return mcp_common::MakeErrorReply(Error);
        }
        OutSystem = Niagara_ResolveSystem(SystemPath, Error);
        if (!OutSystem)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    // A reply object that opens with the system's "system_path".
    TSharedPtr<FJsonObject> Niagara_StartSystemReply(const UNiagaraSystem* System)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("system_path"), System->GetPathName());
        return Data;
    }

    // Niagara_StartSystemReply, then "emitter_name".
    TSharedPtr<FJsonObject> Niagara_StartEmitterReply(const UNiagaraSystem* System, const FString& EmitterName)
    {
        TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
        Data->SetStringField(TEXT("emitter_name"), EmitterName);
        return Data;
    }

    // Niagara_StartSystemReply, then "parameter_name".
    TSharedPtr<FJsonObject> Niagara_StartParameterReply(const UNiagaraSystem* System, const FString& ParameterName)
    {
        TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
        Data->SetStringField(TEXT("parameter_name"), ParameterName);
        return Data;
    }
}

// ---------------------------------------------------------------------------
// create_niagara_system
// Creates (or loads, idempotently) an empty UNiagaraSystem under a /Game path
// and saves it. Uses NewObject + UNiagaraSystemFactoryNew::InitializeSystem
// (NIAGARAEDITOR_API) instead of IAssetTools::CreateAsset with the factory,
// because the factory's ConfigureProperties() opens a modal asset-browser window
// (NOT headless-safe). InitializeSystem sets up the system spawn/update scripts.
// ---------------------------------------------------------------------------
// MCP:create_niagara_system - Creates (or loads, idempotently) an empty UNiagaraSystem asset and saves it.  Uses NewObject + UNiagaraSystemFactoryNew::InitializeSystem (headless-safe); the new system has its system spawn/update scripts wired but no emitters.
//   @param name - Name of the Niagara System asset to create.
//   @param package_path - Target content folder; defaults to /Game/Niagara
//   @returns { system_name: string, asset_path: string, created: bool, already_exists?: bool, saved?: bool, save_error?: string, emitter_count: int }

TSharedPtr<FJsonObject> HandleCreateNiagaraSystem(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath = TEXT("/Game/Niagara");
    Params->TryGetStringField(TEXT("package_path"), PackagePath);
    PackagePath.RemoveFromEnd(TEXT("/"));

    FString Name;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name)) { return Missing; }

    const FString PackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(PackageName)) { return Invalid; }
    const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *Name);

    // Idempotent: if it already exists, load and return.
    if (UNiagaraSystem* Existing = LoadObject<UNiagaraSystem>(nullptr, *ObjectPath))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("system_name"), Existing->GetName());
        Data->SetStringField(TEXT("asset_path"), ObjectPath);
        Data->SetBoolField(TEXT("created"), false);
        Data->SetBoolField(TEXT("already_exists"), true);
        Data->SetNumberField(TEXT("emitter_count"), Existing->GetEmitterHandles().Num());
        return mcp_common::MakeOkReply(Data);
    }

    FString CreateError;
    UNiagaraSystem* NewSystem = mcp_common::NewAssetInPackage<UNiagaraSystem>(PackageName, Name,
        TEXT("Failed to create UNiagaraSystem object"), CreateError);
    if (!NewSystem)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    // Wire up the system scripts (empty system, no default emitter -> headless-safe).
    // bCreateDefaultNodes=false avoids loading the default editor module script;
    // the system is still a valid, saveable empty system.
    UNiagaraSystemFactoryNew::InitializeSystem(NewSystem, /*bCreateDefaultNodes*/ false);

    FAssetRegistryModule::AssetCreated(NewSystem);
    NewSystem->MarkPackageDirty();

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewSystem->GetPackage(), NewSystem, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("system_name"), NewSystem->GetName());
    Data->SetStringField(TEXT("asset_path"), ObjectPath);
    Data->SetBoolField(TEXT("created"), true);
    Data->SetNumberField(TEXT("emitter_count"), NewSystem->GetEmitterHandles().Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("create_niagara_system"), SaveError);
}

// ---------------------------------------------------------------------------
// get_niagara_system_info
// Reads emitter count, system name, warmup time and the number of user-exposed parameters.
// ---------------------------------------------------------------------------
// MCP:get_niagara_system_info - Reads metadata for a UNiagaraSystem: emitter count, warmup time and exposed user parameters.
//   @param system_path - Asset path of the Niagara System (e.g. /Game/Niagara/NS_Example or /Game/Niagara/NS_Example.NS_Example)
//   @returns { system_name: string, system_path: string, emitter_count: int, num_user_parameters: int, warmup_time: number, user_parameters: [{ name, type }] }

TSharedPtr<FJsonObject> HandleGetNiagaraSystemInfo(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    const FNiagaraUserRedirectionParameterStore& ExposedParams = System->GetExposedParameters();
    TArray<FNiagaraVariable> UserVars;
    ExposedParams.GetUserParameters(UserVars);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("system_name"), System->GetName());
    Data->SetStringField(TEXT("system_path"), System->GetPathName());
    Data->SetNumberField(TEXT("emitter_count"), System->GetEmitterHandles().Num());
    Data->SetNumberField(TEXT("num_user_parameters"), UserVars.Num());
    Data->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());

    TArray<TSharedPtr<FJsonValue>> ParamArray;
    for (const FNiagaraVariable& Var : UserVars)
    {
        TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
        P->SetStringField(TEXT("name"), Var.GetName().ToString());
        P->SetStringField(TEXT("type"), Var.GetType().GetName());
        ParamArray.Add(MakeShared<FJsonValueObject>(P));
    }
    Data->SetArrayField(TEXT("user_parameters"), ParamArray);

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// User-parameter type resolution.
//
// Niagara's POD user-param types each have an inline-static FNiagaraTypeDefinition
// accessor (NiagaraTypes.h) and FNiagaraParameterStore::SetParameterValue<T>
// (NiagaraParameterStore.h) writes the value generically (it asserts
// Param.GetSizeInBytes() == sizeof(T), so the FNiagaraVariable's TypeDef and the C++
// payload type MUST agree). The mapping below is the single source of truth shared by
// the setter and the reader so a round-trip is exact:
//   Float  -> float            GetFloatDef
//   Vector -> FVector3f        GetVec3Def
//   Vec2   -> FVector2f        GetVec2Def
//   Vec4   -> FVector4f        GetVec4Def
//   Color  -> FLinearColor     GetColorDef
//   Bool   -> FNiagaraBool     GetBoolDef   (NOT raw C++ bool; FNiagaraBool wraps int32)
//   Int    -> int32            GetIntDef
// ---------------------------------------------------------------------------
namespace
{
    // Canonical type token resolution (case-insensitive). Returns false on an
    // unknown token so the caller can emit an honest error instead of guessing.
    bool ResolveUserParamType(const FString& InType, FNiagaraTypeDefinition& OutDef, FString& OutCanonical)
    {
        const FString T = InType.TrimStartAndEnd();
        if (T.Equals(TEXT("Float"), ESearchCase::IgnoreCase))                                       { OutDef = FNiagaraTypeDefinition::GetFloatDef(); OutCanonical = TEXT("Float");  return true; }
        if (T.Equals(TEXT("Vector"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Vec3"), ESearchCase::IgnoreCase)) { OutDef = FNiagaraTypeDefinition::GetVec3Def();  OutCanonical = TEXT("Vector"); return true; }
        if (T.Equals(TEXT("Vec2"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Vector2"), ESearchCase::IgnoreCase)) { OutDef = FNiagaraTypeDefinition::GetVec2Def();  OutCanonical = TEXT("Vec2");   return true; }
        if (T.Equals(TEXT("Vec4"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Vector4"), ESearchCase::IgnoreCase)) { OutDef = FNiagaraTypeDefinition::GetVec4Def();  OutCanonical = TEXT("Vec4");   return true; }
        if (T.Equals(TEXT("Color"), ESearchCase::IgnoreCase) || T.Equals(TEXT("LinearColor"), ESearchCase::IgnoreCase)) { OutDef = FNiagaraTypeDefinition::GetColorDef(); OutCanonical = TEXT("Color"); return true; }
        if (T.Equals(TEXT("Bool"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase)) { OutDef = FNiagaraTypeDefinition::GetBoolDef();  OutCanonical = TEXT("Bool");   return true; }
        if (T.Equals(TEXT("Int"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Int32"), ESearchCase::IgnoreCase) || T.Equals(TEXT("Integer"), ESearchCase::IgnoreCase)) { OutDef = FNiagaraTypeDefinition::GetIntDef(); OutCanonical = TEXT("Int"); return true; }
        return false;
    }

    // Read a JSON [x,y,z,...] / {X,Y,Z,...} array of N floats. Returns the count read.
    int32 ReadFloatComponents(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, float* Out, int32 Max)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(Field, Arr) || !Arr)
        {
            return 0;
        }
        int32 N = 0;
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            if (N >= Max) { break; }
            double D = 0.0;
            if (V.IsValid()) { V->TryGetNumber(D); }
            Out[N++] = static_cast<float>(D);
        }
        return N;
    }

    // Serialise a user param's current value (resolved by its canonical type) into the
    // result object under "value_read_back". Returns true if the offset resolved.
    bool ReadUserParamValueBack(
        FNiagaraUserRedirectionParameterStore& Store, const FString& Canonical,
        const FNiagaraTypeDefinition& Def, const FName& ShortName, const TSharedPtr<FJsonObject>& Out)
    {
        const FNiagaraVariable ReadVar(Def, ShortName);
        const int32 Off = Store.IndexOf(ReadVar);
        if (Off == INDEX_NONE)
        {
            return false;
        }
        if (Canonical == TEXT("Float"))
        {
            Out->SetNumberField(TEXT("value_read_back"), Store.GetParameterValueFromOffset<float>(Off));
        }
        else if (Canonical == TEXT("Int"))
        {
            Out->SetNumberField(TEXT("value_read_back"), Store.GetParameterValueFromOffset<int32>(Off));
        }
        else if (Canonical == TEXT("Bool"))
        {
            const FNiagaraBool B = Store.GetParameterValueFromOffset<FNiagaraBool>(Off);
            Out->SetBoolField(TEXT("value_read_back"), B.GetValue());
        }
        else if (Canonical == TEXT("Vector"))
        {
            Out->SetArrayField(TEXT("value_read_back"), mcp_common::VectorToJsonArray(Store.GetParameterValueFromOffset<FVector3f>(Off)));
        }
        else if (Canonical == TEXT("Vec2"))
        {
            Out->SetArrayField(TEXT("value_read_back"), mcp_common::Vector2DToJsonArray(Store.GetParameterValueFromOffset<FVector2f>(Off)));
        }
        else if (Canonical == TEXT("Vec4"))
        {
            const FVector4f V = Store.GetParameterValueFromOffset<FVector4f>(Off);
            TArray<TSharedPtr<FJsonValue>> A;
            A.Add(MakeShared<FJsonValueNumber>(V.X)); A.Add(MakeShared<FJsonValueNumber>(V.Y));
            A.Add(MakeShared<FJsonValueNumber>(V.Z)); A.Add(MakeShared<FJsonValueNumber>(V.W));
            Out->SetArrayField(TEXT("value_read_back"), A);
        }
        else if (Canonical == TEXT("Color"))
        {
            Out->SetArrayField(TEXT("value_read_back"), mcp_common::ColorToJsonArray(Store.GetParameterValueFromOffset<FLinearColor>(Off)));
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// set_niagara_user_parameter
// Sets a user-exposed parameter default on the system's exposed parameter store
// (FNiagaraUserRedirectionParameterStore). Adds the parameter (User. namespaced)
// if it does not already exist, then sets the value and reads it back to verify.
// It covers the full POD set: float/vector/color/bool/int/vec2/vec4,
// dispatched on an explicit "type" hint (back-compat:
// "vector" key => Vector, otherwise => Float).
// ---------------------------------------------------------------------------
// MCP:set_niagara_user_parameter - Sets a user-exposed parameter default on a Niagara System's exposed store.  Adds the parameter (under the User. namespace) if it does not yet exist, sets the value, then reads it back to verify. Supports float/vector(vec3)/vec2/vec4/color/bool/int. Type is taken from the optional ``type`` hint; if omitted, ``vector`` key implies Vector and otherwise Float (back-compat). Color is FLinearColor and bool is FNiagaraBool.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - User parameter name (without the "User." prefix).
//   @param type - Optional type token: Float|Vector|Vec2|Vec4|Color|Bool|Int (case-insensitive). Defaults to Vector if a ``vector`` key is present, else Float
//   @param value - Optional float scalar (Float), int (Int) or bool (Bool) value, per the resolved type
//   @param vector - Optional [X, Y, Z] for Vector (also implies type=Vector when type omitted)
//   @param vec2 - Optional [X, Y] for a Vec2 parameter
//   @param vec4 - Optional [X, Y, Z, W] for a Vec4 parameter
//   @param color - Optional [R, G, B, A] (linear) for a Color parameter
//   @returns { system_path: string, parameter_name: string, parameter_type: string, value_set: bool, value_read_back?: number|bool|[number...], verified: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString ParameterName;
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadParameterSystem(Params, ParameterName, System))
    {
        return Error;
    }

    // Resolve the parameter type. Explicit "type" wins; otherwise infer from the
    // value-carrying key, so a {value}|{vector} payload without a type works.
    FString TypeHint;
    if (!Params->TryGetStringField(TEXT("type"), TypeHint) || TypeHint.IsEmpty())
    {
        if      (Params->HasField(TEXT("vector"))) { TypeHint = TEXT("Vector"); }
        else if (Params->HasField(TEXT("vec2")))   { TypeHint = TEXT("Vec2"); }
        else if (Params->HasField(TEXT("vec4")))   { TypeHint = TEXT("Vec4"); }
        else if (Params->HasField(TEXT("color")))  { TypeHint = TEXT("Color"); }
        else                                        { TypeHint = TEXT("Float"); }
    }

    FNiagaraTypeDefinition TypeDef;
    FString Canonical;
    if (!ResolveUserParamType(TypeHint, TypeDef, Canonical))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown user-parameter type '%s' (expected Float/Vector/Vec2/Vec4/Color/Bool/Int)"), *TypeHint));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraUserParameter", "Set Niagara User Parameter"));
    System->Modify();

    FNiagaraUserRedirectionParameterStore& ExposedParams = System->GetExposedParameters();
    const FNiagaraVariable Var(TypeDef, FName(*ParameterName));
    // AddParameter is idempotent w.r.t. the redirect table; it namespaces to User.
    ExposedParams.AddParameter(Var, /*bInitialize*/ true, /*bTriggerRebind*/ true);

    // SetParameterValue<T> asserts the variable's byte size matches sizeof(T); the
    // Canonical->T mapping below mirrors ResolveUserParamType exactly so that holds.
    bool bSet = false;
    if (Canonical == TEXT("Float"))
    {
        float Value = 0.0f;
        Params->TryGetNumberField(TEXT("value"), Value);
        bSet = ExposedParams.SetParameterValue(Value, Var, /*bAdd*/ true);
    }
    else if (Canonical == TEXT("Int"))
    {
        int32 Value = 0;
        Params->TryGetNumberField(TEXT("value"), Value);
        bSet = ExposedParams.SetParameterValue(Value, Var, /*bAdd*/ true);
    }
    else if (Canonical == TEXT("Bool"))
    {
        bool bValue = false;
        Params->TryGetBoolField(TEXT("value"), bValue);
        FNiagaraBool NiBool;
        NiBool.SetValue(bValue);
        bSet = ExposedParams.SetParameterValue(NiBool, Var, /*bAdd*/ true);
    }
    else if (Canonical == TEXT("Vector"))
    {
        const FVector Vec = mcp_common::ReadJsonVector(Params, TEXT("vector"));
        bSet = ExposedParams.SetParameterValue(FVector3f((float)Vec.X, (float)Vec.Y, (float)Vec.Z), Var, /*bAdd*/ true);
    }
    else if (Canonical == TEXT("Vec2"))
    {
        float C[2] = {0.f, 0.f};
        ReadFloatComponents(Params, TEXT("vec2"), C, 2);
        bSet = ExposedParams.SetParameterValue(FVector2f(C[0], C[1]), Var, /*bAdd*/ true);
    }
    else if (Canonical == TEXT("Vec4"))
    {
        float C[4] = {0.f, 0.f, 0.f, 0.f};
        ReadFloatComponents(Params, TEXT("vec4"), C, 4);
        bSet = ExposedParams.SetParameterValue(FVector4f(C[0], C[1], C[2], C[3]), Var, /*bAdd*/ true);
    }
    else // Color
    {
        float C[4] = {0.f, 0.f, 0.f, 1.f};
        ReadFloatComponents(Params, TEXT("color"), C, 4);
        bSet = ExposedParams.SetParameterValue(FLinearColor(C[0], C[1], C[2], C[3]), Var, /*bAdd*/ true);
    }

    System->MarkPackageDirty();

    // Read the value back to verify (the user store redirects the short name).
    TSharedPtr<FJsonObject> Data = Niagara_StartParameterReply(System, ParameterName);
    Data->SetStringField(TEXT("parameter_type"), Canonical);
    Data->SetBoolField(TEXT("value_set"), bSet);
    const bool bVerified = ReadUserParamValueBack(ExposedParams, Canonical, TypeDef, FName(*ParameterName), Data);
    Data->SetBoolField(TEXT("verified"), bVerified);

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_user_parameter
// Reads a named user-exposed parameter's current value back off the system's
// exposed store. The caller supplies the parameter's type (the store keys by
// short name + type) so the read resolves to the right POD; this completes the
// round-trip companion to set_niagara_user_parameter.
// ---------------------------------------------------------------------------
// MCP:get_niagara_user_parameter - Read a user-exposed parameter's current value from a Niagara System's exposed store.  Resolves the parameter by short name (no "User." prefix) and the supplied ``type`` token, reads the POD value back. Round-trip companion to set_niagara_user_parameter.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - User parameter name (without the "User." prefix).
//   @param type - Type token: Float|Vector|Vec2|Vec4|Color|Bool|Int (case-insensitive); defaults to Float
//   @returns { system_path: string, parameter_name: string, parameter_type: string, found: bool, value_read_back?: number|bool|[number...] }

TSharedPtr<FJsonObject> HandleGetNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString ParameterName;
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadParameterSystem(Params, ParameterName, System))
    {
        return Error;
    }

    FString TypeHint = TEXT("Float");
    Params->TryGetStringField(TEXT("type"), TypeHint);
    FNiagaraTypeDefinition TypeDef;
    FString Canonical;
    if (!ResolveUserParamType(TypeHint, TypeDef, Canonical))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown user-parameter type '%s' (expected Float/Vector/Vec2/Vec4/Color/Bool/Int)"), *TypeHint));
    }

    FNiagaraUserRedirectionParameterStore& ExposedParams = System->GetExposedParameters();

    TSharedPtr<FJsonObject> Data = Niagara_StartParameterReply(System, ParameterName);
    Data->SetStringField(TEXT("parameter_type"), Canonical);
    const bool bFound = ReadUserParamValueBack(ExposedParams, Canonical, TypeDef, FName(*ParameterName), Data);
    Data->SetBoolField(TEXT("found"), bFound);
    if (!bFound)
    {
        Data->SetStringField(TEXT("note"),
            FString::Printf(TEXT("No '%s' user parameter of type %s on this system"), *ParameterName, *Canonical));
    }

    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Spawned-component override parameters
//
// These commands operate on a live UNiagaraComponent in the editor world (its
// OverrideParameters store: FNiagaraUserRedirectionParameterStore), not on the
// system asset's exposed defaults. The setters used are the non-deprecated FName
// variants verified against NiagaraComponent.h (all NIAGARA_API):
//   SetVariableFloat(FName, float)            GetVariableFloat(FName, bool&)
//   SetVariableVec3(FName, FVector)           GetVariableVec3(FName, bool&)
//   SetVariableLinearColor(FName, FLinearColor&)  GetVariableColor(FName, bool&)
//   SetVariableBool(FName, bool)              GetVariableBool(FName, bool&)
//   SetSystemFixedBounds(FBox)                GetSystemFixedBounds()
// The setters add the override locally if necessary, so they do not require the
// variable to pre-exist on the (possibly empty) system. Read-back getters return
// an out bIsValid flag, which we report as "verified".
// ===========================================================================

namespace
{
    // The UNiagaraComponent on the actor named ActorName; null with OutError set when there is none.
    UNiagaraComponent* Niagara_FindComponent(const FString& ActorName, AActor*& OutOwner, FString& OutError)
    {
        UNiagaraComponent* Component = FindNiagaraComponentByOwner(ActorName, OutOwner);
        if (!Component)
        {
            OutError = FString::Printf(TEXT("No UNiagaraComponent found on actor: %s"), *ActorName);
        }
        return Component;
    }

    // Reads the required `actor_name`, then `variable_name`; false with OutError set when one is missing.
    bool Niagara_ReadComponentVariableNames(const TSharedPtr<FJsonObject>& Params,
        FString& OutActorName, FString& OutVariableName, FString& OutError)
    {
        if (!Params->TryGetStringField(TEXT("actor_name"), OutActorName) || OutActorName.IsEmpty())
        {
            OutError = TEXT("Missing required 'actor_name' parameter");
            return false;
        }
        if (!Params->TryGetStringField(TEXT("variable_name"), OutVariableName) || OutVariableName.IsEmpty())
        {
            OutError = TEXT("Missing required 'variable_name' parameter");
            return false;
        }
        return true;
    }

    // An override variable of a spawned component: the `actor_name` and `variable_name` params and what they name.
    struct FNiagaraComponentTarget
    {
        FString ActorName;
        FString VariableName;
        AActor* Owner = nullptr;
        UNiagaraComponent* Component = nullptr;
    };

    // Niagara_ReadComponentVariableNames, then the component on that actor; null on success, else the error reply.
    TSharedPtr<FJsonObject> Niagara_ResolveComponentVariable(const TSharedPtr<FJsonObject>& Params, FNiagaraComponentTarget& Out)
    {
        FString Error;
        if (!Niagara_ReadComponentVariableNames(Params, Out.ActorName, Out.VariableName, Error))
        {
            return mcp_common::MakeErrorReply(Error);
        }
        Out.Component = Niagara_FindComponent(Out.ActorName, Out.Owner, Error);
        if (!Out.Component)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    // A reply object that opens with the target's "actor_name" and "variable_name", then "variable_type".
    TSharedPtr<FJsonObject> Niagara_StartComponentReply(const FNiagaraComponentTarget& Target, const TCHAR* VariableType)
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("actor_name"), Target.Owner ? Target.Owner->GetName() : Target.ActorName);
        Data->SetStringField(TEXT("variable_name"), Target.VariableName);
        Data->SetStringField(TEXT("variable_type"), VariableType);
        return Data;
    }

    // The note of a component setter whose value UNiagaraComponent has no getter to read back.
    FString Niagara_NoReadbackNote(const TCHAR* VariableType)
    {
        return FString::Printf(
            TEXT("UE 5.8 UNiagaraComponent does not expose GetNiagaraVariable%s as public API -- set succeeded but round-trip readback is not available"),
            VariableType);
    }
}

// ---------------------------------------------------------------------------
// spawn_niagara_component
// Spawns a UNiagaraComponent in the editor world and assigns the owner actor a
// deterministic label so subsequent set_niagara_component_* calls can target it.
// owner_actor_name is the owner's INTERNAL object name (AActor::GetName) so the
// caller can delete it via delete_actor (which resolves by GetName); the label
// is returned separately. Either identifier resolves the component later.
// ---------------------------------------------------------------------------
// MCP:spawn_niagara_component - Spawns a Niagara System as a persistent UNiagaraComponent in the editor world.  Spawns via UNiagaraFunctionLibrary::SpawnSystemAtLocation with bAutoDestroy disabled so the component persists for follow-up parameter calls. The owner actor can later be targeted by either its internal object name (``owner_actor_name``) or its editor label (``owner_actor_label``).
//   @param system_path - Asset path of the Niagara System to spawn (e.g. /Game/MyGame/NS_Example)
//   @param location [string!] - World location [X, Y, Z].
//   @param rotation - Optional [pitch, yaw, roll]; defaults to identity
//   @param actor_label - Optional desired editor label for the owner actor
//   @returns { system_path: string, component_name: string, owner_actor_name: string, owner_actor_label: string, is_registered: bool, is_active: bool, location: [number, number, number] }

TSharedPtr<FJsonObject> HandleSpawnNiagaraComponent(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }

    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    // bAutoDestroy=false so the component/owner persist for follow-up parameter calls.
    UNiagaraComponent* Component = UNiagaraFunctionLibrary::SpawnSystemAtLocation(
        World, System, Location, Rotation,
        FVector(1.0f), /*bAutoDestroy*/ false, /*bAutoActivate*/ true,
        ENCPoolMethod::None, /*bPreCullCheck*/ true);

    if (!Component)
    {
        return mcp_common::MakeErrorReply(
            TEXT("SpawnSystemAtLocation returned null (system may have been culled or invalid in the editor world)"));
    }

    AActor* Owner = Component->GetOwner();

    // Assign a deterministic label so callers may target by label too. Optional.
    FString DesiredLabel;
    if (Params->TryGetStringField(TEXT("actor_label"), DesiredLabel) && !DesiredLabel.IsEmpty() && Owner)
    {
        Owner->SetActorLabel(DesiredLabel);
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("component_name"), Component->GetName());
    // Internal object name first (delete_actor resolves by GetName).
    Data->SetStringField(TEXT("owner_actor_name"), Owner ? Owner->GetName() : FString(TEXT("None")));
    Data->SetStringField(TEXT("owner_actor_label"), Owner ? Owner->GetActorLabel() : FString(TEXT("None")));
    Data->SetBoolField(TEXT("is_registered"), Component->IsRegistered());
    Data->SetBoolField(TEXT("is_active"), Component->IsActive());
    Data->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(Location));

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_component_float
// ---------------------------------------------------------------------------
// MCP:set_niagara_component_float - Sets a float override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor (matched by internal name or editor label) and calls SetVariableFloat, then reads it back.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param value - Float value to set.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: number }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentFloat(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    double Value = 0.0;
    Params->TryGetNumberField(TEXT("value"), Value);

    const FName VarName(*Target.VariableName);
    Target.Component->SetVariableFloat(VarName, (float)Value);

    bool bIsValid = false;
    const float ReadBack = Target.Component->GetVariableFloat(VarName, bIsValid);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Float"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), bIsValid);
    Data->SetNumberField(TEXT("value_read_back"), ReadBack);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_component_vector
// ---------------------------------------------------------------------------
// MCP:set_niagara_component_vector - Sets a Vector3 override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableVec3, then reads it back.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param vector [string!] - Vector value [X, Y, Z].
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentVector(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }

    const FVector Vec = mcp_common::ReadJsonVector(Params, TEXT("vector"));
    const FName VarName(*Target.VariableName);
    Target.Component->SetVariableVec3(VarName, Vec);

    bool bIsValid = false;
    const FVector ReadBack = Target.Component->GetVariableVec3(VarName, bIsValid);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Vector"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), bIsValid);
    Data->SetArrayField(TEXT("value_read_back"), mcp_common::VectorToJsonArray(ReadBack));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_component_color
// ---------------------------------------------------------------------------
// MCP:set_niagara_component_color - Sets a LinearColor override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableLinearColor, then reads it back.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param color - Color value [R, G, B, A] (A defaults to 1.0 if omitted).
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentColor(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }

    // Read RGBA from the "color" array (RGBA, defaulting A=1).
    const TArray<TSharedPtr<FJsonValue>>* ColorArr = nullptr;
    Params->TryGetArrayField(TEXT("color"), ColorArr);
    const FLinearColor Color = mcp_common::LinearColorFromJsonArray(ColorArr, FLinearColor(0.f, 0.f, 0.f, 1.f));

    const FName VarName(*Target.VariableName);
    Target.Component->SetVariableLinearColor(VarName, Color);

    bool bIsValid = false;
    const FLinearColor ReadBack = Target.Component->GetVariableColor(VarName, bIsValid);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("LinearColor"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), bIsValid);
    Data->SetArrayField(TEXT("value_read_back"), mcp_common::ColorToJsonArray(ReadBack));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_component_bool
// ---------------------------------------------------------------------------
// MCP:set_niagara_component_bool - Sets a bool override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableBool, then reads it back.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param value - Bool value to set.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentBool(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    bool bValue = false;
    Params->TryGetBoolField(TEXT("value"), bValue);

    const FName VarName(*Target.VariableName);
    Target.Component->SetVariableBool(VarName, bValue);

    bool bIsValid = false;
    const bool ReadBack = Target.Component->GetVariableBool(VarName, bIsValid);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Bool"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), bIsValid);
    Data->SetBoolField(TEXT("value_read_back"), ReadBack);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_system_fixed_bounds
// Sets the per-component fixed bounds via UNiagaraComponent::SetSystemFixedBounds
// (FBox in local space). Reads back via GetSystemFixedBounds to verify.
// ---------------------------------------------------------------------------
// MCP:set_niagara_system_fixed_bounds - Sets fixed local-space bounds on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls UNiagaraComponent::SetSystemFixedBounds(FBox), then reads back via GetSystemFixedBounds.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param min [string!] - Local-space bounds minimum [X, Y, Z].
//   @param max [string!] - Local-space bounds maximum [X, Y, Z].
//   @returns { actor_name: string, applied: bool, bounds_valid: bool, min_read_back: [number, number, number], max_read_back: [number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemFixedBounds(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    AActor* Owner = nullptr;
    FString Error;
    UNiagaraComponent* Component = Niagara_FindComponent(ActorName, Owner, Error);
    if (!Component)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const FVector Min = mcp_common::ReadJsonVector(Params, TEXT("min"));
    const FVector Max = mcp_common::ReadJsonVector(Params, TEXT("max"));
    const FBox LocalBounds(Min, Max);

    Component->SetSystemFixedBounds(LocalBounds);

    const FBox ReadBack = Component->GetSystemFixedBounds();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), Owner ? Owner->GetName() : ActorName);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("bounds_valid"), ReadBack.IsValid != 0);
    Data->SetArrayField(TEXT("min_read_back"), mcp_common::VectorToJsonArray(ReadBack.Min));
    Data->SetArrayField(TEXT("max_read_back"), mcp_common::VectorToJsonArray(ReadBack.Max));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Spawned-component complex-type override parameters.
// ---------------------------------------------------------------------------

// MCP:set_niagara_component_vec2 - Sets a Vec2 (FVector2D) override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableVec2, then reads it back via GetNiagaraVariableVec2.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param x - X component of the Vec2.
//   @param y - Y component of the Vec2.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentVec2(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    double X = 0.0;
    double Y = 0.0;
    Params->TryGetNumberField(TEXT("x"), X);
    Params->TryGetNumberField(TEXT("y"), Y);

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableVec2(FName(*VarName), FVector2D((float)X, (float)Y));

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Vec2"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Vec2")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_vec4 - Sets a Vec4 (FVector4) override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableVec4, then reads it back via GetNiagaraVariableVec4.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param x - X component of the Vec4.
//   @param y - Y component of the Vec4.
//   @param z - Z component of the Vec4.
//   @param w - W component of the Vec4.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentVec4(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    double W = 0.0;
    Params->TryGetNumberField(TEXT("x"), X);
    Params->TryGetNumberField(TEXT("y"), Y);
    Params->TryGetNumberField(TEXT("z"), Z);
    Params->TryGetNumberField(TEXT("w"), W);

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableVec4(FName(*VarName), FVector4((float)X, (float)Y, (float)Z, (float)W));

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Vec4"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Vec4")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_int - Sets an int32 override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableInt, then reads it back via GetNiagaraVariableInt.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param value - Integer value to set.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: integer }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentInt(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    int32 Value = 0;
    Params->TryGetNumberField(TEXT("value"), Value);

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableInt(FName(*VarName), (int32)Value);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Int"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Int")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_position - Sets a Position (FVector) override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariablePosition, then reads it back via GetNiagaraVariablePosition.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param position - World-space position override as [x, y, z].
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentPosition(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }

    const FVector Vec = mcp_common::ReadJsonVector(Params, TEXT("position"));
    const FString VarName(*Target.VariableName);
    Target.Component->SetVariablePosition(FName(*VarName), Vec);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Position"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Position")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_quat - Sets a Quat (FQuat) override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableQuat, then reads it back via GetNiagaraVariableQuat and serialises as 4 floats in (W, X, Y, Z) order.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param w - W (scalar) component of the quaternion.
//   @param x - X component of the quaternion.
//   @param y - Y component of the quaternion.
//   @param z - Z component of the quaternion.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentQuat(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    double W = 1.0;
    double X = 0.0;
    double Y = 0.0;
    double Z = 0.0;
    Params->TryGetNumberField(TEXT("w"), W);
    Params->TryGetNumberField(TEXT("x"), X);
    Params->TryGetNumberField(TEXT("y"), Y);
    Params->TryGetNumberField(TEXT("z"), Z);

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableQuat(FName(*VarName), FQuat((float)X, (float)Y, (float)Z, (float)W));

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Quat"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Quat")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_object - Sets an Object (UObject*) override parameter on a spawned Niagara component by loading the UObject at the given object_path.  Resolves the UNiagaraComponent on the owner actor and calls SetVariableObject, then reads it back via GetNiagaraVariableObject and returns its GetPathName() as a string.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param object_path - Asset path of the UObject to bind (e.g. "/Game/Materials/M_Heat.M_Heat").
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, object_path_read_back: string, object_class_read_back: string }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentObject(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    FString Error;
    if (!Niagara_ReadComponentVariableNames(Params, Target.ActorName, Target.VariableName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ObjectPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath)) { return Missing; }

    Target.Component = Niagara_FindComponent(Target.ActorName, Target.Owner, Error);
    if (!Target.Component)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UObject* Resolved = LoadObject<UObject>(nullptr, *ObjectPath);
    if (!Resolved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not load UObject: %s"), *ObjectPath));
    }

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableObject(FName(*VarName), Resolved);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Object"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("object_path_set"), ObjectPath);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Object")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_matrix - Sets a Matrix (FMatrix) override parameter on a spawned Niagara component. Resolves the UNiagaraComponent on the owner actor and calls SetVariableMatrix with a 4x4 row-major matrix built from 16 floats. UNiagaraComponent does NOT expose GetNiagaraVariableMatrix as public API, so round-trip readback is not available; verified:true means the set call succeeded.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param matrix - Array of 16 numbers in 4x4 row-major order (M00, M01, ..., M33).
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number x 16] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentMatrix(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }

    // Read 16 floats from "matrix" (row-major). Defaults to identity if absent.
    float M[16] = {
        1.f, 0.f, 0.f, 0.f,
        0.f, 1.f, 0.f, 0.f,
        0.f, 0.f, 1.f, 0.f,
        0.f, 0.f, 0.f, 1.f,
    };
    const TArray<TSharedPtr<FJsonValue>>* MatrixArr = nullptr;
    if (Params->TryGetArrayField(TEXT("matrix"), MatrixArr) && MatrixArr)
    {
        const int32 N = FMath::Min<int32>(MatrixArr->Num(), 16);
        for (int32 i = 0; i < N; ++i)
        {
            M[i] = (float)(*MatrixArr)[i]->AsNumber();
        }
    }

    // FMatrix ctor signature: FMatrix(FVector InX, FVector InY, FVector InZ, FVector InW)
    // where InX = row 0 (M00..M02 + pad), InY = row 1, etc. UE matrix is row-major in storage.
    const FMatrix Mat(
        FVector(M[0],  M[1],  M[2]),
        FVector(M[4],  M[5],  M[6]),
        FVector(M[8],  M[9],  M[10]),
        FVector(M[12], M[13], M[14]));

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableMatrix(FName(*VarName), Mat);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Matrix"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Matrix")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_niagara_component_matrix - Reads a Matrix (FMatrix4f) override parameter back off a spawned Niagara component — the round-trip reader for set_niagara_component_matrix. UNiagaraComponent exposes no GetNiagaraVariableMatrix, so this reads the value straight out of the component's OverrideParameters store (FNiagaraParameterStore) by looking up the Matrix4 FNiagaraVariable; the override is stored as FMatrix44f. Returns found:false with an empty matrix (never a faked identity) when the variable has no local override on the component.
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name to read.
//   @returns { actor_name: string, variable_name: string, variable_type: string, found: bool, matrix: [number x 16] }
TSharedPtr<FJsonObject> HandleGetNiagaraComponentMatrix(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Matrix"));

    // The override store holds the matrix as FMatrix44f (NOT FMatrix). Look it up by
    // the Matrix4 type def; if the component has no local override for this variable,
    // report found:false honestly rather than fabricating an identity matrix.
    const FNiagaraParameterStore& Store = Target.Component->GetOverrideParameters();
    const FNiagaraVariable Var(FNiagaraTypeDefinition::GetMatrix4Def(), FName(*Target.VariableName));
    const int32 Off = Store.IndexOf(Var);
    if (Off == INDEX_NONE)
    {
        Data->SetBoolField(TEXT("found"), false);
        Data->SetArrayField(TEXT("matrix"), TArray<TSharedPtr<FJsonValue>>());
        return mcp_common::MakeOkReply(Data);
    }

    FMatrix44f Out;
    Store.GetParameterValue(Out, Var);
    // Convert to double-precision FMatrix, then emit 16 doubles row-major — mirroring the
    // exact layout the setter writes (row 0 = M00..M02 padded, ..., row 3 = M30..M32,M33).
    const FMatrix Mat((FMatrix)Out);
    TArray<TSharedPtr<FJsonValue>> MatrixArr;
    MatrixArr.Reserve(16);
    for (int32 Row = 0; Row < 4; ++Row)
    {
        for (int32 Col = 0; Col < 4; ++Col)
        {
            MatrixArr.Add(MakeShared<FJsonValueNumber>(Mat.M[Row][Col]));
        }
    }
    Data->SetBoolField(TEXT("found"), true);
    Data->SetArrayField(TEXT("matrix"), MatrixArr);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Emitter handle operations
//
// These commands manipulate the emitter handles on a UNiagaraSystem asset.
// All mutation APIs are WITH_EDITORONLY_DATA on UNiagaraSystem (NIAGARA_API),
// verified against NiagaraSystem.h:
//   AddEmitterHandle(UNiagaraEmitter&, FName, FGuid)
//   RemoveEmitterHandle(const FNiagaraEmitterHandle&)
//   DuplicateEmitterHandle(const FNiagaraEmitterHandle&, FName)
//   GetEmitterHandles() -> TArray<FNiagaraEmitterHandle>&
// And on FNiagaraEmitterHandle (NiagaraEmitterHandle.h):
//   SetName(FName, UNiagaraSystem&)
//   SetIsEnabled(bool, UNiagaraSystem&, bool)
//   GetId(), GetName(), GetIsEnabled(), GetEmitterData()
// The emitter version GUID for AddEmitterHandle comes from
// UNiagaraEmitter::GetExposedVersion().VersionGuid.
// ===========================================================================

namespace
{
    // Find an emitter handle in a system by name. Returns pointer into the array
    // or nullptr. The pointer is valid as long as the array is not resized.
    FNiagaraEmitterHandle* FindEmitterHandleByName(UNiagaraSystem* System, const FString& InName)
    {
        if (!System || InName.IsEmpty())
        {
            return nullptr;
        }
        const FName TargetName(*InName);
        TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        for (FNiagaraEmitterHandle& H : Handles)
        {
            if (H.GetName() == TargetName)
            {
                return &H;
            }
        }
        return nullptr;
    }

    // The three wordings the handlers use when an emitter handle has no emitter data.
    const TCHAR* const NiagaraNoEmitterData = TEXT("Emitter data is null");
    const TCHAR* const NiagaraNoEmitterDataUninitialised = TEXT("Emitter data is null (handle may not be initialised)");
    const TCHAR* const NiagaraNoEmitterDataTemplate = TEXT("Emitter has no data (instanced template?)");

    // Reads the required `system_path`, then `emitter_name`; false with OutError set when one is missing.
    bool Niagara_ReadSystemAndEmitterNames(const TSharedPtr<FJsonObject>& Params,
        FString& OutSystemPath, FString& OutEmitterName, FString& OutError)
    {
        // Read here rather than through ExtractNiagaraSystemPath: the catalog generator follows calls two
        // deep, and this helper is already the second one under Niagara_LoadEmitterData.
        if (mcp_common::ReadRequiredString(Params, TEXT("system_path"), OutSystemPath).IsValid())
        {
            OutError = TEXT("Missing required 'system_path' parameter");
            return false;
        }
        if (!Params->TryGetStringField(TEXT("emitter_name"), OutEmitterName) || OutEmitterName.IsEmpty())
        {
            OutError = TEXT("Missing required 'emitter_name' parameter");
            return false;
        }
        return true;
    }

    // The system at SystemPath, then its handle named EmitterName; null with OutError set on the first miss.
    FNiagaraEmitterHandle* Niagara_ResolveEmitterHandle(const FString& SystemPath, const FString& EmitterName,
        UNiagaraSystem*& OutSystem, FString& OutError)
    {
        OutSystem = Niagara_ResolveSystem(SystemPath, OutError);
        if (!OutSystem)
        {
            return nullptr;
        }
        FNiagaraEmitterHandle* Handle = FindEmitterHandleByName(OutSystem, EmitterName);
        if (!Handle)
        {
            OutError = FString::Printf(TEXT("Emitter handle not found: %s"), *EmitterName);
        }
        return Handle;
    }

    // Niagara_ResolveEmitterHandle, then the handle's data (NoDataError when it has none); OutHandle gets the handle.
    FVersionedNiagaraEmitterData* Niagara_ResolveEmitterData(const FString& SystemPath, const FString& EmitterName,
        const TCHAR* NoDataError, UNiagaraSystem*& OutSystem, FString& OutError, FNiagaraEmitterHandle** OutHandle = nullptr)
    {
        FNiagaraEmitterHandle* Handle = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, OutSystem, OutError);
        if (!Handle)
        {
            return nullptr;
        }
        if (OutHandle)
        {
            *OutHandle = Handle;
        }
        FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
        if (!EmitterData)
        {
            OutError = NoDataError;
        }
        return EmitterData;
    }

    // Niagara_ReadSystemAndEmitterNames, then Niagara_ResolveEmitterData; null on success, else the error reply.
    TSharedPtr<FJsonObject> Niagara_LoadEmitterData(const TSharedPtr<FJsonObject>& Params, const TCHAR* NoDataError,
        FString& OutEmitterName, UNiagaraSystem*& OutSystem, FVersionedNiagaraEmitterData*& OutEmitterData,
        FNiagaraEmitterHandle** OutHandle = nullptr)
    {
        FString SystemPath;
        FString Error;
        if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, OutEmitterName, Error))
        {
            return mcp_common::MakeErrorReply(Error);
        }
        OutEmitterData = Niagara_ResolveEmitterData(SystemPath, OutEmitterName, NoDataError, OutSystem, Error, OutHandle);
        if (!OutEmitterData)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }

    // Niagara_ResolveEmitterHandle, then the handle's UNiagaraEmitter, then its emitter data.
    UNiagaraEmitter* Niagara_ResolveEditableEmitter(const FString& SystemPath, const FString& EmitterName,
        UNiagaraSystem*& OutSystem, FVersionedNiagaraEmitterData*& OutEmitterData, FString& OutError)
    {
        FNiagaraEmitterHandle* Handle = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, OutSystem, OutError);
        if (!Handle)
        {
            return nullptr;
        }
        UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Handle->GetEmitterBase());
        if (!Emitter)
        {
            OutError = TEXT("Emitter base is not a UNiagaraEmitter (may be a stateless emitter)");
            return nullptr;
        }
        OutEmitterData = Handle->GetEmitterData();
        if (!OutEmitterData)
        {
            OutError = NiagaraNoEmitterDataUninitialised;
            return nullptr;
        }
        return Emitter;
    }

    // The handle named by the optional `emitter_name` param, else the one at the optional `emitter_index`.
    FNiagaraEmitterHandle* Niagara_FindHandleByNameOrIndex(const TSharedPtr<FJsonObject>& Params,
        UNiagaraSystem* System, FString& OutError)
    {
        TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        FString HandleName;
        int32 HandleIndex = -1;
        const bool bHasName = Params->TryGetStringField(TEXT("emitter_name"), HandleName);
        const bool bHasIndex = Params->TryGetNumberField(TEXT("emitter_index"), HandleIndex);

        FNiagaraEmitterHandle* Target = nullptr;
        if (bHasName && !HandleName.IsEmpty())
        {
            Target = FindEmitterHandleByName(System, HandleName);
        }
        else if (bHasIndex && HandleIndex >= 0 && HandleIndex < Handles.Num())
        {
            Target = &Handles[HandleIndex];
        }
        if (!Target)
        {
            OutError = TEXT("Emitter handle not found (provide 'emitter_name' or valid 'emitter_index')");
        }
        return Target;
    }

    // Adds "save_error" when SaveError is not empty.
    void Niagara_AddSaveError(const TSharedPtr<FJsonObject>& Data, const FString& SaveError)
    {
        if (!SaveError.IsEmpty())
        {
            Data->SetStringField(TEXT("save_error"), SaveError);
        }
    }

    // Save a modified Niagara system asset to disk (reuses the package save pattern).
    bool SaveModifiedNiagaraSystem(UNiagaraSystem* System, FString& OutError)
    {
        if (!System)
        {
            OutError = TEXT("Null system");
            return false;
        }
        return mcp_common::SaveAssetPackage(System->GetOutermost(), System, OutError);
    }

    // --- Event-handler helpers: event-handler execution mode <-> string ---------
    FString EventExecModeToString(EScriptExecutionMode Mode)
    {
        switch (Mode)
        {
        case EScriptExecutionMode::EveryParticle:    return TEXT("EveryParticle");
        case EScriptExecutionMode::SpawnedParticles: return TEXT("SpawnedParticles");
        case EScriptExecutionMode::SingleParticle:   return TEXT("SingleParticle");
        default:                                     return TEXT("Unknown");
        }
    }

    bool ParseEventExecMode(const FString& In, EScriptExecutionMode& Out)
    {
        const FString S = In.TrimStartAndEnd();
        if (S.Equals(TEXT("EveryParticle"), ESearchCase::IgnoreCase))    { Out = EScriptExecutionMode::EveryParticle;    return true; }
        if (S.Equals(TEXT("SpawnedParticles"), ESearchCase::IgnoreCase)) { Out = EScriptExecutionMode::SpawnedParticles; return true; }
        if (S.Equals(TEXT("SingleParticle"), ESearchCase::IgnoreCase))   { Out = EScriptExecutionMode::SingleParticle;   return true; }
        return false;
    }

    // Serialise one event handler. The handle's identity is its Script's UsageId
    // (what RemoveEventHandlerByUsageId matches on).
    TSharedPtr<FJsonObject> EventHandlerToJson(const FNiagaraEventScriptProperties& EH)
    {
        TSharedPtr<FJsonObject> J = MakeShared<FJsonObject>();
        J->SetStringField(TEXT("usage_id"),
            EH.Script ? EH.Script->GetUsageId().ToString(EGuidFormats::DigitsWithHyphens) : FString());
        J->SetStringField(TEXT("source_event_name"), EH.SourceEventName.ToString());
        J->SetStringField(TEXT("execution_mode"), EventExecModeToString(EH.ExecutionMode));
        J->SetNumberField(TEXT("spawn_number"), static_cast<double>(EH.SpawnNumber));
        J->SetNumberField(TEXT("max_events_per_frame"), static_cast<double>(EH.MaxEventsPerFrame));
        J->SetNumberField(TEXT("min_spawn_number"), static_cast<double>(EH.MinSpawnNumber));
        J->SetBoolField(TEXT("random_spawn_number"), EH.bRandomSpawnNumber);
        J->SetStringField(TEXT("source_emitter_id"),
            EH.SourceEmitterID.IsValid() ? EH.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphens) : FString());
        J->SetBoolField(TEXT("has_script"), EH.Script != nullptr);
        return J;
    }
}

// ---------------------------------------------------------------------------
// add_niagara_emitter_handle
// Adds an emitter to a Niagara system from an existing UNiagaraEmitter asset.
// Loads the emitter, gets its exposed version GUID, calls AddEmitterHandle.
// ---------------------------------------------------------------------------
// MCP:add_niagara_emitter_handle - Add an emitter to a Niagara system from an existing UNiagaraEmitter asset.  Loads the emitter asset, gets its exposed version GUID, and calls UNiagaraSystem::AddEmitterHandle. The system is saved after modification.
//   @param system_path - Asset path of the target Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_path - Asset path of the source UNiagaraEmitter to add.
//   @param emitter_name - Optional desired name for the new handle; defaults to the source emitter's name
//   @returns { system_path: string, emitter_handle_name: string, emitter_handle_id: string, is_valid: bool, is_enabled: bool, total_emitters: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddNiagaraEmitterHandle(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    FString EmitterPath;
    if (!mcp_common::ExtractStringPath(Params, TEXT("emitter_path"), EmitterPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'emitter_path' parameter"));
    }

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Load the source emitter asset.
    UNiagaraEmitter* Emitter = mcp_common::ResolveAsset<UNiagaraEmitter>(EmitterPath);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Niagara emitter not found: %s"), *EmitterPath));
    }

    // Get the emitter's current version GUID.
    const FNiagaraAssetVersion ExposedVer = Emitter->GetExposedVersion();
    const FGuid VersionGuid = ExposedVer.VersionGuid;

    // Optional desired name; if empty, AddEmitterHandle generates a unique one.
    FString DesiredName;
    Params->TryGetStringField(TEXT("emitter_name"), DesiredName);
    const FName EmitterName = DesiredName.IsEmpty() ? FName(*Emitter->GetName()) : FName(*DesiredName);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraEmitterHandle", "Add Niagara Emitter Handle"));
    System->Modify();

    const FNiagaraEmitterHandle NewHandle = System->AddEmitterHandle(*Emitter, EmitterName, VersionGuid);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("emitter_handle_name"), NewHandle.GetName().ToString());
    Data->SetStringField(TEXT("emitter_handle_id"), NewHandle.GetId().ToString());
    Data->SetBoolField(TEXT("is_valid"), NewHandle.IsValid());
    Data->SetBoolField(TEXT("is_enabled"), NewHandle.GetIsEnabled());
    Data->SetNumberField(TEXT("total_emitters"), System->GetEmitterHandles().Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_niagara_emitter_handle"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_niagara_emitter_handle
// Removes an emitter handle by name or index from a Niagara system.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_emitter_handle - Remove an emitter handle from a Niagara system by name or index.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Optional display name of the emitter handle to remove
//   @param emitter_index - Optional zero-based index; used if emitter_name not provided
//   @returns { system_path: string, removed_emitter_name: string, removed_emitter_id: string, emitters_before: int, emitters_after: int, removed: bool }

TSharedPtr<FJsonObject> HandleRemoveNiagaraEmitterHandle(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    const int32 BeforeCount = System->GetEmitterHandles().Num();

    FString Error;
    FNiagaraEmitterHandle* Target = Niagara_FindHandleByNameOrIndex(Params, System, Error);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const FString RemovedName = Target->GetName().ToString();
    const FString RemovedId = Target->GetId().ToString();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraEmitterHandle", "Remove Niagara Emitter Handle"));
    System->Modify();

    System->RemoveEmitterHandle(*Target);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("removed_emitter_name"), RemovedName);
    Data->SetStringField(TEXT("removed_emitter_id"), RemovedId);
    Data->SetNumberField(TEXT("emitters_before"), BeforeCount);
    Data->SetNumberField(TEXT("emitters_after"), System->GetEmitterHandles().Num());
    Data->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// duplicate_niagara_emitter_handle
// Duplicates an existing emitter handle in a Niagara system.
// ---------------------------------------------------------------------------
// MCP:duplicate_niagara_emitter_handle - Duplicate an emitter handle of a Niagara System asset under a new name, keeping its enabled state, then save the system. Returns the actual name and id of the copy plus total_emitters; a failed save is not reported in the reply. Find handle names with get_niagara_emitter_handles.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param source_emitter_name - Display name of the emitter handle to copy (case-insensitive), as get_niagara_emitter_handles lists it.
//   @param new_emitter_name - Name for the copy (default "<source_emitter_name>_Copy").
//   @returns { system_path: string, source_emitter_name: string, new_emitter_name: string, new_emitter_id: string, is_valid: bool, total_emitters: int }

TSharedPtr<FJsonObject> HandleDuplicateNiagaraEmitterHandle(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    FString SourceName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_emitter_name"), SourceName)) { return Missing; }

    FString Error;
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Source = Niagara_ResolveEmitterHandle(SystemPath, SourceName, System, Error);
    if (!Source)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FString NewName;
    Params->TryGetStringField(TEXT("new_emitter_name"), NewName);
    const FName DupName = NewName.IsEmpty()
        ? FName(*FString::Printf(TEXT("%s_Copy"), *SourceName))
        : FName(*NewName);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "DuplicateNiagaraEmitterHandle", "Duplicate Niagara Emitter Handle"));
    System->Modify();

    const FNiagaraEmitterHandle NewHandle = System->DuplicateEmitterHandle(*Source, DupName);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("source_emitter_name"), SourceName);
    Data->SetStringField(TEXT("new_emitter_name"), NewHandle.GetName().ToString());
    Data->SetStringField(TEXT("new_emitter_id"), NewHandle.GetId().ToString());
    Data->SetBoolField(TEXT("is_valid"), NewHandle.IsValid());
    Data->SetNumberField(TEXT("total_emitters"), System->GetEmitterHandles().Num());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_handles
// Lists all emitter handles on a Niagara system: name, ID, enabled state.
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_handles - List every emitter handle of a Niagara System asset with its display name, id GUID, enabled and valid flags, plus emitter_count. Read-only; the names feed the other emitter tools, and get_niagara_emitter_handle_info gives one handle in detail.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @returns { system_path: string, emitter_count: int, emitter_handles: [{ name, id, is_enabled, is_valid }] }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterHandles(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

    TArray<TSharedPtr<FJsonValue>> HandleArray;
    for (const FNiagaraEmitterHandle& H : Handles)
    {
        TSharedPtr<FJsonObject> HObj = MakeShared<FJsonObject>();
        HObj->SetStringField(TEXT("name"), H.GetName().ToString());
        HObj->SetStringField(TEXT("id"), H.GetId().ToString());
        HObj->SetBoolField(TEXT("is_enabled"), H.GetIsEnabled());
        HObj->SetBoolField(TEXT("is_valid"), H.IsValid());
        HandleArray.Add(MakeShared<FJsonValueObject>(HObj));
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("emitter_count"), Handles.Num());
    Data->SetArrayField(TEXT("emitter_handles"), HandleArray);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_emitter_enabled
// Enable or disable an emitter handle by name.
// ---------------------------------------------------------------------------
// MCP:set_niagara_emitter_enabled - Enable or disable an emitter handle on a Niagara system.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param enabled - Optional bool; defaults to true
//   @returns { system_path: string, emitter_name: string, enabled: bool, state_changed: bool, verified: bool, is_enabled_read_back: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterEnabled(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Target = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, System, Error);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    bool bEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterEnabled", "Set Niagara Emitter Enabled"));
    System->Modify();

    const bool bChanged = Target->SetIsEnabled(bEnabled, *System, /*bRecompileIfChanged*/ true);

    // Read back to verify.
    const bool ReadBack = Target->GetIsEnabled();

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetBoolField(TEXT("enabled"), bEnabled);
    Data->SetBoolField(TEXT("state_changed"), bChanged);
    Data->SetBoolField(TEXT("verified"), ReadBack == bEnabled);
    Data->SetBoolField(TEXT("is_enabled_read_back"), ReadBack);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_handle_info
// Get detailed info for a specific emitter handle by name or index.
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_handle_info - Read one emitter handle of a Niagara System asset, chosen by display name or 0-based index: its id, id name, enabled and valid flags, version GUID and, when it has emitter data, its local-space and deprecated flags. Read-only; list every handle with get_niagara_emitter_handles.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param emitter_name - Display name of the handle (case-insensitive); when non-empty it wins over emitter_index.
//   @param emitter_index - 0-based position in the emitter list of the system, used only when emitter_name is absent or empty.
//   @returns { system_path: string, emitter_name: string, emitter_id: string, emitter_id_name: string, is_enabled: bool, is_valid: bool, emitter_version: string, is_local_space?: bool, is_deprecated?: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterHandleInfo(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    FString Error;
    FNiagaraEmitterHandle* Target = Niagara_FindHandleByNameOrIndex(Params, System, Error);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, Target->GetName().ToString());
    Data->SetStringField(TEXT("emitter_id"), Target->GetId().ToString());
    Data->SetStringField(TEXT("emitter_id_name"), Target->GetIdName().ToString());
    Data->SetBoolField(TEXT("is_enabled"), Target->GetIsEnabled());
    Data->SetBoolField(TEXT("is_valid"), Target->IsValid());

    // Try to get emitter data version info.
    FVersionedNiagaraEmitterData* EmitterData = Target->GetEmitterData();
    if (EmitterData)
    {
        Data->SetStringField(TEXT("emitter_version"), EmitterData->Version.VersionGuid.ToString());
        Data->SetBoolField(TEXT("is_local_space"), EmitterData->bLocalSpace);
        Data->SetBoolField(TEXT("is_deprecated"), EmitterData->bDeprecated);
    }
    else
    {
        Data->SetStringField(TEXT("emitter_version"), TEXT("N/A"));
    }

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// rename_niagara_emitter_handle
// Renames an emitter handle. Uses FNiagaraEmitterHandle::SetName which requires
// the owning system reference (to ensure name uniqueness).
// ---------------------------------------------------------------------------
// MCP:rename_niagara_emitter_handle - Rename an emitter handle of a Niagara System asset and save the system. The engine sanitizes the name and makes it unique among the emitters of the system, so read new_name and verified (false when the stored name differs from the request); a failed save is not reported.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param current_name - Current display name of the emitter handle (case-insensitive), as get_niagara_emitter_handles lists it.
//   @param new_name - Requested display name; it may come back sanitized or with a numeric suffix.
//   @returns { system_path: string, old_name: string, new_name: string, verified: bool }

TSharedPtr<FJsonObject> HandleRenameNiagaraEmitterHandle(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    FString CurrentName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("current_name"), CurrentName)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }

    FString Error;
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Target = Niagara_ResolveEmitterHandle(SystemPath, CurrentName, System, Error);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RenameNiagaraEmitterHandle", "Rename Niagara Emitter Handle"));
    System->Modify();

    Target->SetName(FName(*NewName), *System);

    // Read back to verify.
    const FString ReadBackName = Target->GetName().ToString();

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("old_name"), CurrentName);
    Data->SetStringField(TEXT("new_name"), ReadBackName);
    Data->SetBoolField(TEXT("verified"), ReadBackName == NewName);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Niagara simulation advanced
//
// These commands operate on FVersionedNiagaraEmitterData (sim target, sim
// stages) and UNiagaraEffectType (effect type, scalability) of UNiagaraSystem
// assets. All APIs verified against the engine source:
//   - FVersionedNiagaraEmitterData     NiagaraEmitter.h
//       SimTarget (ENiagaraSimTarget), bLocalSpace, bDeterministic
//       AddSimulationStage(UNiagaraSimulationStageBase*, FGuid)
//       GetSimulationStages()
//   - UNiagaraSimulationStageGeneric   NiagaraSimulationStageBase.h
//       IterationSource, NumIterations, ExecuteBehavior,
//       DirectDispatchType, ElementCountX/Y/Z, bGpuDispatchForceLinear
//   - UNiagaraEffectType               NiagaraEffectType.h
//       UpdateFrequency, CullReaction, bAllowCullingForLocalPlayers,
//       SystemScalabilitySettings, EmitterScalabilitySettings
//   - UNiagaraSystem                   NiagaraSystem.h
//       GetEffectType(), SetEffectType(UNiagaraEffectType*)
// ===========================================================================

namespace
{
    // Convert ENiagaraSimTarget to a human-readable string.
    FString SimTargetToString(ENiagaraSimTarget Target)
    {
        switch (Target)
        {
        case ENiagaraSimTarget::CPUSim:        return TEXT("CPUSim");
        case ENiagaraSimTarget::GPUComputeSim:  return TEXT("GPUComputeSim");
        default:                                return TEXT("Unknown");
        }
    }

    // Parse a string to ENiagaraSimTarget. Returns false if unrecognised.
    bool StringToSimTarget(const FString& Str, ENiagaraSimTarget& OutTarget)
    {
        if (Str.Equals(TEXT("CPUSim"), ESearchCase::IgnoreCase))
        {
            OutTarget = ENiagaraSimTarget::CPUSim;
            return true;
        }
        if (Str.Equals(TEXT("GPUComputeSim"), ESearchCase::IgnoreCase))
        {
            OutTarget = ENiagaraSimTarget::GPUComputeSim;
            return true;
        }
        return false;
    }

    // Safely convert a UENUM value to its name string. Returns "Unknown(<val>)"
    // when the enum metadata is unavailable.
    FString EnumValueToStringSafe(UEnum* EnumPtr, int64 Value)
    {
        if (EnumPtr)
        {
            return EnumPtr->GetNameStringByValue(Value);
        }
        return FString::Printf(TEXT("Unknown(%lld)"), Value);
    }

}

// ---------------------------------------------------------------------------
// get_niagara_emitter_sim_target
// Reads SimTarget, bLocalSpace and bDeterministic from the emitter's
// FVersionedNiagaraEmitterData (accessed via FNiagaraEmitterHandle::GetEmitterData).
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_sim_target - Get the simulation target configuration of a Niagara emitter.  Reads SimTarget, bLocalSpace and bDeterministic from the emitter's FVersionedNiagaraEmitterData.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Display name of the emitter handle.
//   @returns { system_path: string, emitter_name: string, sim_target: string, is_local_space: bool, is_deterministic: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterSimTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataUninitialised, EmitterName, System, EmitterData))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("sim_target"), SimTargetToString(EmitterData->SimTarget));
    Data->SetBoolField(TEXT("is_local_space"), EmitterData->bLocalSpace);
    Data->SetBoolField(TEXT("is_deterministic"), EmitterData->bDeterminism);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_emitter_sim_target
// Sets SimTarget, bLocalSpace and bDeterministic on the emitter's
// FVersionedNiagaraEmitterData. Saves the system after modification.
// ---------------------------------------------------------------------------
// MCP:set_niagara_emitter_sim_target - Configure the simulation target of a Niagara emitter.  Sets SimTarget, and optionally bLocalSpace and bDeterministic, on the emitter's FVersionedNiagaraEmitterData. The system is saved after modification.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param sim_target - Target type — "CPUSim" or "GPUComputeSim".
//   @param local_space - Optional bool override for bLocalSpace
//   @param deterministic - Optional bool override for bDeterminism
//   @returns { system_path: string, emitter_name: string, sim_target: string, success: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterSimTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString SimTargetStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("sim_target"), SimTargetStr)) { return Missing; }

    ENiagaraSimTarget NewTarget;
    if (!StringToSimTarget(SimTargetStr, NewTarget))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid sim_target '%s' (expected CPUSim or GPUComputeSim)"), *SimTargetStr));
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataUninitialised, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterSimTarget", "Set Niagara Emitter Sim Target"));
    System->Modify();

    EmitterData->SimTarget = NewTarget;

    // Optional bLocalSpace / bDeterminism overrides.
    bool bLocalSpace;
    if (Params->TryGetBoolField(TEXT("local_space"), bLocalSpace))
    {
        EmitterData->bLocalSpace = bLocalSpace;
    }
    bool bDeterministic;
    if (Params->TryGetBoolField(TEXT("deterministic"), bDeterministic))
    {
        EmitterData->bDeterminism = bDeterministic;
    }

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("sim_target"), SimTargetToString(EmitterData->SimTarget));
    Data->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_effect_type
// Reads the UNiagaraEffectType associated with a UNiagaraSystem and returns
// its key properties: UpdateFrequency, CullReaction, bAllowCullingForLocalPlayers.
// ---------------------------------------------------------------------------
// MCP:get_niagara_effect_type - Get the EffectType configuration of a Niagara system.  Reads the UNiagaraEffectType associated with the system and returns its key properties: UpdateFrequency, CullReaction, bAllowCullingForLocalPlayers.
//   @param system_path - Asset path of the Niagara System.
//   @returns { system_path: string, effect_type_path: string, update_frequency: string, cull_reaction: string, allows_culling: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraEffectType(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    UNiagaraEffectType* EffectType = System->GetEffectType();

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);

    if (!EffectType)
    {
        Data->SetStringField(TEXT("effect_type_path"), TEXT(""));
        Data->SetStringField(TEXT("update_frequency"), TEXT("None"));
        Data->SetStringField(TEXT("cull_reaction"), TEXT("None"));
        Data->SetBoolField(TEXT("allows_culling"), false);
        return mcp_common::MakeOkReply(Data);
    }

    Data->SetStringField(TEXT("effect_type_path"), EffectType->GetPathName());

    // Convert UENUM properties to strings via StaticEnum.
    UEnum* UpdateFreqEnum = StaticEnum<ENiagaraScalabilityUpdateFrequency>();
    Data->SetStringField(TEXT("update_frequency"),
        EnumValueToStringSafe(UpdateFreqEnum, (int64)EffectType->UpdateFrequency));

    UEnum* CullReactionEnum = StaticEnum<ENiagaraCullReaction>();
    Data->SetStringField(TEXT("cull_reaction"),
        EnumValueToStringSafe(CullReactionEnum, (int64)EffectType->CullReaction));

    Data->SetBoolField(TEXT("allows_culling"), EffectType->bAllowCullingForLocalPlayers);

    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_effect_type
// Assigns a UNiagaraEffectType asset to a UNiagaraSystem via SetEffectType
// (editor-only API). Saves the system after modification.
// ---------------------------------------------------------------------------
// MCP:set_niagara_effect_type - Assign an EffectType asset to a Niagara system.  Loads the UNiagaraEffectType from the given path and calls UNiagaraSystem::SetEffectType. The system is saved after modification.
//   @param system_path - Asset path of the Niagara System.
//   @param effect_type_path - Asset path of the UNiagaraEffectType to assign.
//   @returns { system_path: string, effect_type_path: string, success: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraEffectType(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    FString EffectTypePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("effect_type_path"), EffectTypePath)) { return Missing; }

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraEffectType* EffectType = mcp_common::ResolveAsset<UNiagaraEffectType>(EffectTypePath);
    if (!EffectType)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Niagara effect type not found: %s"), *EffectTypePath));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEffectType", "Set Niagara Effect Type"));
    System->Modify();

    System->SetEffectType(EffectType);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("effect_type_path"), EffectType->GetPathName());
    Data->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_scalability_settings
// Reads scalability configuration from the system's UNiagaraEffectType.
// Extracts MaxDistance / MaxInstances from the first system scalability entry
// and ScaleSpawnCount from the first emitter scalability entry.
// ---------------------------------------------------------------------------
// MCP:get_niagara_scalability_settings - Get the scalability configuration of a Niagara system.  Reads scalability settings from the system's UNiagaraEffectType: MaxDistance, MaxInstances (system-level), ScaleSpawnCount (emitter-level), and whether distance culling is active.
//   @param system_path - Asset path of the Niagara System.
//   @returns { system_path: string, effect_type_path: string, has_scalability_settings: bool, max_distance: number, max_instances: int, scale_spawn_count: number, distance_culling_enabled: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraScalabilitySettings(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    UNiagaraEffectType* EffectType = System->GetEffectType();
    if (!EffectType)
    {
        return mcp_common::MakeErrorReply(
            TEXT("System has no EffectType assigned; cannot read scalability settings"));
    }

    // Read system-level scalability (first entry, if any).
    float MaxDistance = 0.0f;
    int32 MaxInstances = 0;
    const auto& SysSettings = EffectType->SystemScalabilitySettings.Settings;
    if (SysSettings.Num() > 0)
    {
        MaxDistance = SysSettings[0].MaxDistance;
        MaxInstances = SysSettings[0].MaxInstances;
    }

    // Read emitter-level scalability (first entry, if any).
    float ScaleSpawnCount = 1.0f;
    const auto& EmtSettings = EffectType->EmitterScalabilitySettings.Settings;
    if (EmtSettings.Num() > 0)
    {
        ScaleSpawnCount = EmtSettings[0].SpawnCountScale;
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("max_distance"), MaxDistance);
    Data->SetNumberField(TEXT("max_instances"), MaxInstances);
    Data->SetBoolField(TEXT("cull_by_distance"), MaxDistance > 0.0f);
    Data->SetNumberField(TEXT("scale_spawn_count"), ScaleSpawnCount);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_niagara_simulation_stage
// Creates a UNiagaraSimulationStageGeneric, configures it with the requested
// iteration source and dispatch type, and adds it to the emitter's simulation
// stages via FVersionedNiagaraEmitterData::AddSimulationStage.
// ---------------------------------------------------------------------------
// MCP:add_niagara_simulation_stage - Add a simulation stage to a Niagara emitter.  Creates a UNiagaraSimulationStageGeneric, configures it with default iteration source (Particles), execute behavior (Always), and GPU dispatch type (OneD), then adds it to the emitter via FVersionedNiagaraEmitterData::AddSimulationStage. The system is saved after modification.  The stage's graph is created in a default state; further graph wiring must be completed in the Niagara editor graph.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param stage_name - Name for the new simulation stage.
//   @param iteration_count - Optional iteration count for the stage; defaults to 1
//   @returns { system_path: string, emitter_name: string, stage_name: string, requested_iteration_count: int, total_simulation_stages: int, success: bool }

TSharedPtr<FJsonObject> HandleAddNiagaraSimulationStage(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString StageName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stage_name"), StageName)) { return Missing; }

    // Add the stage to the emitter using the emitter's current version GUID.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraSimulationStage", "Add Niagara Simulation Stage"));
    System->Modify();
    Emitter->Modify();

    // Create the simulation stage generic object. The Outer MUST be the emitter
    // (matching the editor's add-stage path in NiagaraStackEmitterPropertiesGroup),
    // NOT the system — the stage is owned by the emitter's versioned data and the
    // engine relies on the emitter being its outer for correct serialisation.
    UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(
        Emitter, UNiagaraSimulationStageGeneric::StaticClass(), FName(*StageName),
        RF_Transactional);

    if (!Stage)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to create UNiagaraSimulationStageGeneric object"));
    }

    // CRITICAL: UNiagaraEmitter::AddSimulationStage() unconditionally dereferences
    // SimulationStage->Script (it registers a RapidIterationParameters change handler
    // under WITH_EDITOR). A stage with a null Script crashes the editor immediately.
    // The editor UI always allocates the Script (Outer = the stage) and assigns its
    // usage / usage id / graph source before calling AddSimulationStage, so we mirror
    // that exactly here.
    Stage->Script = NewObject<UNiagaraScript>(
        Stage, MakeUniqueObjectName(Stage, UNiagaraScript::StaticClass(), TEXT("SimulationStage")),
        RF_Transactional);
    if (!Stage->Script)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to create UNiagaraScript for the simulation stage"));
    }
    Stage->Script->SetUsage(ENiagaraScriptUsage::ParticleSimulationStageScript);
    Stage->Script->SetUsageId(Stage->GetMergeId());

    // Reuse the emitter's existing graph source for the stage script so it is
    // consistent with the rest of the emitter graph. The emitter spawn script's
    // latest source is the canonical editor-data source (see UNiagaraEmitter
    // script setup in NiagaraEmitter.cpp). If unavailable we leave the source null;
    // the stage still serialises and appears in GetSimulationStages().
    if (EmitterData->SpawnScriptProps.Script)
    {
        if (UNiagaraScriptSourceBase* EmitterSource = EmitterData->SpawnScriptProps.Script->GetLatestSource())
        {
            Stage->Script->SetLatestSource(EmitterSource);
        }
    }

    // Configure the stage with sensible defaults.
    Stage->IterationSource = ENiagaraIterationSource::Particles;
    Stage->ExecuteBehavior = ENiagaraSimStageExecuteBehavior::Always;
    Stage->DirectDispatchType = ENiagaraGpuDispatchType::OneD;
    Stage->SimulationStageName = FName(*StageName);

    int32 RequestedIterations = 1;
    Params->TryGetNumberField(TEXT("iteration_count"), RequestedIterations);

    Emitter->AddSimulationStage(Stage, EmitterData->Version.VersionGuid);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    // Verify the stage was added.
    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    const bool bAdded = Stages.Contains(Stage);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("stage_name"), StageName);
    Data->SetNumberField(TEXT("requested_iteration_count"), RequestedIterations);
    Data->SetNumberField(TEXT("total_simulation_stages"), Stages.Num());
    Data->SetBoolField(TEXT("success"), bAdded);
    // The verdict measured above is the verdict EMITTED.
    return mcp_common::MakeOutcomeReply(Data, bAdded,
        bAdded ? FString() : mcp_common::MeasuredOutcomeError(TEXT("add_niagara_simulation_stage"), TEXT("bAdded")));
}

// ===========================================================================
// set_niagara_simulation_stage_properties.
//
// add_niagara_simulation_stage creates a UNiagaraSimulationStageGeneric with
// hardcoded defaults (IterationSource=Particles, ExecuteBehavior=Always,
// DirectDispatchType=OneD) and exposes no setter. This is that setter: it
// writes the SETTABLE enum/bool/FName/FIntPoint UPROPERTYs on the LIVE stage by
// reflection (MergeJsonWithClamp — the exact mechanism set_niagara_renderer_properties
// uses; MinimalAPI is irrelevant to reflection). bEnabled is written via the base
// SetEnabled (NIAGARA_API) rather than reflection because it is a uint32:1 bitfield
// with editor side effects.
//
// HONEST GATE: NumIterations / ElementCountX|Y|Z / DataInterface are
// FNiagaraParameterBindingWithValue / binding structs (NiagaraSimulationStageBase.h,
// 100, 155+). ImportText cannot author them cleanly, so they are DELIBERATELY out of
// scope: any such key is reported in "not_supported" (never silently swallowed, never
// faked). The iteration count is already authored on the add path.
//
// Save/compile mirrors set_niagara_renderer_properties exactly: System->Modify() +
// Stage->Modify() + write + (enum writes -> PostEditChangeProperty) +
// System->RequestCompile(true) + SaveModifiedNiagaraSystem.
// ===========================================================================
namespace
{
    // Accepted snake_case input keys -> exact settable UPROPERTY on the stage.
    // Binding-valued / value fields are NOT in this table (see GSimStageBindingKeys).
    struct FNiagaraPropKey { const TCHAR* JsonKey; const TCHAR* PropName; };
    static const FNiagaraPropKey GSimStagePropKeys[] = {
        // base UNiagaraSimulationStageBase
        { TEXT("name"),                              TEXT("SimulationStageName") },
        { TEXT("simulation_stage_name"),             TEXT("SimulationStageName") },
        // UNiagaraSimulationStageGeneric — enum/bool/FIntPoint
        { TEXT("iteration_source"),                  TEXT("IterationSource") },
        { TEXT("execute_behavior"),                  TEXT("ExecuteBehavior") },
        { TEXT("disable_partial_particle_update"),   TEXT("bDisablePartialParticleUpdate") },
        { TEXT("particle_iteration_state_enabled"),  TEXT("bParticleIterationStateEnabled") },
        { TEXT("particle_iteration_state_range"),    TEXT("ParticleIterationStateRange") },
        { TEXT("gpu_dispatch_force_linear"),         TEXT("bGpuDispatchForceLinear") },
        { TEXT("direct_dispatch_type"),              TEXT("DirectDispatchType") },
        { TEXT("direct_dispatch_element_type"),      TEXT("DirectDispatchElementType") },
    };

    // Binding-valued / value keys we explicitly DO NOT author here (honest gate).
    static const TCHAR* const GSimStageBindingKeys[] = {
        TEXT("num_iterations"), TEXT("iteration_count"),
        TEXT("element_count_x"), TEXT("element_count_y"), TEXT("element_count_z"),
        TEXT("data_interface"),
    };

    // Enum-typed stage props whose write benefits from a PostEditChangeProperty so any
    // derived editor state recomputes (matches the renderer-property pattern).
    bool SimStagePropNeedsPostEdit(const TCHAR* PropName)
    {
        return FCString::Strcmp(PropName, TEXT("IterationSource")) == 0
            || FCString::Strcmp(PropName, TEXT("ExecuteBehavior")) == 0
            || FCString::Strcmp(PropName, TEXT("DirectDispatchType")) == 0
            || FCString::Strcmp(PropName, TEXT("DirectDispatchElementType")) == 0;
    }

    // Merges each supplied key whose UPROPERTY exists on Class into MergeJson (one write per property) and lists it
    // in Applied; the other supplied keys go to NotApplicable. Returns the last merged property NeedsPostEdit accepts.
    template <SIZE_T N>
    FProperty* Niagara_CollectSuppliedProps(const TSharedPtr<FJsonObject>& Params, const FNiagaraPropKey (&Keys)[N], UClass* Class,
        bool (*NeedsPostEdit)(const TCHAR*), const TSharedPtr<FJsonObject>& MergeJson, TArray<FString>& Applied,
        TArray<TSharedPtr<FJsonValue>>& NotApplicable)
    {
        FProperty* PostEditProp = nullptr;
        for (const FNiagaraPropKey& K : Keys)
        {
            TSharedPtr<FJsonValue> InVal = Params->TryGetField(K.JsonKey);
            if (!InVal.IsValid())
            {
                continue;
            }
            if (MergeJson->HasField(K.PropName))
            {
                continue;
            }
            FProperty* Prop = Class->FindPropertyByName(K.PropName);
            if (Prop == nullptr)
            {
                NotApplicable.Add(MakeShared<FJsonValueString>(K.JsonKey));
                continue;
            }
            MergeJson->SetField(K.PropName, InVal);
            Applied.Add(K.PropName);
            if (NeedsPostEdit(K.PropName))
            {
                PostEditProp = Prop;
            }
        }
        return PostEditProp;
    }
}

// ---------------------------------------------------------------------------
// set_niagara_simulation_stage_properties
// ---------------------------------------------------------------------------
// MCP:set_niagara_simulation_stage_properties - Author the settable enum/bool/FName/FIntPoint UPROPERTYs on a UNiagaraSimulationStageGeneric (the spawn-without-setter companion to add_niagara_simulation_stage).  Resolves the stage on an emitter by stage_index or stage_name, writes only the supplied fields by reflection (MergeJsonWithClamp), sets bEnabled via SetEnabled, fires PostEditChangeProperty for enum writes, then requests a system compile and saves. Idempotent. HONEST GATE: the binding-valued fields num_iterations/element_count_x|y|z/data_interface are FNiagaraParameterBindingWithValue / binding structs that ImportText cannot author here -- any such key is reported in not_supported, never faked (iteration count is authored on the add path).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle owning the stage.
//   @param stage_index - Optional 0-based index into the emitter's simulation stages (use add_niagara_simulation_stage / get_niagara_emitter_runtime_summary); takes precedence over stage_name
//   @param stage_name - Optional SimulationStageName to match if stage_index is not supplied.
//   @param name - Optional FName: SimulationStageName (renames the stage; also accepted as simulation_stage_name).
//   @param enabled - Optional bool: base bEnabled (written via SetEnabled).
//   @param iteration_source - Optional enum name: IterationSource (Particles|DataInterface|DirectSet).
//   @param execute_behavior - Optional enum name: ExecuteBehavior (Always|OnSimulationReset|NotOnSimulationReset).
//   @param disable_partial_particle_update - Optional bool: bDisablePartialParticleUpdate.
//   @param particle_iteration_state_enabled - Optional bool: bParticleIterationStateEnabled.
//   @param particle_iteration_state_range - Optional {X,Y} object: ParticleIterationStateRange (FIntPoint; the generic struct path needs the object form, not a [x,y] array).
//   @param gpu_dispatch_force_linear - Optional bool: bGpuDispatchForceLinear.
//   @param direct_dispatch_type - Optional enum name: DirectDispatchType (OneD|TwoD|ThreeD).
//   @param direct_dispatch_element_type - Optional enum name: DirectDispatchElementType (NumThreads|NumThreadsNoClipping|NumGroups).
//   @returns { system_path, emitter_name, stage_index, stage_name, stage_class, applied:[prop names], not_applicable:[keys], not_supported:[binding keys], enabled?: bool, post_edit_fired: bool, save_error? }
TSharedPtr<FJsonObject> HandleSetNiagaraSimulationStageProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataTemplate, EmitterName, System, EmitterData))
    {
        return Error;
    }

    // Resolve the target stage by index (preferred) or by SimulationStageName.
    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    UNiagaraSimulationStageGeneric* Stage = nullptr;
    int32 ResolvedIndex = INDEX_NONE;

    int32 StageIndex = INDEX_NONE;
    if (Params->TryGetNumberField(TEXT("stage_index"), StageIndex) && StageIndex >= 0)
    {
        if (StageIndex >= Stages.Num())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("stage_index %d out of range (count=%d)"), StageIndex, Stages.Num()));
        }
        Stage = Cast<UNiagaraSimulationStageGeneric>(Stages[StageIndex]);
        ResolvedIndex = StageIndex;
    }
    else
    {
        FString StageName;
        if (!Params->TryGetStringField(TEXT("stage_name"), StageName) || StageName.IsEmpty())
        {
            return mcp_common::MakeErrorReply(
                TEXT("Provide 'stage_index' (0-based) or 'stage_name' to identify the simulation stage"));
        }
        const FName Target(*StageName);
        for (int32 i = 0; i < Stages.Num(); ++i)
        {
            if (Stages[i] && Stages[i]->SimulationStageName == Target)
            {
                Stage = Cast<UNiagaraSimulationStageGeneric>(Stages[i]);
                ResolvedIndex = i;
                break;
            }
        }
        if (ResolvedIndex == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Simulation stage not found by name: %s"), *StageName));
        }
    }
    if (!Stage)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Simulation stage at index %d is not a UNiagaraSimulationStageGeneric"), ResolvedIndex));
    }

    // Honest gate: surface any binding-valued keys the caller supplied. These are
    // FNiagaraParameterBindingWithValue / binding structs (NumIterations / ElementCountX|Y|Z
    // / DataInterface) that ImportText cannot author cleanly — reported, never written.
    TArray<TSharedPtr<FJsonValue>> NotSupported;
    for (const TCHAR* BK : GSimStageBindingKeys)
    {
        if (Params->HasField(BK))
        {
            NotSupported.Add(MakeShared<FJsonValueString>(FString(BK)));
        }
    }

    // Build the reflection merge-JSON keyed by the EXACT UPROPERTY name. Only props
    // present on the concrete stage class are merged; the rest go to not_applicable.
    TSharedPtr<FJsonObject> MergeJson = MakeShared<FJsonObject>();
    TArray<FString> AppliedProps;
    TArray<TSharedPtr<FJsonValue>> NotApplicable;
    FProperty* EnumPropForPostEdit = Niagara_CollectSuppliedProps(Params, GSimStagePropKeys, Stage->GetClass(),
        &SimStagePropNeedsPostEdit, MergeJson, AppliedProps, NotApplicable);

    // bEnabled is a uint32:1 bitfield with side effects — write it via SetEnabled,
    // NOT reflection. Tracked separately from the merge set.
    const bool bHasEnabled = Params->HasField(TEXT("enabled"));
    bool bEnabledValue = false;
    if (bHasEnabled)
    {
        Params->TryGetBoolField(TEXT("enabled"), bEnabledValue);
    }

    if (AppliedProps.Num() == 0 && !bHasEnabled)
    {
        // Nothing writable was supplied. If the caller ONLY sent binding keys, say so
        // honestly rather than returning a vacuous success.
        const FString Hint = NotSupported.Num() > 0
            ? TEXT(" (only binding-valued fields supplied; those are not authorable here -- see not_supported)")
            : TEXT("");
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No settable simulation-stage properties supplied%s"), *Hint));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSimStageProperties", "Set Niagara Simulation Stage Properties"));
    System->Modify();
    Stage->Modify();

    if (AppliedProps.Num() > 0)
    {
        FString MergeError;
        if (!mcp_common::MergeJsonWithClamp(MergeJson, Stage->GetClass(), Stage, MergeError))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to apply simulation-stage properties: %s"), *MergeError));
        }
    }

    // Enum writes need the stage notified so derived state recomputes (matches the
    // renderer-property pattern; the editor UI fires this on edit).
    const bool bNeedsPostEdit = (EnumPropForPostEdit != nullptr);
    if (bNeedsPostEdit)
    {
        FPropertyChangedEvent ChangeEvent(EnumPropForPostEdit, EPropertyChangeType::ValueSet);
        Stage->PostEditChangeProperty(ChangeEvent);
    }

    if (bHasEnabled)
    {
        // SetEnabled is WITH_EDITORONLY_DATA on the base class; this plugin runs in the
        // editor, so this is the canonical path (it also requests a recompile internally).
        Stage->SetEnabled(bEnabledValue);
    }

    System->RequestCompile(true);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    // Build the report (round-trip the enum/name state the editor now holds).
    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("stage_index"), ResolvedIndex);
    Data->SetStringField(TEXT("stage_name"), Stage->SimulationStageName.ToString());
    Data->SetStringField(TEXT("stage_class"), Stage->GetClass()->GetName());
    Data->SetArrayField(TEXT("applied"), mcp_common::StringsToJson(AppliedProps));
    Data->SetArrayField(TEXT("not_applicable"), NotApplicable);
    Data->SetArrayField(TEXT("not_supported"), NotSupported);
    if (bHasEnabled)
    {
        Data->SetBoolField(TEXT("enabled"), Stage->bEnabled != 0);
    }
    Data->SetBoolField(TEXT("post_edit_fired"), bNeedsPostEdit);
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Remove + reorder simulation stages.
//
// Round-trip the lifecycle of a UNiagaraSimulationStageBase owned by
// FVersionedNiagaraEmitterData::SimulationStages: the delete + the reorder.
// Identical pattern to add_niagara_simulation_stage (resolve system -> emitter
// handle -> EmitterData -> call UNiagaraEmitter::{Remove,Move}SimulationStage*
// with the version GUID -> System->RequestCompile(true) -> SaveModifiedNiagaraSystem).
// Idempotent: the resolve-first linear search lets us report
// already_removed / already_at_target instead of pretending to act.
//
// APIs verified against Engine/Plugins/FX/Niagara/Source/Niagara/Classes/NiagaraEmitter.h:
//   UNiagaraEmitter::RemoveSimulationStage(UNiagaraSimulationStageBase*, FGuid) :989 (NIAGARA_API)
//   UNiagaraEmitter::MoveSimulationStageToIndex(UNiagaraSimulationStageBase*, int32, FGuid) :990 (NIAGARA_API)
//   UNiagaraSimulationStageBase::SimulationStageName                         NiagaraSimulationStageBase.h
// ===========================================================================

namespace
{
    // Resolve a UNiagaraSimulationStageBase* on the emitter's simulation stages
    // array. If StageIndex is present and in range it wins (the canonical handle,
    // mirroring get_niagara_emitter_runtime_summary which reports indices). If
    // StageName is supplied we match SimulationStageName (the human-readable FName
    // set by add_niagara_simulation_stage). Returns nullptr if neither matched.
    // OutIndex gets the resolved 0-based position (or INDEX_NONE on miss).
    UNiagaraSimulationStageBase* ResolveSimulationStage(
        FVersionedNiagaraEmitterData* EmitterData,
        int32 InRequestedIndex,
        const FString& InRequestedName,
        int32& OutIndex)
    {
        OutIndex = INDEX_NONE;
        if (!EmitterData)
        {
            return nullptr;
        }
        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        const bool bHasIndex = (InRequestedIndex >= 0);
        const bool bHasName  = !InRequestedName.IsEmpty();
        if (!bHasIndex && !bHasName)
        {
            return nullptr;
        }
        if (bHasIndex)
        {
            if (Stages.IsValidIndex(InRequestedIndex))
            {
                UNiagaraSimulationStageBase* S = Stages[InRequestedIndex];
                if (S && (!bHasName || S->SimulationStageName.ToString().Equals(InRequestedName, ESearchCase::IgnoreCase)))
                {
                    OutIndex = InRequestedIndex;
                    return S;
                }
            }
            // Index out of range -> fall through to name lookup so the caller
            // gets the real error path (stage-not-found), not a confusing
            // out-of-range when the name still uniquely identifies a stage.
        }
        if (bHasName)
        {
            for (int32 i = 0; i < Stages.Num(); ++i)
            {
                UNiagaraSimulationStageBase* S = Stages[i];
                if (S && S->SimulationStageName.ToString().Equals(InRequestedName, ESearchCase::IgnoreCase))
                {
                    OutIndex = i;
                    return S;
                }
            }
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// remove_niagara_simulation_stage
// Resolves the stage on the emitter's simulation stages (by stage_index or
// stage_name), calls UNiagaraEmitter::RemoveSimulationStage under a scoped
// transaction, then requests a compile + saves. Idempotent: a missing stage
// reports already_removed:true with the up-to-date stage count.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_simulation_stage - Remove a Niagara simulation stage from an emitter's stage list by stage_index or stage_name.  Resolves the stage on FVersionedNiagaraEmitterData::GetSimulationStages() (stage_index preferred, stage_name matches SimulationStageName), then calls UNiagaraEmitter::RemoveSimulationStage with the emitter version GUID, requests a system compile and saves the asset. Idempotent: a missing stage reports already_removed:true with the current total_simulation_stages. NiagaraEmitter.h (NIAGARA_API).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle owning the stage.
//   @param stage_index - Optional 0-based index into the emitter's simulation stages; takes precedence over stage_name when both are supplied.
//   @param stage_name - Optional SimulationStageName to match if stage_index is not supplied (matches add_niagara_simulation_stage's name).
//   @returns { system_path, emitter_name, stage_index, stage_name, stage_class, removed, already_removed, total_simulation_stages, saved, save_error? }

TSharedPtr<FJsonObject> HandleRemoveNiagaraSimulationStage(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 StageIndex = INDEX_NONE;
    Params->TryGetNumberField(TEXT("stage_index"), StageIndex);
    FString StageName;
    Params->TryGetStringField(TEXT("stage_name"), StageName);
    if (StageIndex < 0 && StageName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required stage identifier: supply 'stage_index' (>=0) or 'stage_name'"));
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    int32 ResolvedIndex = INDEX_NONE;
    UNiagaraSimulationStageBase* Stage = ResolveSimulationStage(EmitterData, StageIndex, StageName, ResolvedIndex);
    if (!Stage)
    {
        const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
        TSharedPtr<FJsonObject> Resp = Niagara_StartEmitterReply(System, EmitterName);
        Resp->SetNumberField(TEXT("stage_index"), (StageIndex >= 0) ? StageIndex : INDEX_NONE);
        Resp->SetStringField(TEXT("stage_name"), StageName);
        Resp->SetNumberField(TEXT("total_simulation_stages"), Stages.Num());
        Resp->SetBoolField(TEXT("removed"), false);
        Resp->SetBoolField(TEXT("already_removed"), true);
        Resp->SetBoolField(TEXT("saved"), true);
        UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] remove_niagara_simulation_stage: stage not found on emitter '%s' (idempotent no-op)"), *EmitterName);
        return mcp_common::MakeOkReply(Resp);
    }

    const FString ResolvedName = Stage->SimulationStageName.ToString();
    const FString ResolvedClass = Stage->GetClass()->GetName();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraSimulationStage", "Remove Niagara Simulation Stage"));
    System->Modify();
    Emitter->Modify();

    Emitter->RemoveSimulationStage(Stage, EmitterData->Version.VersionGuid);

    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Verify removal by reading the live array after the engine mutation (the
    // engine does not return success/fail; we have to re-read).
    const TArray<UNiagaraSimulationStageBase*>& StagesAfter = EmitterData->GetSimulationStages();
    const bool bStillThere = StagesAfter.Contains(Stage);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("stage_index"), ResolvedIndex);
    Data->SetStringField(TEXT("stage_name"), ResolvedName);
    Data->SetStringField(TEXT("stage_class"), ResolvedClass);
    Data->SetBoolField(TEXT("removed"), !bStillThere);
    Data->SetBoolField(TEXT("already_removed"), false);
    Data->SetNumberField(TEXT("total_simulation_stages"), StagesAfter.Num());
    if (bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_niagara_simulation_stage"));
}

// ---------------------------------------------------------------------------
// move_niagara_simulation_stage
// Resolves the stage (by stage_index or stage_name), then calls
// UNiagaraEmitter::MoveSimulationStageToIndex under a scoped transaction,
// requests a compile + saves. Idempotent: an in-position move is reported as
// already_at_target:true with the live before/after positions so callers can
// confirm the asset didn't drift between calls.
// ---------------------------------------------------------------------------
// MCP:move_niagara_simulation_stage - Reorder a Niagara simulation stage on its emitter's stage list by moving it to a target index.  Resolves the stage on FVersionedNiagaraEmitterData::GetSimulationStages() (stage_index or stage_name), validates target_index is in range, then calls UNiagaraEmitter::MoveSimulationStageToIndex with the emitter version GUID, requests a system compile and saves the asset. Idempotent: an in-position move reports already_at_target:true with the live before/after indices. NiagaraEmitter.h (NIAGARA_API).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle owning the stage.
//   @param stage_index - Optional 0-based index into the emitter's simulation stages; takes precedence over stage_name when both are supplied.
//   @param stage_name - Optional SimulationStageName to match if stage_index is not supplied.
//   @param target_index - Required 0-based target index the stage should occupy after the move (must be in [0, total_simulation_stages)).
//   @returns { system_path, emitter_name, stage_index, stage_name, stage_class, target_index, moved, already_at_target, before_index, after_index, total_simulation_stages, saved, save_error? }

TSharedPtr<FJsonObject> HandleMoveNiagaraSimulationStage(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 StageIndex = INDEX_NONE;
    Params->TryGetNumberField(TEXT("stage_index"), StageIndex);
    FString StageName;
    Params->TryGetStringField(TEXT("stage_name"), StageName);
    if (StageIndex < 0 && StageName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required stage identifier: supply 'stage_index' (>=0) or 'stage_name'"));
    }
    int32 TargetIndex = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("target_index"), TargetIndex) || TargetIndex < 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'target_index' parameter (must be >= 0)"));
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const TArray<UNiagaraSimulationStageBase*>& StagesBefore = EmitterData->GetSimulationStages();
    if (!StagesBefore.IsValidIndex(TargetIndex))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("target_index %d out of range (total_simulation_stages=%d)"),
                TargetIndex, StagesBefore.Num()));
    }

    int32 BeforeIndex = INDEX_NONE;
    UNiagaraSimulationStageBase* Stage = ResolveSimulationStage(EmitterData, StageIndex, StageName, BeforeIndex);
    if (!Stage)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Simulation stage not found on emitter '%s' (stage_index=%d, stage_name='%s')"),
                *EmitterName, StageIndex, *StageName));
    }

    const FString ResolvedName = Stage->SimulationStageName.ToString();
    const FString ResolvedClass = Stage->GetClass()->GetName();

    if (BeforeIndex == TargetIndex)
    {
        // Idempotent no-op: stage is already where the caller asked. Still report
        // the live state so callers can confirm the asset wasn't mutated by another
        // process between lookups.
        TSharedPtr<FJsonObject> Resp = Niagara_StartEmitterReply(System, EmitterName);
        Resp->SetNumberField(TEXT("stage_index"), BeforeIndex);
        Resp->SetStringField(TEXT("stage_name"), ResolvedName);
        Resp->SetStringField(TEXT("stage_class"), ResolvedClass);
        Resp->SetNumberField(TEXT("target_index"), TargetIndex);
        Resp->SetBoolField(TEXT("moved"), false);
        Resp->SetBoolField(TEXT("already_at_target"), true);
        Resp->SetNumberField(TEXT("before_index"), BeforeIndex);
        Resp->SetNumberField(TEXT("after_index"), BeforeIndex);
        Resp->SetNumberField(TEXT("total_simulation_stages"), StagesBefore.Num());
        Resp->SetBoolField(TEXT("saved"), true);
        UE_LOG(LogTemp, Verbose, TEXT("[UEAgent] move_niagara_simulation_stage: stage '%s' already at target index %d (idempotent no-op)"),
            *ResolvedName, TargetIndex);
        return mcp_common::MakeOkReply(Resp);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "MoveNiagaraSimulationStage", "Move Niagara Simulation Stage"));
    System->Modify();
    Emitter->Modify();

    Emitter->MoveSimulationStageToIndex(Stage, TargetIndex, EmitterData->Version.VersionGuid);

    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Verify the new position by linear-searching the post-move array for the same
    // stage pointer (MoveSimulationStageToIndex returns void, so we re-read).
    const TArray<UNiagaraSimulationStageBase*>& StagesAfter = EmitterData->GetSimulationStages();
    int32 AfterIndex = StagesAfter.IndexOfByKey(Stage);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("stage_index"), BeforeIndex);
    Data->SetStringField(TEXT("stage_name"), ResolvedName);
    Data->SetStringField(TEXT("stage_class"), ResolvedClass);
    Data->SetNumberField(TEXT("target_index"), TargetIndex);
    Data->SetBoolField(TEXT("moved"), AfterIndex == TargetIndex);
    Data->SetBoolField(TEXT("already_at_target"), false);
    Data->SetNumberField(TEXT("before_index"), BeforeIndex);
    Data->SetNumberField(TEXT("after_index"), AfterIndex);
    Data->SetNumberField(TEXT("total_simulation_stages"), StagesAfter.Num());
    if (bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("move_niagara_simulation_stage"));
}

// ===========================================================================
// Emitter from template + module-to-stack
//
// add_niagara_emitter_from_template uses the canonical editor path
// FNiagaraEditorUtilities::AddEmitterToSystem (NIAGARAEDITOR_API static), which
// rebuilds the emitter's nodes and syncs the system overview graph — the same
// thing the editor's "Add Emitter" does. Unlike UNiagaraSystem::AddEmitterHandle
// (used by add_niagara_emitter_handle), it always creates a standalone COPY of the
// emitter inside the system.
//
// add_niagara_module_to_stack inserts an engine module UNiagaraScript into the
// ParticleSpawn or ParticleUpdate stack of an emitter via
// FNiagaraStackGraphUtilities::AddScriptModuleToStack (NIAGARAEDITOR_API). Only the
// module's script-default inputs are applied — per-instance input overrides are out
// of scope (reported as added_with_defaults=true).
//
// Engine APIs used:
//   FNiagaraEditorUtilities::AddEmitterToSystem(UNiagaraSystem&, UNiagaraEmitter&, FGuid, bool)
//   FNiagaraStackGraphUtilities::AddScriptModuleToStack(UNiagaraScript*, UNiagaraNodeOutput&, int32, FString, const FGuid&)
//   UNiagaraScriptSource::NodeGraph (UNiagaraGraph*)
//   UNiagaraGraph::FindOutputNode(ENiagaraScriptUsage, FGuid)

// ---------------------------------------------------------------------------
// add_niagara_emitter_from_template
// Loads a template/parent UNiagaraEmitter asset and adds a COPY of it to a
// UNiagaraSystem via the canonical FNiagaraEditorUtilities::AddEmitterToSystem.
// GUARD: AddEmitterToSystem CastChecked's System->GetEditorData() — if the system
// has no editor data (e.g. a cooked/headless system without editor data) it asserts,
// so we null-check GetEditorData() first and return a clean error.
// ---------------------------------------------------------------------------
// MCP:add_niagara_emitter_from_template - Add a copy of a template emitter to a Niagara system (canonical path).  Loads the template/parent UNiagaraEmitter asset and adds a standalone copy of it to the system via FNiagaraEditorUtilities::AddEmitterToSystem — the same operation the Niagara editor's "Add Emitter" performs. This rebuilds the emitter's nodes and syncs the system overview graph, then requests a compile and saves the system.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example")
//   @param emitter_asset_path - Asset path of the source UNiagaraEmitter template (e.g. "/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain")
//   @returns { system_path: string, source_emitter_asset: string, emitter_handle_id: string, emitter_handle_name: string, is_valid: bool, is_enabled: bool, emitters_before: int, total_emitters: int, success: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddNiagaraEmitterFromTemplate(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    FString EmitterAssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("emitter_asset_path"), EmitterAssetPath)) { return Missing; }

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // GUARD: AddEmitterToSystem CastChecked's the system editor data; assert if null.
    if (!System->GetEditorData())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Niagara system has no editor data; cannot add emitter (system not authorable headless)"));
    }

    // Load the source template emitter asset (try with and without .Object suffix).
    UNiagaraEmitter* Emitter = mcp_common::ResolveAsset<UNiagaraEmitter>(EmitterAssetPath);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Niagara emitter asset not found: %s"), *EmitterAssetPath));
    }

    const int32 BeforeCount = System->GetEmitterHandles().Num();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraEmitterFromTemplate", "Add Niagara Emitter From Template"));
    System->Modify();

    // Canonical add: creates a copy of the emitter, rebuilds nodes, syncs overview graph.
    const FGuid NewId = FNiagaraEditorUtilities::AddEmitterToSystem(
        *System, *Emitter, Emitter->GetExposedVersion().VersionGuid, /*bCreateCopy*/ true);

    // Trigger a compile so the freshly added emitter is valid.
    System->RequestCompile(false);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Locate the new handle by id to report its generated name.
    FString NewName;
    bool bEnabled = false;
    bool bValid = false;
    for (const FNiagaraEmitterHandle& H : System->GetEmitterHandles())
    {
        if (H.GetId() == NewId)
        {
            NewName = H.GetName().ToString();
            bEnabled = H.GetIsEnabled();
            bValid = H.IsValid();
            break;
        }
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("source_emitter_asset"), Emitter->GetPathName());
    Data->SetStringField(TEXT("emitter_handle_id"), NewId.ToString());
    Data->SetStringField(TEXT("emitter_handle_name"), NewName);
    Data->SetBoolField(TEXT("is_valid"), bValid);
    Data->SetBoolField(TEXT("is_enabled"), bEnabled);
    Data->SetNumberField(TEXT("emitters_before"), BeforeCount);
    Data->SetNumberField(TEXT("total_emitters"), System->GetEmitterHandles().Num());
    Data->SetBoolField(TEXT("success"), NewId.IsValid());
    mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
    // The verdict measured above is the verdict EMITTED. The verdict is BOTH things: the operation
    // succeeded AND it persisted. If the save fails (e.g. a read-only file) the change stays only
    // in memory, and that is not a success.
    return mcp_common::MakeOutcomeReply(Data, (NewId.IsValid()) && bSaved,
        ((NewId.IsValid()) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("add_niagara_emitter_from_template")));
}

namespace
{
    // The optional `script_group` param, "update" when absent or empty.
    FString Niagara_ReadScriptGroup(const TSharedPtr<FJsonObject>& Params)
    {
        FString ScriptGroup;
        Params->TryGetStringField(TEXT("script_group"), ScriptGroup);
        if (ScriptGroup.IsEmpty())
        {
            ScriptGroup = TEXT("update");
        }
        return ScriptGroup;
    }

    // "spawn" (any case) names the particle spawn stack, anything else the update stack.
    ENiagaraScriptUsage Niagara_ScriptGroupUsage(const FString& ScriptGroup)
    {
        return ScriptGroup.Equals(TEXT("spawn"), ESearchCase::IgnoreCase)
            ? ENiagaraScriptUsage::ParticleSpawnScript
            : ENiagaraScriptUsage::ParticleUpdateScript;
    }

    // Niagara_ReadScriptGroup for a command that accepts only "spawn" or "update" (any case), and its stack.
    bool Niagara_ReadStrictScriptGroup(const TSharedPtr<FJsonObject>& Params,
        FString& OutScriptGroup, ENiagaraScriptUsage& OutUsage, FString& OutError)
    {
        OutScriptGroup = Niagara_ReadScriptGroup(Params);
        if (OutScriptGroup.Equals(TEXT("spawn"), ESearchCase::IgnoreCase))
        {
            OutUsage = ENiagaraScriptUsage::ParticleSpawnScript;
            return true;
        }
        if (OutScriptGroup.Equals(TEXT("update"), ESearchCase::IgnoreCase))
        {
            OutUsage = ENiagaraScriptUsage::ParticleUpdateScript;
            return true;
        }
        OutError = FString::Printf(TEXT("Invalid script_group '%s' (expected 'spawn' or 'update')"), *OutScriptGroup);
        return false;
    }
}

// ---------------------------------------------------------------------------
// add_niagara_module_to_stack
// Loads an engine module UNiagaraScript and inserts it into the ParticleSpawn or
// ParticleUpdate stack of an emitter. Resolves the emitter's script source graph,
// finds the output node for the requested usage, then calls
// FNiagaraStackGraphUtilities::AddScriptModuleToStack.
// GUARD: stateless emitters / emitters without a graph have a null GraphSource,
// NodeGraph, or output node — return a clean error rather than crashing.
// NOTE: only the module's script-default inputs are applied; per-instance input
// overrides are out of scope (reported via added_with_defaults=true).
// ---------------------------------------------------------------------------
// MCP:add_niagara_module_to_stack - Insert an engine module script into a Niagara emitter's particle stack.  Loads the module UNiagaraScript and inserts it at the end of the emitter's ParticleSpawn or ParticleUpdate stack via FNiagaraStackGraphUtilities::AddScriptModuleToStack, then recompiles and saves.  NOTE: only the module's script-default inputs are applied — per-instance input overrides are out of scope (the response reports added_with_defaults=true). Stateless emitters (no editable graph) return a clean error.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle to modify.
//   @param module_script_path - Asset path of the module UNiagaraScript (e.g. "/Niagara/Modules/Update/Forces/GravityForce.GravityForce")
//   @param script_group - Optional "spawn" (ParticleSpawnScript) or "update" (ParticleUpdateScript); defaults to "update"
//   @returns { system_path: string, emitter_name: string, module_script_path: string, script_group: string, module_node_name: string, added_with_defaults: bool, success: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddNiagaraModuleToStack(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleScriptPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_script_path"), ModuleScriptPath)) { return Missing; }
    FString ScriptGroup;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!Niagara_ReadStrictScriptGroup(Params, ScriptGroup, Usage, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataUninitialised, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Resolve the emitter graph source (stateless emitters have none).
    UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
    if (!Src || !Src->NodeGraph)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Emitter has no editable script graph (stateless emitter or missing GraphSource)"));
    }

    // UNiagaraGraph::FindOutputNode is not NIAGARAEDITOR_API-exported (the class is
    // MinimalAPI). Get the output node from the emitter's per-usage script via the
    // exported FNiagaraEditorUtilities::GetScriptOutputNode instead.
    UNiagaraScript* GroupScript = (Usage == ENiagaraScriptUsage::ParticleSpawnScript)
        ? EmitterData->SpawnScriptProps.Script
        : EmitterData->UpdateScriptProps.Script;
    if (!GroupScript)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Emitter has no %s script"), *ScriptGroup));
    }
    UNiagaraNodeOutput* Out = FNiagaraEditorUtilities::GetScriptOutputNode(*GroupScript);
    if (!Out)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No %s output node found in the emitter graph"), *ScriptGroup));
    }

    // Load the engine module script.
    UNiagaraScript* Mod = mcp_common::ResolveAsset<UNiagaraScript>(ModuleScriptPath);
    if (!Mod)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Niagara module script not found: %s"), *ModuleScriptPath));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraModuleToStack", "Add Niagara Module To Stack"));
    System->Modify();
    Src->NodeGraph->Modify();

    // Insert at the end of the stack (INDEX_NONE). Only script-default inputs apply.
    UNiagaraNodeFunctionCall* NewModule =
        FNiagaraStackGraphUtilities::AddScriptModuleToStack(Mod, *Out, INDEX_NONE);

    if (!NewModule)
    {
        return mcp_common::MakeErrorReply(
            TEXT("AddScriptModuleToStack returned null (module could not be inserted into the stack)"));
    }

    // Force a recompile so the new module is reflected (UNiagaraGraph::NotifyGraphNeedsRecompile
    // is not exported cross-module; a forced RequestCompile covers the dirtied graph).
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("module_script_path"), Mod->GetPathName());
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_node_name"), NewModule->GetFunctionName());
    Data->SetBoolField(TEXT("added_with_defaults"), true);
    Data->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_niagara_module_to_stack"), SaveError);
}

// ===========================================================================
// Niagara module input override (rapid-iteration parameter store)
//
// set_niagara_module_input / get_niagara_module_input write/read a module
// function-call input's value by directly manipulating the emitter group script's
// RapidIterationParameters store — the exact headless mechanism that
// UNiagaraStackFunctionInput uses for RI-eligible inputs, but with NO view-model.
//
// The mechanism (mirrors NiagaraStackGraphUtilities::CreateRapidIterationParameter
// + NiagaraStackFunctionInput::SetRapidIterationParameter):
//   1. Resolve the emitter handle by display name -> FVersionedNiagaraEmitter +
//      FVersionedNiagaraEmitterData.
//   2. Find the module UNiagaraNodeFunctionCall by name on the spawn OR update
//      group graph; pick the matching group script (Spawn/UpdateScriptProps.Script).
//   3. Enumerate the module inputs with the EXPORTED
//      FNiagaraStackGraphUtilities::GetStackFunctionInputs(..., ModuleInputsOnly),
//      using a header-inline FCompileConstantResolver. Match the input whose leaf
//      name (after the "Module." namespace) == input_name.
//   4. RI-eligibility gate (inline — engine's IsRapidIterationType is NOT exported):
//      eligible iff NOT static, type != Bool, not an enum, type != ParameterMap, and
//      not a UObject/DataInterface. Otherwise return a clean honest error.
//   5. Build the rapid-iteration variable:
//        Aliased = FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(
//                      FNiagaraParameterHandle(InputVar.GetName()), ModuleNode);
//        RIVar(InputVar.GetType(), Aliased.GetParameterHandleString());
//        RIVar = FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(
//                    RIVar, *UniqueEmitterName, ScriptUsage);
//      then GroupScript->RapidIterationParameters.SetParameterData(bytes, RIVar, true).
//   6. Read back via IndexOf + GetParameterData to verify.
//
// LNK TRAP: FNiagaraStackGraphUtilities::CreateRapidIterationParameter &
// IsRapidIterationType are NOT exported — do NOT call them. We use
// FNiagaraUtilities::ConvertVariableToRapidIterationConstantName (NIAGARA_API) and
// inline the eligibility predicate.
// ===========================================================================

namespace
{
    // Resolve the spawn/update group: returns the matching FVersionedNiagaraEmitterData,
    // the FVersionedNiagaraEmitter handle instance, the group UNiagaraScript, the script
    // usage, and the module UNiagaraNodeFunctionCall whose name == ModuleNodeName, by
    // searching BOTH the spawn and update group graphs. Returns false if not found.
    bool ResolveModuleNodeInGroups(
        UNiagaraSystem* System,
        const FString& EmitterDisplayName,
        const FString& ModuleNodeName,
        FVersionedNiagaraEmitter& OutVersionedEmitter,
        FVersionedNiagaraEmitterData*& OutEmitterData,
        UNiagaraScript*& OutGroupScript,
        ENiagaraScriptUsage& OutUsage,
        UNiagaraNodeFunctionCall*& OutModuleNode,
        FString& OutError)
    {
        OutEmitterData = nullptr;
        OutGroupScript = nullptr;
        OutModuleNode = nullptr;

        FNiagaraEmitterHandle* Handle = FindEmitterHandleByName(System, EmitterDisplayName);
        if (!Handle)
        {
            OutError = FString::Printf(TEXT("Emitter handle not found: %s"), *EmitterDisplayName);
            return false;
        }
        FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
        if (!EmitterData)
        {
            OutError = TEXT("Emitter data is null (handle may not be initialised)");
            return false;
        }

        OutVersionedEmitter = Handle->GetInstance();
        OutEmitterData = EmitterData;

        // Search spawn then update group for the module function-call node by name.
        const ENiagaraScriptUsage Usages[2] = {
            ENiagaraScriptUsage::ParticleSpawnScript,
            ENiagaraScriptUsage::ParticleUpdateScript };
        UNiagaraScript* const GroupScripts[2] = {
            EmitterData->SpawnScriptProps.Script,
            EmitterData->UpdateScriptProps.Script };

        for (int32 i = 0; i < 2; ++i)
        {
            UNiagaraScript* GroupScript = GroupScripts[i];
            if (!GroupScript)
            {
                continue;
            }
            UNiagaraNodeOutput* Out = FNiagaraEditorUtilities::GetScriptOutputNode(*GroupScript);
            if (!Out)
            {
                continue;
            }
            UNiagaraGraph* Graph = Cast<UNiagaraGraph>(Out->GetGraph());
            if (!Graph)
            {
                continue;
            }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                UNiagaraNodeFunctionCall* FuncNode = Cast<UNiagaraNodeFunctionCall>(Node);
                if (!FuncNode)
                {
                    continue;
                }
                if (FuncNode->GetFunctionName() == ModuleNodeName ||
                    FuncNode->NodeGuid.ToString() == ModuleNodeName ||
                    FuncNode->GetName() == ModuleNodeName)
                {
                    OutGroupScript = GroupScript;
                    OutUsage = Usages[i];
                    OutModuleNode = FuncNode;
                    return true;
                }
            }
        }

        OutError = FString::Printf(
            TEXT("Module node '%s' not found in the spawn or update stack of emitter '%s'"),
            *ModuleNodeName, *EmitterDisplayName);
        return false;
    }

    // Inline RI-eligibility predicate (engine's FNiagaraStackGraphUtilities::IsRapidIterationType
    // is NOT exported). Eligible iff: not static, not bool, not enum, not parameter-map,
    // not a UObject/data-interface type.
    bool IsRapidIterationEligible(const FNiagaraTypeDefinition& Type)
    {
        if (Type.IsStatic())                         { return false; }
        if (Type == FNiagaraTypeDefinition::GetBoolDef()) { return false; }
        if (Type.IsEnum())                           { return false; }
        if (Type == FNiagaraTypeDefinition::GetParameterMapDef()) { return false; }
        if (Type.IsUObject() || Type.IsDataInterface()) { return false; }
        return true;
    }

    // Extract the leaf name of an input variable: the part after the trailing
    // "Module." namespace (e.g. "Module.Drag" -> "Drag"). If no namespace, returns as-is.
    FString InputLeafName(const FNiagaraVariable& Var)
    {
        FString Full = Var.GetName().ToString();
        int32 DotIdx = INDEX_NONE;
        if (Full.FindLastChar(TEXT('.'), DotIdx) && DotIdx != INDEX_NONE && DotIdx + 1 < Full.Len())
        {
            return Full.RightChop(DotIdx + 1);
        }
        return Full;
    }

    // The input of ModuleNode whose leaf name is InputName; false with OutError listing the inputs otherwise.
    bool Niagara_FindModuleInput(const FVersionedNiagaraEmitter& VersionedEmitter, ENiagaraScriptUsage ScriptUsage,
        const UNiagaraNodeFunctionCall& ModuleNode, const FString& InputName, FNiagaraVariable& OutInput, FString& OutError)
    {
        FCompileConstantResolver Resolver(VersionedEmitter, ScriptUsage);
        TArray<FNiagaraVariable> Inputs;
        TSet<FNiagaraVariable> Hidden;
        FNiagaraStackGraphUtilities::GetStackFunctionInputs(
            ModuleNode, Inputs, Hidden, Resolver,
            FNiagaraStackGraphUtilities::ENiagaraGetStackFunctionInputPinsOptions::ModuleInputsOnly);

        TArray<FString> AvailableLeaves;
        for (const FNiagaraVariable& Var : Inputs)
        {
            const FString Leaf = InputLeafName(Var);
            AvailableLeaves.Add(Leaf);
            if (Leaf == InputName)
            {
                OutInput = Var;
                return true;
            }
        }
        OutError = FString::Printf(TEXT("Module input '%s' not found. Available inputs: %s"),
            *InputName, *FString::Join(AvailableLeaves, TEXT(", ")));
        return false;
    }

    // The rapid-iteration parameter that overrides InputVar of ModuleNode (mirrors CreateRapidIterationParameter).
    FNiagaraVariable Niagara_RapidIterationVariable(const FNiagaraVariable& InputVar,
        const UNiagaraNodeFunctionCall* ModuleNode, UNiagaraEmitter* Emitter, ENiagaraScriptUsage ScriptUsage)
    {
        const FNiagaraParameterHandle InputHandle(InputVar.GetName());
        const FNiagaraParameterHandle Aliased =
            FNiagaraParameterHandle::CreateAliasedModuleParameterHandle(InputHandle, ModuleNode);
        const FString UniqueEmitterName = Emitter->GetUniqueEmitterName();
        const FNiagaraVariable RIVar(InputVar.GetType(), Aliased.GetParameterHandleString());
        return FNiagaraUtilities::ConvertVariableToRapidIterationConstantName(RIVar, *UniqueEmitterName, ScriptUsage);
    }

    // A module input: the `emitter_name` and `input_name` params, and the system, emitter, stack script, module node
    // and input variable they resolve to.
    struct FNiagaraModuleInputTarget
    {
        FString EmitterName;
        FString InputName;
        UNiagaraSystem* System = nullptr;
        UNiagaraEmitter* Emitter = nullptr;
        UNiagaraScript* GroupScript = nullptr;
        ENiagaraScriptUsage ScriptUsage = ENiagaraScriptUsage::ParticleUpdateScript;
        UNiagaraNodeFunctionCall* ModuleNode = nullptr;
        FNiagaraVariable InputVar;
    };

    // Reads the required `system_path`, `emitter_name`, `module_node_name` and `input_name`, then finds the module node
    // on the emitter's spawn or update stack and its input; null on success, else the error reply.
    TSharedPtr<FJsonObject> Niagara_ResolveModuleInput(const TSharedPtr<FJsonObject>& Params, FNiagaraModuleInputTarget& Out)
    {
        FString SystemPath;
        FString Error;
        if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, Out.EmitterName, Error))
        {
            return mcp_common::MakeErrorReply(Error);
        }
        FString ModuleNodeName;
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_node_name"), ModuleNodeName))
        {
            return Missing;
        }
        if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("input_name"), Out.InputName))
        {
            return Missing;
        }

        Out.System = Niagara_ResolveSystem(SystemPath, Error);
        if (!Out.System)
        {
            return mcp_common::MakeErrorReply(Error);
        }
        FVersionedNiagaraEmitter VersionedEmitter;
        FVersionedNiagaraEmitterData* EmitterData = nullptr;
        if (!ResolveModuleNodeInGroups(Out.System, Out.EmitterName, ModuleNodeName,
                VersionedEmitter, EmitterData, Out.GroupScript, Out.ScriptUsage, Out.ModuleNode, Error))
        {
            return mcp_common::MakeErrorReply(Error);
        }
        Out.Emitter = VersionedEmitter.Emitter;
        if (!Out.Emitter)
        {
            return mcp_common::MakeErrorReply(
                TEXT("Versioned emitter instance has a null emitter (stateless emitter?)"));
        }
        // Enumerate the module inputs to locate the target and capture its real type.
        if (!Niagara_FindModuleInput(VersionedEmitter, Out.ScriptUsage, *Out.ModuleNode, Out.InputName, Out.InputVar, Error))
        {
            return mcp_common::MakeErrorReply(Error);
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// set_niagara_module_input
// Writes a module input override into the emitter group script's
// RapidIterationParameters store. value_type selects how the JSON value is packed:
//   "float" -> float (4 bytes); "vector" -> FVector3f (12); "int" -> int32 (4);
//   "color" -> FLinearColor (16). The packed type MUST match the input's actual
//   FNiagaraTypeDefinition (from GetStackFunctionInputs) or a clean error is returned.
// HONEST GATE: bool/enum/data-interface/object inputs are not RI-eligible -> clean error.
// ---------------------------------------------------------------------------
// MCP:set_niagara_module_input - Override a Niagara module input by writing the rapid-iteration store.  Locates the module function-call node (by its module_node_name, as returned by add_niagara_module_to_stack) on the emitter's spawn or update stack, matches the input by its leaf name, and writes the value into the owning group script's RapidIterationParameters store. The system is recompiled and saved.  HONEST GATE: bool / enum / data-interface / object inputs are not rapid-iteration eligible and return a clean error ("override of <type> inputs requires the linked-value path, not supported headless"). The packed value_type must also match the input's actual type or a clean error is returned.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Display name of the emitter handle.
//   @param module_node_name - The module function-call node name (from add_niagara_module_to_stack's module_node_name).
//   @param input_name - Leaf name of the module input (e.g. "Drag" for "Module.Drag").
//   @param value_type - One of "float", "int", "vector", or "color"; defaults to "float"
//   @param value - For float/int: scalar number. For vector: an [x, y, z] list. For color: an [r, g, b, a] list.
//   @returns { system_path: string, emitter_name: string, module_node_name: string, input_name: string, value_type: string, ri_param_name: string, verified: bool, success: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraModuleInput(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraModuleInputTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveModuleInput(Params, Target))
    {
        return Error;
    }
    FString ValueType = TEXT("float");
    Params->TryGetStringField(TEXT("value_type"), ValueType);
    ValueType = ValueType.ToLower();

    const FNiagaraTypeDefinition InputType = Target.InputVar.GetType();

    // HONEST GATE: only RI-eligible scalar/vector/color/int inputs are supported headless.
    if (!IsRapidIterationEligible(InputType))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(
                TEXT("override of %s inputs requires the linked-value path, not supported headless"),
                *InputType.GetName()));
    }

    // Validate value_type against the actual input type, and pack native bytes.
    // We use the actual InputType for the RI variable, but value_type selects packing.
    TArray<uint8> Bytes;
    FString CanonicalType;

    if (ValueType == TEXT("float"))
    {
        if (InputType != FNiagaraTypeDefinition::GetFloatDef())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Input '%s' is of type %s, not float"), *Target.InputName, *InputType.GetName()));
        }
        double V = 0.0;
        Params->TryGetNumberField(TEXT("value"), V);
        float F = (float)V;
        Bytes.Append((const uint8*)&F, sizeof(float));
        CanonicalType = TEXT("float");
    }
    else if (ValueType == TEXT("int") || ValueType == TEXT("int32"))
    {
        if (InputType != FNiagaraTypeDefinition::GetIntDef())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Input '%s' is of type %s, not int"), *Target.InputName, *InputType.GetName()));
        }
        double V = 0.0;
        Params->TryGetNumberField(TEXT("value"), V);
        int32 I = (int32)V;
        Bytes.Append((const uint8*)&I, sizeof(int32));
        CanonicalType = TEXT("int");
    }
    else if (ValueType == TEXT("vector") || ValueType == TEXT("vec3"))
    {
        if (InputType != FNiagaraTypeDefinition::GetVec3Def())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Input '%s' is of type %s, not vector"), *Target.InputName, *InputType.GetName()));
        }
        const FVector Vec = mcp_common::ReadJsonVector(Params, TEXT("value"));
        FVector3f V3((float)Vec.X, (float)Vec.Y, (float)Vec.Z);
        Bytes.Append((const uint8*)&V3, sizeof(FVector3f));
        CanonicalType = TEXT("vector");
    }
    else if (ValueType == TEXT("color") || ValueType == TEXT("linearcolor"))
    {
        if (InputType != FNiagaraTypeDefinition::GetColorDef())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Input '%s' is of type %s, not color"), *Target.InputName, *InputType.GetName()));
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        Params->TryGetArrayField(TEXT("value"), Arr);
        const FLinearColor Color = mcp_common::LinearColorFromJsonArray(Arr, FLinearColor(0.f, 0.f, 0.f, 1.f));
        Bytes.Append((const uint8*)&Color, sizeof(FLinearColor));
        CanonicalType = TEXT("color");
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unsupported value_type '%s' (expected float, int, vector, or color)"), *ValueType));
    }

    // Build the rapid-iteration variable (mirrors CreateRapidIterationParameter).
    const FNiagaraVariable RIVar = Niagara_RapidIterationVariable(Target.InputVar, Target.ModuleNode, Target.Emitter, Target.ScriptUsage);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraModuleInput", "Set Niagara Module Input"));
    Target.System->Modify();
    Target.GroupScript->Modify();

    const bool bWritten = Target.GroupScript->RapidIterationParameters.SetParameterData(
        Bytes.GetData(), RIVar, /*bAdd*/ true);

    // Read back to verify.
    bool bVerified = false;
    const int32 Off = Target.GroupScript->RapidIterationParameters.IndexOf(RIVar);
    if (Off != INDEX_NONE)
    {
        const uint8* P = Target.GroupScript->RapidIterationParameters.GetParameterData(Off, InputType);
        if (P)
        {
            bVerified = (FMemory::Memcmp(P, Bytes.GetData(), Bytes.Num()) == 0);
        }
    }

    Target.System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(Target.System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(Target.System, Target.EmitterName);
    Data->SetStringField(TEXT("module_node_name"), Target.ModuleNode->GetFunctionName());
    Data->SetStringField(TEXT("input_name"), Target.InputName);
    Data->SetStringField(TEXT("value_type"), CanonicalType);
    Data->SetStringField(TEXT("ri_param_name"), RIVar.GetName().ToString());
    Data->SetBoolField(TEXT("verified"), bVerified);
    Data->SetBoolField(TEXT("success"), bWritten);
    mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
    // The verdict measured above is the verdict EMITTED. The verdict is BOTH things: the operation
    // succeeded AND it persisted. If the save fails (e.g. a read-only file) the change stays only
    // in memory, and that is not a success.
    return mcp_common::MakeOutcomeReply(Data, (bWritten) && bSaved,
        ((bWritten) && bSaved) ? FString() : mcp_common::SavedOutcomeError(TEXT("set_niagara_module_input")));
}

// ---------------------------------------------------------------------------
// get_niagara_module_input
// Reads a module input's current override from the emitter group script's
// RapidIterationParameters store. found=false when no RI override exists yet (the
// module is using its script default); value_type reflects the input's real type.
// ---------------------------------------------------------------------------
// MCP:get_niagara_module_input - Read a Niagara module input override from the rapid-iteration store.  Locates the module function-call node and the named input, reports the input's real type, and reads back any rapid-iteration override currently set for it. found=false means no override exists yet (the module is using its script default); in that case 'value' is omitted.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param module_node_name - The module function-call node name.
//   @param input_name - Leaf name of the module input (e.g. "Drag").
//   @returns { system_path: string, emitter_name: string, module_node_name: string, input_name: string, ri_param_name: string, value_type: string, found: bool, value?: number | [number, number, number] | [number, number, number, number], success: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraModuleInput(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraModuleInputTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveModuleInput(Params, Target))
    {
        return Error;
    }

    const FNiagaraTypeDefinition InputType = Target.InputVar.GetType();

    // Build the RI variable name (same as the setter) and read it back if present.
    const FNiagaraVariable RIVar = Niagara_RapidIterationVariable(Target.InputVar, Target.ModuleNode, Target.Emitter, Target.ScriptUsage);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(Target.System, Target.EmitterName);
    Data->SetStringField(TEXT("module_node_name"), Target.ModuleNode->GetFunctionName());
    Data->SetStringField(TEXT("input_name"), Target.InputName);
    Data->SetStringField(TEXT("ri_param_name"), RIVar.GetName().ToString());

    FString TypeStr;
    if (InputType == FNiagaraTypeDefinition::GetFloatDef())      { TypeStr = TEXT("float"); }
    else if (InputType == FNiagaraTypeDefinition::GetIntDef())   { TypeStr = TEXT("int"); }
    else if (InputType == FNiagaraTypeDefinition::GetVec3Def())  { TypeStr = TEXT("vector"); }
    else if (InputType == FNiagaraTypeDefinition::GetColorDef()) { TypeStr = TEXT("color"); }
    else if (InputType == FNiagaraTypeDefinition::GetBoolDef())  { TypeStr = TEXT("bool"); }
    else { TypeStr = InputType.GetName(); }
    Data->SetStringField(TEXT("value_type"), TypeStr);

    const int32 Off = Target.GroupScript->RapidIterationParameters.IndexOf(RIVar);
    const bool bFound = (Off != INDEX_NONE);
    Data->SetBoolField(TEXT("found"), bFound);

    if (bFound)
    {
        const uint8* P = Target.GroupScript->RapidIterationParameters.GetParameterData(Off, InputType);
        if (P)
        {
            if (InputType == FNiagaraTypeDefinition::GetFloatDef())
            {
                float F = 0.f; FMemory::Memcpy(&F, P, sizeof(float));
                Data->SetNumberField(TEXT("value"), F);
            }
            else if (InputType == FNiagaraTypeDefinition::GetIntDef())
            {
                int32 I = 0; FMemory::Memcpy(&I, P, sizeof(int32));
                Data->SetNumberField(TEXT("value"), I);
            }
            else if (InputType == FNiagaraTypeDefinition::GetVec3Def())
            {
                FVector3f V; FMemory::Memcpy(&V, P, sizeof(FVector3f));
                TArray<TSharedPtr<FJsonValue>> Arr = mcp_common::VectorToJsonArray(V);
                Data->SetArrayField(TEXT("value"), Arr);
            }
            else if (InputType == FNiagaraTypeDefinition::GetColorDef())
            {
                FLinearColor C; FMemory::Memcpy(&C, P, sizeof(FLinearColor));
                Data->SetArrayField(TEXT("value"), mcp_common::ColorToJsonArray(C));
            }
        }
    }

    Data->SetBoolField(TEXT("success"), true);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Niagara renderer authoring (Sprite / Ribbon / Mesh) + data interface
// bindings read.
//
// This is the DATA-ONLY half of the emitter's visual stack: which renderer
// properties are attached and which data interface bindings the emitter
// depends on. No rendering happens, no simulation runs — we only mutate the
// asset-level FVersionedNiagaraEmitterData graph. That keeps everything
// headless-safe and avoids GPU/PIE fragility.
//
// We do NOT add a "create a UNiagaraDataInterface instance and slot it into a
// script" path here, because that requires view-models (FNiagaraStackGraphUtilities
// AddDataInterface) and is editor-only — no stub, we just LIST via
// EmitterDependencies.
//
// Engine APIs (line numbers from the UE 5.8 source):
//   FVersionedNiagaraEmitterData::RendererProperties is PRIVATE (NiagaraEmitter.h).
//     We use the public mutators on UNiagaraEmitter: AddRenderer / RemoveRenderer.
//     For reads we use FVersionedNiagaraEmitterData::GetRenderers().
//   FVersionedNiagaraEmitterData::EmitterDependencies
//     TArray<FNiagaraDataInterfaceEmitterBinding>     NiagaraEmitter.h
//     The struct (NiagaraDataInterfaceEmitterBinding.h) has only two
//     fields: BindingMode (ENiagaraDataInterfaceEmitterBindingMode: Self|Other)
//     and EmitterName (FName). We list those.
//   UNiagaraSpriteRendererProperties::Material        TObjectPtr<UMaterialInterface>
//   UNiagaraMeshRendererProperties::Material         TObjectPtr<UMaterialInterface>
//   UNiagaraRibbonRendererProperties::Material       TObjectPtr<UMaterialInterface>
//   UNiagaraRendererProperties (base, UCLASS(MinimalAPI) — Material is NOT on
//     the base class, so we find the "Material" UPROPERTY via reflection on
//     the concrete subclass.
// ===========================================================================

namespace
{
    // Resolve a renderer class from a string ("Sprite" / "Ribbon" / "Mesh" / "Light").
    UClass* ResolveRendererClass(const FString& InType)
    {
        const FString T = InType.ToLower();
        if (T == TEXT("sprite") || T == TEXT("spriterenderer"))
        {
            return UNiagaraSpriteRendererProperties::StaticClass();
        }
        if (T == TEXT("ribbon") || T == TEXT("ribbonrenderer"))
        {
            return UNiagaraRibbonRendererProperties::StaticClass();
        }
        if (T == TEXT("mesh") || T == TEXT("meshrenderer"))
        {
            return UNiagaraMeshRendererProperties::StaticClass();
        }
        if (T == TEXT("light") || T == TEXT("lightrenderer"))
        {
            return UNiagaraLightRendererProperties::StaticClass();
        }
        return nullptr;
    }

    // Read a "Material" UPROPERTY off any UNiagaraRendererProperties-derived
    // object via reflection (the field is on the concrete subclass, not the
    // base). Returns empty FString if the property is null or missing.
    FString ReadRendererMaterialPath(const UNiagaraRendererProperties* R)
    {
        if (!R) { return FString(); }
        FProperty* MatProp = R->GetClass()->FindPropertyByName(TEXT("Material"));
        if (!MatProp) { return FString(); }
        FObjectProperty* ObjProp = CastField<FObjectProperty>(MatProp);
        if (!ObjProp) { return FString(); }
        UObject* MatObj = ObjProp->GetObjectPropertyValue(
            ObjProp->ContainerPtrToValuePtr<void>(R));
        return MatObj ? MatObj->GetPathName() : FString();
    }

    // Write a UMaterialInterface to the "Material" UPROPERTY on a
    // UNiagaraRendererProperties via reflection.
    bool WriteRendererMaterial(UNiagaraRendererProperties* R, UMaterialInterface* Mat)
    {
        if (!R) { return false; }
        FProperty* MatProp = R->GetClass()->FindPropertyByName(TEXT("Material"));
        if (!MatProp) { return false; }
        FObjectProperty* ObjProp = CastField<FObjectProperty>(MatProp);
        if (!ObjProp) { return false; }
        ObjProp->SetObjectPropertyValue(
            ObjProp->ContainerPtrToValuePtr<void>(R), Mat);
        return true;
    }

    // --- Renderer-property reflection round-trip -----------------
    // Export one UPROPERTY (by name) on a renderer instance to a JSON value via
    // the property's own ExportText_Direct (the inverse of ImportText_Direct used
    // by the generic setter). Skips missing props and FNiagaraVariableAttributeBinding
    // struct fields (bindings are NOT part of this surface). Returns nullptr when
    // the property is absent on this concrete class.
    TSharedPtr<FJsonValue> ExportRendererPropValue(const UNiagaraRendererProperties* R, const TCHAR* PropName)
    {
        if (!R) { return nullptr; }
        FProperty* Prop = R->GetClass()->FindPropertyByName(PropName);
        if (!Prop) { return nullptr; }
        const void* Addr = Prop->ContainerPtrToValuePtr<void>(R);
        if (const FBoolProperty* B = CastField<FBoolProperty>(Prop))
        {
            return MakeShared<FJsonValueBoolean>(B->GetPropertyValue(Addr));
        }
        if (const FNumericProperty* N = CastField<FNumericProperty>(Prop))
        {
            // Enums are backed by an FNumericProperty whose GetEnum() is set; export
            // the human-readable name so it round-trips through ImportText cleanly.
            if (UEnum* EnumDef = N->GetIntPropertyEnum())
            {
                const int64 Raw = N->GetSignedIntPropertyValue(Addr);
                return MakeShared<FJsonValueString>(EnumDef->GetNameStringByValue(Raw));
            }
            if (N->IsFloatingPoint())
            {
                return MakeShared<FJsonValueNumber>(N->GetFloatingPointPropertyValue(Addr));
            }
            return MakeShared<FJsonValueNumber>(static_cast<double>(N->GetSignedIntPropertyValue(Addr)));
        }
        if (const FEnumProperty* E = CastField<FEnumProperty>(Prop))
        {
            const FNumericProperty* Underlying = E->GetUnderlyingProperty();
            const int64 Raw = Underlying->GetSignedIntPropertyValue(Addr);
            return MakeShared<FJsonValueString>(E->GetEnum()->GetNameStringByValue(Raw));
        }
        // FVector2D / FVector and the like: export as text (e.g. "X=1.000 Y=1.000").
        FString Text;
        Prop->ExportText_Direct(Text, Addr, Addr, const_cast<UNiagaraRendererProperties*>(R), PPF_None);
        return MakeShared<FJsonValueString>(Text);
    }

    // The set of authored UPROPERTYs we round-trip. Base props apply to every
    // renderer; Sprite/Mesh-only props are reported only when present on the class
    // (ExportRendererPropValue returns nullptr otherwise, so they're simply omitted).
    void DumpRendererProperties(const UNiagaraRendererProperties* R, TSharedPtr<FJsonObject>& Out)
    {
        static const TCHAR* const PropNames[] = {
            // base
            TEXT("bIsEnabled"), TEXT("SortOrderHint"), TEXT("MotionVectorSetting"), TEXT("bAllowInCullProxies"),
            // sprite
            TEXT("Alignment"), TEXT("FacingMode"), TEXT("SortMode"), TEXT("SubImageSize"),
            TEXT("MacroUVRadius"), TEXT("PivotInUVSpace"), TEXT("bCastShadows"), TEXT("RendererVisibility"),
            // mesh
            TEXT("bOverrideMaterials"), TEXT("MeshBoundsScale"), TEXT("LockedAxis"),
            // light (add_niagara_light_renderer exists, so the generic
            // get_niagara_renderer_info dump must pick up these fields too).
            TEXT("RadiusScale"), TEXT("DefaultExponent"), TEXT("SpecularScale"), TEXT("DiffuseScale"),
            TEXT("bUseInverseSquaredFalloff"), TEXT("bAffectsTranslucency"),
            TEXT("bAllowMegaLights"), TEXT("bMegaLightsCastShadows"), TEXT("bAlphaScalesBrightness"),
            // decal
            TEXT("SourceMode"), TEXT("DecalScreenSizeFade"),
        };
        for (const TCHAR* Name : PropNames)
        {
            TSharedPtr<FJsonValue> Val = ExportRendererPropValue(R, Name);
            if (Val.IsValid())
            {
                Out->SetField(Name, Val);
            }
        }
    }

    // Read the static-mesh path of slot Index on a UNiagaraMeshRendererProperties
    // (its Meshes[Index].Mesh), via reflection on the struct element. Returns empty
    // if the renderer is not a mesh renderer, the slot is out of range, or .Mesh is null.
    FString ReadMeshRendererSlotPath(const UNiagaraRendererProperties* R, int32 Index)
    {
        if (!R) { return FString(); }
        FArrayProperty* MeshesProp = CastField<FArrayProperty>(R->GetClass()->FindPropertyByName(TEXT("Meshes")));
        if (!MeshesProp) { return FString(); }
        FScriptArrayHelper Helper(MeshesProp, MeshesProp->ContainerPtrToValuePtr<void>(R));
        if (Index < 0 || Index >= Helper.Num()) { return FString(); }
        FStructProperty* ElemStruct = CastField<FStructProperty>(MeshesProp->Inner);
        if (!ElemStruct) { return FString(); }
        void* ElemPtr = Helper.GetRawPtr(Index);
        FObjectProperty* MeshProp = CastField<FObjectProperty>(ElemStruct->Struct->FindPropertyByName(TEXT("Mesh")));
        if (!MeshProp) { return FString(); }
        UObject* MeshObj = MeshProp->GetObjectPropertyValue(MeshProp->ContainerPtrToValuePtr<void>(ElemPtr));
        return MeshObj ? MeshObj->GetPathName() : FString();
    }

    // Niagara_ResolveEmitterData, then that emitter's renderer at RendererIndex (already >= 0); null with OutError set
    // on the first miss, or when the index is out of range or the renderer null.
    UNiagaraRendererProperties* Niagara_ResolveRenderer(const FString& SystemPath, const FString& EmitterName,
        int32 RendererIndex, UNiagaraSystem*& OutSystem, FString& OutError)
    {
        FVersionedNiagaraEmitterData* EmitterData =
            Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, OutSystem, OutError);
        if (!EmitterData)
        {
            return nullptr;
        }
        const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
        if (RendererIndex >= Renderers.Num())
        {
            OutError = FString::Printf(TEXT("renderer_index %d out of range (count=%d)"), RendererIndex, Renderers.Num());
            return nullptr;
        }
        UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
        if (!Renderer)
        {
            OutError = FString::Printf(TEXT("renderer_index %d is null"), RendererIndex);
        }
        return Renderer;
    }
}

// ---------------------------------------------------------------------------
// add_niagara_renderer
// Creates a UNiagaraSpriteRendererProperties / UNiagaraMeshRendererProperties /
// UNiagaraRibbonRendererProperties and appends it to the emitter via the
// public UNiagaraEmitter::AddRenderer (which honours the versioned-emitter
// bookkeeping under the hood). Material is set via reflection.
// ---------------------------------------------------------------------------
// MCP:add_niagara_renderer - Add a new renderer (Sprite / Ribbon / Mesh / Light) to a Niagara emitter.  Creates a UNiagaraRendererProperties instance of the requested concrete class (Sprite / Ribbon / Mesh / Light), optionally sets the Material UPROPERTY, and appends it to the emitter's RendererProperties array. Then requests a system compile and saves the asset.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Name of the emitter handle (the human-readable name in the system's emitter list).
//   @param renderer_type - One of "Sprite" / "Ribbon" / "Mesh" / "Light" (case-insensitive).
//   @param material_path - Optional asset path; sets the renderer's Material property. If omitted, the renderer is added with a null material (still a valid empty renderer).
//   @returns { system_path: string, emitter_name: string, renderer_type: string, renderer_class: string, renderer_index: int, material_path: string, renderer_count: int, save_error?: string }

TSharedPtr<FJsonObject> HandleAddNiagaraRenderer(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString RendererType;
    if (!Params->TryGetStringField(TEXT("renderer_type"), RendererType) || RendererType.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'renderer_type' parameter ('Sprite'|'Ribbon'|'Mesh')"));
    }
    FString MaterialPath;
    Params->TryGetStringField(TEXT("material_path"), MaterialPath);

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, System, Error, &Handle);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Handle->GetEmitterBase());
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(TEXT("Emitter handle has no UNiagaraEmitter asset (may be a system instance)"));
    }

    UClass* RendererClass = ResolveRendererClass(RendererType);
    if (!RendererClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown renderer_type: %s (expected 'Sprite'|'Ribbon'|'Mesh')"), *RendererType));
    }

    UNiagaraRendererProperties* NewRenderer =
        NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
    if (!NewRenderer)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to instantiate UNiagaraRendererProperties"));
    }

    if (!MaterialPath.IsEmpty())
    {
        UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (Mat)
        {
            WriteRendererMaterial(NewRenderer, Mat);
        }
    }

    const int32 IndexBefore = EmitterData->GetRenderers().Num();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraRenderer", "Add Niagara Renderer"));
    System->Modify();
    Emitter->AddRenderer(NewRenderer, EmitterData->Version.VersionGuid);
    System->RequestCompile(true);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("renderer_type"), RendererType);
    Data->SetStringField(TEXT("renderer_class"), NewRenderer->GetClass()->GetName());
    Data->SetNumberField(TEXT("renderer_index"), IndexBefore);
    Data->SetStringField(TEXT("material_path"), ReadRendererMaterialPath(NewRenderer));
    Data->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_niagara_light_renderer companion.
// ---------------------------------------------------------------------------
// MCP:add_niagara_light_renderer - Add a NiagaraLightRendererProperties (UNiagaraLightRendererProperties) to a Niagara emitter with light-specific defaults applied via reflection. Companion to add_niagara_renderer with renderer_type='Light' — use this cmd when you want to seed light config (radius scale, falloff mode, affects-translucency, color add) in the same call. Then requests a system compile and saves.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Name of the emitter handle.
//   @param use_inverse_squared_falloff - Optional bool (default true). True => UE physical light; false => use DefaultExponent below.
//   @param affects_translucency - Optional bool (default true). Set false for cheaper lights that shouldn't bleed through translucent surfaces.
//   @param allow_mega_lights - Optional bool (default false). True => MegaLights path (Lumen); see bMegaLightsCastShadows below.
//   @param mega_lights_cast_shadows - Optional bool (default false).
//   @param alpha_scales_brightness - Optional bool (default true). When true, particle color alpha modulates brightness.
//   @param radius_scale - Optional float (default 1.0, ClampMin=0).
//   @param default_exponent - Optional float (default 1.0, ClampMin=0); only used when use_inverse_squared_falloff=false.
//   @param specular_scale - Optional float (default 1.0, ClampMin=0).
//   @param diffuse_scale - Optional float (default 1.0, ClampMin=0).
//   @param color_add - Optional [r, g, b] static color shift.
//   @returns { system_path, emitter_name, renderer_index, renderer_class, radius_scale, default_exponent, specular_scale, diffuse_scale, color_add:{}, renderer_count, save_error? }
TSharedPtr<FJsonObject> HandleAddNiagaraLightRenderer(
    const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataTemplate, EmitterName, System, EmitterData, &Handle))
    {
        return Error;
    }

    // Read all 10 optional light-config fields. Defaults match the engine UPROPERTY defaults.
    bool bInverse = true;
    if (Params->HasField(TEXT("use_inverse_squared_falloff"))) { Params->TryGetBoolField(TEXT("use_inverse_squared_falloff"), bInverse); }
    bool bTranslucency = true;
    if (Params->HasField(TEXT("affects_translucency"))) { Params->TryGetBoolField(TEXT("affects_translucency"), bTranslucency); }
    bool bMega = false;
    if (Params->HasField(TEXT("allow_mega_lights"))) { Params->TryGetBoolField(TEXT("allow_mega_lights"), bMega); }
    bool bMegaShadows = false;
    if (Params->HasField(TEXT("mega_lights_cast_shadows"))) { Params->TryGetBoolField(TEXT("mega_lights_cast_shadows"), bMegaShadows); }
    bool bAlphaBrightness = true;
    if (Params->HasField(TEXT("alpha_scales_brightness"))) { Params->TryGetBoolField(TEXT("alpha_scales_brightness"), bAlphaBrightness); }
    double RadiusScale = 1.0;
    Params->TryGetNumberField(TEXT("radius_scale"), RadiusScale);
    double DefaultExponent = 1.0;
    Params->TryGetNumberField(TEXT("default_exponent"), DefaultExponent);
    double SpecularScale = 1.0;
    Params->TryGetNumberField(TEXT("specular_scale"), SpecularScale);
    double DiffuseScale = 1.0;
    Params->TryGetNumberField(TEXT("diffuse_scale"), DiffuseScale);
    FVector3f ColorAdd = FVector3f::ZeroVector;
    if (Params->HasField(TEXT("color_add")))
    {
        const FVector CA = mcp_common::ReadJsonVector(Params, TEXT("color_add"));
        ColorAdd = FVector3f(static_cast<float>(CA.X), static_cast<float>(CA.Y), static_cast<float>(CA.Z));
    }

    // Create the UNiagaraLightRendererProperties instance, configure it, add to emitter.
    UNiagaraLightRendererProperties* NewRenderer =
        NewObject<UNiagaraLightRendererProperties>(System, NAME_None, RF_Transactional);
    if (!NewRenderer)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create UNiagaraLightRendererProperties"));
    }

    const int32 IndexBefore = EmitterData->GetRenderers().Num();

    // Apply all 10 fields via the typed setters (avoids reflection on the bitfields).
    NewRenderer->bUseInverseSquaredFalloff      = bInverse       ? 1u : 0u;
    NewRenderer->bAffectsTranslucency           = bTranslucency  ? 1u : 0u;
    NewRenderer->bAllowMegaLights               = bMega          ? 1u : 0u;
    NewRenderer->bMegaLightsCastShadows         = bMegaShadows   ? 1u : 0u;
    NewRenderer->bAlphaScalesBrightness         = bAlphaBrightness ? 1u : 0u;
    NewRenderer->RadiusScale    = static_cast<float>(RadiusScale);
    NewRenderer->DefaultExponent = static_cast<float>(DefaultExponent);
    NewRenderer->SpecularScale  = static_cast<float>(SpecularScale);
    NewRenderer->DiffuseScale   = static_cast<float>(DiffuseScale);
    NewRenderer->ColorAdd       = ColorAdd;

    // Bookkeeping via the public UNiagaraEmitter::AddRenderer (versioned-emitter-aware).
    UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Handle->GetEmitterBase());
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Handle->GetEmitterBase() did not resolve to UNiagaraEmitter"));
    }
    Emitter->AddRenderer(NewRenderer, EmitterData->Version.VersionGuid);

    // Compile + save via the existing helper.
    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), IndexBefore);
    Data->SetStringField(TEXT("renderer_class"), NewRenderer->GetClass()->GetName());
    Data->SetNumberField(TEXT("radius_scale"), RadiusScale);
    Data->SetNumberField(TEXT("default_exponent"), DefaultExponent);
    Data->SetNumberField(TEXT("specular_scale"), SpecularScale);
    Data->SetNumberField(TEXT("diffuse_scale"), DiffuseScale);
    Data->SetNumberField(TEXT("color_add_x"), ColorAdd.X);
    Data->SetNumberField(TEXT("color_add_y"), ColorAdd.Y);
    Data->SetNumberField(TEXT("color_add_z"), ColorAdd.Z);
    Data->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// add_niagara_decal_renderer
// Companion to add_niagara_light_renderer for the Decal renderer class. Seeds
// the Decal-renderer specific UPROPERTYs (material, source mode, visibility,
// screen-size fade) in one call, mirroring the add_niagara_light_renderer shape.
// ---------------------------------------------------------------------------
// MCP:add_niagara_decal_renderer - Add a NiagaraDecalRendererProperties (UNiagaraDecalRendererProperties) to a Niagara emitter with decal-specific defaults applied via reflection. Companion to add_niagara_renderer with renderer_type='Decal' — use this cmd when you want to seed decal config (material, source mode, visibility tag, screen-size fade) in the same call. Then requests a system compile and saves.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Name of the emitter handle (the human-readable name in the system's emitter list).
//   @param material_path - Optional asset path; sets the renderer's Material UPROPERTY.
//   @param source_mode - Optional ENiagaraRendererSourceDataMode name (Particles/Emitter). Default Particles.
//   @param renderer_visibility - Optional int32 visibility tag (default 0).
//   @param decal_screen_size_fade - Optional float fade threshold (default 0.0).
//   @returns { system_path, emitter_name, renderer_index, renderer_class, material_path, source_mode, renderer_visibility, decal_screen_size_fade, renderer_count, save_error? }
TSharedPtr<FJsonObject> HandleAddNiagaraDecalRenderer(
    const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataTemplate, EmitterName, System, EmitterData, &Handle))
    {
        return Error;
    }
    FString MaterialPath;
    Params->TryGetStringField(TEXT("material_path"), MaterialPath);

    // source_mode defaults to "Particles" — the engine UPROPERTY default for UNiagaraDecalRendererProperties.
    FString SourceModeStr = TEXT("Particles");
    Params->TryGetStringField(TEXT("source_mode"), SourceModeStr);

    int32 RendererVisibility = 0;
    Params->TryGetNumberField(TEXT("renderer_visibility"), RendererVisibility);
    double DecalScreenSizeFadeD = 0.0;
    Params->TryGetNumberField(TEXT("decal_screen_size_fade"), DecalScreenSizeFadeD);

    UNiagaraDecalRendererProperties* NewRenderer =
        NewObject<UNiagaraDecalRendererProperties>(System, NAME_None, RF_Transactional);
    if (!NewRenderer)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create UNiagaraDecalRendererProperties"));
    }

    // Resolve optional material via the asset registry (resolves /Game/... paths headlessly).
    if (!MaterialPath.IsEmpty())
    {
        UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (!Mat)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Material asset could not be loaded as UMaterialInterface: %s"), *MaterialPath));
        }
        NewRenderer->Material = Mat;
    }

    // Resolve the enum value from string. Default "Particles" is the engine UPROPERTY default,
    // so passing it explicitly is a no-op semantically (but we still emit the right value to
    // ensure round-trip symmetry with get_niagara_renderer_info dumps).
    const UEnum* SrcEnum = StaticEnum<ENiagaraRendererSourceDataMode>();
    int64 SrcEnumValue = SrcEnum ? SrcEnum->GetValueByNameString(SourceModeStr) : INDEX_NONE;
    if (SrcEnumValue == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid source_mode '%s' (expected one of the ENiagaraRendererSourceDataMode names like Particles / Emitter)"), *SourceModeStr));
    }
    NewRenderer->SourceMode = static_cast<ENiagaraRendererSourceDataMode>(SrcEnumValue);

    NewRenderer->RendererVisibility = RendererVisibility;
    NewRenderer->DecalScreenSizeFade = static_cast<float>(DecalScreenSizeFadeD);

    const int32 IndexBefore = EmitterData->GetRenderers().Num();

    UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Handle->GetEmitterBase());
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Handle->GetEmitterBase() did not resolve to UNiagaraEmitter"));
    }
    Emitter->AddRenderer(NewRenderer, EmitterData->Version.VersionGuid);

    // SourceMode is one of the four enum-driven props that require PostEditChangeProperty to
    // propagate (drives UpdateSourceModeDerivates). Skip if the user passed the default value
    // (no real change), but fire it when they explicitly overrode.
    if (!SourceModeStr.Equals(TEXT("Particles"), ESearchCase::IgnoreCase))
    {
        FPropertyChangedEvent ChangeEvent(
            UNiagaraDecalRendererProperties::StaticClass()->FindPropertyByName(TEXT("SourceMode")),
            EPropertyChangeType::ValueSet);
        NewRenderer->PostEditChangeProperty(ChangeEvent);
    }

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), IndexBefore);
    Data->SetStringField(TEXT("renderer_class"), NewRenderer->GetClass()->GetName());
    Data->SetStringField(TEXT("material_path"), MaterialPath.IsEmpty() ? FString() : MaterialPath);
    Data->SetStringField(TEXT("source_mode"), SrcEnum ? SrcEnum->GetNameStringByValue(SrcEnumValue) : SourceModeStr);
    Data->SetNumberField(TEXT("renderer_visibility"), NewRenderer->RendererVisibility);
    Data->SetNumberField(TEXT("decal_screen_size_fade"), NewRenderer->DecalScreenSizeFade);
    Data->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// remove_niagara_renderer
// Removes the renderer at Index via the public UNiagaraEmitter::RemoveRenderer.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_renderer - Remove a renderer from a Niagara emitter by index.  Validates that renderer_index is in range, removes from the emitter's RendererProperties array, requests a system compile and saves the asset.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index into the emitter's renderers list (use get_niagara_emitter_renderers to find the index)
//   @returns { system_path: string, emitter_name: string, removed_renderer_index: int, removed_renderer_class: string, renderer_count_after: int, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveNiagaraRenderer(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 RendererIndex = -1;
    if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || RendererIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid 'renderer_index' parameter (must be >= 0)"));
    }

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, System, Error, &Handle);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Handle->GetEmitterBase());
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(TEXT("Emitter handle has no UNiagaraEmitter asset (may be a system instance)"));
    }

    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
    if (RendererIndex >= Renderers.Num())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("renderer_index %d out of range (count=%d)"), RendererIndex, Renderers.Num()));
    }
    UNiagaraRendererProperties* Target = Renderers[RendererIndex];
    FString RemovedClassName = Target ? Target->GetClass()->GetName() : FString();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraRenderer", "Remove Niagara Renderer"));
    System->Modify();
    Emitter->RemoveRenderer(Target, EmitterData->Version.VersionGuid);
    System->RequestCompile(true);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("removed_renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("removed_renderer_class"), RemovedClassName);
    Data->SetNumberField(TEXT("renderer_count_after"), EmitterData->GetRenderers().Num());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_renderers
// Read-only enumeration of the emitter's renderers (via the const GetRenderers).
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_renderers - List all renderers on a Niagara emitter (read-only).  Each entry also dumps the authored renderer UPROPERTYs (base bIsEnabled/SortOrderHint/MotionVectorSetting/bAllowInCullProxies plus the Sprite/Mesh-specific fields present on the concrete class) and, for Mesh renderers, the assigned static-mesh slots — so set_niagara_renderer_properties / set_niagara_mesh_renderer_mesh round-trip.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @returns { system_path: string, emitter_name: string, renderer_count: int, renderers: [{ index, class, display_name, material_path, properties:{...}, meshes?:[{ slot, mesh_path }] }] }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterRenderers(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataTemplate, EmitterName, System, EmitterData))
    {
        return Error;
    }

    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
    TArray<TSharedPtr<FJsonValue>> RendererArray;
    for (int32 i = 0; i < Renderers.Num(); ++i)
    {
        const UNiagaraRendererProperties* R = Renderers[i];
        TSharedPtr<FJsonObject> RObj = MakeShared<FJsonObject>();
        RObj->SetNumberField(TEXT("index"), i);
        RObj->SetStringField(TEXT("class"), R ? R->GetClass()->GetName() : TEXT("null"));
        RObj->SetStringField(TEXT("display_name"), R ? R->GetName() : TEXT("null"));
        RObj->SetStringField(TEXT("material_path"), ReadRendererMaterialPath(R));

        // Round-trip: dump the authored renderer UPROPERTYs.
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        DumpRendererProperties(R, PropsObj);
        RObj->SetObjectField(TEXT("properties"), PropsObj);

        // For mesh renderers, dump the assigned static-mesh slots (Meshes[].Mesh).
        if (R && CastField<FArrayProperty>(R->GetClass()->FindPropertyByName(TEXT("Meshes"))))
        {
            FArrayProperty* MeshesProp = CastField<FArrayProperty>(R->GetClass()->FindPropertyByName(TEXT("Meshes")));
            FScriptArrayHelper Helper(MeshesProp, MeshesProp->ContainerPtrToValuePtr<void>(R));
            TArray<TSharedPtr<FJsonValue>> MeshArr;
            for (int32 s = 0; s < Helper.Num(); ++s)
            {
                TSharedPtr<FJsonObject> MObj = MakeShared<FJsonObject>();
                MObj->SetNumberField(TEXT("slot"), s);
                MObj->SetStringField(TEXT("mesh_path"), ReadMeshRendererSlotPath(R, s));
                MeshArr.Add(MakeShared<FJsonValueObject>(MObj));
            }
            RObj->SetArrayField(TEXT("meshes"), MeshArr);
        }
        RendererArray.Add(MakeShared<FJsonValueObject>(RObj));
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_count"), Renderers.Num());
    Data->SetArrayField(TEXT("renderers"), RendererArray);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_niagara_simulation_stages / get_niagara_simulation_stages
// Read-only enumeration of an emitter's simulation stages via
// the const FVersionedNiagaraEmitterData::GetSimulationStages() (NiagaraEmitter.h,
// inline; returns const TArray<UNiagaraSimulationStageBase*>&). Same
// resolution chain as HandleGetNiagaraEmitterRenderers, honest-empty arrays.
//
// list vs get (renderers precedent — the renderers pair exposes only ONE reader
// shape, the whole array): both readers build the SAME per-stage array; the only
// difference is get_niagara_simulation_stages accepts an OPTIONAL stage_index /
// stage_name filter and returns just the matching stage(s). No stage identifier
// => get behaves identically to list (full array).
// ---------------------------------------------------------------------------
namespace
{
    // Shared per-stage JSON builder for the simulation-stage readers. When
    // bHasFilter is true, only the stage whose 0-based position == FilterIndex
    // (if FilterIndex >= 0) or whose SimulationStageName == FilterName (if set)
    // is emitted; otherwise every stage is emitted. Fields exactly mirror the
    // fields the add/set-simulation-stage handlers author.
    TArray<TSharedPtr<FJsonValue>> BuildNiagaraSimulationStageArray(
        const TArray<UNiagaraSimulationStageBase*>& Stages,
        bool bHasFilter, int32 FilterIndex, const FString& FilterName)
    {
        TArray<TSharedPtr<FJsonValue>> StageArray;
        for (int32 i = 0; i < Stages.Num(); ++i)
        {
            const UNiagaraSimulationStageBase* Stage = Stages[i];
            if (bHasFilter)
            {
                const bool bIndexMatch = (FilterIndex >= 0) && (i == FilterIndex);
                const bool bNameMatch = !FilterName.IsEmpty() && Stage &&
                    Stage->SimulationStageName.ToString().Equals(FilterName, ESearchCase::IgnoreCase);
                if (!bIndexMatch && !bNameMatch)
                {
                    continue;
                }
            }
            TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
            SObj->SetNumberField(TEXT("index"), i);
            SObj->SetStringField(TEXT("stage_name"),
                Stage ? Stage->SimulationStageName.ToString() : TEXT("null"));
            SObj->SetStringField(TEXT("class"),
                Stage ? Stage->GetClass()->GetName() : TEXT("null"));
            SObj->SetBoolField(TEXT("enabled"), Stage ? (bool)Stage->bEnabled : false);
            // IterationSource lives on UNiagaraSimulationStageGeneric only
            // (NiagaraSimulationStageBase.h) — guard the cast, never faked on
            // the base class.
            if (const UNiagaraSimulationStageGeneric* Generic = Cast<UNiagaraSimulationStageGeneric>(Stage))
            {
                const UEnum* IterEnum = StaticEnum<ENiagaraIterationSource>();
                SObj->SetStringField(TEXT("iteration_source"),
                    IterEnum ? IterEnum->GetNameStringByValue((int64)Generic->IterationSource)
                             : FString::FromInt((int32)Generic->IterationSource));
            }
            StageArray.Add(MakeShared<FJsonValueObject>(SObj));
        }
        return StageArray;
    }
}

// MCP:list_niagara_simulation_stages - List all simulation stages on a Niagara emitter (read-only). Resolves the system by path and the emitter by name, then enumerates FVersionedNiagaraEmitterData::GetSimulationStages(). Each entry reports index, stage_name (SimulationStageName), class, enabled, and (for UNiagaraSimulationStageGeneric stages) iteration_source. Honest-empty: returns simulation_stages:[] / simulation_stage_count:0 when the emitter has no stages.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @returns { system_path: string, emitter_name: string, simulation_stage_count: int, simulation_stages: [{ index, stage_name, class, enabled, iteration_source? }] }

TSharedPtr<FJsonObject> HandleListNiagaraSimulationStages(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("list_niagara_simulation_stages: enter"));
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    UE_LOG(LogTemp, Verbose, TEXT("list_niagara_simulation_stages: %s/%s has %d stage(s)"),
        *SystemPath, *EmitterName, Stages.Num());
    TArray<TSharedPtr<FJsonValue>> StageArray =
        BuildNiagaraSimulationStageArray(Stages, /*bHasFilter*/ false, INDEX_NONE, FString());

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("simulation_stage_count"), Stages.Num());
    Data->SetArrayField(TEXT("simulation_stages"), StageArray);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_niagara_simulation_stages - Read the simulation stages of a Niagara emitter (read-only). Same enumeration as list_niagara_simulation_stages, but accepts an OPTIONAL stage_index (preferred) or stage_name to return just the matching stage; with no identifier it returns the full array. Each entry reports index, stage_name, class, enabled, and (for UNiagaraSimulationStageGeneric) iteration_source. Honest-empty when the emitter has no stages or the filter matches nothing.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param stage_index - Optional 0-based index of a single stage to return.
//   @param stage_name - Optional SimulationStageName to match if stage_index is not supplied.
//   @returns { system_path: string, emitter_name: string, simulation_stage_count: int, total_simulation_stages: int, simulation_stages: [{ index, stage_name, class, enabled, iteration_source? }] }

TSharedPtr<FJsonObject> HandleGetNiagaraSimulationStages(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("get_niagara_simulation_stages: enter"));
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Optional single-item filter: stage_index (preferred) or stage_name. No
    // identifier => full array (identical to list_niagara_simulation_stages).
    int32 FilterIndex = INDEX_NONE;
    bool bHasIndex = Params->TryGetNumberField(TEXT("stage_index"), FilterIndex);
    FString FilterName;
    bool bHasName = Params->TryGetStringField(TEXT("stage_name"), FilterName) && !FilterName.IsEmpty();
    const bool bHasFilter = bHasIndex || bHasName;

    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    UE_LOG(LogTemp, Verbose, TEXT("get_niagara_simulation_stages: %s/%s has %d stage(s); filter=%s (index=%d name='%s')"),
        *SystemPath, *EmitterName, Stages.Num(),
        bHasFilter ? TEXT("yes") : TEXT("no"), bHasIndex ? FilterIndex : -1, *FilterName);
    TArray<TSharedPtr<FJsonValue>> StageArray = BuildNiagaraSimulationStageArray(
        Stages, bHasFilter, bHasIndex ? FilterIndex : INDEX_NONE, bHasName ? FilterName : FString());

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("simulation_stage_count"), StageArray.Num());
    Data->SetNumberField(TEXT("total_simulation_stages"), Stages.Num());
    Data->SetArrayField(TEXT("simulation_stages"), StageArray);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_renderer_material
// Sets the Material UPROPERTY on a renderer (Sprite / Ribbon / Mesh all expose
// a TObjectPtr<UMaterialInterface> Material; we look it up via reflection).
// ---------------------------------------------------------------------------
// MCP:set_niagara_renderer_material - Set the Material UPROPERTY on a Niagara renderer (Sprite/Ribbon/Mesh).  Loads the material, sets it on the renderer's public Material field, requests a system compile and saves. Reads back to verify the material path is now bound.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index of the renderer (use get_niagara_emitter_renderers to find the index)
//   @param material_path - Asset path of the UMaterialInterface to assign.
//   @returns { system_path: string, emitter_name: string, renderer_index: int, renderer_class: string, material_path: string, material_path_read_back: string, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 RendererIndex = -1;
    if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || RendererIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid 'renderer_index' parameter"));
    }
    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }

    UNiagaraSystem* System = nullptr;
    UNiagaraRendererProperties* R = Niagara_ResolveRenderer(SystemPath, EmitterName, RendererIndex, System, Error);
    if (!R)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (R->GetClass()->FindPropertyByName(TEXT("Material")) == nullptr)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Renderer class '%s' has no Material UPROPERTY"), *R->GetClass()->GetName()));
    }

    UMaterialInterface* Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Mat)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Material not found: %s"), *MaterialPath));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraRendererMaterial", "Set Niagara Renderer Material"));
    System->Modify();
    R->Modify();
    WriteRendererMaterial(R, Mat);
    System->RequestCompile(true);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    const FString ReadBackPath = ReadRendererMaterialPath(R);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
    Data->SetStringField(TEXT("material_path"), ReadBackPath);
    Data->SetBoolField(TEXT("verified"), ReadBackPath == MaterialPath);
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Renderer-property authoring.
//
// add_niagara_renderer + set_niagara_renderer_material leave EVERY
// renderer-specific property unauthored. These two writers author them:
//   set_niagara_renderer_properties — generic, by reflection, covers base +
//     Sprite/Mesh subclass UPROPERTYs uniformly (no C++ method is called on the
//     renderer class; we set UPROPERTYs by name via MergeJsonWithClamp on the LIVE
//     concrete renderer from GetRenderers() — the exact mechanism WriteRendererMaterial
//     already relies on, so MinimalAPI / WITH_EDITORONLY_DATA are irrelevant).
//   set_niagara_mesh_renderer_mesh — assigns the static mesh into a Meshes[] slot
//     (a STRUCT array, so the generic merge can't express it without replacing it).
//
// Both mirror set_niagara_renderer_material's save/compile exactly:
//   System->Modify() + R->Modify() + write + System->RequestCompile(true) +
//   SaveModifiedNiagaraSystem. Enum writes additionally call PostEditChangeProperty
//   (Alignment/FacingMode drive UpdateSourceModeDerivates).
// ===========================================================================
namespace
{
    // Map of accepted snake_case input keys -> exact UPROPERTY name on the renderer.
    // Keys whose property is absent on the concrete renderer class are reported in
    // "not_applicable" (honest: a Mesh-only key on a Sprite is not silently swallowed).
    // FNiagaraVariableAttributeBinding struct fields (PositionBinding/ColorBinding/...)
    // are DELIBERATELY not in this table — bindings are not part of this surface.
    static const FNiagaraPropKey GRendererPropKeys[] = {
        // base UNiagaraRendererProperties
        { TEXT("enabled"),               TEXT("bIsEnabled") },
        { TEXT("renderer_enabled"),      TEXT("bIsEnabled") },
        { TEXT("sort_order_hint"),       TEXT("SortOrderHint") },
        { TEXT("motion_vector_setting"), TEXT("MotionVectorSetting") },
        { TEXT("allow_in_cull_proxies"), TEXT("bAllowInCullProxies") },
        // sprite
        { TEXT("alignment"),             TEXT("Alignment") },
        { TEXT("sort_mode"),             TEXT("SortMode") },
        { TEXT("sub_image_size"),        TEXT("SubImageSize") },
        { TEXT("macro_uv_radius"),       TEXT("MacroUVRadius") },
        { TEXT("pivot_in_uv_space"),     TEXT("PivotInUVSpace") },
        { TEXT("cast_shadows"),          TEXT("bCastShadows") },
        { TEXT("renderer_visibility"),   TEXT("RendererVisibility") },
        // mesh (facing_mode is shared with sprite; resolves to whichever FacingMode
        // exists on the concrete class — both are named "FacingMode")
        { TEXT("facing_mode"),           TEXT("FacingMode") },
        { TEXT("override_materials"),    TEXT("bOverrideMaterials") },
        { TEXT("mesh_bounds_scale"),     TEXT("MeshBoundsScale") },
        { TEXT("locked_axis"),           TEXT("LockedAxis") },
        // light — UNiagaraLightRendererProperties (its
        // 12 scalar fields). Bitfields (uint32:1) and
        // FVector3f (ColorAdd, from {x,y,z}) are handled by the JSON converter.
        { TEXT("use_inverse_squared_falloff"), TEXT("bUseInverseSquaredFalloff") },
        { TEXT("affects_translucency"),        TEXT("bAffectsTranslucency") },
        { TEXT("allow_mega_lights"),           TEXT("bAllowMegaLights") },
        { TEXT("mega_lights_cast_shadows"),    TEXT("bMegaLightsCastShadows") },
        { TEXT("alpha_scales_brightness"),     TEXT("bAlphaScalesBrightness") },
        { TEXT("override_inverse_exposure_blend"), TEXT("bOverrideInverseExposureBlend") },
        { TEXT("radius_scale"),                TEXT("RadiusScale") },
        { TEXT("default_exponent"),            TEXT("DefaultExponent") },
        { TEXT("specular_scale"),              TEXT("SpecularScale") },
        { TEXT("diffuse_scale"),               TEXT("DiffuseScale") },
        { TEXT("color_add"),                   TEXT("ColorAdd") },
        { TEXT("inverse_exposure_blend"),      TEXT("InverseExposureBlend") },
        // decal — UNiagaraDecalRendererProperties (its
        // 3 scalar fields; renderer_visibility is already
        // a base key above). SourceMode is an enum -> RendererPropNeedsPostEdit fires the
        // decal's UpdateSourceModeDerivates via PostEditChangeProperty.
        { TEXT("decal_screen_size_fade"),      TEXT("DecalScreenSizeFade") },
        { TEXT("decal_source_mode"),           TEXT("SourceMode") },
    };

    // Enum-typed renderer props whose write drives derived state (e.g. the decal
    // renderer's UpdateSourceModeDerivates on SourceMode) and therefore require a
    // PostEditChangeProperty to take effect (material setter skips it).
    bool RendererPropNeedsPostEdit(const TCHAR* PropName)
    {
        return FCString::Strcmp(PropName, TEXT("Alignment")) == 0
            || FCString::Strcmp(PropName, TEXT("FacingMode")) == 0
            || FCString::Strcmp(PropName, TEXT("SortMode")) == 0
            || FCString::Strcmp(PropName, TEXT("MotionVectorSetting")) == 0
            || FCString::Strcmp(PropName, TEXT("SourceMode")) == 0;
    }
}

// ---------------------------------------------------------------------------
// set_niagara_renderer_properties
// ---------------------------------------------------------------------------
// MCP:set_niagara_renderer_properties - Author renderer UPROPERTYs on a Niagara renderer (Sprite/Mesh/Ribbon/Light/Decal) generically by reflection.  Writes only the supplied fields onto the LIVE concrete renderer via MergeJsonWithClamp (FJsonObjectConverter + ClampMin/Max meta), then requests a system compile and saves. Enum writes (alignment/facing_mode/sort_mode/motion_vector_setting/decal_source_mode) also fire PostEditChangeProperty so derived state updates. Light/Decal scalar fields (radius_scale, affects_translucency, color_add, decal_screen_size_fade, decal_source_mode, ...) are covered here — use set_niagara_decal_renderer_material for the decal Material (object-ref). Keys not present on the concrete renderer class are reported in not_applicable (honest, not silently swallowed). Idempotent. Covers base renderer_enabled for free.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index of the renderer (use get_niagara_emitter_renderers).
//   @param enabled - Optional bool: base bIsEnabled (also accepted as renderer_enabled).
//   @param sort_order_hint - Optional int: base SortOrderHint.
//   @param motion_vector_setting - Optional enum name: base MotionVectorSetting (e.g. "AutoDetect"/"Precise"/"Disable").
//   @param allow_in_cull_proxies - Optional bool: base bAllowInCullProxies.
//   @param alignment - Optional enum name (Sprite): Alignment (e.g. "Unaligned"/"VelocityAligned"/"CustomAlignment"/"Automatic").
//   @param facing_mode - Optional enum name (Sprite ENiagaraSpriteFacingMode or Mesh ENiagaraMeshFacingMode, resolved by class): FacingMode.
//   @param sort_mode - Optional enum name (Sprite/Mesh): SortMode (e.g. "None"/"ViewDepth"/"ViewDistance"/"CustomAscending"/"CustomDescending").
//   @param sub_image_size - Optional [x,y] or {X,Y} (Sprite/Mesh): SubImageSize.
//   @param macro_uv_radius - Optional float (Sprite): MacroUVRadius.
//   @param pivot_in_uv_space - Optional [x,y] or {X,Y} (Sprite): PivotInUVSpace.
//   @param cast_shadows - Optional bool (Sprite): bCastShadows.
//   @param renderer_visibility - Optional int (Sprite): RendererVisibility tag value.
//   @param override_materials - Optional bool (Mesh): bOverrideMaterials.
//   @param mesh_bounds_scale - Optional [x,y,z] or {X,Y,Z} (Mesh): MeshBoundsScale.
//   @param locked_axis - Optional [x,y,z] or {X,Y,Z} (Mesh): LockedAxis.
//   @param use_inverse_squared_falloff - Optional bool (Light): bUseInverseSquaredFalloff.
//   @param affects_translucency - Optional bool (Light): bAffectsTranslucency.
//   @param allow_mega_lights - Optional bool (Light): bAllowMegaLights.
//   @param mega_lights_cast_shadows - Optional bool (Light): bMegaLightsCastShadows.
//   @param alpha_scales_brightness - Optional bool (Light): bAlphaScalesBrightness.
//   @param override_inverse_exposure_blend - Optional bool (Light): bOverrideInverseExposureBlend.
//   @param radius_scale - Optional float (Light, ClampMin=0): RadiusScale.
//   @param default_exponent - Optional float (Light, ClampMin=0): DefaultExponent (used when use_inverse_squared_falloff=false).
//   @param specular_scale - Optional float (Light, ClampMin=0): SpecularScale.
//   @param diffuse_scale - Optional float (Light, ClampMin=0): DiffuseScale.
//   @param color_add - Optional {x,y,z} or [x,y,z] (Light): ColorAdd (FVector3f RGB shift).
//   @param inverse_exposure_blend - Optional float (Light): InverseExposureBlend.
//   @param decal_screen_size_fade - Optional float (Decal): DecalScreenSizeFade.
//   @param decal_source_mode - Optional enum name/int (Decal): SourceMode ("Particles"/"Emitter"); fires PostEditChangeProperty so UpdateSourceModeDerivates runs.
//   @returns { system_path, emitter_name, renderer_index, renderer_class, applied:[prop names], not_applicable:[input keys], post_edit_fired: bool, properties:{round-trip dump}, save_error? }
TSharedPtr<FJsonObject> HandleSetNiagaraRendererProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 RendererIndex = -1;
    if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || RendererIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid 'renderer_index' parameter"));
    }

    UNiagaraSystem* System = nullptr;
    UNiagaraRendererProperties* R = Niagara_ResolveRenderer(SystemPath, EmitterName, RendererIndex, System, Error);
    if (!R)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Build a merge-JSON keyed by the EXACT UPROPERTY name, copying the value
    // verbatim from each present input key. Only props that exist on THIS concrete
    // class are merged; the rest are reported in not_applicable. Enum values are
    // passed as their name string (FJsonObjectConverter imports them via ImportText).
    TSharedPtr<FJsonObject> MergeJson = MakeShared<FJsonObject>();
    TArray<FString> AppliedProps;
    TArray<TSharedPtr<FJsonValue>> NotApplicable;
    FProperty* EnumPropForPostEdit = Niagara_CollectSuppliedProps(Params, GRendererPropKeys, R->GetClass(),
        &RendererPropNeedsPostEdit, MergeJson, AppliedProps, NotApplicable);
    const bool bNeedsPostEdit = (EnumPropForPostEdit != nullptr);

    if (AppliedProps.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No applicable renderer properties supplied for class '%s' (check key names / renderer type)"),
                *R->GetClass()->GetName()));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraRendererProperties", "Set Niagara Renderer Properties"));
    System->Modify();
    R->Modify();

    FString MergeError;
    if (!mcp_common::MergeJsonWithClamp(MergeJson, R->GetClass(), R, MergeError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to apply renderer properties: %s"), *MergeError));
    }

    // Enum writes (Alignment/FacingMode/SortMode/MotionVectorSetting) need the
    // renderer notified so any derived state recomputes; the material setter skips
    // PostEditChangeProperty but these need it. Fire one edit with the changed enum
    // property (a valid FProperty*, matching how the editor UI would notify).
    if (bNeedsPostEdit)
    {
        FPropertyChangedEvent ChangeEvent(EnumPropForPostEdit, EPropertyChangeType::ValueSet);
        R->PostEditChangeProperty(ChangeEvent);
    }

    System->RequestCompile(true);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    // Round-trip dump so the caller sees the authoritative post-set state.
    TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
    DumpRendererProperties(R, PropsObj);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
    Data->SetArrayField(TEXT("applied"), mcp_common::StringsToJson(AppliedProps));
    Data->SetArrayField(TEXT("not_applicable"), NotApplicable);
    Data->SetBoolField(TEXT("post_edit_fired"), bNeedsPostEdit);
    Data->SetObjectField(TEXT("properties"), PropsObj);
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_mesh_renderer_mesh
// Assigns a UStaticMesh into a UNiagaraMeshRendererProperties::Meshes[] slot. The
// array element is a STRUCT (FNiagaraMeshRendererMeshProperties), so we resolve the
// slot (growing the array to it if needed) and set its inner .Mesh via reflection on
// the struct — the generic merge can't express this without replacing the whole array.
// Idempotency: if Meshes[slot].Mesh already equals the requested mesh, no write/grow.
// ---------------------------------------------------------------------------
// MCP:set_niagara_mesh_renderer_mesh - Assign a static mesh into a Niagara Mesh renderer's Meshes[] slot.  Resolves (or grows the array to) the slot index, loads the UStaticMesh, and sets the slot's inner .Mesh via reflection on the FNiagaraMeshRendererMeshProperties element. Then requests a system compile and saves. add_niagara_renderer type=Mesh leaves Meshes empty (renders nothing); this is the command that makes a Mesh renderer actually draw. Idempotent: re-assigning the same mesh to the same slot does not grow the array.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index of the Mesh renderer (use get_niagara_emitter_renderers).
//   @param mesh_path - Asset path of the UStaticMesh to assign (e.g. "/Engine/BasicShapes/Cube").
//   @param slot - Optional 0-based slot index into Meshes[] (default 0). The array is grown with default elements up to this index if needed.
//   @returns { system_path, emitter_name, renderer_index, renderer_class, slot, mesh_path, mesh_count, already_assigned: bool, save_error? }
TSharedPtr<FJsonObject> HandleSetNiagaraMeshRendererMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 RendererIndex = -1;
    if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || RendererIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid 'renderer_index' parameter"));
    }
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mesh_path"), MeshPath)) { return Missing; }
    int32 Slot = 0;
    if (Params->HasField(TEXT("slot")))
    {
        if (!Params->TryGetNumberField(TEXT("slot"), Slot) || Slot < 0)
        {
            return mcp_common::MakeErrorReply(TEXT("Invalid 'slot' parameter (must be >= 0)"));
        }
    }

    UNiagaraSystem* System = nullptr;
    UNiagaraRendererProperties* R = Niagara_ResolveRenderer(SystemPath, EmitterName, RendererIndex, System, Error);
    if (!R)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Resolve the Meshes array (mesh-renderer only) + its element struct + the inner
    // .Mesh FObjectProperty, all by reflection (no class method calls).
    FArrayProperty* MeshesProp = CastField<FArrayProperty>(R->GetClass()->FindPropertyByName(TEXT("Meshes")));
    if (!MeshesProp)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Renderer class '%s' has no Meshes array (not a Mesh renderer)"), *R->GetClass()->GetName()));
    }
    FStructProperty* ElemStruct = CastField<FStructProperty>(MeshesProp->Inner);
    if (!ElemStruct)
    {
        return mcp_common::MakeErrorReply(TEXT("Meshes array element is not a struct (engine layout changed?)"));
    }
    FObjectProperty* MeshInnerProp = CastField<FObjectProperty>(ElemStruct->Struct->FindPropertyByName(TEXT("Mesh")));
    if (!MeshInnerProp)
    {
        return mcp_common::MakeErrorReply(TEXT("FNiagaraMeshRendererMeshProperties has no Mesh property (engine layout changed?)"));
    }

    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
    if (!Mesh)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Static mesh not found: %s"), *MeshPath));
    }

    // Idempotency: if the slot already exists AND already holds this mesh, do nothing.
    {
        FScriptArrayHelper PeekHelper(MeshesProp, MeshesProp->ContainerPtrToValuePtr<void>(R));
        if (Slot < PeekHelper.Num())
        {
            void* ElemPtr = PeekHelper.GetRawPtr(Slot);
            UObject* Cur = MeshInnerProp->GetObjectPropertyValue(MeshInnerProp->ContainerPtrToValuePtr<void>(ElemPtr));
            if (Cur == Mesh)
            {
                TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
                Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
                Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
                Data->SetNumberField(TEXT("slot"), Slot);
                Data->SetStringField(TEXT("mesh_path"), Mesh->GetPathName());
                Data->SetNumberField(TEXT("mesh_count"), PeekHelper.Num());
                Data->SetBoolField(TEXT("already_assigned"), true);
                return mcp_common::MakeOkReply(Data);
            }
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraMeshRendererMesh", "Set Niagara Mesh Renderer Mesh"));
    System->Modify();
    R->Modify();

    // Grow the array to the requested slot (default-constructed elements), then set
    // the inner .Mesh. ExpandForIndex grows + default-initialises in one call.
    FScriptArrayHelper Helper(MeshesProp, MeshesProp->ContainerPtrToValuePtr<void>(R));
    Helper.ExpandForIndex(Slot);
    void* ElemPtr = Helper.GetRawPtr(Slot);
    MeshInnerProp->SetObjectPropertyValue(MeshInnerProp->ContainerPtrToValuePtr<void>(ElemPtr), Mesh);

    // Mesh array assignment drives the renderer's derived mesh state; fire one edit.
    {
        FPropertyChangedEvent ChangeEvent(MeshesProp, EPropertyChangeType::ValueSet);
        R->PostEditChangeProperty(ChangeEvent);
    }

    System->RequestCompile(true);

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    const FString ReadBack = ReadMeshRendererSlotPath(R, Slot);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
    Data->SetNumberField(TEXT("slot"), Slot);
    Data->SetStringField(TEXT("mesh_path"), ReadBack);
    Data->SetNumberField(TEXT("mesh_count"), Helper.Num());
    Data->SetBoolField(TEXT("already_assigned"), false);
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_data_interfaces
// Read-only enumeration of FNiagaraDataInterfaceEmitterBinding entries on the
// emitter. The struct has only two fields (BindingMode + EmitterName) — see
// NiagaraDataInterfaceEmitterBinding.h. We list those; there is no
// public add path (view-model required), so we do NOT stub one.
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_data_interfaces - List the data interface bindings on a Niagara emitter (read-only).  Reads the FVersionedNiagaraEmitterData::EmitterDependencies array (TArray<FNiagaraDataInterfaceEmitterBinding>). Each binding has a name, type, interface_class, interface_path, and instance_index.  Adding or removing data interface bindings on a script requires view-models (FNiagaraStackGraphUtilities AddDataInterface) and is editor-only -- this command lists them but does not mutate them.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @returns { system_path: string, emitter_name: string, data_interface_count: int, data_interfaces: [{ emitter_name, binding_mode }] }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterDataInterfaces(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataTemplate, EmitterName, System, EmitterData))
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> DIs;
    for (const FNiagaraDataInterfaceEmitterBinding& B : EmitterData->EmitterDependencies)
    {
        TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();
        DObj->SetStringField(TEXT("emitter_name"), B.EmitterName.ToString());
        DObj->SetStringField(TEXT("binding_mode"),
            B.BindingMode == ENiagaraDataInterfaceEmitterBindingMode::Self ? TEXT("Self") : TEXT("Other"));
        DIs.Add(MakeShared<FJsonValueObject>(DObj));
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("data_interface_count"), EmitterData->EmitterDependencies.Num());
    Data->SetArrayField(TEXT("data_interfaces"), DIs);
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Niagara User-Parameter Data Interfaces (data-only path).
//
// These commands manage UNiagaraDataInterface instances on the system's
// ExposedParameters store (the user-param map exposed to runtime component
// instances). All are headless-safe: the parameter store is a UPROPERTY array
// on UNiagaraSystem and the AddParameter API auto-creates a DI instance via
// NewObject (NiagaraParameterStore.cpp).
//
// We DO NOT add the "DI inside a script graph" path - that requires
// FNiagaraStackGraphUtilities::AddDataInterface (editor-only view-models).
// See the renderer-authoring docstring for the limitation context.
//
// Engine APIs (line numbers from the UE 5.8 source):
//   FNiagaraTypeDefinition(UClass*)                  NiagaraTypes.h (inline)
//   FNiagaraVariable(TypeDef, Name)                  NiagaraTypes.h (inline)
//   UNiagaraSystem::GetExposedParameters()          NiagaraSystem.h
//   FNiagaraParameterStore::AddParameter             NiagaraParameterStore.h (NIAGARA_API)
//   FNiagaraParameterStore::RemoveParameter          NiagaraParameterStore.h (NIAGARA_API)
//   FNiagaraParameterStore::GetDataInterfaces        NiagaraParameterStore.h (inline)
//   FNiagaraParameterStore::GetDataInterface(Var)    NiagaraParameterStore.h (NIAGARA_API)
//   FNiagaraParameterStore::SetDataInterface(Var)    NiagaraParameterStore.h (inline)
//   UNiagaraDataInterface (base)                     NiagaraDataInterface.h
// ===========================================================================

namespace
{
    // Resolve a UNiagaraDataInterface-derived UClass from a short name
    // ("StaticMesh" / "AudioPlayer" / "Texture") or a full /Script/ path.
    // Returns nullptr on miss. Caller is responsible for IsChildOf check.
    UClass* ResolveNiagaraDataInterfaceClass(const FString& InType)
    {
        if (InType.IsEmpty())
        {
            return nullptr;
        }
        // First try as a class name in any loaded package.
        if (UClass* Found = FindFirstObject<UClass>(*InType))
        {
            return Found;
        }
        // Fallback: load from /Script/Niagara.* if short name.
        const FString FullPath = FString::Printf(TEXT("/Script/Niagara.NiagaraDataInterface%s"),
            InType.StartsWith(TEXT("NiagaraDataInterface")) ? *InType.RightChop(FString(TEXT("NiagaraDataInterface")).Len()) : *InType);
        if (UClass* Loaded = LoadObject<UClass>(nullptr, *FullPath))
        {
            return Loaded;
        }
        return nullptr;
    }

    // Look up a user-param UNiagaraDataInterface on the system by short name
    // (no "User." prefix). The redirect store stores entries keyed by short
    // name with the FULL "User." name as the value. The base FNiagaraParameterStore
    // comparator (operator==) requires Name AND TypeDefHandle to match, but we
    // only know the name from the wire payload. We therefore use the
    // IgnoreType=true overload of the redirect store's FindParameterOffset
    // override, which does a name-only match against the redirect map and
    // returns the offset for the FULL underlying variable (whose concrete
    // DI subclass we don't need to know).
    UNiagaraDataInterface* LookupUserDataInterface(
        FNiagaraUserRedirectionParameterStore& Exposed, const FName& ShortName)
    {
        const FNiagaraVariable LookupVar(
            FNiagaraTypeDefinition(UNiagaraDataInterface::StaticClass()), ShortName);
        const int32* OffsetPtr = Exposed.FindParameterOffset(LookupVar, /*IgnoreType=*/true);
        if (!OffsetPtr)
        {
            return nullptr;
        }
        return Exposed.GetDataInterface(*OffsetPtr);
    }

    // The system at SystemPath, then its user-parameter data interface named ParameterName.
    UNiagaraDataInterface* Niagara_ResolveUserDataInterface(const FString& SystemPath, const FString& ParameterName,
        UNiagaraSystem*& OutSystem, FString& OutError)
    {
        OutSystem = Niagara_ResolveSystem(SystemPath, OutError);
        if (!OutSystem)
        {
            return nullptr;
        }
        UNiagaraDataInterface* DI = LookupUserDataInterface(OutSystem->GetExposedParameters(), FName(*ParameterName));
        if (!DI)
        {
            OutError = FString::Printf(TEXT("Data interface not found: %s"), *ParameterName);
        }
        return DI;
    }
}

// ---------------------------------------------------------------------------
// add_niagara_data_interface
// Creates a new user-parameter DI on the system. The parameter name is
// stored under Niagara's "User." namespace automatically by the redirect
// store (see FNiagaraUserRedirectionParameterStore::AddParameter); callers
// pass the SHORT name (no "User." prefix) and we add it as-is - same pattern
// as HandleSetNiagaraUserParameter.
// ---------------------------------------------------------------------------
// MCP:add_niagara_data_interface - Add a new UNiagaraDataInterface user-parameter binding to a Niagara system.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the new user-parameter (no "User." prefix)
//   @param data_interface_class - Data interface class name (e.g. "StaticMesh", "Texture"); resolved from /Script/Niagara.NiagaraDataInterface<Name> if not already loaded
//   @returns { system_path: string, parameter_name: string, data_interface_class: string, data_interface_class_path?: string, offset?: int, added?: bool, already_exists?: bool }

TSharedPtr<FJsonObject> HandleAddNiagaraDataInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    FString Error;
    if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, ParameterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString DIClassName;
    if (!Params->TryGetStringField(TEXT("data_interface_class"), DIClassName) || DIClassName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'data_interface_class' parameter (e.g. 'StaticMesh', 'Texture')"));
    }

    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UClass* DIClass = ResolveNiagaraDataInterfaceClass(DIClassName);
    if (!DIClass || !DIClass->IsChildOf(UNiagaraDataInterface::StaticClass()))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown / invalid UNiagaraDataInterface class: %s"), *DIClassName));
    }

    const FNiagaraTypeDefinition TypeDef(DIClass);
    const FNiagaraVariable NewVar(TypeDef, FName(*ParameterName));

    FNiagaraUserRedirectionParameterStore& Exposed = System->GetExposedParameters();
    if (Exposed.FindParameterVariable(NewVar))
    {
        TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
        Result->SetStringField(TEXT("data_interface_class"), DIClass->GetName());
        Result->SetBoolField(TEXT("already_exists"), true);
        return mcp_common::MakeOkReply(Result);
    }

    int32 NewOffset = INDEX_NONE;
    const bool bAdded = Exposed.AddParameter(NewVar, /*bInitialize=*/true, /*bTriggerRebind=*/true, &NewOffset);
    if (!bAdded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to add parameter '%s' of type '%s'"), *ParameterName, *DIClass->GetName()));
    }

    System->RequestCompile(true);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("data_interface_class"), DIClass->GetName());
    Result->SetStringField(TEXT("data_interface_class_path"), DIClass->GetPathName());
    Result->SetNumberField(TEXT("offset"), NewOffset);
    Result->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// remove_niagara_data_interface
// ---------------------------------------------------------------------------
// MCP:remove_niagara_data_interface - Remove a UNiagaraDataInterface user-parameter binding from a Niagara system by name.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter to remove (no "User." prefix)
//   @returns { system_path: string, parameter_name: string, removed: bool }

TSharedPtr<FJsonObject> HandleRemoveNiagaraDataInterface(const TSharedPtr<FJsonObject>& Params)
{
    FString ParameterName;
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadParameterSystem(Params, ParameterName, System))
    {
        return Error;
    }

    FNiagaraUserRedirectionParameterStore& Exposed = System->GetExposedParameters();
    const FNiagaraTypeDefinition DITypeDef(UNiagaraDataInterface::StaticClass());
    FNiagaraVariable VarToRemove(DITypeDef, FName(*ParameterName));
    // The redirect store keys its TMap by short name and the value carries the
    // FULL "User.X" variable with the concrete DI subclass as its type. Our wire
    // payload only knows the short name + the base UNiagaraDataInterface type,
    // so the type-strict operator== on the TMap would fail. Use IgnoreType=true
    // for the name-only lookup, then rebuild the FNiagaraVariable from the
    // resolved entry so RemoveParameter sees the exact stored signature.
    const FNiagaraVariableWithOffset* Found = Exposed.FindParameterVariable(VarToRemove, /*IgnoreType=*/true);
    if (Found)
    {
        VarToRemove = FNiagaraVariable(Found->GetType(), Found->GetName());
    }
    const bool bRemoved = Exposed.RemoveParameter(VarToRemove);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to remove parameter '%s' (not found)"), *ParameterName));
    }

    System->RequestCompile(true);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// get_niagara_data_interfaces  - list all user-param DIs on the system.
// ---------------------------------------------------------------------------
// MCP:get_niagara_data_interfaces - List all UNiagaraDataInterface user-parameter bindings on a Niagara system.
//   @param system_path - Asset path of the Niagara System.
//   @returns { system_path: string, count: int, data_interfaces: [{ index, parameter_name, data_interface_class, data_interface_class_path, path_name }] }

TSharedPtr<FJsonObject> HandleGetNiagaraDataInterfaces(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    const FNiagaraUserRedirectionParameterStore& Exposed = System->GetExposedParameters();
    const TArray<UNiagaraDataInterface*>& DIs = Exposed.GetDataInterfaces();

    TArray<TSharedPtr<FJsonValue>> DIArray;
    int32 Idx = 0;
    for (UNiagaraDataInterface* DI : DIs)
    {
        if (!DI)
        {
            ++Idx;
            continue;
        }
        TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();
        DObj->SetNumberField(TEXT("index"), Idx);
        DObj->SetStringField(TEXT("parameter_name"), DI->GetName());
        DObj->SetStringField(TEXT("data_interface_class"), DI->GetClass()->GetName());
        DObj->SetStringField(TEXT("data_interface_class_path"), DI->GetClass()->GetPathName());
        DObj->SetStringField(TEXT("path_name"), DI->GetPathName());
        DIArray.Add(MakeShared<FJsonValueObject>(DObj));
        ++Idx;
    }

    TSharedPtr<FJsonObject> Result = Niagara_StartSystemReply(System);
    Result->SetNumberField(TEXT("count"), DIArray.Num());
    Result->SetArrayField(TEXT("data_interfaces"), DIArray);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_asset
// Set the underlying asset on an existing user-param DI (e.g. set the
// UStaticMesh on a UNiagaraDataInterfaceStaticMesh, or UTexture on a
// UNiagaraDataInterfaceTexture). Uses reflection: we FindPropertyByName
// the DI's UPROPERTY that matches the asset class.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_asset - Points an object property of a user-parameter data interface on a Niagara system at an asset (e.g. the mesh of a StaticMesh DI, the texture of a Texture DI), then requests a system recompile. The DI must already exist (add_niagara_data_interface); property_name picks the property, else the first hard object property in reflection order. The asset must load and be of the property's class, else it is an error. The tool does not save the system, mark it dirty or open an undo transaction: persist it with save_asset. For non-asset fields use set_niagara_data_interface_default.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter DI to mutate
//   @param asset_path - Asset path of the UObject to assign (must be the right subclass)
//   @param property_name - Optional FObjectProperty name on the DI; defaults to the first FObjectProperty
//   @returns { system_path: string, parameter_name: string, property_name: string, asset_path: string, set: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraDataInterfaceAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    FString Error;
    if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, ParameterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString AssetPath;
    if (!mcp_common::ExtractStringPath(Params, TEXT("asset_path"), AssetPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'asset_path' parameter"));
    }
    FString PropertyName;
    Params->TryGetStringField(TEXT("property_name"), PropertyName);
    UNiagaraSystem* System = nullptr;
    UNiagaraDataInterface* DI = Niagara_ResolveUserDataInterface(SystemPath, ParameterName, System, Error);
    if (!DI)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // Find the property. If not specified, pick the first FObjectProperty on the DI.
    FProperty* ObjProp = nullptr;
    if (!PropertyName.IsEmpty())
    {
        ObjProp = DI->GetClass()->FindPropertyByName(FName(*PropertyName));
        if (!ObjProp || !ObjProp->IsA<FObjectProperty>())
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Property '%s' not found / not an object property on %s"),
                    *PropertyName, *DI->GetClass()->GetName()));
        }
    }
    else
    {
        for (TFieldIterator<FObjectProperty> It(DI->GetClass()); It; ++It)
        {
            ObjProp = *It;
            break;
        }
        if (!ObjProp)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("DI class %s has no FObjectProperty to set"), *DI->GetClass()->GetName()));
        }
    }

    FObjectProperty* ConcreteObjProp = static_cast<FObjectProperty*>(ObjProp);
    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset not found: %s"), *AssetPath));
    }
    if (!Asset->IsA(ConcreteObjProp->PropertyClass))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset '%s' is not a %s"), *AssetPath, *ConcreteObjProp->PropertyClass->GetName()));
    }

    ConcreteObjProp->SetObjectPropertyValue(ConcreteObjProp->ContainerPtrToValuePtr<void>(DI), Asset);
    System->RequestCompile(true);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("property_name"), ObjProp->GetName());
    Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_default
// Set an internal default property on the DI (e.g. a FVector / float scalar
// on the DI's user-exposed defaults, distinct from the bound asset above).
// Uses ImportText_Direct against an arbitrary property by name.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_default - Writes one property of a user-parameter data interface on a Niagara system from text parsed by the property's ImportText, so any type it accepts works ("1.5", "(X=1,Y=2,Z=3)", "(R=1,G=0,B=0,A=1)", an enum name), then requests a system recompile. A missing DI or property, or text that does not parse, is an error. The tool does not save the system, mark it dirty or open an undo transaction (persist with save_asset); value is echoed, not read back. List the DI's properties with get_niagara_data_interface_info; for asset references use set_niagara_data_interface_asset.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter DI to mutate
//   @param property_name - Name of the FProperty on the DI to set
//   @param value - String representation of the value (parsed by FProperty::ImportText_Direct)
//   @returns { system_path: string, parameter_name: string, property_name: string, value: string, set: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraDataInterfaceDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    FString Error;
    if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, ParameterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString PropertyName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), PropertyName)) { return Missing; }
    FString Value;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("value"), Value, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UNiagaraSystem* System = nullptr;
    UNiagaraDataInterface* DI = Niagara_ResolveUserDataInterface(SystemPath, ParameterName, System, Error);
    if (!DI)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FProperty* Prop = DI->GetClass()->FindPropertyByName(FName(*PropertyName));
    if (!Prop)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *DI->GetClass()->GetName()));
    }
    void* Addr = Prop->ContainerPtrToValuePtr<void>(DI);
    // FProperty has ImportText_Direct, not ImportText.
    if (!Prop->ImportText_Direct(*Value, Addr, DI, PPF_None))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to parse value '%s' as %s"), *Value, *Prop->GetClass()->GetName()));
    }
    System->RequestCompile(true);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("property_name"), Prop->GetName());
    Result->SetStringField(TEXT("value"), Value);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// get_niagara_data_interface_info  - detail of one user-param DI.
// ---------------------------------------------------------------------------
// MCP:get_niagara_data_interface_info - Get detail and a UPROPERTY dump for one UNiagaraDataInterface user-parameter binding on a Niagara system.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter DI
//   @returns { system_path: string, parameter_name: string, data_interface_class: string, data_interface_class_path: string, path_name: string, properties: [{ name, type }], property_count: int }

TSharedPtr<FJsonObject> HandleGetNiagaraDataInterfaceInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    FString Error;
    if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, ParameterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraSystem* System = nullptr;
    UNiagaraDataInterface* DI = Niagara_ResolveUserDataInterface(SystemPath, ParameterName, System, Error);
    if (!DI)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("data_interface_class"), DI->GetClass()->GetName());
    Result->SetStringField(TEXT("data_interface_class_path"), DI->GetClass()->GetPathName());
    Result->SetStringField(TEXT("path_name"), DI->GetPathName());

    // Dump every UPROPERTY on the DI - lightweight introspection of its state.
    TArray<TSharedPtr<FJsonValue>> PropsArray;
    for (TFieldIterator<FProperty> It(DI->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        if (!Prop) { continue; }
        // Skip editor-only / transient meta and only show "data" properties.
        if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient)) { continue; }

        TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
        PObj->SetStringField(TEXT("name"), Prop->GetName());
        PObj->SetStringField(TEXT("type"), Prop->GetClass()->GetName());
        PropsArray.Add(MakeShared<FJsonValueObject>(PObj));
    }
    Result->SetArrayField(TEXT("properties"), PropsArray);
    Result->SetNumberField(TEXT("property_count"), PropsArray.Num());

    return mcp_common::MakeOkReply(Result);
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_curve  - author the FRichCurve keys on a curve DI.
// Generic over the curve DI family: the caller names the FRichCurve UPROPERTY
// (default "Curve" for UNiagaraDataInterfaceCurve; "RedCurve"/"GreenCurve"/...
// for ColorCurve, "XCurve"/"YCurve"/"ZCurve" for VectorCurve). We resolve it by
// reflection, replace its keys, then rebuild the DI's LUT via the curve base.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_curve - Replaces all keys of one FRichCurve property on a curve user-parameter data interface of a Niagara system with the given time/value keys (an empty array clears it), rebuilds the DI's lookup table, requests a recompile and saves the system; success requires the save. The DI must already exist (add_niagara_data_interface) and curve_property must name an FRichCurve on it: Curve by default, XCurve/YCurve/ZCurve (WCurve too on Vector4) on vector curve DIs, RedCurve/GreenCurve/BlueCurve/AlphaCurve on color curve DIs. For array DIs use set_niagara_data_interface_array.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter curve DI (no "User." prefix)
//   @param keys - Array of { time: number, value: number } keys to set (replaces all existing keys on the channel)
//   @param curve_property - Optional FRichCurve UPROPERTY to author (default "Curve"; e.g. "RedCurve"/"XCurve" on Color/Vector curve DIs)
//   @returns { system_path: string, parameter_name: string, curve_property: string, key_count: int, keys: [{time,value}], set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetNiagaraDataInterfaceCurve(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    FString Error;
    if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, ParameterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    const TArray<TSharedPtr<FJsonValue>>* KeysArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("keys"), KeysArr) || !KeysArr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'keys' array parameter"));
    }
    FString CurvePropName = TEXT("Curve");
    Params->TryGetStringField(TEXT("curve_property"), CurvePropName);

    UNiagaraSystem* System = nullptr;
    UNiagaraDataInterface* DI = Niagara_ResolveUserDataInterface(SystemPath, ParameterName, System, Error);
    if (!DI)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FStructProperty* CurveProp = CastField<FStructProperty>(DI->GetClass()->FindPropertyByName(FName(*CurvePropName)));
    if (!CurveProp || CurveProp->Struct != FRichCurve::StaticStruct())
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("FRichCurve property '%s' not found on %s (Color/Vector curve DIs use RedCurve/XCurve/...)"),
                *CurvePropName, *DI->GetClass()->GetName()));
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraDICurve", "Set Niagara DI Curve (MCP)"));
    System->Modify();

    FRichCurve* Curve = CurveProp->ContainerPtrToValuePtr<FRichCurve>(DI);
    Curve->Reset();
    for (const TSharedPtr<FJsonValue>& KV : *KeysArr)
    {
        const TSharedPtr<FJsonObject>* KeyObj = nullptr;
        if (!KV.IsValid() || !KV->TryGetObject(KeyObj) || !KeyObj) { continue; }
        double Time = 0.0, Value = 0.0;
        (*KeyObj)->TryGetNumberField(TEXT("time"), Time);
        (*KeyObj)->TryGetNumberField(TEXT("value"), Value);
        Curve->AddKey(static_cast<float>(Time), static_cast<float>(Value));
    }

    // Rebuild the sampling LUT from the authored keys (what the editor does on edit).
    if (UNiagaraDataInterfaceCurveBase* CurveBase = Cast<UNiagaraDataInterfaceCurveBase>(DI))
    {
        CurveBase->UpdateTimeRanges();
        CurveBase->UpdateLUT();
    }
    System->RequestCompile(true);
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Honest read-back from the stored curve.
    TArray<TSharedPtr<FJsonValue>> OutKeys;
    for (const FRichCurveKey& K : Curve->Keys)
    {
        TSharedPtr<FJsonObject> KObj = MakeShared<FJsonObject>();
        KObj->SetNumberField(TEXT("time"), K.Time);
        KObj->SetNumberField(TEXT("value"), K.Value);
        OutKeys.Add(MakeShared<FJsonValueObject>(KObj));
    }

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("curve_property"), CurveProp->GetName());
    Result->SetNumberField(TEXT("key_count"), Curve->GetNumKeys());
    Result->SetArrayField(TEXT("keys"), OutKeys);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_niagara_data_interface_curve"));
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_array  - set the numeric values of an array DI.
// Generic over numeric array DIs (UNiagaraDataInterfaceArrayFloat::FloatData,
// the int32 array DI's IntData, ...). We resolve the FArrayProperty by name and
// require a numeric inner; struct-element arrays (Vector/Color/Position) carry an
// LWC editor/runtime split and are rejected with an honest error.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_array - Set the numeric values of an array user-parameter data interface (e.g. ArrayFloat FloatData / int32 array IntData).
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter array DI (no "User." prefix)
//   @param values - Array of numbers to store (replaces all existing array contents)
//   @param array_property - Optional array UPROPERTY to set (default "FloatData"; e.g. "IntData" for the int32 array DI)
//   @returns { system_path: string, parameter_name: string, array_property: string, element_type: string, count: int, values: [number], set: bool, saved: bool }
TSharedPtr<FJsonObject> HandleSetNiagaraDataInterfaceArray(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString ParameterName;
    FString Error;
    if (!Niagara_ReadSystemAndParameterNames(Params, SystemPath, ParameterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    const TArray<TSharedPtr<FJsonValue>>* ValuesArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("values"), ValuesArr) || !ValuesArr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'values' array parameter"));
    }
    FString ArrayPropName = TEXT("FloatData");
    Params->TryGetStringField(TEXT("array_property"), ArrayPropName);

    UNiagaraSystem* System = nullptr;
    UNiagaraDataInterface* DI = Niagara_ResolveUserDataInterface(SystemPath, ParameterName, System, Error);
    if (!DI)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FArrayProperty* ArrProp = CastField<FArrayProperty>(DI->GetClass()->FindPropertyByName(FName(*ArrayPropName)));
    if (!ArrProp)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Array property '%s' not found on %s"), *ArrayPropName, *DI->GetClass()->GetName()));
    }
    FNumericProperty* Inner = CastField<FNumericProperty>(ArrProp->Inner);
    if (!Inner)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Array '%s' on %s has non-numeric element type '%s' (this command supports float/int arrays)"),
                *ArrayPropName, *DI->GetClass()->GetName(), *ArrProp->Inner->GetClass()->GetName()));
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraDIArray", "Set Niagara DI Array (MCP)"));
    System->Modify();

    FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(DI));
    Helper.Resize(ValuesArr->Num());
    const bool bIsFloat = Inner->IsFloatingPoint();
    for (int32 i = 0; i < ValuesArr->Num(); ++i)
    {
        double Num = 0.0;
        if ((*ValuesArr)[i].IsValid()) { (*ValuesArr)[i]->TryGetNumber(Num); }
        void* ElemPtr = Helper.GetRawPtr(i);
        if (bIsFloat) { Inner->SetFloatingPointPropertyValue(ElemPtr, Num); }
        else { Inner->SetIntPropertyValue(ElemPtr, static_cast<int64>(Num)); }
    }

    System->RequestCompile(true);
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Honest read-back from the stored array.
    TArray<TSharedPtr<FJsonValue>> OutValues;
    for (int32 i = 0; i < Helper.Num(); ++i)
    {
        void* ElemPtr = Helper.GetRawPtr(i);
        const double V = bIsFloat ? Inner->GetFloatingPointPropertyValue(ElemPtr)
                                  : static_cast<double>(Inner->GetSignedIntPropertyValue(ElemPtr));
        OutValues.Add(MakeShared<FJsonValueNumber>(V));
    }

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("array_property"), ArrProp->GetName());
    Result->SetStringField(TEXT("element_type"), Inner->GetClass()->GetName());
    Result->SetNumberField(TEXT("count"), Helper.Num());
    Result->SetArrayField(TEXT("values"), OutValues);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_niagara_data_interface_array"));
}
// ========================================================================== //
// Niagara system authoring advanced
//
// Commands that operate on UNiagaraSystem asset-level knobs (warmup +
// determinism) and on UNiagaraEmitter asset-level metadata. All headless-safe.
//   set_niagara_system_warmup_time         UNiagaraSystem::SetWarmupTime(float)
//   set_niagara_system_warmup_tick_count  WarmupTickCount UPROPERTY (no public setter;
//                                         ResolveWarmupTickCount auto-derives from WarmupTime).
//   set_niagara_system_warmup_tick_delta  UNiagaraSystem::SetWarmupTickDelta(float)
//   set_niagara_system_deterministic      bDeterminism UPROPERTY
//   get_niagara_system_warmup_time        GetWarmupTime() + GetWarmupTickCount()
//   set_niagara_emitter_template_description
//                                         UNiagaraEmitter::TemplateAssetDescription (FText)
// ========================================================================== //

// MCP:set_niagara_system_warmup_time - Set the warmup time in seconds of a Niagara System asset and save it; a failed save returns success false. With a non-zero warmup_tick_delta the engine rounds the time down to a whole number of ticks, so read new_warmup_time and warmup_tick_count back.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param warmup_time - Warmup time in seconds (>= 0; 0 disables warmup); the call fails without it.
//   @returns { system_path: string, old_warmup_time: number, new_warmup_time: number, warmup_tick_count: int, applied: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemWarmupTime(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    double WarmupTimeValue = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("warmup_time"), WarmupTimeValue)) { return Missing; }
    const float WarmupTime = static_cast<float>(WarmupTimeValue);

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const float OldTime = System->GetWarmupTime();
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemWarmupTime", "Set Niagara System Warmup Time (MCP)"));
    System->Modify();
    System->SetWarmupTime(WarmupTime);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("old_warmup_time"), OldTime);
    Data->SetNumberField(TEXT("new_warmup_time"), System->GetWarmupTime());
    Data->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    Data->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_warmup_time"));
}

// MCP:set_niagara_system_warmup_tick_count - Re-resolve the protected UNiagaraSystem::WarmupTickCount from the current WarmupTime/WarmupTickDelta pair.
//   @param system_path - Asset path of the Niagara System.
//   @returns { system_path: string, old_warmup_tick_count: int, new_warmup_tick_count: int, warmup_time: number, warmup_tick_delta: number, applied: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemWarmupTickCount(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    // UNiagaraSystem::WarmupTickCount is protected (NiagaraSystem.h). The
    // only public mutation paths are: (1) UNiagaraSystem::SetWarmupTime +
    // UNiagaraSystem::ResolveWarmupTickCount auto-derives the count; (2) UPROPERTY
    // exposure via reflection on the asset. We use (1) here: clear warmup time
    // so the resolved count lands at zero (matching the documented behaviour).
    // For arbitrary positive counts, callers should use set_niagara_system_warmup_time
    // and let the engine derive.
    const int32 OldCount = System->GetWarmupTickCount();
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ResolveNiagaraSystemWarmupTickCount", "Resolve Niagara System Warmup Tick Count (MCP)"));
    System->Modify();
    // Re-resolve from the current WarmupTime + WarmupTickDelta pair so the
    // protected WarmupTickCount field reflects a coherent state.
    System->ResolveWarmupTickCount();

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("old_warmup_tick_count"), OldCount);
    Data->SetNumberField(TEXT("new_warmup_tick_count"), System->GetWarmupTickCount());
    Data->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
    Data->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    Data->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_warmup_tick_count"));
}

// MCP:set_niagara_system_warmup_tick_delta - Set the warmup tick delta (seconds per tick) on a Niagara system.
//   @param system_path - Asset path of the Niagara System.
//   @param warmup_tick_delta - Tick delta in seconds (must be > 0)
//   @returns { system_path: string, old_warmup_tick_delta: number, new_warmup_tick_delta: number, applied: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemWarmupTickDelta(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    if (!Params->HasField(TEXT("warmup_tick_delta")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'warmup_tick_delta' parameter"));
    }
    const float TickDelta = static_cast<float>(Params->GetNumberField(TEXT("warmup_tick_delta")));
    if (TickDelta <= 0.0f)
    {
        return mcp_common::MakeErrorReply(TEXT("warmup_tick_delta must be > 0"));
    }

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const float OldDelta = System->GetWarmupTickDelta();
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemWarmupTickDelta", "Set Niagara System Warmup Tick Delta (MCP)"));
    System->Modify();
    System->SetWarmupTickDelta(TickDelta);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("old_warmup_tick_delta"), OldDelta);
    Data->SetNumberField(TEXT("new_warmup_tick_delta"), System->GetWarmupTickDelta());
    Data->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_warmup_tick_delta"));
}

// MCP:set_niagara_system_deterministic - Set the bDeterminism UPROPERTY on a Niagara system via reflection.
//   @param system_path - Asset path of the Niagara System.
//   @param deterministic - Boolean to assign to bDeterminism
//   @returns { system_path: string, old_deterministic: bool, new_deterministic: bool, applied: bool, saved: bool }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemDeterministic(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    if (!Params->HasField(TEXT("deterministic")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'deterministic' parameter"));
    }

    // UNiagaraSystem::bDeterminism is protected (NiagaraSystem.h). The
    // public surface only exposes the read-only NeedsDeterminism() getter. To
    // honour the intent of the caller, we mutate the field via reflection
    // (FProperty::SetValue_InContainer). This routes through Unreal's
    // property-system bookkeeping so dirty-tracking + serialization still work.
    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UClass* SystemClass = System->GetClass();
    FBoolProperty* DetProp = FindFProperty<FBoolProperty>(SystemClass, TEXT("bDeterminism"));
    if (!DetProp)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Could not locate UNiagaraSystem::bDeterminism property (engine layout changed?)"));
    }

    const bool bDeterministic = Params->GetBoolField(TEXT("deterministic"));
    const bool OldValue = System->NeedsDeterminism();
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemDeterministic", "Set Niagara System Deterministic (MCP)"));
    System->Modify();
    DetProp->SetPropertyValue_InContainer(System, bDeterministic);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetBoolField(TEXT("old_deterministic"), OldValue);
    Data->SetBoolField(TEXT("new_deterministic"), System->NeedsDeterminism());
    Data->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_deterministic"));
}

// MCP:get_niagara_system_warmup_time - Read the warmup and determinism settings of a Niagara System asset: warmup_time in seconds, warmup_tick_count, warmup_tick_delta, needs_warmup and deterministic. Read-only; change them with set_niagara_system_warmup_time, set_niagara_system_warmup_tick_count, set_niagara_system_warmup_tick_delta and set_niagara_system_deterministic.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @returns { system_path: string, warmup_time: number, warmup_tick_count: int, warmup_tick_delta: number, needs_warmup: bool, deterministic: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraSystemWarmupTime(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
    Data->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    Data->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    Data->SetBoolField(TEXT("needs_warmup"), System->NeedsWarmup());
    Data->SetBoolField(TEXT("deterministic"), System->NeedsDeterminism());
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_emitter_template_description - Set the TemplateAssetDescription text of a standalone Niagara Emitter asset (not an emitter inside a system) and save the asset; a failed save returns success false. Returns old_description and new_description; an empty string clears the text.
//   @param emitter_asset_path - Asset path of the UNiagaraEmitter, e.g. "/Game/FX/NE_Sparks" (an emitter asset, not a system path).
//   @param description - New description text, stored as FText; an empty string clears it.
//   @returns { emitter_asset_path: string, old_description: string, new_description: string, applied: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterTemplateDescription(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("emitter_asset_path"), EmitterPath)) { return Missing; }
    FString DescriptionText;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("description"), DescriptionText, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UNiagaraEmitter* Emitter = mcp_common::ResolveAsset<UNiagaraEmitter>(EmitterPath);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Niagara emitter not found: %s"), *EmitterPath));
    }

    const FText OldDescription = Emitter->TemplateAssetDescription;
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterTemplateDescription", "Set Niagara Emitter Template Description (MCP)"));
    Emitter->Modify();
    Emitter->TemplateAssetDescription = FText::FromString(DescriptionText);

    bool bSaved = false;
    FString SaveError;
    if (UPackage* Package = Emitter->GetPackage())
    {
        bSaved = mcp_common::SaveAssetPackage(Package, Emitter, SaveError);
    }

TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("emitter_asset_path"), Emitter->GetPathName());
    Data->SetStringField(TEXT("old_description"), OldDescription.ToString());
    Data->SetStringField(TEXT("new_description"), Emitter->TemplateAssetDescription.ToString());
    Data->SetBoolField(TEXT("applied"), true);
    if (!bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_template_description"));
}

// ========================================================================== //
// Niagara emitter authoring (asset-level)
//
// Per-emitter authoring that complements the emitter-handle ops, the sim target / effect type
// commands and the module-input commands. All mutations are direct on
// FVersionedNiagaraEmitterData (accessed via FNiagaraEmitterHandle::GetEmitterData()),
// so they are fully headless and only need the system path + emitter name.
//
//   set_niagara_emitter_bounds              CalculateBoundsMode (Dynamic|Fixed|Programmable)
//                                            + FixedBounds (FBox: min XYZ + max XYZ).
//   set_niagara_emitter_allocation_mode     EParticleAllocationMode (AutomaticEstimate |
//                                            ManualEstimate | FixedCount) + PreAllocationCount.
//   set_niagara_emitter_max_gpu_particles   MaxGPUParticlesSpawnPerFrame override
//                                            (only meaningful when SimTarget == GPUComputeSim).
//   set_niagara_emitter_local_space         toggle bLocalSpace (particles in emitter-local
//                                            vs world space).
//   set_niagara_emitter_requires_persistent_ids
//                                            toggle bRequiresPersistentIDs (stable
//                                            Particles.ID across frames; small perf cost).
//   get_niagara_emitter_runtime_summary     flat readback: module count, renderer count,
//                                            script count, sim target, bounds mode,
//                                            allocation mode, max GPU particles, local
//                                            space, persistent IDs.
//
// Every setter follows the same pattern:
//   1. Niagara_ResolveEmitterData(SystemPath, EmitterName, ...) -> FVersionedNiagaraEmitterData*
//   2. FScopedTransaction + System->Modify()
//   3. Direct mutation on the FVersionedNiagaraEmitterData fields
//   4. SaveModifiedNiagaraSystem(System, SaveError)
// ========================================================================== //

// MCP:set_niagara_emitter_bounds - Set the CalculateBoundsMode and FixedBounds on a Niagara emitter's FVersionedNiagaraEmitterData.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param bounds_mode - One of "Dynamic", "Fixed", or "Programmable"; defaults to "Fixed"
//   @param bounds_min - Optional [x, y, z] string (FVector) for the FixedBounds minimum
//   @param bounds_max - Optional [x, y, z] string (FVector) for the FixedBounds maximum
//   @returns { system_path: string, emitter_name: string, bounds_mode: string, bounds_min: string, bounds_max: string, applied: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterBounds(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // bounds_mode is optional; default = Fixed (the only mode that uses the
    // explicit FixedBounds FBox). Dynamic ignores FixedBounds entirely.
    FString BoundsModeStr = TEXT("Fixed");
    Params->TryGetStringField(TEXT("bounds_mode"), BoundsModeStr);
    ENiagaraEmitterCalculateBoundMode NewMode = ENiagaraEmitterCalculateBoundMode::Fixed;
    if (BoundsModeStr.Equals(TEXT("Dynamic"), ESearchCase::IgnoreCase))
    {
        NewMode = ENiagaraEmitterCalculateBoundMode::Dynamic;
    }
    else if (BoundsModeStr.Equals(TEXT("Fixed"), ESearchCase::IgnoreCase))
    {
        NewMode = ENiagaraEmitterCalculateBoundMode::Fixed;
    }
    else if (BoundsModeStr.Equals(TEXT("Programmable"), ESearchCase::IgnoreCase))
    {
        NewMode = ENiagaraEmitterCalculateBoundMode::Programmable;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid bounds_mode '%s' (expected Dynamic|Fixed|Programmable)"), *BoundsModeStr));
    }

    // bounds_min / bounds_max default to the engine default FBox
    // (FBox(FVector(-100), FVector(100))) when bounds_mode != Fixed. When
    // mode is Fixed and only one of min/max is supplied, the unspecified
    // side keeps its current value.
    FVector NewMin = FVector(-100.0);
    FVector NewMax = FVector(100.0);
    bool bHasMin = false, bHasMax = false;
    FString MinStr, MaxStr;
    if (Params->TryGetStringField(TEXT("bounds_min"), MinStr) && !MinStr.IsEmpty())
    {
        NewMin.InitFromString(MinStr);
        bHasMin = true;
    }
    if (Params->TryGetStringField(TEXT("bounds_max"), MaxStr) && !MaxStr.IsEmpty())
    {
        NewMax.InitFromString(MaxStr);
        bHasMax = true;
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterData, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterBounds", "Set Niagara Emitter Bounds"));
    System->Modify();
    EmitterData->CalculateBoundsMode = NewMode;
    if (bHasMin || bHasMax)
    {
        // Build the new FBox from any supplied corners + the current
        // counterpart (so partial updates work).
        const FBox OldBox = EmitterData->FixedBounds;
        const FVector Min = bHasMin ? NewMin : OldBox.Min;
        const FVector Max = bHasMax ? NewMax : OldBox.Max;
        EmitterData->FixedBounds = FBox(Min, Max);
    }
    else if (NewMode == ENiagaraEmitterCalculateBoundMode::Fixed)
    {
        // No explicit min/max supplied + Fixed mode: reset to the engine default.
        EmitterData->FixedBounds = FVersionedNiagaraEmitterData::GetDefaultFixedBounds();
    }

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("bounds_mode"), BoundsModeStr);
    Data->SetStringField(TEXT("bounds_min"), EmitterData->FixedBounds.Min.ToString());
    Data->SetStringField(TEXT("bounds_max"), EmitterData->FixedBounds.Max.ToString());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("saved"), SaveError.IsEmpty());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_emitter_allocation_mode - Set the EParticleAllocationMode (and optional PreAllocationCount) on a Niagara emitter's FVersionedNiagaraEmitterData.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param allocation_mode - One of "AutomaticEstimate", "ManualEstimate", or "FixedCount" (case-insensitive; "Automatic" and "Manual" and "Fixed" also accepted)
//   @param pre_allocation_count - Optional int; only meaningful when not AutomaticEstimate
//   @returns { system_path: string, emitter_name: string, allocation_mode: string, pre_allocation_count: int, applied: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterAllocationMode(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModeStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("allocation_mode"), ModeStr)) { return Missing; }

    EParticleAllocationMode NewMode = EParticleAllocationMode::AutomaticEstimate;
    if (ModeStr.Equals(TEXT("AutomaticEstimate"), ESearchCase::IgnoreCase) ||
        ModeStr.Equals(TEXT("Automatic"), ESearchCase::IgnoreCase))
    {
        NewMode = EParticleAllocationMode::AutomaticEstimate;
    }
    else if (ModeStr.Equals(TEXT("ManualEstimate"), ESearchCase::IgnoreCase) ||
             ModeStr.Equals(TEXT("Manual"), ESearchCase::IgnoreCase))
    {
        NewMode = EParticleAllocationMode::ManualEstimate;
    }
    else if (ModeStr.Equals(TEXT("FixedCount"), ESearchCase::IgnoreCase) ||
             ModeStr.Equals(TEXT("Fixed"), ESearchCase::IgnoreCase))
    {
        NewMode = EParticleAllocationMode::FixedCount;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid allocation_mode '%s' (expected AutomaticEstimate|ManualEstimate|FixedCount)"),
                            *ModeStr));
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterData, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterAllocationMode", "Set Niagara Emitter Allocation Mode"));
    System->Modify();
    EmitterData->AllocationMode = NewMode;
    // pre_allocation_count only meaningful when not AutomaticEstimate.
    if (Params->HasField(TEXT("pre_allocation_count")))
    {
        const int32 PreCount = static_cast<int32>(Params->GetNumberField(TEXT("pre_allocation_count")));
        EmitterData->PreAllocationCount = FMath::Max(0, PreCount);
    }

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("allocation_mode"), ModeStr);
    Data->SetNumberField(TEXT("pre_allocation_count"), EmitterData->PreAllocationCount);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("saved"), SaveError.IsEmpty());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_emitter_max_gpu_particles - Set the MaxGPUParticlesSpawnPerFrame override on a Niagara emitter (only effective when SimTarget is GPUComputeSim).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param max_gpu_particles - Non-negative integer
//   @returns { system_path: string, emitter_name: string, sim_target: string, max_gpu_particles: int, effective_for_current_sim: bool, applied: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterMaxGPUParticles(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (!Params->HasField(TEXT("max_gpu_particles")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'max_gpu_particles' parameter"));
    }
    const int32 NewCount = static_cast<int32>(Params->GetNumberField(TEXT("max_gpu_particles")));
    if (NewCount < 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("max_gpu_particles must be >= 0 (got %d)"), NewCount));
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterData, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The EditCondition on MaxGPUParticlesSpawnPerFrame is
    // "SimTarget == ENiagaraSimTarget::GPUComputeSim"; we warn (but still
    // apply) when the emitter is CPU — the caller may be pre-configuring
    // before flipping SimTarget.
    const bool bIsGPUSim = (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim);

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterMaxGPUParticles", "Set Niagara Emitter Max GPU Particles"));
    System->Modify();
    EmitterData->MaxGPUParticlesSpawnPerFrame = NewCount;

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("sim_target"), bIsGPUSim ? TEXT("GPUComputeSim") : TEXT("CPUSim"));
    Data->SetNumberField(TEXT("max_gpu_particles"), NewCount);
    Data->SetBoolField(TEXT("effective_for_current_sim"), bIsGPUSim);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("saved"), SaveError.IsEmpty());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_emitter_local_space - Set whether one emitter of a Niagara System asset simulates in local space (bLocalSpace on its emitter data), then save the system. Returns local_space as read back, applied and saved (with save_error); the reply stays success even when the save fails, so check saved.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param emitter_name - Display name of the emitter handle (case-insensitive).
//   @param local_space - true to simulate the particles of the emitter in its local space, false for world space; the call fails without it.
//   @returns { system_path: string, emitter_name: string, local_space: bool, applied: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterLocalSpace(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    bool bLocalSpace = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("local_space"), bLocalSpace)) { return Missing; }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterData, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterLocalSpace", "Set Niagara Emitter Local Space"));
    System->Modify();
    EmitterData->bLocalSpace = bLocalSpace;

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("saved"), SaveError.IsEmpty());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_emitter_requires_persistent_ids - Toggle bRequiresPersistentIDs on a Niagara emitter (stable Particles.ID across frames; small perf cost).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param requires_persistent_ids - Boolean to assign
//   @returns { system_path: string, emitter_name: string, requires_persistent_ids: bool, applied: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterRequiresPersistentIds(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (!Params->HasField(TEXT("requires_persistent_ids")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'requires_persistent_ids' parameter"));
    }
    const bool bRequires = Params->GetBoolField(TEXT("requires_persistent_ids"));

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterData, System, Error);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterRequiresPersistentIds", "Set Niagara Emitter Requires Persistent Ids"));
    System->Modify();
    EmitterData->bRequiresPersistentIDs = bRequires ? 1 : 0;

    FString SaveError;
    SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetBoolField(TEXT("requires_persistent_ids"), EmitterData->bRequiresPersistentIDs != 0);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("saved"), SaveError.IsEmpty());
    Niagara_AddSaveError(Data, SaveError);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_niagara_emitter_runtime_summary - Flat readback of a Niagara emitter's runtime knobs and counts.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @returns { system_path: string, emitter_name: string, sim_target: string, bounds_mode: string, bounds_min: string, bounds_max: string, allocation_mode: string, pre_allocation_count: int, max_gpu_particles: int, local_space: bool, deterministic: bool, requires_persistent_ids: bool, update_module_count: int, spawn_module_count: int, emitter_spawn_module_count: int, emitter_update_module_count: int, renderer_count: int, event_handler_count: int, simulation_stage_count: int }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterRuntimeSummary(const TSharedPtr<FJsonObject>& Params)
{
    // Flat readback for an emitter's runtime knobs. Same fields that the
    // setter commands above mutate, plus counts of modules / renderers /
    // scripts so callers can quickly sanity-check the emitter shape.
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterData, EmitterName, System, EmitterData))
    {
        return Error;
    }

    // Count modules on each of the 4 standard scripts (Update/Spawn/EmitterSpawn/EmitterUpdate)
    // by walking the UNiagaraGraph's UNiagaraNodeFunctionCall nodes. Skipping
    // the graph walk keeps the readback cheap and headless-safe (no recompile).
    auto CountFunctionCalls = [](UNiagaraScript* Script) -> int32
    {
        if (!Script) { return 0; }
        // UNiagaraScript exposes GetLatestSource() (NiagaraScript.h)
        // as the 0-arg editor-only overload; GetSource() requires a version
        // FGuid. Downcast to the concrete UNiagaraScriptSource to access the
        // NodeGraph TObjectPtr (the field is public on the derived class; no
        // GetGraph() accessor exists).
        UNiagaraScriptSourceBase* BaseSource = Script->GetLatestSource();
        UNiagaraScriptSource* Source = Cast<UNiagaraScriptSource>(BaseSource);
        if (!Source) { return 0; }
        UNiagaraGraph* Graph = Source->NodeGraph;
        if (!Graph) { return 0; }
        int32 Count = 0;
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            if (Cast<UNiagaraNodeFunctionCall>(Node))
            {
                ++Count;
            }
        }
        return Count;
    };
    const int32 UpdateModules = CountFunctionCalls(EmitterData->UpdateScriptProps.Script);
    const int32 SpawnModules = CountFunctionCalls(EmitterData->SpawnScriptProps.Script);
    const int32 EmitterSpawnModules = CountFunctionCalls(EmitterData->EmitterSpawnScriptProps.Script);
    const int32 EmitterUpdateModules = CountFunctionCalls(EmitterData->EmitterUpdateScriptProps.Script);
    const int32 RendererCount = EmitterData->GetRenderers().Num();

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("sim_target"),
        EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPUComputeSim") : TEXT("CPUSim"));
    Data->SetStringField(TEXT("bounds_mode"),
        EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Dynamic ? TEXT("Dynamic") :
        EmitterData->CalculateBoundsMode == ENiagaraEmitterCalculateBoundMode::Fixed ? TEXT("Fixed") :
        TEXT("Programmable"));
    Data->SetStringField(TEXT("bounds_min"), EmitterData->FixedBounds.Min.ToString());
    Data->SetStringField(TEXT("bounds_max"), EmitterData->FixedBounds.Max.ToString());
    Data->SetStringField(TEXT("allocation_mode"),
        EmitterData->AllocationMode == EParticleAllocationMode::AutomaticEstimate ? TEXT("AutomaticEstimate") :
        EmitterData->AllocationMode == EParticleAllocationMode::ManualEstimate ? TEXT("ManualEstimate") :
        TEXT("FixedCount"));
    Data->SetNumberField(TEXT("pre_allocation_count"), EmitterData->PreAllocationCount);
    Data->SetNumberField(TEXT("max_gpu_particles"), EmitterData->MaxGPUParticlesSpawnPerFrame);
    Data->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
    Data->SetBoolField(TEXT("deterministic"), EmitterData->bDeterminism);
    Data->SetBoolField(TEXT("requires_persistent_ids"), EmitterData->bRequiresPersistentIDs != 0);
    Data->SetNumberField(TEXT("update_module_count"), UpdateModules);
    Data->SetNumberField(TEXT("spawn_module_count"), SpawnModules);
    Data->SetNumberField(TEXT("emitter_spawn_module_count"), EmitterSpawnModules);
    Data->SetNumberField(TEXT("emitter_update_module_count"), EmitterUpdateModules);
    Data->SetNumberField(TEXT("renderer_count"), RendererCount);
    Data->SetNumberField(TEXT("event_handler_count"), EmitterData->EventHandlerScriptProps.Num());
    Data->SetNumberField(TEXT("simulation_stage_count"), EmitterData->GetSimulationStages().Num());
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Niagara module stack graph CRUD
//
// The commands of this section operate on the UNiagaraGraph that backs the ParticleSpawn /
// ParticleUpdate stack of an emitter. add_niagara_module_to_graph differs from
// add_niagara_module_to_stack in that it accepts an explicit
// target_index (insert at position N rather than only at the end) and is the
// insertion path the duplicate / reorder commands also rely on.
// remove / duplicate / enable / info round-trip the same node graph directly
// via FNiagaraStackGraphUtilities.
//
// Engine APIs (NiagaraStackGraphUtilities.h):
//   FNiagaraStackGraphUtilities::AddScriptModuleToStack(UNiagaraScript*, UNiagaraNodeOutput&, int32, FString, const FGuid&)
//   FNiagaraStackGraphUtilities::RemoveModuleFromStack(UNiagaraSystem&, FGuid, UNiagaraNodeFunctionCall&)
//   FNiagaraStackGraphUtilities::GetAllModuleNodes(UNiagaraGraph*)
//   FNiagaraStackGraphUtilities::GetOrderedModuleNodes(UNiagaraNodeOutput&, TArray<UNiagaraNodeFunctionCall*>&)
//   FNiagaraStackGraphUtilities::GetModuleIsEnabled(UNiagaraNodeFunctionCall&)
//   FNiagaraStackGraphUtilities::SetModuleIsEnabled(UNiagaraNodeFunctionCall&, bool)
//   FNiagaraEditorUtilities::GetScriptOutputNode(UNiagaraScript&)                                                         NiagaraEditorUtilities.h
// ===========================================================================

namespace
{
    // Resolve the per-usage group script of an emitter.
    UNiagaraScript* GetEmitterGroupScript(FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage)
    {
        if (!EmitterData)
        {
            return nullptr;
        }
        if (Usage == ENiagaraScriptUsage::ParticleSpawnScript)
        {
            return EmitterData->SpawnScriptProps.Script;
        }
        return EmitterData->UpdateScriptProps.Script;
    }

    // Resolve the spawn/update output node for an emitter's group graph, the same
    // resolution HandleAddNiagaraModuleToStack does inline, so every command of this
    // section shares one canonical emitter-graph entry path.
    UNiagaraNodeOutput* ResolveEmitterOutputNode(FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage, FString& OutError)
    {
        if (!EmitterData)
        {
            OutError = TEXT("Emitter data is null");
            return nullptr;
        }
        UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
        if (!Src || !Src->NodeGraph)
        {
            OutError = TEXT("Emitter has no editable script graph (stateless emitter or missing GraphSource)");
            return nullptr;
        }
        UNiagaraScript* GroupScript = GetEmitterGroupScript(EmitterData, Usage);
        if (!GroupScript)
        {
            OutError = TEXT("Emitter has no group script for the requested usage");
            return nullptr;
        }
        UNiagaraNodeOutput* Out = FNiagaraEditorUtilities::GetScriptOutputNode(*GroupScript);
        if (!Out)
        {
            OutError = TEXT("No output node found in the emitter group graph");
        }
        return Out;
    }

    // Find a UNiagaraNodeFunctionCall by its display name (GetFunctionName) on the
    // ordered module list of a group graph. Used by remove / duplicate / enable /
    // info to locate a module by name without requiring a GUID.
    UNiagaraNodeFunctionCall* FindModuleNodeByNameOnGraph(UNiagaraGraph* Graph, const FString& ModuleName)
    {
        if (!Graph || ModuleName.IsEmpty())
        {
            return nullptr;
        }
        for (UEdGraphNode* Node : Graph->Nodes)
        {
            UNiagaraNodeFunctionCall* Func = Cast<UNiagaraNodeFunctionCall>(Node);
            if (Func && Func->GetFunctionName().Equals(ModuleName, ESearchCase::CaseSensitive))
            {
                return Func;
            }
        }
        return nullptr;
    }

    // The system, the emitter data and the output node of the emitter's Usage stack, resolved in that order.
    UNiagaraNodeOutput* Niagara_ResolveStackOutput(const FString& SystemPath, const FString& EmitterName,
        ENiagaraScriptUsage Usage, UNiagaraSystem*& OutSystem, FVersionedNiagaraEmitterData*& OutEmitterData, FString& OutError)
    {
        FNiagaraEmitterHandle* Handle = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, OutSystem, OutError);
        if (!Handle)
        {
            return nullptr;
        }
        OutEmitterData = Handle->GetEmitterData();
        return ResolveEmitterOutputNode(OutEmitterData, Usage, OutError);
    }

    // Niagara_ResolveStackOutput for the ScriptGroup stack, then the module named ModuleName on that stack's graph.
    UNiagaraNodeFunctionCall* Niagara_ResolveStackModule(const FString& SystemPath, const FString& EmitterName,
        const FString& ScriptGroup, const FString& ModuleName, UNiagaraSystem*& OutSystem,
        FVersionedNiagaraEmitterData*& OutEmitterData, UNiagaraNodeOutput*& OutOutput, FString& OutError)
    {
        OutOutput = Niagara_ResolveStackOutput(SystemPath, EmitterName, Niagara_ScriptGroupUsage(ScriptGroup),
            OutSystem, OutEmitterData, OutError);
        if (!OutOutput)
        {
            return nullptr;
        }
        UNiagaraNodeFunctionCall* Module = FindModuleNodeByNameOnGraph(Cast<UNiagaraGraph>(OutOutput->GetGraph()), ModuleName);
        if (!Module)
        {
            OutError = FString::Printf(TEXT("Module '%s' not found on %s graph"), *ModuleName, *ScriptGroup);
        }
        return Module;
    }

    // Calls Modify() on the emitter's script graph, when it has one.
    void Niagara_ModifyEmitterGraph(FVersionedNiagaraEmitterData* EmitterData)
    {
        UNiagaraScriptSource* Src = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
        if (Src && Src->NodeGraph)
        {
            Src->NodeGraph->Modify();
        }
    }
}

// ---------------------------------------------------------------------------
// add_niagara_module_to_graph
// Same underlying API as add_niagara_module_to_stack (FNiagaraStackGraphUtilities::
// AddScriptModuleToStack) but accepts an explicit target_index so callers can
// place the module anywhere in the stack. INDEX_NONE appends to the end.
// ---------------------------------------------------------------------------
// MCP:add_niagara_module_to_graph - Insert a UNiagaraScript module at a specific index in a Niagara emitter's particle stack graph.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param module_script_path - Asset path of the module UNiagaraScript
//   @param script_group - Optional "spawn" or "update"; defaults to "update"
//   @param target_index - Optional stack index for insertion; INDEX_NONE (-1) appends to the end
//   @returns { system_path: string, emitter_name: string, module_script_path: string, script_group: string, target_index: int, module_node_name: string, added: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleAddNiagaraModuleToGraph(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleScriptPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_script_path"), ModuleScriptPath)) { return Missing; }
    FString ScriptGroup;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!Niagara_ReadStrictScriptGroup(Params, ScriptGroup, Usage, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    int32 TargetIndex = INDEX_NONE;
    Params->TryGetNumberField(TEXT("target_index"), TargetIndex);

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = Niagara_ResolveStackOutput(SystemPath, EmitterName, Usage, System, EmitterData, Error);
    if (!Out)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraScript* Mod = mcp_common::ResolveAsset<UNiagaraScript>(ModuleScriptPath);
    if (!Mod)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Niagara module script not found: %s"), *ModuleScriptPath));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraModuleToGraph", "Add Niagara Module To Graph"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);

    UNiagaraNodeFunctionCall* NewModule =
        FNiagaraStackGraphUtilities::AddScriptModuleToStack(Mod, *Out, TargetIndex);
    if (!NewModule)
    {
        return mcp_common::MakeErrorReply(
            TEXT("AddScriptModuleToStack returned null (module could not be inserted into the graph)"));
    }
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("module_script_path"), Mod->GetPathName());
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetNumberField(TEXT("target_index"), TargetIndex);
    Data->SetStringField(TEXT("module_node_name"), NewModule->GetFunctionName());
    Data->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_niagara_module_to_graph"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_niagara_module_from_stack
// Removes a UNiagaraNodeFunctionCall module from the emitter's group graph.
// Identifies the module by name (GetFunctionName) on the spawn OR update graph.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_module_from_stack - Remove a UNiagaraNodeFunctionCall module from a Niagara emitter's spawn or update graph.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param module_name - Display name of the module function-call node to remove
//   @param script_group - Optional "spawn" or "update"; defaults to "update"
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, removed: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveNiagaraModuleFromStack(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_name"), ModuleName)) { return Missing; }
    const FString ScriptGroup = Niagara_ReadScriptGroup(Params);

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = nullptr;
    UNiagaraNodeFunctionCall* TargetModule =
        Niagara_ResolveStackModule(SystemPath, EmitterName, ScriptGroup, ModuleName, System, EmitterData, Out, Error);
    if (!TargetModule)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraModuleFromStack", "Remove Niagara Module From Stack"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);

    // FNiagaraStackGraphUtilities::RemoveModuleFromStack is NOT exported, so we
    // replicate the editor's removal path directly: UEdGraph::RemoveNode (which
    // breaks all links by default) + UEdGraphNode::DestroyNode + recompile. This
    // is a single-module call (the user asked to remove one module by name), so
    // the broader stack-group disassembly that the internal helper performs is
    // not needed.
    UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph());
    if (!NiagaraGraph)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Could not resolve UNiagaraGraph from output node"));
    }
    TargetModule->Modify();
    const bool bRemoved = NiagaraGraph->RemoveNode(TargetModule);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("RemoveNode refused to remove '%s' (module may be wired to required pins)"), *ModuleName));
    }
    TargetModule->DestroyNode();
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), ModuleName);
    Data->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_niagara_module_from_stack"), SaveError);
}

// ---------------------------------------------------------------------------
// duplicate_niagara_module_in_stack
// Loads the source module UNiagaraScript by GetFunctionName's script reference,
// then re-inserts it via AddScriptModuleToStack with an optional insertion
// offset (relative to the original module's index, or absolute target_index
// if target_index_override is set).
// ---------------------------------------------------------------------------
// MCP:duplicate_niagara_module_in_stack - Duplicate a UNiagaraNodeFunctionCall module in a Niagara emitter's spawn or update graph.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param module_name - Display name of the module to duplicate
//   @param script_group - Optional "spawn" or "update"; defaults to "update"
//   @param target_index_override - Optional absolute target index for the duplicate; defaults to one-past-source
//   @returns { system_path: string, emitter_name: string, script_group: string, source_module_name: string, new_module_name: string, inserted_index: int, duplicated: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleDuplicateNiagaraModuleInStack(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_name"), ModuleName)) { return Missing; }
    const FString ScriptGroup = Niagara_ReadScriptGroup(Params);
    int32 TargetIndexOverride = INDEX_NONE;
    Params->TryGetNumberField(TEXT("target_index_override"), TargetIndexOverride);

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = nullptr;
    UNiagaraNodeFunctionCall* Source =
        Niagara_ResolveStackModule(SystemPath, EmitterName, ScriptGroup, ModuleName, System, EmitterData, Out, Error);
    if (!Source)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraScript* SourceScript = Source->FunctionScript;
    if (!SourceScript)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Module '%s' has no FunctionScript (cannot duplicate)"), *ModuleName));
    }

    // Compute insertion index: explicit override > one-past-source > end.
    // We use UNiagaraGraph::GetNodesOfClass (public, ENGINE_API) instead of the
    // non-exported FNiagaraStackGraphUtilities::GetOrderedModuleNodes. Order
    // matches the internal helper well enough for index computation; the actual
    // insertion position is determined by AddScriptModuleToStack.
    int32 InsertIndex = TargetIndexOverride;
    if (InsertIndex == INDEX_NONE)
    {
        TArray<UNiagaraNodeFunctionCall*> Ordered;
        if (UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph()))
        {
            NiagaraGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Ordered);
        }
        const int32 SourceIdx = Ordered.IndexOfByKey(Source);
        InsertIndex = (SourceIdx != INDEX_NONE) ? SourceIdx + 1 : INDEX_NONE;
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "DuplicateNiagaraModuleInStack", "Duplicate Niagara Module In Stack"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);

    UNiagaraNodeFunctionCall* Duplicate = FNiagaraStackGraphUtilities::AddScriptModuleToStack(
        SourceScript, *Out, InsertIndex);
    if (!Duplicate)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to duplicate module '%s'"), *ModuleName));
    }
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("source_module_name"), ModuleName);
    Data->SetStringField(TEXT("new_module_name"), Duplicate->GetFunctionName());
    Data->SetNumberField(TEXT("inserted_index"), InsertIndex);
    Data->SetBoolField(TEXT("duplicated"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("duplicate_niagara_module_in_stack"), SaveError);
}

// ---------------------------------------------------------------------------
// set_niagara_module_enabled
// Toggle bIsEnabled on a UNiagaraNodeFunctionCall via
// FNiagaraStackGraphUtilities::SetModuleIsEnabled. Returns the pre/post state.
// ---------------------------------------------------------------------------
// MCP:set_niagara_module_enabled - Enable or disable a UNiagaraNodeFunctionCall module in a Niagara emitter's spawn or update graph.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param module_name - Display name of the module to toggle
//   @param enabled - Optional bool; defaults to true
//   @param script_group - Optional "spawn" or "update"; defaults to "update"
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, previous_enabled?: bool, enabled: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraModuleEnabled(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_name"), ModuleName)) { return Missing; }
    bool bEnabled = true;
    Params->TryGetBoolField(TEXT("enabled"), bEnabled);
    const FString ScriptGroup = Niagara_ReadScriptGroup(Params);

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = nullptr;
    UNiagaraNodeFunctionCall* Target =
        Niagara_ResolveStackModule(SystemPath, EmitterName, ScriptGroup, ModuleName, System, EmitterData, Out, Error);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // FNiagaraStackGraphUtilities::GetModuleIsEnabled is NOT exported, so we
    // call the public UEdGraphNode::IsNodeEnabled() (which UNiagaraNode inherits
    // unchanged) directly. For a single module node this is identical to the
    // pre-state that the internal helper would have returned.
    const TOptional<bool> OldState = TOptional<bool>(Target->IsNodeEnabled());
    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraModuleEnabled", "Set Niagara Module Enabled"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);
    FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Target, bEnabled);
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), ModuleName);
    if (OldState.IsSet())
    {
        Data->SetBoolField(TEXT("previous_enabled"), OldState.GetValue());
    }
    Data->SetBoolField(TEXT("enabled"), bEnabled);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_module_enabled"), SaveError);
}

// ---------------------------------------------------------------------------
// reorder_niagara_modules
// Move a module to a new index within its group stack, in place (see the
// handler). This gives full positional control without depending on MoveModule
// (which moves across scripts, not within a single stack).
// ---------------------------------------------------------------------------
// MCP:reorder_niagara_modules - Move a UNiagaraNodeFunctionCall module to a new index in its Niagara emitter stack (in-place reordering).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param module_name - Display name of the module to move
//   @param target_index - Non-negative target index in the ordered stack
//   @param script_group - Optional "spawn" or "update"; defaults to "update"
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, old_index: int, new_index: int, target_index: int, reordered: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleReorderNiagaraModules(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_name"), ModuleName)) { return Missing; }
    int32 TargetIndex = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("target_index"), TargetIndex) || TargetIndex < 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing or non-negative 'target_index' required"));
    }
    const FString ScriptGroup = Niagara_ReadScriptGroup(Params);

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = nullptr;
    UNiagaraNodeFunctionCall* Target =
        Niagara_ResolveStackModule(SystemPath, EmitterName, ScriptGroup, ModuleName, System, EmitterData, Out, Error);
    if (!Target)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraScript* SourceScript = Target->FunctionScript;
    if (!SourceScript)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Module '%s' has no FunctionScript (cannot reorder)"), *ModuleName));
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ReorderNiagaraModules", "Reorder Niagara Modules"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);

    // Capture old position for the response. UNiagaraGraph::GetNodesOfClass
    // (inherited from UEdGraph, ENGINE_API) replaces the non-exported
    // FNiagaraStackGraphUtilities::GetOrderedModuleNodes.
    UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph());
    if (!NiagaraGraph)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Could not resolve UNiagaraGraph from output node"));
    }
    TArray<UNiagaraNodeFunctionCall*> Before;
    NiagaraGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Before);
    const int32 OldIndex = Before.IndexOfByKey(Target);

    // In-place reorder within UEdGraph::Nodes (UEdGraph ENGINE_API). A
    // RemoveNode+DestroyNode+AddScriptModuleToStack path crashes
    // deterministically: destroying the node orphans cached links inside the bound
    // script and the subsequent AddScriptModuleToStack walks those stale
    // links on the second invocation (UAF). Reordering the same UObject in
    // place preserves the SourceScript binding and pin wiring. NotifyGraphChanged
    // (UEdGraph ENGINE_API) replaces the non-exported
    // FNiagaraStackGraphUtilities::RelayoutGraph — the editor refreshes
    // visual layout on next view, which is sufficient for a one-shot CLI
    // reordering. The project's get_niagara_module_info measures stack_index
    // via UNiagaraGraph::GetNodesOfClass (array iteration), so swapping the
    // filtered position in Nodes is the source of truth the rest of the
    // subsystem reads.
    Target->Modify();
    NiagaraGraph->Modify();
    TArray<UNiagaraNodeFunctionCall*> Ordered;
    NiagaraGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Ordered);
    const int32 OldOrderedIndex = Ordered.IndexOfByKey(Target);
    if (OldOrderedIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Module '%s' not present in graph FunctionCall nodes"), *ModuleName));
    }
    const int32 MaxOrderedIndex = Ordered.Num() > 0 ? Ordered.Num() - 1 : 0;
    const int32 ClampedTargetIndex = FMath::Clamp(TargetIndex, 0, MaxOrderedIndex);
    UNiagaraNodeFunctionCall* Reinserted = nullptr;
    // UEdGraph::Nodes is TArray<TObjectPtr<UEdGraphNode>>. Bind by
    // reference to that exact type; TObjectPtr<T> converts implicitly to T*
    // in expressions so RemoveAt/Insert + the raw pointer use below are
    // well-formed.
    TArray<TObjectPtr<UEdGraphNode>>& GraphNodes = NiagaraGraph->Nodes;
    auto FindGraphIndexOf = [&GraphNodes](UEdGraphNode* Needle) -> int32
    {
        for (int32 i = 0; i < GraphNodes.Num(); ++i)
        {
            if (GraphNodes[i].Get() == Needle)
            {
                return i;
            }
        }
        return INDEX_NONE;
    };
    if (OldOrderedIndex == ClampedTargetIndex)
    {
        Reinserted = Target;
    }
    else
    {
        const int32 OldGraphIndex = FindGraphIndexOf(Target);
        if (OldGraphIndex == INDEX_NONE)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Module '%s' not present in graph node array (old_index=%d)"), *ModuleName, OldIndex));
        }
        GraphNodes.RemoveAt(OldGraphIndex);
        // Re-resolve the destination module's graph position AFTER removal so
        // the insert lands at the right slot in the post-removal array.
        UNiagaraNodeFunctionCall* InsertBefore = (ClampedTargetIndex < Ordered.Num())
            ? Ordered[ClampedTargetIndex]
            : nullptr;
        int32 InsertAt;
        if (!InsertBefore || InsertBefore == Target)
        {
            InsertAt = GraphNodes.Num();
        }
        else
        {
            InsertAt = FindGraphIndexOf(InsertBefore);
            if (InsertAt == INDEX_NONE)
            {
                InsertAt = GraphNodes.Num();
            }
        }
        GraphNodes.Insert(Target, InsertAt);
        Reinserted = Target;
        NiagaraGraph->NotifyGraphChanged();
    }
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TArray<UNiagaraNodeFunctionCall*> After;
    NiagaraGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(After);
    const int32 NewIndex = After.IndexOfByKey(Reinserted);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), ModuleName);
    Data->SetNumberField(TEXT("old_index"), OldIndex);
    Data->SetNumberField(TEXT("new_index"), NewIndex);
    Data->SetNumberField(TEXT("target_index"), TargetIndex);
    Data->SetBoolField(TEXT("reordered"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("reorder_niagara_modules"), SaveError);
}

// ---------------------------------------------------------------------------
// get_niagara_module_info
// Flat readback for a single UNiagaraNodeFunctionCall on the spawn OR update
// graph: script class, function name, position in the ordered stack, enabled
// state, input pin count, and the node GUID.
// ---------------------------------------------------------------------------
// MCP:get_niagara_module_info - Read detail for a single UNiagaraNodeFunctionCall module in a Niagara emitter's spawn or update graph.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param module_name - Display name of the module function-call node
//   @param script_group - Optional "spawn" or "update"; defaults to "update"
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, node_guid: string, stack_index: int, total_modules_in_stack: int, function_script?: string, function_script_class?: string, is_enabled?: bool, input_pin_count: int, output_pin_count: int, is_deprecated: bool }

TSharedPtr<FJsonObject> HandleGetNiagaraModuleInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString ModuleName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("module_name"), ModuleName)) { return Missing; }
    const FString ScriptGroup = Niagara_ReadScriptGroup(Params);
    const ENiagaraScriptUsage Usage = Niagara_ScriptGroupUsage(ScriptGroup);

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = Niagara_ResolveStackOutput(SystemPath, EmitterName, Usage, System, EmitterData, Error);
    if (!Out)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TArray<UNiagaraNodeFunctionCall*> Ordered;
    if (UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph()))
    {
        // Public UNiagaraGraph::GetNodesOfClass (inherited from UEdGraph,
        // ENGINE_API) replaces the non-exported
        // FNiagaraStackGraphUtilities::GetOrderedModuleNodes.
        NiagaraGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Ordered);
    }
    UNiagaraNodeFunctionCall* Target = nullptr;
    int32 StackIndex = INDEX_NONE;
    for (int32 i = 0; i < Ordered.Num(); ++i)
    {
        if (Ordered[i] && Ordered[i]->GetFunctionName().Equals(ModuleName, ESearchCase::CaseSensitive))
        {
            Target = Ordered[i];
            StackIndex = i;
            break;
        }
    }
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Module '%s' not found on %s graph"), *ModuleName, *ScriptGroup));
    }

    int32 InputPinCount = 0;
    int32 OutputPinCount = 0;
    for (UEdGraphPin* Pin : Target->Pins)
    {
        if (!Pin)
        {
            continue;
        }
        if (Pin->Direction == EGPD_Input)
        {
            ++InputPinCount;
        }
        else if (Pin->Direction == EGPD_Output)
        {
            ++OutputPinCount;
        }
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), Target->GetFunctionName());
    Data->SetStringField(TEXT("node_guid"), Target->NodeGuid.ToString());
    Data->SetNumberField(TEXT("stack_index"), StackIndex);
    Data->SetNumberField(TEXT("total_modules_in_stack"), Ordered.Num());
    if (Target->FunctionScript)
    {
        Data->SetStringField(TEXT("function_script"), Target->FunctionScript->GetPathName());
        Data->SetStringField(TEXT("function_script_class"), Target->FunctionScript->GetClass()->GetName());
    }
    // FNiagaraStackGraphUtilities::GetModuleIsEnabled is NOT exported; for a
    // single module node UEdGraphNode::IsNodeEnabled() (inherited by
    // UNiagaraNode) returns the same single-module state the internal helper
    // would have surfaced.
    const TOptional<bool> Enabled = TOptional<bool>(Target->IsNodeEnabled());
    if (Enabled.IsSet())
    {
        Data->SetBoolField(TEXT("is_enabled"), Enabled.GetValue());
    }
    Data->SetNumberField(TEXT("input_pin_count"), InputPinCount);
    Data->SetNumberField(TEXT("output_pin_count"), OutputPinCount);
    Data->SetBoolField(TEXT("is_deprecated"), Target->IsDeprecated());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// list_niagara_modules / get_niagara_module_stack
// Read-only enumeration of the UNiagaraNodeFunctionCall modules
// on an emitter's spawn/update stack. Same export-SAFE ordered-node path as
// HandleGetNiagaraModuleInfo (ResolveEmitterOutputNode +
// UNiagaraGraph::GetNodesOfClass). It deliberately AVOIDS
// FNiagaraStackGraphUtilities::GetOrderedModuleNodes / GetAllModuleNodes /
// GetModuleIsEnabled (NOT exported -> LNK2019); the stale comment block naming
// them above is wrong. Honest-empty arrays when a group has no modules.
//
// list_niagara_modules = ONE script_group (spawn|update, default update).
// get_niagara_module_stack = BOTH spawn AND update, grouped as spawn:[]/update:[].
// ---------------------------------------------------------------------------
namespace
{
    // Enumerate the ordered UNiagaraNodeFunctionCall modules of an emitter's
    // spawn/update group graph into a JSON array. Fields mirror
    // HandleGetNiagaraModuleInfo exactly. On resolution failure returns an empty
    // array and sets OutError (caller decides whether to surface it) — the
    // per-group entry stays honest-empty rather than faking modules.
    TArray<TSharedPtr<FJsonValue>> BuildNiagaraModuleArray(
        FVersionedNiagaraEmitterData* EmitterData, ENiagaraScriptUsage Usage,
        const FString& ScriptGroupLabel, int32& OutTotalModules, FString& OutError)
    {
        OutTotalModules = 0;
        TArray<TSharedPtr<FJsonValue>> ModuleArray;
        UNiagaraNodeOutput* Out = ResolveEmitterOutputNode(EmitterData, Usage, OutError);
        if (!Out)
        {
            return ModuleArray;
        }
        TArray<UNiagaraNodeFunctionCall*> Ordered;
        if (UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph()))
        {
            // Public UNiagaraGraph::GetNodesOfClass (ENGINE_API, inherited from
            // UEdGraph) — the export-safe substitute for GetOrderedModuleNodes.
            NiagaraGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(Ordered);
        }
        OutTotalModules = Ordered.Num();
        for (int32 i = 0; i < Ordered.Num(); ++i)
        {
            UNiagaraNodeFunctionCall* Func = Ordered[i];
            TSharedPtr<FJsonObject> MObj = MakeShared<FJsonObject>();
            MObj->SetNumberField(TEXT("index"), i);
            MObj->SetStringField(TEXT("script_group"), ScriptGroupLabel);
            MObj->SetStringField(TEXT("module_name"), Func ? Func->GetFunctionName() : TEXT("null"));
            MObj->SetStringField(TEXT("node_guid"), Func ? Func->NodeGuid.ToString() : TEXT("null"));
            if (Func && Func->FunctionScript)
            {
                MObj->SetStringField(TEXT("function_script"), Func->FunctionScript->GetPathName());
                MObj->SetStringField(TEXT("function_script_class"),
                    Func->FunctionScript->GetClass()->GetName());
            }
            // UEdGraphNode::IsNodeEnabled() — the export-safe substitute for the
            // non-exported FNiagaraStackGraphUtilities::GetModuleIsEnabled.
            MObj->SetBoolField(TEXT("is_enabled"), Func ? Func->IsNodeEnabled() : false);
            MObj->SetBoolField(TEXT("is_deprecated"), Func ? Func->IsDeprecated() : false);
            int32 InputPinCount = 0;
            int32 OutputPinCount = 0;
            if (Func)
            {
                for (UEdGraphPin* Pin : Func->Pins)
                {
                    if (!Pin)
                    {
                        continue;
                    }
                    if (Pin->Direction == EGPD_Input)
                    {
                        ++InputPinCount;
                    }
                    else if (Pin->Direction == EGPD_Output)
                    {
                        ++OutputPinCount;
                    }
                }
            }
            MObj->SetNumberField(TEXT("input_pin_count"), InputPinCount);
            MObj->SetNumberField(TEXT("output_pin_count"), OutputPinCount);
            MObj->SetNumberField(TEXT("total_modules_in_stack"), Ordered.Num());
            ModuleArray.Add(MakeShared<FJsonValueObject>(MObj));
        }
        return ModuleArray;
    }
}

// MCP:list_niagara_modules - List the module function-call nodes on ONE stack of a Niagara emitter (read-only). Resolves the emitter's spawn OR update group graph (script_group, default "update") via the export-safe ResolveEmitterOutputNode + UNiagaraGraph::GetNodesOfClass path, then reports each UNiagaraNodeFunctionCall: index, module_name (GetFunctionName), node_guid, function_script(+class), is_enabled (IsNodeEnabled), is_deprecated, input/output pin counts. Honest-empty modules:[] when the stack has no modules or the graph is not resolvable (a note is returned).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param script_group - Optional "spawn" or "update"; defaults to "update".
//   @returns { system_path: string, emitter_name: string, script_group: string, module_count: int, total_modules_in_stack: int, modules: [{ index, script_group, module_name, node_guid, function_script?, function_script_class?, is_enabled, is_deprecated, input_pin_count, output_pin_count, total_modules_in_stack }], note?: string }

TSharedPtr<FJsonObject> HandleListNiagaraModules(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("list_niagara_modules: enter"));
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    const FString ScriptGroup = Niagara_ReadScriptGroup(Params);
    const ENiagaraScriptUsage Usage = Niagara_ScriptGroupUsage(ScriptGroup);

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, System, Error);
    if (!Handle)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();

    FString ResolveError;
    int32 TotalModules = 0;
    TArray<TSharedPtr<FJsonValue>> ModuleArray =
        BuildNiagaraModuleArray(EmitterData, Usage, ScriptGroup, TotalModules, ResolveError);
    UE_LOG(LogTemp, Verbose, TEXT("list_niagara_modules: %s/%s [%s] -> %d module(s)%s"),
        *SystemPath, *EmitterName, *ScriptGroup, TotalModules,
        ResolveError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (note: %s)"), *ResolveError));

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetNumberField(TEXT("module_count"), ModuleArray.Num());
    Data->SetNumberField(TEXT("total_modules_in_stack"), TotalModules);
    Data->SetArrayField(TEXT("modules"), ModuleArray);
    // Honest note when the group graph could not be resolved (stateless emitter,
    // missing output node) — the array stays empty rather than faking modules.
    if (!ResolveError.IsEmpty())
    {
        Data->SetStringField(TEXT("note"), ResolveError);
    }
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_niagara_module_stack - Read BOTH the spawn AND update module stacks of a Niagara emitter in one call (read-only). Runs the same export-safe enumeration as list_niagara_modules twice, returning the modules grouped as spawn:[...] and update:[...] (each entry also carries its own script_group tag). Honest-empty arrays per group, with a spawn_note/update_note if a graph could not be resolved.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @returns { system_path: string, emitter_name: string, spawn: [{...}], update: [{...}], spawn_module_count: int, update_module_count: int, total_modules_in_stack: int, spawn_note?: string, update_note?: string }

TSharedPtr<FJsonObject> HandleGetNiagaraModuleStack(const TSharedPtr<FJsonObject>& Params)
{
    UE_LOG(LogTemp, Verbose, TEXT("get_niagara_module_stack: enter"));
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, System, Error);
    if (!Handle)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();

    // Run the export-safe enumeration chain twice — once per group.
    FString SpawnError;
    int32 SpawnTotal = 0;
    TArray<TSharedPtr<FJsonValue>> SpawnArray = BuildNiagaraModuleArray(
        EmitterData, ENiagaraScriptUsage::ParticleSpawnScript, TEXT("spawn"), SpawnTotal, SpawnError);
    FString UpdateError;
    int32 UpdateTotal = 0;
    TArray<TSharedPtr<FJsonValue>> UpdateArray = BuildNiagaraModuleArray(
        EmitterData, ENiagaraScriptUsage::ParticleUpdateScript, TEXT("update"), UpdateTotal, UpdateError);
    UE_LOG(LogTemp, Verbose, TEXT("get_niagara_module_stack: %s/%s -> spawn:%d update:%d"),
        *SystemPath, *EmitterName, SpawnTotal, UpdateTotal);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetArrayField(TEXT("spawn"), SpawnArray);
    Data->SetArrayField(TEXT("update"), UpdateArray);
    Data->SetNumberField(TEXT("spawn_module_count"), SpawnArray.Num());
    Data->SetNumberField(TEXT("update_module_count"), UpdateArray.Num());
    Data->SetNumberField(TEXT("total_modules_in_stack"), SpawnTotal + UpdateTotal);
    // Honest per-group notes if a graph could not be resolved (arrays stay empty).
    if (!SpawnError.IsEmpty())
    {
        Data->SetStringField(TEXT("spawn_note"), SpawnError);
    }
    if (!UpdateError.IsEmpty())
    {
        Data->SetStringField(TEXT("update_note"), UpdateError);
    }
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Emitter event handlers
// Writer side for the reader-only event_handler_count: author
// FNiagaraEventScriptProperties on an emitter's FVersionedNiagaraEmitterData via
// UNiagaraEmitter::AddEventHandler / RemoveEventHandlerByUsageId. Mirrors the
// add_niagara_simulation_stage pattern (the handler's UNiagaraScript MUST be
// allocated — AddEventHandler dereferences EventHandler.Script under WITH_EDITOR;
// the Script's UsageId is the handler's identity for removal).
// ===========================================================================

// MCP:add_niagara_event_handler - Add an event handler to a Niagara emitter (the writer for event_handler_count).  Constructs an FNiagaraEventScriptProperties (allocating its UNiagaraScript with usage ParticleEventScript, mirroring add_niagara_simulation_stage) and adds it via UNiagaraEmitter::AddEventHandler. Idempotent by source_event_name: if a handler for that event already exists it is returned (already_exists=true) without adding a duplicate. The system is saved after modification. The handler's event-script graph is created in a default state — full event wiring must be completed in the Niagara editor (reported honestly via note); the handler properties are persisted and re-readable.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Display name of the emitter handle within the system.
//   @param source_event_name - Name of the source event this handler consumes (its identity key for idempotency).
//   @param execution_mode - Optional EveryParticle | SpawnedParticles | SingleParticle (default EveryParticle).
//   @param spawn_number - Optional int particles to spawn per event (SpawnedParticles mode).
//   @param max_events_per_frame - Optional int cap on events consumed per frame.
//   @param min_spawn_number - Optional int minimum spawn when random_spawn_number is set.
//   @param random_spawn_number - Optional bool: spawn a random count in [min_spawn_number, spawn_number].
//   @param source_emitter_id - Optional GUID of the emitter handle that generates the event (default: this emitter).
//   @returns { system_path: string, emitter_name: string, usage_id: string, source_event_name: string, execution_mode: string, spawn_number: int, max_events_per_frame: int, already_exists: bool, event_handler_count: int, saved: bool, save_error?: string, note: string }

TSharedPtr<FJsonObject> HandleAddNiagaraEventHandler(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString SourceEventName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_event_name"), SourceEventName)) { return Missing; }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const FName EventName(*SourceEventName);

    // Idempotent by source event name: return the existing handler unchanged.
    for (const FNiagaraEventScriptProperties& Existing : EmitterData->GetEventHandlers())
    {
        if (Existing.SourceEventName == EventName)
        {
            TSharedPtr<FJsonObject> Resp = EventHandlerToJson(Existing);
            Resp->SetStringField(TEXT("system_path"), System->GetPathName());
            Resp->SetStringField(TEXT("emitter_name"), EmitterName);
            Resp->SetBoolField(TEXT("already_exists"), true);
            Resp->SetNumberField(TEXT("event_handler_count"), EmitterData->GetEventHandlers().Num());
            Resp->SetBoolField(TEXT("saved"), false);
            Resp->SetStringField(TEXT("note"),
                TEXT("An event handler for this source_event_name already exists; returned unchanged. To change it, remove_niagara_event_handler by usage_id then re-add."));
            return mcp_common::MakeOkReply(Resp);
        }
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraEventHandler", "Add Niagara Event Handler"));
    System->Modify();
    Emitter->Modify();

    // Build the event handler. Its Script must be non-null (AddEventHandler
    // dereferences it under WITH_EDITOR) and its UsageId is the removal key.
    FNiagaraEventScriptProperties EventHandler;
    EventHandler.Script = NewObject<UNiagaraScript>(
        Emitter, MakeUniqueObjectName(Emitter, UNiagaraScript::StaticClass(), TEXT("EventScript")),
        RF_Transactional);
    if (!EventHandler.Script)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to create UNiagaraScript for the event handler"));
    }
    EventHandler.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
    const FGuid UsageId = FGuid::NewGuid();
    EventHandler.Script->SetUsageId(UsageId);
    // Reuse the emitter spawn script's source so the event script is consistent
    // with the rest of the emitter graph (matches add_niagara_simulation_stage).
    if (EmitterData->SpawnScriptProps.Script)
    {
        if (UNiagaraScriptSourceBase* EmitterSource = EmitterData->SpawnScriptProps.Script->GetLatestSource())
        {
            EventHandler.Script->SetLatestSource(EmitterSource);
        }
    }

    EventHandler.SourceEventName = EventName;

    FString ExecModeStr;
    if (Params->TryGetStringField(TEXT("execution_mode"), ExecModeStr) && !ExecModeStr.IsEmpty())
    {
        EScriptExecutionMode Mode;
        if (!ParseEventExecMode(ExecModeStr, Mode))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid execution_mode '%s' (use EveryParticle|SpawnedParticles|SingleParticle)"), *ExecModeStr));
        }
        EventHandler.ExecutionMode = Mode;
    }

    int32 IntVal = 0;
    if (Params->TryGetNumberField(TEXT("spawn_number"), IntVal))         { EventHandler.SpawnNumber = static_cast<uint32>(FMath::Max(0, IntVal)); }
    if (Params->TryGetNumberField(TEXT("max_events_per_frame"), IntVal)) { EventHandler.MaxEventsPerFrame = static_cast<uint32>(FMath::Max(0, IntVal)); }
    if (Params->TryGetNumberField(TEXT("min_spawn_number"), IntVal))     { EventHandler.MinSpawnNumber = static_cast<uint32>(FMath::Max(0, IntVal)); }
    bool BoolVal = false;
    if (Params->TryGetBoolField(TEXT("random_spawn_number"), BoolVal))   { EventHandler.bRandomSpawnNumber = BoolVal; }
    FString SourceEmitterIdStr;
    if (Params->TryGetStringField(TEXT("source_emitter_id"), SourceEmitterIdStr) && !SourceEmitterIdStr.IsEmpty())
    {
        FGuid SrcGuid;
        if (FGuid::Parse(SourceEmitterIdStr, SrcGuid))
        {
            EventHandler.SourceEmitterID = SrcGuid;
        }
    }

    Emitter->AddEventHandler(EventHandler, EmitterData->Version.VersionGuid);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Resp = EventHandlerToJson(EventHandler);
    Resp->SetStringField(TEXT("system_path"), System->GetPathName());
    Resp->SetStringField(TEXT("emitter_name"), EmitterName);
    Resp->SetBoolField(TEXT("already_exists"), false);
    Resp->SetNumberField(TEXT("event_handler_count"), EmitterData->GetEventHandlers().Num());
    if (!bSaved && !SaveError.IsEmpty())
    {
        Resp->SetStringField(TEXT("save_error"), SaveError);
    }
    Resp->SetStringField(TEXT("note"),
        TEXT("Event handler created with a default event script; complete the event-script graph (DataSetRead/Write wiring) in the Niagara editor. The handler properties are persisted and re-readable via get_niagara_event_handlers."));
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("add_niagara_event_handler"));
}

// MCP:remove_niagara_event_handler - Remove an event handler from a Niagara emitter by its usage_id.  Calls UNiagaraEmitter::RemoveEventHandlerByUsageId. Idempotent: if no handler with that usage_id exists it reports already_removed without error. The system is saved after a removal.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle within the system.
//   @param usage_id - The handler's usage_id (as returned by add_niagara_event_handler / get_niagara_event_handlers).
//   @returns { system_path: string, emitter_name: string, usage_id: string, already_removed: bool, removed: bool, event_handler_count: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveNiagaraEventHandler(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString UsageIdStr;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("usage_id"), UsageIdStr)) { return Missing; }
    // Accept the GUID with or without hyphens/braces.
    FGuid TargetUsageId;
    {
        FString Stripped = UsageIdStr.Replace(TEXT("-"), TEXT("")).Replace(TEXT("{"), TEXT("")).Replace(TEXT("}"), TEXT("")).TrimStartAndEnd();
        if (!FGuid::Parse(Stripped, TargetUsageId))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Invalid usage_id '%s' (expected a GUID)"), *UsageIdStr));
        }
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    auto Exists = [EmitterData, TargetUsageId]() -> bool
    {
        for (const FNiagaraEventScriptProperties& EH : EmitterData->GetEventHandlers())
        {
            if (EH.Script && EH.Script->GetUsageId() == TargetUsageId) { return true; }
        }
        return false;
    };

    const bool bExisted = Exists();
    bool bSaved = false;
    FString SaveError;
    if (bExisted)
    {
        FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraEventHandler", "Remove Niagara Event Handler"));
        System->Modify();
        Emitter->Modify();
        Emitter->RemoveEventHandlerByUsageId(TargetUsageId, EmitterData->Version.VersionGuid);
        bSaved = SaveModifiedNiagaraSystem(System, SaveError);
    }
    const bool bRemoved = bExisted && !Exists();

    TSharedPtr<FJsonObject> Resp = Niagara_StartEmitterReply(System, EmitterName);
    Resp->SetStringField(TEXT("usage_id"), TargetUsageId.ToString(EGuidFormats::DigitsWithHyphens));
    Resp->SetBoolField(TEXT("already_removed"), !bExisted);
    Resp->SetBoolField(TEXT("removed"), bRemoved);
    Resp->SetNumberField(TEXT("event_handler_count"), EmitterData->GetEventHandlers().Num());
    if (bExisted && !bSaved && !SaveError.IsEmpty())
    {
        Resp->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_niagara_event_handler"));
}

// MCP:get_niagara_event_handlers - List a Niagara emitter's event handlers and their properties.  Reads FVersionedNiagaraEmitterData::GetEventHandlers(); each entry reports its usage_id (the remove key), source_event_name, execution_mode, spawn/event counts and source_emitter_id.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle within the system.
//   @returns { system_path: string, emitter_name: string, event_handler_count: int, event_handlers: [{usage_id, source_event_name, execution_mode, spawn_number, max_events_per_frame, min_spawn_number, random_spawn_number, source_emitter_id, has_script}] }

TSharedPtr<FJsonObject> HandleGetNiagaraEventHandlers(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataUninitialised, EmitterName, System, EmitterData))
    {
        return Error;
    }

    TArray<TSharedPtr<FJsonValue>> Handlers;
    for (const FNiagaraEventScriptProperties& EH : EmitterData->GetEventHandlers())
    {
        Handlers.Add(MakeShared<FJsonValueObject>(EventHandlerToJson(EH)));
    }

    TSharedPtr<FJsonObject> Resp = Niagara_StartEmitterReply(System, EmitterName);
    Resp->SetNumberField(TEXT("event_handler_count"), Handlers.Num());
    Resp->SetArrayField(TEXT("event_handlers"), Handlers);
    return mcp_common::MakeOkReply(Resp);
}

// ---------------------------------------------------------------------------
// UNiagaraSystem render/system flags (the asset-level overrides
// that drive the spawned UNiagaraComponent's defaults, plus the system-wide
// LWC / current-frame-data toggles + fixed bounds).
//
// These are plain settable UPROPERTYs that DO NOT participate in the Niagara
// compile/scalability pipeline, so we mutate them via raw reflection
// (FProperty::SetPropertyValue_InContainer) under an FScopedTransaction +
// System->Modify() and persist with SaveModifiedNiagaraSystem. We deliberately
// DO NOT call PostEditChangeProperty: it broadcasts scalability/CommitUpdate
// which is headless-fragile and unnecessary (confirmed reopen-clean, no
// recompile). The bitfield access specifier is irrelevant to reflection
// (mirrors HandleSetNiagaraSystemDeterministic, whose bDeterminism is protected).
//
// Render overrides are no-ops at runtime unless their bOverride* companion is
// also set, so when a value field is supplied we set its companion too.
// ---------------------------------------------------------------------------
namespace
{
    // Set / read a bool (uint8:1 / uint32:1), int or float UPROPERTY of the system by member name. Returns
    // false if the property could not be located (engine layout drift -> honest error).
    template <typename TProperty, typename TValue>
    bool Niagara_SetSystemProp(UNiagaraSystem* System, const TCHAR* MemberName, const TValue& Value)
    {
        TProperty* Prop = FindFProperty<TProperty>(System->GetClass(), MemberName);
        if (!Prop) { return false; }
        Prop->SetPropertyValue_InContainer(System, Value);
        return true;
    }
    template <typename TProperty, typename TValue>
    bool Niagara_GetSystemProp(UNiagaraSystem* System, const TCHAR* MemberName, TValue& OutValue)
    {
        TProperty* Prop = FindFProperty<TProperty>(System->GetClass(), MemberName);
        if (!Prop) { return false; }
        OutValue = Prop->GetPropertyValue_InContainer(System);
        return true;
    }

    // Parse a {"min":[x,y,z],"max":[x,y,z]} JSON object into an FBox. Returns
    // false (with OutError) if a corner is present but malformed; missing
    // corners default to the supplied previous values.
    bool ParseFixedBoundsObject(const TSharedPtr<FJsonObject>& Obj, const FBox& Prev, FBox& OutBox, FString& OutError)
    {
        FVector Min = Prev.Min;
        FVector Max = Prev.Max;
        auto ReadCorner = [&OutError](const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& OutVec) -> bool
        {
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            if (!O->TryGetArrayField(Key, Arr) || !Arr) { return true; } // absent: keep previous
            if (Arr->Num() != 3)
            {
                OutError = FString::Printf(TEXT("fixed_bounds.%s must be an array of 3 numbers"), Key);
                return false;
            }
            OutVec = FVector((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
            return true;
        };
        if (!ReadCorner(Obj, TEXT("min"), Min)) { return false; }
        if (!ReadCorner(Obj, TEXT("max"), Max)) { return false; }
        OutBox = FBox(Min, Max);
        return true;
    }

    void FixedBoundsToJson(const FBox& Box, TSharedPtr<FJsonObject>& Out)
    {
        TArray<TSharedPtr<FJsonValue>> MinArr = mcp_common::VectorToJsonArray(Box.Min);
        TArray<TSharedPtr<FJsonValue>> MaxArr = mcp_common::VectorToJsonArray(Box.Max);
        TSharedPtr<FJsonObject> BoundsObj = MakeShared<FJsonObject>();
        BoundsObj->SetArrayField(TEXT("min"), MinArr);
        BoundsObj->SetArrayField(TEXT("max"), MaxArr);
        Out->SetObjectField(TEXT("fixed_bounds"), BoundsObj);
        Out->SetBoolField(TEXT("fixed_bounds_valid"), Box.IsValid != 0);
    }

    // Reads the render/system flags and fixed bounds back into Data; a missing flag goes to OutMissing if given.
    void Niagara_AddSystemFlags(UNiagaraSystem* System, TSharedPtr<FJsonObject>& Data, TArray<FString>* OutMissing)
    {
        auto AddBool = [&](const TCHAR* Member, const TCHAR* JsonKey)
        {
            bool bValue = false;
            if (Niagara_GetSystemProp<FBoolProperty>(System, Member, bValue)) { Data->SetBoolField(JsonKey, bValue); }
            else if (OutMissing) { OutMissing->Add(Member); }
        };
        auto AddInt = [&](const TCHAR* Member, const TCHAR* JsonKey)
        {
            int32 Value = 0;
            if (Niagara_GetSystemProp<FIntProperty>(System, Member, Value)) { Data->SetNumberField(JsonKey, Value); }
            else if (OutMissing) { OutMissing->Add(Member); }
        };
        auto AddFloat = [&](const TCHAR* Member, const TCHAR* JsonKey)
        {
            float Value = 0.f;
            if (Niagara_GetSystemProp<FFloatProperty>(System, Member, Value)) { Data->SetNumberField(JsonKey, Value); }
            else if (OutMissing) { OutMissing->Add(Member); }
        };
        AddBool(TEXT("bCastShadow"), TEXT("cast_shadow"));
        AddBool(TEXT("bOverrideCastShadow"), TEXT("override_cast_shadow"));
        AddBool(TEXT("bReceivesDecals"), TEXT("receives_decals"));
        AddBool(TEXT("bOverrideReceivesDecals"), TEXT("override_receives_decals"));
        AddBool(TEXT("bRenderCustomDepth"), TEXT("render_custom_depth"));
        AddBool(TEXT("bOverrideRenderCustomDepth"), TEXT("override_render_custom_depth"));
        AddInt(TEXT("CustomDepthStencilValue"), TEXT("custom_depth_stencil_value"));
        AddBool(TEXT("bOverrideCustomDepthStencilValue"), TEXT("override_custom_depth_stencil_value"));
        AddInt(TEXT("TranslucencySortPriority"), TEXT("translucency_sort_priority"));
        AddBool(TEXT("bOverrideTranslucencySortPriority"), TEXT("override_translucency_sort_priority"));
        AddFloat(TEXT("TranslucencySortDistanceOffset"), TEXT("translucency_sort_distance_offset"));
        AddBool(TEXT("bOverrideTranslucencySortDistanceOffset"), TEXT("override_translucency_sort_distance_offset"));
        AddBool(TEXT("bSupportLargeWorldCoordinates"), TEXT("support_large_world_coordinates"));
        AddBool(TEXT("bRequireCurrentFrameData"), TEXT("require_current_frame_data"));
        AddBool(TEXT("bFixedBounds"), TEXT("b_fixed_bounds"));
        FixedBoundsToJson(System->GetFixedBounds(), Data);
    }
}

// MCP:set_niagara_system_properties - Set render/system flags on a UNiagaraSystem asset via raw reflection (no PostEditChangeProperty -> avoids the compile/scalability pipeline). Only the supplied fields are applied; each render override value also sets its bOverride* companion so it actually takes effect on spawned components. Persists via SavePackage and is reopen-clean (no recompile). Idempotent.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param cast_shadow - Optional bool: default CastShadow for spawned components (sets bCastShadow + bOverrideCastShadow).
//   @param override_cast_shadow - Optional bool: explicitly set bOverrideCastShadow (auto-set when cast_shadow is given).
//   @param receives_decals - Optional bool: default ReceivesDecals (sets bReceivesDecals + bOverrideReceivesDecals).
//   @param override_receives_decals - Optional bool: explicitly set bOverrideReceivesDecals.
//   @param render_custom_depth - Optional bool: default RenderCustomDepth (sets bRenderCustomDepth + bOverrideRenderCustomDepth).
//   @param override_render_custom_depth - Optional bool: explicitly set bOverrideRenderCustomDepth.
//   @param custom_depth_stencil_value - Optional int: default CustomDepthStencilValue (sets CustomDepthStencilValue + bOverrideCustomDepthStencilValue).
//   @param override_custom_depth_stencil_value - Optional bool: explicitly set bOverrideCustomDepthStencilValue.
//   @param translucency_sort_priority - Optional int: default TranslucencySortPriority (sets it + bOverrideTranslucencySortPriority).
//   @param override_translucency_sort_priority - Optional bool: explicitly set bOverrideTranslucencySortPriority.
//   @param translucency_sort_distance_offset - Optional float: default TranslucencySortDistanceOffset (sets it + bOverrideTranslucencySortDistanceOffset).
//   @param override_translucency_sort_distance_offset - Optional bool: explicitly set bOverrideTranslucencySortDistanceOffset.
//   @param support_large_world_coordinates - Optional bool: bSupportLargeWorldCoordinates.
//   @param require_current_frame_data - Optional bool: bRequireCurrentFrameData.
//   @param b_fixed_bounds - Optional bool: bFixedBounds (enables the system FixedBounds box).
//   @param fixed_bounds - Optional object {min:[x,y,z], max:[x,y,z]}: the system FixedBounds FBox. Partial corners keep the current value.
//   @returns { system_path, applied_count, override_applied:[...], <read-back of every field>, fixed_bounds:{min,max}, saved, save_error? }
TSharedPtr<FJsonObject> HandleSetNiagaraSystemProperties(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemProperties", "Set Niagara System Properties (MCP)"));
    System->Modify();

    int32 AppliedCount = 0;
    TArray<TSharedPtr<FJsonValue>> OverrideApplied; // names of bOverride* companions auto-set
    TArray<FString> MissingProps;                   // honest layout-drift report

    // A field written counts as applied; a member the system lacks is reported.
    auto Track = [&](bool bWritten, const TCHAR* Member) -> void
    {
        if (bWritten) { ++AppliedCount; }
        else { MissingProps.Add(Member); }
    };
    // Auto-set a render value's bOverride* companion so the override actually takes effect,
    // unless the caller explicitly provided it (handled in the explicit pass below).
    auto ApplyOverrideCompanion = [&](const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        if (!Params->HasField(OverrideJsonKey))
        {
            if (Niagara_SetSystemProp<FBoolProperty>(System, OverrideMember, true)) { OverrideApplied.Add(MakeShared<FJsonValueString>(OverrideMember)); }
            else { MissingProps.Add(OverrideMember); }
        }
    };
    // Each render value field: set the value AND its bOverride* companion (unless
    // the caller explicitly passed the companion, which we honour as-is below).
    auto ApplyBool = [&](const TCHAR* JsonKey, const TCHAR* Member) -> void
    {
        bool bVal = false;
        if (Params->TryGetBoolField(JsonKey, bVal))
        {
            Track(Niagara_SetSystemProp<FBoolProperty>(System, Member, bVal), Member);
        }
    };
    // Render value + companion override.
    auto ApplyBoolWithOverride = [&](const TCHAR* JsonKey, const TCHAR* Member,
                                     const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        bool bVal = false;
        if (Params->TryGetBoolField(JsonKey, bVal))
        {
            Track(Niagara_SetSystemProp<FBoolProperty>(System, Member, bVal), Member);
            ApplyOverrideCompanion(OverrideJsonKey, OverrideMember);
        }
    };
    auto ApplyIntWithOverride = [&](const TCHAR* JsonKey, const TCHAR* Member,
                                    const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        int32 Val = 0;
        if (Params->TryGetNumberField(JsonKey, Val))
        {
            Track(Niagara_SetSystemProp<FIntProperty>(System, Member, Val), Member);
            ApplyOverrideCompanion(OverrideJsonKey, OverrideMember);
        }
    };
    auto ApplyFloatWithOverride = [&](const TCHAR* JsonKey, const TCHAR* Member,
                                      const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        double Val = 0.0;
        if (Params->TryGetNumberField(JsonKey, Val))
        {
            Track(Niagara_SetSystemProp<FFloatProperty>(System, Member, static_cast<float>(Val)), Member);
            ApplyOverrideCompanion(OverrideJsonKey, OverrideMember);
        }
    };

    // Render overrides (value + auto-companion).
    ApplyBoolWithOverride(TEXT("cast_shadow"), TEXT("bCastShadow"), TEXT("override_cast_shadow"), TEXT("bOverrideCastShadow"));
    ApplyBoolWithOverride(TEXT("receives_decals"), TEXT("bReceivesDecals"), TEXT("override_receives_decals"), TEXT("bOverrideReceivesDecals"));
    ApplyBoolWithOverride(TEXT("render_custom_depth"), TEXT("bRenderCustomDepth"), TEXT("override_render_custom_depth"), TEXT("bOverrideRenderCustomDepth"));
    ApplyIntWithOverride(TEXT("custom_depth_stencil_value"), TEXT("CustomDepthStencilValue"), TEXT("override_custom_depth_stencil_value"), TEXT("bOverrideCustomDepthStencilValue"));
    ApplyIntWithOverride(TEXT("translucency_sort_priority"), TEXT("TranslucencySortPriority"), TEXT("override_translucency_sort_priority"), TEXT("bOverrideTranslucencySortPriority"));
    ApplyFloatWithOverride(TEXT("translucency_sort_distance_offset"), TEXT("TranslucencySortDistanceOffset"), TEXT("override_translucency_sort_distance_offset"), TEXT("bOverrideTranslucencySortDistanceOffset"));

    // Explicit bOverride* companions (caller-supplied; override the auto behaviour above).
    ApplyBool(TEXT("override_cast_shadow"), TEXT("bOverrideCastShadow"));
    ApplyBool(TEXT("override_receives_decals"), TEXT("bOverrideReceivesDecals"));
    ApplyBool(TEXT("override_render_custom_depth"), TEXT("bOverrideRenderCustomDepth"));
    ApplyBool(TEXT("override_custom_depth_stencil_value"), TEXT("bOverrideCustomDepthStencilValue"));
    ApplyBool(TEXT("override_translucency_sort_priority"), TEXT("bOverrideTranslucencySortPriority"));
    ApplyBool(TEXT("override_translucency_sort_distance_offset"), TEXT("bOverrideTranslucencySortDistanceOffset"));

    // System-wide toggles (no companion).
    ApplyBool(TEXT("support_large_world_coordinates"), TEXT("bSupportLargeWorldCoordinates"));
    ApplyBool(TEXT("require_current_frame_data"), TEXT("bRequireCurrentFrameData"));
    ApplyBool(TEXT("b_fixed_bounds"), TEXT("bFixedBounds"));

    // FixedBounds FBox.
    const TSharedPtr<FJsonObject>* FixedBoundsObj = nullptr;
    if (Params->TryGetObjectField(TEXT("fixed_bounds"), FixedBoundsObj) && FixedBoundsObj && FixedBoundsObj->IsValid())
    {
        FStructProperty* BoxProp = FindFProperty<FStructProperty>(System->GetClass(), TEXT("FixedBounds"));
        if (!BoxProp)
        {
            MissingProps.Add(TEXT("FixedBounds"));
        }
        else
        {
            const FBox Prev = System->GetFixedBounds();
            FBox NewBox;
            FString ParseError;
            if (!ParseFixedBoundsObject(*FixedBoundsObj, Prev, NewBox, ParseError))
            {
                return mcp_common::MakeErrorReply(ParseError);
            }
            FBox* BoxPtr = BoxProp->ContainerPtrToValuePtr<FBox>(System);
            *BoxPtr = NewBox;
            ++AppliedCount;
        }
    }

    if (MissingProps.Num() > 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not locate UNiagaraSystem properties (engine layout changed?): %s"),
                *FString::Join(MissingProps, TEXT(", "))));
    }

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Read everything back so the caller gets the authoritative post-set state.
    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("applied_count"), AppliedCount);
    Data->SetArrayField(TEXT("override_applied"), OverrideApplied);
    Niagara_AddSystemFlags(System, Data, nullptr);

    if (!bSaved && !SaveError.IsEmpty())
    {
        Data->SetStringField(TEXT("save_error"), SaveError);
    }
    UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_niagara_system_properties: %s applied_count=%d saved=%d"),
        *System->GetPathName(), AppliedCount, bSaved ? 1 : 0);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_properties"));
}

// MCP:get_niagara_system_properties - Read the render/system flags + fixed bounds of a UNiagaraSystem asset via reflection (GetPropertyValue_InContainer + GetFixedBounds). Read-only; no recompile.
//   @param system_path - Asset path of the Niagara System.
//   @returns { system_path, cast_shadow, override_cast_shadow, receives_decals, override_receives_decals, render_custom_depth, override_render_custom_depth, custom_depth_stencil_value, override_custom_depth_stencil_value, translucency_sort_priority, override_translucency_sort_priority, translucency_sort_distance_offset, override_translucency_sort_distance_offset, support_large_world_coordinates, require_current_frame_data, b_fixed_bounds, fixed_bounds:{min,max}, fixed_bounds_valid }
TSharedPtr<FJsonObject> HandleGetNiagaraSystemProperties(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    TArray<FString> MissingProps;
    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Niagara_AddSystemFlags(System, Data, &MissingProps);

    if (MissingProps.Num() > 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not locate UNiagaraSystem properties (engine layout changed?): %s"),
                *FString::Join(MissingProps, TEXT(", "))));
    }
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Renderer-class introspection
// ===========================================================================

namespace
{
    // { count, classes } of the concrete subclasses of BaseClass, deprecated ones (kept only for loading) skipped.
    TSharedPtr<FJsonObject> Niagara_ListConcreteSubclasses(UClass* BaseClass)
    {
        TArray<TSharedPtr<FJsonValue>> ClassesArray;

        for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
        {
            UClass* Class = *ClassIt;
            if (!Class) { continue; }
            if (!Class->IsChildOf(BaseClass)) { continue; }
            if (Class->HasAnyClassFlags(CLASS_Abstract)) { continue; }
            if (Class == BaseClass) { continue; }
            if (Class->HasAnyClassFlags(CLASS_Deprecated)) { continue; }

            TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
            ClassObj->SetStringField(TEXT("class_name"), Class->GetName());
            ClassObj->SetStringField(TEXT("class_path"), Class->GetPathName());
            ClassObj->SetStringField(TEXT("display_name"), Class->GetDisplayNameText().ToString());
            ClassObj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
            ClassObj->SetNumberField(TEXT("class_flags"), static_cast<int64>(Class->ClassFlags));
            ClassesArray.Add(MakeShared<FJsonValueObject>(ClassObj));
        }

        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetNumberField(TEXT("count"), ClassesArray.Num());
        Data->SetArrayField(TEXT("classes"), ClassesArray);
        return Data;
    }
}

// MCP:list_niagara_renderer_classes - Enumerate every concrete UNiagaraRendererProperties subclass registered in this process via TObjectIterator. Skips Abstract (those are base classes only). Used to discover what renderer_class strings are valid for add_niagara_renderer.
//   @returns { count: int, classes: [{class_name, class_path, display_name, abstract, class_flags}] }
TSharedPtr<FJsonObject> HandleListNiagaraRendererClasses(
    const TSharedPtr<FJsonObject>& Params)
{
    return mcp_common::MakeOkReply(Niagara_ListConcreteSubclasses(UNiagaraRendererProperties::StaticClass()));
}

// MCP:list_niagara_data_interface_classes - Enumerate every concrete UNiagaraDataInterface subclass registered in this process via TObjectIterator. Skips Abstract (those are base classes only). Used to discover what data-interface class strings are valid for add_niagara_data_interface.
//   @returns { count: int, classes: [{class_name, class_path, display_name, abstract, class_flags}] }
TSharedPtr<FJsonObject> HandleListNiagaraDataInterfaceClasses(
    const TSharedPtr<FJsonObject>& Params)
{
    return mcp_common::MakeOkReply(Niagara_ListConcreteSubclasses(UNiagaraDataInterface::StaticClass()));
}

// MCP:get_niagara_renderer_info - Dump the full property set of a specific renderer instance owned by a system emitter. Resolves the system via path, the emitter via name (or handle_index), and the renderer via index — then runs FJsonObjectConverter::UStructToJsonObject on the renderer to produce a flat property map. get_niagara_emitter_renderers lists every renderer; this cmd returns the full UPROPERTY surface of one renderer.
//   @param system_path - Content path to the UNiagaraSystem asset.
//   @param emitter_name - Name of the emitter handle (preferred) or its FName.
//   @param handle_index - Optional fallback when multiple emitters share a name (0-based).
//   @param renderer_index - 0-based index into FVersionedNiagaraEmitterData::RendererProperties.
//   @returns { system_path, emitter_name, renderer_index, renderer_class, renderer_class_path, properties: {...} }
TSharedPtr<FJsonObject> HandleGetNiagaraRendererInfo(
    const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("system_path is required"));
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("emitter_name"), EmitterName)) { return Missing; }

    int32 HandleIndex = 0;
    Params->TryGetNumberField(TEXT("handle_index"), HandleIndex);
    int32 RendererIndex = 0;
    if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex))
    {
        return mcp_common::MakeErrorReply(TEXT("renderer_index is required (0-based)"));
    }

    UNiagaraSystem* System = LoadObject<UNiagaraSystem>(nullptr, *SystemPath);
    if (!System)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Cannot load UNiagaraSystem: %s"), *SystemPath));
    }

    // Resolve the emitter (same pattern as the renderer commands: by name first, then handle_index).
    FNiagaraEmitterHandle* Handle = nullptr;
    if (!EmitterName.IsEmpty())
    {
        Handle = FindEmitterHandleByName(System, EmitterName);
    }
    if (!Handle)
    {
        // GetEmitterHandles() returns const TArray& — cast away const to share
        // the same FNiagaraEmitterHandle* across the name-first / index-fallback branches.
        const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();
        if (Handles.IsValidIndex(HandleIndex))
        {
            Handle = const_cast<FNiagaraEmitterHandle*>(&Handles[HandleIndex]);
        }
    }
    if (!Handle)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Emitter '%s' not found in system '%s' (handle_index=%d)"),
                *EmitterName, *SystemPath, HandleIndex));
    }

    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(NiagaraNoEmitterDataTemplate);
    }

    const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
    if (!Renderers.IsValidIndex(RendererIndex) || !Renderers[RendererIndex])
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Renderer index %d out of range (%d renderers on emitter '%s')"),
                RendererIndex, Renderers.Num(), *EmitterName));
    }

    UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
    UClass* RenderClass = Renderer->GetClass();

    // Dump the full UPROPERTY surface via FJsonObjectConverter.
    TSharedPtr<FJsonObject> PropertiesJson = MakeShared<FJsonObject>();
    FJsonObjectConverter::UStructToJsonObject(
        RenderClass, Renderer, PropertiesJson.ToSharedRef(), /*CheckFlags*/ 0, /*SkipFlags*/ 0);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("system_path"), SystemPath);
    Data->SetStringField(TEXT("emitter_name"), EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("renderer_class"), RenderClass->GetName());
    Data->SetStringField(TEXT("renderer_class_path"), RenderClass->GetPathName());
    Data->SetBoolField(TEXT("renderer_enabled"), Renderer->bIsEnabled);
    Data->SetObjectField(TEXT("properties"), PropertiesJson);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Typed setter on an existing
// UNiagaraDecalRendererProperties instance.
//
// Scalar renderer properties go through the reflective
// set_niagara_renderer_properties route. set_niagara_decal_renderer_material is a
// typed setter because it resolves a UMaterialInterface by asset path (object-ref),
// which the reflective route cannot LoadObject. It uses the existing
// LoadNiagaraSystem + SaveModifiedNiagaraSystem helpers and the ResolveRendererAtIndex
// template below. Renderer index is 0-based; matches the array order returned by
// FVersionedNiagaraEmitterData::GetRenderers().
// ---------------------------------------------------------------------------

namespace
{
    // Resolve + return the specific renderer at Index in the named emitter,
    // casting to the expected TSubclassOf. On any failure (system missing,
    // emitter missing, index out of range, type mismatch) returns nullptr
    // and writes a precise error to OutError.
    template <typename TRenderer>
    TRenderer* ResolveRendererAtIndex(
        const TSharedPtr<FJsonObject>& Params,
        const TCHAR* ExpectedClass,
        TRenderer*& OutTyped,
        FString& OutError)
    {
        OutTyped = nullptr;
        FString SystemPath;
        FString EmitterName;
        if (!ExtractNiagaraSystemPath(Params, SystemPath))
        {
            OutError = TEXT("Missing required parameter: system_path");
            return nullptr;
        }
        if (!Params->TryGetStringField(TEXT("emitter_name"), EmitterName) || EmitterName.IsEmpty())
        {
            OutError = TEXT("Missing required parameter: emitter_name");
            return nullptr;
        }
        double IndexD = 0.0;
        if (!Params->TryGetNumberField(TEXT("renderer_index"), IndexD))
        {
            OutError = TEXT("Missing required parameter: renderer_index");
            return nullptr;
        }
        const int32 RendererIndex = static_cast<int32>(IndexD);
        if (RendererIndex < 0)
        {
            OutError = FString::Printf(TEXT("renderer_index must be >= 0 (got %d)"), RendererIndex);
            return nullptr;
        }

        UNiagaraSystem* System = nullptr;
        FVersionedNiagaraEmitterData* EmitterData =
            Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, System, OutError);
        if (!EmitterData)
        {
            return nullptr;
        }
        const TArray<UNiagaraRendererProperties*>& Renderers = EmitterData->GetRenderers();
        if (RendererIndex >= Renderers.Num())
        {
            OutError = FString::Printf(TEXT("renderer_index %d out of range (emitter has %d renderers)"),
                RendererIndex, Renderers.Num());
            return nullptr;
        }
        UNiagaraRendererProperties* Renderer = Renderers[RendererIndex];
        if (!Renderer || !Renderer->IsA<TRenderer>())
        {
            OutError = FString::Printf(
                TEXT("Renderer at index %d is not a %s (actual: %s)"),
                RendererIndex, ExpectedClass,
                Renderer ? *Renderer->GetClass()->GetName() : TEXT("null"));
            return nullptr;
        }
        OutTyped = Cast<TRenderer>(Renderer);
        return OutTyped;
    }
}

// MCP:set_niagara_decal_renderer_material - Assign a material to the Decal renderer at renderer_index of one emitter in a Niagara System asset and save the system; a failed save returns success false. Returns the previous and new material paths and changed. For sprite, ribbon or mesh renderers use set_niagara_renderer_material.
//   @param system_path - Niagara System asset path, e.g. "/Game/Niagara/NS_Example"; the call fails without it.
//   @param emitter_name - Display name of the emitter handle that owns the renderer (case-insensitive); the call fails without it.
//   @param renderer_index - 0-based index in the renderer list of the emitter, in get_niagara_emitter_renderers order; that renderer must be a Decal renderer. The call fails without it.
//   @param material_path - Object path of the UMaterialInterface to assign, e.g. "/Game/Decals/M_Blood.M_Blood".
//   @returns { system_path, emitter_name, renderer_index, previous_material_path, new_material_path, changed, saved }
TSharedPtr<FJsonObject> HandleSetNiagaraDecalRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString Error;
    UNiagaraDecalRendererProperties* Renderer = nullptr;
    // TRenderer is deduced from Renderer: an explicit <...> would hide the call, and the param reads in
    // ResolveRendererAtIndex, from the catalog generator.
    if (!ResolveRendererAtIndex(Params, TEXT("UNiagaraDecalRendererProperties"), Renderer, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FString MaterialPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("material_path"), MaterialPath)) { return Missing; }
    UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
    if (!Material)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("UMaterialInterface not found: %s"), *MaterialPath));
    }
    const FString PreviousPath = Renderer->Material ? Renderer->Material->GetPathName() : FString();

    Renderer->Modify();
    Renderer->Material = Material;
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(mcp_common::ResolveAsset<UNiagaraSystem>(Params->GetStringField(TEXT("system_path"))), SaveError);

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("system_path"), Params->GetStringField(TEXT("system_path")));
    ResultObj->SetStringField(TEXT("emitter_name"), Params->GetStringField(TEXT("emitter_name")));
    ResultObj->SetNumberField(TEXT("renderer_index"), Params->GetNumberField(TEXT("renderer_index")));
    ResultObj->SetStringField(TEXT("previous_material_path"), PreviousPath);
    ResultObj->SetStringField(TEXT("new_material_path"), Material->GetPathName());
    ResultObj->SetBoolField(TEXT("changed"), PreviousPath != Material->GetPathName());
    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_niagara_decal_renderer_material"));
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_niagara_data_interface"), &HandleAddNiagaraDataInterface },
    { TEXT("add_niagara_decal_renderer"), &HandleAddNiagaraDecalRenderer },
    { TEXT("add_niagara_emitter_from_template"), &HandleAddNiagaraEmitterFromTemplate },
    { TEXT("add_niagara_emitter_handle"), &HandleAddNiagaraEmitterHandle },
    { TEXT("add_niagara_event_handler"), &HandleAddNiagaraEventHandler },
    { TEXT("add_niagara_light_renderer"), &HandleAddNiagaraLightRenderer },
    { TEXT("add_niagara_module_to_graph"), &HandleAddNiagaraModuleToGraph },
    { TEXT("add_niagara_module_to_stack"), &HandleAddNiagaraModuleToStack },
    { TEXT("add_niagara_renderer"), &HandleAddNiagaraRenderer },
    { TEXT("add_niagara_simulation_stage"), &HandleAddNiagaraSimulationStage },
    { TEXT("create_niagara_system"), &HandleCreateNiagaraSystem },
    { TEXT("duplicate_niagara_emitter_handle"), &HandleDuplicateNiagaraEmitterHandle },
    { TEXT("duplicate_niagara_module_in_stack"), &HandleDuplicateNiagaraModuleInStack },
    { TEXT("get_niagara_component_matrix"), &HandleGetNiagaraComponentMatrix },
    { TEXT("get_niagara_data_interface_info"), &HandleGetNiagaraDataInterfaceInfo },
    { TEXT("get_niagara_data_interfaces"), &HandleGetNiagaraDataInterfaces },
    { TEXT("get_niagara_effect_type"), &HandleGetNiagaraEffectType },
    { TEXT("get_niagara_emitter_data_interfaces"), &HandleGetNiagaraEmitterDataInterfaces },
    { TEXT("get_niagara_emitter_handle_info"), &HandleGetNiagaraEmitterHandleInfo },
    { TEXT("get_niagara_emitter_handles"), &HandleGetNiagaraEmitterHandles },
    { TEXT("get_niagara_emitter_renderers"), &HandleGetNiagaraEmitterRenderers },
    { TEXT("get_niagara_emitter_runtime_summary"), &HandleGetNiagaraEmitterRuntimeSummary },
    { TEXT("get_niagara_emitter_sim_target"), &HandleGetNiagaraEmitterSimTarget },
    { TEXT("get_niagara_event_handlers"), &HandleGetNiagaraEventHandlers },
    { TEXT("get_niagara_module_info"), &HandleGetNiagaraModuleInfo },
    { TEXT("get_niagara_module_input"), &HandleGetNiagaraModuleInput },
    { TEXT("get_niagara_module_stack"), &HandleGetNiagaraModuleStack },
    { TEXT("get_niagara_renderer_info"), &HandleGetNiagaraRendererInfo },
    { TEXT("get_niagara_scalability_settings"), &HandleGetNiagaraScalabilitySettings },
    { TEXT("get_niagara_simulation_stages"), &HandleGetNiagaraSimulationStages },
    { TEXT("get_niagara_system_info"), &HandleGetNiagaraSystemInfo },
    { TEXT("get_niagara_system_properties"), &HandleGetNiagaraSystemProperties },
    { TEXT("get_niagara_system_warmup_time"), &HandleGetNiagaraSystemWarmupTime },
    { TEXT("get_niagara_user_parameter"), &HandleGetNiagaraUserParameter },
    { TEXT("list_niagara_data_interface_classes"), &HandleListNiagaraDataInterfaceClasses },
    { TEXT("list_niagara_modules"), &HandleListNiagaraModules },
    { TEXT("list_niagara_renderer_classes"), &HandleListNiagaraRendererClasses },
    { TEXT("list_niagara_simulation_stages"), &HandleListNiagaraSimulationStages },
    { TEXT("move_niagara_simulation_stage"), &HandleMoveNiagaraSimulationStage },
    { TEXT("remove_niagara_data_interface"), &HandleRemoveNiagaraDataInterface },
    { TEXT("remove_niagara_emitter_handle"), &HandleRemoveNiagaraEmitterHandle },
    { TEXT("remove_niagara_event_handler"), &HandleRemoveNiagaraEventHandler },
    { TEXT("remove_niagara_module_from_stack"), &HandleRemoveNiagaraModuleFromStack },
    { TEXT("remove_niagara_renderer"), &HandleRemoveNiagaraRenderer },
    { TEXT("remove_niagara_simulation_stage"), &HandleRemoveNiagaraSimulationStage },
    { TEXT("rename_niagara_emitter_handle"), &HandleRenameNiagaraEmitterHandle },
    { TEXT("reorder_niagara_modules"), &HandleReorderNiagaraModules },
    { TEXT("set_niagara_component_bool"), &HandleSetNiagaraComponentBool },
    { TEXT("set_niagara_component_color"), &HandleSetNiagaraComponentColor },
    { TEXT("set_niagara_component_float"), &HandleSetNiagaraComponentFloat },
    { TEXT("set_niagara_component_int"), &HandleSetNiagaraComponentInt },
    { TEXT("set_niagara_component_matrix"), &HandleSetNiagaraComponentMatrix },
    { TEXT("set_niagara_component_object"), &HandleSetNiagaraComponentObject },
    { TEXT("set_niagara_component_position"), &HandleSetNiagaraComponentPosition },
    { TEXT("set_niagara_component_quat"), &HandleSetNiagaraComponentQuat },
    { TEXT("set_niagara_component_vec2"), &HandleSetNiagaraComponentVec2 },
    { TEXT("set_niagara_component_vec4"), &HandleSetNiagaraComponentVec4 },
    { TEXT("set_niagara_component_vector"), &HandleSetNiagaraComponentVector },
    { TEXT("set_niagara_data_interface_array"), &HandleSetNiagaraDataInterfaceArray },
    { TEXT("set_niagara_data_interface_asset"), &HandleSetNiagaraDataInterfaceAsset },
    { TEXT("set_niagara_data_interface_curve"), &HandleSetNiagaraDataInterfaceCurve },
    { TEXT("set_niagara_data_interface_default"), &HandleSetNiagaraDataInterfaceDefault },
    { TEXT("set_niagara_decal_renderer_material"), &HandleSetNiagaraDecalRendererMaterial },
    { TEXT("set_niagara_effect_type"), &HandleSetNiagaraEffectType },
    { TEXT("set_niagara_emitter_allocation_mode"), &HandleSetNiagaraEmitterAllocationMode },
    { TEXT("set_niagara_emitter_bounds"), &HandleSetNiagaraEmitterBounds },
    { TEXT("set_niagara_emitter_enabled"), &HandleSetNiagaraEmitterEnabled },
    { TEXT("set_niagara_emitter_local_space"), &HandleSetNiagaraEmitterLocalSpace },
    { TEXT("set_niagara_emitter_max_gpu_particles"), &HandleSetNiagaraEmitterMaxGPUParticles },
    { TEXT("set_niagara_emitter_requires_persistent_ids"), &HandleSetNiagaraEmitterRequiresPersistentIds },
    { TEXT("set_niagara_emitter_sim_target"), &HandleSetNiagaraEmitterSimTarget },
    { TEXT("set_niagara_emitter_template_description"), &HandleSetNiagaraEmitterTemplateDescription },
    { TEXT("set_niagara_mesh_renderer_mesh"), &HandleSetNiagaraMeshRendererMesh },
    { TEXT("set_niagara_module_enabled"), &HandleSetNiagaraModuleEnabled },
    { TEXT("set_niagara_module_input"), &HandleSetNiagaraModuleInput },
    { TEXT("set_niagara_renderer_material"), &HandleSetNiagaraRendererMaterial },
    { TEXT("set_niagara_renderer_properties"), &HandleSetNiagaraRendererProperties },
    { TEXT("set_niagara_simulation_stage_properties"), &HandleSetNiagaraSimulationStageProperties },
    { TEXT("set_niagara_system_deterministic"), &HandleSetNiagaraSystemDeterministic },
    { TEXT("set_niagara_system_fixed_bounds"), &HandleSetNiagaraSystemFixedBounds },
    { TEXT("set_niagara_system_properties"), &HandleSetNiagaraSystemProperties },
    { TEXT("set_niagara_system_warmup_tick_count"), &HandleSetNiagaraSystemWarmupTickCount },
    { TEXT("set_niagara_system_warmup_tick_delta"), &HandleSetNiagaraSystemWarmupTickDelta },
    { TEXT("set_niagara_system_warmup_time"), &HandleSetNiagaraSystemWarmupTime },
    { TEXT("set_niagara_user_parameter"), &HandleSetNiagaraUserParameter },
    { TEXT("spawn_niagara_component"), &HandleSpawnNiagaraComponent },
};
} // namespace UEAgentNiagaraCommands
