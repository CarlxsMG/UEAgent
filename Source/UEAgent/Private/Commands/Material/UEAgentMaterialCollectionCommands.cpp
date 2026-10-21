// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Data/DataAuthoringUtils.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Misc/PackageName.h"
#include "Misc/Guid.h"
// MPCI (transient per-world instance).
#include "Editor.h"                                  // GEditor
#include "Engine/World.h"                            // UWorld::GetParameterCollectionInstance
#include "Engine/EngineTypes.h"                     // FLinearColor

/**
 * Material Parameter Collection commands: authoring and introspection of UMaterialParameterCollection
 * assets and of their transient per-world instances (UMaterialParameterCollectionInstance, MPCI).
 * Asset read-back uses the ENGINE_API getters GetScalarParameterByName / GetVectorParameterByName.
 */
namespace UEAgentMaterialCollectionCommands
{

// MCP-DOMAIN: Material

// ========================================================================== //
// Material Parameter Collections + parameter authoring
//
// The engine API used here:
//   Runtime/Engine/Public/Materials/MaterialParameterCollection.h
//     UMaterialParameterCollection : UObject (UCLASS MinimalAPI, BlueprintType)
//       UPROPERTY TArray<FCollectionScalarParameter> ScalarParameters;  (public)
//       UPROPERTY TArray<FCollectionVectorParameter> VectorParameters;  (public)
//       ENGINE_API const FCollectionScalarParameter* GetScalarParameterByName(FName) const;
//       ENGINE_API const FCollectionVectorParameter* GetVectorParameterByName(FName) const;
//     FCollectionParameterBase { FName ParameterName; FGuid Id; }  (Id auto via ctor)
//     FCollectionScalarParameter : Base { float DefaultValue; }
//     FCollectionVectorParameter : Base { FLinearColor DefaultValue; }
//
// We mutate the public arrays directly + call the virtual PostEditChangeProperty,
// exactly as the engine editor does (MaterialParameterCollectionDetails.cpp:
//   FPropertyChangedEvent CollectionChangedEvent(Prop, EPropertyChangeType::ValueSet);
//   Collection->PostEditChangeProperty(CollectionChangedEvent);).
// The dedicated SetScalarParameterDefaultValue setters are WITH_EDITOR but NOT
// ENGINE_API exported, so we avoid linking them.
// ========================================================================== //

namespace
{
    // Load an MPC asset from a /Game path. Returns nullptr on failure.
    UMaterialParameterCollection* LoadCollectionAt(const FString& CollectionPath)
    {
        return Cast<UMaterialParameterCollection>(
            StaticLoadObject(UMaterialParameterCollection::StaticClass(), nullptr, *CollectionPath));
    }

    // Notify the asset that the named array property changed (canonical editor flow).
    void NotifyCollectionPropertyChanged(UMaterialParameterCollection* Collection, const TCHAR* PropertyName)
    {
        if (!Collection)
        {
            return;
        }
        FProperty* Prop = FindFProperty<FProperty>(UMaterialParameterCollection::StaticClass(), PropertyName);
        FPropertyChangedEvent ChangedEvent(Prop, EPropertyChangeType::ValueSet);
        Collection->PostEditChangeProperty(ChangedEvent);
    }

    // Build a JSON object describing one scalar parameter.
    TSharedPtr<FJsonObject> ScalarParamToJson(const FCollectionScalarParameter& Param)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Param.ParameterName.ToString());
        Obj->SetNumberField(TEXT("default"), Param.DefaultValue);
        Obj->SetStringField(TEXT("id"), Param.Id.ToString());
        return Obj;
    }

    // Build a JSON object describing one vector parameter (RGBA + array form).
    TSharedPtr<FJsonObject> VectorParamToJson(const FCollectionVectorParameter& Param)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("name"), Param.ParameterName.ToString());
        Obj->SetStringField(TEXT("id"), Param.Id.ToString());
        Obj->SetObjectField(TEXT("default"), mcp_common::LinearColorToJsonObject(Param.DefaultValue));

        TArray<TSharedPtr<FJsonValue>> Arr = mcp_common::ColorToJsonArray(Param.DefaultValue);
        Obj->SetArrayField(TEXT("default_rgba"), Arr);
        return Obj;
    }

    // Read an FLinearColor from a JSON array field [r,g,b,a] (a defaults to 1.0 if
    // only 3 entries provided). Returns false if the field is missing.
    bool TryGetLinearColorArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FLinearColor& OutColor)
    {
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        if (!Params->TryGetArrayField(FieldName, Arr) || !Arr)
        {
            return false;
        }
        OutColor = mcp_common::LinearColorFromJsonArray(Arr, FLinearColor(0.0f, 0.0f, 0.0f, 1.0f));
        return true;
    }

    // The UWorld an MPCI command acts on, read from world_label with the plugin's canonical selector
    // (mcp_common::ResolveWorldFromParams): empty or "editor" is the editor world; "pie" (alias "game"),
    // "server", "client" and "pie:N" a Play-in-Editor world. Any other value names a live world, as earlier
    // releases accepted: by its object name or by its package name. The object name alone cannot reach a PIE
    // world: it is duplicated under the editor world's name (UWorld::GetDuplicatedWorldForPIE keeps
    // InWorld->GetFName()) and the editor world's context comes first, so only its package, with the
    // UEDPIE_N_ prefix, tells it apart. Returns nullptr and sets OutError, naming the worlds present, on failure.
    UWorld* ResolveMpciWorld(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString Selector;
        Params->TryGetStringField(TEXT("world_label"), Selector);
        Selector.TrimStartAndEndInline();
        if (Selector.Equals(TEXT("game"), ESearchCase::IgnoreCase))
        {
            Selector = TEXT("pie");
        }
        const bool bSelectorWord = Selector.IsEmpty()
            || Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase)
            || Selector.Equals(TEXT("pie"), ESearchCase::IgnoreCase)
            || Selector.Equals(TEXT("server"), ESearchCase::IgnoreCase)
            || Selector.Equals(TEXT("client"), ESearchCase::IgnoreCase)
            || Selector.StartsWith(TEXT("pie:"), ESearchCase::IgnoreCase);
        if (!bSelectorWord && GEngine)
        {
            for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
            {
                UWorld* World = Ctx.World();
                if (!World)
                {
                    continue;
                }
                const FString PackageName = World->GetOutermost() ? World->GetOutermost()->GetName() : FString();
                if (World->GetName().Equals(Selector, ESearchCase::IgnoreCase)
                    || PackageName.Equals(Selector, ESearchCase::IgnoreCase)
                    || FPackageName::GetShortName(PackageName).Equals(Selector, ESearchCase::IgnoreCase))
                {
                    return World;
                }
            }
        }
        // A selector word, or a name no live world has: the resolver answers, or fails listing the worlds present.
        const TSharedPtr<FJsonObject> WorldParams = MakeShared<FJsonObject>();
        WorldParams->SetStringField(TEXT("world_label"), Selector);
        return mcp_common::ResolveWorldFromParams(WorldParams, OutError, TEXT("world_label"));
    }
}

namespace UEAgentMaterialCollectionImpl
{
    // Reads the required collection_path and param_name and loads that collection; null, with OutError set, on failure.
    UMaterialParameterCollection* LoadCollectionForParam(const TSharedPtr<FJsonObject>& Params, FString& OutCollectionPath, FString& OutParamName, FString& OutError)
    {
        if (!Params->TryGetStringField(TEXT("collection_path"), OutCollectionPath))
        {
            OutError = TEXT("Missing required parameter: collection_path");
            return nullptr;
        }
        if (!Params->TryGetStringField(TEXT("param_name"), OutParamName))
        {
            OutError = TEXT("Missing required parameter: param_name");
            return nullptr;
        }
        UMaterialParameterCollection* Collection = LoadCollectionAt(OutCollectionPath);
        if (!Collection)
        {
            OutError = FString::Printf(TEXT("MaterialParameterCollection not found: %s"), *OutCollectionPath);
        }
        return Collection;
    }

    // Loads the collection at CollectionPath and resolves the MPCI world (world_label); null, with OutError set, on failure.
    UMaterialParameterCollection* LoadCollectionAndWorld(const TSharedPtr<FJsonObject>& Params, const FString& CollectionPath, UWorld*& OutWorld, FString& OutError)
    {
        UMaterialParameterCollection* Collection = LoadCollectionAt(CollectionPath);
        if (!Collection)
        {
            OutError = FString::Printf(TEXT("MaterialParameterCollection not found: %s"), *CollectionPath);
            return nullptr;
        }
        OutWorld = ResolveMpciWorld(Params, OutError);
        return OutWorld ? Collection : nullptr;
    }

    // Collection's instance on World, added when the world has none yet; null when it still has none.
    UMaterialParameterCollectionInstance* FindOrAddInstance(UWorld* World, UMaterialParameterCollection* Collection)
    {
        UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);
        if (!Instance)
        {
            // Auto-create on demand (idempotent — same semantics as the explicit
            // create command). Avoids forcing the caller to do create -> set.
            World->AddParameterCollectionInstance(Collection, /*bUpdateScene=*/true);
            Instance = World->GetParameterCollectionInstance(Collection);
        }
        return Instance;
    }

    // Index of the first parameter named Name in Parameters, or INDEX_NONE.
    template <typename ParameterType>
    int32 FindParameterIndex(const TArray<ParameterType>& Parameters, FName Name)
    {
        return Parameters.IndexOfByPredicate([Name](const ParameterType& Parameter) { return Parameter.ParameterName == Name; });
    }

    // Runs Edit(Parameters, Index) on the parameter named Name in the scalar or the vector array (ParamType picks), then
    // tells the collection that array changed; false when the array holds no parameter of that name.
    template <typename EditType>
    bool EditNamedParameter(UMaterialParameterCollection* Collection, const FString& ParamType, FName Name, EditType Edit)
    {
        auto EditIn = [Collection, Name, &Edit](auto& Parameters, const TCHAR* ArrayName)
        {
            const int32 Index = FindParameterIndex(Parameters, Name);
            if (Index != INDEX_NONE)
            {
                Edit(Parameters, Index);
            }
            NotifyCollectionPropertyChanged(Collection, ArrayName);
            return Index != INDEX_NONE;
        };
        return ParamType == TEXT("scalar")
            ? EditIn(Collection->ScalarParameters, TEXT("ScalarParameters"))
            : EditIn(Collection->VectorParameters, TEXT("VectorParameters"));
    }

    TSharedPtr<FJsonObject> ParamNotFound(const FString& ParamName, const FString& ParamType)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Param '%s' (%s) not found in collection"), *ParamName, *ParamType));
    }

    TSharedPtr<FJsonObject> ParamReply(const FString& CollectionPath, const FString& ParamName, const FString& ParamType)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("collection_path"), CollectionPath);
        Reply->SetStringField(TEXT("param_name"), ParamName);
        Reply->SetStringField(TEXT("param_type"), ParamType);
        return Reply;
    }

    // The reply of create_material_parameter_collection before its outcome: the request, then the collection the path holds.
    TSharedPtr<FJsonObject> CreatedCollectionReply(const FString& Name, const FString& PackagePath, const UMaterialParameterCollection* Collection)
    {
        TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
        Reply->SetStringField(TEXT("name"), Name);
        Reply->SetStringField(TEXT("package_path"), PackagePath);
        Reply->SetStringField(TEXT("collection_path"), Collection->GetPathName());
        Reply->SetStringField(TEXT("asset_path"), Collection->GetPathName());
        Reply->SetStringField(TEXT("collection_class"), Collection->GetClass()->GetName());
        Reply->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
        Reply->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
        return Reply;
    }
}

// MCP:create_material_parameter_collection - Create a UMaterialParameterCollection asset at package_path/name and save it to disk. Idempotent: an existing collection there is returned unchanged with created=false, already_exists=true and saved=false, and nothing is saved, while another asset type at that path is an error naming it. A failed save makes success false with saved=false. Returns collection_path, scalar_count, vector_count and saved. Add parameters with add_collection_parameter (param_type scalar or vector); read them with get_collection_info.
//   @param name - Asset name without a path, e.g. "MPC_Global"
//   @param package_path - Content folder for the asset, e.g. "/Game/MaterialCollections" (the default)
//   @returns { name: string, package_path: string, collection_path: string, asset_path: string, collection_class: string, scalar_count: int, vector_count: int, created: bool, already_exists: bool, success: bool, saved: bool, message: string }
TSharedPtr<FJsonObject> HandleCreateMaterialParameterCollection(const TSharedPtr<FJsonObject>& Params)
{
    FString PackagePath;
    FString Name;

    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("name"), Name, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game/MaterialCollections");
    }

    const FString FullPath = FString::Printf(TEXT("%s/%s"), *PackagePath, *Name);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }

    // Idempotent: a collection already there is returned unchanged and nothing is saved; an asset of any other
    // class there is not the one asked for.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UMaterialParameterCollection* Existing = LoadCollectionAt(FullPath);
        if (Existing && Existing->GetClass() == UMaterialParameterCollection::StaticClass())
        {
            TSharedPtr<FJsonObject> ExistObj = UEAgentMaterialCollectionImpl::CreatedCollectionReply(Name, PackagePath, Existing);
            DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj, TEXT("MaterialParameterCollection"));
            return mcp_common::MakeOkReply(ExistObj);
        }
        return DataAuthoringUtils::OtherClassAtPathReply(FullPath, Existing, UMaterialParameterCollection::StaticClass());
    }

    FString CreateError;
    UMaterialParameterCollection* NewCollection = mcp_common::NewAssetInPackage<UMaterialParameterCollection>(
        FullPath, Name, TEXT("Failed to create MaterialParameterCollection"), CreateError);
    if (!NewCollection)
    {
        return mcp_common::MakeErrorReply(CreateError);
    }

    FAssetRegistryModule::AssetCreated(NewCollection);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(NewCollection->GetOutermost(), NewCollection, SaveError);

    TSharedPtr<FJsonObject> ResultObj = UEAgentMaterialCollectionImpl::CreatedCollectionReply(Name, PackagePath, NewCollection);
    DataAuthoringUtils::AddCreatedOutcome(ResultObj, bSaved, TEXT("MaterialParameterCollection"), SaveError);

    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSaved,
        bSaved ? FString() : mcp_common::MeasuredOutcomeError(TEXT("create_material_parameter_collection"), TEXT("bSaved")));
}

namespace UEAgentMaterialCollectionImpl
{
    // add_collection_parameter for param_type vector: appends a vector parameter with a new GUID, or overwrites the
    // default of the vector of that name, then saves the collection and reads it back.
    TSharedPtr<FJsonObject> AddVectorParameter(const TSharedPtr<FJsonObject>& Params, UMaterialParameterCollection* Collection,
        const FString& CollectionPath, const FString& ParamName)
    {
        // Default value: optional [r, g, b, a] array under "default_value" (default (0,0,0,1)).
        FLinearColor DefaultColor(0.0f, 0.0f, 0.0f, 1.0f);
        TryGetLinearColorArray(Params, TEXT("default_value"), DefaultColor);

        const FName ParamFName(*ParamName);
        const int32 CountBefore = Collection->VectorParameters.Num();

        const int32 ExistingIndex = FindParameterIndex(Collection->VectorParameters, ParamFName);
        const bool bAlreadyExisted = ExistingIndex != INDEX_NONE;

        Collection->Modify();

        if (!bAlreadyExisted)
        {
            FCollectionVectorParameter NewParam;
            NewParam.ParameterName = ParamFName;
            NewParam.DefaultValue = DefaultColor;
            NewParam.Id = FGuid::NewGuid();
            Collection->VectorParameters.Add(NewParam);
        }
        else
        {
            Collection->VectorParameters[ExistingIndex].DefaultValue = DefaultColor;
        }

        NotifyCollectionPropertyChanged(Collection, TEXT("VectorParameters"));

        FString SaveError;
        const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

        const FCollectionVectorParameter* Found = Collection->GetVectorParameterByName(ParamFName);
        const int32 CountAfter = Collection->VectorParameters.Num();

        TSharedPtr<FJsonObject> ResultObj = ParamReply(CollectionPath, ParamName, TEXT("vector"));
        ResultObj->SetBoolField(TEXT("param_added"), Found != nullptr);
        ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
        ResultObj->SetNumberField(TEXT("vector_count_before"), CountBefore);
        ResultObj->SetNumberField(TEXT("vector_count"), CountAfter);
        if (Found)
        {
            ResultObj->SetObjectField(TEXT("default"), mcp_common::LinearColorToJsonObject(Found->DefaultValue));
            ResultObj->SetArrayField(TEXT("default_rgba"), mcp_common::ColorToJsonArray(Found->DefaultValue));
            ResultObj->SetStringField(TEXT("id"), Found->Id.ToString());
        }

        return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_collection_parameter"), SaveError);
    }
}

// MCP:add_collection_parameter - Add a scalar or vector (RGBA color) parameter with a new GUID to a Material Parameter Collection asset and save it; a new parameter changes the collection layout, so the engine recompiles the materials that use the collection. An existing parameter of that name and kind only gets its default overwritten (already_existed=true). The kind is param_type, or inferred from default_value: an [r, g, b, a] array means a vector, anything else a scalar. Returns the count before and after for that kind, the default and the id. A failed save makes success false with saved=false and save_error, the change kept in memory. Change a default later with set_collection_parameter_default; read back with get_collection_info.
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the new parameter
//   @param param_type - Optional "scalar" or "vector"; default inferred from default_value (an array -> vector, else scalar). Any other value is an error
//   @param default_value - Scalar: default float value, default 0. Vector: [r, g, b, a] floats, default [0, 0, 0, 1]; a shorter array keeps 0 for missing color channels and 1 for alpha. An existing parameter of that name gets this value too, even when it is omitted
//   @returns { collection_path: string, param_name: string, param_type: string, param_added: bool, already_existed: bool, scalar_count_before?: int, scalar_count?: int, default_value?: number (scalar), vector_count_before?: int, vector_count?: int, default?: {r,g,b,a}, default_rgba?: [number] (vector), id: string, saved: bool }
TSharedPtr<FJsonObject> HandleAddCollectionScalarParam(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath, ParamName, LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionForParam(Params, CollectionPath, ParamName, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }

    // The kind: param_type, else the shape of default_value (an array is a color).
    FString ParamType;
    Params->TryGetStringField(TEXT("param_type"), ParamType);
    ParamType = ParamType.ToLower();
    const TArray<TSharedPtr<FJsonValue>>* DefaultArray = nullptr;
    if (ParamType.IsEmpty())
    {
        ParamType = Params->TryGetArrayField(TEXT("default_value"), DefaultArray) ? TEXT("vector") : TEXT("scalar");
    }
    if (ParamType == TEXT("vector"))
    {
        return UEAgentMaterialCollectionImpl::AddVectorParameter(Params, Collection, CollectionPath, ParamName);
    }
    if (ParamType != TEXT("scalar"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown param_type: %s (expected scalar or vector)"), *ParamType));
    }

    double DefaultValueD = 0.0;
    Params->TryGetNumberField(TEXT("default_value"), DefaultValueD);

    const FName ParamFName(*ParamName);
    const int32 CountBefore = Collection->ScalarParameters.Num();

    // If a scalar param with this name already exists, treat as idempotent update of
    // its default (do not append a duplicate).
    const int32 ExistingIndex = UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->ScalarParameters, ParamFName);
    const bool bAlreadyExisted = ExistingIndex != INDEX_NONE;

    Collection->Modify();

    if (!bAlreadyExisted)
    {
        FCollectionScalarParameter NewParam;
        NewParam.ParameterName = ParamFName;
        NewParam.DefaultValue = static_cast<float>(DefaultValueD);
        NewParam.Id = FGuid::NewGuid();   // unique id per parameter
        Collection->ScalarParameters.Add(NewParam);
    }
    else
    {
        Collection->ScalarParameters[ExistingIndex].DefaultValue = static_cast<float>(DefaultValueD);
    }

    NotifyCollectionPropertyChanged(Collection, TEXT("ScalarParameters"));

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    // Verify by read-back via the ENGINE_API getter.
    const FCollectionScalarParameter* Found = Collection->GetScalarParameterByName(ParamFName);
    const int32 CountAfter = Collection->ScalarParameters.Num();

    TSharedPtr<FJsonObject> ResultObj = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, TEXT("scalar"));
    ResultObj->SetBoolField(TEXT("param_added"), Found != nullptr);
    ResultObj->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    ResultObj->SetNumberField(TEXT("scalar_count_before"), CountBefore);
    ResultObj->SetNumberField(TEXT("scalar_count"), CountAfter);
    if (Found)
    {
        ResultObj->SetNumberField(TEXT("default_value"), Found->DefaultValue);
        ResultObj->SetStringField(TEXT("id"), Found->Id.ToString());
    }

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_collection_parameter"), SaveError);
}

// MCP:set_collection_parameter_default - Set the default of an existing scalar or vector parameter of a Material Parameter Collection asset and save it: a scalar takes scalar_value, a vector takes vector [r, g, b, a]. Errors when neither array has the name or the needed value is missing.
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the existing parameter to update
//   @param scalar_value - Optional new float value for a scalar parameter
//   @param vector [array] - New linear color for a vector parameter, as a JSON array [r, g, b, a]; alpha is 1 when only three values are given
//   @returns { collection_path: string, param_name: string, param_type: string, default_set: bool, saved: bool, default_value: number, scalar_value: number, default: object, default_rgba: [number] }
TSharedPtr<FJsonObject> HandleSetCollectionParamDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath, ParamName, LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionForParam(Params, CollectionPath, ParamName, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }

    const FName ParamFName(*ParamName);

    // Find the param by name. A scalar is identified by a "scalar_value" field;
    // a vector by a "vector"/"vector_value"/"default_value" array. We also fall back
    // to whichever array actually contains the name.
    double ScalarValueD = 0.0;
    const bool bHasScalar = Params->TryGetNumberField(TEXT("scalar_value"), ScalarValueD);

    FLinearColor VectorValue(0.0f, 0.0f, 0.0f, 1.0f);
    bool bHasVector = TryGetLinearColorArray(Params, TEXT("vector"), VectorValue);
    if (!bHasVector) { bHasVector = TryGetLinearColorArray(Params, TEXT("vector_value"), VectorValue); }
    if (!bHasVector) { bHasVector = TryGetLinearColorArray(Params, TEXT("default_value"), VectorValue); }

    // Locate the parameter in either array.
    const int32 ScalarIdx = UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->ScalarParameters, ParamFName);
    const int32 VectorIdx = UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->VectorParameters, ParamFName);

    if (ScalarIdx == INDEX_NONE && VectorIdx == INDEX_NONE)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Parameter '%s' not found in collection (no scalar or vector match)"), *ParamName));
    }

    Collection->Modify();

    FString AppliedType;
    if (ScalarIdx != INDEX_NONE && (bHasScalar || !bHasVector))
    {
        // Treat as a scalar update. If no explicit scalar_value was supplied, leave
        // it unchanged (set to current) — but require scalar_value for a real change.
        if (!bHasScalar)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parameter '%s' is scalar; provide scalar_value"), *ParamName));
        }
        Collection->ScalarParameters[ScalarIdx].DefaultValue = static_cast<float>(ScalarValueD);
        NotifyCollectionPropertyChanged(Collection, TEXT("ScalarParameters"));
        AppliedType = TEXT("scalar");
    }
    else if (VectorIdx != INDEX_NONE)
    {
        if (!bHasVector)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Parameter '%s' is vector; provide vector [r,g,b,a]"), *ParamName));
        }
        Collection->VectorParameters[VectorIdx].DefaultValue = VectorValue;
        NotifyCollectionPropertyChanged(Collection, TEXT("VectorParameters"));
        AppliedType = TEXT("vector");
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    TSharedPtr<FJsonObject> ResultObj = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, AppliedType);
    ResultObj->SetBoolField(TEXT("default_set"), true);

    // Read-back the applied value via the ENGINE_API getters.
    if (AppliedType == TEXT("scalar"))
    {
        const FCollectionScalarParameter* Found = Collection->GetScalarParameterByName(ParamFName);
        if (Found)
        {
            ResultObj->SetNumberField(TEXT("default_value"), Found->DefaultValue);
            ResultObj->SetNumberField(TEXT("scalar_value"), Found->DefaultValue);
        }
    }
    else if (AppliedType == TEXT("vector"))
    {
        const FCollectionVectorParameter* Found = Collection->GetVectorParameterByName(ParamFName);
        if (Found)
        {
            ResultObj->SetObjectField(TEXT("default"), mcp_common::LinearColorToJsonObject(Found->DefaultValue));
            ResultObj->SetArrayField(TEXT("default_rgba"), mcp_common::ColorToJsonArray(Found->DefaultValue));
        }
    }

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_collection_parameter_default"), SaveError);
}

// MCP:get_collection_info - Read the parameters of a Material Parameter Collection asset: scalar_params (name, default, id, index), vector_params (name, id, default object, default_rgba, index), scalar_count, vector_count and total_count. Read-only. Narrow it to one parameter with param_name and/or one kind with param_type: the lists then hold only the matches, found says whether the name matched, and the counts stay the collection's totals. index is the parameter's position in its scalar or vector array. The live per-world values: get_material_parameter_collection_instance_info.
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Optional: list only the parameter with this name, and report found
//   @param param_type - Optional "scalar" or "vector": list only that kind. Any other value is an error
//   @returns { collection_path: string, collection_name: string, collection_class: string, scalar_params: [{name, default, id, index}], vector_params: [{name, id, default, default_rgba, index}], scalar_count: int, vector_count: int, total_count: int, found?: bool (with param_name) }
TSharedPtr<FJsonObject> HandleGetCollectionInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NameFilter;
    Params->TryGetStringField(TEXT("param_name"), NameFilter);
    FString TypeFilter;
    Params->TryGetStringField(TEXT("param_type"), TypeFilter);
    TypeFilter = TypeFilter.ToLower();
    if (!TypeFilter.IsEmpty() && TypeFilter != TEXT("scalar") && TypeFilter != TEXT("vector"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown param_type: %s (expected scalar or vector)"), *TypeFilter));
    }

    UMaterialParameterCollection* Collection = LoadCollectionAt(CollectionPath);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("MaterialParameterCollection not found: %s"), *CollectionPath));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("collection_path"), CollectionPath);
    ResultObj->SetStringField(TEXT("collection_name"), Collection->GetName());
    ResultObj->SetStringField(TEXT("collection_class"), Collection->GetClass()->GetName());

    // A parameter is listed when it passes both filters; index is its position in its own array.
    const FName NameFName(*NameFilter);
    auto Listed = [&NameFilter, &NameFName](FName ParameterName) { return NameFilter.IsEmpty() || ParameterName == NameFName; };

    TArray<TSharedPtr<FJsonValue>> ScalarArr;
    if (TypeFilter.IsEmpty() || TypeFilter == TEXT("scalar"))
    {
        for (int32 Index = 0; Index < Collection->ScalarParameters.Num(); ++Index)
        {
            const FCollectionScalarParameter& P = Collection->ScalarParameters[Index];
            if (!Listed(P.ParameterName))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = ScalarParamToJson(P);
            Entry->SetNumberField(TEXT("index"), Index);
            ScalarArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
    ResultObj->SetArrayField(TEXT("scalar_params"), ScalarArr);

    TArray<TSharedPtr<FJsonValue>> VectorArr;
    if (TypeFilter.IsEmpty() || TypeFilter == TEXT("vector"))
    {
        for (int32 Index = 0; Index < Collection->VectorParameters.Num(); ++Index)
        {
            const FCollectionVectorParameter& P = Collection->VectorParameters[Index];
            if (!Listed(P.ParameterName))
            {
                continue;
            }
            TSharedPtr<FJsonObject> Entry = VectorParamToJson(P);
            Entry->SetNumberField(TEXT("index"), Index);
            VectorArr.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }
    ResultObj->SetArrayField(TEXT("vector_params"), VectorArr);

    ResultObj->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    ResultObj->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    ResultObj->SetNumberField(TEXT("total_count"),
        Collection->ScalarParameters.Num() + Collection->VectorParameters.Num());
    if (!NameFilter.IsEmpty())
    {
        ResultObj->SetBoolField(TEXT("found"), ScalarArr.Num() + VectorArr.Num() > 0);
    }

    return mcp_common::MakeOkReply(ResultObj);
}

// ===========================================================================
// MPC advanced: remove / rename / clear / regenerate-id /
// per-param info + duplicate-collection. All headless-safe (direct array
// mutation + SavePackage, same flow as the existing add/info handlers).
// ===========================================================================

// Helper: resolve param_type ("scalar" | "vector") to the bit we'll set on the
// result payload. Defaults to "scalar" if missing/empty/unknown.
static FString ResolveParamType(const TSharedPtr<FJsonObject>& Params)
{
    FString Type;
    Params->TryGetStringField(TEXT("param_type"), Type);
    Type = Type.ToLower();
    if (Type != TEXT("scalar") && Type != TEXT("vector"))
    {
        Type = TEXT("scalar");
    }
    return Type;
}

// MCP:remove_collection_parameter - Remove one parameter from a Material Parameter Collection asset by name and save it, or with all=true clear every parameter of a kind (or both kinds) at once. Errors when param_name is not in the array param_type selects. Reports the scalar and vector counts before and after.
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the parameter to remove. Required unless all is true, and must be omitted with all=true
//   @param param_type - Which array: "scalar" or "vector" (default scalar); with all=true also "all" (the default then) for both arrays. Other values: scalar without all, an error with all=true
//   @param all - Optional bool (default false): clear every parameter of param_type instead of one
//   @returns { collection_path: string, param_name?: string, param_type: string, removed?: bool, cleared?: bool (all=true), scalar_count_before: int, vector_count_before: int, scalar_count: int, vector_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveCollectionParam(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // param_name is required unless all=true, so it is read as optional and checked below.
    FString ParamName;
    const bool bHasParamName = Params->TryGetStringField(TEXT("param_name"), ParamName);
    bool bAll = false;
    Params->TryGetBoolField(TEXT("all"), bAll);
    FString RequestedType;
    Params->TryGetStringField(TEXT("param_type"), RequestedType);
    RequestedType = RequestedType.ToLower();

    FString Type;
    if (bAll)
    {
        if (bHasParamName)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("all=true clears every parameter of param_type; omit param_name ('%s') or drop all to remove just that one"), *ParamName));
        }
        Type = RequestedType.IsEmpty() ? FString(TEXT("all")) : RequestedType;
        if (Type != TEXT("scalar") && Type != TEXT("vector") && Type != TEXT("all"))
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Unknown param_type: %s (expected scalar, vector or all)"), *RequestedType));
        }
    }
    else
    {
        if (!bHasParamName)
        {
            return mcp_common::MissingParameterReply(TEXT("param_name"));
        }
        if (RequestedType == TEXT("all"))
        {
            return mcp_common::MakeErrorReply(TEXT("param_type all clears a whole kind and needs all=true"));
        }
        Type = ResolveParamType(Params);
    }

    UMaterialParameterCollection* Collection = LoadCollectionAt(CollectionPath);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("MaterialParameterCollection not found: %s"), *CollectionPath));
    }

    Collection->Modify();
    const int32 ScalarsBefore = Collection->ScalarParameters.Num();
    const int32 VectorsBefore = Collection->VectorParameters.Num();

    TSharedPtr<FJsonObject> Result;
    if (bAll)
    {
        if (Type == TEXT("scalar") || Type == TEXT("all"))
        {
            Collection->ScalarParameters.Reset();
            NotifyCollectionPropertyChanged(Collection, TEXT("ScalarParameters"));
        }
        if (Type == TEXT("vector") || Type == TEXT("all"))
        {
            Collection->VectorParameters.Reset();
            NotifyCollectionPropertyChanged(Collection, TEXT("VectorParameters"));
        }
        Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("collection_path"), CollectionPath);
        Result->SetStringField(TEXT("param_type"), Type);
        Result->SetBoolField(TEXT("cleared"), true);
    }
    else
    {
        const FName ParamFName(*ParamName);
        const bool bRemoved = UEAgentMaterialCollectionImpl::EditNamedParameter(Collection, Type, ParamFName, [](auto& Parameters, int32 Index)
        {
            Parameters.RemoveAt(Index);
        });
        if (!bRemoved)
        {
            return UEAgentMaterialCollectionImpl::ParamNotFound(ParamName, Type);
        }
        Result = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, Type);
        Result->SetBoolField(TEXT("removed"), true);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    Result->SetNumberField(TEXT("scalar_count_before"), ScalarsBefore);
    Result->SetNumberField(TEXT("vector_count_before"), VectorsBefore);
    Result->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    Result->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("remove_collection_parameter"), SaveError);
}

// MCP:rename_collection_parameter - Rename one parameter of a Material Parameter Collection asset, keeping its Id, and save it. Refuses when new_name is already used in the same array, and errors when param_name is not in the array param_type selects (default scalar).
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Current parameter name
//   @param new_name - New parameter name, must not collide with another
//   @param param_type - Which array to search: "scalar" or "vector", any case; omitted or anything else means scalar
//   @returns { collection_path: string, param_name: string, new_name: string, param_type: string, renamed: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRenameCollectionParam(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString ParamName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("param_name"), ParamName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }
    const FString Type = ResolveParamType(Params);
    const FName OldFName(*ParamName);
    const FName NewFName(*NewName);

    UMaterialParameterCollection* Collection = LoadCollectionAt(CollectionPath);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("MaterialParameterCollection not found: %s"), *CollectionPath));
    }

    Collection->Modify();

    // Refuse to rename over an existing different param.
    const bool bNameTaken = Type == TEXT("scalar")
        ? UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->ScalarParameters, NewFName) != INDEX_NONE
        : UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->VectorParameters, NewFName) != INDEX_NONE;
    if (bNameTaken)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Another %s param named '%s' already exists"), *Type, *NewName));
    }
    const bool bRenamed = UEAgentMaterialCollectionImpl::EditNamedParameter(Collection, Type, OldFName, [&NewFName](auto& Parameters, int32 Index)
    {
        Parameters[Index].ParameterName = NewFName;
    });
    if (!bRenamed)
    {
        return UEAgentMaterialCollectionImpl::ParamNotFound(ParamName, Type);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), CollectionPath);
    Result->SetStringField(TEXT("param_name"), ParamName);
    Result->SetStringField(TEXT("new_name"), NewName);
    Result->SetStringField(TEXT("param_type"), Type);
    Result->SetBoolField(TEXT("renamed"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("rename_collection_parameter"), SaveError);
}

// MCP:regenerate_collection_parameter_id - Assign a new random FGuid Id to one parameter of a Material Parameter Collection asset and save it; returns new_id. It searches only the array param_type selects (default scalar) and errors when the name is not there.
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the parameter to regenerate the id for
//   @param param_type - Which array to search: "scalar" or "vector", any case; omitted or anything else means scalar
//   @returns { collection_path: string, param_name: string, param_type: string, new_id: string, regenerated: bool, saved: bool }
TSharedPtr<FJsonObject> HandleRegenerateCollectionParamId(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath, ParamName, LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionForParam(Params, CollectionPath, ParamName, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    const FString Type = ResolveParamType(Params);
    const FName ParamFName(*ParamName);

    Collection->Modify();
    const FGuid NewId = FGuid::NewGuid();
    const bool bRegenerated = UEAgentMaterialCollectionImpl::EditNamedParameter(Collection, Type, ParamFName, [&NewId](auto& Parameters, int32 Index)
    {
        Parameters[Index].Id = NewId;
    });
    if (!bRegenerated)
    {
        return UEAgentMaterialCollectionImpl::ParamNotFound(ParamName, Type);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    TSharedPtr<FJsonObject> Result = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, Type);
    Result->SetStringField(TEXT("new_id"), NewId.ToString());
    Result->SetBoolField(TEXT("regenerated"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("regenerate_collection_parameter_id"), SaveError);
}

// MCP:duplicate_material_parameter_collection - Duplicate a Material Parameter Collection asset, parameters included with their names, Ids and defaults, to package_path/new_name and save the copy. Refuses when the target already exists. Returns collection_path, scalar_count, vector_count, saved and save_error.
//   @param source_path - Source MPC path, e.g. "/Game/MPC/MPC_A" or the object path "/Game/MPC/MPC_A.MPC_A"
//   @param new_name - Asset name of the copy, e.g. "MPC_B"; must not be empty
//   @param package_path - Target folder; default is the folder of source_path when it is an object path with a dot ("/Game/MPC/MPC_A.MPC_A"), else "/Game/MaterialCollections"
//   @returns { source_path: string, collection_path: string, asset_path: string, name: string, package_path: string, scalar_count: int, vector_count: int, duplicated: bool, saved: bool, save_error: string }
TSharedPtr<FJsonObject> HandleDuplicateMpc(const TSharedPtr<FJsonObject>& Params)
{
    FString SourcePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("source_path"), SourcePath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString NewName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("new_name"), NewName)) { return Missing; }
    FString PackagePath;
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        // Default to the source's package path so the clone lands next to the source.
        FString SourcePackageName;
        if (SourcePath.Split(TEXT("."), &SourcePackageName, nullptr))
        {
            int32 LastSlash = INDEX_NONE;
            SourcePackageName.FindLastChar(TEXT('/'), LastSlash);
            if (LastSlash != INDEX_NONE)
            {
                PackagePath = SourcePackageName.Left(LastSlash);
            }
        }
        if (PackagePath.IsEmpty())
        {
            PackagePath = TEXT("/Game/MaterialCollections");
        }
    }

    UMaterialParameterCollection* Source = LoadCollectionAt(SourcePath);
    if (!Source)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Source MaterialParameterCollection not found: %s"), *SourcePath));
    }

    const FString TargetPackageName = FString::Printf(TEXT("%s/%s"), *PackagePath, *NewName);
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(TargetPackageName)) { return Invalid; }
    const FString TargetObjectName = NewName;
    const FString FullPath = FString::Printf(TEXT("%s.%s"), *TargetPackageName, *TargetObjectName);

    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Target MPC already exists: %s"), *FullPath));
    }

    UPackage* TargetPackage = CreatePackage(*TargetPackageName);
    if (!TargetPackage)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to create target package: %s"), *TargetPackageName));
    }
    TargetPackage->FullyLoad();

    // DuplicateObject performs a deep UPROPERTY copy: ScalarParameters and
    // VectorParameters are UPROPERTY arrays, so the entries are cloned with
    // their FName + FGuid + default value preserved.
    UMaterialParameterCollection* Clone = Cast<UMaterialParameterCollection>(
        StaticDuplicateObject(Source, TargetPackage, *TargetObjectName,
            RF_Public | RF_Standalone | RF_Transactional));
    if (!Clone)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to duplicate MaterialParameterCollection to %s"), *FullPath));
    }

    FAssetRegistryModule::AssetCreated(Clone);

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(TargetPackage, Clone, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("source_path"), Source->GetPathName());
    Result->SetStringField(TEXT("collection_path"), Clone->GetPathName());
    Result->SetStringField(TEXT("asset_path"), Clone->GetPathName());
    Result->SetStringField(TEXT("name"), NewName);
    Result->SetStringField(TEXT("package_path"), PackagePath);
    Result->SetNumberField(TEXT("scalar_count"), Clone->ScalarParameters.Num());
    Result->SetNumberField(TEXT("vector_count"), Clone->VectorParameters.Num());
    Result->SetBoolField(TEXT("duplicated"), true);
    if (!SaveError.IsEmpty())
    {
        Result->SetStringField(TEXT("save_error"), SaveError);
    }
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("duplicate_material_parameter_collection"));
}

// ========================================================================== //
// Material Parameter Collection Instance (MPCI)
//
// MPCIs are TRANSIENT per-world runtime objects (verified
// MaterialParameterCollectionInstance.h: "Instances of this class
// are always transient."). The canonical public path to obtain / create one
// is UWorld::AddParameterCollectionInstance(MPC, bUpdateScene) +
// UWorld::GetParameterCollectionInstance(MPC). Every MPCI command routes through a
// world (editor by default; a Play-in-Editor world when world_label selects one, see
// ResolveMpciWorld) and stamps the reply with that world's identity (AddWorldEvidence).
//
//   set_material_parameter_collection_instance_value        override one scalar (value) or
//                                          vector (R/G/B/A or rgba=[...]) on
//                                          the per-world instance.
//   get_material_parameter_collection_instance_info                          readback: parent collection
//                                          path, override count, current
//                                          override values for all params.
//   reset_material_parameter_collection_instance         get-or-create the instance, then
//                                          ForceReturnToDefaultValues +
//                                          UpdateRenderState on it.
//
// All operations are direct on the live UMaterialParameterCollectionInstance
// object — no asset file I/O (MPCI instances are runtime/transient).
// ========================================================================== //

// MCP:set_material_parameter_collection_instance_value - Override one scalar or vector (color) parameter on the Material Parameter Collection Instance (MPCI: the transient per-world values) of a collection asset on one world, creating the instance when needed, and push the value to the renderer. A scalar takes value; a vector takes rgba or r/g/b/a (r/g/b/a override the matching rgba entries, and channels given by neither are 0, alpha 1, not the current value) -- the color form is used when any of those is given or when parameter_name is a vector of the collection. The world is the editor world by default, or the Play-in-Editor world world_label selects; world_package and world_type say which one was reached. Runtime only: the asset is untouched, nothing is saved, no undo, and the override is lost when the instance is rebuilt. A missing value or color, a collection that does not load, an unmatched world, or a parameter_name that is not a parameter of that kind in the collection is an error; readback_value / readback_rgba is what the instance holds afterwards, and one that differs from what was written makes success false with readback_ok=false. Saved default: set_collection_parameter_default; drop every override: reset_material_parameter_collection_instance; read values: get_material_parameter_collection_instance_info.
//   @param collection_path - Full /Game path of the parent MPC
//   @param parameter_name - Name of the scalar or vector parameter to override
//   @param value - New float value for a scalar parameter. Required unless a color (rgba or r/g/b/a) is given for a vector parameter
//   @param rgba - Optional [r, g, b, a] color for a vector parameter
//   @param r - Optional red channel for a vector parameter, alternative to rgba
//   @param g - Optional green channel
//   @param b - Optional blue channel
//   @param a - Optional alpha channel
//   @param world_label - Which world: empty or "editor" for the editor world (the default), even while Play-in-Editor runs; "pie" (alias "game") for the running PIE world, "server" or "client" for that PIE world, "pie:N" for PIE instance N. any other value names a live world by its object name (the editor world is checked first, so a map name selects it) or by its package name (UEDPIE_N_ and the map name for a PIE world), and a value no world has fails and lists the worlds present
//   @returns { collection_path: string, parameter_name: string, param_type: string ("scalar" or "vector"), value?: number, readback_value?: number (scalar), value_rgba?: [number], readback_rgba?: [number] (vector), readback_ok: bool, world_name: string, world_package: string, world_type: string, net_mode: string, pie_instance: int }
TSharedPtr<FJsonObject> HandleSetMpciScalarParameterValue(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath)) { return Missing; }
    FString ParamName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName)) { return Missing; }
    double ValueD = 0.0;
    const bool bHasValue = Params->TryGetNumberField(TEXT("value"), ValueD);
    const float Value = static_cast<float>(ValueD);

    // A color: a flat rgba=[] array and/or individual r/g/b/a number fields.
    FLinearColor Color(0.0f, 0.0f, 0.0f, 1.0f);
    bool bHasColor = false;
    const TArray<TSharedPtr<FJsonValue>>* RgbaArr = nullptr;
    if (Params->TryGetArrayField(TEXT("rgba"), RgbaArr) && RgbaArr && RgbaArr->Num() >= 3)
    {
        Color.R = static_cast<float>((*RgbaArr)[0]->AsNumber());
        Color.G = static_cast<float>((*RgbaArr)[1]->AsNumber());
        Color.B = static_cast<float>((*RgbaArr)[2]->AsNumber());
        if (RgbaArr->Num() >= 4) { Color.A = static_cast<float>((*RgbaArr)[3]->AsNumber()); }
        bHasColor = true;
    }
    double NumBuf = 0.0;
    if (Params->TryGetNumberField(TEXT("r"), NumBuf)) { Color.R = static_cast<float>(NumBuf); bHasColor = true; }
    if (Params->TryGetNumberField(TEXT("g"), NumBuf)) { Color.G = static_cast<float>(NumBuf); bHasColor = true; }
    if (Params->TryGetNumberField(TEXT("b"), NumBuf)) { Color.B = static_cast<float>(NumBuf); bHasColor = true; }
    if (Params->TryGetNumberField(TEXT("a"), NumBuf)) { Color.A = static_cast<float>(NumBuf); bHasColor = true; }

    // Nothing to write: refused before the collection is loaded or a world is resolved.
    if (!bHasValue && !bHasColor)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: value (a number, for a scalar parameter), or rgba / r,g,b,a for a vector parameter. Nothing was written."));
    }

    UWorld* World = nullptr;
    FString LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionAndWorld(Params, CollectionPath, World, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }

    // The kind: a color was sent, or the name is a vector of the collection and is not one of its scalars.
    const bool bIsScalarParam = Collection->GetScalarParameterByName(*ParamName) != nullptr;
    const bool bIsVectorParam = Collection->GetVectorParameterByName(*ParamName) != nullptr;
    const bool bVector = bHasColor || (bIsVectorParam && !bIsScalarParam);
    if (bVector && !bHasColor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' is a vector parameter of collection '%s': pass rgba (array of 3-4 numbers) or r/g/b/a"), *ParamName, *CollectionPath));
    }
    UMaterialParameterCollectionInstance* Instance = UEAgentMaterialCollectionImpl::FindOrAddInstance(World, Collection);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to obtain MaterialParameterCollectionInstance from world"));
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    Result->SetStringField(TEXT("parameter_name"), ParamName);
    Result->SetStringField(TEXT("param_type"), bVector ? TEXT("vector") : TEXT("scalar"));
    bool bReadback = false;
    FString ReadbackError;
    if (bVector)
    {
        // The setter returns false for a name the collection does not have, so check it upfront for a clear error.
        if (!bIsVectorParam)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Vector parameter '%s' not found on collection '%s'"),
                                *ParamName, *CollectionPath));
        }
        Instance->SetVectorParameterValue(FName(*ParamName), Color);
        Instance->UpdateRenderState(/*bRecreateUniformBuffer=*/false);

        // Read back: the getter answers the override, or the collection default when there is none, so only a color
        // equal to the one written proves the override landed.
        FLinearColor OutColor(0.0f, 0.0f, 0.0f, 1.0f);
        bReadback = Instance->GetVectorParameterValue(FName(*ParamName), OutColor) && OutColor == Color;
        Result->SetArrayField(TEXT("value_rgba"), mcp_common::ColorToJsonArray(Color));
        Result->SetArrayField(TEXT("readback_rgba"), mcp_common::ColorToJsonArray(OutColor));
        if (!bReadback)
        {
            ReadbackError = FString::Printf(
                TEXT("set_material_parameter_collection_instance_value: the instance reads back %s for '%s', not the %s written, so the override did not land"),
                *OutColor.ToString(), *ParamName, *Color.ToString());
        }
    }
    else
    {
        // SetScalarParameterValue returns false when the parameter name doesn't
        // exist on the parent collection. Verify upfront for a clearer error.
        if (!bIsScalarParam)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Scalar parameter '%s' not found on collection '%s'"),
                                *ParamName, *CollectionPath));
        }
        Instance->SetScalarParameterValue(FName(*ParamName), Value);
        // Push the new value to the render thread so PIE / live preview picks it
        // up. Without this the uniform buffer stays at the prior value.
        Instance->UpdateRenderState(/*bRecreateUniformBuffer=*/false);

        // Read back: the getter answers the override, or the collection default when there is none, so only a value
        // equal to the one written proves the override landed (the setter writes nothing once the instance has lost
        // its world).
        float OutValue = 0.0f;
        bReadback = Instance->GetScalarParameterValue(FName(*ParamName), OutValue) && OutValue == Value;
        Result->SetNumberField(TEXT("value"), Value);
        Result->SetNumberField(TEXT("readback_value"), OutValue);
        if (!bReadback)
        {
            ReadbackError = FString::Printf(
                TEXT("set_material_parameter_collection_instance_value: the instance reads back %s for '%s', not the %s written, so the override did not land"),
                *FString::SanitizeFloat(OutValue), *ParamName, *FString::SanitizeFloat(Value));
        }
    }
    Result->SetBoolField(TEXT("readback_ok"), bReadback);
    mcp_common::AddWorldEvidence(Result, World);
    return mcp_common::MakeOutcomeReply(Result, bReadback, ReadbackError);
}

// MCP:get_material_parameter_collection_instance_info - Read the transient Material Parameter Collection Instance of one collection on a world: instance_exists, is_valid, parent_collection_path and, per parameter, current_value or current_rgba with readback_ok. The world is the editor world by default, or the Play-in-Editor world world_label selects; world_package and world_type say which one was read. Changes no value, but the engine lookup creates the instance, at the collection defaults, when the world has none, so instance_exists is true in practice; scalar_override_count and vector_override_count are only the parameter counts.
//   @param collection_path - Full /Game path of the parent MPC
//   @param world_label - Which world: empty or "editor" for the editor world (the default), even while Play-in-Editor runs; "pie" (alias "game") for the running PIE world, "server" or "client" for that PIE world, "pie:N" for PIE instance N. any other value names a live world by its object name (the editor world is checked first, so a map name selects it) or by its package name (UEDPIE_N_ and the map name for a PIE world), and a value no world has fails and lists the worlds present
//   @returns { collection_path: string, world_name: string, world_package: string, world_type: string, net_mode: string, pie_instance: int, instance_exists: bool, is_valid: bool, parent_collection_path: string, scalar_override_count: int, vector_override_count: int, scalars: [object], vectors: [object] }
TSharedPtr<FJsonObject> HandleGetMpciInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath)) { return Missing; }
    UWorld* World = nullptr;
    FString LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionAndWorld(Params, CollectionPath, World, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    // UWorld::GetParameterCollectionInstance lazily creates the instance when the world has
    // none (the instance list is private, so there is no lookup that does not create it).
    UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    mcp_common::AddWorldEvidence(Result, World);
    Result->SetBoolField(TEXT("instance_exists"), Instance != nullptr);
    Result->SetBoolField(TEXT("is_valid"), Instance ? Instance->IsCollectionValid() : false);

    if (Instance)
    {
        const UMaterialParameterCollection* Parent = Instance->GetCollection();
        Result->SetStringField(TEXT("parent_collection_path"), Parent ? Parent->GetPathName() : TEXT(""));
        Result->SetNumberField(TEXT("scalar_override_count"), Collection->ScalarParameters.Num());
        Result->SetNumberField(TEXT("vector_override_count"), Collection->VectorParameters.Num());

        // Live readback of every parameter's current effective value.
        TArray<TSharedPtr<FJsonValue>> ScalarsArr;
        for (const FCollectionScalarParameter& P : Collection->ScalarParameters)
        {
            float V = P.DefaultValue;
            const bool bOK = Instance->GetScalarParameterValue(P.ParameterName, V);
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), P.ParameterName.ToString());
            O->SetNumberField(TEXT("current_value"), V);
            O->SetBoolField(TEXT("readback_ok"), bOK);
            ScalarsArr.Add(MakeShared<FJsonValueObject>(O));
        }
        Result->SetArrayField(TEXT("scalars"), ScalarsArr);

        TArray<TSharedPtr<FJsonValue>> VectorsArr;
        for (const FCollectionVectorParameter& P : Collection->VectorParameters)
        {
            FLinearColor C = P.DefaultValue;
            const bool bOK = Instance->GetVectorParameterValue(P.ParameterName, C);
            TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("name"), P.ParameterName.ToString());
            O->SetArrayField(TEXT("current_rgba"), mcp_common::ColorToJsonArray(C));
            O->SetBoolField(TEXT("readback_ok"), bOK);
            VectorsArr.Add(MakeShared<FJsonValueObject>(O));
        }
        Result->SetArrayField(TEXT("vectors"), VectorsArr);
    }
    return mcp_common::MakeOkReply(Result);
}

// MCP:reset_material_parameter_collection_instance - Reset the Material Parameter Collection Instance (MPCI: the transient per-world values of a collection asset) of one collection on a world to the collection's default values by dropping all its scalar and vector overrides. The world is the editor world by default, or the Play-in-Editor world world_label selects; world_package and world_type say which one was reached. Runtime only: nothing is saved, no undo. A world with no instance for the collection gets a fresh one at the defaults (created=true, already_existed=false), so this is also how to give a world its instance; is_valid says whether the instance is bound to a valid collection. scalars_reset_to_default and vectors_reset_to_default count parameters now reading their default. For a Material Instance Constant asset use set_material_instance_parameter_override with all=true and override=false.
//   @param collection_path - Full /Game path of the parent MPC
//   @param world_label - Which world: empty or "editor" for the editor world (the default), even while Play-in-Editor runs; "pie" (alias "game") for the running PIE world, "server" or "client" for that PIE world, "pie:N" for PIE instance N. any other value names a live world by its object name (the editor world is checked first, so a map name selects it) or by its package name (UEDPIE_N_ and the map name for a PIE world), and a value no world has fails and lists the worlds present
//   @returns { collection_path: string, world_name: string, world_package: string, world_type: string, net_mode: string, pie_instance: int, scalar_count: int, vector_count: int, scalars_reset_to_default: int, vectors_reset_to_default: int, cleared: bool, is_valid: bool, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleClearMpciParameterOverrides(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath)) { return Missing; }
    UWorld* World = nullptr;
    FString LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionAndWorld(Params, CollectionPath, World, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    // A world without an instance for the collection gets one (at the defaults, so there is nothing more to reset).
    const bool bAlreadyExisted = World->GetParameterCollectionInstance(Collection) != nullptr;
    UMaterialParameterCollectionInstance* Instance = UEAgentMaterialCollectionImpl::FindOrAddInstance(World, Collection);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Failed to obtain a MaterialParameterCollectionInstance for collection '%s' on world '%s'"),
                            *CollectionPath, *World->GetName()));
    }

    // ForceReturnToDefaultValues wipes the per-instance ScalarParameterValues /
    // VectorParameterValues TMaps and pushes the now-cleared state to the
    // render thread via UpdateRenderState(bRecreateUniformBuffer=true). This
    // is what the editor's "Reset to Default" context-menu entry does.
    Instance->ForceReturnToDefaultValues();
    Instance->UpdateRenderState(/*bRecreateUniformBuffer=*/true);

    // Read back a couple of scalar + vector defaults so the caller has
    // confirmation that the values are now at the parent collection's
    // defaults (not stale overrides).
    float ReadbackScalar = 0.0f;
    int32 ScalarsReadback = 0;
    for (const FCollectionScalarParameter& P : Collection->ScalarParameters)
    {
        float V = 0.0f;
        if (Instance->GetScalarParameterValue(P.ParameterName, V) && FMath::IsNearlyEqual(V, P.DefaultValue))
        {
            ++ScalarsReadback;
        }
    }
    int32 VectorsReadback = 0;
    for (const FCollectionVectorParameter& P : Collection->VectorParameters)
    {
        FLinearColor C(0.0f, 0.0f, 0.0f, 1.0f);
        if (Instance->GetVectorParameterValue(P.ParameterName, C))
        {
            const FLinearColor D = P.DefaultValue;
            if (C.Equals(D, KINDA_SMALL_NUMBER)) { ++VectorsReadback; }
        }
    }

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    mcp_common::AddWorldEvidence(Result, World);
    Result->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    Result->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    Result->SetNumberField(TEXT("scalars_reset_to_default"), ScalarsReadback);
    Result->SetNumberField(TEXT("vectors_reset_to_default"), VectorsReadback);
    Result->SetBoolField(TEXT("cleared"), true);
    Result->SetBoolField(TEXT("is_valid"), Instance->IsCollectionValid());
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !bAlreadyExisted);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_collection_parameter"), &HandleAddCollectionScalarParam },
    { TEXT("reset_material_parameter_collection_instance"), &HandleClearMpciParameterOverrides },
    { TEXT("create_material_parameter_collection"), &HandleCreateMaterialParameterCollection },
    { TEXT("duplicate_material_parameter_collection"), &HandleDuplicateMpc },
    { TEXT("get_collection_info"), &HandleGetCollectionInfo },
    { TEXT("get_material_parameter_collection_instance_info"), &HandleGetMpciInfo },
    { TEXT("regenerate_collection_parameter_id"), &HandleRegenerateCollectionParamId },
    { TEXT("remove_collection_parameter"), &HandleRemoveCollectionParam },
    { TEXT("rename_collection_parameter"), &HandleRenameCollectionParam },
    { TEXT("set_collection_parameter_default"), &HandleSetCollectionParamDefault },
    { TEXT("set_material_parameter_collection_instance_value"), &HandleSetMpciScalarParameterValue },
};
} // namespace UEAgentMaterialCollectionCommands
