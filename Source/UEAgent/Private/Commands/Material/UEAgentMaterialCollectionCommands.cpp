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

    // Resolve the UWorld to operate on for MPCI commands. Default is the
    // editor world (mcp_common::GetEditorWorld()); PIE / Game
    // worlds are resolved by iterating GEngine->GetWorldContexts() and
    // matching the WorldType + an optional label/name heuristic.
    // Returns nullptr and sets OutError on failure.
    UWorld* ResolveMpciWorld(const TSharedPtr<FJsonObject>& Params, FString& OutError)
    {
        FString WorldLabel;
        Params->TryGetStringField(TEXT("world_label"), WorldLabel);
        // Empty world_label OR "editor" -> editor world.
        if (WorldLabel.IsEmpty() || WorldLabel.Equals(TEXT("editor"), ESearchCase::IgnoreCase))
        {
            if (!GEditor)
            {
                OutError = TEXT("GEditor is null (headless test harness?): cannot resolve editor world");
                return nullptr;
            }
            UWorld* World = mcp_common::GetEditorWorld();
            if (!World)
            {
                OutError = TEXT("Editor world is null");
            }
            return World;
        }
        // PIE / Game worlds: scan world contexts.
        if (!GEngine)
        {
            OutError = TEXT("GEngine is null");
            return nullptr;
        }
        for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
        {
            if (!Ctx.World()) { continue; }
            const FString CtxName = Ctx.World()->GetName();
            if (CtxName.Equals(WorldLabel, ESearchCase::IgnoreCase))
            {
                return Ctx.World();
            }
        }
        OutError = FString::Printf(TEXT("World '%s' not found among the live world contexts"), *WorldLabel);
        return nullptr;
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

// MCP:create_material_parameter_collection - Create and save a new UMaterialParameterCollection asset
//   @param name - Asset name, e.g. MPC_Global
//   @param package_path - Target content path, defaults to /Game/MaterialCollections
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

    // Idempotent: if it already exists, load and return a STABLE payload.
    if (UEditorAssetLibrary::DoesAssetExist(FullPath))
    {
        UMaterialParameterCollection* Existing = LoadCollectionAt(FullPath);
        if (Existing)
        {
            TSharedPtr<FJsonObject> ExistObj = UEAgentMaterialCollectionImpl::CreatedCollectionReply(Name, PackagePath, Existing);
            DataAuthoringUtils::AddAlreadyExistsFlags(ExistObj);
            return mcp_common::MakeOkReply(ExistObj);
        }
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset exists at %s but is not a MaterialParameterCollection"), *FullPath));
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

// MCP:add_collection_scalar_param - Append a scalar parameter to a Material Parameter Collection
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the new scalar parameter
//   @param default_value - Optional float default, default 0
//   @returns { collection_path: string, param_name: string, param_type: string, param_added: bool, already_existed: bool, scalar_count_before: int, scalar_count: int, default_value: number, id: string, saved: bool }
TSharedPtr<FJsonObject> HandleAddCollectionScalarParam(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath, ParamName, LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionForParam(Params, CollectionPath, ParamName, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
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

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_collection_scalar_param"), SaveError);
}

// MCP:add_collection_vector_param - Append a vector RGBA parameter to a Material Parameter Collection
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the new vector parameter
//   @param default_value - Optional [r, g, b, a] default color, default [0,0,0,1]
//   @returns { collection_path: string, param_name: string, param_type: string, param_added: bool, already_existed: bool, vector_count_before: int, vector_count: int, default: object, default_rgba: [number], id: string, saved: bool }
TSharedPtr<FJsonObject> HandleAddCollectionVectorParam(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath, ParamName, LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionForParam(Params, CollectionPath, ParamName, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }

    // Default value: optional [r, g, b, a] array under "default_value" (default (0,0,0,1)).
    FLinearColor DefaultColor(0.0f, 0.0f, 0.0f, 1.0f);
    TryGetLinearColorArray(Params, TEXT("default_value"), DefaultColor);

    const FName ParamFName(*ParamName);
    const int32 CountBefore = Collection->VectorParameters.Num();

    const int32 ExistingIndex = UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->VectorParameters, ParamFName);
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

    TSharedPtr<FJsonObject> ResultObj = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, TEXT("vector"));
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

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("add_collection_vector_param"), SaveError);
}

// MCP:set_collection_param_default - Set the default value of an existing collection parameter
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the existing parameter to update
//   @param scalar_value - Optional new float value for a scalar parameter
//   @param vector - Optional new [r, g, b, a] color for a vector parameter
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

    return mcp_common::MakeSavedReply(ResultObj, bSaved, TEXT("set_collection_param_default"), SaveError);
}

// MCP:get_collection_info - Read all parameters of a Material Parameter Collection
//   @param collection_path - Full /Game path to the MPC asset
//   @returns { collection_path: string, collection_name: string, collection_class: string, scalar_params: [object], vector_params: [object], scalar_count: int, vector_count: int, total_count: int }
TSharedPtr<FJsonObject> HandleGetCollectionInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

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

    TArray<TSharedPtr<FJsonValue>> ScalarArr;
    for (const FCollectionScalarParameter& P : Collection->ScalarParameters)
    {
        ScalarArr.Add(MakeShared<FJsonValueObject>(ScalarParamToJson(P)));
    }
    ResultObj->SetArrayField(TEXT("scalar_params"), ScalarArr);

    TArray<TSharedPtr<FJsonValue>> VectorArr;
    for (const FCollectionVectorParameter& P : Collection->VectorParameters)
    {
        VectorArr.Add(MakeShared<FJsonValueObject>(VectorParamToJson(P)));
    }
    ResultObj->SetArrayField(TEXT("vector_params"), VectorArr);

    ResultObj->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    ResultObj->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    ResultObj->SetNumberField(TEXT("total_count"),
        Collection->ScalarParameters.Num() + Collection->VectorParameters.Num());

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

// MCP:remove_collection_param - Remove a parameter from a Material Parameter Collection by name
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the parameter to remove
//   @param param_type - scalar or vector, default scalar
//   @returns { collection_path: string, param_name: string, param_type: string, removed: bool, scalar_count: int, vector_count: int, saved: bool }
TSharedPtr<FJsonObject> HandleRemoveCollectionParam(const TSharedPtr<FJsonObject>& Params)
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

    const bool bRemoved = UEAgentMaterialCollectionImpl::EditNamedParameter(Collection, Type, ParamFName, [](auto& Parameters, int32 Index)
    {
        Parameters.RemoveAt(Index);
    });
    if (!bRemoved)
    {
        return UEAgentMaterialCollectionImpl::ParamNotFound(ParamName, Type);
    }

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    TSharedPtr<FJsonObject> Result = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, Type);
    Result->SetBoolField(TEXT("removed"), true);
    Result->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    Result->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("remove_collection_param"), SaveError);
}

// MCP:rename_collection_param - Rename a parameter in a Material Parameter Collection
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Current parameter name
//   @param new_name - New parameter name, must not collide with another
//   @param param_type - scalar or vector, default scalar
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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("rename_collection_param"), SaveError);
}

// MCP:clear_collection_params - Clear all parameters of a given type from a Material Parameter Collection
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_type - scalar, vector, or all, default all
//   @returns { collection_path: string, param_type: string, scalar_count_before: int, vector_count_before: int, scalar_count: int, vector_count: int, cleared: bool, saved: bool }
TSharedPtr<FJsonObject> HandleClearCollectionParams(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // param_type: "scalar" | "vector" | "all" (default: all)
    FString Type;
    Params->TryGetStringField(TEXT("param_type"), Type);
    Type = Type.ToLower();
    if (Type.IsEmpty() || Type == TEXT("all"))
    {
        Type = TEXT("all");
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

    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Collection->GetOutermost(), Collection, SaveError);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), CollectionPath);
    Result->SetStringField(TEXT("param_type"), Type);
    Result->SetNumberField(TEXT("scalar_count_before"), ScalarsBefore);
    Result->SetNumberField(TEXT("vector_count_before"), VectorsBefore);
    Result->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    Result->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    Result->SetBoolField(TEXT("cleared"), true);
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("clear_collection_params"), SaveError);
}

// MCP:regenerate_collection_param_id - Regenerate the FGuid of a collection parameter
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the parameter to regenerate the id for
//   @param param_type - scalar or vector, default scalar
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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("regenerate_collection_param_id"), SaveError);
}

// MCP:get_collection_param_info - Read detailed info about one parameter in a collection
//   @param collection_path - Full /Game path to the MPC asset
//   @param param_name - Name of the parameter to query
//   @param param_type - scalar or vector, default scalar
//   @returns { collection_path: string, param_name: string, param_type: string, id: string, default_value: number, default: object, index: int, collection_class: string }
TSharedPtr<FJsonObject> HandleGetCollectionParamInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath, ParamName, LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionForParam(Params, CollectionPath, ParamName, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    const FString Type = ResolveParamType(Params);
    const FName ParamFName(*ParamName);

    TSharedPtr<FJsonObject> Result = UEAgentMaterialCollectionImpl::ParamReply(CollectionPath, ParamName, Type);

    if (Type == TEXT("scalar"))
    {
        const FCollectionScalarParameter* Found = Collection->GetScalarParameterByName(ParamFName);
        if (!Found)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Scalar param '%s' not found"), *ParamName));
        }
        Result->SetStringField(TEXT("id"), Found->Id.ToString());
        Result->SetNumberField(TEXT("default_value"), Found->DefaultValue);
        Result->SetNumberField(TEXT("index"),
            UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->ScalarParameters, ParamFName));
        Result->SetStringField(TEXT("collection_class"), Collection->GetClass()->GetName());
    }
    else
    {
        const FCollectionVectorParameter* Found = Collection->GetVectorParameterByName(ParamFName);
        if (!Found)
        {
            return mcp_common::MakeErrorReply(
                FString::Printf(TEXT("Vector param '%s' not found"), *ParamName));
        }
        Result->SetStringField(TEXT("id"), Found->Id.ToString());
        Result->SetObjectField(TEXT("default"), mcp_common::LinearColorToJsonObject(Found->DefaultValue));
        Result->SetNumberField(TEXT("index"),
            UEAgentMaterialCollectionImpl::FindParameterIndex(Collection->VectorParameters, ParamFName));
        Result->SetStringField(TEXT("collection_class"), Collection->GetClass()->GetName());
    }

    return mcp_common::MakeOkReply(Result);
}

// MCP:duplicate_mpc - Duplicate a Material Parameter Collection to a new path
//   @param source_path - Full /Game path of the source MPC
//   @param new_name - Name for the duplicated MPC
//   @param package_path - Target package path, defaults to source's folder
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
    return mcp_common::MakeSavedReply(Result, bSaved, TEXT("duplicate_mpc"));
}

// ========================================================================== //
// Material Parameter Collection Instance (MPCI)
//
// MPCIs are TRANSIENT per-world runtime objects (verified
// MaterialParameterCollectionInstance.h: "Instances of this class
// are always transient."). The canonical public path to obtain / create one
// is UWorld::AddParameterCollectionInstance(MPC, bUpdateScene) +
// UWorld::GetParameterCollectionInstance(MPC). Every MPCI command routes through a
// world (editor by default; PIE/Game when world_label is specified).
//
//   create_material_parameter_collection_instance
//                                          get-or-create a transient MPCI
//                                          on the target world. Returns
//                                          already_existed: true when one
//                                          was already attached.
//   set_mpci_scalar_parameter_value        override one scalar on the
//                                          per-world instance.
//   set_mpci_vector_parameter_value        override one vector on the
//                                          per-world instance (R/G/B/A or
//                                          rgba=[...]).
//   get_mpci_info                          readback: parent collection
//                                          path, override count, current
//                                          override values for all params.
//   clear_mpci_parameter_overrides         ForceReturnToDefaultValues +
//                                          UpdateRenderState on the live
//                                          instance.
//   set_mpci_parent_collection             re-bind the instance to a
//                                          different collection (same path
//                                          via World->AddParameterCollectionInstance).
//
// All operations are direct on the live UMaterialParameterCollectionInstance
// object — no asset file I/O (MPCI instances are runtime/transient).
// ========================================================================== //

// MCP:create_material_parameter_collection_instance - Get or create a transient MPCI on the target world
//   @param collection_path - Full /Game path of the parent MPC
//   @param world_label - Optional world context label, default editor
//   @returns { collection_path: string, world_name: string, is_valid: bool, already_existed: bool, created: bool }
TSharedPtr<FJsonObject> HandleCreateMaterialParameterCollectionInstance(const TSharedPtr<FJsonObject>& Params)
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

    // AddParameterCollectionInstance overwrites any prior instance with the
    // same parent (the engine docs describe it as "Adds a new instance ...
    // or overwrites an existing instance if there is one"). Detect the
    // already-existed case via GetParameterCollectionInstance BEFORE the call
    // so we can return already_existed: true.
    UMaterialParameterCollectionInstance* Existing = World->GetParameterCollectionInstance(Collection);
    const bool bAlreadyExisted = (Existing != nullptr);

    // UWorld::AddParameterCollectionInstance(MPC*, bUpdateScene) — bUpdateScene
    // = true so the render state picks up the new instance immediately (it's a
    // cheap op; this is what the engine editor does on level load).
    World->AddParameterCollectionInstance(Collection, /*bUpdateScene=*/true);
    UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    Result->SetStringField(TEXT("world_name"), World->GetName());
    Result->SetBoolField(TEXT("is_valid"), Instance != nullptr);
    Result->SetBoolField(TEXT("already_existed"), bAlreadyExisted);
    Result->SetBoolField(TEXT("created"), !bAlreadyExisted && Instance != nullptr);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_mpci_scalar_parameter_value - Override a scalar parameter value on the world's MPCI
//   @param collection_path - Full /Game path of the parent MPC
//   @param parameter_name - Name of the scalar parameter to override
//   @param value - New float value for the parameter
//   @param world_label - Optional world context label, default editor
//   @returns { collection_path: string, parameter_name: string, value: number, readback_value: number, readback_ok: bool, world_name: string }
TSharedPtr<FJsonObject> HandleSetMpciScalarParameterValue(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath)) { return Missing; }
    FString ParamName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName)) { return Missing; }
    if (!Params->HasField(TEXT("value")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'value' parameter"));
    }
    const float Value = static_cast<float>(Params->GetNumberField(TEXT("value")));

    UWorld* World = nullptr;
    FString LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionAndWorld(Params, CollectionPath, World, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    UMaterialParameterCollectionInstance* Instance = UEAgentMaterialCollectionImpl::FindOrAddInstance(World, Collection);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to obtain MaterialParameterCollectionInstance from world"));
    }

    // SetScalarParameterValue returns false when the parameter name doesn't
    // exist on the parent collection. Verify upfront for a clearer error.
    if (!Collection->GetScalarParameterByName(*ParamName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Scalar parameter '%s' not found on collection '%s'"),
                            *ParamName, *CollectionPath));
    }

    Instance->SetScalarParameterValue(FName(*ParamName), Value);
    // Push the new value to the render thread so PIE / live preview picks it
    // up. Without this the uniform buffer stays at the prior value.
    Instance->UpdateRenderState(/*bRecreateUniformBuffer=*/false);

    // Read back so the caller has confirmation the override landed.
    float OutValue = 0.0f;
    const bool bReadback = Instance->GetScalarParameterValue(FName(*ParamName), OutValue);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    Result->SetStringField(TEXT("parameter_name"), ParamName);
    Result->SetNumberField(TEXT("value"), Value);
    Result->SetNumberField(TEXT("readback_value"), OutValue);
    Result->SetBoolField(TEXT("readback_ok"), bReadback);
    Result->SetStringField(TEXT("world_name"), World->GetName());
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_mpci_vector_parameter_value - Override a vector parameter value on the world's MPCI
//   @param collection_path - Full /Game path of the parent MPC
//   @param parameter_name - Name of the vector parameter to override
//   @param rgba - Optional [r, g, b, a] color value
//   @param r - Optional red channel, alternative to rgba
//   @param g - Optional green channel
//   @param b - Optional blue channel
//   @param a - Optional alpha channel
//   @param world_label - Optional world context label, default editor
//   @returns { collection_path: string, parameter_name: string, value_rgba: [number], readback_rgba: [number], readback_ok: bool, world_name: string }
TSharedPtr<FJsonObject> HandleSetMpciVectorParameterValue(const TSharedPtr<FJsonObject>& Params)
{
    FString CollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), CollectionPath)) { return Missing; }
    FString ParamName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("parameter_name"), ParamName)) { return Missing; }

    // Accept either a flat rgba=[] array OR individual r/g/b/a number fields.
    FLinearColor Value(0.0f, 0.0f, 0.0f, 1.0f);
    bool bHasAny = false;
    const TArray<TSharedPtr<FJsonValue>>* RgbaArr = nullptr;
    if (Params->TryGetArrayField(TEXT("rgba"), RgbaArr) && RgbaArr && RgbaArr->Num() >= 3)
    {
        Value.R = static_cast<float>((*RgbaArr)[0]->AsNumber());
        Value.G = static_cast<float>((*RgbaArr)[1]->AsNumber());
        Value.B = static_cast<float>((*RgbaArr)[2]->AsNumber());
        if (RgbaArr->Num() >= 4) { Value.A = static_cast<float>((*RgbaArr)[3]->AsNumber()); }
        bHasAny = true;
    }
    double NumBuf = 0.0;
    if (Params->TryGetNumberField(TEXT("r"), NumBuf)) { Value.R = static_cast<float>(NumBuf); bHasAny = true; }
    if (Params->TryGetNumberField(TEXT("g"), NumBuf)) { Value.G = static_cast<float>(NumBuf); bHasAny = true; }
    if (Params->TryGetNumberField(TEXT("b"), NumBuf)) { Value.B = static_cast<float>(NumBuf); bHasAny = true; }
    if (Params->TryGetNumberField(TEXT("a"), NumBuf)) { Value.A = static_cast<float>(NumBuf); bHasAny = true; }
    if (!bHasAny)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'rgba' (array of 3-4 numbers) OR 'r'/'g'/'b'/'a' parameters"));
    }

    UWorld* World = nullptr;
    FString LoadError;
    UMaterialParameterCollection* Collection = UEAgentMaterialCollectionImpl::LoadCollectionAndWorld(Params, CollectionPath, World, LoadError);
    if (!Collection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    UMaterialParameterCollectionInstance* Instance = UEAgentMaterialCollectionImpl::FindOrAddInstance(World, Collection);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            TEXT("Failed to obtain MaterialParameterCollectionInstance from world"));
    }
    if (!Collection->GetVectorParameterByName(*ParamName))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Vector parameter '%s' not found on collection '%s'"),
                            *ParamName, *CollectionPath));
    }

    Instance->SetVectorParameterValue(FName(*ParamName), Value);
    Instance->UpdateRenderState(/*bRecreateUniformBuffer=*/false);

    FLinearColor OutColor(0.0f, 0.0f, 0.0f, 1.0f);
    const bool bReadback = Instance->GetVectorParameterValue(FName(*ParamName), OutColor);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    Result->SetStringField(TEXT("parameter_name"), ParamName);
    Result->SetArrayField(TEXT("value_rgba"), mcp_common::ColorToJsonArray(Value));
    Result->SetArrayField(TEXT("readback_rgba"), mcp_common::ColorToJsonArray(OutColor));
    Result->SetBoolField(TEXT("readback_ok"), bReadback);
    Result->SetStringField(TEXT("world_name"), World->GetName());
    return mcp_common::MakeOkReply(Result);
}

// MCP:get_mpci_info - Read MPCI state, parent collection, and live parameter values
//   @param collection_path - Full /Game path of the parent MPC
//   @param world_label - Optional world context label, default editor
//   @returns { collection_path: string, world_name: string, instance_exists: bool, is_valid: bool, parent_collection_path: string, scalar_override_count: int, vector_override_count: int, scalars: [object], vectors: [object] }
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
    UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), Collection->GetPathName());
    Result->SetStringField(TEXT("world_name"), World->GetName());
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

// MCP:clear_mpci_parameter_overrides - Force the world's MPCI back to the parent's default values
//   @param collection_path - Full /Game path of the parent MPC
//   @param world_label - Optional world context label, default editor
//   @returns { collection_path: string, world_name: string, scalar_count: int, vector_count: int, scalars_reset_to_default: int, vectors_reset_to_default: int, cleared: bool }
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
    UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(Collection);
    if (!Instance)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("No live MPCI for collection '%s' on world '%s' (nothing to clear)"),
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
    Result->SetStringField(TEXT("world_name"), World->GetName());
    Result->SetNumberField(TEXT("scalar_count"), Collection->ScalarParameters.Num());
    Result->SetNumberField(TEXT("vector_count"), Collection->VectorParameters.Num());
    Result->SetNumberField(TEXT("scalars_reset_to_default"), ScalarsReadback);
    Result->SetNumberField(TEXT("vectors_reset_to_default"), VectorsReadback);
    Result->SetBoolField(TEXT("cleared"), true);
    return mcp_common::MakeOkReply(Result);
}

// MCP:set_mpci_parent_collection - Rebind the world's MPCI to a different parent collection
//   @param collection_path - Full /Game path of the new parent MPC
//   @param world_label - Optional world context label, default editor
//   @returns { collection_path: string, previous_parent_path: string, world_name: string, is_valid: bool, already_attached: bool, attached: bool }
TSharedPtr<FJsonObject> HandleSetMpciParentCollection(const TSharedPtr<FJsonObject>& Params)
{
    // "Set parent" on an MPCI is just "add the (possibly different) collection
    // to this world". World->AddParameterCollectionInstance overwrites the
    // existing instance with the new parent and seeds it from the new
    // collection's defaults.
    FString NewCollectionPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("collection_path"), NewCollectionPath)) { return Missing; }
    UWorld* World = nullptr;
    FString LoadError;
    UMaterialParameterCollection* NewCollection = UEAgentMaterialCollectionImpl::LoadCollectionAndWorld(Params, NewCollectionPath, World, LoadError);
    if (!NewCollection)
    {
        return mcp_common::MakeErrorReply(LoadError);
    }
    // Detect already-attached to short-circuit the rewrite (idempotent).
    UMaterialParameterCollectionInstance* Existing = World->GetParameterCollectionInstance(NewCollection);
    const bool bAlreadyAttached = (Existing != nullptr);
    const FString PreviousParentPath = Existing && Existing->GetCollection()
        ? Existing->GetCollection()->GetPathName()
        : TEXT("");

    World->AddParameterCollectionInstance(NewCollection, /*bUpdateScene=*/true);
    UMaterialParameterCollectionInstance* Instance = World->GetParameterCollectionInstance(NewCollection);

    TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("collection_path"), NewCollection->GetPathName());
    Result->SetStringField(TEXT("previous_parent_path"), PreviousParentPath);
    Result->SetStringField(TEXT("world_name"), World->GetName());
    Result->SetBoolField(TEXT("is_valid"), Instance != nullptr);
    Result->SetBoolField(TEXT("already_attached"), bAlreadyAttached);
    Result->SetBoolField(TEXT("attached"), Instance != nullptr);
    return mcp_common::MakeOkReply(Result);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("add_collection_scalar_param"), &HandleAddCollectionScalarParam },
    { TEXT("add_collection_vector_param"), &HandleAddCollectionVectorParam },
    { TEXT("clear_collection_params"), &HandleClearCollectionParams },
    { TEXT("clear_mpci_parameter_overrides"), &HandleClearMpciParameterOverrides },
    { TEXT("create_material_parameter_collection"), &HandleCreateMaterialParameterCollection },
    { TEXT("create_material_parameter_collection_instance"), &HandleCreateMaterialParameterCollectionInstance },
    { TEXT("duplicate_mpc"), &HandleDuplicateMpc },
    { TEXT("get_collection_info"), &HandleGetCollectionInfo },
    { TEXT("get_collection_param_info"), &HandleGetCollectionParamInfo },
    { TEXT("get_mpci_info"), &HandleGetMpciInfo },
    { TEXT("regenerate_collection_param_id"), &HandleRegenerateCollectionParamId },
    { TEXT("remove_collection_param"), &HandleRemoveCollectionParam },
    { TEXT("rename_collection_param"), &HandleRenameCollectionParam },
    { TEXT("set_collection_param_default"), &HandleSetCollectionParamDefault },
    { TEXT("set_mpci_parent_collection"), &HandleSetMpciParentCollection },
    { TEXT("set_mpci_scalar_parameter_value"), &HandleSetMpciScalarParameterValue },
    { TEXT("set_mpci_vector_parameter_value"), &HandleSetMpciVectorParameterValue },
};
} // namespace UEAgentMaterialCollectionCommands
