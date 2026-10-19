#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"

// Interchange framework.
#include "InterchangeManager.h"                          // UInterchangeManager (INTERCHANGEENGINE_API)
#include "InterchangeAssetImportData.h"                  // UInterchangeAssetImportData (MinimalAPI inline + INTERCHANGEENGINE_API)
#include "InterchangeProjectSettings.h"                  // UInterchangeProjectSettings + FInterchangeProjectSettingsUtils (INTERCHANGEENGINE_API)
#include "InterchangeTranslatorBase.h"                   // EInterchangeTranslatorType (INTERCHANGECORE_API)
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

// Module status, formats, import data, pipelines, node container

// ---------------------------------------------------------------------------
// interchange_module_status
// Reports whether the Interchange framework is enabled and whether at least
// one translator is registered (derived indirectly, as the annotation explains).
// ---------------------------------------------------------------------------
// MCP:interchange_module_status - Report Interchange framework availability.  Reads UInterchangeManager::IsInterchangeImportEnabled() (InterchangeManager.h, INTERCHANGEENGINE_API) and counts the supported file extensions reported by GetSupportedFormats(Assets) (InterchangeManager.h). Translator count itself is derived because UInterchangeManager's RegisteredTranslatorsClass set is private in 5.8 (InterchangeManager.h) — we surface ``enabled`` (= import enabled AND at least one supported format) which carries the same operational signal.
//   @returns { enabled: bool, import_enabled: bool, asset_format_count: number, actor_format_count: number, scene_format_count: number, asset_formats: [string], translator_type_queried: string }
//   @hints read-only
TSharedPtr<FJsonObject> HandleInterchangeModuleStatus(const TSharedPtr<FJsonObject>& Params)
{
    const bool bImportEnabled = UInterchangeManager::IsInterchangeImportEnabled();

    UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
    const TArray<FString> AssetFormats  = Manager.GetSupportedFormats(EInterchangeTranslatorType::Assets);
    const TArray<FString> ActorFormats  = Manager.GetSupportedFormats(EInterchangeTranslatorType::Actors);
    const TArray<FString> SceneFormats  = Manager.GetSupportedFormats(EInterchangeTranslatorType::Scenes);

    const bool bEnabled = bImportEnabled && AssetFormats.Num() > 0;

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetBoolField(TEXT("enabled"), bEnabled);
    Data->SetBoolField(TEXT("import_enabled"), bImportEnabled);
    Data->SetNumberField(TEXT("asset_format_count"), AssetFormats.Num());
    Data->SetNumberField(TEXT("actor_format_count"), ActorFormats.Num());
    Data->SetNumberField(TEXT("scene_format_count"), SceneFormats.Num());

    Data->SetArrayField(TEXT("asset_formats"), mcp_common::StringsToJson(AssetFormats));

    Data->SetStringField(TEXT("translator_type_queried"), TEXT("Assets"));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// interchange_get_supported_formats
// Returns the file extensions supported by translators of the requested type.
// ---------------------------------------------------------------------------
// MCP:interchange_get_supported_formats - List file extensions supported by registered Interchange translators of a given type.  Calls UInterchangeManager::GetSupportedFormats(EInterchangeTranslatorType) (InterchangeManager.h, INTERCHANGEENGINE_API). The 5.8 enum has Invalid / Assets / Actors / Scenes (Scenes = Assets|Actors). Accepts aliases "Assets", "Actors", "Scenes", and "AssetsAndScene" (mapped to Scenes).
//   @param translator_type [string] - Translator type: "Assets" [default], "Actors", "Scenes", or the spec alias "AssetsAndScene"
//   @returns { translator_type: string, format_count: number, formats: [string] }
TSharedPtr<FJsonObject> HandleInterchangeGetSupportedFormats(const TSharedPtr<FJsonObject>& Params)
{
    FString TypeStr = TEXT("Assets");
    Params->TryGetStringField(TEXT("translator_type"), TypeStr);

    const EInterchangeTranslatorType Type = ParseTranslatorType(TypeStr);
    UInterchangeManager& Manager = UInterchangeManager::GetInterchangeManager();
    const TArray<FString> Formats = Manager.GetSupportedFormats(Type);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("translator_type"), TranslatorTypeToString(Type));
    Data->SetNumberField(TEXT("format_count"), Formats.Num());
    Data->SetArrayField(TEXT("formats"), mcp_common::StringsToJson(Formats));
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// interchange_get_asset_import_data
// Reads UInterchangeAssetImportData from an asset (or reports absent honestly
// for legacy assets that pre-date Interchange).
// ---------------------------------------------------------------------------
// MCP:interchange_get_asset_import_data - Read the Interchange import data (UInterchangeAssetImportData) of an asset loaded by package or object path: its source_filenames, display_labels, pipelines_count and whether it holds a node container. An asset imported through the legacy factory path, or never imported, has none: that answers present=false with zeros and empty lists, a normal state and not an error; only a path that does not load is an error. Read-only: it writes nothing and needs no PIE. The legacy UAssetImportData of a factory import (source file and timestamp): get_asset_import_data; the pipeline objects: interchange_get_pipelines; the nodes the import produced: interchange_get_node_container; reimport from this data: interchange_reimport_asset.
//   @param asset_path [string!] - Asset path (e.g. "/Game/Foo/Foo") or fully-qualified object path ("/Game/Foo/Foo.Foo")
//   @returns { asset_path: string, present: bool, pipelines_count: number, has_node_container: bool, source_filenames: [string], display_labels: [string] }
TSharedPtr<FJsonObject> HandleInterchangeGetAssetImportData(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UObject* Asset = nullptr;
    UInterchangeAssetImportData* ID = nullptr;
    if (TSharedPtr<FJsonObject> ResolveError = Interchange_ResolveImportData(AssetPath, Asset, ID)) { return ResolveError; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);

    if (!ID)
    {
        // Honest absence — legacy imports or non-imported assets. Not an error.
        Data->SetBoolField(TEXT("present"), false);
        Data->SetNumberField(TEXT("pipelines_count"), 0);
        Data->SetBoolField(TEXT("has_node_container"), false);
        TArray<TSharedPtr<FJsonValue>> EmptyArr;
        Data->SetArrayField(TEXT("source_filenames"), EmptyArr);
        Data->SetArrayField(TEXT("display_labels"), EmptyArr);
        return mcp_common::MakeOkReply(Data);
    }

    const TArray<TSharedPtr<FJsonValue>> FilenamesArr = mcp_common::StringsToJson(ID->ScriptExtractFilenames());
    const TArray<TSharedPtr<FJsonValue>> DisplayLabelsArr = mcp_common::StringsToJson(ID->ScriptExtractDisplayLabels());

    Data->SetBoolField(TEXT("present"), true);
    Data->SetNumberField(TEXT("pipelines_count"), ID->GetNumberOfPipelines());
    Data->SetBoolField(TEXT("has_node_container"), ID->GetNodeContainer() != nullptr);
    Data->SetArrayField(TEXT("source_filenames"), FilenamesArr);
    Data->SetArrayField(TEXT("display_labels"), DisplayLabelsArr);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// interchange_get_pipelines
// Lists the pipeline objects stored on the asset's UInterchangeAssetImportData.
// ---------------------------------------------------------------------------
// MCP:interchange_get_pipelines - List the UInterchangePipelineBase instances stored on an asset's UInterchangeAssetImportData.  Loads the asset, retrieves its UInterchangeAssetImportData via UInterchangeManager::GetAssetImportData (InterchangeManager.h), then calls ID->GetPipelines() (InterchangeAssetImportData.h, INTERCHANGEENGINE_API). Returns the class, name, and full object path of each pipeline.
//   @param asset_path [string!] - Asset path (e.g. "/Game/Foo/Foo") or fully-qualified object path
//   @returns { asset_path: string, present: bool, pipeline_count: number, pipelines: [object] }
TSharedPtr<FJsonObject> HandleInterchangeGetPipelines(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UObject* Asset = nullptr;
    UInterchangeAssetImportData* ID = nullptr;
    if (TSharedPtr<FJsonObject> ResolveError = Interchange_ResolveImportData(AssetPath, Asset, ID)) { return ResolveError; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);

    if (!ID)
    {
        Data->SetBoolField(TEXT("present"), false);
        Data->SetNumberField(TEXT("pipeline_count"), 0);
        TArray<TSharedPtr<FJsonValue>> EmptyArr;
        Data->SetArrayField(TEXT("pipelines"), EmptyArr);
        return mcp_common::MakeOkReply(Data);
    }

    TArray<UObject*> Pipelines = ID->GetPipelines();

    TArray<TSharedPtr<FJsonValue>> PipelineArr;
    for (UObject* P : Pipelines)
    {
        if (!P) { continue; }
        TSharedPtr<FJsonObject> PObj = MakeShared<FJsonObject>();
        PObj->SetStringField(TEXT("class"), P->GetClass() ? P->GetClass()->GetName() : FString());
        PObj->SetStringField(TEXT("name"), P->GetName());
        PObj->SetStringField(TEXT("path"), P->GetPathName());
        PipelineArr.Add(MakeShared<FJsonValueObject>(PObj));
    }

    Data->SetBoolField(TEXT("present"), true);
    Data->SetNumberField(TEXT("pipeline_count"), PipelineArr.Num());
    Data->SetArrayField(TEXT("pipelines"), PipelineArr);
    return mcp_common::MakeOkReply(Data);
}

// ---------------------------------------------------------------------------
// interchange_set_pipelines
// Replaces the pipeline stack on an asset's UInterchangeAssetImportData.
// Idempotent (SetPipelines clears TransientPipelines before applying — impl
// verified in InterchangeAssetImportData.cpp).
// ---------------------------------------------------------------------------
// MCP:interchange_set_pipelines - Replace the UInterchangePipelineBase stack on an asset's UInterchangeAssetImportData.  Loads the asset + its UInterchangeAssetImportData, resolves each path in ``pipeline_paths`` via LoadObject<UObject>, then calls ID->SetPipelines(InPipelines) (InterchangeAssetImportData.h, INTERCHANGEENGINE_API). Idempotent: SetPipelines resets TransientPipelines before applying (impl InterchangeAssetImportData.cpp). Marks the package dirty so the change persists on save.
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

// ---------------------------------------------------------------------------
// interchange_get_node_container
// Reads the UInterchangeBaseNodeContainer from the asset and lists root UIDs
// plus every factory node (class + display label + unique ID).
// ---------------------------------------------------------------------------
// MCP:interchange_get_node_container - Read the node container kept in the Interchange import data of an asset loaded by path: the root_uids and every factory node, each with its uid, display_label and the class of the object that factory produces. An asset with no Interchange import data (a legacy factory import, or one never imported), or with import data but no node container, answers present=false with empty lists rather than an error; only a path that does not load is an error. Read-only: it writes nothing and needs no PIE. Use it to see what an Interchange import produced; source files and pipeline count: interchange_get_asset_import_data; the pipeline stack: interchange_get_pipelines; reimport: interchange_reimport_asset.
//   @param asset_path [string!] - Asset path (e.g. "/Game/Foo/Foo") or fully-qualified object path ("/Game/Foo/Foo.Foo")
//   @returns { asset_path: string, present: bool, root_uids: [string], factory_node_count: number, factory_nodes: [{ uid: string, class: string, display_label: string }] }
TSharedPtr<FJsonObject> HandleInterchangeGetNodeContainer(const TSharedPtr<FJsonObject>& Params)
{
    FString AssetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("asset_path"), AssetPath)) { return Missing; }

    UObject* Asset = nullptr;
    UInterchangeAssetImportData* ID = nullptr;
    if (TSharedPtr<FJsonObject> ResolveError = Interchange_ResolveImportData(AssetPath, Asset, ID)) { return ResolveError; }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), AssetPath);

    if (!ID)
    {
        Data->SetBoolField(TEXT("present"), false);
        TArray<TSharedPtr<FJsonValue>> EmptyArr;
        Data->SetArrayField(TEXT("root_uids"), EmptyArr);
        Data->SetNumberField(TEXT("factory_node_count"), 0);
        Data->SetArrayField(TEXT("factory_nodes"), EmptyArr);
        return mcp_common::MakeOkReply(Data);
    }

    UInterchangeBaseNodeContainer* NC = ID->GetNodeContainer();
    if (!NC)
    {
        Data->SetBoolField(TEXT("present"), false);
        TArray<TSharedPtr<FJsonValue>> EmptyArr;
        Data->SetArrayField(TEXT("root_uids"), EmptyArr);
        Data->SetNumberField(TEXT("factory_node_count"), 0);
        Data->SetArrayField(TEXT("factory_nodes"), EmptyArr);
        return mcp_common::MakeOkReply(Data);
    }

    TArray<FString> RootUIDs;
    NC->GetRoots(RootUIDs);

    const TArray<TSharedPtr<FJsonValue>> RootArr = mcp_common::StringsToJson(RootUIDs);

    // Iterate every factory node (template IterateNodesOfType<T>) — captures
    // UIDs and serialized fields by reference.
    TArray<TSharedPtr<FJsonValue>> FactoryArr;
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

    Data->SetBoolField(TEXT("present"), true);
    Data->SetArrayField(TEXT("root_uids"), RootArr);
    Data->SetNumberField(TEXT("factory_node_count"), FactoryArr.Num());
    Data->SetArrayField(TEXT("factory_nodes"), FactoryArr);
    return mcp_common::MakeOkReply(Data);
}

// Project settings and registration.

// MCP:interchange_get_project_settings - Read the project's Interchange settings, the live class defaults of UInterchangeProjectSettings as Project Settings shows them. For content import: the default pipeline stack, every pipeline stack with its pipeline paths, the per-asset-type default-stack overrides, the import dialog class and the import and reimport dialog flags. For scene import: the default stack, the two dialog flags and only a count of its per-translator dialog overrides. Plus the file picker, generic pipeline and converter default pipeline classes, the static mesh smoothing fallback flag and the Interchange groups. Read-only, with no parameters. Change the default stack with interchange_set_pipeline_stack_default; read one pipeline asset with interchange_get_pipeline_properties; the formats translators accept: interchange_get_supported_formats.
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

// MCP:interchange_set_pipeline_stack_default - Set the default Interchange pipeline stack for content or scene import (DefaultPipelineStack of the project's ContentImportSettings or SceneImportSettings), saved to the project's default config.
//   @param content_or_scene [string!] - Which import path's default stack to set: "content" (ContentImportSettings) or "scene" (SceneImportSettings). Case-insensitive.
//   @param stack_name [string!] - Name of the pipeline stack to make the default (an FName key into the project's PipelineStacks map).
//   @returns { content_or_scene: string, stack_name: string, applied: bool }
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
    Settings->Modify();

    FString NormType = ContentOrScene.ToLower();
    if (NormType == TEXT("content"))
    {
        Settings->ContentImportSettings.DefaultPipelineStack = FName(*StackName);
    }
    else if (NormType == TEXT("scene"))
    {
        Settings->SceneImportSettings.DefaultPipelineStack = FName(*StackName);
    }
    else
    {
        return mcp_common::MakeErrorReply(
            FString::Printf(TEXT("Invalid content_or_scene '%s' (expected 'content' or 'scene')"), *ContentOrScene));
    }

    Settings->TryUpdateDefaultConfigFile();
    Settings->PostEditChange();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("content_or_scene"), ContentOrScene);
    Data->SetStringField(TEXT("stack_name"), StackName);
    Data->SetBoolField(TEXT("applied"), true);
    return mcp_common::MakeOkReply(Data);
}

// MCP:interchange_register_translator - Load a translator class (e.g. /Script/InterchangeEngine.InterchangeTextureTranslator) and register it with the Interchange manager. Idempotent: a class already registered is a no-op.
//   @param translator_class_path [string!] - Class path of the translator UClass to instantiate and register (e.g. "/Script/InterchangeEngine.InterchangeTextureTranslator"), loaded via LoadClass.
//   @returns { translator_class_path: string, class: string, registered: bool, note: string }
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

    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    bool Registered = M.RegisterTranslator(Cls);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("translator_class_path"), ClassPath);
    Data->SetStringField(TEXT("class"), Cls->GetName());
    Data->SetBoolField(TEXT("registered"), Registered);
    Data->SetStringField(TEXT("note"), TEXT("RegisterTranslator dedups internally; re-running returns true with no duplicate"));
    return mcp_common::MakeOkReply(Data);
}

// MCP:interchange_register_factory - Load a factory class and register it with the Interchange manager. Idempotent: a class already registered is a no-op.
//   @param factory_class_path [string!] - Class path of the factory UClass to instantiate and register (e.g. "/Script/InterchangeEngine.InterchangeTextureFactory"), loaded via LoadClass.
//   @returns { factory_class_path: string, class: string, registered: bool, note: string }
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

    UInterchangeManager& M = UInterchangeManager::GetInterchangeManager();
    bool Registered = M.RegisterFactory(Cls);

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("factory_class_path"), ClassPath);
    Data->SetStringField(TEXT("class"), Cls->GetName());
    Data->SetBoolField(TEXT("registered"), Registered);
    Data->SetStringField(TEXT("note"), TEXT("RegisterFactory dedups internally; re-running returns true with no duplicate"));
    return mcp_common::MakeOkReply(Data);
}

// Pipeline asset authoring: create pipeline / reflection set+get / save

// MCP:interchange_create_pipeline_asset - Create an Interchange pipeline asset of a concrete pipeline class (the generic texture, mesh, material, animation, audio, scenes, groom or assets pipelines, glTF, sparse volume texture, MaterialX). Idempotent: an object of that name already in package_path is returned with already_existed:true.
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
    Data->SetStringField(TEXT("note"), TEXT("Pipeline asset created; call interchange_save_pipeline to persist"));
    return mcp_common::MakeOkReply(Data);
}

// MCP:interchange_set_pipeline_properties - Set properties of an Interchange pipeline asset by reflection: properties is {name: value}, where a value can be a scalar, an object (a struct) or an array (a TArray).
//   @param pipeline_path [string!] - Object path of the pipeline asset to modify (loaded via LoadObject, must be a UInterchangePipelineBase).
//   @param properties [object!] - JSON object of { propertyName: value } pairs applied by reflection to the pipeline's EditAnywhere UPROPERTYs. Values may be scalars, nested objects (structs), or arrays; non-editable or unknown fields are skipped and reported.
//   @returns { pipeline_path: string, applied_count: number, skipped_count: number, skipped: [string], note: string }
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
    Data->SetStringField(TEXT("note"), TEXT("Call interchange_save_pipeline to persist the new values"));
    return mcp_common::MakeOkReply(Data);
}

// MCP:interchange_get_pipeline_properties - Read the editable (EditAnywhere) properties of an Interchange pipeline asset by reflection, inherited ones included: each entry gives name, C++ type and, unless class_only is true, the current value as JSON (left out when it cannot be converted). The values are the in-memory ones, so edits made with interchange_set_pipeline_properties and not yet saved already show. Read-only: it writes nothing and needs no PIE. A path that does not load as a UInterchangePipelineBase is an error. Discover the names with class_only=true, write them with interchange_set_pipeline_properties and persist with interchange_save_pipeline; the pipelines an imported asset used: interchange_get_pipelines; a new pipeline asset: interchange_create_pipeline_asset.
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

// MCP:interchange_save_pipeline - Write the package of an Interchange pipeline asset (a UInterchangePipelineBase loaded by object path) over its .uasset on disk, dirty or not, through the shared asset save; saved false makes success false and save_error carries the measured cause. It is the persist step after interchange_create_pipeline_asset and interchange_set_pipeline_properties, which change memory only; repeating it is idempotent. Like save_asset it refuses during PIE and under a read-only root. A path that does not load as a pipeline is an error.
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
    return mcp_common::MakeSavedReply(Data, bSaved, TEXT("interchange_save_pipeline"), SaveError);
}

// Headless imports via UInterchangeManager (medium risk: game-thread)

// MCP:interchange_can_translate_source_data - Whether a registered Interchange translator can read a source file (a path on disk or an asset path). Read-only.
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

// MCP:interchange_import_asset - Import a source file synchronously through the Interchange manager: unattended, no dialogs, replacing existing assets unless replace_existing is false. When the InterchangeImport plugin is not loaded the note says so.
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

// MCP:interchange_reimport_asset - Reimport an existing asset from its stored UInterchangeAssetImportData (UInterchangeManager::ReimportAsset). An asset without Interchange import data (a legacy factory import, or one never imported) gets an error.
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
    { TEXT("interchange_can_translate_source_data"), &HandleInterchangeCanTranslateSourceData },
    { TEXT("interchange_create_pipeline_asset"), &HandleInterchangeCreatePipelineAsset },
    { TEXT("interchange_get_asset_import_data"), &HandleInterchangeGetAssetImportData },
    { TEXT("interchange_get_node_container"), &HandleInterchangeGetNodeContainer },
    { TEXT("interchange_get_pipeline_properties"), &HandleInterchangeGetPipelineProperties },
    { TEXT("interchange_get_pipelines"), &HandleInterchangeGetPipelines },
    { TEXT("interchange_get_project_settings"), &HandleInterchangeGetProjectSettings },
    { TEXT("interchange_get_supported_formats"), &HandleInterchangeGetSupportedFormats },
    { TEXT("interchange_import_asset"), &HandleInterchangeImportAsset },
    { TEXT("interchange_module_status"), &HandleInterchangeModuleStatus },
    { TEXT("interchange_register_factory"), &HandleInterchangeRegisterFactory },
    { TEXT("interchange_register_translator"), &HandleInterchangeRegisterTranslator },
    { TEXT("interchange_reimport_asset"), &HandleInterchangeReimportAsset },
    { TEXT("interchange_save_pipeline"), &HandleInterchangeSavePipeline },
    { TEXT("interchange_set_pipeline_properties"), &HandleInterchangeSetPipelineProperties },
    { TEXT("interchange_set_pipeline_stack_default"), &HandleInterchangeSetPipelineStackDefault },
    { TEXT("interchange_set_pipelines"), &HandleInterchangeSetPipelines },
};
} // namespace UEAgentInterchangeCommands
