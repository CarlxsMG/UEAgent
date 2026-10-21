// Copyright (c) 2026 CarlxsMG.

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
#include "NiagaraActor.h"                                   // ANiagaraActor (spawn_niagara_component)

// Niagara data-interface content authoring (curve keys + array values, via reflection)
#include "Curves/RichCurve.h"                               // FRichCurve (curve DIs: Curve / Red|Green|Blue|AlphaCurve / X|Y|ZCurve)
#include "NiagaraDataInterfaceCurveBase.h"                  // UNiagaraDataInterfaceCurveBase::UpdateLUT / UpdateTimeRanges
#include "UObject/UnrealType.h"                             // FStructProperty / FArrayProperty / FNumericProperty / FScriptArrayHelper
#include "UObject/TextProperty.h"                           // FTextProperty::Identical_Implementation (template description no-op)

// Niagara editor
#include "NiagaraSystemFactoryNew.h"                        // UNiagaraSystemFactoryNew::InitializeSystem
#include "ActorFactoryNiagara.h"                            // UActorFactoryNiagara (the drag-a-system-into-the-level factory)

// Niagara simulation advanced
#include "NiagaraSimulationStageBase.h"                     // UNiagaraSimulationStageGeneric
#include "NiagaraScript.h"                                  // UNiagaraScript (sim stage needs a Script)
#include "NiagaraEffectType.h"                              // UNiagaraEffectType

// Emitter from template + module-to-stack
#include "NiagaraEditorUtilities.h"                         // FNiagaraEditorUtilities::AddEmitterToSystem
#include "NiagaraScriptSource.h"                            // UNiagaraScriptSource (NodeGraph)
#include "NiagaraGraph.h"                                   // UNiagaraGraph::FindOutputNode
#include "NiagaraNodeOutput.h"                              // UNiagaraNodeOutput
#include "NiagaraNodeInput.h"                               // UNiagaraNodeInput (data-interface inputs of a stack)
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
#include "Materials/MaterialInterface.h"                    // UMaterialInterface for set_niagara_renderer_material
// TObjectIterator<UClass> for list_niagara_renderer_classes.
#include "UObject/UObjectIterator.h"
#include "NiagaraMeshRendererMeshProperties.h"               // FNiagaraMeshRendererMeshProperties (.Mesh slot)
#include "NiagaraDataInterface.h"                            // UNiagaraDataInterface (base)
#include "Engine/StaticMesh.h"                               // UStaticMesh (mesh assignment)

// Engine / editor
#include "Editor.h"                                         // GEditor
#include "GameFramework/Actor.h"
#include "EngineUtils.h"                                    // TActorIterator
#include "ActorEditorUtils.h"                               // FActorEditorUtils::ValidateActorName
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

    // Whether the number param Key is present but not a JSON number (an absent or null one is not). Check it before
    // the param's TryGetNumberField: that read takes any string into an integer through LexFromString (text as 0) and
    // a bool as 0 or 1, and on the other types fails and leaves the default, so a mistyped value would otherwise be
    // written, or pick a target, as a number the caller never sent.
    bool Niagara_IsMistypedNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key)
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
        return Value.IsValid() && !Value->IsNull() && Value->Type != EJson::Number;
    }

    // The error reply for Niagara_IsMistypedNumber(Params, Key); null when the param is absent, null or a number.
    TSharedPtr<FJsonObject> Niagara_RefuseMistypedNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key)
    {
        if (!Niagara_IsMistypedNumber(Params, Key))
        {
            return nullptr;
        }
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("%s must be a JSON number; nothing was changed"), Key));
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
// MCP:create_niagara_system - Create an empty UNiagaraSystem asset at package_path/name and save it: its system spawn and update scripts are set up, but it has no emitters. Idempotent: a Niagara System already at that path is returned unchanged with created=false and already_exists=true, and nothing is saved; an asset of another class at that path is an error and nothing is written. Not an undo step. Next, add emitters with add_niagara_emitter_from_template, check the result with get_niagara_system_info or get_niagara_emitter_handles, and place it in a level with spawn_actor (type NiagaraActor). A failed save is an error, the new asset left in memory unsaved.
//   @param name - Asset name, e.g. 'NS_Sparks'; package_path/name must form a valid long package name.
//   @param package_path - Content folder as a long package path, e.g. '/Game/FX' (default /Game/Niagara); a trailing slash is ignored.
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
// MCP:get_niagara_system_info - Reads metadata for a UNiagaraSystem: emitter count, exposed user parameters, and its warmup and determinism settings (warmup_time in seconds, warmup_tick_count, warmup_tick_delta, needs_warmup, deterministic). Read-only; change the warmup with set_properties, determinism with set_properties.
//   @param system_path - Asset path of the Niagara System (e.g. /Game/Niagara/NS_Example or /Game/Niagara/NS_Example.NS_Example)
//   @returns { system_name: string, system_path: string, emitter_count: int, num_user_parameters: int, warmup_time: number, warmup_tick_count: int, warmup_tick_delta: number, needs_warmup: bool, deterministic: bool, user_parameters: [{ name, type }] }

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
    Data->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    Data->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    Data->SetBoolField(TEXT("needs_warmup"), System->NeedsWarmup());
    Data->SetBoolField(TEXT("deterministic"), System->NeedsDeterminism());

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

    // Reads Items, a JSON array of MinCount to MaxCount numbers, into Out[0..]; the slots past the count read keep
    // the value they had. False with OutError when the count is outside that range or an element is not a finite
    // number within the float range, so that a mistyped component is refused instead of being written as 0.
    bool ReadFloatComponents(const TArray<TSharedPtr<FJsonValue>>& Items, int32 MinCount, int32 MaxCount, float* Out,
        FString& OutError)
    {
        if (Items.Num() < MinCount || Items.Num() > MaxCount)
        {
            OutError = MinCount == MaxCount
                ? FString::Printf(TEXT("an array of %d numbers"), MinCount)
                : FString::Printf(TEXT("an array of %d to %d numbers"), MinCount, MaxCount);
            return false;
        }
        for (int32 Index = 0; Index < Items.Num(); ++Index)
        {
            const TSharedPtr<FJsonValue>& Item = Items[Index];
            const double Number = (Item.IsValid() && Item->Type == EJson::Number) ? Item->AsNumber() : 0.0;
            if (!Item.IsValid() || Item->Type != EJson::Number || !FMath::IsFinite(Number)
                || FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()))
            {
                OutError = FString::Printf(TEXT("an array of finite numbers within the float range (element %d is not)"), Index);
                return false;
            }
            Out[Index] = static_cast<float>(Number);
        }
        return true;
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
// MCP:set_niagara_user_parameter - Set the default value of one user parameter (the User. namespace) of a Niagara System asset in one undo step, adding the parameter when the system has none of that name and type; the asset is marked dirty, not saved (save_asset keeps it). type picks Float, Vector, Vec2, Vec4, Color, Bool or Int; without it a vector, vec2, vec4 or color key picks that type, else Float. A missing value, or one not of the JSON type the parameter takes, is an error with nothing written. A parameter already holding the value is a success with changed=false and value_set=false, nothing written. verified means the value reads back as sent. Read it with get_niagara_user_parameter; inverse: remove_niagara_user_parameter.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - User parameter name (without the "User." prefix).
//   @param type - Optional type token: Float|Vector|Vec2|Vec4|Color|Bool|Int (case-insensitive); when omitted a vector, vec2, vec4 or color key picks that type, else Float
//   @param value - The value of a Float (a number), Int (a whole number) or Bool (true or false) parameter; those types fail without it or with another JSON type
//   @param vector - [X, Y, Z], 3 numbers, the value of a Vector parameter (required for it; also picks type Vector when type is omitted)
//   @param vec2 - [X, Y], 2 numbers, the value of a Vec2 parameter (required for it)
//   @param vec4 - [X, Y, Z, W], 4 numbers, the value of a Vec4 parameter (required for it)
//   @param color - [R, G, B] or [R, G, B, A] linear color of a Color parameter, A 1 when omitted (required for it)
//   @returns { system_path: string, parameter_name: string, parameter_type: string, value_set: bool, changed: bool, value_read_back?: number|bool|[number...], verified: bool, note?: string }

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

    // The value is read and checked before anything changes: a missing value, or one of another JSON type than the
    // parameter's type takes, is an error with nothing written, where a lenient read would write 0, false or zeros.
    float FloatValue = 0.0f;
    int32 IntValue = 0;
    bool bBoolValue = false;
    float Components[4] = {0.f, 0.f, 0.f, 1.f};   // a Color sent as [R, G, B] keeps A = 1
    if (Canonical == TEXT("Float") || Canonical == TEXT("Int") || Canonical == TEXT("Bool"))
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("value"));
        if (!Value.IsValid() || Value->IsNull())
        {
            return mcp_common::MissingParameterReply(TEXT("value"));
        }
        FString ValueError;
        if (Canonical == TEXT("Bool"))
        {
            if (Value->Type == EJson::Boolean)
            {
                bBoolValue = Value->AsBool();
            }
            else
            {
                ValueError = TEXT("a JSON boolean (true or false)");
            }
        }
        else if (Value->Type != EJson::Number)
        {
            ValueError = TEXT("a JSON number");
        }
        else
        {
            const double Number = Value->AsNumber();
            if (Canonical == TEXT("Int"))
            {
                if (!FMath::IsFinite(Number) || Number != FMath::FloorToDouble(Number)
                    || Number < static_cast<double>(MIN_int32) || Number > static_cast<double>(MAX_int32))
                {
                    ValueError = TEXT("a whole number within the int32 range");
                }
                else
                {
                    IntValue = static_cast<int32>(Number);
                }
            }
            else if (!FMath::IsFinite(Number) || FMath::Abs(Number) > static_cast<double>(TNumericLimits<float>::Max()))
            {
                ValueError = TEXT("a finite number within the float range");
            }
            else
            {
                FloatValue = static_cast<float>(Number);
            }
        }
        if (!ValueError.IsEmpty())
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("value must be %s for a %s parameter. Nothing was written."), *ValueError, *Canonical));
        }
    }
    else
    {
        // Vector, Vec2, Vec4 and Color take an array of numbers under their own key.
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        bool bIsArray = false;
        const TCHAR* Key = TEXT("color");
        int32 MinCount = 3;
        int32 MaxCount = 4;
        if (Canonical == TEXT("Vector"))
        {
            Key = TEXT("vector");
            MaxCount = 3;
            bIsArray = Params->TryGetArrayField(TEXT("vector"), Items);
        }
        else if (Canonical == TEXT("Vec2"))
        {
            Key = TEXT("vec2");
            MinCount = MaxCount = 2;
            bIsArray = Params->TryGetArrayField(TEXT("vec2"), Items);
        }
        else if (Canonical == TEXT("Vec4"))
        {
            Key = TEXT("vec4");
            MinCount = MaxCount = 4;
            bIsArray = Params->TryGetArrayField(TEXT("vec4"), Items);
        }
        else
        {
            bIsArray = Params->TryGetArrayField(TEXT("color"), Items);
        }
        if (!Params->HasField(Key))
        {
            return mcp_common::MissingParameterReply(Key);
        }
        // Anything but an array reads as an empty one, which fails the count check with the shape expected.
        static const TArray<TSharedPtr<FJsonValue>> NoItems;
        FString ValueError;
        if (!ReadFloatComponents(bIsArray && Items ? *Items : NoItems, MinCount, MaxCount, Components, ValueError))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be %s for a %s parameter. Nothing was written."), Key, *ValueError, *Canonical));
        }
    }

    FNiagaraUserRedirectionParameterStore& ExposedParams = System->GetExposedParameters();
    const FNiagaraVariable Var(TypeDef, FName(*ParameterName));

    // Calls Fn with the value in the C++ type the store keeps for this parameter type. SetParameterValue<T>
    // asserts the variable's byte size matches sizeof(T); this mapping mirrors ResolveUserParamType so that holds.
    const FNiagaraBool NiagaraBoolValue(bBoolValue);
    auto WithTypedValue = [&](auto&& Fn) -> bool
    {
        if (Canonical == TEXT("Float"))  { return Fn(FloatValue); }
        if (Canonical == TEXT("Int"))    { return Fn(IntValue); }
        if (Canonical == TEXT("Bool"))   { return Fn(NiagaraBoolValue); }
        if (Canonical == TEXT("Vector")) { return Fn(FVector3f(Components[0], Components[1], Components[2])); }
        if (Canonical == TEXT("Vec2"))   { return Fn(FVector2f(Components[0], Components[1])); }
        if (Canonical == TEXT("Vec4"))   { return Fn(FVector4f(Components[0], Components[1], Components[2], Components[3])); }
        return Fn(FLinearColor(Components[0], Components[1], Components[2], Components[3]));
    };
    // Whether the store has this parameter, of this type, holding exactly these bytes (the user store redirects the
    // short name).
    auto Holds = [&ExposedParams, &Var](const auto& Value) -> bool
    {
        using FValueType = std::decay_t<decltype(Value)>;
        const int32 Offset = ExposedParams.IndexOf(Var);
        if (Offset == INDEX_NONE)
        {
            return false;
        }
        const FValueType Stored = ExposedParams.GetParameterValueFromOffset<FValueType>(Offset);
        return FMemory::Memcmp(&Stored, &Value, sizeof(FValueType)) == 0;
    };

    TSharedPtr<FJsonObject> Data = Niagara_StartParameterReply(System, ParameterName);
    Data->SetStringField(TEXT("parameter_type"), Canonical);

    // A parameter that already holds this value is a valid no-op: nothing is added, modified or dirtied.
    if (WithTypedValue(Holds))
    {
        Data->SetBoolField(TEXT("value_set"), false);
        Data->SetBoolField(TEXT("changed"), false);
        ReadUserParamValueBack(ExposedParams, Canonical, TypeDef, FName(*ParameterName), Data);
        Data->SetBoolField(TEXT("verified"), true);
        Data->SetStringField(TEXT("note"), TEXT("The parameter already holds this value; nothing changed"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraUserParameter", "Set Niagara User Parameter"));
    System->Modify();

    // AddParameter is idempotent w.r.t. the redirect table; it namespaces to User.
    ExposedParams.AddParameter(Var, /*bInitialize*/ true, /*bTriggerRebind*/ true);
    const bool bSet = WithTypedValue([&ExposedParams, &Var](const auto& Value) -> bool
    {
        return ExposedParams.SetParameterValue(Value, Var, /*bAdd*/ true);
    });

    System->MarkPackageDirty();

    // verified: the value reads back as the one sent.
    Data->SetBoolField(TEXT("value_set"), bSet);
    Data->SetBoolField(TEXT("changed"), bSet);
    ReadUserParamValueBack(ExposedParams, Canonical, TypeDef, FName(*ParameterName), Data);
    const bool bVerified = WithTypedValue(Holds);
    Data->SetBoolField(TEXT("verified"), bVerified);
    const bool bOk = bSet && bVerified;
    return mcp_common::MakeOutcomeReply(Data, bOk,
        bOk ? FString() : mcp_common::MeasuredOutcomeError(TEXT("set_niagara_user_parameter"), TEXT("verified")));
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
// Places an ANiagaraActor in the editor level through the editor's Niagara actor factory, the
// actor a drag of a Niagara System into the viewport makes: a saved, undoable actor whose root
// UNiagaraComponent runs the system. UNiagaraFunctionLibrary::SpawnSystemAtLocation, used before,
// makes no actor: it outers a TRANSIENT component to the level's WorldSettings, so nothing was
// saved and a label meant for the "owner" renamed the WorldSettings.
// The set_niagara_component_* tools find the component by the actor's internal name or label
// (FindNiagaraComponentByOwner), so the label is refused when another actor already answers to it.
// ---------------------------------------------------------------------------
// MCP:spawn_niagara_component - Place a Niagara System in the editor level as a new ANiagaraActor (the actor a drag of the system into the viewport makes, through the editor's Niagara actor factory) whose root UNiagaraComponent runs the system, at an optional location and rotation, for previews and set_niagara_component_* calls. For a Niagara component on an actor that already exists use add_component_to_actor (NiagaraComponent) and set its Asset with set_properties. actor_label labels the new actor (default: the system's name, made unique); a label that is "None" or blank, or that another editor-world actor already has as label or internal name, is an error and nothing is spawned, so set_niagara_component_* (actor_name = owner_actor_name or the label) reach this actor. A system the factory refuses (settings requiring an effect type) is an error too. One undo step; the level is marked dirty, not saved (save_level keeps it). Editor world only, PIE or not. location and rotation are read back.
//   @retired spawn_actor - type="NiagaraActor", name, location, rotation, properties={"NiagaraComponent.Asset":"/Game/FX/NS_..."}
//   @param system_path - Asset path of the Niagara System to place (e.g. /Game/MyGame/NS_Example); one that does not load is an error and nothing is spawned.
//   @param location - Optional world location [X, Y, Z]; the origin when omitted.
//   @param rotation - Optional world rotation, the object {"pitch":..,"yaw":..,"roll":..} or the array [pitch, yaw, roll] (index 2 is ROLL); zero when omitted.
//   @param actor_label - Optional editor label for the new actor; when omitted the factory labels it after the system, made unique. A label that is not a valid actor label ("None", blank), or that another actor of the editor world already has as its label or internal name, is an error and nothing is spawned.
//   @returns { system_path: string, owner_actor_name: string, owner_actor_label: string, actor_class: string, actor_path: string, component_name: string, is_registered: bool, is_active: bool, location: [number, number, number], rotation: { roll: number, pitch: number, yaw: number } }

TSharedPtr<FJsonObject> HandleSpawnNiagaraComponent(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    UWorld* World = nullptr;
    if (TSharedPtr<FJsonObject> NoWorld = mcp_common::RequireEditorWorld(World)) { return NoWorld; }
    ULevel* Level = World->GetCurrentLevel();
    if (!Level)
    {
        return mcp_common::MakeErrorReply(TEXT("The editor world has no current level to place the Niagara actor in; nothing was spawned."));
    }

    // The dispatch refuses a location or rotation of another shape before this runs (both are vector params).
    const FVector Location = mcp_common::ReadJsonVector(Params, TEXT("location"));
    FRotator Rotation = FRotator::ZeroRotator;
    if (Params->HasField(TEXT("rotation")))
    {
        Rotation = mcp_common::ReadJsonRotator(Params, TEXT("rotation"));
    }

    // The label is checked before anything exists: SetActorLabel only logs a label it rejects, and a label another
    // actor answers to would send the set_niagara_component_* tools to that actor instead of this one.
    FString DesiredLabel;
    const bool bLabelled = Params->TryGetStringField(TEXT("actor_label"), DesiredLabel);
    DesiredLabel.TrimStartAndEndInline();
    if (bLabelled)
    {
        FText LabelError;
        if (!FActorEditorUtils::ValidateActorName(FText::FromString(DesiredLabel), LabelError))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("actor_label '%s' is not a valid actor label: %s; nothing was spawned."), *DesiredLabel, *LabelError.ToString()));
        }
        if (AActor* Holder = mcp_common::FindActorByName(World, DesiredLabel))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("actor_label '%s' is already the label or name of actor %s (%s) in the editor world, which the set_niagara_component_* tools would reach instead; pick another label. Nothing was spawned."),
                *DesiredLabel, *Holder->GetName(), *Holder->GetClass()->GetName()));
        }
    }

    UActorFactory* Factory = GEditor ? GEditor->FindActorFactoryByClass(UActorFactoryNiagara::StaticClass()) : nullptr;
    if (!Factory)
    {
        return mcp_common::MakeErrorReply(TEXT("The editor's Niagara actor factory (UActorFactoryNiagara) is not registered; nothing was spawned."));
    }
    // The factory's own gate, the one a drag into the viewport meets (the Niagara settings can require an effect type).
    FText CannotPlace;
    if (!Factory->CanCreateActorFrom(FAssetData(System), CannotPlace))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s cannot be placed as a Niagara actor: %s; nothing was spawned."), *System->GetPathName(), *CannotPlace.ToString()));
    }

    // One undo step: the spawn records the level, the label its Modify.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SpawnNiagaraActor", "Spawn Niagara Actor (MCP)"));

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    // CreateActor spawns the factory's ANiagaraActor, labels it after the system (unique) and runs PostSpawnActor,
    // which sets the component's asset and registers it.
    ANiagaraActor* NewActor = Cast<ANiagaraActor>(Factory->CreateActor(System, Level, FTransform(Rotation, Location), SpawnParams));
    UNiagaraComponent* Component = NewActor ? NewActor->GetNiagaraComponent() : nullptr;
    if (!Component)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The Niagara actor factory placed no Niagara actor for %s; nothing was spawned."), *System->GetPathName()));
    }
    if (bLabelled)
    {
        NewActor->SetActorLabel(DesiredLabel);
    }

    // Read back from the actor and its component, not echoed from the request.
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("system_path"), Component->GetAsset() ? Component->GetAsset()->GetPathName() : FString());
    // Internal object name first (delete_actor resolves by GetName).
    Data->SetStringField(TEXT("owner_actor_name"), NewActor->GetName());
    Data->SetStringField(TEXT("owner_actor_label"), NewActor->GetActorLabel());
    Data->SetStringField(TEXT("actor_class"), NewActor->GetClass()->GetName());
    Data->SetStringField(TEXT("actor_path"), NewActor->GetPathName());
    Data->SetStringField(TEXT("component_name"), Component->GetName());
    Data->SetBoolField(TEXT("is_registered"), Component->IsRegistered());
    Data->SetBoolField(TEXT("is_active"), Component->IsActive());
    Data->SetArrayField(TEXT("location"), mcp_common::VectorToJsonArray(NewActor->GetActorLocation()));
    mcp_common::AddRotatorField(Data, TEXT("rotation"), NewActor->GetActorRotation());
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_niagara_component_float
// ---------------------------------------------------------------------------
// MCP:set_niagara_component_float - Set a float override (a user parameter value) on the Niagara component of an actor in the editor world, the actor found by internal name or editor label and the first UNiagaraComponent on it used, through UNiagaraComponent::SetVariableFloat, which adds the override when the component has none of that name. value is required and must be a JSON number; a missing or mistyped value, a missing actor_name or variable_name, or an actor without a Niagara component is an error and nothing changes. The value is set on the component in memory with no undo step; the level is neither marked dirty nor saved. verified is whether GetVariableFloat finds the variable, and value_read_back is what it reads. Spawn a component with spawn_niagara_component; whole numbers: set_niagara_component_int; the asset default: set_niagara_user_parameter.
//   @retired set_niagara_component_variable - type="float", value
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param value - Float value to set, a JSON number; the call fails without it.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: number }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentFloat(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    // value is required and must be a JSON number: a lenient read would write 0 for a missing or mistyped one.
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("value"))) { return Mistyped; }
    double Value = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("value"), Value)) { return Missing; }

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
//   @retired set_niagara_component_variable - type="vector", value=[x, y, z]
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
// MCP:set_niagara_component_color - Set a LinearColor override (a user parameter value) on the Niagara component of an actor in the editor world, the actor found by internal name or editor label and the first UNiagaraComponent on it used, through UNiagaraComponent::SetVariableLinearColor, which adds the override when needed. color is [r, g, b] or [r, g, b, a] in numbers within the float range; an omitted alpha is 1 and an omitted color is opaque black. Any other color, a missing actor_name or variable_name, or an actor without a Niagara component is an error and nothing changes. The value is set on the component in memory with no undo step; the level is neither marked dirty nor saved. verified is whether GetVariableColor finds the variable, and value_read_back is what it reads. Four plain components: set_niagara_component_vec4; the asset default: set_niagara_user_parameter.
//   @retired set_niagara_component_variable - type="color", value=[r, g, b, a]
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param color - Color as [R, G, B] or [R, G, B, A], numbers within the float range; A is 1.0 when omitted, and an omitted color is opaque black. Any other value is an error.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, value_read_back: [number, number, number, number] }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentColor(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }

    // RGB or RGBA from the "color" array (A defaults to 1, the whole color to opaque black); any other value is refused
    // before the component is touched.
    FLinearColor Color = FLinearColor::Black;
    if (TSharedPtr<FJsonObject> Malformed = mcp_common::ReadJsonLinearColor(Params, TEXT("color"), FLinearColor::Black, Color))
    {
        return Malformed;
    }

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
//   @retired set_niagara_component_variable - type="bool", value
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
// set_niagara_component_fixed_bounds
// Sets the per-component fixed bounds via UNiagaraComponent::SetSystemFixedBounds
// (FBox in local space). Reads back via GetSystemFixedBounds to verify.
// ---------------------------------------------------------------------------
// MCP:set_niagara_component_fixed_bounds - Set the fixed-bounds override of the Niagara component on a placed actor in the editor world (the first UNiagaraComponent of the actor found by internal name or label) to the local-space box min..max, then read it back. The override is runtime state of that component (UNiagaraComponent::SetSystemFixedBounds): the engine stores it in a member that is not a saved property, in 5.7 and 5.8 alike, and a component has no fixed-bounds field in its details panel, so it is lost when the level reloads and is not carried into PIE; it makes no undo step and does not dirty the level. Repeating the call with the same box gives the same result. An actor that is missing or has no Niagara component is an error and nothing changes. For bounds that persist, write the System asset's Fixed Bounds (bFixedBounds and FixedBounds, with set_properties) or an emitter's with set_niagara_emitter_properties (bounds_mode Fixed); place a Niagara actor with spawn_actor (type NiagaraActor).
//   @param actor_name - Internal name or editor label of an actor in the editor world that has a Niagara component, as get_actors_in_level lists it, e.g. 'NiagaraActor_0'.
//   @param min [string!] - Box minimum corner in the component's local space, [x, y, z] in centimetres, e.g. [-500, -500, 0]; a missing or short array reads as [0, 0, 0], and it is not checked against max.
//   @param max [string!] - Box maximum corner in the component's local space, [x, y, z] in centimetres, e.g. [500, 500, 800]; a missing or short array reads as [0, 0, 0].
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
    // UNiagaraComponent keeps SystemFixedBounds in a plain member, not a UPROPERTY, in 5.7 and 5.8 alike, and the
    // details panel has no fixed-bounds field for a component: there is no serialized property to write instead,
    // so this stays runtime state (the description says so).
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// Spawned-component complex-type override parameters.
// ---------------------------------------------------------------------------

// MCP:set_niagara_component_vec2 - Set a Vec2 override (a user parameter value) on the Niagara component of an actor in the editor world, the actor found by internal name or editor label and the first UNiagaraComponent on it used, through UNiagaraComponent::SetVariableVec2 with x and y. An omitted component is 0; a component that is not a JSON number, a missing actor_name or variable_name, or an actor without a Niagara component is an error and nothing changes. The value is set on the component in memory with no undo step; the level is neither marked dirty nor saved. The component has no public getter for this type, so nothing is read back: verified is always true and note says so. Four components: set_niagara_component_vec4; three: set_niagara_component_vector.
//   @retired set_niagara_component_variable - type="vec2", value=[x, y]
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param x - X component of the Vec2, a JSON number; 0 when omitted.
//   @param y - Y component of the Vec2, a JSON number; 0 when omitted.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, note: string }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentVec2(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    // An omitted component is 0; a present one must be a JSON number (a lenient read would write 0 for it).
    for (const TCHAR* Key : { TEXT("x"), TEXT("y") })
    {
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, Key)) { return Mistyped; }
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

// MCP:set_niagara_component_vec4 - Set a Vec4 override (a user parameter value) on the Niagara component of an actor in the editor world, the actor found by internal name or editor label and the first UNiagaraComponent on it used, through UNiagaraComponent::SetVariableVec4 with x, y, z and w. An omitted component is 0; a component that is not a JSON number, a missing actor_name or variable_name, or an actor without a Niagara component is an error and nothing changes. The value is set on the component in memory with no undo step; the level is neither marked dirty nor saved. The component has no public getter for this type, so nothing is read back: verified is always true and note says so. Two components: set_niagara_component_vec2; a color: set_niagara_component_color.
//   @retired set_niagara_component_variable - type="vec4", value=[x, y, z, w]
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param x - X component of the Vec4, a JSON number; 0 when omitted.
//   @param y - Y component of the Vec4, a JSON number; 0 when omitted.
//   @param z - Z component of the Vec4, a JSON number; 0 when omitted.
//   @param w - W component of the Vec4, a JSON number; 0 when omitted.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, note: string }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentVec4(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    // An omitted component is 0; a present one must be a JSON number (a lenient read would write 0 for it).
    for (const TCHAR* Key : { TEXT("x"), TEXT("y"), TEXT("z"), TEXT("w") })
    {
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, Key)) { return Mistyped; }
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

// MCP:set_niagara_component_int - Set an int32 override (a user parameter value) on the Niagara component of an actor in the editor world, the actor found by internal name or editor label and the first UNiagaraComponent on it used, through UNiagaraComponent::SetVariableInt, which adds the override when needed. value is required and must be a JSON number; a missing or mistyped value, a missing actor_name or variable_name, or an actor without a Niagara component is an error and nothing changes. The value is set on the component in memory with no undo step; the level is neither marked dirty nor saved. The component has no public getter for this type, so nothing is read back: verified is always true and note says so. Floats: set_niagara_component_float; the asset default: set_niagara_user_parameter.
//   @retired set_niagara_component_variable - type="int", value
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param value - Integer value to set, a JSON number; the call fails without it.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, note: string }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentInt(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    // value is required and must be a JSON number: a lenient read would write 0 for a missing or mistyped one.
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("value"))) { return Mistyped; }
    int32 Value = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("value"), Value)) { return Missing; }

    const FString VarName(*Target.VariableName);
    Target.Component->SetVariableInt(FName(*VarName), (int32)Value);

    TSharedPtr<FJsonObject> Data = Niagara_StartComponentReply(Target, TEXT("Int"));
    Data->SetBoolField(TEXT("set"), true);
    Data->SetBoolField(TEXT("verified"), true);
    Data->SetStringField(TEXT("note"), Niagara_NoReadbackNote(TEXT("Int")));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_niagara_component_position - Sets a Position (FVector) override parameter on a spawned Niagara component.  Resolves the UNiagaraComponent on the owner actor and calls SetVariablePosition, then reads it back via GetNiagaraVariablePosition.
//   @retired set_niagara_component_variable - type="position", value=[x, y, z]
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

// MCP:set_niagara_component_quat - Set a Quat override (a user parameter value) on the Niagara component of an actor in the editor world, the actor found by internal name or editor label and the first UNiagaraComponent on it used, through UNiagaraComponent::SetVariableQuat with w, x, y and z, not normalized. An omitted component is the identity one (w 1, x, y and z 0); a component that is not a JSON number, a missing actor_name or variable_name, or an actor without a Niagara component is an error and nothing changes. The value is set on the component in memory with no undo step; the level is neither marked dirty nor saved. The component has no public getter for this type, so nothing is read back: verified is always true and note says so. Four plain components: set_niagara_component_vec4.
//   @retired set_niagara_component_variable - type="quat", value=[x, y, z, w]
//   @param actor_name - Owner actor identifier (internal name or editor label).
//   @param variable_name - Niagara variable name (override is added locally if needed).
//   @param w - W (scalar) component of the quaternion, a JSON number; 1 when omitted.
//   @param x - X component of the quaternion, a JSON number; 0 when omitted.
//   @param y - Y component of the quaternion, a JSON number; 0 when omitted.
//   @param z - Z component of the quaternion, a JSON number; 0 when omitted.
//   @returns { actor_name: string, variable_name: string, variable_type: string, set: bool, verified: bool, note: string }

TSharedPtr<FJsonObject> HandleSetNiagaraComponentQuat(const TSharedPtr<FJsonObject>& Params)
{
    FNiagaraComponentTarget Target;
    if (TSharedPtr<FJsonObject> Error = Niagara_ResolveComponentVariable(Params, Target))
    {
        return Error;
    }
    // An omitted component is the identity's (w 1, x y z 0); a present one must be a JSON number.
    for (const TCHAR* Key : { TEXT("w"), TEXT("x"), TEXT("y"), TEXT("z") })
    {
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, Key)) { return Mistyped; }
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
//   @retired set_niagara_component_variable - type="object", value=<asset path>
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
//   @retired set_niagara_component_variable - type="matrix", value=[16 numbers, row-major]
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

// MCP:get_niagara_component_matrix - Reads a Matrix (FMatrix4f) override parameter back off a spawned Niagara component — the round-trip reader for set_niagara_component_variable. UNiagaraComponent exposes no GetNiagaraVariableMatrix, so this reads the value straight out of the component's OverrideParameters store (FNiagaraParameterStore) by looking up the Matrix4 FNiagaraVariable; the override is stored as FMatrix44f. Returns found:false with an empty matrix (never a faked identity) when the variable has no local override on the component.
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
// remove_niagara_emitter_handle
// Removes an emitter handle by name or index from a Niagara system.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_emitter_handle - Remove one emitter handle from a Niagara System asset in one undo step, then save the system. The handle is chosen by display name (case-insensitive) or, when emitter_name is absent or empty, by 0-based emitter_index. An unknown system, name or index, or an emitter_index that is not a JSON number, is an error and nothing changes. A failed save makes success false, with saved=false and save_error, the removal kept in memory. Returns the removed name and id and the emitter counts before and after. List handles with get_niagara_emitter_handles; to keep an emitter but stop it, use set_niagara_emitter_properties.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Optional display name of the emitter handle to remove
//   @param emitter_index - Optional zero-based index; used if emitter_name not provided
//   @returns { system_path: string, removed_emitter_name: string, removed_emitter_id: string, emitters_before: int, emitters_after: int, removed: bool, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveNiagaraEmitterHandle(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    const int32 BeforeCount = System->GetEmitterHandles().Num();

    // Without a name the handle is picked by emitter_index, which must then be a JSON number: a string would read as
    // 0 through LexFromString and remove the first emitter.
    FString NameGiven;
    Params->TryGetStringField(TEXT("emitter_name"), NameGiven);
    if (NameGiven.IsEmpty())
    {
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("emitter_index"))) { return Mistyped; }
    }

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
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("removed_emitter_name"), RemovedName);
    Data->SetStringField(TEXT("removed_emitter_id"), RemovedId);
    Data->SetNumberField(TEXT("emitters_before"), BeforeCount);
    Data->SetNumberField(TEXT("emitters_after"), System->GetEmitterHandles().Num());
    Data->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_niagara_emitter_handle"), SaveError);
}

// ---------------------------------------------------------------------------
// duplicate_niagara_emitter_handle
// Duplicates an existing emitter handle in a Niagara system.
// ---------------------------------------------------------------------------
// MCP:duplicate_niagara_emitter_handle - Duplicate an emitter handle of a Niagara System asset under a new name, keeping its enabled state, in one undo step, then save the system. The source is found by display name (case-insensitive); an unknown system or source is an error and nothing changes. The stored name can differ from the one asked for, so read new_emitter_name. Returns the actual name and id of the copy plus total_emitters; a failed save makes success false, with saved=false and save_error, the copy kept in memory. Find handle names with get_niagara_emitter_handles; add an emitter asset instead with add_niagara_emitter_from_template; rename the copy with rename_niagara_emitter_handle.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param source_emitter_name - Display name of the emitter handle to copy (case-insensitive), as get_niagara_emitter_handles lists it.
//   @param new_emitter_name - Name for the copy (default "<source_emitter_name>_Copy").
//   @returns { system_path: string, source_emitter_name: string, new_emitter_name: string, new_emitter_id: string, is_valid: bool, total_emitters: int, saved: bool, save_error?: string }

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
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("source_emitter_name"), SourceName);
    Data->SetStringField(TEXT("new_emitter_name"), NewHandle.GetName().ToString());
    Data->SetStringField(TEXT("new_emitter_id"), NewHandle.GetId().ToString());
    Data->SetBoolField(TEXT("is_valid"), NewHandle.IsValid());
    Data->SetNumberField(TEXT("total_emitters"), System->GetEmitterHandles().Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("duplicate_niagara_emitter_handle"), SaveError);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_handles
// Lists all emitter handles on a Niagara system: name, ID, enabled state.
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_handles - List every emitter handle of a Niagara System asset, or just one chosen by display name or 0-based index, with its display name, id GUID, id name, enabled and valid flags, version GUID and, when it has emitter data, its local-space and deprecated flags, plus emitter_count (all handles of the system). Read-only; the names feed the other emitter tools. A name or index that matches no handle, or an emitter_index that is not a JSON number, is an error.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param emitter_name - Optional display name of one handle to return (case-insensitive); when non-empty it wins over emitter_index.
//   @param emitter_index - Optional 0-based position in the emitter list of the system of one handle to return, used only when emitter_name is absent or empty.
//   @returns { system_path: string, emitter_count: int, emitter_handles: [{ index: int, name: string, id: string, id_name: string, is_enabled: bool, is_valid: bool, emitter_version: string, is_local_space?: bool, is_deprecated?: bool }] }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterHandles(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }
    // Optional single-handle filter: emitter_name (wins when non-empty) or emitter_index.
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("emitter_index"))) { return Mistyped; }
    FString FilterName;
    const bool bFilter = (Params->TryGetStringField(TEXT("emitter_name"), FilterName) && !FilterName.IsEmpty())
        || Params->HasTypedField<EJson::Number>(TEXT("emitter_index"));
    const FNiagaraEmitterHandle* Only = nullptr;
    if (bFilter)
    {
        FString Error;
        Only = Niagara_FindHandleByNameOrIndex(Params, System, Error);
        if (!Only)
        {
            return mcp_common::MakeErrorReply(Error);
        }
    }

    const TArray<FNiagaraEmitterHandle>& Handles = System->GetEmitterHandles();

    TArray<TSharedPtr<FJsonValue>> HandleArray;
    for (int32 Index = 0; Index < Handles.Num(); ++Index)
    {
        const FNiagaraEmitterHandle& H = Handles[Index];
        if (Only && Only != &H)
        {
            continue;
        }
        TSharedPtr<FJsonObject> HObj = MakeShared<FJsonObject>();
        HObj->SetNumberField(TEXT("index"), Index);
        HObj->SetStringField(TEXT("name"), H.GetName().ToString());
        HObj->SetStringField(TEXT("id"), H.GetId().ToString());
        HObj->SetStringField(TEXT("id_name"), H.GetIdName().ToString());
        HObj->SetBoolField(TEXT("is_enabled"), H.GetIsEnabled());
        HObj->SetBoolField(TEXT("is_valid"), H.IsValid());
        // The emitter data's version and flags, when the handle has data.
        if (const FVersionedNiagaraEmitterData* EmitterData = H.GetEmitterData())
        {
            HObj->SetStringField(TEXT("emitter_version"), EmitterData->Version.VersionGuid.ToString());
            HObj->SetBoolField(TEXT("is_local_space"), EmitterData->bLocalSpace);
            HObj->SetBoolField(TEXT("is_deprecated"), EmitterData->bDeprecated);
        }
        else
        {
            HObj->SetStringField(TEXT("emitter_version"), TEXT("N/A"));
        }
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
// MCP:set_niagara_emitter_enabled - Enable or disable one emitter handle of a Niagara System asset, found by display name (case-insensitive), in one undo step, then save the system. enabled defaults to true. A handle already in that state is a success with state_changed=false and saved=false, and nothing is written. An unknown system or emitter is an error. A failed save makes success false, with saved=false and save_error, the change kept in memory. verified compares the state read back with the request. List handles and their states with get_niagara_emitter_handles; take an emitter out for good with remove_niagara_emitter_handle.
//   @retired set_niagara_emitter_properties - enabled
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param enabled - Optional bool; defaults to true
//   @returns { system_path: string, emitter_name: string, enabled: bool, state_changed: bool, verified: bool, is_enabled_read_back: bool, saved: bool, save_error?: string, note?: string }

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

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetBoolField(TEXT("enabled"), bEnabled);

    // A handle already in the requested state is a valid no-op: SetIsEnabled would change nothing, so nothing is
    // modified, dirtied or saved (a save would only fail a no-op during PIE or on a read-only file).
    if (Target->GetIsEnabled() == bEnabled)
    {
        Data->SetBoolField(TEXT("state_changed"), false);
        Data->SetBoolField(TEXT("verified"), true);
        Data->SetBoolField(TEXT("is_enabled_read_back"), bEnabled);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter is already in that state; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterEnabled", "Set Niagara Emitter Enabled"));
    System->Modify();

    const bool bChanged = Target->SetIsEnabled(bEnabled, *System, /*bRecompileIfChanged*/ true);

    // Read back to verify.
    const bool ReadBack = Target->GetIsEnabled();

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetBoolField(TEXT("state_changed"), bChanged);
    Data->SetBoolField(TEXT("verified"), ReadBack == bEnabled);
    Data->SetBoolField(TEXT("is_enabled_read_back"), ReadBack);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_enabled"), SaveError);
}

// ---------------------------------------------------------------------------
// rename_niagara_emitter_handle
// Renames an emitter handle. Uses FNiagaraEmitterHandle::SetName which requires
// the owning system reference (to ensure name uniqueness).
// ---------------------------------------------------------------------------
// MCP:rename_niagara_emitter_handle - Rename an emitter handle of a Niagara System asset in one undo step, then save the system. The engine sanitizes the name and makes it unique among the emitters of the system, so read new_name and verified (false when the stored name differs from the request). A name the engine takes as the current one (the same after sanitizing, case-sensitive, ignoring a trailing number) is a success with renamed=false and saved=false, and nothing is written. An unknown system or current_name is an error. A failed save makes success false, with saved=false and save_error. List names with get_niagara_emitter_handles; copy a handle under a new name with duplicate_niagara_emitter_handle.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param current_name - Current display name of the emitter handle (case-insensitive), as get_niagara_emitter_handles lists it.
//   @param new_name - Requested display name; it may come back sanitized or with a numeric suffix.
//   @returns { system_path: string, old_name: string, new_name: string, verified: bool, renamed: bool, saved: bool, save_error?: string, note?: string }

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

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("old_name"), CurrentName);

    // FNiagaraEmitterHandle::SetName returns without a change when the sanitized name equals the current one
    // (case-sensitive, the trailing number ignored). That valid no-op is answered here, before anything is
    // modified, dirtied or saved.
    const FName RequestedName(*NewName);
    const FName SanitizedName(*FNiagaraUtilities::SanitizeNameForObjectsAndPackages(RequestedName.ToString()));
    if (SanitizedName.IsEqual(Target->GetName(), ENameCase::CaseSensitive, /*bCompareNumber*/ false))
    {
        const FString StoredName = Target->GetName().ToString();
        Data->SetStringField(TEXT("new_name"), StoredName);
        Data->SetBoolField(TEXT("verified"), StoredName == NewName);
        Data->SetBoolField(TEXT("renamed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The engine takes that name as the current one; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FString OldStoredName = Target->GetName().ToString();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RenameNiagaraEmitterHandle", "Rename Niagara Emitter Handle"));
    System->Modify();

    Target->SetName(RequestedName, *System);

    // Read back to verify.
    const FString ReadBackName = Target->GetName().ToString();

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetStringField(TEXT("new_name"), ReadBackName);
    Data->SetBoolField(TEXT("verified"), ReadBackName == NewName);
    Data->SetBoolField(TEXT("renamed"), !ReadBackName.Equals(OldStoredName, ESearchCase::CaseSensitive));
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("rename_niagara_emitter_handle"), SaveError);
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
// set_niagara_emitter_sim_target
// Sets SimTarget, bLocalSpace and bDeterministic on the emitter's
// FVersionedNiagaraEmitterData. Saves the system after modification.
// ---------------------------------------------------------------------------
// MCP:set_niagara_emitter_sim_target - Set the simulation target (CPUSim or GPUComputeSim, case-insensitive), the local-space flag (bLocalSpace: particles in the emitter's space, or world space) and the deterministic flag of one emitter of a Niagara System asset, found by handle name, in one undo step, then save the system. Each of the three is optional and only the supplied ones are written, but at least one is required. Values already in place are a success with changed=false and saved=false, and nothing is written. An unknown sim_target, system or emitter, or a call with none of the three, is an error and nothing changes. A failed save makes success false, with saved=false and save_error, the change kept in memory. Returns the three as read back. Read them back with get_niagara_emitter_runtime_summary; GPU spawn limit: set_niagara_emitter_max_gpu_particles.
//   @retired set_niagara_emitter_properties - sim_target, local_space, deterministic
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param sim_target - Optional target type, "CPUSim" or "GPUComputeSim"; omitted keeps the current one.
//   @param local_space - Optional bool for bLocalSpace: true simulates the particles in the emitter's local space, false in world space; omitted keeps the current value.
//   @param deterministic - Optional bool for bDeterminism; omitted keeps the current value.
//   @returns { system_path: string, emitter_name: string, sim_target: string, local_space: bool, deterministic: bool, success: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterSimTarget(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    // Three optional settings; at least one must be supplied.
    FString SimTargetStr;
    const bool bHasSimTarget = Params->TryGetStringField(TEXT("sim_target"), SimTargetStr) && !SimTargetStr.IsEmpty();
    bool bLocalSpace = false;
    const bool bHasLocalSpace = Params->TryGetBoolField(TEXT("local_space"), bLocalSpace);
    bool bDeterministic = false;
    const bool bHasDeterministic = Params->TryGetBoolField(TEXT("deterministic"), bDeterministic);
    if (!bHasSimTarget && !bHasLocalSpace && !bHasDeterministic)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Nothing to set: pass at least one of sim_target, local_space (bool) or deterministic (bool)"));
    }

    ENiagaraSimTarget NewTarget = ENiagaraSimTarget::CPUSim;
    if (bHasSimTarget && !StringToSimTarget(SimTargetStr, NewTarget))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid sim_target '%s' (expected CPUSim or GPUComputeSim)"), *SimTargetStr));
    }

    // The emitter data lives in the handle's UNiagaraEmitter, which the transaction must record for an undo.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    if (!bHasSimTarget)
    {
        NewTarget = EmitterData->SimTarget;
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);

    // Values already in place are a valid no-op: nothing is modified, dirtied or saved.
    if (EmitterData->SimTarget == NewTarget
        && (!bHasLocalSpace || EmitterData->bLocalSpace == bLocalSpace)
        && (!bHasDeterministic || EmitterData->bDeterminism == bDeterministic))
    {
        Data->SetStringField(TEXT("sim_target"), SimTargetToString(EmitterData->SimTarget));
        Data->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
        Data->SetBoolField(TEXT("deterministic"), EmitterData->bDeterminism);
        Data->SetBoolField(TEXT("success"), true);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter already has these settings; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterSimTarget", "Set Niagara Emitter Sim Target"));
    System->Modify();
    Emitter->Modify();

    EmitterData->SimTarget = NewTarget;
    if (bHasLocalSpace)
    {
        EmitterData->bLocalSpace = bLocalSpace;
    }
    if (bHasDeterministic)
    {
        EmitterData->bDeterminism = bDeterministic;
    }

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetStringField(TEXT("sim_target"), SimTargetToString(EmitterData->SimTarget));
    Data->SetBoolField(TEXT("local_space"), EmitterData->bLocalSpace);
    Data->SetBoolField(TEXT("deterministic"), EmitterData->bDeterminism);
    Data->SetBoolField(TEXT("success"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_sim_target"), SaveError);
}

// ---------------------------------------------------------------------------
// get_niagara_effect_type
// Reads the UNiagaraEffectType associated with a UNiagaraSystem and returns
// its key properties: UpdateFrequency, CullReaction, bAllowCullingForLocalPlayers.
// ---------------------------------------------------------------------------
// MCP:get_niagara_effect_type - Read which Niagara Effect Type asset a Niagara System asset uses, plus that effect type's settings: update_frequency and cull_reaction (as enum value names), allows_culling (bAllowCullingForLocalPlayers), and the scalability limits of its first system and emitter scalability entries: max_distance and max_instances (system), cull_by_distance (max_distance above 0) and scale_spawn_count (emitter SpawnCountScale). A system with no effect type, or an effect type without those entries, is a success with the defaults: an empty effect_type_path, None for both enums, allows_culling false, max_distance 0, max_instances 0, cull_by_distance false and scale_spawn_count 1. Read-only; an unknown system path is an error. Assign one with set_properties; the system's own flags and fixed bounds: get_properties.
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire" (the object path form also works).
//   @returns { system_path: string, effect_type_path: string, update_frequency: string, cull_reaction: string, allows_culling: bool, max_distance: number, max_instances: int, cull_by_distance: bool, scale_spawn_count: number }

TSharedPtr<FJsonObject> HandleGetNiagaraEffectType(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    UNiagaraEffectType* EffectType = System->GetEffectType();

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);

    // The scalability limits of the first system and emitter scalability entries, with defaults when absent.
    float MaxDistance = 0.0f;
    int32 MaxInstances = 0;
    float ScaleSpawnCount = 1.0f;
    if (EffectType)
    {
        const auto& SysSettings = EffectType->SystemScalabilitySettings.Settings;
        if (SysSettings.Num() > 0)
        {
            MaxDistance = SysSettings[0].MaxDistance;
            MaxInstances = SysSettings[0].MaxInstances;
        }
        const auto& EmtSettings = EffectType->EmitterScalabilitySettings.Settings;
        if (EmtSettings.Num() > 0)
        {
            ScaleSpawnCount = EmtSettings[0].SpawnCountScale;
        }
    }
    Data->SetNumberField(TEXT("max_distance"), MaxDistance);
    Data->SetNumberField(TEXT("max_instances"), MaxInstances);
    Data->SetBoolField(TEXT("cull_by_distance"), MaxDistance > 0.0f);
    Data->SetNumberField(TEXT("scale_spawn_count"), ScaleSpawnCount);

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
// MCP:set_niagara_effect_type - Assign a Niagara Effect Type asset (the scalability and culling settings) to a Niagara System asset in one undo step, then save the system. effect_type_path is loaded as a UNiagaraEffectType; an unknown system or effect type is an error and nothing changes. A system that already uses that effect type is a success with changed=false and saved=false, and nothing is written. A failed save makes success false, with saved=false and save_error, the change kept in memory. Read it back, with the effect type's limits, with get_niagara_effect_type.
//   @retired set_properties - object_path=<system>, properties={"EffectType":"/Game/..."}, save=true
//   @param system_path - Asset path of the Niagara System.
//   @param effect_type_path - Asset path of the UNiagaraEffectType to assign.
//   @returns { system_path: string, effect_type_path: string, success: bool, changed: bool, saved: bool, save_error?: string, note?: string }

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

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetStringField(TEXT("effect_type_path"), EffectType->GetPathName());
    Data->SetBoolField(TEXT("success"), true);

    // A system that already uses this effect type is a valid no-op: SetEffectType would change nothing, so
    // nothing is modified, dirtied or saved.
    if (System->GetEffectType() == EffectType)
    {
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The system already uses this effect type; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEffectType", "Set Niagara Effect Type"));
    System->Modify();

    System->SetEffectType(EffectType);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_effect_type"), SaveError);
}

// ---------------------------------------------------------------------------
// add_niagara_simulation_stage
// Creates a UNiagaraSimulationStageGeneric, configures it with the default
// iteration source and dispatch type and the requested iteration count, and adds
// it to the emitter's simulation stages via UNiagaraEmitter::AddSimulationStage.
// ---------------------------------------------------------------------------
// MCP:add_niagara_simulation_stage - Add a generic simulation stage (UNiagaraSimulationStageGeneric) to one emitter of a Niagara System asset, found by handle name, in one undo step, then save the system. The stage gets stage_name as its SimulationStageName, iterates over Particles with execute behavior Always and dispatch type OneD, and runs iteration_count times in a row (default 1), read back in iteration_count; its graph starts in a default state, to be wired in the Niagara editor. An iteration_count that is not a JSON number from 0 to 65535 (the range the runtime honors; 0 skips the stage; a fraction is rounded), or an unknown system or emitter, is an error and nothing changes. A failed save makes success false, with saved=false and save_error, the stage kept in memory. Change its other settings with set_niagara_simulation_stage_properties; list stages with get_niagara_simulation_stages.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle.
//   @param stage_name - Name for the new simulation stage.
//   @param iteration_count - Optional number of times the stage runs in a row, from 0 to 65535 (rounded to a whole number); defaults to 1
//   @returns { system_path: string, emitter_name: string, stage_name: string, requested_iteration_count: int, iteration_count: int, total_simulation_stages: int, success: bool, saved: bool, save_error?: string }

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

    // iteration_count becomes the stage's NumIterations value. The runtime clamps it to 0..65535 and skips a stage
    // with 0, so a count outside that range is refused rather than stored and silently clamped. Read into a bool,
    // not negated in the if: the param is optional (default 1).
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("iteration_count"))) { return Mistyped; }
    const int32 MaxIterations = static_cast<int32>(TNumericLimits<uint16>::Max());
    int32 RequestedIterations = 1;
    const bool bIterationsRead = Params->TryGetNumberField(TEXT("iteration_count"), RequestedIterations);
    if (Params->HasField(TEXT("iteration_count"))
        && (!bIterationsRead || RequestedIterations < 0 || RequestedIterations > MaxIterations))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("iteration_count must be a number from 0 to %d; nothing was added"), MaxIterations));
    }

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
    // The object name is left to the engine, as the editor does: NewObject with the name of a stage the emitter
    // already has would rebuild that live stage in place. stage_name goes to SimulationStageName below.
    UNiagaraSimulationStageGeneric* Stage = NewObject<UNiagaraSimulationStageGeneric>(
        Emitter, UNiagaraSimulationStageGeneric::StaticClass(), NAME_None,
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
    // NumIterations is a parameter binding whose constant value is the count used while the binding is unset,
    // which it is on a new stage (UNiagaraSimulationStageGeneric::PostInitProperties seeds it with an int of 1).
    Stage->NumIterations.SetDefaultValueEditorOnly<int32>(RequestedIterations);

    Emitter->AddSimulationStage(Stage, EmitterData->Version.VersionGuid);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Verify the stage was added.
    const TArray<UNiagaraSimulationStageBase*>& Stages = EmitterData->GetSimulationStages();
    const bool bAdded = Stages.Contains(Stage);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("stage_name"), StageName);
    Data->SetNumberField(TEXT("requested_iteration_count"), RequestedIterations);
    Data->SetNumberField(TEXT("iteration_count"), Stage->NumIterations.GetDefaultValue<int32>());
    Data->SetNumberField(TEXT("total_simulation_stages"), Stages.Num());
    Data->SetBoolField(TEXT("success"), bAdded);
    // The verdict is BOTH things: the stage was added AND the system reached disk.
    if (!bAdded)
    {
        mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Data, false,
            mcp_common::MeasuredOutcomeError(TEXT("add_niagara_simulation_stage"), TEXT("bAdded")));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_niagara_simulation_stage"), SaveError);
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

    // Clamps the number at Value, a value of Prop, to Prop's ClampMin / ClampMax metadata: the one-property form of
    // mcp_common::ClampNumericByMeta, which MergeJsonWithClamp runs on the object after its merge.
    void Niagara_ClampValueByMeta(const FProperty* Prop, void* Value)
    {
        const FNumericProperty* Num = CastField<FNumericProperty>(Prop);
        const bool bHasMin = Prop->HasMetaData(TEXT("ClampMin"));
        const bool bHasMax = Prop->HasMetaData(TEXT("ClampMax"));
        if (!Num || (!bHasMin && !bHasMax))
        {
            return;
        }
        if (Num->IsFloatingPoint())
        {
            double V = Num->GetFloatingPointPropertyValue(Value);
            if (bHasMin) { V = FMath::Max(V, FCString::Atod(*Prop->GetMetaData(TEXT("ClampMin")))); }
            if (bHasMax) { V = FMath::Min(V, FCString::Atod(*Prop->GetMetaData(TEXT("ClampMax")))); }
            Num->SetFloatingPointPropertyValue(Value, V);
        }
        else if (Num->IsInteger())
        {
            int64 V = Num->GetSignedIntPropertyValue(Value);
            if (bHasMin) { V = FMath::Max<int64>(V, FCString::Atoi64(*Prop->GetMetaData(TEXT("ClampMin")))); }
            if (bHasMax) { V = FMath::Min<int64>(V, FCString::Atoi64(*Prop->GetMetaData(TEXT("ClampMax")))); }
            Num->SetIntPropertyValue(Value, V);
        }
    }

    // Checks, before anything is written, every field MergeJson holds for PropNames (the UPROPERTY names
    // Niagara_CollectSuppliedProps merged for Object's class). MergeJsonWithClamp stops at the first field it cannot
    // convert and keeps the fields it wrote before it, so it must run only once this has passed. Each field is
    // converted, as the merge converts it, into a scratch copy of the property's current value, so Object is not
    // touched. False with OutError naming the first field that does not convert; bOutChanged tells whether any
    // converted and clamped value differs from the one Object holds.
    bool Niagara_CheckMergeFields(const TSharedPtr<FJsonObject>& MergeJson, const TArray<FString>& PropNames,
        UObject* Object, bool& bOutChanged, FString& OutError)
    {
        bOutChanged = false;
        for (const FString& PropName : PropNames)
        {
            FProperty* Prop = Object->GetClass()->FindPropertyByName(FName(*PropName));
            const TSharedPtr<FJsonValue> JsonValue = MergeJson->TryGetField(PropName);
            // The merge skips a null value, so the check does too.
            if (!Prop || !JsonValue.IsValid() || JsonValue->IsNull())
            {
                continue;
            }
            const void* Current = Prop->ContainerPtrToValuePtr<void>(Object);
            void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
            FMemory::Memzero(Scratch, Prop->GetSize());
            Prop->InitializeValue(Scratch);
            Prop->CopyCompleteValue(Scratch, Current);
            FText FailReason;
            const bool bConverted = FJsonObjectConverter::JsonValueToUProperty(JsonValue, Prop, Scratch,
                /*CheckFlags*/ 0, /*SkipFlags*/ 0, /*bStrictMode*/ false, &FailReason);
            if (bConverted)
            {
                Niagara_ClampValueByMeta(Prop, Scratch);
                bOutChanged |= !Prop->Identical(Current, Scratch, PPF_None);
            }
            Prop->DestroyValue(Scratch);
            FMemory::Free(Scratch);
            if (!bConverted)
            {
                OutError = FString::Printf(TEXT("%s: %s"), *PropName,
                    FailReason.IsEmpty() ? TEXT("the value does not convert to this property") : *FailReason.ToString());
                return false;
            }
        }
        return true;
    }
}

// ---------------------------------------------------------------------------
// set_niagara_simulation_stage_properties
// ---------------------------------------------------------------------------
// MCP:set_niagara_simulation_stage_properties - Set the enum, bool, name and FIntPoint properties of one generic simulation stage (UNiagaraSimulationStageGeneric) of a Niagara emitter, found by stage_index or stage_name, in one undo step, then request a system compile and save. Use it over set_properties, which neither finds the stage nor applies ClampMin/ClampMax, compiles or saves. Only supplied fields are written; enabled goes through SetEnabled and enum writes fire PostEditChangeProperty. Every field is converted before the first write, so a value that does not convert, a non-bool enabled or a non-number stage_index is an error and nothing changes. Values already held: changed=false, saved=false. Binding fields (num_iterations, iteration_count, element_count_x|y|z, data_interface) go to not_supported, never written; add_niagara_simulation_stage sets the iteration count. A failed save makes success false with save_error. List: get_niagara_simulation_stages.
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
//   @returns { system_path, emitter_name, stage_index, stage_name, stage_class, applied:[prop names written], not_applicable:[keys], not_supported:[binding keys], enabled?: bool, post_edit_fired: bool, changed: bool, saved: bool, save_error?, note? }
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

    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("stage_index"))) { return Mistyped; }
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
        // Read into a bool, not negated in the if: stage_name is needed only when stage_index is
        // absent, and a negated read would make the catalog require it on every call.
        FString StageName;
        const bool bHasStageName = Params->TryGetStringField(TEXT("stage_name"), StageName);
        if (!bHasStageName || StageName.IsEmpty())
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
    // Read into a bool, not negated in the if: enabled is optional, and a negated read whose branch returns would
    // make the catalog publish it as required. A value that does not read as a bool (an array, an object, null) is
    // refused, not written as false.
    const bool bHasEnabled = Params->HasField(TEXT("enabled"));
    bool bEnabledValue = false;
    const bool bEnabledIsBool = Params->TryGetBoolField(TEXT("enabled"), bEnabledValue);
    if (bHasEnabled && !bEnabledIsBool)
    {
        return mcp_common::MakeErrorReply(TEXT("enabled must be true or false; nothing was written"));
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

    // Every field is converted before the first write, so a field that does not convert leaves the stage as it was.
    bool bChanged = bHasEnabled && (Stage->bEnabled != 0) != bEnabledValue;
    bool bFieldsChange = false;
    FString CheckError;
    if (!Niagara_CheckMergeFields(MergeJson, AppliedProps, Stage, bFieldsChange, CheckError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to apply simulation-stage properties, nothing was written: %s"), *CheckError));
    }
    bChanged |= bFieldsChange;

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("stage_index"), ResolvedIndex);
    Data->SetStringField(TEXT("stage_class"), Stage->GetClass()->GetName());
    Data->SetArrayField(TEXT("not_applicable"), NotApplicable);
    Data->SetArrayField(TEXT("not_supported"), NotSupported);

    // Values the stage already holds are a valid no-op: nothing is modified, compiled or saved.
    if (!bChanged)
    {
        Data->SetStringField(TEXT("stage_name"), Stage->SimulationStageName.ToString());
        Data->SetArrayField(TEXT("applied"), TArray<TSharedPtr<FJsonValue>>());
        if (bHasEnabled)
        {
            Data->SetBoolField(TEXT("enabled"), Stage->bEnabled != 0);
        }
        Data->SetBoolField(TEXT("post_edit_fired"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The stage already holds these values; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
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
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Build the report (round-trip the enum/name state the editor now holds).
    Data->SetStringField(TEXT("stage_name"), Stage->SimulationStageName.ToString());
    Data->SetArrayField(TEXT("applied"), mcp_common::StringsToJson(AppliedProps));
    if (bHasEnabled)
    {
        Data->SetBoolField(TEXT("enabled"), Stage->bEnabled != 0);
    }
    Data->SetBoolField(TEXT("post_edit_fired"), bNeedsPostEdit);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_simulation_stage_properties"), SaveError);
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
// MCP:remove_niagara_simulation_stage - Remove one simulation stage from an emitter of a Niagara System asset, found by stage_index (0-based, preferred) or stage_name (its SimulationStageName, case-insensitive), in one undo step, then request a system compile and save. One of the two is required, and a stage_index that is not a JSON number is an error. Idempotent: a stage that is not found (an index out of range falls back to stage_name) is a success with removed=false, already_removed=true and saved=false, nothing written. An unknown system or emitter is an error. A failed save makes success false, with saved=false and save_error, the removal kept in memory. List stages with get_niagara_simulation_stages; reorder them with move_niagara_simulation_stage; add one with add_niagara_simulation_stage.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle owning the stage.
//   @param stage_index - Optional 0-based index into the emitter's simulation stages, a JSON number; takes precedence over stage_name when both are supplied.
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
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("stage_index"))) { return Mistyped; }
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
        // Nothing was written, so nothing was saved.
        Resp->SetBoolField(TEXT("saved"), false);
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_niagara_simulation_stage"), SaveError);
}

// ---------------------------------------------------------------------------
// move_niagara_simulation_stage
// Resolves the stage (by stage_index or stage_name), then calls
// UNiagaraEmitter::MoveSimulationStageToIndex under a scoped transaction,
// requests a compile + saves. Idempotent: an in-position move is reported as
// already_at_target:true with the live before/after positions so callers can
// confirm the asset didn't drift between calls.
// ---------------------------------------------------------------------------
// MCP:move_niagara_simulation_stage - Move one simulation stage of an emitter of a Niagara System asset to target_index, its 0-based place in the stage list, in one undo step, then request a system compile and save. The stage is found by stage_index (preferred) or stage_name (its SimulationStageName, case-insensitive); one is required. A stage that is not found, a target_index missing or outside the current list, a stage_index or target_index that is not a JSON number, or an unknown system or emitter is an error and nothing changes. Idempotent: a stage already at target_index is a success with moved=false, already_at_target=true and saved=false, nothing written. A failed save makes success false, with saved=false and save_error. List stages with get_niagara_simulation_stages; take one out with remove_niagara_simulation_stage.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle owning the stage.
//   @param stage_index - Optional 0-based index into the emitter's simulation stages, a JSON number; takes precedence over stage_name when both are supplied.
//   @param stage_name - Optional SimulationStageName to match if stage_index is not supplied.
//   @param target_index - Required 0-based target index the stage should occupy after the move, a JSON number in [0, total_simulation_stages).
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
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("stage_index"))) { return Mistyped; }
    int32 StageIndex = INDEX_NONE;
    Params->TryGetNumberField(TEXT("stage_index"), StageIndex);
    FString StageName;
    Params->TryGetStringField(TEXT("stage_name"), StageName);
    if (StageIndex < 0 && StageName.IsEmpty())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required stage identifier: supply 'stage_index' (>=0) or 'stage_name'"));
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("target_index"))) { return Mistyped; }
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
        // Nothing was written, so nothing was saved.
        Resp->SetBoolField(TEXT("saved"), false);
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("move_niagara_simulation_stage"), SaveError);
}

// ===========================================================================
// Emitter from template + module-to-stack
//
// add_niagara_emitter_from_template uses the canonical editor path
// FNiagaraEditorUtilities::AddEmitterToSystem (NIAGARAEDITOR_API static), which
// rebuilds the emitter's nodes and syncs the system overview graph — the same
// thing the editor's "Add Emitter" does: it always creates a standalone COPY of the
// emitter inside the system (emitter_name, when given, renames the new handle).
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
// MCP:add_niagara_emitter_from_template - Add a copy of a template emitter to a Niagara system (canonical path).  Loads the template/parent UNiagaraEmitter asset and adds a standalone copy of it to the system via FNiagaraEditorUtilities::AddEmitterToSystem — the same operation the Niagara editor's "Add Emitter" performs. This rebuilds the emitter's nodes and syncs the system overview graph, optionally names the new handle, then requests a compile and saves the system.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example")
//   @param emitter_asset_path - Asset path of the source UNiagaraEmitter template (e.g. "/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain")
//   @param emitter_name - Optional name for the new emitter handle; absent or empty keeps the name the engine generates from the source emitter. The engine sanitizes it and makes it unique among the system's emitters, so read emitter_handle_name for the stored name.
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
    // Optional name for the new handle; empty keeps the engine-generated one.
    FString DesiredName;
    Params->TryGetStringField(TEXT("emitter_name"), DesiredName);

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

    // Name the new handle when asked, inside the same transaction and before the compile and save.
    if (NewId.IsValid() && !DesiredName.IsEmpty())
    {
        for (FNiagaraEmitterHandle& H : System->GetEmitterHandles())
        {
            if (H.GetId() == NewId)
            {
                H.SetName(FName(*DesiredName), *System);
                break;
            }
        }
    }

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
        OutError = FString::Printf(TEXT("Invalid script_group '%s' (expected 'spawn' or 'update'); nothing was changed"), *OutScriptGroup);
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
// MCP:add_niagara_module_to_stack - Insert an engine module script into a Niagara emitter's particle stack.  Loads the module UNiagaraScript and inserts it into the emitter's ParticleSpawn or ParticleUpdate stack via FNiagaraStackGraphUtilities::AddScriptModuleToStack, at the end or at a chosen target_index, then recompiles and saves. Not idempotent: every call adds another module node.  NOTE: only the module's script-default inputs are applied — per-instance input overrides are out of scope (the response reports added_with_defaults=true). Stateless emitters (no editable graph) return a clean error.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Display name of the emitter handle to modify.
//   @param module_script_path - Asset path of the module UNiagaraScript (e.g. "/Niagara/Modules/Update/Forces/GravityForce.GravityForce")
//   @param script_group - Optional "spawn" (ParticleSpawnScript) or "update" (ParticleUpdateScript); defaults to "update"
//   @param target_index - Optional 0-based position in the stack; -1 (INDEX_NONE, the default) and any index past the end append. A value that is not a JSON number is an error.
//   @returns { system_path: string, emitter_name: string, module_script_path: string, script_group: string, target_index: int, module_node_name: string, added_with_defaults: bool, success: bool, saved: bool, save_error?: string }

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
    // Optional stack position; INDEX_NONE (the default) appends.
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("target_index"))) { return Mistyped; }
    int32 TargetIndex = INDEX_NONE;
    Params->TryGetNumberField(TEXT("target_index"), TargetIndex);

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

    // Insert at TargetIndex (INDEX_NONE appends). Only script-default inputs apply.
    UNiagaraNodeFunctionCall* NewModule =
        FNiagaraStackGraphUtilities::AddScriptModuleToStack(Mod, *Out, TargetIndex);

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
    Data->SetNumberField(TEXT("target_index"), TargetIndex);
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
// MCP:set_niagara_module_input - Override a Niagara module input by writing the rapid-iteration store. Locates the module function-call node (by its module_node_name, as returned by add_niagara_module_to_stack) on the emitter's spawn or update stack, matches the input by its leaf name, and writes the value into the owning group script's RapidIterationParameters store. The system is recompiled and saved. value is required: a JSON number for a float or int input, [x, y, z] for a vector, [r, g, b] or [r, g, b, a] for a color; a missing or malformed one is an error and nothing changes. HONEST GATE: bool / enum / data-interface / object inputs are not rapid-iteration eligible and return a clean error. The packed value_type must also match the input's actual type or a clean error is returned. Read it back with get_niagara_module_input.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Display name of the emitter handle.
//   @param module_node_name - The module function-call node name (from add_niagara_module_to_stack's module_node_name).
//   @param input_name - Leaf name of the module input (e.g. "Drag" for "Module.Drag").
//   @param value_type - One of "float", "int", "vector", or "color"; defaults to "float"
//   @param value - Required. For float/int: a JSON number. For vector: [x, y, z], numbers within the float range. For color: [r, g, b] or [r, g, b, a], alpha 1 when omitted. A missing or malformed value is an error.
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

    // A float or int value is required and must be a JSON number: a lenient read would write 0 for a missing or
    // mistyped one.
    if (ValueType == TEXT("float") || ValueType == TEXT("int") || ValueType == TEXT("int32"))
    {
        const TSharedPtr<FJsonValue> ScalarValue = Params->TryGetField(TEXT("value"));
        if (!ScalarValue.IsValid() || ScalarValue->IsNull())
        {
            return mcp_common::MissingParameterReply(TEXT("value"));
        }
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("value"))) { return Mistyped; }
    }

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
        // Three finite numbers within the float range: a lenient read would write 0, 0, 0 for a missing or malformed value.
        const TSharedPtr<FJsonValue> VectorValue = Params->TryGetField(TEXT("value"));
        if (!VectorValue.IsValid() || VectorValue->IsNull())
        {
            return mcp_common::MissingParameterReply(TEXT("value"));
        }
        static const TArray<TSharedPtr<FJsonValue>> NoItems;
        const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        float Components[3] = { 0.f, 0.f, 0.f };
        FString ValueError;
        if (!ReadFloatComponents(VectorValue->TryGetArray(Items) && Items ? *Items : NoItems, 3, 3, Components, ValueError))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("value must be %s for a vector input. Nothing was written."), *ValueError));
        }
        const FVector3f V3(Components[0], Components[1], Components[2]);
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
        // [r, g, b] or [r, g, b, a] (alpha 1 when omitted); a missing or malformed value is refused, not written as black.
        const TSharedPtr<FJsonValue> ColorValue = Params->TryGetField(TEXT("value"));
        if (!ColorValue.IsValid() || ColorValue->IsNull())
        {
            return mcp_common::MissingParameterReply(TEXT("value"));
        }
        FLinearColor Color = FLinearColor::Black;
        if (TSharedPtr<FJsonObject> Malformed = mcp_common::ReadJsonLinearColor(Params, TEXT("value"), FLinearColor::Black, Color))
        {
            return Malformed;
        }
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
    // Resolve a renderer class from a string ("Sprite" / "Ribbon" / "Mesh" / "Light" / "Decal").
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
        if (T == TEXT("decal") || T == TEXT("decalrenderer"))
        {
            return UNiagaraDecalRendererProperties::StaticClass();
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
            // light (add_niagara_renderer seeds them, so the generic
            // get_niagara_renderer_info dump must pick up these fields too).
            TEXT("RadiusScale"), TEXT("DefaultExponent"), TEXT("SpecularScale"), TEXT("DiffuseScale"),
            TEXT("bUseInverseSquaredFalloff"), TEXT("bAffectsTranslucency"),
            TEXT("bAllowMegaLights"), TEXT("bMegaLightsCastShadows"), TEXT("bAlphaScalesBrightness"),
            TEXT("ColorAdd"),
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

    // The index of Renderer in EmitterData's renderer list when it is there and finds EmitterData through its outer
    // chain (UNiagaraRendererProperties::GetEmitterData, which the engine binds and compiles it with), else INDEX_NONE.
    int32 Niagara_VerifiedRendererIndex(const UNiagaraRendererProperties* Renderer, const FVersionedNiagaraEmitterData* EmitterData)
    {
        const int32 Index = EmitterData->GetRenderers().IndexOfByKey(Renderer);
        return (Index != INDEX_NONE && Renderer->GetEmitterData() == EmitterData) ? Index : INDEX_NONE;
    }
}

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
// add_niagara_renderer
// Creates a UNiagaraSprite/Ribbon/Mesh/Light/DecalRendererProperties inside the
// emitter and appends it via the public UNiagaraEmitter::AddRenderer (which honours
// the versioned-emitter bookkeeping under the hood). Material is set via reflection;
// the optional `properties` keys go through the same reflected merge
// set_niagara_renderer_properties uses.
// ---------------------------------------------------------------------------
// MCP:add_niagara_renderer - Add a new renderer (Sprite / Ribbon / Mesh / Light / Decal) to a Niagara emitter, optionally with its properties seeded in the same call.  Creates a UNiagaraRendererProperties instance of the requested concrete class inside the emitter, as the Niagara editor does, optionally sets the Material UPROPERTY and the supplied properties (the keys set_niagara_renderer_properties takes, converted and clamped by reflection; enum keys fire PostEditChangeProperty), and appends it to the emitter's RendererProperties array, in one undo step. A Light is seeded with affects_translucency and alpha_scales_brightness true (the engine defaults are false), as the dedicated light-renderer tool seeded it; properties overrides either. Then requests a system compile and saves the asset. verified is true when the renderer is in the emitter's list and resolves that emitter; properties reads the stored values back. Each call adds one more renderer. An unknown renderer_type, a material_path that does not load as a material, an unknown key in properties, a numeric property given something other than a JSON number, or a value that does not convert is an error and nothing is created. A failed save makes success false, with saved=false and save_error, the renderer kept in memory.
//   @param system_path - Asset path of the Niagara System (e.g. "/Game/Niagara/NS_Example").
//   @param emitter_name - Name of the emitter handle (the human-readable name in the system's emitter list).
//   @param renderer_type - One of "Sprite" / "Ribbon" / "Mesh" / "Light" / "Decal" (case-insensitive).
//   @param material_path - Optional asset path; sets the renderer's Material property (a class without one, such as Light, ignores it). If omitted, the renderer is added with a null material (still a valid empty renderer).
//   @param properties [object] - Optional object of renderer properties to seed, with the keys and value forms of set_niagara_renderer_properties, e.g. {"radius_scale": 2, "use_inverse_squared_falloff": false, "color_add": {"X": 1, "Y": 0, "Z": 0}} for a Light (scales clamped at 0; affects_translucency and alpha_scales_brightness default to true) or {"decal_source_mode": "Emitter", "renderer_visibility": 1, "decal_screen_size_fade": 0.1} for a Decal. Keys the renderer class lacks are listed in not_applicable.
//   @returns { system_path: string, emitter_name: string, renderer_type: string, renderer_class: string, renderer_index: int, material_path: string, applied: [string], not_applicable: [string], post_edit_fired: bool, properties: object, renderer_count: int, verified: bool, saved: bool, save_error?: string }

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
        return mcp_common::MakeErrorReply(TEXT("Missing required 'renderer_type' parameter ('Sprite'|'Ribbon'|'Mesh'|'Light'|'Decal')"));
    }
    FString MaterialPath;
    Params->TryGetStringField(TEXT("material_path"), MaterialPath);
    // Optional renderer properties to seed, keyed as set_niagara_renderer_properties takes them.
    const TSharedPtr<FJsonObject>* PropertiesIn = nullptr;
    const bool bPropertiesIsObject = Params->TryGetObjectField(TEXT("properties"), PropertiesIn);
    if (!bPropertiesIsObject && Params->HasField(TEXT("properties")) && !Params->HasTypedField<EJson::Null>(TEXT("properties")))
    {
        return mcp_common::MakeErrorReply(TEXT("properties must be a JSON object of renderer property keys; nothing was created"));
    }
    if (PropertiesIn)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*PropertiesIn)->Values)
        {
            bool bKnown = false;
            for (const FNiagaraPropKey& K : GRendererPropKeys)
            {
                bKnown |= Field.Key == K.JsonKey;
            }
            if (!bKnown)
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("Unknown renderer property key '%s' in properties (use the keys set_niagara_renderer_properties takes); nothing was created"),
                    *Field.Key));
            }
        }
    }

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = nullptr;
    FVersionedNiagaraEmitterData* EmitterData =
        Niagara_ResolveEmitterData(SystemPath, EmitterName, NiagaraNoEmitterDataTemplate, System, Error, &Handle);
    if (!EmitterData)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    // The renderer is created inside the handle's UNiagaraEmitter, as the Niagara editor's renderer factories create
    // one: UNiagaraRendererProperties::GetEmitterData finds the emitter through the outer chain.
    UNiagaraEmitter* Emitter = Cast<UNiagaraEmitter>(Handle->GetEmitterBase());
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(TEXT("Emitter handle has no UNiagaraEmitter asset (may be a system instance)"));
    }

    UClass* RendererClass = ResolveRendererClass(RendererType);
    if (!RendererClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Unknown renderer_type: %s (expected 'Sprite'|'Ribbon'|'Mesh'|'Light'|'Decal')"), *RendererType));
    }

    // The material is resolved before the renderer is created, so a bad one is an error with nothing created.
    UMaterialInterface* Mat = nullptr;
    if (!MaterialPath.IsEmpty())
    {
        Mat = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
        if (!Mat)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Material asset could not be loaded as UMaterialInterface: %s; nothing was created"), *MaterialPath));
        }
    }

    UNiagaraRendererProperties* NewRenderer =
        NewObject<UNiagaraRendererProperties>(Emitter, RendererClass, NAME_None, RF_Transactional);
    if (!NewRenderer)
    {
        return mcp_common::MakeErrorReply(TEXT("Failed to instantiate UNiagaraRendererProperties"));
    }
    if (Mat)
    {
        WriteRendererMaterial(NewRenderer, Mat);
    }
    // A Light starts as the dedicated light-renderer tool seeded it: affecting translucency and with its brightness
    // scaled by the particle alpha (the engine defaults are false for both). properties, merged next, overrides them.
    if (UNiagaraLightRendererProperties* NewLight = Cast<UNiagaraLightRendererProperties>(NewRenderer))
    {
        NewLight->bAffectsTranslucency = 1u;
        NewLight->bAlphaScalesBrightness = 1u;
    }

    // The supplied properties, merged by UPROPERTY name the way set_niagara_renderer_properties merges them. Every
    // field is converted before the renderer is added, so one that does not convert leaves the emitter untouched.
    TSharedPtr<FJsonObject> MergeJson = MakeShared<FJsonObject>();
    TArray<FString> AppliedProps;
    TArray<TSharedPtr<FJsonValue>> NotApplicable;
    FProperty* EnumPropForPostEdit = nullptr;
    if (PropertiesIn)
    {
        EnumPropForPostEdit = Niagara_CollectSuppliedProps(*PropertiesIn, GRendererPropKeys, RendererClass,
            &RendererPropNeedsPostEdit, MergeJson, AppliedProps, NotApplicable);
        // A numeric (non-enum) property must get a JSON number: the reflected conversion would read text as 0.
        for (const FString& PropName : AppliedProps)
        {
            const FNumericProperty* Num = CastField<FNumericProperty>(RendererClass->FindPropertyByName(FName(*PropName)));
            const TSharedPtr<FJsonValue> Value = MergeJson->TryGetField(PropName);
            if (Num && !Num->IsEnum() && Value.IsValid() && Value->Type != EJson::Number && Value->Type != EJson::Null)
            {
                // Named as the caller sent it (radius_scale), with the property it sets (RadiusScale).
                FString SentKey = PropName;
                for (const FNiagaraPropKey& K : GRendererPropKeys)
                {
                    if (PropName == K.PropName && (*PropertiesIn)->HasField(K.JsonKey))
                    {
                        SentKey = K.JsonKey;
                        break;
                    }
                }
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("properties.%s (%s) must be a JSON number; nothing was created"), *SentKey, *PropName));
            }
        }
        bool bFieldsChange = false;
        FString CheckError;
        if (!Niagara_CheckMergeFields(MergeJson, AppliedProps, NewRenderer, bFieldsChange, CheckError))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to apply renderer properties, nothing was created: %s"), *CheckError));
        }
        FString MergeError;
        if (AppliedProps.Num() > 0 && !mcp_common::MergeJsonWithClamp(MergeJson, RendererClass, NewRenderer, MergeError))
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Failed to apply renderer properties, nothing was created: %s"), *MergeError));
        }
    }

    const int32 IndexBefore = EmitterData->GetRenderers().Num();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraRenderer", "Add Niagara Renderer"));
    System->Modify();
    Emitter->AddRenderer(NewRenderer, EmitterData->Version.VersionGuid);

    // Enum writes drive derived state (the decal's UpdateSourceModeDerivates on SourceMode), which needs the renderer
    // in its emitter: notify it once it has been added.
    if (EnumPropForPostEdit)
    {
        FPropertyChangedEvent ChangeEvent(EnumPropForPostEdit, EPropertyChangeType::ValueSet);
        NewRenderer->PostEditChangeProperty(ChangeEvent);
    }
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // The reply reads the renderer back: its place in the emitter's list, the emitter it resolves and the values stored.
    const bool bVerified = Niagara_VerifiedRendererIndex(NewRenderer, EmitterData) != INDEX_NONE;
    TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
    DumpRendererProperties(NewRenderer, PropsObj);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("renderer_type"), RendererType);
    Data->SetStringField(TEXT("renderer_class"), NewRenderer->GetClass()->GetName());
    Data->SetNumberField(TEXT("renderer_index"), IndexBefore);
    Data->SetStringField(TEXT("material_path"), ReadRendererMaterialPath(NewRenderer));
    Data->SetArrayField(TEXT("applied"), mcp_common::StringsToJson(AppliedProps));
    Data->SetArrayField(TEXT("not_applicable"), NotApplicable);
    Data->SetBoolField(TEXT("post_edit_fired"), EnumPropForPostEdit != nullptr);
    Data->SetObjectField(TEXT("properties"), PropsObj);
    Data->SetNumberField(TEXT("renderer_count"), EmitterData->GetRenderers().Num());
    Data->SetBoolField(TEXT("verified"), bVerified);
    // The verdict is BOTH things: the renderer is in place AND the system reached disk.
    if (!bVerified)
    {
        mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Data, false,
            mcp_common::MeasuredOutcomeError(TEXT("add_niagara_renderer"), TEXT("verified")));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("add_niagara_renderer"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_niagara_renderer
// Removes the renderer at Index via the public UNiagaraEmitter::RemoveRenderer.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_renderer - Take the renderer at renderer_index (0-based, in get_niagara_emitter_renderers order) out of one emitter of a Niagara System asset, in one undo step, then request a system compile and save. renderer_index is required and must be a JSON number of 0 or more; an index out of range, an empty renderer slot, or an unknown system or emitter is an error and nothing changes. The renderers after it move down one index. A failed save makes success false, with saved=false and save_error, the removal kept in memory. Returns the class of the renderer taken out and the count left. Add one with add_niagara_renderer; change one with set_niagara_renderer_properties.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index into the emitter's renderers list, a JSON number (use get_niagara_emitter_renderers to find the index)
//   @returns { system_path: string, emitter_name: string, removed_renderer_index: int, removed_renderer_class: string, renderer_count_after: int, saved: bool, save_error?: string }

TSharedPtr<FJsonObject> HandleRemoveNiagaraRenderer(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("renderer_index"))) { return Mistyped; }
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
    // UNiagaraEmitter::RemoveRenderer dereferences the renderer to unbind it, so a null slot is refused here.
    if (!Target)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("renderer_index %d is null; nothing was removed"), RendererIndex));
    }
    FString RemovedClassName = Target->GetClass()->GetName();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraRenderer", "Remove Niagara Renderer"));
    System->Modify();
    Emitter->RemoveRenderer(Target, EmitterData->Version.VersionGuid);
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("removed_renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("removed_renderer_class"), RemovedClassName);
    Data->SetNumberField(TEXT("renderer_count_after"), EmitterData->GetRenderers().Num());
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_niagara_renderer"), SaveError);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_renderers
// Read-only enumeration of the emitter's renderers (via the const GetRenderers).
// ---------------------------------------------------------------------------
// MCP:get_niagara_emitter_renderers - List every renderer of one emitter in a Niagara System asset, in stack order, without changing anything: per renderer its 0-based index, class (Sprite, Mesh, Ribbon, Light, Decal...), object name and material path, the authored fields set_niagara_renderer_properties writes (bIsEnabled, SortOrderHint, MotionVectorSetting, bAllowInCullProxies plus the Sprite, Mesh, Light or Decal fields the class has) and, for a Mesh renderer, its static-mesh slots. Use it to find the renderer_index the renderer tools take; for every property of one renderer use get_niagara_renderer_info. Change a renderer with set_niagara_renderer_properties, set_niagara_renderer_material or set_niagara_mesh_renderer_mesh, add one with add_niagara_renderer, take one out with remove_niagara_renderer. An unknown system or emitter name is an error; an emitter with no renderers is a success with renderer_count 0.
//   @param system_path - Niagara System asset, package or object path, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle in that system, case ignored, e.g. 'Sparks', as get_niagara_emitter_handles lists it.
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
// get_niagara_simulation_stages
// Read-only enumeration of an emitter's simulation stages via
// the const FVersionedNiagaraEmitterData::GetSimulationStages() (NiagaraEmitter.h,
// inline; returns const TArray<UNiagaraSimulationStageBase*>&). Same
// resolution chain as HandleGetNiagaraEmitterRenderers, honest-empty arrays.
// An OPTIONAL stage_index / stage_name filter returns just the matching stage;
// no stage identifier => the full array.
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

// MCP:get_niagara_simulation_stages - Read the simulation stages of a Niagara emitter (read-only): every stage of FVersionedNiagaraEmitterData::GetSimulationStages(), or, with the OPTIONAL stage_index (preferred) or stage_name, just the matching stage. Each entry reports index, stage_name, class, enabled, and (for UNiagaraSimulationStageGeneric) iteration_source. Honest-empty when the emitter has no stages or the filter matches nothing.
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
    // identifier => the full array.
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
// MCP:set_niagara_renderer_material - Set the Material UPROPERTY on a Niagara renderer (Sprite, Ribbon, Mesh or Decal: any renderer class with a Material property).  Loads the material, sets it on the renderer's public Material field, requests a system compile and saves; a failed save makes success false, with saved=false and save_error. A renderer that already uses that material is a success with changed=false and saved=false, and nothing is written. A renderer_index that is not a JSON number, or a renderer class without a Material property (Light), is an error. Reads back to verify the material path is now bound, and returns the material it replaced.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index of the renderer (use get_niagara_emitter_renderers to find the index)
//   @param material_path - Asset path of the UMaterialInterface to assign.
//   @returns { system_path: string, emitter_name: string, renderer_index: int, renderer_class: string, previous_material_path: string, material_path: string, verified: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraRendererMaterial(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("renderer_index"))) { return Mistyped; }
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

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
    // The material bound before the write ("" when none).
    Data->SetStringField(TEXT("previous_material_path"), ReadRendererMaterialPath(R));

    // A renderer that already uses this material is a valid no-op: nothing is modified, dirtied or saved.
    // verified compares the bound material with the object loaded, so a package path counts as its object path.
    const FString MaterialObjectPath = Mat->GetPathName();
    if (ReadRendererMaterialPath(R) == MaterialObjectPath)
    {
        Data->SetStringField(TEXT("material_path"), MaterialObjectPath);
        Data->SetBoolField(TEXT("verified"), true);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The renderer already uses this material; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraRendererMaterial", "Set Niagara Renderer Material"));
    System->Modify();
    R->Modify();
    WriteRendererMaterial(R, Mat);
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    const FString ReadBackPath = ReadRendererMaterialPath(R);

    Data->SetStringField(TEXT("material_path"), ReadBackPath);
    Data->SetBoolField(TEXT("verified"), ReadBackPath == MaterialObjectPath);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_renderer_material"), SaveError);
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

// ---------------------------------------------------------------------------
// set_niagara_renderer_properties
// ---------------------------------------------------------------------------
// MCP:set_niagara_renderer_properties - Set properties of one renderer (Sprite, Mesh, Ribbon, Light or Decal) of a Niagara emitter, found by emitter_name and renderer_index, in one undo step, then request a system compile and save. Use it over set_properties, which neither finds the renderer nor applies ClampMin/ClampMax, compiles or saves. Only supplied fields are written; enum fields (alignment, facing_mode, sort_mode, motion_vector_setting, decal_source_mode) also fire PostEditChangeProperty. Every field is converted before the first write, so a value that does not convert (an unknown enum name, an array for a struct field) or a non-number renderer_index is an error and nothing changes. Values already held: changed=false, saved=false. Keys the renderer class lacks are listed in not_applicable. A failed save makes success false with save_error. Materials: set_niagara_renderer_material; mesh: set_niagara_mesh_renderer_mesh; read back: get_niagara_renderer_info.
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
//   @param sub_image_size - Optional {X,Y} object (Sprite/Mesh): SubImageSize; an [x,y] array is an error.
//   @param macro_uv_radius - Optional float (Sprite): MacroUVRadius.
//   @param pivot_in_uv_space - Optional {X,Y} object (Sprite): PivotInUVSpace; an [x,y] array is an error.
//   @param cast_shadows - Optional bool (Sprite): bCastShadows.
//   @param renderer_visibility - Optional int (Sprite): RendererVisibility tag value.
//   @param override_materials - Optional bool (Mesh): bOverrideMaterials.
//   @param mesh_bounds_scale - Optional {X,Y,Z} object (Mesh): MeshBoundsScale; an [x,y,z] array is an error.
//   @param locked_axis - Optional {X,Y,Z} object (Mesh): LockedAxis; an [x,y,z] array is an error.
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
//   @param color_add - Optional {X,Y,Z} object (Light): ColorAdd (FVector3f RGB shift); an [x,y,z] array is an error.
//   @param inverse_exposure_blend - Optional float (Light): InverseExposureBlend.
//   @param decal_screen_size_fade - Optional float (Decal): DecalScreenSizeFade.
//   @param decal_source_mode - Optional enum name/int (Decal): SourceMode ("Particles"/"Emitter"); fires PostEditChangeProperty so UpdateSourceModeDerivates runs.
//   @returns { system_path, emitter_name, renderer_index, renderer_class, applied:[prop names written], not_applicable:[input keys], post_edit_fired: bool, properties:{round-trip dump}, changed: bool, saved: bool, save_error?, note? }
TSharedPtr<FJsonObject> HandleSetNiagaraRendererProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("renderer_index"))) { return Mistyped; }
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

    // Every field is converted before the first write, so a field that does not convert leaves the renderer as it was.
    bool bChanged = false;
    FString CheckError;
    if (!Niagara_CheckMergeFields(MergeJson, AppliedProps, R, bChanged, CheckError))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to apply renderer properties, nothing was written: %s"), *CheckError));
    }

    // Values the renderer already holds are a valid no-op: nothing is modified, compiled or saved.
    if (!bChanged)
    {
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();
        DumpRendererProperties(R, PropsObj);
        TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
        Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
        Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
        Data->SetArrayField(TEXT("applied"), TArray<TSharedPtr<FJsonValue>>());
        Data->SetArrayField(TEXT("not_applicable"), NotApplicable);
        Data->SetBoolField(TEXT("post_edit_fired"), false);
        Data->SetObjectField(TEXT("properties"), PropsObj);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The renderer already holds these values; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
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
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

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
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_renderer_properties"), SaveError);
}

// ---------------------------------------------------------------------------
// set_niagara_mesh_renderer_mesh
// Assigns a UStaticMesh into a UNiagaraMeshRendererProperties::Meshes[] slot. The
// array element is a STRUCT (FNiagaraMeshRendererMeshProperties), so we resolve the
// slot (growing the array to it if needed) and set its inner .Mesh via reflection on
// the struct — the generic merge can't express this without replacing the whole array.
// Idempotency: if Meshes[slot].Mesh already equals the requested mesh, no write/grow.
// ---------------------------------------------------------------------------
// MCP:set_niagara_mesh_renderer_mesh - Assign a static mesh into a Niagara Mesh renderer's Meshes[] slot.  Resolves (or grows the array to) the slot index, loads the UStaticMesh, and sets the slot's inner .Mesh via reflection on the FNiagaraMeshRendererMeshProperties element. Then requests a system compile and saves; a failed save makes success false, with saved=false and save_error. add_niagara_renderer type=Mesh leaves Meshes empty (renders nothing); this is the command that makes a Mesh renderer actually draw. A renderer_index or slot that is not a JSON number is an error and nothing changes. Idempotent: re-assigning the same mesh to the same slot does not grow the array, writes nothing and saves nothing (already_assigned=true, saved=false).
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param renderer_index - 0-based index of the Mesh renderer (use get_niagara_emitter_renderers).
//   @param mesh_path - Asset path of the UStaticMesh to assign (e.g. "/Engine/BasicShapes/Cube").
//   @param slot - Optional 0-based slot index into Meshes[] (default 0). The array is grown with default elements up to this index if needed.
//   @returns { system_path, emitter_name, renderer_index, renderer_class, slot, mesh_path, mesh_count, already_assigned: bool, saved: bool, save_error? }
TSharedPtr<FJsonObject> HandleSetNiagaraMeshRendererMesh(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("renderer_index"))) { return Mistyped; }
    int32 RendererIndex = -1;
    if (!Params->TryGetNumberField(TEXT("renderer_index"), RendererIndex) || RendererIndex < 0)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing or invalid 'renderer_index' parameter"));
    }
    FString MeshPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("mesh_path"), MeshPath)) { return Missing; }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("slot"))) { return Mistyped; }
    int32 Slot = 0;
    if (Params->HasField(TEXT("slot")))
    {
        // Read into a bool, not negated in the if: slot is optional (default 0), and a negated read
        // whose branch returns would make the catalog publish it as required.
        const bool bSlotIsNumber = Params->TryGetNumberField(TEXT("slot"), Slot);
        if (!bSlotIsNumber || Slot < 0)
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
                Data->SetBoolField(TEXT("saved"), false);
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
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    const FString ReadBack = ReadMeshRendererSlotPath(R, Slot);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetNumberField(TEXT("renderer_index"), RendererIndex);
    Data->SetStringField(TEXT("renderer_class"), R->GetClass()->GetName());
    Data->SetNumberField(TEXT("slot"), Slot);
    Data->SetStringField(TEXT("mesh_path"), ReadBack);
    Data->SetNumberField(TEXT("mesh_count"), Helper.Num());
    Data->SetBoolField(TEXT("already_assigned"), false);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_mesh_renderer_mesh"), SaveError);
}

// ---------------------------------------------------------------------------
// get_niagara_emitter_data_interfaces
// The data interfaces an emitter's stacks use: every UNiagaraNodeInput of the emitter graph that holds a data
// interface object -- what the stack creates when a module's data-interface input is set (a curve, a mesh, a
// vector field...) -- with the stack(s) it feeds, found by walking the graph downstream to the output nodes.
// The emitter's Emitter Dependencies (FNiagaraDataInterfaceEmitterBinding: BindingMode + EmitterName) are a
// separate setting and come back as their own array.
// ---------------------------------------------------------------------------
namespace
{
    // The label of a stack's script usage, as the reply names it.
    const TCHAR* Niagara_StackUsageLabel(ENiagaraScriptUsage Usage)
    {
        switch (Usage)
        {
        case ENiagaraScriptUsage::ParticleSpawnScript:
        case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated: return TEXT("particle_spawn");
        case ENiagaraScriptUsage::ParticleUpdateScript:            return TEXT("particle_update");
        case ENiagaraScriptUsage::EmitterSpawnScript:              return TEXT("emitter_spawn");
        case ENiagaraScriptUsage::EmitterUpdateScript:             return TEXT("emitter_update");
        case ENiagaraScriptUsage::ParticleEventScript:             return TEXT("particle_event");
        case ENiagaraScriptUsage::ParticleSimulationStageScript:   return TEXT("simulation_stage");
        default:                                                   return TEXT("other");
        }
    }

    // The usages of the stacks Start feeds: a breadth-first walk over the links of its output pins, down to the
    // output nodes it reaches. Empty when the node is connected to no stack.
    TArray<FString> Niagara_DownstreamStackUsages(UEdGraphNode* Start)
    {
        TArray<FString> Usages;
        TArray<UEdGraphNode*> Queue;
        TSet<UEdGraphNode*> Visited;
        Queue.Add(Start);
        Visited.Add(Start);
        for (int32 Index = 0; Index < Queue.Num(); ++Index)
        {
            UEdGraphNode* Node = Queue[Index];
            if (const UNiagaraNodeOutput* Output = Cast<UNiagaraNodeOutput>(Node))
            {
                Usages.AddUnique(FString(Niagara_StackUsageLabel(Output->GetUsage())));
                continue;
            }
            for (UEdGraphPin* Pin : Node->Pins)
            {
                if (!Pin || Pin->Direction != EGPD_Output)
                {
                    continue;
                }
                for (UEdGraphPin* Linked : Pin->LinkedTo)
                {
                    UEdGraphNode* Next = Linked ? Linked->GetOwningNode() : nullptr;
                    if (Next && !Visited.Contains(Next))
                    {
                        Visited.Add(Next);
                        Queue.Add(Next);
                    }
                }
            }
        }
        return Usages;
    }
}

// MCP:get_niagara_emitter_data_interfaces - List the data interfaces one emitter of a Niagara System asset uses in its stacks: every data-interface input set on a module (a curve, a color curve, a static mesh, a vector field...), each with its input name, data interface class and object path and the stack it feeds (script_usage: particle_spawn, particle_update, emitter_spawn, emitter_update, particle_event, simulation_stage, or unconnected). Read from the emitter graph, so nothing needs compiling; read-only, it changes nothing. Data interfaces used only inside a module's own script are not listed. The emitter's Emitter Dependencies setting comes back separately as emitter_dependencies (emitter_name, binding_mode Self or Other). For the system's user-parameter data interfaces use get_niagara_data_interfaces (add_niagara_data_interface adds one); list_niagara_modules lists the modules. An emitter with none gives empty lists; a missing system or emitter handle is an error.
//   @param system_path - Content path of the UNiagaraSystem asset, package or object form, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle, as get_niagara_emitter_handles lists it, e.g. 'Fountain'.
//   @returns { system_path: string, emitter_name: string, data_interface_count: int, data_interfaces: [{ name, class, data_interface_path, script_usage, script_usages: [string] }], emitter_dependency_count: int, emitter_dependencies: [{ emitter_name, binding_mode }], note?: string }

TSharedPtr<FJsonObject> HandleGetNiagaraEmitterDataInterfaces(const TSharedPtr<FJsonObject>& Params)
{
    FString EmitterName;
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadEmitterData(Params, NiagaraNoEmitterDataTemplate, EmitterName, System, EmitterData))
    {
        return Error;
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);

    // The data-interface inputs of the stacks. UNiagaraNodeInput::GetDataInterface is not exported (the class is
    // MinimalAPI), so its DataInterface UPROPERTY is read through reflection.
    TArray<TSharedPtr<FJsonValue>> DIs;
    UNiagaraScriptSource* GraphSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
    UNiagaraGraph* EmitterGraph = GraphSource ? GraphSource->NodeGraph.Get() : nullptr;
    const FObjectProperty* DataInterfaceProperty =
        FindFProperty<FObjectProperty>(UNiagaraNodeInput::StaticClass(), TEXT("DataInterface"));
    if (EmitterGraph && DataInterfaceProperty)
    {
        TArray<UNiagaraNodeInput*> Inputs;
        EmitterGraph->GetNodesOfClass<UNiagaraNodeInput>(Inputs);
        for (UNiagaraNodeInput* Input : Inputs)
        {
            const UObject* DataInterface = Input ? DataInterfaceProperty->GetObjectPropertyValue_InContainer(Input) : nullptr;
            if (!DataInterface)
            {
                continue;
            }
            const TArray<FString> Usages = Niagara_DownstreamStackUsages(Input);
            TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();
            DObj->SetStringField(TEXT("name"), Input->Input.GetName().ToString());
            DObj->SetStringField(TEXT("class"), DataInterface->GetClass()->GetName());
            DObj->SetStringField(TEXT("data_interface_path"), DataInterface->GetPathName());
            DObj->SetStringField(TEXT("script_usage"), Usages.Num() > 0 ? Usages[0] : FString(TEXT("unconnected")));
            DObj->SetArrayField(TEXT("script_usages"), mcp_common::StringsToJson(Usages));
            DIs.Add(MakeShared<FJsonValueObject>(DObj));
        }
    }
    else if (!EmitterGraph)
    {
        Data->SetStringField(TEXT("note"), TEXT("The emitter has no editable script graph (a stateless emitter, or no GraphSource): no data interfaces can be read from it."));
    }
    Data->SetNumberField(TEXT("data_interface_count"), DIs.Num());
    Data->SetArrayField(TEXT("data_interfaces"), DIs);

    // The Emitter Dependencies setting: the emitters this one's particle scripts must run after.
    TArray<TSharedPtr<FJsonValue>> Dependencies;
    for (const FNiagaraDataInterfaceEmitterBinding& B : EmitterData->EmitterDependencies)
    {
        TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();
        DObj->SetStringField(TEXT("emitter_name"), B.EmitterName.ToString());
        DObj->SetStringField(TEXT("binding_mode"),
            B.BindingMode == ENiagaraDataInterfaceEmitterBindingMode::Self ? TEXT("Self") : TEXT("Other"));
        Dependencies.Add(MakeShared<FJsonValueObject>(DObj));
    }
    Data->SetNumberField(TEXT("emitter_dependency_count"), Dependencies.Num());
    Data->SetArrayField(TEXT("emitter_dependencies"), Dependencies);
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
        // First try as a class name in any loaded package. A short name such as "StaticMesh" or "Texture"
        // also names an engine class (UStaticMesh, UTexture), so only a data interface class is taken here;
        // anything else falls through to the /Script/Niagara lookup below.
        if (UClass* Found = FindFirstObject<UClass>(*InType))
        {
            if (Found->IsChildOf(UNiagaraDataInterface::StaticClass()))
            {
                return Found;
            }
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
// MCP:add_niagara_data_interface - Add a new UNiagaraDataInterface user-parameter binding to a Niagara system, inside an undo transaction, request a recompile and save the system; success requires the save. A user parameter that already has this name and class is left as is (added false, already_exists true, nothing saved); one with this name and any other type is an error. The inverse is remove_niagara_data_interface.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the new user-parameter (no "User." prefix)
//   @param data_interface_class - Data interface class name (e.g. "StaticMesh", "Texture"): a loaded data interface class with that name, else /Script/Niagara.NiagaraDataInterface<Name>
//   @returns { system_path: string, parameter_name: string, data_interface_class: string, data_interface_class_path?: string, offset?: int, added?: bool, already_exists?: bool, saved: bool, save_error?: string }

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

    // The store keys parameters by name AND type, so AddParameter would add a second user parameter of the
    // same name next to one of another type. The lookup is by name only: the same class is the state asked
    // for (nothing is written or saved), any other type is an error.
    FNiagaraUserRedirectionParameterStore& Exposed = System->GetExposedParameters();
    if (const FNiagaraVariableWithOffset* Existing = Exposed.FindParameterVariable(NewVar, /*IgnoreType=*/true))
    {
        const FNiagaraTypeDefinition ExistingType = Existing->GetType();
        if (ExistingType.GetClass() != DIClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("User parameter '%s' already exists with type '%s', not '%s'; remove it first to change its type"),
                *ParameterName, *ExistingType.GetName(), *DIClass->GetName()));
        }
        TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
        Result->SetStringField(TEXT("data_interface_class"), DIClass->GetName());
        Result->SetBoolField(TEXT("added"), false);
        Result->SetBoolField(TEXT("already_exists"), true);
        Result->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Result);
    }

    // The exposed store is a property of the system, so the system is what the transaction records.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "AddNiagaraDI", "Add Niagara Data Interface (MCP)"));
    System->Modify();

    int32 NewOffset = INDEX_NONE;
    const bool bAdded = Exposed.AddParameter(NewVar, /*bInitialize=*/true, /*bTriggerRebind=*/true, &NewOffset);
    if (!bAdded)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to add parameter '%s' of type '%s'"), *ParameterName, *DIClass->GetName()));
    }

    System->RequestCompile(true);
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("data_interface_class"), DIClass->GetName());
    Result->SetStringField(TEXT("data_interface_class_path"), DIClass->GetPathName());
    Result->SetNumberField(TEXT("offset"), NewOffset);
    Result->SetBoolField(TEXT("added"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("add_niagara_data_interface"), SaveError);
}

// ---------------------------------------------------------------------------
// remove_niagara_data_interface
// ---------------------------------------------------------------------------
// MCP:remove_niagara_data_interface - Remove a UNiagaraDataInterface user-parameter binding from a Niagara system by name, inside an undo transaction, request a recompile and save the system; success requires the save. A name with no user parameter, or a user parameter that is not a data interface, is an error and nothing is changed.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter to remove (no "User." prefix)
//   @returns { system_path: string, parameter_name: string, data_interface_class: string, removed: bool, saved: bool, save_error?: string }

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
    // The redirect store keys its TMap by short name and the value carries the
    // FULL "User.X" variable with the concrete DI subclass as its type. Our wire
    // payload only knows the short name + the base UNiagaraDataInterface type,
    // so the type-strict operator== on the TMap would fail. Use IgnoreType=true
    // for the name-only lookup, then rebuild the FNiagaraVariable from the
    // resolved entry so RemoveParameter sees the exact stored signature.
    const FNiagaraVariableWithOffset* Found =
        Exposed.FindParameterVariable(FNiagaraVariable(DITypeDef, FName(*ParameterName)), /*IgnoreType=*/true);
    if (!Found)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to remove parameter '%s' (not found)"), *ParameterName));
    }
    // The name-only lookup also finds user parameters of any other type; this tool only removes data interfaces.
    if (!Found->IsDataInterface())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("User parameter '%s' is a '%s', not a data interface; nothing was removed"),
            *ParameterName, *Found->GetType().GetName()));
    }
    const FNiagaraVariable VarToRemove(Found->GetType(), Found->GetName());
    const FString DIClassName = Found->GetType().GetName();

    // The exposed store is a property of the system, so the system is what the transaction records.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraDI", "Remove Niagara Data Interface (MCP)"));
    System->Modify();

    const bool bRemoved = Exposed.RemoveParameter(VarToRemove);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to remove parameter '%s' of type '%s'"), *ParameterName, *DIClassName));
    }

    System->RequestCompile(true);
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("data_interface_class"), DIClassName);
    Result->SetBoolField(TEXT("removed"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("remove_niagara_data_interface"), SaveError);
}

// MCP:remove_niagara_user_parameter - Remove one plain user parameter (the User. namespace: a float, int, bool, vector, color...) from a Niagara System asset, in one undo step; the asset is marked dirty, not saved, as set_niagara_user_parameter leaves it (save_asset keeps it). Inverse of set_niagara_user_parameter, which adds the parameter when it is missing. A data interface user parameter is refused (remove_niagara_data_interface removes those), and so is a name the system has no user parameter for, with nothing changed. The reply re-reads the store (still_present). Read: get_niagara_user_parameter.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - User parameter name (without the "User." prefix).
//   @returns { system_path: string, parameter_name: string, parameter_type: string, removed: bool, still_present: bool }

TSharedPtr<FJsonObject> HandleRemoveNiagaraUserParameter(const TSharedPtr<FJsonObject>& Params)
{
    FString ParameterName;
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadParameterSystem(Params, ParameterName, System))
    {
        return Error;
    }
    ParameterName.RemoveFromStart(TEXT("User."));

    FNiagaraUserRedirectionParameterStore& Exposed = System->GetExposedParameters();
    // The store keys its parameters by the full User. name with their type; a name-only lookup finds the entry
    // whatever its type, and the variable rebuilt from it is the exact signature RemoveParameter needs.
    const FNiagaraVariableWithOffset* Found =
        Exposed.FindParameterVariable(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), FName(*ParameterName)), /*IgnoreType=*/true);
    if (!Found)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The system has no user parameter '%s'; nothing was removed"), *ParameterName));
    }
    if (Found->IsDataInterface())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("User parameter '%s' is a data interface (%s); remove it with remove_niagara_data_interface. Nothing was removed."),
            *ParameterName, *Found->GetType().GetName()));
    }
    const FNiagaraVariable VarToRemove(Found->GetType(), Found->GetName());
    const FString TypeName = Found->GetType().GetName();

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraUserParameter", "Remove Niagara User Parameter"));
    System->Modify();
    const bool bRemoved = Exposed.RemoveParameter(VarToRemove);
    if (!bRemoved)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to remove user parameter '%s' of type '%s'"), *ParameterName, *TypeName));
    }
    System->MarkPackageDirty();

    const bool bStillPresent = Exposed.FindParameterVariable(VarToRemove, /*IgnoreType=*/true) != nullptr;
    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("parameter_type"), TypeName);
    Result->SetBoolField(TEXT("removed"), !bStillPresent);
    Result->SetBoolField(TEXT("still_present"), bStillPresent);
    return mcp_common::MakeOutcomeReply(Result, !bStillPresent,
        bStillPresent ? mcp_common::MeasuredOutcomeError(TEXT("remove_niagara_user_parameter"), TEXT("removed")) : FString());
}

// ---------------------------------------------------------------------------
// get_niagara_data_interfaces  - list all user-param DIs on the system.
// ---------------------------------------------------------------------------
// MCP:get_niagara_data_interfaces - List the data interfaces bound to a Niagara System's user parameters (the User.* parameters of a data interface type, such as a static mesh or texture DI), or only the one named parameter_name, without changing anything: per binding its user parameter, the DI object and class, and the name and property class of each UPROPERTY (Transient and DuplicateTransient ones skipped; names and types only, no values). This is the system-level list: for the DIs set on module inputs inside one emitter's stacks use get_niagara_emitter_data_interfaces, and for every user parameter, DI or not, get_niagara_system_info. Add a DI with add_niagara_data_interface, point it at an asset with set_niagara_data_interface_asset, set its other fields with set_niagara_data_interface_default. A parameter_name that is not a data interface user parameter of the system, or a system that does not load, is an error; a system with none is a success with count 0. parameter_name in the reply is the short name the other data interface tools take; index is the DI's slot in the system's exposed-parameter store.
//   @param system_path - Niagara System asset, package or object path, e.g. '/Game/FX/NS_Sparks' or '/Game/FX/NS_Sparks.NS_Sparks'.
//   @param parameter_name - Short name of one data interface user parameter, e.g. 'SourceMesh'; a leading "User." is dropped and case is ignored. Omit it to list every binding.
//   @returns { system_path: string, count: int, data_interfaces: [{ index, parameter_name, object_name, data_interface_class, data_interface_class_path, path_name, properties: [{ name, type }], property_count: int }] }

TSharedPtr<FJsonObject> HandleGetNiagaraDataInterfaces(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    // Optional single-binding filter by the user parameter's short name.
    FString FilterName;
    Params->TryGetStringField(TEXT("parameter_name"), FilterName);
    FilterName.RemoveFromStart(TEXT("User."));

    const FNiagaraUserRedirectionParameterStore& Exposed = System->GetExposedParameters();
    const TArray<UNiagaraDataInterface*>& DIs = Exposed.GetDataInterfaces();

    // The user parameter that owns each data interface, by the DI's offset in the store's data interface array.
    // The store names it "User.<name>"; the other data interface tools take the short name, as
    // get_niagara_system_info lists it, so that is what parameter_name carries.
    TMap<int32, FString> ParameterNameByOffset;
    for (const FNiagaraVariableWithOffset& Var : Exposed.ReadParameterVariables())
    {
        if (Var.IsDataInterface())
        {
            FString ShortName = Var.GetName().ToString();
            ShortName.RemoveFromStart(TEXT("User."));
            ParameterNameByOffset.Add(Var.Offset, ShortName);
        }
    }

    TArray<TSharedPtr<FJsonValue>> DIArray;
    int32 Idx = 0;
    for (UNiagaraDataInterface* DI : DIs)
    {
        // A slot no user parameter points at is not a binding, and no other tool can reach it.
        const FString* ParameterName = ParameterNameByOffset.Find(Idx);
        if (!DI || !ParameterName || (!FilterName.IsEmpty() && !ParameterName->Equals(FilterName, ESearchCase::IgnoreCase)))
        {
            ++Idx;
            continue;
        }
        TSharedPtr<FJsonObject> DObj = MakeShared<FJsonObject>();
        DObj->SetNumberField(TEXT("index"), Idx);
        DObj->SetStringField(TEXT("parameter_name"), *ParameterName);
        DObj->SetStringField(TEXT("object_name"), DI->GetName());
        DObj->SetStringField(TEXT("data_interface_class"), DI->GetClass()->GetName());
        DObj->SetStringField(TEXT("data_interface_class_path"), DI->GetClass()->GetPathName());
        DObj->SetStringField(TEXT("path_name"), DI->GetPathName());
        // Every UPROPERTY of the DI, Transient and DuplicateTransient ones skipped: a light introspection of its state.
        TArray<TSharedPtr<FJsonValue>> PropsArray;
        for (TFieldIterator<FProperty> It(DI->GetClass()); It; ++It)
        {
            const FProperty* Prop = *It;
            if (!Prop || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient))
            {
                continue;
            }
            TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
            PObj->SetStringField(TEXT("name"), Prop->GetName());
            PObj->SetStringField(TEXT("type"), Prop->GetClass()->GetName());
            PropsArray.Add(MakeShared<FJsonValueObject>(PObj));
        }
        DObj->SetArrayField(TEXT("properties"), PropsArray);
        DObj->SetNumberField(TEXT("property_count"), PropsArray.Num());
        DIArray.Add(MakeShared<FJsonValueObject>(DObj));
        ++Idx;
    }
    if (!FilterName.IsEmpty() && DIArray.Num() == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Data interface not found: %s"), *FilterName));
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
// MCP:set_niagara_data_interface_asset - Points an object property of a user-parameter data interface on a Niagara system at an asset (e.g. the mesh of a StaticMesh DI, the texture of a Texture DI), inside an undo transaction, reads it back, requests a system recompile and saves the system; success requires the read-back and the save. The DI must already exist (add_niagara_data_interface); property_name picks the property, else the first hard object property in reflection order. The asset must load and be of the property's class, else it is an error and nothing is written. For non-asset fields use set_niagara_data_interface_default.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter DI to mutate
//   @param asset_path - Asset path of the UObject to assign (must be the right subclass)
//   @param property_name - Optional FObjectProperty name on the DI; defaults to the first FObjectProperty
//   @returns { system_path: string, parameter_name: string, property_name: string, asset_path: string, set: bool (the property reads back the asset), saved: bool, save_error?: string }

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

    // The DI is a subobject of the system: both are recorded so the edit can be undone, and the system's
    // package is what gets saved.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraDIAsset", "Set Niagara DI Asset (MCP)"));
    System->Modify();
    DI->Modify();

    void* ValuePtr = ConcreteObjProp->ContainerPtrToValuePtr<void>(DI);
    ConcreteObjProp->SetObjectPropertyValue(ValuePtr, Asset);
    const bool bSet = ConcreteObjProp->GetObjectPropertyValue(ValuePtr) == Asset;
    System->RequestCompile(true);
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("property_name"), ObjProp->GetName());
    Result->SetStringField(TEXT("asset_path"), Asset->GetPathName());
    Result->SetBoolField(TEXT("set"), bSet);
    if (!bSet)
    {
        mcp_common::AddSaveOutcome(Result, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Result, false, FString::Printf(
            TEXT("set_niagara_data_interface_asset: property '%s' does not read back '%s'"),
            *ObjProp->GetName(), *Asset->GetPathName()));
    }
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_niagara_data_interface_asset"), SaveError);
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_default
// Set an internal default property on the DI (e.g. a FVector / float scalar
// on the DI's user-exposed defaults, distinct from the bound asset above).
// Uses ImportText_Direct against an arbitrary property by name.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_default - Writes one property of a user-parameter data interface on a Niagara system from text parsed by the property's ImportText, so any type it accepts works ("1.5", "(X=1,Y=2,Z=3)", "(R=1,G=0,B=0,A=1)", an enum name), inside an undo transaction, then requests a system recompile and saves the system; success requires the save. A missing DI or property, or text that does not parse, is an error and the DI is left untouched. value echoes the request; value_read_back is the stored value exported as text. List the DI's properties with get_niagara_data_interfaces; for asset references use set_niagara_data_interface_asset.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter DI to mutate
//   @param property_name - Name of the FProperty on the DI to set
//   @param value - String representation of the value (parsed by FProperty::ImportText_Direct)
//   @returns { system_path: string, parameter_name: string, property_name: string, value: string, value_read_back: string, set: bool, saved: bool, save_error?: string }

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

    // Parse into a copy first: ImportText_Direct can write part of a struct before it fails, and a value that
    // does not parse must leave the DI untouched. FProperty has ImportText_Direct, not ImportText.
    void* Parsed = Prop->AllocateAndInitializeValue();
    Prop->CopySingleValue(Parsed, Addr);
    FString ParseError;
    if (!mcp_common::ImportPropertyTextStrict(Prop, Value, Parsed, DI, ParseError))
    {
        Prop->DestroyAndFreeValue(Parsed);
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to parse value: %s"), *ParseError));
    }

    // The DI is a subobject of the system: both are recorded so the edit can be undone, and the system's
    // package is what gets saved.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraDIDefault", "Set Niagara DI Default (MCP)"));
    System->Modify();
    DI->Modify();
    Prop->CopySingleValue(Addr, Parsed);
    Prop->DestroyAndFreeValue(Parsed);

    FString ReadBack;
    Prop->ExportTextItem_Direct(ReadBack, Addr, nullptr, DI, PPF_None);
    System->RequestCompile(true);
    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    TSharedPtr<FJsonObject> Result = Niagara_StartParameterReply(System, ParameterName);
    Result->SetStringField(TEXT("property_name"), Prop->GetName());
    Result->SetStringField(TEXT("value"), Value);
    Result->SetStringField(TEXT("value_read_back"), ReadBack);
    Result->SetBoolField(TEXT("set"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_niagara_data_interface_default"), SaveError);
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_curve  - author the FRichCurve keys on a curve DI.
// Generic over the curve DI family: the caller names the FRichCurve UPROPERTY
// (default "Curve" for UNiagaraDataInterfaceCurve; "RedCurve"/"GreenCurve"/...
// for ColorCurve, "XCurve"/"YCurve"/"ZCurve" for VectorCurve). We resolve it by
// reflection, replace its keys, then rebuild the DI's LUT via the curve base.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_curve - Replaces all keys of one FRichCurve property on a curve user-parameter data interface of a Niagara system with the given time/value keys (an empty array clears it), rebuilds the DI's lookup table, requests a recompile and saves the system, inside an undo transaction that records the DI; success requires the save. The DI must already exist (add_niagara_data_interface) and curve_property must name an FRichCurve on it: Curve by default, XCurve/YCurve/ZCurve (WCurve too on Vector4) on vector curve DIs, RedCurve/GreenCurve/BlueCurve/AlphaCurve on color curve DIs. Every key must be an object whose time and value are finite JSON numbers, or it is an error and nothing changes. For array DIs use set_niagara_data_interface_array.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter curve DI (no "User." prefix)
//   @param keys - Array of { time: number, value: number } keys to set (replaces all existing keys on the channel)
//   @param curve_property - Optional FRichCurve UPROPERTY to author (default "Curve"; e.g. "RedCurve"/"XCurve" on Color/Vector curve DIs)
//   @returns { system_path: string, parameter_name: string, curve_property: string, key_count: int, keys: [{time,value}], set: bool, saved: bool, save_error?: string }
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

    // Every key must be an object whose time and value are finite JSON numbers, checked before the curve is reset:
    // a lenient read would skip the key or store 0 (or a bool as 0 or 1) in its place.
    for (int32 i = 0; i < KeysArr->Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& KV = (*KeysArr)[i];
        const TSharedPtr<FJsonObject>* KeyObj = nullptr;
        bool bKeyOk = KV.IsValid() && KV->TryGetObject(KeyObj) && KeyObj && KeyObj->IsValid();
        for (const TCHAR* Field : { TEXT("time"), TEXT("value") })
        {
            const TSharedPtr<FJsonValue> Number = bKeyOk ? (*KeyObj)->TryGetField(Field) : nullptr;
            bKeyOk = bKeyOk && Number.IsValid() && Number->Type == EJson::Number && FMath::IsFinite(Number->AsNumber());
        }
        if (!bKeyOk)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("keys[%d] is not an object whose time and value are finite JSON numbers; nothing was changed"), i));
        }
    }

    // The curve lives on the DI, a subobject of the system: both are recorded so the edit can be undone.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraDICurve", "Set Niagara DI Curve (MCP)"));
    System->Modify();
    DI->Modify();

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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_niagara_data_interface_curve"), SaveError);
}

// ---------------------------------------------------------------------------
// set_niagara_data_interface_array  - set the numeric values of an array DI.
// Generic over numeric array DIs (UNiagaraDataInterfaceArrayFloat::FloatData,
// the int32 array DI's IntData, ...). We resolve the FArrayProperty by name and
// require a numeric inner; struct-element arrays (Vector/Color/Position) carry an
// LWC editor/runtime split and are rejected with an honest error.
// ---------------------------------------------------------------------------
// MCP:set_niagara_data_interface_array - Replace the contents of a numeric array on an array user-parameter data interface of a Niagara System asset (FloatData by default, IntData on the int32 array DI) with values, in an undo transaction that records the DI, then request a recompile and save the system; success requires the save. Every value must be a finite JSON number; an int array stores it truncated toward zero. A value of another type, an unknown system or data interface, an array_property the DI lacks, or an array of structs (Vector, Color, Position) is an error and nothing changes. There is no no-op check: the same values are written and saved again. The reply reads the stored array back. The DI must exist (add_niagara_data_interface); curve DIs: set_niagara_data_interface_curve; inspect it with get_niagara_data_interfaces.
//   @param system_path - Asset path of the Niagara System.
//   @param parameter_name - Short name of the user-parameter array DI (no "User." prefix)
//   @param values - Array of finite JSON numbers to store (replaces all existing array contents); any other element is an error
//   @param array_property - Optional array UPROPERTY to set (default "FloatData"; e.g. "IntData" for the int32 array DI)
//   @returns { system_path: string, parameter_name: string, array_property: string, element_type: string, count: int, values: [number], set: bool, saved: bool, save_error?: string }
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

    // Every value must be a finite JSON number, checked before anything changes: TryGetNumber leaves 0 for another
    // JSON type (and reads a bool as 0 or 1), which would be stored in its place.
    for (int32 i = 0; i < ValuesArr->Num(); ++i)
    {
        const TSharedPtr<FJsonValue>& Item = (*ValuesArr)[i];
        if (!Item.IsValid() || Item->Type != EJson::Number || !FMath::IsFinite(Item->AsNumber()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("values[%d] is not a finite JSON number; nothing was changed"), i));
        }
    }

    // The array lives on the DI, a subobject of the system: both are recorded so the edit can be undone.
    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraDIArray", "Set Niagara DI Array (MCP)"));
    System->Modify();
    DI->Modify();

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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("set_niagara_data_interface_array"), SaveError);
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
//   (read back by get_niagara_system_info: GetWarmupTime() + GetWarmupTickCount() + ...)
//   set_niagara_emitter_template_description
//                                         UNiagaraEmitter::TemplateAssetDescription (FText)
// ========================================================================== //

namespace
{
    // Whether UNiagaraSystem::ResolveWarmupTickCount, run with warmup time Time and tick delta Delta, would leave the
    // system's warmup time, tick count and tick delta exactly as they are. It repeats the engine's arithmetic in
    // float: a delta near zero becomes 0 with a count of 0 (the time kept), otherwise the count is
    // floor(Time / Delta) and the time is rounded down to whole ticks.
    bool Niagara_WarmupUnchanged(const UNiagaraSystem* System, float Time, float Delta)
    {
        float NewTime = Time;
        int32 NewCount = 0;
        float NewDelta = 0.0f;
        if (!FMath::IsNearlyZero(Delta))
        {
            NewCount = FMath::FloorToInt(Time / Delta);
            NewTime = Delta * NewCount;
            NewDelta = Delta;
        }
        return NewTime == System->GetWarmupTime() && NewCount == System->GetWarmupTickCount()
            && NewDelta == System->GetWarmupTickDelta();
    }
}

// MCP:set_niagara_system_warmup_time - Set the warmup time in seconds (simulated ahead when the system activates; 0 turns warmup off) of a Niagara System asset in one undo step, then save the asset. The engine re-derives warmup_tick_count as floor(warmup_time / warmup_tick_delta) and rounds the time down to whole ticks, so a time that is not a multiple of the delta comes back shorter in new_warmup_time; with a delta of 0 no warmup runs. A time that is not a finite JSON number of 0 or more, a missing param or an unknown system is an error and nothing changes. A time that leaves the warmup settings as they are is a success with changed=false and saved=false, nothing written. A failed save makes success false with saved=false and save_error, the change kept in memory. Set the delta first with set_niagara_system_warmup_tick_delta; read back with get_niagara_system_info.
//   @retired set_properties - object_path=<system>, properties={"WarmupTime":..}, save=true (the engine recomputes WarmupTickCount)
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire"; the call fails without it.
//   @param warmup_time - Warmup time in seconds (>= 0; 0 disables warmup); the call fails without it.
//   @returns { system_path: string, old_warmup_time: number, new_warmup_time: number, warmup_tick_count: int, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemWarmupTime(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("warmup_time"))) { return Mistyped; }
    double WarmupTimeValue = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("warmup_time"), WarmupTimeValue)) { return Missing; }
    const float WarmupTime = static_cast<float>(WarmupTimeValue);
    // The engine stores any float, but a negative time has no meaning and a non-finite one breaks the tick count.
    if (!FMath::IsFinite(WarmupTime) || WarmupTime < 0.0f)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("warmup_time must be a finite number of seconds >= 0 (got %g); nothing was changed"), WarmupTimeValue));
    }

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const float OldTime = System->GetWarmupTime();
    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("old_warmup_time"), OldTime);

    // A time that SetWarmupTime would resolve to the settings already in place is a valid no-op: nothing is
    // modified, dirtied or saved.
    if (Niagara_WarmupUnchanged(System, WarmupTime, System->GetWarmupTickDelta()))
    {
        Data->SetNumberField(TEXT("new_warmup_time"), System->GetWarmupTime());
        Data->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The warmup settings already resolve to this time; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemWarmupTime", "Set Niagara System Warmup Time (MCP)"));
    System->Modify();
    System->SetWarmupTime(WarmupTime);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetNumberField(TEXT("new_warmup_time"), System->GetWarmupTime());
    Data->SetNumberField(TEXT("warmup_tick_count"), System->GetWarmupTickCount());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_warmup_time"), SaveError);
}

// MCP:set_niagara_system_warmup_tick_count - Recompute the warmup tick count of a Niagara System asset, found by asset path, from its current warmup time and tick delta, in one undo step, then save the asset; it takes no count. The engine stores floor(warmup_time / warmup_tick_delta) and rounds warmup_time down to whole ticks; a delta near zero is reset to 0 with a count of 0, so no warmup runs. Settings that are already consistent are a success with changed=false and saved=false, nothing written. An unknown system is an error; a failed save makes success false with saved=false and save_error, the change kept in memory. For N ticks, set the delta with set_niagara_system_warmup_tick_delta, then N times it with set_niagara_system_warmup_time and check warmup_tick_count; read back with get_niagara_system_info.
//   @retired set_properties - object_path=<system>, properties={"WarmupTime":..} or {"WarmupTickDelta":..}, save=true: the engine recomputes WarmupTickCount from them
//   @param system_path - Asset path of the Niagara System.
//   @returns { system_path: string, old_warmup_tick_count: int, new_warmup_tick_count: int, warmup_time: number, warmup_tick_delta: number, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemWarmupTickCount(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    // UNiagaraSystem::WarmupTickCount is protected (NiagaraSystem.h) and has no setter: the
    // public UNiagaraSystem::ResolveWarmupTickCount derives it from WarmupTime and
    // WarmupTickDelta. That is what this command runs; for another count, callers set the
    // warmup time with set_niagara_system_warmup_time and let the engine derive it.
    const int32 OldCount = System->GetWarmupTickCount();
    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("old_warmup_tick_count"), OldCount);

    // Settings that ResolveWarmupTickCount would leave as they are form a valid no-op: nothing is modified,
    // dirtied or saved.
    if (Niagara_WarmupUnchanged(System, System->GetWarmupTime(), System->GetWarmupTickDelta()))
    {
        Data->SetNumberField(TEXT("new_warmup_tick_count"), OldCount);
        Data->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
        Data->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The warmup tick count is already consistent; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ResolveNiagaraSystemWarmupTickCount", "Resolve Niagara System Warmup Tick Count (MCP)"));
    System->Modify();
    // Re-resolve from the current WarmupTime + WarmupTickDelta pair so the
    // protected WarmupTickCount field reflects a coherent state.
    System->ResolveWarmupTickCount();

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetNumberField(TEXT("new_warmup_tick_count"), System->GetWarmupTickCount());
    Data->SetNumberField(TEXT("warmup_time"), System->GetWarmupTime());
    Data->SetNumberField(TEXT("warmup_tick_delta"), System->GetWarmupTickDelta());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_warmup_tick_count"), SaveError);
}

// MCP:set_niagara_system_warmup_tick_delta - Set the warmup tick delta (seconds simulated per warmup tick) of a Niagara System asset in one undo step, then save it. The engine re-derives the tick count as floor(warmup_time / delta) and rounds warmup_time down to a whole number of ticks, so a delta that does not divide the warmup time shortens it and one larger than it zeroes the warmup; the reply does not show this, so read warmup_time and warmup_tick_count back with get_niagara_system_info. A delta that leaves the warmup settings as they are is a success with changed=false and saved=false, nothing written. A missing delta, one that is not a finite JSON number above 0, or an unknown system, is an error and nothing changes; a failed save makes success false with saved=false and save_error. Set the delta before calling set_niagara_system_warmup_time.
//   @retired set_properties - object_path=<system>, properties={"WarmupTickDelta":..}, save=true (the engine recomputes WarmupTickCount)
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire" (the object path form also works).
//   @param warmup_tick_delta - Seconds per warmup tick, > 0, e.g. 0.0333 for 30 ticks per second; the call fails without it.
//   @returns { system_path: string, old_warmup_tick_delta: number, new_warmup_tick_delta: number, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemWarmupTickDelta(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("warmup_tick_delta"))) { return Mistyped; }
    double TickDeltaValue = 0.0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("warmup_tick_delta"), TickDeltaValue)) { return Missing; }
    const float TickDelta = static_cast<float>(TickDeltaValue);
    if (!FMath::IsFinite(TickDelta) || TickDelta <= 0.0f)
    {
        return mcp_common::MakeErrorReply(TEXT("warmup_tick_delta must be a finite number > 0"));
    }

    FString Error;
    UNiagaraSystem* System = Niagara_ResolveSystem(SystemPath, Error);
    if (!System)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    const float OldDelta = System->GetWarmupTickDelta();
    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetNumberField(TEXT("old_warmup_tick_delta"), OldDelta);

    // A delta that SetWarmupTickDelta would resolve to the settings already in place is a valid no-op: nothing is
    // modified, dirtied or saved.
    if (Niagara_WarmupUnchanged(System, System->GetWarmupTime(), TickDelta))
    {
        Data->SetNumberField(TEXT("new_warmup_tick_delta"), OldDelta);
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The warmup settings already resolve to this delta; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemWarmupTickDelta", "Set Niagara System Warmup Tick Delta (MCP)"));
    System->Modify();
    System->SetWarmupTickDelta(TickDelta);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetNumberField(TEXT("new_warmup_tick_delta"), System->GetWarmupTickDelta());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_warmup_tick_delta"), SaveError);
}

// MCP:set_niagara_system_deterministic - Set the system-wide determinism flag (bDeterminism, written by reflection) of a Niagara System asset, found by asset path, in one undo step, then save the asset. When true, every instance seeds its random numbers from the system's RandomSeed plus the component's RandomSeedOffset, so runs are repeatable; when false, each instance draws a new seed on reset. deterministic is required. A value the system already holds is a success with changed=false and saved=false, nothing written. A missing param or unknown system is an error; a failed save (for example while PIE runs) makes success false with saved=false and save_error, the change kept in memory. Read it back with get_niagara_system_info; one emitter's determinism: set_niagara_emitter_sim_target.
//   @retired set_properties - object_path=<system>, properties={"bDeterminism":true,"RandomSeed":..}, save=true
//   @param system_path - Asset path of the Niagara System.
//   @param deterministic - Boolean to assign to bDeterminism
//   @returns { system_path: string, old_deterministic: bool, new_deterministic: bool, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraSystemDeterministic(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    if (!ExtractNiagaraSystemPath(Params, SystemPath))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'system_path' parameter"));
    }
    bool bDeterministic = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("deterministic"), bDeterministic)) { return Missing; }

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

    const bool OldValue = System->NeedsDeterminism();
    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);
    Data->SetBoolField(TEXT("old_deterministic"), OldValue);

    // A value the system already holds is a valid no-op: nothing is modified, dirtied or saved.
    if (OldValue == bDeterministic)
    {
        Data->SetBoolField(TEXT("new_deterministic"), OldValue);
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The system already has this determinism; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemDeterministic", "Set Niagara System Deterministic (MCP)"));
    System->Modify();
    DetProp->SetPropertyValue_InContainer(System, bDeterministic);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetBoolField(TEXT("new_deterministic"), System->NeedsDeterminism());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_deterministic"), SaveError);
}

// MCP:set_niagara_emitter_template_description - Set the TemplateAssetDescription text (shown when the emitter is picked as a template) of a standalone Niagara Emitter asset, not an emitter inside a system, in one undo step, then save the asset. description is stored as FText; an empty string clears it. A description the property system already finds identical is a success with changed=false and saved=false, nothing written. An unknown emitter asset or a missing param is an error. A failed save makes success false, with saved=false and save_error, the change kept in memory. Returns old_description and new_description as read back. Emitters inside a system: get_niagara_emitter_handles; add one from an emitter asset with add_niagara_emitter_from_template.
//   @retired set_properties - object_path=<emitter asset>, properties={"TemplateAssetDescription":"..."}, save=true
//   @param emitter_asset_path - Asset path of the UNiagaraEmitter, e.g. "/Game/FX/NE_Sparks" (an emitter asset, not a system path).
//   @param description - New description text, stored as FText; an empty string clears it.
//   @returns { emitter_asset_path: string, old_description: string, new_description: string, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

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
    const FText NewDescription = FText::FromString(DescriptionText);
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("emitter_asset_path"), Emitter->GetPathName());
    Data->SetStringField(TEXT("old_description"), OldDescription.ToString());

    // A description the property system finds identical to the one held (FTextProperty's own comparison) is a
    // valid no-op: nothing is modified, dirtied or saved.
    if (FTextProperty::Identical_Implementation(OldDescription, NewDescription, PPF_None))
    {
        Data->SetStringField(TEXT("new_description"), OldDescription.ToString());
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter already has this description; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterTemplateDescription", "Set Niagara Emitter Template Description (MCP)"));
    Emitter->Modify();
    Emitter->TemplateAssetDescription = NewDescription;

    bool bSaved = false;
    FString SaveError;
    if (UPackage* Package = Emitter->GetPackage())
    {
        bSaved = mcp_common::SaveAssetPackage(Package, Emitter, SaveError);
    }

    Data->SetStringField(TEXT("new_description"), Emitter->TemplateAssetDescription.ToString());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_template_description"), SaveError);
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

// MCP:set_niagara_emitter_bounds - Set how one emitter of a Niagara System asset, found by handle name, computes its bounds (bounds_mode Dynamic, Fixed or Programmable, case-insensitive, default Fixed) and optionally its fixed box, in one undo step, then save the system. bounds_min and bounds_max are FVector strings like "X=-50 Y=-50 Z=0", not arrays; one alone keeps the other corner. With neither, Fixed resets the box to the engine default (-100 to 100 per axis) and the other modes keep it; min <= max is not checked. An unknown mode, system or emitter, or a corner without X=, Y= and Z=, is an error and nothing changes. A mode and box already in place are a success with changed=false and saved=false, nothing written. A failed save makes success false, with saved=false and save_error. Read back with get_niagara_emitter_runtime_summary; for a placed Niagara component use set_niagara_component_fixed_bounds.
//   @retired set_niagara_emitter_properties - bounds_mode, bounds_min, bounds_max
//   @param system_path - Niagara System asset path, e.g. "/Game/FX/NS_Fire" (the object path form also works).
//   @param emitter_name - Emitter handle name as shown in the system, e.g. "Sparks" (case-insensitive).
//   @param bounds_mode - One of "Dynamic", "Fixed", or "Programmable"; defaults to "Fixed"
//   @param bounds_min - Optional FixedBounds minimum corner as an FVector string, e.g. "X=-50 Y=-50 Z=0" (the format the reply echoes); all three X=, Y= and Z= keys are needed, so "[x, y, z]" is an error.
//   @param bounds_max - Optional FixedBounds maximum corner as an FVector string, e.g. "X=50 Y=50 Z=100"; same format and rules as bounds_min.
//   @returns { system_path: string, emitter_name: string, bounds_mode: string, bounds_min: string, bounds_max: string, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

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
    // InitFromString reads a corner only when the string carries all of X=, Y= and Z=; anything else
    // ("[x, y, z]" included) is refused here, before the system is touched, instead of being written as zeros.
    if (Params->TryGetStringField(TEXT("bounds_min"), MinStr) && !MinStr.IsEmpty())
    {
        if (!NewMin.InitFromString(MinStr))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("bounds_min '%s' is not an FVector string with X=, Y= and Z= (e.g. \"X=-50 Y=-50 Z=0\"); nothing was changed"),
                *MinStr));
        }
        bHasMin = true;
    }
    if (Params->TryGetStringField(TEXT("bounds_max"), MaxStr) && !MaxStr.IsEmpty())
    {
        if (!NewMax.InitFromString(MaxStr))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("bounds_max '%s' is not an FVector string with X=, Y= and Z= (e.g. \"X=50 Y=50 Z=100\"); nothing was changed"),
                *MaxStr));
        }
        bHasMax = true;
    }

    // The emitter data lives in the handle's UNiagaraEmitter, not in the system, so the emitter is what the
    // transaction must record for an undo to restore the bounds.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The box the call leaves: any supplied corners with the current counterpart (so partial updates work), the
    // engine default for Fixed with no corner, or the current box for the other modes.
    const FBox OldBox = EmitterData->FixedBounds;
    FBox NewBox = OldBox;
    if (bHasMin || bHasMax)
    {
        NewBox = FBox(bHasMin ? NewMin : OldBox.Min, bHasMax ? NewMax : OldBox.Max);
    }
    else if (NewMode == ENiagaraEmitterCalculateBoundMode::Fixed)
    {
        NewBox = FVersionedNiagaraEmitterData::GetDefaultFixedBounds();
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("bounds_mode"), BoundsModeStr);

    // A mode and box already in place are a valid no-op: nothing is modified, dirtied or saved. FBox::operator==
    // ignores IsValid, which the write also sets, so it is compared apart.
    if (EmitterData->CalculateBoundsMode == NewMode && OldBox.Min == NewBox.Min && OldBox.Max == NewBox.Max
        && OldBox.IsValid == NewBox.IsValid)
    {
        Data->SetStringField(TEXT("bounds_min"), OldBox.Min.ToString());
        Data->SetStringField(TEXT("bounds_max"), OldBox.Max.ToString());
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter already has these bounds; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterBounds", "Set Niagara Emitter Bounds"));
    System->Modify();
    Emitter->Modify();
    EmitterData->CalculateBoundsMode = NewMode;
    EmitterData->FixedBounds = NewBox;

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetStringField(TEXT("bounds_min"), EmitterData->FixedBounds.Min.ToString());
    Data->SetStringField(TEXT("bounds_max"), EmitterData->FixedBounds.Max.ToString());
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_bounds"), SaveError);
}

// MCP:set_niagara_emitter_allocation_mode - Set how one emitter of a Niagara System asset, found by handle name, sizes its particle buffers (EParticleAllocationMode AutomaticEstimate, ManualEstimate or FixedCount; Automatic, Manual and Fixed also work, case-insensitive) and optionally its PreAllocationCount, in one undo step, then save the system. A negative pre_allocation_count is stored as 0. An unknown mode, a pre_allocation_count that is not a JSON number, or an unknown system or emitter is an error and nothing changes. Values already in place are a success with changed=false and saved=false, nothing written. A failed save makes success false, with saved=false and save_error, the change kept in memory. Read back with get_niagara_emitter_runtime_summary; the GPU spawn limit: set_niagara_emitter_max_gpu_particles.
//   @retired set_niagara_emitter_properties - allocation_mode, pre_allocation_count
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param allocation_mode - One of "AutomaticEstimate", "ManualEstimate", or "FixedCount" (case-insensitive; "Automatic" and "Manual" and "Fixed" also accepted)
//   @param pre_allocation_count - Optional int, the particle count to allocate up front; a negative one is stored as 0. Only meaningful when not AutomaticEstimate
//   @returns { system_path: string, emitter_name: string, allocation_mode: string, pre_allocation_count: int, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

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

    // pre_allocation_count is only meaningful when not AutomaticEstimate; a negative one is stored as 0. A value that
    // is not a JSON number is refused, not stored: a string reads into an int32 through LexFromString, text as 0.
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("pre_allocation_count"))) { return Mistyped; }
    int32 PreCount = 0;
    const bool bHasPreCount = Params->TryGetNumberField(TEXT("pre_allocation_count"), PreCount);
    PreCount = FMath::Max(0, PreCount);

    // The emitter data lives in the handle's UNiagaraEmitter, which the transaction must record for an undo.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("allocation_mode"), ModeStr);

    // Values already in place are a valid no-op: nothing is modified, dirtied or saved.
    if (EmitterData->AllocationMode == NewMode && (!bHasPreCount || EmitterData->PreAllocationCount == PreCount))
    {
        Data->SetNumberField(TEXT("pre_allocation_count"), EmitterData->PreAllocationCount);
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter already has this allocation; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterAllocationMode", "Set Niagara Emitter Allocation Mode"));
    System->Modify();
    Emitter->Modify();
    EmitterData->AllocationMode = NewMode;
    if (bHasPreCount)
    {
        EmitterData->PreAllocationCount = PreCount;
    }

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetNumberField(TEXT("pre_allocation_count"), EmitterData->PreAllocationCount);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_allocation_mode"), SaveError);
}

// MCP:set_niagara_emitter_max_gpu_particles - Set the MaxGPUParticlesSpawnPerFrame override of one emitter of a Niagara System asset, found by handle name, in one undo step, then save the system; it limits the particles a GPU emitter spawns per frame and only takes effect when the sim target is GPUComputeSim (effective_for_current_sim says whether it is now). max_gpu_particles is required and must be a JSON number of 0 or more; a negative count, or an unknown system or emitter, is an error and nothing changes. A count already in place is a success with changed=false and saved=false, nothing written. A failed save makes success false, with saved=false and save_error, the change kept in memory. Read back with get_niagara_emitter_runtime_summary; switch the sim target with set_niagara_emitter_sim_target.
//   @retired set_niagara_emitter_properties - max_gpu_particles
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param max_gpu_particles - Particles the emitter may spawn per frame on the GPU, an integer >= 0; the call fails without it
//   @returns { system_path: string, emitter_name: string, sim_target: string, max_gpu_particles: int, effective_for_current_sim: bool, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterMaxGPUParticles(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    // Read with ReadRequiredNumber so the catalog publishes max_gpu_particles as required: the call fails without it.
    // A string would read as 0 through LexFromString, so the JSON type is checked first.
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("max_gpu_particles"))) { return Mistyped; }
    int32 NewCount = 0;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredNumber(Params, TEXT("max_gpu_particles"), NewCount)) { return Missing; }
    if (NewCount < 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("max_gpu_particles must be >= 0 (got %d)"), NewCount));
    }

    // The emitter data lives in the handle's UNiagaraEmitter, which the transaction must record for an undo.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The EditCondition on MaxGPUParticlesSpawnPerFrame is
    // "SimTarget == ENiagaraSimTarget::GPUComputeSim"; we warn (but still
    // apply) when the emitter is CPU — the caller may be pre-configuring
    // before flipping SimTarget.
    const bool bIsGPUSim = (EmitterData->SimTarget == ENiagaraSimTarget::GPUComputeSim);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("sim_target"), bIsGPUSim ? TEXT("GPUComputeSim") : TEXT("CPUSim"));
    Data->SetBoolField(TEXT("effective_for_current_sim"), bIsGPUSim);

    // A count already in place is a valid no-op: nothing is modified, dirtied or saved.
    if (EmitterData->MaxGPUParticlesSpawnPerFrame == NewCount)
    {
        Data->SetNumberField(TEXT("max_gpu_particles"), EmitterData->MaxGPUParticlesSpawnPerFrame);
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter already has this count; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterMaxGPUParticles", "Set Niagara Emitter Max GPU Particles"));
    System->Modify();
    Emitter->Modify();
    EmitterData->MaxGPUParticlesSpawnPerFrame = NewCount;

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetNumberField(TEXT("max_gpu_particles"), EmitterData->MaxGPUParticlesSpawnPerFrame);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_max_gpu_particles"), SaveError);
}

// MCP:set_niagara_emitter_requires_persistent_ids - Set whether one emitter of a Niagara System asset, found by handle name, gives its particles persistent IDs (bRequiresPersistentIDs: a Particles.ID stable across frames, at a small cost), in one undo step, then save the system. requires_persistent_ids is required. A value already in place is a success with changed=false and saved=false, nothing written. A missing param or an unknown system or emitter is an error. A failed save makes success false, with saved=false and save_error, the change kept in memory. Returns the flag as read back; read it later with get_niagara_emitter_runtime_summary; local space: set_niagara_emitter_sim_target with local_space.
//   @retired set_niagara_emitter_properties - requires_persistent_ids
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param requires_persistent_ids - true to give every particle a persistent ID, false to drop them; the call fails without it
//   @returns { system_path: string, emitter_name: string, requires_persistent_ids: bool, applied: bool, changed: bool, saved: bool, save_error?: string, note?: string }

TSharedPtr<FJsonObject> HandleSetNiagaraEmitterRequiresPersistentIds(const TSharedPtr<FJsonObject>& Params)
{
    FString SystemPath;
    FString EmitterName;
    FString Error;
    if (!Niagara_ReadSystemAndEmitterNames(Params, SystemPath, EmitterName, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    // Read with ReadRequiredBool so the catalog publishes requires_persistent_ids as required: the call fails without it.
    bool bRequires = false;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredBool(Params, TEXT("requires_persistent_ids"), bRequires)) { return Missing; }

    // The emitter data lives in the handle's UNiagaraEmitter, which the transaction must record for an undo.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraEmitter* Emitter = Niagara_ResolveEditableEmitter(SystemPath, EmitterName, System, EmitterData, Error);
    if (!Emitter)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);

    // A value already in place is a valid no-op: nothing is modified, dirtied or saved.
    if ((EmitterData->bRequiresPersistentIDs != 0) == bRequires)
    {
        Data->SetBoolField(TEXT("requires_persistent_ids"), bRequires);
        Data->SetBoolField(TEXT("applied"), false);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The emitter already has this setting; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraEmitterRequiresPersistentIds", "Set Niagara Emitter Requires Persistent Ids"));
    System->Modify();
    Emitter->Modify();
    EmitterData->bRequiresPersistentIDs = bRequires ? 1 : 0;

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    Data->SetBoolField(TEXT("requires_persistent_ids"), EmitterData->bRequiresPersistentIDs != 0);
    Data->SetBoolField(TEXT("applied"), true);
    Data->SetBoolField(TEXT("changed"), true);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_emitter_requires_persistent_ids"), SaveError);
}

// ---------------------------------------------------------------------------
// Per-stack module walking. Every script of an emitter shares ONE graph, so a stack is not "the function-call nodes
// of the graph": it is the parameter-map chain that ends at that stack's output node. These helpers re-implement,
// with exported APIs only, what FNiagaraStackGraphUtilities::GetOrderedModuleNodes / GetStackNodeGroups /
// DisconnectStackNodeGroup / ConnectStackNodeGroup do (NOT exported -> LNK2019).
// ---------------------------------------------------------------------------
namespace
{
    // The parameter-map pin of Node in Direction (a stack node: input, output, module call or override node), else
    // null. A Niagara parameter-map pin carries FNiagaraTypeDefinition::GetParameterMapStruct() as its sub-category
    // object -- what UEdGraphSchema_Niagara::PinToTypeDefinition compares with GetParameterMapDef().
    UEdGraphPin* Niagara_ParameterMapPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
    {
        if (!Node)
        {
            return nullptr;
        }
        const UObject* ParameterMapStruct = FNiagaraTypeDefinition::GetParameterMapStruct();
        for (UEdGraphPin* Pin : Node->Pins)
        {
            if (Pin && Pin->Direction == Direction && Pin->PinType.PinSubCategoryObject.Get() == ParameterMapStruct)
            {
                return Pin;
            }
        }
        return nullptr;
    }

    UEdGraphPin* Niagara_ParameterMapInputPin(UEdGraphNode* Node)
    {
        return Niagara_ParameterMapPin(Node, EGPD_Input);
    }

    // The modules of the stack that ends at Output, in execution order (first module first): the
    // UNiagaraNodeFunctionCall nodes (assignment nodes included) met walking the parameter-map chain back from the
    // output node to the stack's input node, as GetOrderedModuleNodes walks it. A dynamic input feeds a module input
    // pin, not the chain, so it is not a module; a disabled module stays on the chain and is. OutInputNode, when
    // given, receives the node the chain starts at (the stack's input node).
    TArray<UNiagaraNodeFunctionCall*> Niagara_OrderedStackModules(UNiagaraNodeOutput* Output, UEdGraphNode** OutInputNode = nullptr)
    {
        TArray<UNiagaraNodeFunctionCall*> Modules;
        TSet<const UEdGraphNode*> Visited;
        UEdGraphNode* Current = Output;
        while (Current && !Visited.Contains(Current))
        {
            Visited.Add(Current);
            UEdGraphPin* MapInput = Niagara_ParameterMapInputPin(Current);
            if (!MapInput || MapInput->LinkedTo.Num() != 1 || !MapInput->LinkedTo[0])
            {
                break;
            }
            UEdGraphNode* Previous = MapInput->LinkedTo[0]->GetOwningNode();
            if (UNiagaraNodeFunctionCall* Module = Cast<UNiagaraNodeFunctionCall>(Previous))
            {
                Modules.Insert(Module, 0);
            }
            Current = Previous;
        }
        if (OutInputNode)
        {
            *OutInputNode = Current;
        }
        return Modules;
    }

    int32 Niagara_CountStackModules(UNiagaraNodeOutput* Output)
    {
        return Niagara_OrderedStackModules(Output).Num();
    }

    // One group of a stack's parameter-map chain, as FNiagaraStackGraphUtilities::FStackNodeGroup: the nodes whose
    // parameter-map input hangs off the previous group's end (a module's override node and the map-get nodes of its
    // linked inputs, or the module itself) and the node the group ends at (the module; or the input / output node).
    struct FUEAgentNiagaraStackGroup
    {
        TArray<UEdGraphNode*> StartNodes;
        UEdGraphNode* EndNode = nullptr;
    };

    // The groups of the stack that ends at Output: the input group, one group per module in execution order, and
    // the output group. False (OutGroups emptied) when the chain is malformed: a group end without a parameter-map
    // output pin, or one that links to nothing.
    bool Niagara_StackGroups(UNiagaraNodeOutput* Output, TArray<FUEAgentNiagaraStackGroup>& OutGroups)
    {
        OutGroups.Reset();
        UEdGraphNode* InputNode = nullptr;
        const TArray<UNiagaraNodeFunctionCall*> Modules = Niagara_OrderedStackModules(Output, &InputNode);
        if (!InputNode || InputNode == Output)
        {
            return false;
        }
        FUEAgentNiagaraStackGroup InputGroup;
        InputGroup.StartNodes.Add(InputNode);
        InputGroup.EndNode = InputNode;
        OutGroups.Add(InputGroup);

        // The nodes whose parameter-map input is linked from End's parameter-map output.
        auto StartsAfter = [](UEdGraphNode* End, TArray<UEdGraphNode*>& OutStarts) -> bool
        {
            UEdGraphPin* EndOutput = Niagara_ParameterMapPin(End, EGPD_Output);
            if (!EndOutput)
            {
                return false;
            }
            for (UEdGraphPin* Linked : EndOutput->LinkedTo)
            {
                if (UEdGraphNode* Owner = Linked ? Linked->GetOwningNode() : nullptr)
                {
                    OutStarts.AddUnique(Owner);
                }
            }
            return OutStarts.Num() > 0;
        };
        for (UNiagaraNodeFunctionCall* Module : Modules)
        {
            FUEAgentNiagaraStackGroup ModuleGroup;
            if (!StartsAfter(OutGroups.Last().EndNode, ModuleGroup.StartNodes))
            {
                OutGroups.Reset();
                return false;
            }
            ModuleGroup.EndNode = Module;
            OutGroups.Add(ModuleGroup);
        }
        FUEAgentNiagaraStackGroup OutputGroup;
        if (!StartsAfter(OutGroups.Last().EndNode, OutputGroup.StartNodes))
        {
            OutGroups.Reset();
            return false;
        }
        OutputGroup.EndNode = Output;
        OutGroups.Add(OutputGroup);
        return true;
    }

    // Links From (a parameter-map output) to To (a parameter-map input) and notifies both nodes, as
    // FNiagaraStackGraphUtilities::MakeLinkTo does; UNiagaraNode::PinConnectionListChanged marks the node for
    // re-synchronization so the next compile sees the new order.
    void Niagara_LinkMapPins(UEdGraphPin* From, UEdGraphPin* To)
    {
        if (!From || !To)
        {
            return;
        }
        From->MakeLinkTo(To);
        From->GetOwningNode()->PinConnectionListChanged(From);
        To->GetOwningNode()->PinConnectionListChanged(To);
    }

    // Takes Group out of the chain and closes the gap: Previous's end now feeds Next's start nodes.
    void Niagara_DisconnectStackGroup(const FUEAgentNiagaraStackGroup& Group, const FUEAgentNiagaraStackGroup& Previous,
                                      const FUEAgentNiagaraStackGroup& Next)
    {
        UEdGraphPin* PreviousOutput = Niagara_ParameterMapPin(Previous.EndNode, EGPD_Output);
        UEdGraphPin* GroupOutput = Niagara_ParameterMapPin(Group.EndNode, EGPD_Output);
        PreviousOutput->BreakAllPinLinks(true);
        GroupOutput->BreakAllPinLinks(true);
        for (UEdGraphNode* NextStart : Next.StartNodes)
        {
            Niagara_LinkMapPins(PreviousOutput, Niagara_ParameterMapInputPin(NextStart));
        }
    }

    // Puts Group into the chain between NewPrevious and NewNext.
    void Niagara_ConnectStackGroup(const FUEAgentNiagaraStackGroup& Group, const FUEAgentNiagaraStackGroup& NewPrevious,
                                   const FUEAgentNiagaraStackGroup& NewNext)
    {
        UEdGraphPin* PreviousOutput = Niagara_ParameterMapPin(NewPrevious.EndNode, EGPD_Output);
        PreviousOutput->BreakAllPinLinks(true);
        for (UEdGraphNode* GroupStart : Group.StartNodes)
        {
            Niagara_LinkMapPins(PreviousOutput, Niagara_ParameterMapInputPin(GroupStart));
        }
        UEdGraphPin* GroupOutput = Niagara_ParameterMapPin(Group.EndNode, EGPD_Output);
        for (UEdGraphNode* NextStart : NewNext.StartNodes)
        {
            Niagara_LinkMapPins(GroupOutput, Niagara_ParameterMapInputPin(NextStart));
        }
    }
}

// MCP:get_niagara_emitter_runtime_summary - Read one emitter of a Niagara System asset in a single flat reply: simulation target (CPUSim or GPUComputeSim), bounds mode (Dynamic, Fixed or Programmable) and its fixed box as 'X=.. Y=.. Z=..' strings, particle allocation mode and pre-allocation count, GPU spawn cap per frame (max_gpu_particles), local-space, determinism and persistent-ID flags, how many renderers, event handlers and simulation stages it has, and how many modules each stack holds. Read-only: it changes nothing and compiles nothing. Module counts are per stack, read from the chain that ends at each stack's output node: spawn_module_count and update_module_count for particle spawn and update, emitter_spawn_module_count and emitter_update_module_count, event_module_count over all event handlers and simulation_stage_module_count over all stages; dynamic inputs are not counted, disabled modules are, and graph_function_call_count counts every function-call node of the emitter graph. list_niagara_modules lists modules by name. A missing system or emitter handle is an error. Change these settings with set_niagara_emitter_properties.
//   @param system_path - Content path of the UNiagaraSystem asset, package or object form, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle, as get_niagara_emitter_handles lists it, e.g. 'Fountain'.
//   @returns { system_path: string, emitter_name: string, sim_target: string, bounds_mode: string, bounds_min: string, bounds_max: string, allocation_mode: string, pre_allocation_count: int, max_gpu_particles: int, local_space: bool, deterministic: bool, requires_persistent_ids: bool, update_module_count: int, spawn_module_count: int, emitter_spawn_module_count: int, emitter_update_module_count: int, event_module_count: int, simulation_stage_module_count: int, graph_function_call_count: int, renderer_count: int, event_handler_count: int, simulation_stage_count: int, note?: string }

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

    // Modules per script usage. Every script of an emitter shares ONE graph (its GraphSource), with one output node
    // per stack: emitter spawn/update, particle spawn/update, one per event handler and one per simulation stage.
    // Each stack's modules are the function-call nodes on the parameter-map chain that ends at its output node
    // (Niagara_CountStackModules), so a dynamic input, which hangs off a module input, is not a module.
    int32 SpawnModules = 0;
    int32 UpdateModules = 0;
    int32 EmitterSpawnModules = 0;
    int32 EmitterUpdateModules = 0;
    int32 EventModules = 0;
    int32 SimulationStageModules = 0;
    int32 GraphFunctionCalls = 0;
    UNiagaraScriptSource* GraphSource = Cast<UNiagaraScriptSource>(EmitterData->GraphSource);
    UNiagaraGraph* EmitterGraph = GraphSource ? GraphSource->NodeGraph.Get() : nullptr;
    if (EmitterGraph)
    {
        TArray<UNiagaraNodeFunctionCall*> AllCalls;
        EmitterGraph->GetNodesOfClass<UNiagaraNodeFunctionCall>(AllCalls);
        GraphFunctionCalls = AllCalls.Num();

        TArray<UNiagaraNodeOutput*> Outputs;
        EmitterGraph->GetNodesOfClass<UNiagaraNodeOutput>(Outputs);
        for (UNiagaraNodeOutput* Output : Outputs)
        {
            if (!Output)
            {
                continue;
            }
            const int32 StackModules = Niagara_CountStackModules(Output);
            switch (Output->GetUsage())
            {
            case ENiagaraScriptUsage::ParticleSpawnScript:
            case ENiagaraScriptUsage::ParticleSpawnScriptInterpolated:
                SpawnModules += StackModules;
                break;
            case ENiagaraScriptUsage::ParticleUpdateScript:
                UpdateModules += StackModules;
                break;
            case ENiagaraScriptUsage::EmitterSpawnScript:
                EmitterSpawnModules += StackModules;
                break;
            case ENiagaraScriptUsage::EmitterUpdateScript:
                EmitterUpdateModules += StackModules;
                break;
            case ENiagaraScriptUsage::ParticleEventScript:
                EventModules += StackModules;
                break;
            case ENiagaraScriptUsage::ParticleSimulationStageScript:
                SimulationStageModules += StackModules;
                break;
            default:
                break;
            }
        }
    }
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
    Data->SetNumberField(TEXT("event_module_count"), EventModules);
    Data->SetNumberField(TEXT("simulation_stage_module_count"), SimulationStageModules);
    Data->SetNumberField(TEXT("graph_function_call_count"), GraphFunctionCalls);
    Data->SetNumberField(TEXT("renderer_count"), RendererCount);
    Data->SetNumberField(TEXT("event_handler_count"), EmitterData->EventHandlerScriptProps.Num());
    Data->SetNumberField(TEXT("simulation_stage_count"), EmitterData->GetSimulationStages().Num());
    if (!EmitterGraph)
    {
        Data->SetStringField(TEXT("note"), TEXT("The emitter has no editable script graph (a stateless emitter, or no GraphSource): the module counts are 0."));
    }
    return mcp_common::MakeOkReply(Data);
}

// ===========================================================================
// Niagara module stack graph CRUD
//
// The commands of this section operate on the UNiagaraGraph that backs the ParticleSpawn /
// ParticleUpdate stack of an emitter (add_niagara_module_to_stack inserts, with an
// optional target_index). remove / duplicate / enable / reorder round-trip the same
// node graph directly via FNiagaraStackGraphUtilities.
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

    // Niagara_ResolveStackOutput for the ScriptGroup stack, then the first module named ModuleName (GetFunctionName,
    // case-sensitive) on THAT stack -- walked along its parameter-map chain, never searched over the emitter's whole
    // shared graph, which also holds the other stacks and the dynamic inputs.
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
        for (UNiagaraNodeFunctionCall* Module : Niagara_OrderedStackModules(OutOutput))
        {
            if (Module && Module->GetFunctionName().Equals(ModuleName, ESearchCase::CaseSensitive))
            {
                return Module;
            }
        }
        OutError = FString::Printf(
            TEXT("Module '%s' not found on the %s stack of emitter '%s' (list_niagara_modules lists that stack); nothing was changed"),
            *ModuleName, *ScriptGroup, *EmitterName);
        return nullptr;
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
// remove_niagara_module_from_stack
// Removes one module, with its whole stack group, from the emitter's spawn OR
// update stack, found by name (GetFunctionName) on that stack's chain only.
// ---------------------------------------------------------------------------
// MCP:remove_niagara_module_from_stack - Remove one module from a Niagara emitter's particle spawn or update stack, in one undo step: the module and every node that feeds it (its input overrides, dynamic inputs and data-interface inputs) are taken out and the stack's chain is closed over the gap, then a system compile is requested and the system saved. The module is found by function name (case-sensitive) on the script_group stack only, the first one in execution order, as list_niagara_modules lists it. The result is read back: removed is true when the module is off the stack and the stack is one module shorter, chain_intact when the stack still runs from its input to its output node; either false fails the call. An unknown system, emitter, script_group or module (or a module only on the other stack) is an error and nothing changes; a failed save makes success false, with saved=false and save_error. The inverse is add_niagara_module_to_stack.
//   @param system_path - Content path of the UNiagaraSystem asset, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle, as get_niagara_emitter_handles lists it.
//   @param module_name - Function name of the module to remove, case-sensitive, as list_niagara_modules gives module_name for that stack, e.g. 'GravityForce'.
//   @param script_group - Optional "spawn" or "update" (any case), the stack the module is on; defaults to "update"; any other value is an error and nothing changes.
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, removed_index: int, removed_node_count: int, module_count: int, chain_intact: bool, removed: bool, saved: bool, save_error?: string }

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
    // Only "spawn" or "update" names a stack; any other value is refused before anything is loaded or changed.
    FString ScriptGroup;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!Niagara_ReadStrictScriptGroup(Params, ScriptGroup, Usage, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = nullptr;
    UNiagaraNodeFunctionCall* TargetModule =
        Niagara_ResolveStackModule(SystemPath, EmitterName, ScriptGroup, ModuleName, System, EmitterData, Out, Error);
    if (!TargetModule)
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // FNiagaraStackGraphUtilities::RemoveModuleFromStack is NOT exported, so its path is replicated: take the module's
    // whole group out of the stack's parameter-map chain and close the gap, then remove every node that fed the
    // module (its override node, dynamic inputs, data-interface and linked-input nodes). Removing only the module
    // node, as this command once did, left its override node in the chain and the stack cut in two.
    UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph());
    TArray<FUEAgentNiagaraStackGroup> Groups;
    if (!NiagaraGraph || !Niagara_StackGroups(Out, Groups))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The %s stack of emitter '%s' has no well-formed parameter-map chain from its input to its output node; nothing was changed"),
            *ScriptGroup, *EmitterName));
    }
    const int32 GroupIndex = Groups.IndexOfByPredicate(
        [TargetModule](const FUEAgentNiagaraStackGroup& Group) { return Group.EndNode == TargetModule; });
    if (GroupIndex <= 0 || GroupIndex >= Groups.Num() - 1)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Module '%s' is not a group of the %s stack's chain; nothing was changed"), *ModuleName, *ScriptGroup));
    }
    const int32 RemovedIndex = GroupIndex - 1;
    const int32 ModulesBefore = Groups.Num() - 2;

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "RemoveNiagaraModuleFromStack", "Remove Niagara Module From Stack"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);
    NiagaraGraph->Modify();
    // The owning script keeps the module's rapid-iteration values; recording it lets an undo restore them.
    if (UNiagaraScript* OwningScript = GetEmitterGroupScript(EmitterData, Usage))
    {
        OwningScript->Modify();
    }
    Niagara_DisconnectStackGroup(Groups[GroupIndex], Groups[GroupIndex - 1], Groups[GroupIndex + 1]);

    // The group is now cut off the chain, so everything upstream of the module belongs to it.
    TArray<UEdGraphNode*> NodesToRemove;
    NodesToRemove.Add(TargetModule);
    for (int32 Index = 0; Index < NodesToRemove.Num(); ++Index)
    {
        for (UEdGraphPin* Pin : NodesToRemove[Index]->Pins)
        {
            if (!Pin || Pin->Direction != EGPD_Input)
            {
                continue;
            }
            for (UEdGraphPin* Linked : Pin->LinkedTo)
            {
                if (UEdGraphNode* Upstream = Linked ? Linked->GetOwningNode() : nullptr)
                {
                    NodesToRemove.AddUnique(Upstream);
                }
            }
        }
    }
    for (UEdGraphNode* Node : NodesToRemove)
    {
        Node->Modify();
        NiagaraGraph->RemoveNode(Node);
    }
    NiagaraGraph->NotifyGraphChanged();
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Read back: the module is off the stack, and the stack is still one chain from input to output.
    const TArray<UNiagaraNodeFunctionCall*> ModulesAfter = Niagara_OrderedStackModules(Out);
    TArray<FUEAgentNiagaraStackGroup> GroupsAfter;
    const bool bChainIntact = Niagara_StackGroups(Out, GroupsAfter);
    const bool bRemoved = !ModulesAfter.Contains(TargetModule) && ModulesAfter.Num() == ModulesBefore - 1;

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), ModuleName);
    Data->SetNumberField(TEXT("removed_index"), RemovedIndex);
    Data->SetNumberField(TEXT("removed_node_count"), NodesToRemove.Num());
    Data->SetNumberField(TEXT("module_count"), ModulesAfter.Num());
    Data->SetBoolField(TEXT("chain_intact"), bChainIntact);
    Data->SetBoolField(TEXT("removed"), bRemoved);
    if (!bRemoved || !bChainIntact)
    {
        mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("remove_niagara_module_from_stack: read back after the removal, '%s' %s on the %s stack and its chain is %s"),
            *ModuleName, bRemoved ? TEXT("is gone") : TEXT("is still"), *ScriptGroup, bChainIntact ? TEXT("intact") : TEXT("broken")));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("remove_niagara_module_from_stack"), SaveError);
}

// ---------------------------------------------------------------------------
// duplicate_niagara_module_in_stack
// Loads the source module UNiagaraScript by GetFunctionName's script reference,
// then re-inserts it via AddScriptModuleToStack at a STACK position (one past
// the source on its own stack, or target_index_override).
// ---------------------------------------------------------------------------
// MCP:duplicate_niagara_module_in_stack - Add another instance of a module to one emitter of a Niagara System asset: the module named module_name (its function name, case-sensitive, as list_niagara_modules gives it) is found on the script_group stack only (spawn or update, any case, default update) and its script is added again to that stack through FNiagaraStackGraphUtilities::AddScriptModuleToStack, in one undo step, then a system compile is requested and the system saved. The new module starts with the default inputs of the script; the input overrides of the source are not copied. Positions are stack positions, as list_niagara_modules reports index: target_index_override sets it (clamped into the stack, -1 appends), else it goes right after the source. inserted_index is read back from the stack, and a module that did not land where asked fails the call. An unknown system, emitter, script_group or module (or a module only on the other stack) is an error and nothing changes; a failed save makes success false, with saved=false and save_error. Inputs: set_niagara_module_input; move: reorder_niagara_modules.
//   @param system_path - Content path of the UNiagaraSystem asset, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle, as get_niagara_emitter_handles lists it.
//   @param module_name - Function name of the module to duplicate, case-sensitive, as list_niagara_modules gives module_name for that stack, e.g. 'GravityForce'.
//   @param script_group - Optional "spawn" or "update" (any case), the stack the module is on; defaults to "update"; any other value is an error and nothing changes.
//   @param target_index_override - Optional stack position for the new module, a JSON number (0 runs first; -1 appends; others are clamped into the stack); defaults to the source's stack index plus one.
//   @returns { system_path: string, emitter_name: string, script_group: string, source_module_name: string, source_index: int, new_module_name: string, inserted_index: int, module_count: int, duplicated: bool, saved: bool, save_error?: string }

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
    // Only "spawn" or "update" names a stack; any other value is refused before anything is loaded or changed.
    FString ScriptGroup;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!Niagara_ReadStrictScriptGroup(Params, ScriptGroup, Usage, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("target_index_override"))) { return Mistyped; }
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

    // Positions are stack positions, the index list_niagara_modules reports and AddScriptModuleToStack takes (it
    // clamps to [0, module count]): the explicit override, else one past the source on its own stack.
    const TArray<UNiagaraNodeFunctionCall*> ModulesBefore = Niagara_OrderedStackModules(Out);
    const int32 SourceIndex = ModulesBefore.IndexOfByKey(Source);
    const int32 InsertIndex = Params->HasField(TEXT("target_index_override")) ? TargetIndexOverride : SourceIndex + 1;
    // Where AddScriptModuleToStack puts it: INDEX_NONE appends, any other index is clamped into the stack.
    const int32 ExpectedIndex = InsertIndex == INDEX_NONE ? ModulesBefore.Num() : FMath::Clamp(InsertIndex, 0, ModulesBefore.Num());

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

    // Read back from the stack's chain: where the new module really is.
    const TArray<UNiagaraNodeFunctionCall*> ModulesAfter = Niagara_OrderedStackModules(Out);
    const int32 InsertedIndex = ModulesAfter.IndexOfByKey(Duplicate);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("source_module_name"), ModuleName);
    Data->SetNumberField(TEXT("source_index"), SourceIndex);
    Data->SetStringField(TEXT("new_module_name"), Duplicate->GetFunctionName());
    Data->SetNumberField(TEXT("inserted_index"), InsertedIndex);
    Data->SetNumberField(TEXT("module_count"), ModulesAfter.Num());
    Data->SetBoolField(TEXT("duplicated"), InsertedIndex != INDEX_NONE);
    if (InsertedIndex != ExpectedIndex)
    {
        mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("duplicate_niagara_module_in_stack: the new module reads at index %d of the %s stack, not %d"),
            InsertedIndex, *ScriptGroup, ExpectedIndex));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("duplicate_niagara_module_in_stack"), SaveError);
}

// ---------------------------------------------------------------------------
// set_niagara_module_enabled
// Toggle bIsEnabled on a UNiagaraNodeFunctionCall via
// FNiagaraStackGraphUtilities::SetModuleIsEnabled. Returns the pre/post state.
// ---------------------------------------------------------------------------
// MCP:set_niagara_module_enabled - Enable or disable one module of a Niagara emitter's particle spawn or update stack (FNiagaraStackGraphUtilities::SetModuleIsEnabled), in one undo step, then request a system compile and save the system; a disabled module stays on the stack but does not run. The module is found by function name (case-sensitive) on the script_group stack only, the first one in execution order. previous_enabled and enabled are read from the node before and after; an enabled state that did not take fails the call. A module already in that state changes nothing: changed false, saved false and a note. An unknown system, emitter, script_group or module (or a module only on the other stack) is an error and nothing changes; a failed save makes success false, with saved=false and save_error. list_niagara_modules shows each module's is_enabled.
//   @param system_path - Content path of the UNiagaraSystem asset, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle, as get_niagara_emitter_handles lists it.
//   @param module_name - Function name of the module to toggle, case-sensitive, as list_niagara_modules gives module_name for that stack, e.g. 'GravityForce'.
//   @param enabled - Optional bool, the state to set; defaults to true
//   @param script_group - Optional "spawn" or "update" (any case), the stack the module is on; defaults to "update"; any other value is an error and nothing changes.
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, module_index: int, previous_enabled: bool, enabled: bool, changed: bool, saved: bool, save_error?: string, note?: string }

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
    // Only "spawn" or "update" names a stack; any other value is refused before anything is loaded or changed.
    FString ScriptGroup;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!Niagara_ReadStrictScriptGroup(Params, ScriptGroup, Usage, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

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
    const bool bPreviousEnabled = Target->IsNodeEnabled();

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), ModuleName);
    Data->SetNumberField(TEXT("module_index"), Niagara_OrderedStackModules(Out).IndexOfByKey(Target));
    Data->SetBoolField(TEXT("previous_enabled"), bPreviousEnabled);

    // Already in that state: nothing is touched, nothing saved.
    if (bPreviousEnabled == bEnabled)
    {
        Data->SetBoolField(TEXT("enabled"), bPreviousEnabled);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The module already had that enabled state; nothing was changed or saved."));
        return mcp_common::MakeOkReply(Data);
    }

    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraModuleEnabled", "Set Niagara Module Enabled"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);
    FNiagaraStackGraphUtilities::SetModuleIsEnabled(*Target, bEnabled);
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Read back from the node, not echoed from the request.
    const bool bNowEnabled = Target->IsNodeEnabled();
    Data->SetBoolField(TEXT("enabled"), bNowEnabled);
    Data->SetBoolField(TEXT("changed"), bNowEnabled != bPreviousEnabled);
    if (bNowEnabled != bEnabled)
    {
        mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("set_niagara_module_enabled: '%s' reads %s after the write"), *ModuleName, bNowEnabled ? TEXT("enabled") : TEXT("disabled")));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_module_enabled"), SaveError);
}

// ---------------------------------------------------------------------------
// reorder_niagara_modules
// Move a module to a new position within its own stack by rewiring the stack's parameter-map chain, the way the
// stack editor's MoveModule does within one stack (that function is not exported).
// ---------------------------------------------------------------------------
// MCP:reorder_niagara_modules - Move one module of a Niagara emitter's particle spawn or update stack to another position in that same stack, changing its execution order: the module, with its input overrides and linked inputs, is taken out of the stack's parameter-map chain and put back at target_index, in one undo step, then a system compile is requested and the system saved. The module is found by function name (case-sensitive) on the script_group stack only; positions count that stack's modules in execution order, exactly as list_niagara_modules reports index. target_index past the last is clamped to it. new_index is read back from the rewired chain. A module already at that position changes nothing: reordered false, saved false and a note. An unknown system, emitter, script_group or module (or a module only on the other stack) is an error and nothing changes; a failed save makes success false, with saved=false and save_error. Insert a new module at a position: add_niagara_module_to_stack with target_index.
//   @param system_path - Content path of the UNiagaraSystem asset, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle, as get_niagara_emitter_handles lists it.
//   @param module_name - Function name of the module to move, case-sensitive, as list_niagara_modules gives module_name for that stack, e.g. 'GravityForce'.
//   @param target_index - New position in the stack, a JSON number of 0 or more (0 runs first), counted as list_niagara_modules counts index; clamped to the last; the call fails without it.
//   @param script_group - Optional "spawn" or "update" (any case), the stack the module is on; defaults to "update"; any other value is an error.
//   @returns { system_path: string, emitter_name: string, script_group: string, module_name: string, old_index: int, new_index: int, target_index: int, module_count: int, reordered: bool, saved: bool, save_error?: string, note?: string }

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
    if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, TEXT("target_index"))) { return Mistyped; }
    int32 TargetIndex = INDEX_NONE;
    if (!Params->TryGetNumberField(TEXT("target_index"), TargetIndex) || TargetIndex < 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing or non-negative 'target_index' required"));
    }
    FString ScriptGroup;
    ENiagaraScriptUsage Usage = ENiagaraScriptUsage::ParticleUpdateScript;
    if (!Niagara_ReadStrictScriptGroup(Params, ScriptGroup, Usage, Error))
    {
        return mcp_common::MakeErrorReply(Error);
    }

    // The stack's output node, then the stack itself: its parameter-map chain cut into groups (input, one per
    // module in execution order, output). The module is looked for on THIS stack only.
    UNiagaraSystem* System = nullptr;
    FVersionedNiagaraEmitterData* EmitterData = nullptr;
    UNiagaraNodeOutput* Out = Niagara_ResolveStackOutput(SystemPath, EmitterName, Usage, System, EmitterData, Error);
    if (!Out)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    UNiagaraGraph* NiagaraGraph = Cast<UNiagaraGraph>(Out->GetGraph());
    TArray<FUEAgentNiagaraStackGroup> Groups;
    if (!NiagaraGraph || !Niagara_StackGroups(Out, Groups))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("The %s stack of emitter '%s' has no well-formed parameter-map chain from its input to its output node; nothing was changed"),
            *ScriptGroup, *EmitterName));
    }
    const int32 ModuleCount = Groups.Num() - 2;
    int32 OldIndex = INDEX_NONE;
    for (int32 ModuleIndex = 0; ModuleIndex < ModuleCount; ++ModuleIndex)
    {
        const UNiagaraNodeFunctionCall* Module = Cast<UNiagaraNodeFunctionCall>(Groups[ModuleIndex + 1].EndNode);
        if (Module && Module->GetFunctionName().Equals(ModuleName, ESearchCase::CaseSensitive))
        {
            OldIndex = ModuleIndex;
            break;
        }
    }
    if (OldIndex == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Module '%s' not found on the %s stack of emitter '%s' (list_niagara_modules lists that stack); nothing was changed"),
            *ModuleName, *ScriptGroup, *EmitterName));
    }
    UNiagaraNodeFunctionCall* Target = CastChecked<UNiagaraNodeFunctionCall>(Groups[OldIndex + 1].EndNode);
    const int32 ClampedTargetIndex = FMath::Clamp(TargetIndex, 0, ModuleCount - 1);

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    Data->SetStringField(TEXT("script_group"), ScriptGroup);
    Data->SetStringField(TEXT("module_name"), ModuleName);
    Data->SetNumberField(TEXT("old_index"), OldIndex);
    Data->SetNumberField(TEXT("target_index"), TargetIndex);
    Data->SetNumberField(TEXT("module_count"), ModuleCount);

    // Already at that position: nothing is touched, nothing saved.
    if (ClampedTargetIndex == OldIndex)
    {
        Data->SetNumberField(TEXT("new_index"), OldIndex);
        Data->SetBoolField(TEXT("reordered"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The module is already at that position of its stack; nothing was changed or saved."));
        return mcp_common::MakeOkReply(Data);
    }

    // Move the module's whole group (its override node, the map-get nodes of its linked inputs and the module) along
    // the chain, the way the stack editor's MoveModule does inside one stack: take it out and close the gap, then
    // put it between the groups now at the target position. Execution order IS this chain; the order of the graph's
    // node array means nothing to Niagara.
    FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "ReorderNiagaraModules", "Reorder Niagara Modules"));
    System->Modify();
    Niagara_ModifyEmitterGraph(EmitterData);
    NiagaraGraph->Modify();
    const int32 SourceGroupIndex = OldIndex + 1;
    const FUEAgentNiagaraStackGroup SourceGroup = Groups[SourceGroupIndex];
    Niagara_DisconnectStackGroup(SourceGroup, Groups[SourceGroupIndex - 1], Groups[SourceGroupIndex + 1]);
    // The chain without the moved group: input, the other modules, output. Inserting at module position
    // ClampedTargetIndex puts it between Remaining[ClampedTargetIndex] and Remaining[ClampedTargetIndex + 1].
    TArray<FUEAgentNiagaraStackGroup> Remaining = Groups;
    Remaining.RemoveAt(SourceGroupIndex);
    Niagara_ConnectStackGroup(SourceGroup, Remaining[ClampedTargetIndex], Remaining[ClampedTargetIndex + 1]);
    NiagaraGraph->NotifyGraphChanged();
    System->RequestCompile(true);

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Read back from the chain, not computed from the request.
    const int32 NewIndex = Niagara_OrderedStackModules(Out).IndexOfByKey(Target);
    Data->SetNumberField(TEXT("new_index"), NewIndex);
    Data->SetBoolField(TEXT("reordered"), NewIndex != OldIndex);
    if (NewIndex != ClampedTargetIndex)
    {
        mcp_common::AddSaveOutcome(Data, bSaved, SaveError);
        return mcp_common::MakeOutcomeReply(Data, false, FString::Printf(
            TEXT("reorder_niagara_modules: after the rewiring '%s' reads at index %d of the %s stack, not %d"),
            *ModuleName, NewIndex, *ScriptGroup, ClampedTargetIndex));
    }
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("reorder_niagara_modules"), SaveError);
}

// ---------------------------------------------------------------------------
// list_niagara_modules
// Read-only enumeration of the UNiagaraNodeFunctionCall modules
// on an emitter's spawn/update stack, in execution order: ResolveEmitterOutputNode, then
// Niagara_OrderedStackModules walks that stack's parameter-map chain back from its output
// node (the export-safe twin of FNiagaraStackGraphUtilities::GetOrderedModuleNodes, which,
// like GetAllModuleNodes and GetModuleIsEnabled, is NOT exported -> LNK2019). Honest-empty
// arrays when a group has no modules. script_group "all" reads BOTH stacks, grouped as
// spawn:[]/update:[].
// ---------------------------------------------------------------------------
namespace
{
    // Enumerate the ordered UNiagaraNodeFunctionCall modules of an emitter's
    // spawn/update group graph into a JSON array. On resolution failure returns an empty
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
        // The modules of THIS stack in execution order, walked back along its parameter-map chain from its output
        // node -- not every function-call node of the emitter's one shared graph, which mixes every stack with the
        // dynamic inputs.
        const TArray<UNiagaraNodeFunctionCall*> Ordered = Niagara_OrderedStackModules(Out);
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

// MCP:list_niagara_modules - List the modules of one stack, or both stacks, of a Niagara emitter in execution order (read-only), optionally just the module with a given name. script_group picks the particle spawn or update stack (default "update"; "all" reads both and also groups them as spawn[] and update[]); a stack's modules are the function-call nodes on its parameter-map chain from its input to its output node, so the other stack's modules and the dynamic inputs feeding module inputs are not listed, while disabled modules are. Each entry: index (its position in its stack, 0 runs first, the index reorder_niagara_modules and add_niagara_module_to_stack take), module_name (GetFunctionName), node_guid, function_script(+class), is_enabled (IsNodeEnabled), is_deprecated, input/output pin counts. Honest-empty modules:[] when the stack has no modules or the graph is not resolvable (a note is returned); a module_name that matches no module is an error.
//   @param system_path - Asset path of the Niagara System.
//   @param emitter_name - Name of the emitter handle.
//   @param script_group - Optional "spawn", "update" or "all" (both stacks); defaults to "update".
//   @param module_name - Optional module name (GetFunctionName, case-sensitive): return only that module; an error when no module of the stack(s) has it.
//   @returns { system_path: string, emitter_name: string, script_group: string, module_count: int, total_modules_in_stack: int, modules: [{ index, script_group, module_name, node_guid, function_script?, function_script_class?, is_enabled, is_deprecated, input_pin_count, output_pin_count, total_modules_in_stack }], note?: string, spawn?: [{...}], update?: [{...}], spawn_module_count?: int, update_module_count?: int, spawn_note?: string, update_note?: string }

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
    const bool bAllGroups = ScriptGroup.Equals(TEXT("all"), ESearchCase::IgnoreCase);
    const ENiagaraScriptUsage Usage = Niagara_ScriptGroupUsage(ScriptGroup);
    // Optional single-module filter, by exact (case-sensitive) module name.
    FString ModuleNameFilter;
    Params->TryGetStringField(TEXT("module_name"), ModuleNameFilter);

    UNiagaraSystem* System = nullptr;
    FNiagaraEmitterHandle* Handle = Niagara_ResolveEmitterHandle(SystemPath, EmitterName, System, Error);
    if (!Handle)
    {
        return mcp_common::MakeErrorReply(Error);
    }
    FVersionedNiagaraEmitterData* EmitterData = Handle->GetEmitterData();

    // Keeps only the entries named ModuleNameFilter, when one was given.
    auto FilterByName = [&ModuleNameFilter](TArray<TSharedPtr<FJsonValue>>& Modules)
    {
        if (!ModuleNameFilter.IsEmpty())
        {
            Modules.RemoveAll([&ModuleNameFilter](const TSharedPtr<FJsonValue>& Entry)
            {
                return !Entry->AsObject()->GetStringField(TEXT("module_name")).Equals(ModuleNameFilter, ESearchCase::CaseSensitive);
            });
        }
    };

    TSharedPtr<FJsonObject> Data = Niagara_StartEmitterReply(System, EmitterName);
    if (bAllGroups)
    {
        // Both stacks, each through the same export-safe enumeration.
        FString SpawnError;
        int32 SpawnTotal = 0;
        TArray<TSharedPtr<FJsonValue>> SpawnArray = BuildNiagaraModuleArray(
            EmitterData, ENiagaraScriptUsage::ParticleSpawnScript, TEXT("spawn"), SpawnTotal, SpawnError);
        FString UpdateError;
        int32 UpdateTotal = 0;
        TArray<TSharedPtr<FJsonValue>> UpdateArray = BuildNiagaraModuleArray(
            EmitterData, ENiagaraScriptUsage::ParticleUpdateScript, TEXT("update"), UpdateTotal, UpdateError);
        FilterByName(SpawnArray);
        FilterByName(UpdateArray);
        if (!ModuleNameFilter.IsEmpty() && SpawnArray.Num() == 0 && UpdateArray.Num() == 0)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Module '%s' not found on the spawn or update stack of emitter '%s'"), *ModuleNameFilter, *EmitterName));
        }
        UE_LOG(LogTemp, Verbose, TEXT("list_niagara_modules: %s/%s [all] -> spawn:%d update:%d"),
            *SystemPath, *EmitterName, SpawnTotal, UpdateTotal);

        TArray<TSharedPtr<FJsonValue>> AllModules = SpawnArray;
        AllModules.Append(UpdateArray);
        Data->SetStringField(TEXT("script_group"), TEXT("all"));
        Data->SetNumberField(TEXT("module_count"), AllModules.Num());
        Data->SetNumberField(TEXT("total_modules_in_stack"), SpawnTotal + UpdateTotal);
        Data->SetArrayField(TEXT("modules"), AllModules);
        Data->SetArrayField(TEXT("spawn"), SpawnArray);
        Data->SetArrayField(TEXT("update"), UpdateArray);
        Data->SetNumberField(TEXT("spawn_module_count"), SpawnArray.Num());
        Data->SetNumberField(TEXT("update_module_count"), UpdateArray.Num());
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

    FString ResolveError;
    int32 TotalModules = 0;
    TArray<TSharedPtr<FJsonValue>> ModuleArray =
        BuildNiagaraModuleArray(EmitterData, Usage, ScriptGroup, TotalModules, ResolveError);
    UE_LOG(LogTemp, Verbose, TEXT("list_niagara_modules: %s/%s [%s] -> %d module(s)%s"),
        *SystemPath, *EmitterName, *ScriptGroup, TotalModules,
        ResolveError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (note: %s)"), *ResolveError));
    FilterByName(ModuleArray);
    if (!ModuleNameFilter.IsEmpty() && ModuleArray.Num() == 0)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Module '%s' not found on the %s stack of emitter '%s'%s"),
            *ModuleNameFilter, *ScriptGroup, *EmitterName,
            ResolveError.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *ResolveError)));
    }

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

// ===========================================================================
// Emitter event handlers
// Writer side for the reader-only event_handler_count: author
// FNiagaraEventScriptProperties on an emitter's FVersionedNiagaraEmitterData via
// UNiagaraEmitter::AddEventHandler / RemoveEventHandlerByUsageId. Mirrors the
// add_niagara_simulation_stage pattern (the handler's UNiagaraScript MUST be
// allocated — AddEventHandler dereferences EventHandler.Script under WITH_EDITOR;
// the Script's UsageId is the handler's identity for removal).
// ===========================================================================

// MCP:add_niagara_event_handler - Add an event handler to a Niagara emitter (the writer for event_handler_count).  Constructs an FNiagaraEventScriptProperties (allocating its UNiagaraScript with usage ParticleEventScript, mirroring add_niagara_simulation_stage) and adds it via UNiagaraEmitter::AddEventHandler. Idempotent by source_event_name: if a handler for that event already exists it is returned (already_exists=true) without adding a duplicate. A spawn_number, max_events_per_frame or min_spawn_number that is not a JSON number is an error and nothing changes. The system is saved after modification. The handler's event-script graph is created in a default state -- full event wiring must be completed in the Niagara editor (reported honestly via note); the handler properties are persisted and re-readable.
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
    // A count that is present must be a JSON number: a lenient read would write 0 for it.
    for (const TCHAR* Key : { TEXT("spawn_number"), TEXT("max_events_per_frame"), TEXT("min_spawn_number") })
    {
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, Key)) { return Mistyped; }
    }

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
    if (!bExisted)
    {
        // A handler already gone is a valid no-op: nothing was written, so nothing was saved, and that is no failure.
        Resp->SetBoolField(TEXT("saved"), false);
        return mcp_common::MakeOkReply(Resp);
    }
    return mcp_common::MakeSavedReply(Resp, bSaved, TEXT("remove_niagara_event_handler"), SaveError);
}

// MCP:get_niagara_event_handlers - List the event handlers of one emitter in a Niagara System asset (FVersionedNiagaraEmitterData::GetEventHandlers), without changing anything: per handler its usage_id, the source event name and source emitter id it listens to, its execution mode, spawn and event-per-frame limits, and whether it has its event script. Pass usage_id to remove_niagara_event_handler to take one out; add one with add_niagara_event_handler. For the emitter's renderers use get_niagara_emitter_renderers, for its simulation stages get_niagara_simulation_stages. An unknown system or emitter name is an error; an emitter with no handlers is a success with event_handler_count 0.
//   @param system_path - Niagara System asset, package or object path, e.g. '/Game/FX/NS_Sparks'.
//   @param emitter_name - Display name of the emitter handle in that system, case ignored, e.g. 'Sparks', as get_niagara_emitter_handles lists it.
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
    // Set (when bWrite) a bool (uint8:1 / uint32:1), int or float UPROPERTY of the system by member name, telling in
    // bOutDiffers whether Value differs from the value it holds. Returns false if the property could not be located
    // (engine layout drift -> honest error).
    template <typename TProperty, typename TValue>
    bool Niagara_SetSystemProp(UNiagaraSystem* System, const TCHAR* MemberName, const TValue& Value, bool bWrite,
        bool& bOutDiffers)
    {
        TProperty* Prop = FindFProperty<TProperty>(System->GetClass(), MemberName);
        if (!Prop) { return false; }
        bOutDiffers = Prop->GetPropertyValue_InContainer(System) != Value;
        if (bWrite)
        {
            Prop->SetPropertyValue_InContainer(System, Value);
        }
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
    // false (with OutError) if a corner is present but is not an array of 3 finite
    // numbers; missing corners default to the supplied previous values.
    bool ParseFixedBoundsObject(const TSharedPtr<FJsonObject>& Obj, const FBox& Prev, FBox& OutBox, FString& OutError)
    {
        FVector Min = Prev.Min;
        FVector Max = Prev.Max;
        auto ReadCorner = [&OutError](const TSharedPtr<FJsonObject>& O, const TCHAR* Key, FVector& OutVec) -> bool
        {
            if (!O->HasField(Key)) { return true; } // absent: keep previous
            const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
            const bool bIsArray = O->TryGetArrayField(Key, Arr) && Arr;
            bool bValid = bIsArray && Arr->Num() == 3;
            for (int32 Index = 0; bValid && Index < 3; ++Index)
            {
                const TSharedPtr<FJsonValue>& Item = (*Arr)[Index];
                bValid = Item.IsValid() && Item->Type == EJson::Number && FMath::IsFinite(Item->AsNumber());
            }
            if (!bValid)
            {
                OutError = FString::Printf(TEXT("fixed_bounds.%s must be an array of 3 finite numbers"), Key);
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

// MCP:set_niagara_system_properties - Set the render and system flags of a Niagara System asset (cast shadow, decals, custom depth/stencil, translucency sort, large world coordinates, current-frame data, fixed bounds) and save it. Use it instead of set_properties for these fields: a render value also turns its bOverride companion on unless that override_ key is sent, the asset is saved, and PostEditChangeProperty is skipped (no scalability refresh); set_properties writes any other UPROPERTY and leaves it unsaved. Only supplied fields are written, in one undo step. Every field and fixed_bounds is checked before the first write: a corner that is not 3 numbers, a number field that is not a JSON number, or a property the engine lacks is an error and nothing changes. Values the system already holds are a success with changed=false and saved=false. A failed save makes success false with save_error. Read back with get_niagara_system_properties.
//   @retired set_properties - object_path=<system>, properties with the engine names and their "bOverride..." flags, save=true
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
//   @param fixed_bounds - Optional object {min:[x,y,z], max:[x,y,z]}: the system FixedBounds FBox. A corner left out keeps its current value; one that is not 3 finite numbers is an error.
//   @returns { system_path, applied_count, override_applied:[...], <read-back of every field>, fixed_bounds:{min,max}, changed, saved, save_error?, note? }
TSharedPtr<FJsonObject> HandleSetNiagaraSystemProperties(const TSharedPtr<FJsonObject>& Params)
{
    UNiagaraSystem* System = nullptr;
    if (TSharedPtr<FJsonObject> Error = Niagara_LoadSystem(Params, System))
    {
        return Error;
    }

    // A number field that is present must be a JSON number: a lenient read would write 0 for it, or skip it.
    for (const TCHAR* Key : { TEXT("custom_depth_stencil_value"), TEXT("translucency_sort_priority"), TEXT("translucency_sort_distance_offset") })
    {
        if (TSharedPtr<FJsonObject> Mistyped = Niagara_RefuseMistypedNumber(Params, Key)) { return Mistyped; }
    }

    // fixed_bounds is parsed before anything is written, so a malformed corner leaves the system as it was.
    const TSharedPtr<FJsonObject>* FixedBoundsObj = nullptr;
    const bool bHasFixedBounds =
        Params->TryGetObjectField(TEXT("fixed_bounds"), FixedBoundsObj) && FixedBoundsObj && FixedBoundsObj->IsValid();
    FStructProperty* BoxProp = FindFProperty<FStructProperty>(System->GetClass(), TEXT("FixedBounds"));
    FBox NewBox(ForceInit);
    if (bHasFixedBounds && BoxProp)
    {
        FString ParseError;
        if (!ParseFixedBoundsObject(*FixedBoundsObj, System->GetFixedBounds(), NewBox, ParseError))
        {
            return mcp_common::MakeErrorReply(ParseError + TEXT("; nothing was written"));
        }
    }

    // The fields are applied twice: first with bWrite false, which writes nothing but finds a member the system
    // lacks and whether any value would change, then -- only when that pass found no missing member and a change --
    // for real. A missing member is thereby an error with nothing written, and values already held are a no-op.
    bool bWrite = false;
    bool bChanged = false;
    int32 AppliedCount = 0;
    TArray<TSharedPtr<FJsonValue>> OverrideApplied; // names of bOverride* companions auto-set
    TArray<FString> MissingProps;                   // honest layout-drift report

    // A field found counts as applied; a member the system lacks is reported.
    auto Track = [&](bool bFound, bool bDiffers, const TCHAR* Member) -> void
    {
        if (bFound) { ++AppliedCount; bChanged |= bDiffers; }
        else { MissingProps.Add(Member); }
    };
    // Auto-set a render value's bOverride* companion so the override actually takes effect,
    // unless the caller explicitly provided it (handled in the explicit pass below).
    auto ApplyOverrideCompanion = [&](const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        if (!Params->HasField(OverrideJsonKey))
        {
            bool bDiffers = false;
            if (Niagara_SetSystemProp<FBoolProperty>(System, OverrideMember, true, bWrite, bDiffers))
            {
                OverrideApplied.Add(MakeShared<FJsonValueString>(OverrideMember));
                bChanged |= bDiffers;
            }
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
            bool bDiffers = false;
            Track(Niagara_SetSystemProp<FBoolProperty>(System, Member, bVal, bWrite, bDiffers), bDiffers, Member);
        }
    };
    // Render value + companion override.
    auto ApplyBoolWithOverride = [&](const TCHAR* JsonKey, const TCHAR* Member,
                                     const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        bool bVal = false;
        if (Params->TryGetBoolField(JsonKey, bVal))
        {
            bool bDiffers = false;
            Track(Niagara_SetSystemProp<FBoolProperty>(System, Member, bVal, bWrite, bDiffers), bDiffers, Member);
            ApplyOverrideCompanion(OverrideJsonKey, OverrideMember);
        }
    };
    auto ApplyIntWithOverride = [&](const TCHAR* JsonKey, const TCHAR* Member,
                                    const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        int32 Val = 0;
        if (Params->TryGetNumberField(JsonKey, Val))
        {
            bool bDiffers = false;
            Track(Niagara_SetSystemProp<FIntProperty>(System, Member, Val, bWrite, bDiffers), bDiffers, Member);
            ApplyOverrideCompanion(OverrideJsonKey, OverrideMember);
        }
    };
    auto ApplyFloatWithOverride = [&](const TCHAR* JsonKey, const TCHAR* Member,
                                      const TCHAR* OverrideJsonKey, const TCHAR* OverrideMember) -> void
    {
        double Val = 0.0;
        if (Params->TryGetNumberField(JsonKey, Val))
        {
            bool bDiffers = false;
            Track(Niagara_SetSystemProp<FFloatProperty>(System, Member, static_cast<float>(Val), bWrite, bDiffers),
                bDiffers, Member);
            ApplyOverrideCompanion(OverrideJsonKey, OverrideMember);
        }
    };

    auto ApplyAll = [&]() -> void
    {
        AppliedCount = 0;
        OverrideApplied.Reset();
        MissingProps.Reset();

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

        // FixedBounds FBox. FBox::operator== ignores IsValid, which the write also sets, so it is compared apart.
        if (bHasFixedBounds)
        {
            if (!BoxProp)
            {
                MissingProps.Add(TEXT("FixedBounds"));
            }
            else
            {
                FBox* BoxPtr = BoxProp->ContainerPtrToValuePtr<FBox>(System);
                bChanged |= !(BoxPtr->Min == NewBox.Min && BoxPtr->Max == NewBox.Max && BoxPtr->IsValid == NewBox.IsValid);
                if (bWrite)
                {
                    *BoxPtr = NewBox;
                }
                ++AppliedCount;
            }
        }
    };

    ApplyAll();
    if (MissingProps.Num() > 0)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Could not locate UNiagaraSystem properties (engine layout changed?), nothing was written: %s"),
                *FString::Join(MissingProps, TEXT(", "))));
    }

    TSharedPtr<FJsonObject> Data = Niagara_StartSystemReply(System);

    // Values the system already holds (or no field at all) are a valid no-op: nothing is modified or saved.
    if (!bChanged)
    {
        Data->SetNumberField(TEXT("applied_count"), 0);
        Data->SetArrayField(TEXT("override_applied"), TArray<TSharedPtr<FJsonValue>>());
        Niagara_AddSystemFlags(System, Data, nullptr);
        Data->SetBoolField(TEXT("changed"), false);
        Data->SetBoolField(TEXT("saved"), false);
        Data->SetStringField(TEXT("note"), TEXT("The system already holds these values; nothing changed, nothing saved"));
        return mcp_common::MakeOkReply(Data);
    }

    const FScopedTransaction Transaction(NSLOCTEXT("UEAgent", "SetNiagaraSystemProperties", "Set Niagara System Properties (MCP)"));
    System->Modify();
    bWrite = true;
    ApplyAll();

    FString SaveError;
    const bool bSaved = SaveModifiedNiagaraSystem(System, SaveError);

    // Read everything back so the caller gets the authoritative post-set state.
    Data->SetNumberField(TEXT("applied_count"), AppliedCount);
    Data->SetArrayField(TEXT("override_applied"), OverrideApplied);
    Niagara_AddSystemFlags(System, Data, nullptr);
    Data->SetBoolField(TEXT("changed"), true);
    UE_LOG(LogTemp, Verbose, TEXT("[MCP] set_niagara_system_properties: %s applied_count=%d saved=%d"),
        *System->GetPathName(), AppliedCount, bSaved ? 1 : 0);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("set_niagara_system_properties"), SaveError);
}

// MCP:get_niagara_system_properties - Read the render flags (cast shadow, decals, custom depth/stencil, translucency sort, each with its bOverride companion), the large-world/current-frame flags and the fixed bounds of a Niagara System asset, under the names set_niagara_system_properties takes. A curated view; get_properties reads any other UPROPERTY on the system. Read-only, no recompile.
//   @retired get_properties - object_path=<system>, property_names=[engine names with their "bOverride..." flags]
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

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_niagara_data_interface"), &HandleAddNiagaraDataInterface },
    { TEXT("add_niagara_emitter_from_template"), &HandleAddNiagaraEmitterFromTemplate },
    { TEXT("add_niagara_event_handler"), &HandleAddNiagaraEventHandler },
    { TEXT("add_niagara_module_to_stack"), &HandleAddNiagaraModuleToStack },
    { TEXT("add_niagara_renderer"), &HandleAddNiagaraRenderer },
    { TEXT("add_niagara_simulation_stage"), &HandleAddNiagaraSimulationStage },
    { TEXT("create_niagara_system"), &HandleCreateNiagaraSystem },
    { TEXT("duplicate_niagara_emitter_handle"), &HandleDuplicateNiagaraEmitterHandle },
    { TEXT("duplicate_niagara_module_in_stack"), &HandleDuplicateNiagaraModuleInStack },
    { TEXT("get_niagara_component_matrix"), &HandleGetNiagaraComponentMatrix },
    { TEXT("get_niagara_data_interfaces"), &HandleGetNiagaraDataInterfaces },
    { TEXT("get_niagara_effect_type"), &HandleGetNiagaraEffectType },
    { TEXT("get_niagara_emitter_data_interfaces"), &HandleGetNiagaraEmitterDataInterfaces },
    { TEXT("get_niagara_emitter_handles"), &HandleGetNiagaraEmitterHandles },
    { TEXT("get_niagara_emitter_renderers"), &HandleGetNiagaraEmitterRenderers },
    { TEXT("get_niagara_emitter_runtime_summary"), &HandleGetNiagaraEmitterRuntimeSummary },
    { TEXT("get_niagara_event_handlers"), &HandleGetNiagaraEventHandlers },
    { TEXT("get_niagara_module_input"), &HandleGetNiagaraModuleInput },
    { TEXT("get_niagara_renderer_info"), &HandleGetNiagaraRendererInfo },
    { TEXT("get_niagara_simulation_stages"), &HandleGetNiagaraSimulationStages },
    { TEXT("get_niagara_system_info"), &HandleGetNiagaraSystemInfo },
    { TEXT("get_niagara_system_properties"), &HandleGetNiagaraSystemProperties },
    { TEXT("get_niagara_user_parameter"), &HandleGetNiagaraUserParameter },
    { TEXT("list_niagara_data_interface_classes"), &HandleListNiagaraDataInterfaceClasses },
    { TEXT("list_niagara_modules"), &HandleListNiagaraModules },
    { TEXT("list_niagara_renderer_classes"), &HandleListNiagaraRendererClasses },
    { TEXT("move_niagara_simulation_stage"), &HandleMoveNiagaraSimulationStage },
    { TEXT("remove_niagara_data_interface"), &HandleRemoveNiagaraDataInterface },
    { TEXT("remove_niagara_user_parameter"), &HandleRemoveNiagaraUserParameter },
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
    { TEXT("set_niagara_effect_type"), &HandleSetNiagaraEffectType },
    { TEXT("set_niagara_emitter_allocation_mode"), &HandleSetNiagaraEmitterAllocationMode },
    { TEXT("set_niagara_emitter_bounds"), &HandleSetNiagaraEmitterBounds },
    { TEXT("set_niagara_emitter_enabled"), &HandleSetNiagaraEmitterEnabled },
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
    { TEXT("set_niagara_component_fixed_bounds"), &HandleSetNiagaraSystemFixedBounds },
    { TEXT("set_niagara_system_properties"), &HandleSetNiagaraSystemProperties },
    { TEXT("set_niagara_system_warmup_tick_count"), &HandleSetNiagaraSystemWarmupTickCount },
    { TEXT("set_niagara_system_warmup_tick_delta"), &HandleSetNiagaraSystemWarmupTickDelta },
    { TEXT("set_niagara_system_warmup_time"), &HandleSetNiagaraSystemWarmupTime },
    { TEXT("set_niagara_user_parameter"), &HandleSetNiagaraUserParameter },
    { TEXT("spawn_niagara_component"), &HandleSpawnNiagaraComponent },
};
} // namespace UEAgentNiagaraCommands
