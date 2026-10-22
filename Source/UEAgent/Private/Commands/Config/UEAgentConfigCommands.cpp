// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/LightComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialEditingLibrary.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyPortFlags.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Blueprint/BlueprintSupport.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h" // read component TEMPLATE off a BP's SCS
#include "Engine/SCS_Node.h"                  // USCS_Node::ComponentTemplate
#include "Engine/Selection.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"                  // IFileManager (FindFiles/DirectoryExists) for diff_project_config
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "EditorAssetLibrary.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

namespace UEAgentConfigCommands
{

// MCP-DOMAIN: Config

namespace UEAgentConfigImpl
{
    // Find an actor in the given world by object name first, then by editor label.
    AActor* FindActorByNameOrLabel(UWorld* World, const FString& NameOrLabel)
    {
        if (!World)
        {
            return nullptr;
        }
        AActor* ByLabel = nullptr;
        for (TActorIterator<AActor> Cursor(World); Cursor; ++Cursor)
        {
            AActor* const Candidate = *Cursor;
            if (!Candidate)
            {
                continue;
            }
            if (Candidate->GetName() == NameOrLabel)
            {
                return Candidate; // exact object-name match wins
            }
            if (!ByLabel && Candidate->GetActorLabel(false) == NameOrLabel)
            {
                ByLabel = Candidate;
            }
        }
        return ByLabel;
    }

    // The component of Actor whose object name is ComponentName, or null.
    UActorComponent* FindNamedComponent(AActor* Actor, const FString& ComponentName)
    {
        if (!Actor)
        {
            return nullptr;
        }
        for (UActorComponent* const Candidate : Actor->GetComponents())
        {
            if (Candidate && Candidate->GetName() == ComponentName)
            {
                return Candidate;
            }
        }
        return nullptr;
    }

    // Text form of a property's value inside Container, as the details panel prints it.
    FString ExportPropertyText(const FProperty& Property, UObject* Container)
    {
        FString Text;
        Property.ExportTextItem_Direct(Text, Property.ContainerPtrToValuePtr<void>(Container), nullptr, nullptr, PPF_PropertyWindow);
        return Text;
    }

    // Property flags reported in each entry's "flags" array, in this order.
    struct FReportedPropertyFlag
    {
        EPropertyFlags Flag;
        const TCHAR* Label;
    };
    const FReportedPropertyFlag ReportedPropertyFlags[] =
    {
        { CPF_Edit,                  TEXT("Edit") },
        { CPF_BlueprintVisible,      TEXT("BlueprintVisible") },
        { CPF_Transient,             TEXT("Transient") },
        { CPF_Config,                TEXT("Config") },
        { CPF_GlobalConfig,          TEXT("GlobalConfig") },
        { CPF_ExportObject,          TEXT("ExportObject") },
        { CPF_NoClear,               TEXT("NoClear") },
        { CPF_EditConst,             TEXT("EditConst") },
        { CPF_Parm,                  TEXT("Parm") },
        { CPF_OutParm,               TEXT("OutParm") },
        { CPF_ZeroConstructor,       TEXT("ZeroConstructor") },
        { CPF_ReturnParm,            TEXT("ReturnParm") },
        { CPF_DisableEditOnTemplate, TEXT("DisableEditOnTemplate") },
    };

    // Metadata copied into each entry: JSON field name, then the metadata key it comes from.
    // A key the property does not define yields an empty string.
    struct FReportedPropertyMeta
    {
        const TCHAR* Field;
        const TCHAR* Key;
    };
    const FReportedPropertyMeta ReportedPropertyMeta[] =
    {
        { TEXT("tooltip"),      TEXT("Tooltip") },
        { TEXT("category"),     TEXT("Category") },
        { TEXT("display_name"), TEXT("DisplayName") },
        { TEXT("units"),        TEXT("Units") },
        { TEXT("clamp_min"),    TEXT("ClampMin") },
        { TEXT("clamp_max"),    TEXT("ClampMax") },
        { TEXT("ui_min"),       TEXT("UIMin") },
        { TEXT("ui_max"),       TEXT("UIMax") },
    };

    // One element of get_component_properties' "properties": identity, current value,
    // reported flags and metadata of a property read on Owner.
    TSharedRef<FJsonObject> DescribeProperty(const FProperty& Property, UObject* Owner)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Property.GetName());
        Entry->SetStringField(TEXT("type"), Property.GetClass()->GetName());
        Entry->SetBoolField(TEXT("is_editable"), Property.HasAnyPropertyFlags(CPF_Edit));
        Entry->SetBoolField(TEXT("is_blueprint_visible"), Property.HasAnyPropertyFlags(CPF_BlueprintVisible));
        Entry->SetStringField(TEXT("value"), ExportPropertyText(Property, Owner));

        TArray<TSharedPtr<FJsonValue>> FlagLabels;
        for (const FReportedPropertyFlag& Reported : ReportedPropertyFlags)
        {
            if (Property.HasAnyPropertyFlags(Reported.Flag))
            {
                FlagLabels.Add(MakeShared<FJsonValueString>(Reported.Label));
            }
        }
        Entry->SetArrayField(TEXT("flags"), FlagLabels);

        for (const FReportedPropertyMeta& Meta : ReportedPropertyMeta)
        {
            Entry->SetStringField(Meta.Field, Property.GetMetaData(Meta.Key));
        }
        return Entry;
    }

    // How a component came to exist; creation methods without a label are "Unknown".
    const TCHAR* CreationMethodLabel(const EComponentCreationMethod Method)
    {
        switch (Method)
        {
            case EComponentCreationMethod::Native:                   return TEXT("Native");
            case EComponentCreationMethod::SimpleConstructionScript: return TEXT("SimpleConstructionScript");
            case EComponentCreationMethod::UserConstructionScript:   return TEXT("UserConstructionScript");
            case EComponentCreationMethod::Instance:                 return TEXT("Instance");
            default:                                                 break;
        }
        return TEXT("Unknown");
    }

    // ------------------------------------------------------------------
    // USER CONSTRUCTION SCRIPT (UCS) helpers.
    //
    // AActor::RerunConstructionScripts() is ENGINE_API, public and WITH_EDITOR, but it is
    // NOT a UFUNCTION -- so it is unreachable from reflection and from execute_python. That
    // is why set_component_property could report value_changed:true on a RelativeScale3D the
    // UCS silently rewrites the instant anything re-runs it, and why rerun_construction_scripts
    // exists at all.
    //
    // DANGER, and the reason every path below re-resolves BY NAME: the rerun DESTROYS every
    // component the construction script generated and builds new ones. Any UActorComponent*
    // (and any FProperty* taken off its class) captured before the call is a dangling pointer
    // afterwards -- dereferencing it is a use-after-free that takes the editor down.
    // ------------------------------------------------------------------

    // Snapshot the component names of an actor (used to diff what the UCS destroyed/created).
    void UCSCollectComponentNames(AActor* Actor, TArray<FString>& OutNames)
    {
        OutNames.Reset();
        if (!Actor)
        {
            return;
        }
        for (UActorComponent* const Candidate : Actor->GetComponents())
        {
            if (Candidate)
            {
                OutNames.Add(Candidate->GetName());
            }
        }
        OutNames.Sort();
    }
}

// MCP:get_component_properties - Read EVERY editable or Blueprint-visible property of one component, each value as text, plus whether the component is active and registered: the read-back for set_component_property. Pick it to see a whole component, on a placed actor (actor_name; world picks the editor instance or a live PIE world) or on a Blueprint's component template by its SCS variable or native subobject name (blueprint_name); get_properties reads only the named properties of a placed actor's component (object_path "Actor.Component"), typed instead of text, and get_blueprint_component_property reads a Blueprint template property's effective default, resolving components inherited from parent Blueprints. Read-only.
//   @retired get_object_info - object_path="<actor>.<component>" (world=pie for a live instance), or a Blueprint component template's path from get_blueprint_component_object_path
//   @param actor_name - Object name, else editor label, of the placed actor that owns the component; ignored when blueprint_name is given.
//   @param component_name - Name of the component: its object name on a placed actor, or its SCS variable or native subobject name in a Blueprint.
//   @param world - Optional world selector: absent/'editor' (default) reads the frozen editor archetype/CDO (pre-BeginPlay defaults); 'server' (the PIE world with authority -- what you want for replicated state in co-op) | 'client' | 'pie:N' | 'pie' (ambiguous) resolve the LIVE running PIE actor+component so runtime-mutated values (e.g. Health after BeginPlay) are returned; 'game' is an alias of 'pie'. Only affects the actor_name path; ignored for blueprint_name.
//   @param blueprint_name - Blueprint whose component template to read, by short name (searched under /Game/Blueprints, then the whole project) or asset path such as "/Game/Characters/BP_Hero"; it wins over actor_name, and world is ignored.
//   @returns { actor_name: string, component_name: string, component_class: string, properties: [{...}], properties_count: int, editable_properties_count: int, blueprint_visible_properties_count: int, public_properties_count: int, component_owner: string, component_is_active: bool, component_is_registered: bool, component_creation_method: string, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetComponentProperties(const TSharedPtr<FJsonObject>& Params)
{
    // component_name is mandatory. The component comes either from a placed level actor
    // (actor_name) or from a Blueprint's component TEMPLATE (blueprint_name).
    FString HostActor;
    FString HostBlueprint;
    FString Wanted;
    Params->TryGetStringField(TEXT("actor_name"), HostActor);
    Params->TryGetStringField(TEXT("blueprint_name"), HostBlueprint);
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), Wanted, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    if (Wanted.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Parameter 'component_name' cannot be empty"));
    }
    if (HostActor.IsEmpty() && HostBlueprint.IsEmpty())
    {
        return mcp_common::MakeErrorReply(TEXT("Provide exactly one of 'actor_name' (a level actor) or 'blueprint_name' (a BP SCS/CDO component template)"));
    }

    UActorComponent* Component = nullptr;
    // The world the level-actor path resolved (stays null on the blueprint_name path,
    // which reads an asset template and touches no world at all -- and the evidence says so).
    UWorld* ResolvedWorld = nullptr;
    FString ResolvedWorldMode;

    if (!HostBlueprint.IsEmpty())
    {
        // Template path: the SCS node's component template (the _GEN_VARIABLE archetype), or
        // else a native CreateDefaultSubobject component of the generated-class CDO. Everything
        // after this block reads through Component, so it serves templates and live components
        // alike.
        UBlueprint* const Blueprint = mcp_common::ResolveBlueprint(HostBlueprint);
        if (Blueprint == nullptr)
        {
            return mcp_common::BlueprintNotFoundReply(HostBlueprint);
        }
        if (USimpleConstructionScript* const Scs = Blueprint->SimpleConstructionScript)
        {
            if (USCS_Node* const Node = Scs->FindSCSNode(FName(*Wanted)))
            {
                Component = Node->ComponentTemplate;
            }
        }
        if (Component == nullptr && Blueprint->GeneratedClass)
        {
            AActor* const Defaults = Cast<AActor>(Blueprint->GeneratedClass->GetDefaultObject());
            Component = UEAgentConfigImpl::FindNamedComponent(Defaults, Wanted);
        }
        if (Component == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Component '%s' not found in Blueprint '%s' (searched SCS nodes and native CDO subobjects)"),
                *Wanted, *HostBlueprint));
        }
    }
    else
    {
        // Level-actor path, through the canonical world resolver: absent/'editor' reads the
        // frozen editor instance, 'pie' the ambiguous live PIE actor, 'server'/'client'/'pie:N'
        // the addressed one.
        FString WorldMode, WorldError;
        UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), WorldMode, WorldError);
        if (!World)
        {
            return mcp_common::MakeErrorReply(WorldError.IsEmpty() ? TEXT("No world found") : WorldError);
        }
        ResolvedWorld = World;
        ResolvedWorldMode = WorldMode;

        // The live actor: object name first, editor label as the fallback.
        AActor* const Host = UEAgentConfigImpl::FindActorByNameOrLabel(World, HostActor);
        if (Host == nullptr)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Actor not found: %s (world='%s')"), *HostActor, *WorldMode));
        }
        Component = UEAgentConfigImpl::FindNamedComponent(Host, Wanted);
        if (Component == nullptr)
        {
            const FString Missing = FString::Printf(TEXT("Component not found: %s"), *Wanted);
            return mcp_common::MakeErrorReply(Missing);
        }
    }

    // Every property the details panel or Blueprints can see, in class-iteration order.
    TArray<TSharedPtr<FJsonValue>> Listed;
    int32 EditableCount = 0;
    int32 BlueprintVisibleCount = 0;
    int32 PublicCount = 0;
    for (TFieldIterator<FProperty> Field(Component->GetClass()); Field; ++Field)
    {
        const FProperty* const Property = *Field;
        if (Property == nullptr || !Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
        {
            continue;
        }
        Listed.Add(MakeShared<FJsonValueObject>(UEAgentConfigImpl::DescribeProperty(*Property, Component)));
        if (Property->HasAnyPropertyFlags(CPF_Edit))
        {
            ++EditableCount;
        }
        if (Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
        {
            ++BlueprintVisibleCount;
        }
        // Public: a native `public:` member (the access specifier UHT records) that a Blueprint has
        // not marked private.
        if (Property->HasAnyPropertyFlags(CPF_NativeAccessSpecifierPublic) && !Property->GetBoolMetaData(TEXT("BlueprintPrivate")))
        {
            ++PublicCount;
        }
    }

    const AActor* const Owner = Component->GetOwner();

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), HostActor);
    Report->SetStringField(TEXT("blueprint_name"), HostBlueprint); // empty on the actor path
    Report->SetStringField(TEXT("component_name"), Wanted);
    Report->SetStringField(TEXT("component_class"), Component->GetClass()->GetName());
    Report->SetArrayField(TEXT("properties"), Listed);
    Report->SetNumberField(TEXT("properties_count"), Listed.Num());
    Report->SetNumberField(TEXT("editable_properties_count"), EditableCount);
    Report->SetNumberField(TEXT("blueprint_visible_properties_count"), BlueprintVisibleCount);
    Report->SetNumberField(TEXT("public_properties_count"), PublicCount);
    Report->SetStringField(TEXT("component_owner"), Owner != nullptr ? Owner->GetName() : FString(TEXT("None")));
    Report->SetBoolField(TEXT("component_is_active"), Component->IsActive());
    Report->SetBoolField(TEXT("component_is_registered"), Component->IsRegistered());
    Report->SetStringField(TEXT("component_creation_method"), UEAgentConfigImpl::CreationMethodLabel(Component->CreationMethod));

    // WHICH world the component was read from. On the blueprint_name path no world is
    // involved and the five fields come back null -- that is the honest answer, not a gap.
    Report->SetStringField(TEXT("world"), ResolvedWorldMode.IsEmpty() ? TEXT("none (blueprint template)") : ResolvedWorldMode);
    mcp_common::AddWorldEvidence(Report, ResolvedWorld);

    return Report;
}

// MCP:set_component_property - Set ONE property on a component of a placed level actor, in the editor world by default or in a live PIE world (world='server' | 'client' | 'pie:N'). Pick it over set_properties when you need the running PIE actor, a property set_properties' edit gate refuses (no CPF_Edit gate here: BlueprintReadWrite-only and DisableEditOnInstance properties are reachable), or proof the value survives the construction script (verify_persistence=true); for a Blueprint's component template use set_blueprint_component_properties. It sends no PreEditChange/PostEditChange, so a component that reacts to Details-panel edits is better written with set_properties. actor_name is the OBJECT name, not the label. The reply returns old_value and final_value read back and stamps which world was written (world_package carries the UEDPIE_N prefix). WARNING: the default editor-world write does NOT reach an actor already running in PIE -- it lands in the next PIE session. Not saved: save_level keeps an editor-world change.
//   @param actor_name - Name of the actor containing the component. Matched against the actor's OBJECT name (GetName) only -- not its editor label.
//   @param component_name - Name of the component
//   @param property_name - Name of the property to set
//   @param property_value - New value. Any JSON type: a bare string (e.g. "/Game/Foo.Foo" object path, "Movable" enum name, "A.B.C" gameplay tag, "(X=1,Y=2,Z=3)" struct text), a number/bool, or nested JSON (a struct object, an array for array/set/tag-container props, an object for map props). Strings still import via ImportText_Direct so prior callers keep working.
//   @param world - Optional world selector: absent/'editor' is the DEFAULT -- it writes the editor world even with PIE running, which is what persists into the next PIE and what makes CPF_DisableEditOnInstance reachable; 'server' (the live PIE world with authority) | 'client' | 'pie:N' write the RUNNING actor instead; 'pie' is the ambiguous resolution and 'game' its alias. An unrecognised value FAILS with a message naming what was asked for and listing the worlds that exist -- it never falls back to another world in silence.
//   @param verify_persistence - Optional bool, default false. When true, run AActor::RerunConstructionScripts() and re-read the property to find out whether the value SURVIVES the User Construction Script. Without this, value_changed:true is measured BEFORE the UCS runs, so a property the UCS recomputes (the classic case: RelativeScale3D on a spline/box the UCS rescales) reports as changed and is silently reverted a moment later. Costs a full component rebuild of that actor, and it re-runs in whichever world you selected.
//   @returns { actor_name: string, component_name: string, property_name: string, property_type: string (FProperty class name), old_value: string, new_value: string (== final applied value), final_value: string (real value read back off the component), success: bool, value_changed: bool, component_class: string, component_is_active: bool, component_is_registered: bool, world: string (the selector actually used), world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }. With verify_persistence=true it also returns { construction_scripts_rerun: bool, has_non_trivial_user_construction_script: bool, post_construction_value: string, survives_construction_script: bool, persistence_note: string, verification: object, verification_checks: [object] }. On failure returns { success:false, error: <precise reason from the importer, or from the world resolver> }.

TSharedPtr<FJsonObject> HandleSetComponentProperty(const TSharedPtr<FJsonObject>& Params)
{
    // Three names locate the property; each one is mandatory.
    FString TargetActor;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), TargetActor, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetComponent;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("component_name"), TargetComponent, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString TargetProperty;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("property_name"), TargetProperty, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    // property_value is taken as a RAW JsonValue (not a pre-stringified string) so numbers, bools,
    // arrays and objects reach the canonical importer intact. A bare JSON string still works
    // (ImportText_Direct handles object paths, enum names and struct text). The guard on the
    // field itself makes the catalog publish it as required (of any JSON type).
    if (!Params->HasField(TEXT("property_value")))
    {
        return mcp_common::MakeErrorReply(TEXT("Missing 'property_value' parameter"));
    }
    TSharedPtr<FJsonValue> ValJson = Params->TryGetField(TEXT("property_value"));

    // WORLD SELECTOR. The DEFAULT is the editor world, ON PURPOSE: it is the only route to a
    // CPF_DisableEditOnInstance property (Python's set_editor_property rejects those), and because
    // the value sits in the editor world the NEXT PIE already starts with it. 'world' adds
    // 'server'/'client'/'pie:N'/'pie' and the EVIDENCE of where the write landed; an unrecognised
    // selector returns nullptr + an error naming the worlds that exist instead of quietly falling
    // back. 'editor' is GetEditorWorld() (GEditor's own edit context, asked for BY NAME), so an
    // empty context array is never read out of bounds.
    FString WorldMode, WorldError;
    UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), WorldMode, WorldError);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty() ? TEXT("No world found") : WorldError);
    }

    // The actor is matched by OBJECT name only, on purpose: adding the label fallback would
    // change what existing callers resolve to.
    AActor* Actor = mcp_common::FindActorByObjectName(World, TargetActor);
    if (Actor == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor not found: %s (world='%s')"), *TargetActor, *WorldMode));
    }

    UActorComponent* Component = UEAgentConfigImpl::FindNamedComponent(Actor, TargetComponent);
    if (Component == nullptr)
    {
        const FString Missing = FString::Printf(TEXT("Component not found: %s"), *TargetComponent);
        return mcp_common::MakeErrorReply(Missing);
    }

    // The FProperty is looked up here as well (SetObjectProperty does its own lookup) to export
    // the before/after text and to report the real property type. There is deliberately no
    // CPF_Edit gate: the canonical route does not impose one, and BlueprintReadWrite-only
    // properties are legitimate targets.
    FProperty* Property = Component->GetClass()->FindPropertyByName(FName(*TargetProperty));
    if (Property == nullptr)
    {
        const FString Missing = FString::Printf(TEXT("Property not found: %s"), *TargetProperty);
        return mcp_common::MakeErrorReply(Missing);
    }
    const FString PropertyType = Property->GetClass()->GetName();
    const FString OldValue = UEAgentConfigImpl::ExportPropertyText(*Property, Component);

    // The write goes through the single canonical JSON->property route (the same one
    // set_blueprint_component_properties uses): object/soft/class references from "/Game/..."
    // paths, enums by name, registry-validated gameplay tags, nested structs, arrays, sets and
    // maps. Modify() first, so the component's package (OFPA external actor packages included)
    // is dirtied and save_level persists the change.
    Component->Modify();
    FString SetErr;
    if (!mcp_common::SetObjectProperty(Component, TargetProperty, ValJson, SetErr))
    {
        return mcp_common::MakeErrorReply(SetErr);
    }

    // Read straight back off the component; new_value and final_value both carry this reading.
    const FString FinalValue = UEAgentConfigImpl::ExportPropertyText(*Property, Component);
    const bool bValueChanged = (FinalValue != OldValue);

    TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetStringField(TEXT("actor_name"), TargetActor);
    Report->SetStringField(TEXT("component_name"), TargetComponent);
    Report->SetStringField(TEXT("property_name"), TargetProperty);
    Report->SetStringField(TEXT("property_type"), PropertyType);
    Report->SetStringField(TEXT("old_value"), OldValue);
    Report->SetStringField(TEXT("new_value"), FinalValue);
    Report->SetStringField(TEXT("final_value"), FinalValue);
    // This `true` is not a hardcoded optimism -- every failure path above returns
    // MakeErrorReply and never reaches here, and final_value was read back off the live
    // component. What it does NOT claim is durability: that answer is value_changed (did the
    // write move anything) and, with verify_persistence, survives_construction_script.
    Report->SetBoolField(TEXT("success"), true);
    Report->SetBoolField(TEXT("value_changed"), bValueChanged);
    Report->SetStringField(TEXT("component_class"), Component->GetClass()->GetName());
    Report->SetBoolField(TEXT("component_is_active"), Component->IsActive());
    Report->SetBoolField(TEXT("component_is_registered"), Component->IsRegistered());

    // WHICH world was written. Without this field
    // `success:true` read identically whether the value landed in the editor instance or in
    // UEDPIE_1. Stamped HERE, before the verify_persistence block, so every early return below
    // carries it too. world_package is the one that matters -- in World Partition the UWorld keeps
    // the map's name and only the package carries UEDPIE_N.
    Report->SetStringField(TEXT("world"), WorldMode);
    mcp_common::AddWorldEvidence(Report, World);

    // ------------------------------------------------------------------
    // verify_persistence -- does the value SURVIVE the User Construction Script?
    //
    // Everything above is read BEFORE the UCS runs, which is why the tool could answer
    // value_changed:true about a RelativeScale3D the UCS rewrites to its own computed value
    // the instant the actor is reconstructed (e.g. for a box component sized by a spline-driven
    // UCS, BoxExtent and the spline persist, RelativeScale3D does NOT).
    //
    // OPT-IN (default false) because the rerun rebuilds every constructed component of the
    // actor, which is neither free nor side-effect-free.
    //
    // WARNING: from here on, `Component` and `Property` are treated as POISONED. The rerun destroys
    // UCS-generated components, so the only safe way back in is to re-resolve the actor AND
    // the component BY NAME and re-find the FProperty on the NEW component's class.
    // ------------------------------------------------------------------
    bool bVerifyPersistence = false;
    Params->TryGetBoolField(TEXT("verify_persistence"), bVerifyPersistence);
    if (!bVerifyPersistence)
    {
        return Report;
    }

    // Capture the identity keys we will re-resolve with, and the pre-rerun facts, NOW.
    const FString ActorObjectName = Actor->GetName();
    const FString ResolvedComponentName = Component->GetName();
    const bool bHasNonTrivialUCS = Actor->HasNonTrivialUserConstructionScript();
    Report->SetBoolField(TEXT("has_non_trivial_user_construction_script"), bHasNonTrivialUCS);

    // Component->Modify() above already dirtied the owning package (incl. the OFPA external
    // actor package), so RerunConstructionScripts' "restore clean packages" pass leaves it dirty.
    Actor->RerunConstructionScripts();
    Report->SetBoolField(TEXT("construction_scripts_rerun"), true);

    // POISONED pointers are never touched again -- re-resolve from the world by name.
    Component = nullptr;
    Property = nullptr;
    Actor = mcp_common::FindActorByObjectName(World, ActorObjectName);
    if (!Actor)
    {
        const FString Note = FString::Printf(
            TEXT("actor '%s' could not be re-resolved after RerunConstructionScripts; persistence unknown"),
            *ActorObjectName);
        Report->SetStringField(TEXT("persistence_note"), Note);
        mcp_common::AddVerificationEvidence(Report, mcp_common::EMcpVerification::NotVerifiable,
            TEXT("AActor::RerunConstructionScripts + re-read"), Note);
        return Report;
    }

    UActorComponent* PostComponent = UEAgentConfigImpl::FindNamedComponent(Actor, ResolvedComponentName);
    if (!PostComponent)
    {
        const FString Note = FString::Printf(
            TEXT("component '%s' no longer exists on '%s' after RerunConstructionScripts (the construction script regenerated it under a different name); persistence unknown"),
            *ResolvedComponentName, *ActorObjectName);
        Report->SetStringField(TEXT("persistence_note"), Note);
        mcp_common::AddVerificationEvidence(Report, mcp_common::EMcpVerification::NotVerifiable,
            TEXT("AActor::RerunConstructionScripts + re-read"), Note);
        return Report;
    }

    FProperty* PostProperty = PostComponent->GetClass()->FindPropertyByName(FName(*TargetProperty));
    if (!PostProperty)
    {
        const FString Note = FString::Printf(
            TEXT("property '%s' not found on the rebuilt component (class is now %s); persistence unknown"),
            *TargetProperty, *PostComponent->GetClass()->GetName());
        Report->SetStringField(TEXT("persistence_note"), Note);
        mcp_common::AddVerificationEvidence(Report, mcp_common::EMcpVerification::NotVerifiable,
            TEXT("AActor::RerunConstructionScripts + re-read"), Note);
        return Report;
    }

    const FString PostValue = UEAgentConfigImpl::ExportPropertyText(*PostProperty, PostComponent);
    const bool bSurvives = (PostValue == FinalValue);
    Report->SetStringField(TEXT("post_construction_value"), PostValue);
    Report->SetBoolField(TEXT("survives_construction_script"), bSurvives);
    const FString Note = bSurvives
        ? FString::Printf(TEXT("'%s' still reads %s after RerunConstructionScripts"), *TargetProperty, *PostValue)
        : FString::Printf(TEXT("the construction script overwrote '%s': set to %s, reads %s after the rerun -- this value does NOT persist, drive it from whatever the UCS computes it from instead"),
            *TargetProperty, *FinalValue, *PostValue);
    Report->SetStringField(TEXT("persistence_note"), Note);
    mcp_common::AddVerificationEvidence(Report,
        bSurvives ? mcp_common::EMcpVerification::Verified : mcp_common::EMcpVerification::Failed,
        TEXT("AActor::RerunConstructionScripts + re-read"), Note);
    // `success` stays true: the write itself DID happen and old_value/new_value are accurate.
    // Whether it survives is reported by survives_construction_script, not hidden in success.
    return Report;
}

// MCP:rerun_construction_scripts - Run AActor::RerunConstructionScripts() on a level actor: destroys every construction-script-generated component and rebuilds it, exactly as dragging the actor in the editor would. This is the ONLY way to reach that function from MCP -- it is ENGINE_API and public but NOT a UFUNCTION, so reflection, call_function and execute_python cannot see it. Use it to find out what a property edit really survives, or to re-apply a Blueprint's construction logic after editing the instance. Reports the before/after component sets instead of a bare success.
//   @param actor_name - Object name (or editor label) of the actor to reconstruct
//   @param world - Optional world selector: absent/'editor' (default) reconstructs the actor in the editor world; 'server' | 'client' | 'pie:N' | 'pie' (ambiguous) target the LIVE Play-In-Editor actor instead; 'game' is an alias of 'pie'. There is no other side effect.
//   @returns { actor_name: string, resolved_actor_name: string, actor_label: string, actor_class: string, world: string, has_non_trivial_user_construction_script: bool, actor_survived: bool, components_before: int, components_after: int, component_names_before: [string], component_names_after: [string], destroyed_component_names: [string], created_component_names: [string], verification: object, verification_checks: [object], world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }
TSharedPtr<FJsonObject> HandleRerunConstructionScripts(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor_name"), ActorName)) { return Missing; }

    FString WorldMode, WorldError;
    UWorld* World = mcp_common::ResolveWorldSelector(Params, TEXT("editor"), WorldMode, WorldError);
    if (!World)
    {
        return mcp_common::MakeErrorReply(WorldError.IsEmpty() ? TEXT("No editor world found") : WorldError);
    }

    AActor* Actor = UEAgentConfigImpl::FindActorByNameOrLabel(World, ActorName);
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor not found: %s (world='%s')"), *ActorName, *WorldMode));
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("actor_name"), ActorName);
    Data->SetStringField(TEXT("world"), WorldMode);
    // Reconstructing the PIE copy of an actor changes nothing that saves; WHICH world
    // was reconstructed is the difference between "it didn't work" and "wrong world".
    mcp_common::AddWorldEvidence(Data, World);

    // Capture identity + the "before" picture while the pointers are still alive.
    const FString ActorObjectName = Actor->GetName();
    const FString ActorLabel = Actor->GetActorLabel(false);
    const FString ActorClass = Actor->GetClass()->GetName();
    const bool bHasNonTrivialUCS = Actor->HasNonTrivialUserConstructionScript();
    TArray<FString> NamesBefore;
    UEAgentConfigImpl::UCSCollectComponentNames(Actor, NamesBefore);

    Data->SetStringField(TEXT("resolved_actor_name"), ActorObjectName);
    Data->SetStringField(TEXT("actor_label"), ActorLabel);
    Data->SetStringField(TEXT("actor_class"), ActorClass);
    Data->SetBoolField(TEXT("has_non_trivial_user_construction_script"), bHasNonTrivialUCS);
    Data->SetNumberField(TEXT("components_before"), NamesBefore.Num());
    Data->SetArrayField(TEXT("component_names_before"), mcp_common::StringsToJson(NamesBefore));

    Actor->RerunConstructionScripts();

    // POISONED: the rerun destroys constructed components, and a child actor's rerun is
    // redirected to its parent and can replace the actor itself. Re-resolve BY NAME.
    Actor = mcp_common::FindActorByObjectName(World, ActorObjectName);
    const bool bActorSurvived = (Actor != nullptr);
    Data->SetBoolField(TEXT("actor_survived"), bActorSurvived);

    if (!bActorSurvived)
    {
        const FString Detail = FString::Printf(
            TEXT("actor '%s' is no longer resolvable in the '%s' world after the rerun; the component diff cannot be reported"),
            *ActorObjectName, *WorldMode);
        Data->SetNumberField(TEXT("components_after"), 0);
        Data->SetArrayField(TEXT("component_names_after"), TArray<TSharedPtr<FJsonValue>>());
        return mcp_common::MakeVerifiedReply(Data, false, mcp_common::EMcpVerification::Failed,
            TEXT("re-resolve actor by name after RerunConstructionScripts"), Detail, Detail);
    }

    TArray<FString> NamesAfter;
    UEAgentConfigImpl::UCSCollectComponentNames(Actor, NamesAfter);
    Data->SetNumberField(TEXT("components_after"), NamesAfter.Num());
    Data->SetArrayField(TEXT("component_names_after"), mcp_common::StringsToJson(NamesAfter));

    TArray<FString> Destroyed, Created;
    for (const FString& N : NamesBefore) { if (!NamesAfter.Contains(N)) { Destroyed.Add(N); } }
    for (const FString& N : NamesAfter)  { if (!NamesBefore.Contains(N)) { Created.Add(N); } }
    Data->SetArrayField(TEXT("destroyed_component_names"), mcp_common::StringsToJson(Destroyed));
    Data->SetArrayField(TEXT("created_component_names"), mcp_common::StringsToJson(Created));

    // Honest note: RerunConstructionScripts has no return value and silently no-ops on a
    // non-Blueprint actor (UBlueprint::GetBlueprintHierarchyFromClass fails), on an actor
    // mid-construction, and while reruns are suspended. What we CAN prove is that the actor
    // is still there and what its component set looks like now -- so that is what we report.
    const FString Detail = FString::Printf(
        TEXT("actor re-resolved; components %d -> %d (%d destroyed, %d created)"),
        NamesBefore.Num(), NamesAfter.Num(), Destroyed.Num(), Created.Num());
    return mcp_common::MakeVerifiedReply(Data, true, mcp_common::EMcpVerification::Verified,
        TEXT("re-resolve actor by name after RerunConstructionScripts"), Detail);
}

// MCP:diff_project_config - Diff THIS project's Config/Default*.ini against ANOTHER project's, aligned by section+key (pure file read via FConfigFile::Read; no world, no editor state). Surfaces settings that differ or failed to migrate: custom collision channels (GameTraceChannel/ObjectChannel under [/Script/Engine.CollisionProfile]), input mappings, enabled plugins ([Plugins]/[/Script/...]), and every other key. Perspective is THIS -> OTHER: 'added' = present in OTHER but not THIS; 'removed' = present in THIS but not OTHER; 'changed' = present in both with different value(s). Multi-value keys (array-of-struct like +Channels/+ActionMappings) are compared as sorted value lists so hash-ordering never yields a false diff.
//   @param other_project_dir - Filesystem path to the OTHER project. Either the project ROOT (its 'Config' subfolder is used automatically) or a Config folder directly.
//   @param files - Optional array of ini base filenames to compare (e.g. ["DefaultEngine.ini","DefaultInput.ini"]). Default: the union of Default*.ini present in EITHER project's Config folder.
//   @param sections - Optional array of exact section names to restrict the diff to (e.g. ["/Script/Engine.CollisionProfile","/Script/EngineSettings.GameMapsSettings"]). Default: all sections.
//   @returns { this_project_dir: string, other_project_dir: string, this_config_dir: string, other_config_dir: string, files: [ { file: string, this_exists: bool, other_exists: bool, this_section_count: int, other_section_count: int, added: [{ section, key, values:[string] }], removed: [{ section, key, values:[string] }], changed: [{ section, key, this_values:[string], other_values:[string] }], added_count: int, removed_count: int, changed_count: int } ], total_added: int, total_removed: int, total_changed: int, identical: bool }

TSharedPtr<FJsonObject> HandleDiffProjectConfig(const TSharedPtr<FJsonObject>& Params)
{
    FString OtherProjectDir;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("other_project_dir"), OtherProjectDir)) { return Missing; }

    IFileManager& FileMgr = IFileManager::Get();

    // Resolve config dirs. THIS project = the running project's Config. OTHER = the provided path,
    // treated as a project root when it has a 'Config' subfolder, else as a Config folder directly.
    const FString ThisConfigDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir());
    FString OtherConfigDir;
    {
        const FString AsRoot = FPaths::Combine(OtherProjectDir, TEXT("Config"));
        OtherConfigDir = FileMgr.DirectoryExists(*AsRoot) ? AsRoot : OtherProjectDir;
        OtherConfigDir = FPaths::ConvertRelativePathToFull(OtherConfigDir);
    }
    if (!FileMgr.DirectoryExists(*OtherConfigDir))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Other project config dir not found: %s (pass the project root or its Config folder)"), *OtherConfigDir));
    }

    // Optional exact-name section filter.
    TSet<FString> SectionFilter;
    const TArray<TSharedPtr<FJsonValue>>* SectionsArr = nullptr;
    if (Params->TryGetArrayField(TEXT("sections"), SectionsArr) && SectionsArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *SectionsArr)
        {
            if (V.IsValid())
            {
                const FString S = V->AsString();
                if (!S.IsEmpty()) { SectionFilter.Add(S); }
            }
        }
    }
    const bool bHasSectionFilter = SectionFilter.Num() > 0;

    // File list: explicit 'files' (base filenames), else the union of Default*.ini in either dir.
    TArray<FString> FileNames;
    const TArray<TSharedPtr<FJsonValue>>* FilesArr = nullptr;
    if (Params->TryGetArrayField(TEXT("files"), FilesArr) && FilesArr)
    {
        for (const TSharedPtr<FJsonValue>& V : *FilesArr)
        {
            if (V.IsValid())
            {
                const FString S = V->AsString();
                if (!S.IsEmpty()) { FileNames.AddUnique(FPaths::GetCleanFilename(S)); }
            }
        }
    }
    else
    {
        TSet<FString> Names;
        TArray<FString> Found;
        FileMgr.FindFiles(Found, *FPaths::Combine(ThisConfigDir, TEXT("Default*.ini")), /*Files=*/true, /*Directories=*/false);
        for (const FString& F : Found) { Names.Add(FPaths::GetCleanFilename(F)); }
        Found.Reset();
        FileMgr.FindFiles(Found, *FPaths::Combine(OtherConfigDir, TEXT("Default*.ini")), /*Files=*/true, /*Directories=*/false);
        for (const FString& F : Found) { Names.Add(FPaths::GetCleanFilename(F)); }
        FileNames = Names.Array();
    }
    FileNames.Sort();

    // Load one ini file into section -> key -> (sorted) values. Sorting the per-key value lists
    // makes the later comparison order-independent (TMultiMap iteration order is hash-based, and
    // array-of-struct keys carry many values) and keeps the JSON output deterministic.
    auto LoadIni = [bHasSectionFilter, &SectionFilter](
        const FString& Path,
        TMap<FString, TMap<FString, TArray<FString>>>& OutSections,
        bool& bOutExists,
        int32& OutSectionCount)
    {
        bOutExists = FPaths::FileExists(Path);
        OutSectionCount = 0;
        if (!bOutExists) { return; }

        FConfigFile Cfg;
        Cfg.Read(Path); // single-file disk read; no hierarchy/platform merge

        TArray<FString> SectionNames;
        Cfg.GetKeys(SectionNames);
        OutSectionCount = SectionNames.Num();

        for (const FString& SectionName : SectionNames)
        {
            if (bHasSectionFilter && !SectionFilter.Contains(SectionName)) { continue; }
            const FConfigSection* Section = Cfg.FindSection(SectionName);
            if (!Section) { continue; }

            TMap<FString, TArray<FString>>& KV = OutSections.FindOrAdd(SectionName);
            for (const TPair<FName, FConfigValue>& Pair : *Section)
            {
                KV.FindOrAdd(Pair.Key.ToString()).Add(Pair.Value.GetSavedValue());
            }
            for (TPair<FString, TArray<FString>>& KvPair : KV)
            {
                KvPair.Value.Sort();
            }
        }
    };

    TArray<TSharedPtr<FJsonValue>> FilesJson;
    int32 TotalAdded = 0, TotalRemoved = 0, TotalChanged = 0;

    for (const FString& FileName : FileNames)
    {
        TMap<FString, TMap<FString, TArray<FString>>> ThisSecs, OtherSecs;
        bool bThisExists = false, bOtherExists = false;
        int32 ThisSecCount = 0, OtherSecCount = 0;
        LoadIni(FPaths::Combine(ThisConfigDir, FileName), ThisSecs, bThisExists, ThisSecCount);
        LoadIni(FPaths::Combine(OtherConfigDir, FileName), OtherSecs, bOtherExists, OtherSecCount);

        TArray<TSharedPtr<FJsonValue>> AddedArr, RemovedArr, ChangedArr;

        // Union of section names across both files.
        TSet<FString> AllSections;
        for (const TPair<FString, TMap<FString, TArray<FString>>>& P : ThisSecs) { AllSections.Add(P.Key); }
        for (const TPair<FString, TMap<FString, TArray<FString>>>& P : OtherSecs) { AllSections.Add(P.Key); }
        TArray<FString> SortedSections = AllSections.Array();
        SortedSections.Sort();

        for (const FString& SectionName : SortedSections)
        {
            const TMap<FString, TArray<FString>>* ThisKV = ThisSecs.Find(SectionName);
            const TMap<FString, TArray<FString>>* OtherKV = OtherSecs.Find(SectionName);

            // Union of keys within this section.
            TSet<FString> AllKeys;
            if (ThisKV) { for (const TPair<FString, TArray<FString>>& P : *ThisKV) { AllKeys.Add(P.Key); } }
            if (OtherKV) { for (const TPair<FString, TArray<FString>>& P : *OtherKV) { AllKeys.Add(P.Key); } }
            TArray<FString> SortedKeys = AllKeys.Array();
            SortedKeys.Sort();

            for (const FString& Key : SortedKeys)
            {
                const TArray<FString>* ThisVals = ThisKV ? ThisKV->Find(Key) : nullptr;
                const TArray<FString>* OtherVals = OtherKV ? OtherKV->Find(Key) : nullptr;

                if (ThisVals && !OtherVals)
                {
                    // In THIS, absent from OTHER -> removed.
                    TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                    E->SetStringField(TEXT("section"), SectionName);
                    E->SetStringField(TEXT("key"), Key);
                    E->SetArrayField(TEXT("values"), mcp_common::StringsToJson(*ThisVals));
                    RemovedArr.Add(MakeShared<FJsonValueObject>(E));
                }
                else if (!ThisVals && OtherVals)
                {
                    // In OTHER, absent from THIS -> added.
                    TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                    E->SetStringField(TEXT("section"), SectionName);
                    E->SetStringField(TEXT("key"), Key);
                    E->SetArrayField(TEXT("values"), mcp_common::StringsToJson(*OtherVals));
                    AddedArr.Add(MakeShared<FJsonValueObject>(E));
                }
                else if (ThisVals && OtherVals && (*ThisVals != *OtherVals))
                {
                    TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
                    E->SetStringField(TEXT("section"), SectionName);
                    E->SetStringField(TEXT("key"), Key);
                    E->SetArrayField(TEXT("this_values"), mcp_common::StringsToJson(*ThisVals));
                    E->SetArrayField(TEXT("other_values"), mcp_common::StringsToJson(*OtherVals));
                    ChangedArr.Add(MakeShared<FJsonValueObject>(E));
                }
            }
        }

        TotalAdded += AddedArr.Num();
        TotalRemoved += RemovedArr.Num();
        TotalChanged += ChangedArr.Num();

        TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
        FileObj->SetStringField(TEXT("file"), FileName);
        FileObj->SetBoolField(TEXT("this_exists"), bThisExists);
        FileObj->SetBoolField(TEXT("other_exists"), bOtherExists);
        FileObj->SetNumberField(TEXT("this_section_count"), ThisSecCount);
        FileObj->SetNumberField(TEXT("other_section_count"), OtherSecCount);
        FileObj->SetArrayField(TEXT("added"), AddedArr);
        FileObj->SetArrayField(TEXT("removed"), RemovedArr);
        FileObj->SetArrayField(TEXT("changed"), ChangedArr);
        FileObj->SetNumberField(TEXT("added_count"), AddedArr.Num());
        FileObj->SetNumberField(TEXT("removed_count"), RemovedArr.Num());
        FileObj->SetNumberField(TEXT("changed_count"), ChangedArr.Num());
        FilesJson.Add(MakeShared<FJsonValueObject>(FileObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("this_project_dir"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
    ResultObj->SetStringField(TEXT("other_project_dir"), OtherProjectDir);
    ResultObj->SetStringField(TEXT("this_config_dir"), ThisConfigDir);
    ResultObj->SetStringField(TEXT("other_config_dir"), OtherConfigDir);
    ResultObj->SetArrayField(TEXT("files"), FilesJson);
    ResultObj->SetNumberField(TEXT("total_added"), TotalAdded);
    ResultObj->SetNumberField(TEXT("total_removed"), TotalRemoved);
    ResultObj->SetNumberField(TEXT("total_changed"), TotalChanged);
    ResultObj->SetBoolField(TEXT("identical"), (TotalAdded + TotalRemoved + TotalChanged) == 0);
    return ResultObj;
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("diff_project_config"), &HandleDiffProjectConfig },
    { TEXT("get_component_properties"), &HandleGetComponentProperties },
    { TEXT("rerun_construction_scripts"), &HandleRerunConstructionScripts },
    { TEXT("set_component_property"), &HandleSetComponentProperty },
};
} // namespace UEAgentConfigCommands
