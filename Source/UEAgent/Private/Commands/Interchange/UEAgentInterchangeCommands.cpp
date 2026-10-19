#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Interchange framework.
#include "InterchangeManager.h"                          // UInterchangeManager (INTERCHANGEENGINE_API)
#include "InterchangeAssetImportData.h"                  // UInterchangeAssetImportData (MinimalAPI inline + INTERCHANGEENGINE_API)
#include "InterchangeProjectSettings.h"                  // UInterchangeProjectSettings + FInterchangeProjectSettingsUtils (INTERCHANGEENGINE_API)
#include "InterchangeTranslatorBase.h"                   // UInterchangeTranslatorBase + EInterchangeTranslatorType (INTERCHANGECORE_API)
#include "InterchangeFactoryBase.h"                      // UInterchangeFactoryBase::GetFactoryClass (INTERCHANGECORE_API)
#include "InterchangePipelineBase.h"                    // UInterchangePipelineBase (abstract, INTERCHANGECORE_API)
#include "InterchangeGenericAssetsPipeline.h"           // UInterchangeGenericAssetsPipeline + texture/mesh/material/animation/audio/scenes/groom/glTF/sparse/materialX variants (concrete MinimalAPI classes, INTERCHANGEPIPELINES_API)
#include "Nodes/InterchangeBaseNodeContainer.h"          // UInterchangeBaseNodeContainer (INTERCHANGECORE_API)
#include "Nodes/InterchangeFactoryBaseNode.h"            // UInterchangeFactoryBaseNode (INTERCHANGECORE_API)
#include "Nodes/InterchangeBaseNode.h"                   // UInterchangeBaseNode::GetUniqueID / GetDisplayLabel

#include "AssetToolsModule.h"                            // IAssetTools (for CreateAsset)
#include "AssetRegistry/AssetRegistryModule.h"           // FAssetRegistryModule (MarkPackageDirty)
#include "JsonObjectConverter.h"                        // FJsonObjectConverter::JsonValueToUProperty / UPropertyToJsonValue
#include "UObject/SavePackage.h"                         // UPackage::Save (SavePackage)

#include "UObject/UObjectGlobals.h"                      // LoadObject<UObject>
#include "UObject/Package.h"                             // UPackage (for MarkPackageDirty)
#include "UObject/UnrealType.h"                          // FSetProperty / FScriptSetHelper (registered translators)
#include "HAL/FileManager.h"                            // IFileManager::IsReadOnly (DefaultEngine.ini)
#include "Misc/Paths.h"                                 // FPaths::ConvertRelativePathToFull
#include "Editor.h"                                      // GEditor (for editor subsystems; unused here, kept for symmetry)

/**
 * Interchange framework commands: module status and supported formats; the import data, pipelines and
 * node container of an imported asset; project settings and translator/factory registration; pipeline
 * asset authoring; and headless imports through UInterchangeManager.
 *
 * Core engine APIs (UE 5.8, Engine/Source/Runtime/Interchange):
 *
 *   - UInterchangeManager::GetInterchangeManager() (static)
 *       InterchangeManager.h — returns the singleton UInterchangeManager&.
 *       (UInterchangeManager::Get() does NOT exist in 5.8.)
 *   - UInterchangeManager::IsInterchangeImportEnabled() (static)
 *       InterchangeManager.h — backs the CVar
 *       "Interchange.FeatureFlags.Import.Enable" (the plugin sets it true).
 *   - UInterchangeManager::GetSupportedFormats(EInterchangeTranslatorType) const
 *       InterchangeManager.h — returns the union of file extensions
 *       reported by every registered translator for the requested type
 *       (Assets / Actors / Scenes). The enum is defined in
 *       Core/Public/InterchangeTranslatorBase.h
 *       (EInterchangeTranslatorType = uint8: Invalid=0, Assets=2, Actors=4,
 *       Scenes=Assets|Actors=6).
 *   - UInterchangeManager::GetAssetImportData(UObject* Asset) const
 *       InterchangeManager.h — returns the UInterchangeAssetImportData
 *       sub-object of an imported asset, or nullptr if the asset has no
 *       Interchange import data (e.g. legacy factory imports).
 *   - UInterchangeAssetImportData
 *       InterchangeAssetImportData.h. UCLASS(MinimalAPI). Signatures:
 *         * ScriptExtractFilenames()  — UFUNCTION inline (WITH_EDITORONLY_DATA).
 *         * ScriptExtractDisplayLabels() — UFUNCTION inline.
 *         * GetNodeContainer() -> UInterchangeBaseNodeContainer*
 *         * GetPipelines() -> TArray<UObject*>
 *         * GetNumberOfPipelines() -> int32
 *         * SetPipelines(const TArray<UObject*>&)
 *       All INTERCHANGEENGINE_API except the two Script* helpers which are
 *       inline UFUNCTIONs in a MinimalAPI class — still linkable from any
 *       translation unit that includes the header.
 *   - UInterchangeBaseNodeContainer
 *       Core/Public/Nodes/InterchangeBaseNodeContainer.h.
 *         * GetRoots(TArray<FString>&)            — INTERCHANGECORE_API
 *         * GetNode(UID) -> const UInterchangeBaseNode*  — INTERCHANGECORE_API
 *         * GetFactoryNode(UID) -> UInterchangeFactoryBaseNode*
 *         * IterateNodesOfType<T>(TFunctionRef<void(const FString&, T*)>)
 *           — template, header-only.
 *   - UInterchangeFactoryBaseNode : UInterchangeBaseNode
 *       Core/Public/Nodes/InterchangeFactoryBaseNode.h.
 *         * GetObjectClass() -> UClass*     — INTERCHANGECORE_API
 *         * GetUniqueID(), GetDisplayLabel() — inherited from base node
 *           (InterchangeBaseNode.h), INTERCHANGECORE_API.
 *
 * NOTE on translator-count reporting:
 *   UInterchangeManager's `RegisteredTranslatorsClass` is a PRIVATE
 *   TSet<TObjectPtr<const UClass>> (InterchangeManager.h). There is no
 *   public Num() getter in 5.8. We therefore derive translator availability
 *   indirectly: enabled iff `IsInterchangeImportEnabled() && Num supported
 *   formats >= 1`. This is honest and gives the same operational signal
 *   (plugin loaded + at least one translator registered).
 */
namespace UEAgentInterchangeCommands
{

// MCP-DOMAIN: Interchange

namespace
{
    // Resolve an EInterchangeTranslatorType from a JSON string.
    // EInterchangeTranslatorType (InterchangeTranslatorBase.h):
    //   Invalid = 0, Assets = 2, Actors = 4, Scenes = Assets | Actors = 6.
    // The 5.8 enum does NOT have a separate "AssetsAndScene" or "Scene" variant;
    // scenes = Assets|Actors. We accept the spec's three aliases and map them
    // to the canonical values.
    EInterchangeTranslatorType ParseTranslatorType(const FString& InType)
    {
        const FString T = InType.ToLower();
        if (T == TEXT("actors"))         { return EInterchangeTranslatorType::Actors; }
        if (T == TEXT("scenes"))         { return EInterchangeTranslatorType::Scenes; }
        if (T == TEXT("assetsandscene")) { return EInterchangeTranslatorType::Scenes; }
        // Default (and "assets").
        return EInterchangeTranslatorType::Assets;
    }

    // Stringify an EInterchangeTranslatorType the same way the spec lists the
    // expected aliases ("Assets" / "AssetsAndScene" / "Actors" / "Scene" / "Invalid").
    FString TranslatorTypeToString(EInterchangeTranslatorType InType)
    {
        switch (InType)
        {
            case EInterchangeTranslatorType::Invalid: return TEXT("Invalid");
            case EInterchangeTranslatorType::Assets:  return TEXT("Assets");
            case EInterchangeTranslatorType::Actors:  return TEXT("Actors");
            case EInterchangeTranslatorType::Scenes:  return TEXT("Scene");
        }
        return TEXT("Assets");
    }

    // Resolve an asset object path to a UObject*. Returns nullptr + fills
    // OutError on failure. Accepts either the package path ("/Game/Foo/Foo")
    // or the fully-qualified object path ("/Game/Foo/Foo.Foo").
    UObject* ResolveAssetByPath(const FString& InPath, FString& OutError)
    {
        if (InPath.IsEmpty())
        {
            OutError = TEXT("Missing required 'asset_path' parameter");
            return nullptr;
        }
        UObject* Asset = LoadObject<UObject>(nullptr, *InPath);
        if (!Asset)
        {
            OutError = FString::Printf(TEXT("Asset not found at '%s'"), *InPath);
            return nullptr;
        }
        return Asset;
    }

    // ResolveAssetByPath, then the asset's UInterchangeAssetImportData (null for a legacy import): null once
    // OutAsset holds the asset, otherwise the error reply.
    TSharedPtr<FJsonObject> Interchange_ResolveImportData(const FString& AssetPath, UObject*& OutAsset,
        UInterchangeAssetImportData*& OutImportData)
    {
        FString ResolveError;
        OutAsset = ResolveAssetByPath(AssetPath, ResolveError);
        if (!OutAsset)
        {
            return mcp_common::MakeErrorReply(ResolveError);
        }
        OutImportData = UInterchangeManager::GetInterchangeManager().GetAssetImportData(OutAsset);
        return nullptr;
    }
}

// Module status and formats, import data with its pipelines and node container

// ---------------------------------------------------------------------------
// get_interchange_supported_formats
// Returns the file extensions supported by translators of the requested type.
// ---------------------------------------------------------------------------
// MCP:get_interchange_supported_formats - Report Interchange framework availability and list the file extensions supported by registered Interchange translators of a given type.  Calls UInterchangeManager::GetSupportedFormats(EInterchangeTranslatorType) (InterchangeManager.h, INTERCHANGEENGINE_API) and IsInterchangeImportEnabled(). The 5.8 enum has Invalid / Assets / Actors / Scenes (Scenes = Assets|Actors). Accepts aliases "Assets", "Actors", "Scenes", and "AssetsAndScene" (mapped to Scenes). The registered translator set is private in 5.8, so availability is reported as enabled (= import enabled AND at least one supported asset format) with the per-type format counts.
//   @param translator_type [string] - Translator type: "Assets" [default], "Actors", "Scenes", or the spec alias "AssetsAndScene"
//   @returns { translator_type: string, format_count: number, formats: [string], enabled: bool, import_enabled: bool, asset_format_count: number, actor_format_count: number, scene_format_count: number }
//   @hints read-only
TSharedPtr<FJsonObject> HandleInterchangeGetSupportedFormats(const TSharedPtr<FJsonObject>& Params)
{
    FString TypeStr = TEXT("Assets");
    Params->TryGetStringField(TEXT("translator_type"), TypeStr);

    const EInterchangeTranslatorType Type = ParseTranslatorType(TypeStr);
    UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
    const TArray<FString> Formats = Manager.GetSupportedFormats(Type);

    // Framework availability: the import switch and the per-type format counts.
    const bool bImportEnabled = UInterchangeManager::IsInterchangeImportEnabled();
    const int32 AssetFormatCount = Manager.GetSupportedFormats(EInterchangeTranslatorType::Assets).Num();
    const int32 ActorFormatCount = Manager.GetSupportedFormats(EInterchangeTranslatorType::Actors).Num();
    const int32 SceneFormatCount = Manager.GetSupportedFormats(EInterchangeTranslatorType::Scenes).Num();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("translator_type"), TranslatorTypeToString(Type));
    Data->SetNumberField(TEXT("format_count"), Formats.Num());
    Data->SetArrayField(TEXT("formats"), mcp_common::StringsToJson(Formats));
    Data->SetBoolField(TEXT("enabled"), bImportEnabled && AssetFormatCount > 0);
    Data->SetBoolField(TEXT("import_enabled"), bImportEnabled);
    Data->SetNumberField(TEXT("asset_format_count"), AssetFormatCount);
    Data->SetNumberField(TEXT("actor_format_count"), ActorFormatCount);
    Data->SetNumberField(TEXT("scene_format_count"), SceneFormatCount);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// get_interchange_asset_import_data
// Reads UInterchangeAssetImportData from an asset (or reports absent honestly
// for legacy assets that pre-date Interchange).
// ---------------------------------------------------------------------------
// MCP:get_interchange_asset_import_data - Read the Interchange import data (UInterchangeAssetImportData) of an asset loaded by package or object path: its source_filenames, display_labels, the pipeline objects stored on it (class, name, object path of each) and whether it holds a node container; with include_node_container, also that container's root_uids and every factory node (uid, display_label and the class of the object that factory produces) -- what an Interchange import produced. An asset imported through the legacy factory path, or never imported, has none: that answers present=false with zeros and empty lists, a normal state and not an error; only a path that does not load is an error. Read-only: it writes nothing and needs no PIE. The legacy UAssetImportData of a factory import (source file and timestamp): get_asset_import_data; replace the pipeline stack: set_interchange_pipelines; reimport from this data: reimport_interchange_asset.
//   @param asset_path [string!] - Asset path (e.g. "/Game/Foo/Foo") or fully-qualified object path ("/Game/Foo/Foo.Foo")
//   @param include_node_container - Optional bool, default false: add root_uids, factory_node_count and factory_nodes from the import data's node container (empty when there is none)
//   @returns { asset_path: string, present: bool, pipelines_count: number, pipelines: [{ class: string, name: string, path: string }], has_node_container: bool, source_filenames: [string], display_labels: [string], root_uids?: [string], factory_node_count?: number, factory_nodes?: [{ uid: string, class: string, display_label: string }] }
TSharedPtr<FJsonObject> HandleInterchangeGetAssetImportData(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }
    bool bIncludeNodeContainer = false;
    Params->TryGetBoolField(TEXT("include_node_container"), bIncludeNodeContainer);

    UObject* Asset = nullptr;
    UInterchangeAssetImportData* ID = nullptr;
    if (TSharedPtr<FJsonObject> ResolveError = Interchange_ResolveImportData(AssetPath, Asset, ID)) { return ResolveError; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);

    // The node-container listing, empty when there is no import data or no container.
    TArray<TSharedPtr<FJsonValue>> RootArr;
    TArray<TSharedPtr<FJsonValue>> FactoryArr;
    auto AddNodeContainer = [&Data, &RootArr, &FactoryArr]()
    {
        Data->SetArrayField(TEXT("root_uids"), RootArr);
        Data->SetNumberField(TEXT("factory_node_count"), FactoryArr.Num());
        Data->SetArrayField(TEXT("factory_nodes"), FactoryArr);
    };

    if (!ID)
    {
        // Honest absence — legacy imports or non-imported assets. Not an error.
        Data->SetBoolField(TEXT("present"), false);
        Data->SetNumberField(TEXT("pipelines_count"), 0);
        Data->SetArrayField(TEXT("pipelines"), TArray<TSharedPtr<FJsonValue>>());
        Data->SetBoolField(TEXT("has_node_container"), false);
        TArray<TSharedPtr<FJsonValue>> EmptyArr;
        Data->SetArrayField(TEXT("source_filenames"), EmptyArr);
        Data->SetArrayField(TEXT("display_labels"), EmptyArr);
        if (bIncludeNodeContainer)
        {
            AddNodeContainer();
        }
        return mcp_common::MakeOkReply(Data);
    }

    const TArray<TSharedPtr<FJsonValue>> FilenamesArr = mcp_common::StringsToJson(ID->ScriptExtractFilenames());
    const TArray<TSharedPtr<FJsonValue>> DisplayLabelsArr = mcp_common::StringsToJson(ID->ScriptExtractDisplayLabels());

    // The pipeline objects stored on the import data.
    TArray<TSharedPtr<FJsonValue>> PipelineArr;
    for (UObject* P : ID->GetPipelines())
    {
        if (!P) { continue; }
        TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
        PObj->SetStringField(TEXT("class"), P->GetClass() ? P->GetClass()->GetName() : FString());
        PObj->SetStringField(TEXT("name"), P->GetName());
        PObj->SetStringField(TEXT("path"), P->GetPathName());
        PipelineArr.Add(MakeShared<FJsonValueObject>(PObj));
    }

    UInterchangeBaseNodeContainer* NC = ID->GetNodeContainer();
    Data->SetBoolField(TEXT("present"), true);
    Data->SetNumberField(TEXT("pipelines_count"), ID->GetNumberOfPipelines());
    Data->SetArrayField(TEXT("pipelines"), PipelineArr);
    Data->SetBoolField(TEXT("has_node_container"), NC != nullptr);
    Data->SetArrayField(TEXT("source_filenames"), FilenamesArr);
    Data->SetArrayField(TEXT("display_labels"), DisplayLabelsArr);

    if (bIncludeNodeContainer)
    {
        if (NC)
        {
            TArray<FString> RootUIDs;
            NC->GetRoots(RootUIDs);
            RootArr = mcp_common::StringsToJson(RootUIDs);

            // Iterate every factory node (template IterateNodesOfType<T>).
            NC->IterateNodesOfType<UInterchangeFactoryBaseNode>(
                [&FactoryArr](const FString& UID, UInterchangeFactoryBaseNode* Node)
                {
                    if (!Node) { return; }
                    TSharedPtr<FJsonObject> NObj = MakeShared<FJsonObject>();
                    const UClass* NodeClass = Node->GetObjectClass();
                    NObj->SetStringField(TEXT("uid"), UID);
                    NObj->SetStringField(TEXT("class"), NodeClass ? NodeClass->GetName() : FString());
                    NObj->SetStringField(TEXT("display_label"), Node->GetDisplayLabel());
                    FactoryArr.Add(MakeShared<FJsonValueObject>(NObj));
                });
        }
        AddNodeContainer();
    }
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// set_interchange_pipelines
// Replaces the pipeline stack on an asset's UInterchangeAssetImportData.
// Idempotent (SetPipelines clears TransientPipelines before applying — impl
// verified in InterchangeAssetImportData.cpp).
// ---------------------------------------------------------------------------
// MCP:set_interchange_pipelines - Replace the UInterchangePipelineBase stack on an asset's UInterchangeAssetImportData.  Loads the asset + its UInterchangeAssetImportData, resolves each path in ``pipeline_paths`` via LoadObject<UObject>, then calls ID->SetPipelines(InPipelines) (InterchangeAssetImportData.h, INTERCHANGEENGINE_API). Idempotent: SetPipelines resets TransientPipelines before applying (impl InterchangeAssetImportData.cpp). Marks the package dirty so the change persists on save.
//   @param asset_path [string!] - Asset path (e.g. "/Game/Foo/Foo")
//   @param pipeline_paths [array!] - Array of fully-qualified pipeline asset paths (UObject* resolved via LoadObject). Entries that fail to resolve are reported in ``unresolved_paths`` but do not abort the call.
//   @returns { asset_path: string, present: bool, pipeline_count: number, unresolved_paths: [string] }
TSharedPtr<FJsonObject> HandleInterchangeSetPipelines(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    const TArray<TSharedPtr<FJsonValue>>* PipelinePathsArr = nullptr;
    if (!Params->TryGetArrayField(TEXT("pipeline_paths"), PipelinePathsArr) || !PipelinePathsArr)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required 'pipeline_paths' parameter (array of asset paths)"));
    }

    UObject* Asset = nullptr;
    UInterchangeAssetImportData* ID = nullptr;
    if (TSharedPtr<FJsonObject> ResolveError = Interchange_ResolveImportData(AssetPath, Asset, ID)) { return ResolveError; }
    if (!ID)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Asset '%s' has no UInterchangeAssetImportData (legacy import?)"), *AssetPath));
    }

    TArray<UObject*> Resolved;
    TArray<TSharedPtr<FJsonValue>> UnresolvedArr;
    for (const TSharedPtr<FJsonValue>& V : *PipelinePathsArr)
    {
        FString Path;
        if (!V->TryGetString(Path) || Path.IsEmpty())
        {
            UnresolvedArr.Add(MakeShared<FJsonValueString>(TEXT("<empty or non-string>")));
            continue;
        }
        if (UObject* P = LoadObject<UObject>(nullptr, *Path))
        {
            Resolved.Add(P);
        }
        else
        {
            UnresolvedArr.Add(MakeShared<FJsonValueString>(Path));
        }
    }

    // Idempotent — SetPipelines clears TransientPipelines before applying.
    ID->Modify();
    ID->SetPipelines(Resolved);
    if (UPackage* Pkg = Asset->GetOutermost())
    {
        Pkg->MarkPackageDirty();
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("present"), true);
    Data->SetNumberField(TEXT("pipeline_count"), Resolved.Num());
    Data->SetArrayField(TEXT("unresolved_paths"), UnresolvedArr);
    return mcp_common::MakeOkReply(Data);
}

// Project settings and registration.

// MCP:get_interchange_project_settings - Read the project's Interchange settings, the live class defaults of UInterchangeProjectSettings as Project Settings shows them. For content import: the default pipeline stack, every pipeline stack with its pipeline paths, the per-asset-type default-stack overrides, the import dialog class and the import and reimport dialog flags. For scene import: the default stack, the two dialog flags and only a count of its per-translator dialog overrides. Plus the file picker, generic pipeline and converter default pipeline classes, the static mesh smoothing fallback flag and the Interchange groups. Read-only, with no parameters. Change the default stack with set_interchange_pipeline_stack_default; read one pipeline asset with get_interchange_pipeline_properties; the formats translators accept: get_interchange_supported_formats.
//   @returns { content_import_settings: { default_pipeline_stack: string, import_dialog_class: string, b_show_import_dialog: bool, b_show_reimport_dialog: bool, pipeline_stacks: [{ name: string, pipelines: [string] }], pipeline_stacks_count: int, default_pipeline_stack_override: [{ translator_type: string, stack_name: string }] }, scene_import_settings: { default_pipeline_stack: string, b_show_import_dialog: bool, b_show_reimport_dialog: bool, per_translator_dialog_override_count: int }, file_picker_class: string, generic_pipeline_class: string, converter_default_pipeline: string, b_static_mesh_use_smooth_edges_if_smoothing_information_is_missing: bool, interchange_groups: [{ display_name: string, unique_id: string, default_pipeline_stack: string, b_show_import_dialog: bool, b_show_reimport_dialog: bool }], interchange_groups_count: int }
TSharedPtr<FJsonObject> HandleInterchangeGetProjectSettings(const TSharedPtr<FJsonObject>& Params)
{
    const UInterchangeProjectSettings* Settings = GetDefault<UInterchangeProjectSettings>();
    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

    // ContentImportSettings (inherits FInterchangeImportSettings).
    TSharedPtr<FJsonObject> ContentObj = MakeShared<FJsonObject>();
    ContentObj->SetStringField(TEXT("default_pipeline_stack"),
        Settings->ContentImportSettings.DefaultPipelineStack.ToString());
    ContentObj->SetStringField(TEXT("import_dialog_class"),
        Settings->ContentImportSettings.ImportDialogClass.IsValid() ? Settings->ContentImportSettings.ImportDialogClass.ToString() : FString());
    ContentObj->SetBoolField(TEXT("b_show_import_dialog"), Settings->ContentImportSettings.bShowImportDialog);
    ContentObj->SetBoolField(TEXT("b_show_reimport_dialog"), Settings->ContentImportSettings.bShowReimportDialog);

    // PipelineStacks: TMap<FName, FInterchangePipelineStack> -> array of {name, pipelines[]}.
    TArray<TSharedPtr<FJsonValue>> StackArr;
    for (const TPair<FName, FInterchangePipelineStack>& StackPair : Settings->ContentImportSettings.PipelineStacks)
    {
        TSharedPtr<FJsonObject> StackObj = MakeShared<FJsonObject>();
        StackObj->SetStringField(TEXT("name"), StackPair.Key.ToString());
        TArray<TSharedPtr<FJsonValue>> PipelineRefs;
        for (const FSoftObjectPath& PipelineRef : StackPair.Value.Pipelines)
        {
            PipelineRefs.Add(MakeShared<FJsonValueString>(PipelineRef.ToString()));
        }
        StackObj->SetArrayField(TEXT("pipelines"), PipelineRefs);
        StackArr.Add(MakeShared<FJsonValueObject>(StackObj));
    }
    ContentObj->SetArrayField(TEXT("pipeline_stacks"), StackArr);
    ContentObj->SetNumberField(TEXT("pipeline_stacks_count"), Settings->ContentImportSettings.PipelineStacks.Num());

    // DefaultPipelineStackOverride (TMap<EInterchangeTranslatorAssetType, FName>).
    TArray<TSharedPtr<FJsonValue>> OverrideArr;
    for (const TPair<EInterchangeTranslatorAssetType, FName>& Ovr : Settings->ContentImportSettings.DefaultPipelineStackOverride)
    {
        TSharedPtr<FJsonObject> OvrObj = MakeShared<FJsonObject>();
        FString EnumName = StaticEnum<EInterchangeTranslatorAssetType>()
            ? StaticEnum<EInterchangeTranslatorAssetType>()->GetNameStringByValue(static_cast<int64>(Ovr.Key))
            : FString::FromInt(static_cast<int32>(Ovr.Key));
        OvrObj->SetStringField(TEXT("translator_type"), EnumName);
        OvrObj->SetStringField(TEXT("stack_name"), Ovr.Value.ToString());
        OverrideArr.Add(MakeShared<FJsonValueObject>(OvrObj));
    }
    ContentObj->SetArrayField(TEXT("default_pipeline_stack_override"), OverrideArr);
    Data->SetObjectField(TEXT("content_import_settings"), ContentObj);

    // SceneImportSettings (inherits FInterchangeImportSettings).
    TSharedPtr<FJsonObject> SceneObj = MakeShared<FJsonObject>();
    SceneObj->SetStringField(TEXT("default_pipeline_stack"),
        Settings->SceneImportSettings.DefaultPipelineStack.ToString());
    SceneObj->SetBoolField(TEXT("b_show_import_dialog"), Settings->SceneImportSettings.bShowImportDialog);
    SceneObj->SetBoolField(TEXT("b_show_reimport_dialog"), Settings->SceneImportSettings.bShowReimportDialog);
    SceneObj->SetNumberField(TEXT("per_translator_dialog_override_count"),
        Settings->SceneImportSettings.PerTranslatorDialogOverride.Num());
    Data->SetObjectField(TEXT("scene_import_settings"), SceneObj);

    // File picker + editor pipeline class + converter default pipeline.
    Data->SetStringField(TEXT("file_picker_class"),
        Settings->FilePickerClass.IsValid() ? Settings->FilePickerClass.ToString() : FString());
    Data->SetStringField(TEXT("generic_pipeline_class"),
        Settings->GenericPipelineClass.IsValid() ? Settings->GenericPipelineClass.ToString() : FString());
    Data->SetStringField(TEXT("converter_default_pipeline"),
        Settings->ConverterDefaultPipeline.IsValid() ? Settings->ConverterDefaultPipeline.ToString() : FString());
    Data->SetBoolField(TEXT("b_static_mesh_use_smooth_edges_if_smoothing_information_is_missing"),
        Settings->bStaticMeshUseSmoothEdgesIfSmoothingInformationIsMissing);

    // InterchangeGroups (TArray<FInterchangeGroup> on the project settings, NOT on ContentImportSettings).
    TArray<TSharedPtr<FJsonValue>> GroupsArr;
    for (const FInterchangeGroup& G : Settings->InterchangeGroups)
    {
        TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
        GObj->SetStringField(TEXT("display_name"), G.DisplayName.ToString());
        GObj->SetStringField(TEXT("unique_id"), G.UniqueID.ToString(EGuidFormats::DigitsWithHyphens));
        GObj->SetStringField(TEXT("default_pipeline_stack"), G.DefaultPipelineStack.ToString());
        GObj->SetBoolField(TEXT("b_show_import_dialog"), G.bShowImportDialog);
        GObj->SetBoolField(TEXT("b_show_reimport_dialog"), G.bShowReimportDialog);
        GroupsArr.Add(MakeShared<FJsonValueObject>(GObj));
    }
    Data->SetArrayField(TEXT("interchange_groups"), GroupsArr);
    Data->SetNumberField(TEXT("interchange_groups_count"), Settings->InterchangeGroups.Num());

    return mcp_common::MakeOkReply(Data);
}

// MCP:set_interchange_pipeline_stack_default - Choose the named pipeline stack Interchange uses by default for content imports (assets) or scene imports (levels) -- the DefaultPipelineStack that Project Settings > Interchange shows -- and write it to the project's DefaultEngine.ini, so it outlives the editor session. The stack must already exist in that import path's pipeline stacks: an unknown name is refused with the list of the ones there, and so is a read-only DefaultEngine.ini, both before anything changes. The previous default is overwritten and returned in previous_stack_name; choosing the stack that is already the default changes nothing (changed=false), so a repeat call is harmless (idempotent). No undo step is recorded. The content stacks and both current defaults are listed by get_interchange_project_settings; to choose the pipelines of one imported asset instead, use set_interchange_pipelines.
//   @param content_or_scene [string!] - Which default to set: "content" for asset imports (ContentImportSettings) or "scene" for level imports (SceneImportSettings). Case-insensitive.
//   @param stack_name [string!] - Name of an existing pipeline stack of that import path, e.g. "Assets"; matched without case.
//   @returns { content_or_scene, stack_name, previous_stack_name, changed, config_file, applied }
TSharedPtr<FJsonObject> HandleInterchangeSetPipelineStackDefault(const TSharedPtr<FJsonObject>& Params)
{
    FString ContentOrScene;
    if (!Params->TryGetStringField(TEXT("content_or_scene"), ContentOrScene))
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'content_or_scene' parameter (\"content\" or \"scene\")"));
    }

    FString StackName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("stack_name"), StackName)) { return Missing; }

    UInterchangeProjectSettings* Settings = GetMutableDefault<UInterchangeProjectSettings>();
    const FString NormType = ContentOrScene.ToLower();
    FInterchangeImportSettings* ImportSettings = nullptr;
    if (NormType == TEXT("content"))
    {
        ImportSettings = &Settings->ContentImportSettings;
    }
    else if (NormType == TEXT("scene"))
    {
        ImportSettings = &Settings->SceneImportSettings;
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid content_or_scene '%s' (expected 'content' or 'scene')"), *ContentOrScene));
    }

    // A default that names no stack leaves imports without pipelines, so only an existing stack is accepted,
    // under the spelling the map stores.
    const FName Requested(*StackName);
    FName StackKey = NAME_None;
    TArray<FString> Available;
    for (const TPair<FName, FInterchangePipelineStack>& Stack : ImportSettings->PipelineStacks)
    {
        Available.Add(Stack.Key.ToString());
        if (Stack.Key == Requested)
        {
            StackKey = Stack.Key;
        }
    }
    if (StackKey.IsNone())
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s import has no pipeline stack named '%s'; its stacks are: %s. Nothing was changed."),
            *NormType, *StackName, Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
    }

    const FString ConfigFile = FPaths::ConvertRelativePathToFull(Settings->GetDefaultConfigFilename());
    if (FPaths::FileExists(ConfigFile) && IFileManager::Get().IsReadOnly(*ConfigFile))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s is read-only (checked in, or locked by source control); make it writable first. Nothing was changed."),
            *ConfigFile));
    }

    const FName Previous = ImportSettings->DefaultPipelineStack;
    ImportSettings->DefaultPipelineStack = StackKey;
    if (!Settings->TryUpdateDefaultConfigFile())
    {
        ImportSettings->DefaultPipelineStack = Previous;
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Could not write %s; the default stack was put back to '%s'."), *ConfigFile, *Previous.ToString()));
    }
    Settings->PostEditChange();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("content_or_scene"), NormType);
    Data->SetStringField(TEXT("stack_name"), StackKey.ToString());
    Data->SetStringField(TEXT("previous_stack_name"), Previous.ToString());
    Data->SetBoolField(TEXT("changed"), Previous != StackKey);
    Data->SetStringField(TEXT("config_file"), ConfigFile);
    Data->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Data);
}

namespace
{
/** Size of the manager's registered-translator set, read by reflection (the set has no getter); INDEX_NONE if the property is gone. */
int32 NumRegisteredTranslators(UInterchangeManager& Manager)
{
    const FSetProperty* SetProp = FindFProperty<FSetProperty>(UInterchangeManager::StaticClass(), TEXT("RegisteredTranslatorsClass"));
    if (!SetProp)
    {
        return INDEX_NONE;
    }
    FScriptSetHelper Helper(SetProp, SetProp->ContainerPtrToValuePtr<void>(&Manager));
    return Helper.Num();
}
}

// MCP:register_interchange_translator - Register a translator class -- the Interchange reader of one or more source file formats -- with the Interchange manager for this editor session, so imports and get_interchange_supported_formats pick up its formats. Nothing is written to disk and the registration ends when the editor closes; the engine registers its own translators on startup, so this is for a class whose plugin did not. The class must load and be a concrete UInterchangeTranslatorBase subclass: anything else is refused before it is registered, since the manager would assert on the next format query. A class already registered changes nothing and reports already_registered=true, so a repeat call is harmless (idempotent). A factory, which turns translated data into assets, registers with register_interchange_factory; whether a given file can be read: can_translate_interchange_source_data.
//   @param translator_class_path [string!] - Class path of the translator class, e.g. "/Script/InterchangeImport.InterchangeImageWrapperTranslator"; loaded with LoadClass.
//   @returns { translator_class_path, class, registered, already_registered?, supported_formats: [string] }
TSharedPtr<FJsonObject> HandleInterchangeRegisterTranslator(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("translator_class_path"), ClassPath)) { return Missing; }

    UClass* Cls = LoadClass<UObject>(nullptr, *ClassPath);
    if (!Cls)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("translator class not found: '%s'"), *ClassPath));
    }
    // The manager stores any class and later reads it as a translator CDO, which asserts on anything else.
    if (!Cls->IsChildOf<UInterchangeTranslatorBase>() || Cls->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' is not a concrete Interchange translator (a non-abstract UInterchangeTranslatorBase subclass); nothing was registered."),
            *Cls->GetPathName()));
    }

    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    const int32 Before = NumRegisteredTranslators(M);
    const bool Registered = M.RegisterTranslator(Cls);
    const int32 After = NumRegisteredTranslators(M);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("translator_class_path"), ClassPath);
    Data->SetStringField(TEXT("class"), Cls->GetName());
    Data->SetBoolField(TEXT("registered"), Registered);
    if (Before != INDEX_NONE)
    {
        Data->SetBoolField(TEXT("already_registered"), After == Before);
    }
    Data->SetArrayField(TEXT("supported_formats"),
        mcp_common::StringsToJson(Cls->GetDefaultObject<UInterchangeTranslatorBase>()->GetSupportedFormats()));
    return mcp_common::MakeOkReply(Data);
}

// MCP:register_interchange_factory - Register a factory class -- the Interchange step that creates one kind of asset, such as a texture or a static mesh, from translated data -- with the Interchange manager for this editor session. Nothing is written to disk and the registration ends when the editor closes; the engine registers its own factories on startup, so this is for a class whose plugin did not. The manager keeps one factory per asset class and the first one registered stays: when another factory already makes that asset class the call is refused and names it. Registering the factory that is already active changes nothing and reports already_registered=true, so a repeat call is harmless (idempotent). A class that does not load, is abstract or is not a UInterchangeFactoryBase subclass is refused. The reader of a source file format registers with register_interchange_translator.
//   @param factory_class_path [string!] - Class path of the factory class, e.g. "/Script/InterchangeImport.InterchangeTextureFactory"; loaded with LoadClass.
//   @returns { factory_class_path, class, asset_class, registered, already_registered }
TSharedPtr<FJsonObject> HandleInterchangeRegisterFactory(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("factory_class_path"), ClassPath)) { return Missing; }

    UClass* Cls = LoadClass<UObject>(nullptr, *ClassPath);
    if (!Cls)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("factory class not found: '%s'"), *ClassPath));
    }
    if (!Cls->IsChildOf<UInterchangeFactoryBase>() || Cls->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' is not a concrete Interchange factory (a non-abstract UInterchangeFactoryBase subclass); nothing was registered."),
            *Cls->GetPathName()));
    }
    UClass* AssetClass = Cls->GetDefaultObject<UInterchangeFactoryBase>()->GetFactoryClass();
    if (!AssetClass)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("'%s' names no asset class (GetFactoryClass returns none), so the manager cannot register it."), *Cls->GetPathName()));
    }

    // RegisterFactory keeps the first factory of an asset class and still returns true, so the outcome is read
    // back from the class it now resolves to.
    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    const bool bAlready = M.GetRegisteredFactoryClass(AssetClass) == Cls;
    M.RegisterFactory(Cls);
    const UClass* Active = M.GetRegisteredFactoryClass(AssetClass);
    if (Active != Cls)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("%s already makes %s and the Interchange manager keeps the first factory of an asset class; '%s' was not registered."),
            Active ? *Active->GetPathName() : TEXT("another factory"), *AssetClass->GetName(), *Cls->GetPathName()));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("factory_class_path"), ClassPath);
    Data->SetStringField(TEXT("class"), Cls->GetName());
    Data->SetStringField(TEXT("asset_class"), AssetClass->GetPathName());
    Data->SetBoolField(TEXT("registered"), true);
    Data->SetBoolField(TEXT("already_registered"), bAlready);
    return mcp_common::MakeOkReply(Data);
}

// Pipeline asset authoring: create pipeline / reflection set+get / save

// MCP:create_interchange_pipeline_asset - Create an Interchange pipeline asset of a concrete pipeline class (the generic texture, mesh, material, animation, audio, scenes, groom or assets pipelines, glTF, sparse volume texture, MaterialX). Idempotent: an object of that name already in package_path is returned with already_existed:true.
//   @param pipeline_class_path [string!] - Class path of the concrete (non-abstract) UInterchangePipelineBase subclass to instantiate (e.g. "/Script/InterchangePipelines.InterchangeGenericAssetsPipeline"), loaded via LoadClass.
//   @param package_path [string] - Destination UE content folder for the new pipeline asset (e.g. "/Game/Pipelines"). Defaults to "/Game".
//   @param asset_name [string!] - Name of the new pipeline asset (the object/package name under package_path).
//   @returns { pipeline_path: string, class: string, already_existed: bool, note: string }
TSharedPtr<FJsonObject> HandleInterchangeCreatePipelineAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassPath, PackagePath, AssetName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pipeline_class_path"), ClassPath)) { return Missing; }
    if (!Params->TryGetStringField(TEXT("package_path"), PackagePath))
    {
        PackagePath = TEXT("/Game");
    }
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_name"), AssetName)) { return Missing; }

    // Idempotency: if it already exists, return it.
    const FString FullPath = PackagePath / AssetName + TEXT(".") + AssetName;
    if (TSharedPtr<FJsonObject> Invalid = mcp_common::InvalidPackageNameReply(FullPath)) { return Invalid; }
    if (UObject* Existing = LoadObject<UObject>(nullptr, *FullPath))
    {
        TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetStringField(TEXT("pipeline_path"), Existing->GetPathName());
        Data->SetStringField(TEXT("class"), Existing->GetClass()->GetName());
        Data->SetBoolField(TEXT("already_existed"), true);
        return mcp_common::MakeOkReply(Data);
    }

    UClass* PipelineClass = LoadClass<UObject>(nullptr, *ClassPath);
    if (!PipelineClass)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("pipeline class not found: '%s'"), *ClassPath));
    }
    if (PipelineClass->HasAnyClassFlags(CLASS_Abstract))
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("pipeline class '%s' is abstract; pick a concrete MinimalAPI subclass"), *ClassPath));
    }

    FString PackageError;
    UPackage* Package = mcp_common::CreateAssetPackage(*(PackagePath / AssetName), FName(*AssetName), PipelineClass, PackageError);
    if (!Package)
    {
        return mcp_common::MakeErrorReply(PackageError);
    }
    Package->FullyLoad();

    UInterchangePipelineBase* Pipeline = NewObject<UInterchangePipelineBase>(
        Package, PipelineClass, FName(*AssetName), RF_Public | RF_Standalone);
    if (!Pipeline)
    {
        return mcp_common::MakeErrorReply(
            TEXT("NewObject<UInterchangePipelineBase> returned null"));
    }

    FAssetRegistryModule::AssetCreated(Pipeline);
    Package->MarkPackageDirty();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pipeline_path"), Pipeline->GetPathName());
    Data->SetStringField(TEXT("class"), Pipeline->GetClass()->GetName());
    Data->SetBoolField(TEXT("already_existed"), false);
    Data->SetStringField(TEXT("note"), TEXT("Pipeline asset created; call save_interchange_pipeline to persist"));
    return mcp_common::MakeOkReply(Data);
}

// MCP:set_interchange_pipeline_properties - Write settings of an Interchange pipeline asset -- what a pipeline does at import, such as whether it imports materials or how it names the assets -- by reflection, from a {name: value} object in which a value is a scalar, an object for a struct or an array for a TArray. Each value overwrites the one in memory, records no undo step and is not saved: save_interchange_pipeline writes the asset to disk. Names are applied one by one; one that does not exist, is not editable in the editor or does not convert is skipped and listed with the reason in skipped, and the others are still written. An empty properties object is refused. Writing the same values again leaves the asset as it was (idempotent). The names, types and current values come from get_interchange_pipeline_properties; to choose which pipelines an imported asset uses, set_interchange_pipelines.
//   @param pipeline_path [string!] - Object path of the pipeline asset, e.g. "/Game/Pipelines/MyMeshPipeline.MyMeshPipeline"; it must load as a UInterchangePipelineBase.
//   @param properties [object!] - {property_name: value}, with names as get_interchange_pipeline_properties lists them; a struct takes an object of its fields and a TArray an array.
//   @returns { pipeline_path, applied_count, skipped_count, skipped?: [string], note }
TSharedPtr<FJsonObject> HandleInterchangeSetPipelineProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString PipelinePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pipeline_path"), PipelinePath)) { return Missing; }
    const TSharedPtr<FJsonObject>* PropsJson = nullptr;
    if (!Params->TryGetObjectField(TEXT("properties"), PropsJson) || !PropsJson || !PropsJson->IsValid())
    {
        return mcp_common::MakeErrorReply(
            TEXT("Missing required 'properties' object field"));
    }
    if ((*PropsJson)->Values.Num() == 0)
    {
        return mcp_common::MakeErrorReply(
            TEXT("properties is empty; pass {property_name: value} (get_interchange_pipeline_properties lists the names)"));
    }

    UObject* Asset = LoadObject<UObject>(nullptr, *PipelinePath);
    UInterchangePipelineBase* Pipeline = Cast<UInterchangePipelineBase>(Asset);
    if (!Pipeline)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("pipeline not found at '%s' (or not a UInterchangePipelineBase)"), *PipelinePath));
    }

    Pipeline->Modify();
    int32 AppliedCount = 0;
    int32 SkippedCount = 0;
    TArray<FString> SkippedNames;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsJson)->Values)
    {
        const FString& PropName = Pair.Key;
        const TSharedPtr<FJsonValue>& JsonVal = Pair.Value;

        FProperty* Prop = Pipeline->GetClass()->FindPropertyByName(FName(*PropName));
        if (!Prop)
        {
            ++SkippedCount;
            SkippedNames.Add(PropName + TEXT(" (not found)"));
            continue;
        }
        // Skip CPF_Transient / !CPF_Edit to avoid writing runtime-only fields.
        if (!(Prop->PropertyFlags & CPF_Edit))
        {
            ++SkippedCount;
            SkippedNames.Add(PropName + TEXT(" (not EditAnywhere)"));
            continue;
        }
        if (Prop->HasAnyPropertyFlags(CPF_EditConst))
        {
            ++SkippedCount;
            SkippedNames.Add(PropName + TEXT(" (read-only in the editor)"));
            continue;
        }

        void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Pipeline);
        const TSharedRef<FJsonValue> JsonRef = JsonVal.ToSharedRef();
        const bool bConverted = FJsonObjectConverter::JsonValueToUProperty(
            JsonRef, Prop, ValuePtr, 0, 0);
        if (bConverted)
        {
            ++AppliedCount;
        }
        else
        {
            ++SkippedCount;
            SkippedNames.Add(PropName + TEXT(" (type mismatch)"));
        }
    }
    Pipeline->PostEditChange();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pipeline_path"), PipelinePath);
    Data->SetNumberField(TEXT("applied_count"), AppliedCount);
    Data->SetNumberField(TEXT("skipped_count"), SkippedCount);
    if (SkippedNames.Num() > 0)
    {
        Data->SetArrayField(TEXT("skipped"), mcp_common::StringsToJson(SkippedNames));
    }
    Data->SetStringField(TEXT("note"), TEXT("Call save_interchange_pipeline to persist the new values"));
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_interchange_pipeline_properties - Read the editable (EditAnywhere) properties of an Interchange pipeline asset by reflection, inherited ones included: each entry gives name, C++ type and, unless class_only is true, the current value as JSON (left out when it cannot be converted). The values are the in-memory ones, so edits made with set_interchange_pipeline_properties and not yet saved already show. Read-only: it writes nothing and needs no PIE. A path that does not load as a UInterchangePipelineBase is an error. Discover the names with class_only=true, write them with set_interchange_pipeline_properties and persist with save_interchange_pipeline; the pipelines an imported asset used: get_interchange_asset_import_data; a new pipeline asset: create_interchange_pipeline_asset.
//   @param pipeline_path [string!] - Object path of the pipeline asset to read (loaded via LoadObject, must be a UInterchangePipelineBase), e.g. "/Game/Pipelines/MyMeshPipeline.MyMeshPipeline".
//   @param class_only [bool] - When true, return only the EditAnywhere property NAMES and types (no values) for discovery. Defaults to false (includes current values).
//   @returns { pipeline_path: string, class: string, properties: [{ name: string, type: string, value?: any }], property_count: number }
TSharedPtr<FJsonObject> HandleInterchangeGetPipelineProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString PipelinePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pipeline_path"), PipelinePath)) { return Missing; }
    bool bClassOnly = false;
    Params->TryGetBoolField(TEXT("class_only"), bClassOnly);

    UObject* Asset = LoadObject<UObject>(nullptr, *PipelinePath);
    UInterchangePipelineBase* Pipeline = Cast<UInterchangePipelineBase>(Asset);
    if (!Pipeline)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("pipeline not found at '%s' (or not a UInterchangePipelineBase)"), *PipelinePath));
    }

    TArray<TSharedPtr<FJsonValue>> PropsArr;
    for (TFieldIterator<FProperty> It(Pipeline->GetClass()); It; ++It)
    {
        FProperty* Prop = *It;
        if (!(Prop->PropertyFlags & CPF_Edit)) { continue; }
        TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
        PObj->SetStringField(TEXT("name"), Prop->GetName());
        PObj->SetStringField(TEXT("type"), Prop->GetCPPType());
        if (!bClassOnly)
        {
            void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Pipeline);
            TSharedPtr<FJsonValue> JsonVal = FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0);
            if (JsonVal.IsValid())
            {
                PObj->SetField(TEXT("value"), JsonVal);
            }
        }
        PropsArr.Add(MakeShared<FJsonValueObject>(PObj));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pipeline_path"), PipelinePath);
    Data->SetStringField(TEXT("class"), Pipeline->GetClass()->GetName());
    Data->SetArrayField(TEXT("properties"), PropsArr);
    Data->SetNumberField(TEXT("property_count"), PropsArr.Num());
    return mcp_common::MakeOkReply(Data);
}

// MCP:save_interchange_pipeline - Write the package of an Interchange pipeline asset (a UInterchangePipelineBase loaded by object path) over its .uasset on disk, dirty or not, through the shared asset save; saved false makes success false and save_error carries the measured cause. It is the persist step after create_interchange_pipeline_asset and set_interchange_pipeline_properties, which change memory only; repeating it is idempotent. Like save_asset it refuses during PIE and under a read-only root. A path that does not load as a pipeline is an error.
//   @param pipeline_path [string!] - Object path of the pipeline asset whose package is written to disk (loaded via LoadObject, must be a UInterchangePipelineBase).
//   @returns { pipeline_path: string, file_path: string, saved: bool, save_error?: string }
TSharedPtr<FJsonObject> HandleInterchangeSavePipeline(const TSharedPtr<FJsonObject>& Params)
{
    FString PipelinePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("pipeline_path"), PipelinePath)) { return Missing; }

    UObject* Asset = LoadObject<UObject>(nullptr, *PipelinePath);
    UInterchangePipelineBase* Pipeline = Cast<UInterchangePipelineBase>(Asset);
    if (!Pipeline)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("pipeline not found at '%s'"), *PipelinePath));
    }

    UPackage* Package = Pipeline->GetOutermost();
    if (!Package)
    {
        return mcp_common::MakeErrorReply(
            TEXT("pipeline has no package (orphaned object)"));
    }

    FString FilePath = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());

    // The shared save refuses during PIE and under a read-only root, and measures why a write failed.
    FString SaveError;
    const bool bSaved = mcp_common::SaveAssetPackage(Package, Pipeline, SaveError);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("pipeline_path"), PipelinePath);
    Data->SetStringField(TEXT("file_path"), FilePath);
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("save_interchange_pipeline"), SaveError);
}

// Headless imports via UInterchangeManager (medium risk: game-thread)

// MCP:can_translate_interchange_source_data - Whether a registered Interchange translator can read a source file (a path on disk or an asset path). Read-only.
//   @param file_path [string!] - Path of the source file to test on disk (e.g. "C:/Assets/Mesh.fbx"), or an Unreal asset path. Passed to UInterchangeManager::CreateSourceData.
//   @param scene_only [bool] - When true, restrict the check to scene translators (bSceneOnly). Defaults to false (any translator type).
//   @returns { file_path: string, scene_only: bool, can_translate: bool }
TSharedPtr<FJsonObject> HandleInterchangeCanTranslateSourceData(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("file_path"), FilePath)) { return Missing; }
    bool bSceneOnly = false;
    Params->TryGetBoolField(TEXT("scene_only"), bSceneOnly);

    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    UInterchangeSourceData* SourceData = M.CreateSourceData(FilePath);
    if (!SourceData)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("could not create source data for path '%s'"), *FilePath));
    }
    const bool bCanTranslate = M.CanTranslateSourceData(SourceData, bSceneOnly);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("file_path"), FilePath);
    Data->SetBoolField(TEXT("scene_only"), bSceneOnly);
    Data->SetBoolField(TEXT("can_translate"), bCanTranslate);
    return mcp_common::MakeOkReply(Data);
}

// MCP:import_interchange_asset - Import a source file synchronously through the Interchange manager: unattended, no dialogs, replacing existing assets unless replace_existing is false. When the InterchangeImport plugin is not loaded the note says so.
//   @param file_path [string!] - Path of the source file to import on disk (e.g. "C:/Assets/Mesh.fbx"). Passed to UInterchangeManager::CreateSourceData.
//   @param destination_path [string] - Destination UE content folder for the imported asset(s) (e.g. "/Game/Meshes"). Defaults to "/Game/Imported".
//   @param replace_existing [bool] - When true, overwrite an existing asset at the destination (bReplaceExisting). Defaults to true.
//   @returns { file_path: string, destination_path: string, imported: bool, count: number, imported_objects: [string], note: string }
TSharedPtr<FJsonObject> HandleInterchangeImportAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString FilePath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("file_path"), FilePath)) { return Missing; }
    FString DestinationPath;
    if (!Params->TryGetStringField(TEXT("destination_path"), DestinationPath))
    {
        DestinationPath = TEXT("/Game/Imported");
    }
    bool bReplaceExisting = true;
    Params->TryGetBoolField(TEXT("replace_existing"), bReplaceExisting);

    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    UInterchangeSourceData* SourceData = M.CreateSourceData(FilePath);
    if (!SourceData)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("could not create source data for path '%s'"), *FilePath));
    }

    FImportAssetParameters ImportParams;
    ImportParams.bIsAutomated = true;
    ImportParams.bReplaceExisting = bReplaceExisting;
    // DO NOT set ImportParams.DestinationName to DestinationPath. DestinationName is the ASSET
    // NAME, not the folder: the engine uses it verbatim as the package name
    // (InterchangeManager.cpp, FString AssetpackageName = ImportAssetParameters.DestinationName
    // .IsEmpty() ? GetAssetNameFromFilename() : ImportAssetParameters.DestinationName), while the
    // folder is the ContentPath argument of ImportAsset() below -- which already receives
    // DestinationPath. Setting both made "/Game/MyGame/Meshes" land as
    // ".../Meshes/_Game_MyGame_Meshes" with imported:true, and because bReplaceExisting defaults to
    // true, a SECOND import into the same folder reimported the FIRST asset instead of adding one
    // -- five FBX pieces collapsing into one, with a success response every time. Leaving it empty
    // restores the documented contract (destination_path = "Destination UE content folder") and
    // lets the engine derive the asset name from the source filename.

    TArray<UObject*> ImportedObjects;
    const bool bImported = M.ImportAsset(FString(DestinationPath), SourceData, ImportParams, ImportedObjects);

    TArray<TSharedPtr<FJsonValue>> ImportedPaths;
    for (UObject* Obj : ImportedObjects)
    {
        if (Obj) { ImportedPaths.Add(MakeShared<FJsonValueString>(Obj->GetPathName())); }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("file_path"), FilePath);
    Data->SetStringField(TEXT("destination_path"), DestinationPath);
    Data->SetBoolField(TEXT("imported"), bImported);
    Data->SetNumberField(TEXT("count"), ImportedPaths.Num());
    Data->SetArrayField(TEXT("imported_objects"), ImportedPaths);
    Data->SetStringField(TEXT("note"),
        FString::Printf(TEXT("Interchange manager %s. Imported %d object(s) under %s."),
            bImported ? TEXT("succeeded") : TEXT("returned false (check Interchange logs for asset-class translator failures)"),
            ImportedPaths.Num(), *DestinationPath));
    return mcp_common::MakeOkReply(Data);
}

// MCP:reimport_interchange_asset - Reimport an existing asset from its stored UInterchangeAssetImportData (UInterchangeManager::ReimportAsset). An asset without Interchange import data (a legacy factory import, or one never imported) gets an error.
//   @param asset_path [string!] - Asset path (e.g. "/Game/Foo/Foo") or fully-qualified object path ("/Game/Foo/Foo.Foo")
//   @returns { asset_path: string, reimported: bool, count: number, note: string }
TSharedPtr<FJsonObject> HandleInterchangeReimportAsset(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UObject* Asset = nullptr;
    UInterchangeAssetImportData* ID = nullptr;
    if (TSharedPtr<FJsonObject> ResolveError = Interchange_ResolveImportData(AssetPath, Asset, ID)) { return ResolveError; }

    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    if (!ID)
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("asset '%s' has no UInterchangeAssetImportData (legacy factory import or not Interchange-managed)"), *AssetPath));
    }

    FImportAssetParameters ImportParams;
    ImportParams.bIsAutomated = true;
    ImportParams.bReplaceExisting = true;
    ImportParams.ReimportAsset = Asset;

    TArray<UObject*> ReimportedObjects;
    const bool bReimported = M.ReimportAsset(Asset, ImportParams, ReimportedObjects);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);
    Data->SetBoolField(TEXT("reimported"), bReimported);
    Data->SetNumberField(TEXT("count"), ReimportedObjects.Num());
    Data->SetStringField(TEXT("note"),
        FString::Printf(TEXT("ReimportAsset %s. Reimported %d object(s)."),
            bReimported ? TEXT("succeeded") : TEXT("returned false (check editor log for translator failures)"),
            ReimportedObjects.Num()));
    return mcp_common::MakeOkReply(Data);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("can_translate_interchange_source_data"), &HandleInterchangeCanTranslateSourceData },
    { TEXT("create_interchange_pipeline_asset"), &HandleInterchangeCreatePipelineAsset },
    { TEXT("get_interchange_asset_import_data"), &HandleInterchangeGetAssetImportData },
    { TEXT("get_interchange_pipeline_properties"), &HandleInterchangeGetPipelineProperties },
    { TEXT("get_interchange_project_settings"), &HandleInterchangeGetProjectSettings },
    { TEXT("get_interchange_supported_formats"), &HandleInterchangeGetSupportedFormats },
    { TEXT("import_interchange_asset"), &HandleInterchangeImportAsset },
    { TEXT("register_interchange_factory"), &HandleInterchangeRegisterFactory },
    { TEXT("register_interchange_translator"), &HandleInterchangeRegisterTranslator },
    { TEXT("reimport_interchange_asset"), &HandleInterchangeReimportAsset },
    { TEXT("save_interchange_pipeline"), &HandleInterchangeSavePipeline },
    { TEXT("set_interchange_pipeline_properties"), &HandleInterchangeSetPipelineProperties },
    { TEXT("set_interchange_pipeline_stack_default"), &HandleInterchangeSetPipelineStackDefault },
    { TEXT("set_interchange_pipelines"), &HandleInterchangeSetPipelines },
};
} // namespace UEAgentInterchangeCommands
