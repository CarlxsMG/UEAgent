// Copyright (c) 2026 CarlxsMG.

#include "Mcp/McpCommandFamilies.h"
#include "Commands/Common/McpCommonUtils.h"
#include "Commands/Common/UEAgentGraphUtils.h" // FUEAgentGraphUtils::ResolveClass (name / /Script / BP-asset resolver: call_function class_name / component_class)
#include "Commands/Subsystem/SubsystemLifetime.h" // get_class_hierarchy: lifetime of a subsystem class
#include "Commands/Asset/AssetCommandHelpers.h"
#include "Engine/Blueprint.h"                        // UBlueprint::GeneratedClass — a Blueprint asset path passed as a TSubclassOf argument must become its generated class
#include "Misc/Guid.h"                               // FGuid::Parse (call_function FGuid param marshaling)
#include "GameFramework/WorldSettings.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"
#include "UObject/StrProperty.h"
#include "UObject/Field.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "GameplayTagContainer.h" // FGameplayTag validation of single-tag params
#include "UObject/StructOnScope.h"
#include "Engine/World.h"
#include "Engine/Engine.h"          // GEngine, FWorldContext, GetWorldContexts (PIE-world resolve)
#include "EngineUtils.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h" // UGameplayStatics::GetPlayerPawn
#include "Engine/UserDefinedEnum.h" // UUserDefinedEnum (get_enum_info BP display names)
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/BlendSpace.h" // set_properties resamples a blend space after a write, as its editor does
#include "EdGraph/EdGraph.h"                    // set_properties finishes a graph node it wrote
#include "EdGraph/EdGraphNode.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Sound/SoundCue.h"                     // ...a sound cue whose node it wrote
#include "NiagaraSystem.h"                      // ...and a Niagara system whose renderer or stage it wrote
#include "Animation/AnimInstance.h" // FindSubobjectByName's "AnimInstance" alias returns one as UObject* — needs the complete type, which only the unity blob would supply
#include "Misc/PackageName.h"
#include "HAL/PlatformFilemanager.h"
#include "CoreGlobals.h" // GAllowActorScriptExecutionInEditor (call_function)
#include "ScopedTransaction.h" // `transact` opt-in on call_function (UnrealEd is already a module dep, UEAgent.Build.cs)
#include "UObject/Package.h"   // UPackage::IsDirty / ContainsMap — the OFPA half of list_instance_overrides and of the transact response

/**
 * Commands of the Reflection domain: what a class or an object IS (types, hierarchy, UPROPERTY
 * and UFUNCTION surfaces, structs, enums, packages), reading and writing property values, and
 * invoking UFUNCTIONs with JSON-marshalled arguments, in the editor world or a live PIE world.
 */
namespace UEAgentReflectionCommands
{

// MCP-DOMAIN: Reflection

// ---------------------------------------------------------------------------
// PIE-aware helper (the world='pie' path of the world-aware readers and of
// call_function). Forward-declared here so every command below can use it;
// defined near the bottom of this file.
//   - ReflectionPie_ResolveObjectInWorld: resolve a path/name/label to a live
//     object, first by full/asset path then by dotted Actor[.Component[.Sub]]
//     lookup within World.
static UObject* ReflectionPie_ResolveObjectInWorld(const FString& Path, UWorld* World);

// ---------------------------------------------------------------------------
// OFPA / transaction helpers shared by list_instance_overrides and by the
// `transact` opt-in of call_function.
// Defined further down in this translation unit.
//   - ReflectionOfpa_ResolveOwningPackage: the UPackage that actually PERSISTS an
//     object. For a World Partition (OFPA) actor that is its own external package
//     under __ExternalActors__, NOT the .umap; for a component it is its owner's.
//   - ReflectionOfpa_DescribePackage: that package as JSON (name, on-disk file,
//     whether the file exists, dirty flag, external-ness).
//   - ReflectionTransact_Refuse: preflight that REFUSES a transact request which
//     could only produce a false-positive dirty flag (pure getter / PIE target).
//   - ReflectionTransact_ModifyTargets: Modify() the object (and, for a component,
//     its owner) so the write lands in the transaction buffer AND dirties the package.
//   - ReflectionOverrides_*: the archetype-delta engine behind list_instance_overrides.
static UPackage* ReflectionOfpa_ResolveOwningPackage(const UObject* Object, bool& bOutIsExternal);
static void ReflectionOfpa_DescribePackage(const UObject* Object, const TSharedPtr<FJsonObject>& Out);
static bool ReflectionTransact_Refuse(const UObject* Target, const UFunction* Function, FString& OutError);
static void ReflectionTransact_ModifyTargets(UObject* Target, TArray<TSharedPtr<FJsonValue>>& OutModified, bool& bOutAnySavedToBuffer);
static mcp_common::EMcpVerification ReflectionTransact_Report(
    UObject* Target,
    bool bDirtyBefore,
    bool bAnySavedToBuffer,
    const TArray<TSharedPtr<FJsonValue>>& Modified,
    const TSharedPtr<FJsonObject>& OutTransaction,
    FString& OutDetail);
static const TCHAR* ReflectionOverrides_CreationMethodToString(EComponentCreationMethod Method);

// ---------------------------------------------------------------------------
// File-local helpers of the inspection / property / invocation handlers below.
// ---------------------------------------------------------------------------
namespace UEAgentReflectionCommandsImpl
{
    /** Editor-side resolver of a handler (ResolveObjectByName), handed in by the caller. */
    using FEditorObjectLookup = TFunctionRef<UObject*(const FString&)>;

    /** True when the optional `world` selector names a live world (anything but absent/'editor'). */
    bool IsLiveWorldSelector(FString Selector)
    {
        Selector.TrimStartAndEndInline();
        return !Selector.IsEmpty() && !Selector.Equals(TEXT("editor"), ESearchCase::IgnoreCase);
    }

    /** Which world a handler reads: the editor/asset space, or one live (PIE) world. */
    struct FWorldChoice
    {
        bool bLive = false;
        UWorld* World = nullptr;        // set only for a live world
        FString Label = TEXT("editor"); // reported back as "world"
    };

    /**
     * Reads the optional `world` selector. Absent or 'editor' keeps the editor resolution (an absent
     * one means the live 'pie' world instead when bLiveWhenAbsent); any other value must name a live
     * world that exists, and an unknown one is REFUSED (the error lists the worlds that do exist)
     * instead of quietly answering from another world.
     * Returns false with OutError set when the selector cannot be honoured.
     */
    bool ChooseWorld(const TSharedPtr<FJsonObject>& Params, const TCHAR* CommandName, FWorldChoice& Out,
        TSharedPtr<FJsonObject>& OutError, bool bLiveWhenAbsent = false)
    {
        FString Selector;
        Params->TryGetStringField(TEXT("world"), Selector);
        Out.bLive = Selector.TrimStartAndEnd().IsEmpty() ? bLiveWhenAbsent : IsLiveWorldSelector(Selector);
        if (!Out.bLive)
        {
            return true;
        }
        FString WorldError;
        Out.World = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), Out.Label, WorldError);
        if (!Out.World)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("%s: %s"), CommandName, *WorldError));
            return false;
        }
        return true;
    }

    /** One path resolved in the chosen world: inside the live world, or through the editor lookup. */
    UObject* ResolveInChoice(const FWorldChoice& Choice, const FString& Path, FEditorObjectLookup EditorLookup)
    {
        return Choice.bLive ? ReflectionPie_ResolveObjectInWorld(Path, Choice.World) : EditorLookup(Path);
    }

    /** Target of a world-aware single-object handler, with the world that answered. */
    struct FWorldScopedObject
    {
        UObject* Object = nullptr;
        UWorld* World = nullptr;
        FString WorldLabel = TEXT("editor");
    };

    /**
     * World choice + object resolution + "not found" error of the single-object readers.
     * Out.World is the live world when one was chosen, otherwise the object's OWN world (null
     * for an asset or a CDO: nothing world-scoped was read). Returns false with OutError set
     * when the world or the object cannot be resolved. bLiveWhenAbsent is ChooseWorld's.
     */
    bool ResolveWorldScopedObject(const TSharedPtr<FJsonObject>& Params, const FString& ObjectPath,
        const TCHAR* CommandName, FEditorObjectLookup EditorLookup, FWorldScopedObject& Out, TSharedPtr<FJsonObject>& OutError,
        bool bLiveWhenAbsent = false)
    {
        FWorldChoice Choice;
        if (!ChooseWorld(Params, CommandName, Choice, OutError, bLiveWhenAbsent))
        {
            return false;
        }
        Out.WorldLabel = Choice.Label;
        Out.Object = ResolveInChoice(Choice, ObjectPath, EditorLookup);
        if (!Out.Object)
        {
            OutError = mcp_common::MakeErrorReply(FString::Printf(TEXT("Object not found: %s (world='%s')"), *ObjectPath, *Out.WorldLabel));
            return false;
        }
        Out.World = Choice.World ? Choice.World : Out.Object->GetWorld();
        return true;
    }

    /** {name, type} of one function parameter; type is the FProperty class name. */
    TSharedPtr<FJsonValue> ParameterEntry(const FProperty* Param)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Param->GetName());
        Entry->SetStringField(TEXT("type"), Param->GetClass()->GetName());
        return MakeShared<FJsonValueObject>(Entry);
    }

    /**
     * get_class_functions' parameter list: the function's LEADING CPF_Parm properties, stopping
     * at the first property that is not a parameter. The return value is a CPF_Parm too, so it
     * is listed here (get_function_info is the one that separates it).
     */
    TArray<TSharedPtr<FJsonValue>> LeadingParameterEntries(UFunction* Function)
    {
        TArray<TSharedPtr<FJsonValue>> Entries;
        for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
        {
            if (!ParamIt->HasAnyPropertyFlags(CPF_Parm))
            {
                break;
            }
            Entries.Add(ParameterEntry(*ParamIt));
        }
        return Entries;
    }

    /**
     * Call-relevant flags and metadata of a UFUNCTION, shared by get_class_functions and
     * get_function_info. BlueprintPure is a real function flag; CallInEditor is UFUNCTION
     * METADATA (the editor's own asset utilities read it with HasMetaData), hence the
     * WITH_METADATA block.
     */
    void WriteFunctionTraits(const UFunction* Function, const TSharedPtr<FJsonObject>& Out)
    {
        Out->SetBoolField(TEXT("is_blueprint_callable"), Function->HasAnyFunctionFlags(FUNC_BlueprintCallable));
        Out->SetBoolField(TEXT("is_blueprint_event"), Function->HasAnyFunctionFlags(FUNC_BlueprintEvent));
        Out->SetBoolField(TEXT("is_public"), Function->HasAnyFunctionFlags(FUNC_Public));
        Out->SetBoolField(TEXT("is_static"), Function->HasAnyFunctionFlags(FUNC_Static));
        Out->SetBoolField(TEXT("is_blueprint_pure"), Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
        Out->SetBoolField(TEXT("is_call_in_editor"), Function->HasMetaData(TEXT("CallInEditor")));
        if (Function->HasMetaData(TEXT("Category")))
        {
            Out->SetStringField(TEXT("category"), Function->GetMetaData(TEXT("Category")));
        }
        if (Function->HasMetaData(TEXT("ToolTip")))
        {
            Out->SetStringField(TEXT("tooltip"), Function->GetMetaData(TEXT("ToolTip")).TrimStartAndEnd());
        }
    }

    /** { property name: typed value } over every reflected property of Class read off Object. */
    TSharedPtr<FJsonObject> PropertyValueMap(const UClass* Class, UObject* Object)
    {
        TSharedPtr<FJsonObject> Values = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> PropIt(Class); PropIt; ++PropIt)
        {
            FProperty* Property = *PropIt;
            if (Property == nullptr)
            {
                continue;
            }
            if (void* ValueAddress = Property->ContainerPtrToValuePtr<void>(Object))
            {
                Values->SetField(Property->GetName(), mcp_common::PropertyValueToJson(Property, ValueAddress));
            }
        }
        return Values;
    }

    /** {name, class} of each live component of an actor, in GetComponents order. */
    TArray<TSharedPtr<FJsonValue>> ComponentSummaries(AActor* Actor)
    {
        TArray<UActorComponent*> Owned;
        Actor->GetComponents(Owned);
        TArray<TSharedPtr<FJsonValue>> Summaries;
        for (const UActorComponent* Component : Owned)
        {
            if (Component == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
            Summary->SetStringField(TEXT("name"), Component->GetName());
            Summary->SetStringField(TEXT("class"), Component->GetClass()->GetName());
            Summaries.Add(MakeShared<FJsonValueObject>(Summary));
        }
        return Summaries;
    }

    /**
     * The live component of Actor whose object name is Name (first in GetComponents order), or null.
     * A path segment names an INSTANCE, not a class, which is why GetComponentByClass does not apply.
     */
    UActorComponent* FindComponentNamed(AActor* Actor, const FString& Name)
    {
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (Component && Component->GetName() == Name)
            {
                return Component;
            }
        }
        return nullptr;
    }

    /** One get_enum_info entry. */
    TSharedPtr<FJsonValue> EnumEntry(const UEnum* Enum, int32 Index)
    {
        // internal_name is the AUTHORED entry name ('NewEnumerator8' on a Blueprint enum, the
        // C++ enumerator on a native one), without the "EnumType::" prefix of enum classes.
        // display_name is the editor label: the Blueprint display text or the native DisplayName
        // metadata, falling back to the internal name when there is none.
        const FString InternalName = Enum->GetNameStringByIndex(Index);
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetNumberField(TEXT("index"), Index);
        Entry->SetStringField(TEXT("name"), InternalName); // kept for compatibility, == internal_name
        Entry->SetStringField(TEXT("internal_name"), InternalName);
        Entry->SetStringField(TEXT("display_name"), Enum->GetDisplayNameTextByIndex(Index).ToString());
        Entry->SetNumberField(TEXT("value"), Enum->GetValueByIndex(Index));
        return MakeShared<FJsonValueObject>(Entry);
    }

    /** {name, class} of every object whose DIRECT outer is Package (object-iterator order). */
    TArray<TSharedPtr<FJsonValue>> PackageMembers(const UPackage* Package)
    {
        TArray<TSharedPtr<FJsonValue>> Members;
        for (TObjectIterator<UObject> Scan; Scan; ++Scan)
        {
            const UObject* Candidate = *Scan;
            if (Candidate == nullptr || Candidate->GetOuter() != Package)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
            Member->SetStringField(TEXT("name"), Candidate->GetName());
            Member->SetStringField(TEXT("class"), Candidate->GetClass()->GetName());
            Members.Add(MakeShared<FJsonValueObject>(Member));
        }
        return Members;
    }

    /** diff_objects entry for a property that exists on one side only. */
    TSharedPtr<FJsonValue> OneSidedDifference(const FString& PropertyName, const TCHAR* Which)
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("property"), PropertyName);
        Entry->SetStringField(TEXT("difference"), Which);
        return MakeShared<FJsonValueObject>(Entry);
    }

    /**
     * diff_objects' default 'identical' predicate for a property present on both sides.
     * Equality is the ENGINE's (FProperty::Identical + PPF_DeepComparison, via mcp_common): the
     * cheap struct comparison runs first and the typed JSON form is only built for what is not
     * identical. A real difference is appended to OutDifferences; one that only shows in the
     * exported text (FText localization identity, object outer) is counted per kind as noise,
     * and listed in OutNoise only when bListNoise.
     */
    void CompareWithEngineEquality(const FString& PropertyName, FProperty* PropA, UObject* ObjectA, FProperty* PropB, UObject* ObjectB,
        bool bListNoise, TArray<TSharedPtr<FJsonValue>>& OutDifferences, TArray<TSharedPtr<FJsonValue>>& OutNoise,
        TMap<FString, int32>& InOutNoiseByKind)
    {
        if (mcp_common::ComparePropertyValues(PropA, ObjectA, PropB, ObjectB).Kind == mcp_common::EPropertyDiffKind::Identical)
        {
            return;
        }
        TSharedPtr<FJsonObject> Entry = mcp_common::ComparePropertyValuesToJson(PropA, ObjectA, PropB, ObjectB);
        if (!Entry.IsValid())
        {
            return;
        }

        bool bDiffers = false;
        Entry->TryGetBoolField(TEXT("differs"), bDiffers);
        FString Kind;
        Entry->TryGetStringField(TEXT("kind"), Kind);
        Entry->SetStringField(TEXT("property"), PropertyName);

        // The comparator's value_a / value_b are re-exposed under this tool's names value_1 /
        // value_2. Both are COPIED out first: SetField adds keys and may rehash the map, which
        // would invalidate a pointer returned by Find().
        TSharedPtr<FJsonValue> SideA;
        TSharedPtr<FJsonValue> SideB;
        if (const TSharedPtr<FJsonValue>* FoundA = Entry->Values.Find(TEXT("value_a"))) { SideA = *FoundA; }
        if (const TSharedPtr<FJsonValue>* FoundB = Entry->Values.Find(TEXT("value_b"))) { SideB = *FoundB; }
        if (SideA.IsValid()) { Entry->SetField(TEXT("value_1"), SideA); }
        if (SideB.IsValid()) { Entry->SetField(TEXT("value_2"), SideB); }

        if (bDiffers)
        {
            Entry->SetStringField(TEXT("difference"),
                Kind == TEXT("type_mismatch") ? TEXT("Types differ (values NOT compared)") : TEXT("Values differ"));
            OutDifferences.Add(MakeShared<FJsonValueObject>(Entry));
            return;
        }
        if (Kind == TEXT("identical"))
        {
            return;
        }
        // Equal for the engine, different on paper: always counted (so a caller can see what
        // was suppressed and how much), listed only on request.
        InOutNoiseByKind.FindOrAdd(Kind)++;
        if (bListNoise)
        {
            Entry->SetStringField(TEXT("difference"), TEXT("Equal per the engine; export text differs"));
            OutNoise.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    /**
     * The noise half of a diff reply: "noise_counts" per kind and their "noise_count" total, emitted
     * even when empty so "0 differences" stays distinguishable from "N differences hidden", and the
     * "noise" list itself only when bIncludeNoise.
     */
    void WriteNoiseSummary(const TSharedPtr<FJsonObject>& Out, const TMap<FString, int32>& NoiseByKind, bool bIncludeNoise,
        const TArray<TSharedPtr<FJsonValue>>& Noise)
    {
        TSharedPtr<FJsonObject> Counts = MakeShared<FJsonObject>();
        int32 Total = 0;
        for (const TPair<FString, int32>& KindCount : NoiseByKind)
        {
            Counts->SetNumberField(KindCount.Key, KindCount.Value);
            Total += KindCount.Value;
        }
        Out->SetObjectField(TEXT("noise_counts"), Counts);
        Out->SetNumberField(TEXT("noise_count"), Total);
        if (bIncludeNoise)
        {
            Out->SetArrayField(TEXT("noise"), Noise);
        }
    }

    /**
     * Why Function cannot be invoked through reflection, or empty when it can. Only functions exposed
     * to scripting (BlueprintCallable+Public, or a BlueprintEvent) are -- the filter Remote Control
     * applies too (IsFunctionAllowed); any other UFunction (internal, editor-only) could crash or
     * corrupt state. FunctionLabel is the name the reason quotes.
     */
    FString NotScriptCallableReason(const UFunction* Function, const FString& FunctionLabel)
    {
        if (Function->HasAllFunctionFlags(FUNC_BlueprintCallable | FUNC_Public) || Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
        {
            return FString();
        }
        return FString::Printf(
            TEXT("Function '%s' is not callable via reflection (requires BlueprintCallable+Public or BlueprintEvent)"),
            *FunctionLabel);
    }

    /**
     * Typed read-back of a called function's frame: the return value onto Out as
     * "return_value", every other out-parameter into an "out_params" object that is only added
     * when there is at least one.
     */
    void ReadBackCallFrame(UFunction* Function, uint8* Frame, const TSharedPtr<FJsonObject>& Out)
    {
        TSharedPtr<FJsonObject> OutParams = MakeShared<FJsonObject>();
        for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
        {
            FProperty* Prop = *PropIt;
            if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Parm))
            {
                continue;
            }
            void* Slot = Prop->ContainerPtrToValuePtr<void>(Frame);
            if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
            {
                Out->SetField(TEXT("return_value"), mcp_common::PropertyValueToJson(Prop, Slot));
            }
            else if (Prop->HasAnyPropertyFlags(CPF_OutParm))
            {
                OutParams->SetField(Prop->GetName(), mcp_common::PropertyValueToJson(Prop, Slot));
            }
        }
        if (OutParams->Values.Num() > 0)
        {
            Out->SetObjectField(TEXT("out_params"), OutParams);
        }
    }

    /**
     * The `transact` opt-in of the call_* handlers: the undo bracket around one call. The
     * FScopedTransaction is held in a TUniquePtr because it starts the transaction in its
     * constructor and ends it in its destructor, so it must not exist at all when transact=false.
     */
    struct FCallTransaction
    {
        TUniquePtr<FScopedTransaction> Scope;
        TArray<TSharedPtr<FJsonValue>> Modified;
        bool bDirtyBefore = false;
        bool bSavedToBuffer = false;

        /** Reads the owning package's dirty flag, then opens the transaction and Modify()s Target (and a component's owner). */
        void Open(UObject* Target, const TCHAR* CommandName, const FString& FunctionName)
        {
            bool bIsExternal = false;
            const UPackage* Package = ReflectionOfpa_ResolveOwningPackage(Target, bIsExternal);
            bDirtyBefore = Package != nullptr && Package->IsDirty();
            Scope = MakeUnique<FScopedTransaction>(FText::FromString(
                FString::Printf(TEXT("MCP %s: %s.%s"), CommandName, *Target->GetName(), *FunctionName)));
            ReflectionTransact_ModifyTargets(Target, Modified, bSavedToBuffer);
        }

        /**
         * After a rejected call: an open transaction is canceled, which rolls the recorded object
         * state back but not the package's dirty flag, and InOutError says so.
         */
        void CancelAfterFailure(FString& InOutError)
        {
            if (Scope.IsValid())
            {
                Scope->Cancel();
                Scope.Reset();
                InOutError += TEXT(" [transact=true: the transaction was CANCELED and the object state rolled back; the package may still read as dirty]");
            }
        }

        /**
         * Closes the transaction first, so the undo record is complete (Modify() already marked the
         * package dirty, so the dirty read is valid either way), then writes the re-read of the owning
         * package under Reply's "transaction". Returns the verdict, explained in OutDetail.
         */
        mcp_common::EMcpVerification Close(UObject* Target, const TSharedPtr<FJsonObject>& Reply, FString& OutDetail)
        {
            Scope.Reset();
            TSharedPtr<FJsonObject> Report = MakeShared<FJsonObject>();
            const mcp_common::EMcpVerification Verification =
                ReflectionTransact_Report(Target, bDirtyBefore, bSavedToBuffer, Modified, Report, OutDetail);
            Reply->SetObjectField(TEXT("transaction"), Report);
            return Verification;
        }
    };

    /**
     * set_properties' single `value`, whatever its JSON type, tried in this order: array, object,
     * then string, number, bool. A number or a bool is normally picked up by the string read,
     * which converts it, so the later typed reads are only a fallback. Null when none applies.
     */
    TSharedPtr<FJsonValue> ReadAnyTypedValue(const TSharedPtr<FJsonObject>& Params)
    {
        const TArray<TSharedPtr<FJsonValue>>* AsArray = nullptr;
        if (Params->TryGetArrayField(TEXT("value"), AsArray) && AsArray)
        {
            return MakeShared<FJsonValueArray>(*AsArray);
        }
        const TSharedPtr<FJsonObject>* AsObject = nullptr;
        if (Params->TryGetObjectField(FStringView(TEXT("value")), AsObject) && AsObject && (*AsObject))
        {
            return MakeShared<FJsonValueObject>(*AsObject);
        }
        FString AsString;
        if (Params->TryGetStringField(TEXT("value"), AsString))
        {
            return MakeShared<FJsonValueString>(AsString);
        }
        double AsNumber = 0.0;
        if (Params->TryGetNumberField(TEXT("value"), AsNumber))
        {
            return MakeShared<FJsonValueNumber>(AsNumber);
        }
        bool bAsBool = false;
        if (Params->TryGetBoolField(TEXT("value"), bAsBool))
        {
            return MakeShared<FJsonValueBoolean>(bAsBool);
        }
        return nullptr;
    }
}

// First segment of a dotted path: a top-level editor-world actor by name
// (legacy single-segment ResolveObjectByName behavior, extracted for reuse).
UObject* FindActorByName(const FString& ActorName)
{
    UWorld* EditorWorld = mcp_common::GetEditorWorld();
    if (EditorWorld == nullptr)
    {
        return nullptr;
    }

    if (AActor* Named = mcp_common::FindActorByObjectName(EditorWorld, ActorName))
    {
        return Named;
    }

    // "WorldSettings" is accepted as an alias of the world's AWorldSettings: the actor
    // iteration may not surface it, and its internal name can carry a numeric suffix.
    AWorldSettings* Settings = EditorWorld->GetWorldSettings();
    if (Settings != nullptr && (ActorName == TEXT("WorldSettings") || Settings->GetName() == ActorName))
    {
        return Settings;
    }

    // The label the Outliner shows, when exactly one actor has it: most actor tools take a label, and an agent
    // that placed "Sun" should not need its internal name to reach it.
    return mcp_common::FindActorByUniqueLabel(EditorWorld, ActorName);
}

// Resolves one further segment of a dotted path: a named component on an actor, the "AnimInstance" alias on a
// USkeletalMeshComponent, or any other named subobject owned by Owner.
UObject* FindSubobjectByName(UObject* Owner, const FString& SubobjectName)
{
    if (!Owner)
    {
        return nullptr;
    }

    // Actor -> component, matched by its instance name, or by a class only one of its components has
    // ("Sun.DirectionalLightComponent"): the instance names (LightComponent0, HeightFogComponent0) are not guessable.
    if (AActor* OwnerActor = Cast<AActor>(Owner))
    {
        return mcp_common::FindComponentByNameOrClass(OwnerActor, SubobjectName);
    }

    // Component -> its running AnimInstance. UAnimInstance is a transient subobject
    // created at runtime (InitAnim); its object name is not user-facing, so a
    // generic by-name subobject search can never find it -- expose the engine's own
    // getter as a named alias instead (mirrors the "WorldSettings" alias above).
    if (USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(Owner))
    {
        if (SubobjectName == TEXT("AnimInstance"))
        {
            return SkelMeshComp->GetAnimInstance();
        }
    }

    // Generic fallback: any other named subobject owned by this object (e.g. a
    // UObject-derived member created with NewObject(this, ...)).
    return StaticFindObjectFast(UObject::StaticClass(), Owner, *SubobjectName);
}

// Editor-side object resolution shared by the handlers: an asset path, the GetPathName() of
// a live object, a bare top-level actor name, or a dotted "Actor.Component[.Subobject]".
UObject* ResolveObjectByName(const FString& ObjectPath)
{
    // Resolve by path -- covers on-disk asset paths AND the full GetPathName()
    // of an already-loaded live actor/component (StaticFindObject finds
    // in-memory objects first). Gated resolve so a miss never leaves a phantom
    // UPackage that would break a later create_X at this path.
    UObject* FoundObject = mcp_common::ResolveObjectByPath(ObjectPath);
    if (FoundObject)
    {
        return FoundObject;
    }

    // Convenience dotted path "ActorName[.ComponentName[.Subobject]]": resolve the
    // first segment as a top-level editor-world actor (all that a path without a
    // dot does), then walk each further segment down into a named component/subobject. This is what lets call_function/
    // get_properties/set_properties target a runtime subobject (e.g. the
    // UAnimInstance running on a USkeletalMeshComponent), not just top-level actors.
    TArray<FString> Segments;
    ObjectPath.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
    if (Segments.Num() == 0)
    {
        return nullptr;
    }

    UObject* Resolved = FindActorByName(Segments[0]);
    for (int32 i = 1; Resolved && i < Segments.Num(); ++i)
    {
        Resolved = FindSubobjectByName(Resolved, Segments[i]);
    }
    return Resolved;
}

// Why ResolveObjectByName found nothing for ObjectPath, as an error message: which segment of a dotted path missed,
// and for a component, the names and classes the actor does have.
FString DescribeUnresolvedPath(const FString& ObjectPath)
{
    TArray<FString> Segments;
    ObjectPath.ParseIntoArray(Segments, TEXT("."), /*InCullEmpty=*/true);
    if (ObjectPath.StartsWith(TEXT("/")) || Segments.Num() == 0)
    {
        return FString::Printf(TEXT("Object not found: %s"), *ObjectPath);
    }
    AActor* Actor = Cast<AActor>(FindActorByName(Segments[0]));
    if (!Actor)
    {
        int32 LabelMatches = 0;
        mcp_common::FindActorByUniqueLabel(mcp_common::GetEditorWorld(), Segments[0], &LabelMatches);
        return LabelMatches > 1
            ? FString::Printf(TEXT("Object not found: %d actors are labelled '%s'; name one by its internal name (find_actors_by_name)"), LabelMatches, *Segments[0])
            : FString::Printf(TEXT("Object not found: no editor-world actor is named or labelled '%s'"), *Segments[0]);
    }
    if (Segments.Num() < 2)
    {
        return FString::Printf(TEXT("Object not found: %s"), *ObjectPath);
    }
    TArray<FString> Ambiguous;
    if (!mcp_common::FindComponentByNameOrClass(Actor, Segments[1], &Ambiguous) && Ambiguous.Num() > 0)
    {
        return FString::Printf(TEXT("Object not found: '%s' has %d components of class %s (%s); name one"),
            *Segments[0], Ambiguous.Num(), *Segments[1], *FString::Join(Ambiguous, TEXT(", ")));
    }
    TArray<UActorComponent*> Components;
    Actor->GetComponents(Components);
    TArray<FString> Listed;
    for (const UActorComponent* Component : Components)
    {
        if (Component && Listed.Num() < 40)
        {
            Listed.Add(FString::Printf(TEXT("%s (%s)"), *Component->GetName(), *Component->GetClass()->GetName()));
        }
    }
    return FString::Printf(TEXT("Object not found: %s. Actor '%s' has these components, by name or by a class only one of them has: %s"),
        *ObjectPath, *Segments[0], *FString::Join(Listed, TEXT(", ")));
}

FString GetPropertyValueAsString(FProperty* Property, void* ValuePtr)
{
    if (!Property || !ValuePtr)
    {
        return TEXT("null");
    }

    // FProperty exposes ExportTextItem_Direct, which dispatches through the
    // virtual ExportText_Internal to the correct concrete property type. This handles
    // every common type uniformly (bool/int/float/double/FString/FName, enums via
    // FByteProperty/FEnumProperty, object refs via FObjectProperty, and structs like
    // FVector via FStructProperty). Passing nullptr for Defaults forces the value to be
    // exported even when it matches the property default (no delta suppression).
    FString OutputString;
    Property->ExportTextItem_Direct(
        OutputString,
        ValuePtr,
        /*DefaultValue=*/nullptr,
        /*Parent=*/nullptr,
        PPF_None);

    return OutputString;
}

// Append the enum's valid values (name + numeric value) to a descriptor, skipping the
// autogenerated _MAX and any Hidden entries. Shared by BuildPropertyDescriptor.
static void AppendEnumValues(const TSharedPtr<FJsonObject>& Desc, UEnum* Enum)
{
    if (!Enum)
    {
        return;
    }
    Desc->SetStringField(TEXT("enum_name"), Enum->GetName());
    Desc->SetStringField(TEXT("enum_path"), Enum->GetPathName());
    TArray<TSharedPtr<FJsonValue>> Values;
    const int32 Num = Enum->NumEnums();
    for (int32 i = 0; i < Num - 1; ++i) // -1 to skip the autogenerated _MAX
    {
        if (Enum->HasMetaData(TEXT("Hidden"), i))
        {
            continue;
        }
        TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
        ValueObj->SetStringField(TEXT("name"), Enum->GetNameStringByIndex(i));
        ValueObj->SetNumberField(TEXT("value"), Enum->GetValueByIndex(i));
        Values.Add(MakeShared<FJsonValueObject>(ValueObj));
    }
    Desc->SetArrayField(TEXT("enum_values"), Values);
}

// Build a typed, self-describing descriptor for one FProperty: the LLM learns the exact kind, the
// valid enum values, struct fields, expected object class and container element types — enough to
// drive any property generically without a dedicated per-property command.
TSharedPtr<FJsonObject> BuildPropertyDescriptor(FProperty* Property, int32 Depth = 1)
{
    TSharedPtr<FJsonObject> Desc = MakeShared<FJsonObject>();
    if (!Property)
    {
        return Desc;
    }

    Desc->SetStringField(TEXT("name"), Property->GetName());
    Desc->SetStringField(TEXT("property_class"), Property->GetClass()->GetName());
    {
        FString ExtendedType;
        const FString CppType = Property->GetCPPType(&ExtendedType);
        Desc->SetStringField(TEXT("cpp_type"), CppType + ExtendedType);
    }

    // Edit / Blueprint flags. 'editable' = editor-editability; 'writable' (below) is the
    // actual gate set/set_properties enforce (editor-editable OR Blueprint-writable).
    Desc->SetBoolField(TEXT("editable"), Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst));
    Desc->SetBoolField(TEXT("edit_const"), Property->HasAnyPropertyFlags(CPF_EditConst));
    Desc->SetBoolField(TEXT("blueprint_visible"), Property->HasAnyPropertyFlags(CPF_BlueprintVisible));
    // Same test as the ApplyCheckedPropertySet gate: editable-in-editor OR Blueprint-writable
    // (where private AllowPrivateAccess UPROPERTYs live). The LLM uses this to know what set_property can mutate.
    Desc->SetBoolField(TEXT("writable"),
        (Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst)) ||
        (Property->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly)));
    Desc->SetBoolField(TEXT("blueprint_readonly"), Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly));
    Desc->SetBoolField(TEXT("transient"), Property->HasAnyPropertyFlags(CPF_Transient));

    // Additional runtime-safe flags (no gating): does this property
    // persist to a SaveGame archive, replicate over the network, come from an .ini config
    // section, carry a deprecation warning, or refuse per-instance edits on template defaults.
    Desc->SetBoolField(TEXT("save_game"), Property->HasAnyPropertyFlags(CPF_SaveGame));
    Desc->SetBoolField(TEXT("replicated"), Property->HasAnyPropertyFlags(CPF_Net));
    Desc->SetBoolField(TEXT("config"), Property->HasAnyPropertyFlags(CPF_Config));
    Desc->SetBoolField(TEXT("deprecated"), Property->HasAnyPropertyFlags(CPF_Deprecated));
    Desc->SetBoolField(TEXT("disable_edit_on_instance"), Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance));

    // Canonical kind + kind-specific shape. Cast most-derived first (FClass/FSoftClass/
    // FSoftObject before FObjectPropertyBase; FEnum/FByte-enum before plain numeric).
    FString Kind;
    if (CastField<FBoolProperty>(Property))
    {
        Kind = TEXT("bool");
    }
    else if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
    {
        Kind = TEXT("enum");
        AppendEnumValues(Desc, EnumProp->GetEnum());
    }
    else if (FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
    {
        if (UEnum* ByteEnum = NumProp->GetIntPropertyEnum())
        {
            Kind = TEXT("enum");
            AppendEnumValues(Desc, ByteEnum);
        }
        else
        {
            Kind = NumProp->IsFloatingPoint() ? TEXT("float") : TEXT("int");
        }
    }
    else if (CastField<FStrProperty>(Property))
    {
        Kind = TEXT("string");
    }
    else if (CastField<FNameProperty>(Property))
    {
        Kind = TEXT("name");
    }
    else if (CastField<FTextProperty>(Property))
    {
        Kind = TEXT("text");
    }
    else if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
    {
        Kind = TEXT("struct");
        if (StructProp->Struct)
        {
            Desc->SetStringField(TEXT("struct_name"), StructProp->Struct->GetName());
            Desc->SetStringField(TEXT("struct_path"), StructProp->Struct->GetPathName());
            if (Depth > 0)
            {
                TArray<TSharedPtr<FJsonValue>> Fields;
                for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
                {
                    Fields.Add(MakeShared<FJsonValueObject>(BuildPropertyDescriptor(*It, Depth - 1)));
                }
                Desc->SetArrayField(TEXT("fields"), Fields);
            }
        }
    }
    else if (FClassProperty* ClassProp = CastField<FClassProperty>(Property))
    {
        Kind = TEXT("class");
        if (ClassProp->MetaClass)
        {
            Desc->SetStringField(TEXT("meta_class"), ClassProp->MetaClass->GetPathName());
        }
    }
    else if (FSoftClassProperty* SoftClassProp = CastField<FSoftClassProperty>(Property))
    {
        Kind = TEXT("soft_class");
        if (SoftClassProp->MetaClass)
        {
            Desc->SetStringField(TEXT("meta_class"), SoftClassProp->MetaClass->GetPathName());
        }
    }
    else if (FSoftObjectProperty* SoftObjProp = CastField<FSoftObjectProperty>(Property))
    {
        Kind = TEXT("soft_object");
        if (SoftObjProp->PropertyClass)
        {
            Desc->SetStringField(TEXT("object_class"), SoftObjProp->PropertyClass->GetPathName());
        }
    }
    else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
    {
        Kind = TEXT("object");
        if (ObjProp->PropertyClass)
        {
            Desc->SetStringField(TEXT("object_class"), ObjProp->PropertyClass->GetPathName());
        }
    }
    else if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
    {
        Kind = TEXT("array");
        if (Depth > 0 && ArrayProp->Inner)
        {
            Desc->SetObjectField(TEXT("inner"), BuildPropertyDescriptor(ArrayProp->Inner, Depth - 1));
        }
    }
    else if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
    {
        Kind = TEXT("set");
        if (Depth > 0 && SetProp->ElementProp)
        {
            Desc->SetObjectField(TEXT("element"), BuildPropertyDescriptor(SetProp->ElementProp, Depth - 1));
        }
    }
    else if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
    {
        Kind = TEXT("map");
        if (Depth > 0)
        {
            if (MapProp->KeyProp)
            {
                Desc->SetObjectField(TEXT("key"), BuildPropertyDescriptor(MapProp->KeyProp, Depth - 1));
            }
            if (MapProp->ValueProp)
            {
                Desc->SetObjectField(TEXT("value"), BuildPropertyDescriptor(MapProp->ValueProp, Depth - 1));
            }
        }
    }
    else
    {
        Kind = TEXT("unknown");
    }
    Desc->SetStringField(TEXT("kind"), Kind);

    // Numeric ranges + tooltip are metadata (WITH_METADATA: present in editor builds, absent
    // in cooked/Shipping). Emit only when actually present — never invent.
    if (Property->HasMetaData(TEXT("ClampMin"))) { Desc->SetStringField(TEXT("clamp_min"), Property->GetMetaData(TEXT("ClampMin"))); }
    if (Property->HasMetaData(TEXT("ClampMax"))) { Desc->SetStringField(TEXT("clamp_max"), Property->GetMetaData(TEXT("ClampMax"))); }
    if (Property->HasMetaData(TEXT("UIMin")))    { Desc->SetStringField(TEXT("ui_min"),    Property->GetMetaData(TEXT("UIMin"))); }
    if (Property->HasMetaData(TEXT("UIMax")))    { Desc->SetStringField(TEXT("ui_max"),    Property->GetMetaData(TEXT("UIMax"))); }
    if (Property->HasMetaData(TEXT("ToolTip")))  { Desc->SetStringField(TEXT("tooltip"),   Property->GetMetaData(TEXT("ToolTip")).TrimStartAndEnd()); }

    // edit_condition/display_name/units/allow_private_access are plain
    // metadata strings; categories splits the "Category" metadata on '|' (UE's own subcategory
    // separator, e.g. "Movement|Advanced" -> ["Movement","Advanced"]).
    if (Property->HasMetaData(TEXT("EditCondition"))) { Desc->SetStringField(TEXT("edit_condition"), Property->GetMetaData(TEXT("EditCondition"))); }
    if (Property->HasMetaData(TEXT("DisplayName")))   { Desc->SetStringField(TEXT("display_name"),   Property->GetMetaData(TEXT("DisplayName"))); }
    if (Property->HasMetaData(TEXT("Units")))         { Desc->SetStringField(TEXT("units"),          Property->GetMetaData(TEXT("Units"))); }
    if (Property->HasMetaData(TEXT("Category")))
    {
        TArray<FString> Categories;
        Property->GetMetaData(TEXT("Category")).ParseIntoArray(Categories, TEXT("|"), /*InCullEmpty=*/true);
        Desc->SetArrayField(TEXT("categories"), mcp_common::StringsToJson(Categories));
    }
    Desc->SetBoolField(TEXT("allow_private_access"),
        Property->HasMetaData(TEXT("AllowPrivateAccess")) && Property->GetMetaData(TEXT("AllowPrivateAccess")).Equals(TEXT("true"), ESearchCase::IgnoreCase));

    return Desc;
}

// Class-level descriptor: flags (runtime-safe), the full implemented-interface
// set (walks GetSuperClass() since UClass::Interfaces only lists the ones declared at that exact
// level -- ImplementsInterface() walks the hierarchy internally too, but only as a single-interface
// predicate, not an enumerator), class_group (WITH_EDITOR), and metadata (WITH_METADATA). Shared by
// get_object_info, get_class_hierarchy, get_class_properties.
TSharedPtr<FJsonObject> BuildClassDescriptor(UClass* Class)
{
    TSharedPtr<FJsonObject> Desc = MakeShared<FJsonObject>();
    if (!Class)
    {
        return Desc;
    }

    Desc->SetBoolField(TEXT("is_abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
    Desc->SetBoolField(TEXT("is_deprecated"), Class->HasAnyClassFlags(CLASS_Deprecated));
    Desc->SetBoolField(TEXT("is_placeable"), !Class->HasAnyClassFlags(CLASS_NotPlaceable));
    Desc->SetBoolField(TEXT("is_native"), Class->IsNative());
    Desc->SetStringField(TEXT("config_name"), Class->GetConfigName());

    TArray<TSharedPtr<FJsonValue>> InterfacesArray;
    TSet<UClass*> SeenInterfaces;
    for (UClass* Cur = Class; Cur; Cur = Cur->GetSuperClass())
    {
        for (const FImplementedInterface& Impl : Cur->Interfaces)
        {
            if (!Impl.Class || SeenInterfaces.Contains(Impl.Class))
            {
                continue;
            }
            SeenInterfaces.Add(Impl.Class);
            TSharedPtr<FJsonObject> InterfaceObj = MakeShared<FJsonObject>();
            InterfaceObj->SetStringField(TEXT("interface"), Impl.Class->GetName());
            InterfaceObj->SetBoolField(TEXT("implemented_by_k2"), Impl.bImplementedByK2);
            InterfacesArray.Add(MakeShared<FJsonValueObject>(InterfaceObj));
        }
    }
    Desc->SetArrayField(TEXT("implemented_interfaces"), InterfacesArray);

    // GetClassGroupNames is WITH_EDITOR-gated in the engine itself (not WITH_METADATA).
    TArray<FString> ClassGroups;
    Class->GetClassGroupNames(ClassGroups);
    if (ClassGroups.Num() > 0)
    {
        Desc->SetArrayField(TEXT("class_group"), mcp_common::StringsToJson(ClassGroups));
    }

    // "IsBlueprintBase" is the key (see EdGraphSchema_K2.cpp/Kismet2.cpp) -- NOT
    // "Blueprintable", which does not exist anywhere in the engine.
    Desc->SetBoolField(TEXT("is_blueprintable"), Class->GetBoolMetaDataHierarchical(FName(TEXT("IsBlueprintBase"))));

    // BlueprintType: manual hierarchy walk mirroring UEdGraphSchema_K2::IsAllowableBlueprintVariableType
    // -- the first of BlueprintType/NotBlueprintType found walking up wins; NOT a simple inverse of
    // NotBlueprintType. No metadata anywhere in the chain defaults to false (conservative: don't
    // over-claim BP-variable usability for a class that never opted in).
    {
        static const FName MD_BlueprintType(TEXT("BlueprintType"));
        static const FName MD_NotBlueprintType(TEXT("NotBlueprintType"));
        bool bIsBlueprintType = false;
        for (UClass* Cur = Class; Cur; Cur = Cur->GetSuperClass())
        {
            if (Cur->GetBoolMetaData(MD_BlueprintType)) { bIsBlueprintType = true; break; }
            if (Cur->GetBoolMetaData(MD_NotBlueprintType)) { bIsBlueprintType = false; break; }
        }
        Desc->SetBoolField(TEXT("is_blueprint_type"), bIsBlueprintType);
    }

    if (Class->HasMetaData(TEXT("DisplayName"))) { Desc->SetStringField(TEXT("display_name"), Class->GetMetaData(TEXT("DisplayName"))); }
    if (Class->HasMetaData(TEXT("ToolTip")))     { Desc->SetStringField(TEXT("tooltip"), Class->GetMetaData(TEXT("ToolTip")).TrimStartAndEnd()); }

    return Desc;
}

namespace UEAgentReflectionWriteImpl
{
    void CollectUnknownValueKeys(const FProperty* Property, const TSharedPtr<FJsonValue>& Value, const FString& Name, TArray<FString>& OutUnknown);

    // One element of Property (a C-array element, an array's inner element, or the value itself).
    void CollectUnknownElementKeys(const FProperty* Property, const TSharedPtr<FJsonValue>& Value, const FString& Name, TArray<FString>& OutUnknown)
    {
        if (!Value.IsValid())
        {
            return;
        }
        if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            const TSharedPtr<FJsonObject>* Object = nullptr;
            if (!Value->TryGetObject(Object) || !Object || !Object->IsValid())
            {
                return; // a string is imported as text, an array by the struct's own importer
            }
            for (const auto& Pair : (*Object)->Values)
            {
                const FString Key(*Pair.Key);
                const FProperty* Field = nullptr;
                for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
                {
                    // The engine's importer looks each field up by its authored name, without case.
                    if (StructProperty->Struct->GetAuthoredNameForField(*It).Equals(Key, ESearchCase::IgnoreCase)
                        || It->GetName().Equals(Key, ESearchCase::IgnoreCase))
                    {
                        Field = *It;
                        break;
                    }
                }
                if (!Field)
                {
                    OutUnknown.Add(FString::Printf(TEXT("%s.%s"), *Name, *Key));
                    continue;
                }
                CollectUnknownValueKeys(Field, Pair.Value, FString::Printf(TEXT("%s.%s"), *Name, *Key), OutUnknown);
            }
        }
        else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
            if (Value->TryGetArray(Elements) && Elements)
            {
                for (int32 Index = 0; Index < Elements->Num(); ++Index)
                {
                    CollectUnknownValueKeys(ArrayProperty->Inner, (*Elements)[Index], FString::Printf(TEXT("%s[%d]"), *Name, Index), OutUnknown);
                }
            }
        }
    }

    // Keys of a JSON object written into a struct that name no field of it, as dotted names. The engine's importer
    // skips such a key without a word, so a misspelled field would read as written while nothing changed.
    void CollectUnknownValueKeys(const FProperty* Property, const TSharedPtr<FJsonValue>& Value, const FString& Name, TArray<FString>& OutUnknown)
    {
        const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
        if (Property->ArrayDim > 1 && Value.IsValid() && Value->TryGetArray(Elements) && Elements)
        {
            for (int32 Index = 0; Index < Elements->Num(); ++Index)
            {
                CollectUnknownElementKeys(Property, (*Elements)[Index], FString::Printf(TEXT("%s[%d]"), *Name, Index), OutUnknown);
            }
            return;
        }
        CollectUnknownElementKeys(Property, Value, Name, OutUnknown);
    }

    void ClampValue(const FProperty* Property, void* ValuePtr, const FString& Name, TArray<FString>& OutClamped);

    void ClampElement(const FProperty* Property, void* Element, const FString& Name, TArray<FString>& OutClamped)
    {
        if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
        {
            const bool bHasMin = Property->HasMetaData(TEXT("ClampMin"));
            const bool bHasMax = Property->HasMetaData(TEXT("ClampMax"));
            if (!bHasMin && !bHasMax)
            {
                return;
            }
            if (Numeric->IsFloatingPoint())
            {
                const double Before = Numeric->GetFloatingPointPropertyValue(Element);
                double After = Before;
                if (bHasMin) { After = FMath::Max(After, FCString::Atod(*Property->GetMetaData(TEXT("ClampMin")))); }
                if (bHasMax) { After = FMath::Min(After, FCString::Atod(*Property->GetMetaData(TEXT("ClampMax")))); }
                if (After != Before)
                {
                    Numeric->SetFloatingPointPropertyValue(Element, After);
                    OutClamped.Add(FString::Printf(TEXT("%s=%s"), *Name, *FString::SanitizeFloat(After)));
                }
            }
            else if (Numeric->IsInteger() && !Numeric->IsEnum())
            {
                const int64 Before = Numeric->GetSignedIntPropertyValue(Element);
                int64 After = Before;
                if (bHasMin) { After = FMath::Max<int64>(After, FCString::Atoi64(*Property->GetMetaData(TEXT("ClampMin")))); }
                if (bHasMax) { After = FMath::Min<int64>(After, FCString::Atoi64(*Property->GetMetaData(TEXT("ClampMax")))); }
                if (After != Before)
                {
                    Numeric->SetIntPropertyValue(Element, After);
                    OutClamped.Add(FString::Printf(TEXT("%s=%lld"), *Name, After));
                }
            }
        }
        else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
        {
            for (TFieldIterator<FProperty> It(StructProperty->Struct); It; ++It)
            {
                ClampValue(*It, It->ContainerPtrToValuePtr<void>(Element), FString::Printf(TEXT("%s.%s"), *Name, *It->GetName()), OutClamped);
            }
        }
        else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
        {
            FScriptArrayHelper Helper(ArrayProperty, Element);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                ClampElement(ArrayProperty->Inner, Helper.GetRawPtr(Index), FString::Printf(TEXT("%s[%d]"), *Name, Index), OutClamped);
            }
        }
    }

    // Clamps the value to the ClampMin/ClampMax metadata of each numeric property in it, nested structs and array
    // elements included, as the Details panel's number boxes do. Adds "name=value" for each value it changed.
    void ClampValue(const FProperty* Property, void* ValuePtr, const FString& Name, TArray<FString>& OutClamped)
    {
        for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
        {
            ClampElement(Property, static_cast<uint8*>(ValuePtr) + Index * Property->GetElementSize(),
                Property->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Name, Index) : Name, OutClamped);
        }
    }
}

// Apply a single property write through the honest editable gate + editor change notification.
// Every write of set_properties goes through here. The gate accepts a property that is editable in
// the editor (EditAnywhere/EditDefaultsOnly and not EditConst) OR writable from Blueprint
// (BlueprintReadWrite); the Modify/PreEditChange/PostEditChangeProperty sequence is what makes the
// change take effect and persist. All notify calls are WITH_EDITOR (our harness runs an editor target).
// A JSON key that names no field of a struct is refused before anything is written. With OutClamped, the value is
// clamped to the property's ClampMin/ClampMax metadata before PostEditChange, and each value changed is listed.
bool ApplyCheckedPropertySet(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonValue>& Value, FString& OutError,
    TArray<FString>* OutClamped = nullptr)
{
    if (!Object)
    {
        OutError = TEXT("Invalid object");
        return false;
    }

    FProperty* Property = Object->GetClass()->FindPropertyByName(*PropertyName);
    if (!Property)
    {
        OutError = FString::Printf(TEXT("Property not found: %s"), *PropertyName);
        return false;
    }

    // Writable via reflection if editable in the editor (EditAnywhere/EditDefaultsOnly,
    // not EditConst) OR writable from Blueprint (BlueprintReadWrite). The latter covers
    // private UPROPERTYs exposed with meta=(AllowPrivateAccess="true"): they are
    // legitimately mutable even when not surfaced in the editor details panel. EditConst
    // still blocks an edit-only property; a BlueprintReadWrite one stays writable.
    const bool bEditable = Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_EditConst);
    const bool bBlueprintWritable = Property->HasAnyPropertyFlags(CPF_BlueprintVisible) && !Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
    if (!bEditable && !bBlueprintWritable)
    {
        OutError = FString::Printf(TEXT("Property '%s' is not writable via reflection (requires EditAnywhere/EditDefaultsOnly-and-not-EditConst, or BlueprintReadWrite)"), *PropertyName);
        return false;
    }

    TArray<FString> Unknown;
    UEAgentReflectionWriteImpl::CollectUnknownValueKeys(Property, Value, PropertyName, Unknown);
    if (Unknown.Num() > 0)
    {
        OutError = FString::Printf(TEXT("%s names no field of its struct: %s. Nothing was written to %s; get_properties shows its fields."),
            Unknown.Num() == 1 ? TEXT("This key") : TEXT("These keys"), *FString::Join(Unknown, TEXT(", ")), *PropertyName);
        return false;
    }

    Object->Modify();
    Object->PreEditChange(Property);

    const bool bSet = mcp_common::SetObjectProperty(Object, PropertyName, Value, OutError);
    if (bSet && OutClamped)
    {
        UEAgentReflectionWriteImpl::ClampValue(Property, Property->ContainerPtrToValuePtr<void>(Object), PropertyName, *OutClamped);
    }

    // Always close the edit cycle so the object re-validates its current state.
    FPropertyChangedEvent ChangeEvent(Property, bSet ? EPropertyChangeType::ValueSet : EPropertyChangeType::Unspecified);
    Object->PostEditChangeProperty(ChangeEvent);

    return bSet;
}

// Types and instances: classes, their members and the inheritance chain.

// MCP:get_class_properties - List every property VISIBLE on a class — the ones it DECLARES plus every inherited one, each tagged with owner_class and is_inherited so "does this class add anything of its own?" is a FIELD (declared_property_count) and not an eyeball diff against a remembered list of engine members. declared_only=true returns just this class's own. WARNING: a component or data class usually exposes its Blueprint API as PROPERTIES, not functions: get_class_functions coming back with nothing of its own does NOT mean the class has no API — ask here too (e.g. MetaHumanComponentBase: 0 own functions, 12 own properties). Full typed descriptor per property — kind, cpp_type, editable, edit_const, blueprint_visible, replicated, save_game, config, clamp min/max, enum entries, struct fields, tooltip, edit_condition — plus the class's own flags, config name and implemented interfaces. Schema only: NO instance values. Use get_object_info (include_property_metadata=true) for the same descriptors WITH the current values of a live object, get_properties when you already know the names, and get_blueprint_variables for just the variables a Blueprint itself authored.
//   @param class_name - UClass identifier, resolved with ResolveClass: bare FName ("Character"), /Script/Module.Class, a Blueprint asset path, or a generated class ("BP_MyActor_C"). WARNING: the REGISTERED FName can differ from the C++ type name and from the name the Python bindings expose — UMetaHumanCharacterInstance is registered as "MetaHumanInstance" — so a failure here is NOT evidence the class is absent; the error names the near misses instead. The response echoes what it resolved to in resolved_class_name / resolved_class_path.
//   @param declared_only - Optional bool (default false). true = ONLY the properties this class declares, skipping every inherited one. The cheap way to ask "what does this class add?", and what keeps a big class's response from being offloaded for size (get_class_properties("MetaHumanComponentBase") is 44 KB inherited-heavy, 12 properties declared).
//   @returns { class_name: string, resolved_class_name: string, resolved_class_path: string, declared_only: bool, declared_property_count: int, inherited_property_count: int, is_abstract, is_deprecated, is_placeable, is_native, config_name, implemented_interfaces: [{interface, implemented_by_k2}], class_group?, is_blueprintable?, is_blueprint_type?, display_name?, tooltip?, properties: [<typed descriptor: name, owner_class, is_inherited, kind, cpp_type, editable, edit_const, blueprint_visible, save_game, replicated, config, deprecated, disable_edit_on_instance, enum_values?, fields?, object_class?, inner?, clamp_min/max?, tooltip?, edit_condition?, categories?, units?, display_name?, allow_private_access>], property_count: int }

TSharedPtr<FJsonObject> HandleGetClassProperties(const TSharedPtr<FJsonObject>& Params)
{
    FString ClassName;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), ClassName, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UClass* Class = FUEAgentGraphUtils::ResolveClass(ClassName);
    if (!Class)
    {
        return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(ClassName));
    }
    // declared_only answers "what does this class add of its own?" in one call, without the inherited bulk that is
    // most of a component class's payload.
    bool bDeclaredOnly = false;
    Params->TryGetBoolField(TEXT("declared_only"), bDeclaredOnly);

    // Each property names the class that declares it; owner_class is empty when the owner is not a class (a struct
    // or a function), never invented.
    TArray<TSharedPtr<FJsonValue>> Properties;
    int32 OwnCount = 0;
    const EFieldIteratorFlags::SuperClassFlags Scope = bDeclaredOnly ? EFieldIteratorFlags::ExcludeSuper : EFieldIteratorFlags::IncludeSuper;
    for (TFieldIterator<FProperty> Field(Class, Scope); Field; ++Field)
    {
        const UClass* Owner = Field->GetOwnerClass();
        const bool bOwn = Owner == Class;
        OwnCount += bOwn ? 1 : 0;
        TSharedPtr<FJsonObject> Described = BuildPropertyDescriptor(*Field);
        Described->SetStringField(TEXT("owner_class"), Owner ? Owner->GetName() : FString());
        Described->SetBoolField(TEXT("is_inherited"), !bOwn);
        Properties.Add(MakeShared<FJsonValueObject>(Described));
    }

    // The class as the identifier RESOLVED -- which can differ from what was typed (a /Script path, an A/U prefix,
    // a Blueprint asset path), the very difference a bare "Class not found" would hide -- with its flags, metadata
    // and interfaces. The two counts answer "has this class an API of its own?" without reading the array, and
    // survive an offloaded oversize response.
    TSharedPtr<FJsonObject> Data = BuildClassDescriptor(Class);
    Data->SetStringField(TEXT("class_name"), ClassName);
    Data->SetStringField(TEXT("resolved_class_name"), Class->GetName());
    Data->SetStringField(TEXT("resolved_class_path"), Class->GetPathName());
    Data->SetBoolField(TEXT("declared_only"), bDeclaredOnly);
    Data->SetNumberField(TEXT("declared_property_count"), OwnCount);
    Data->SetNumberField(TEXT("inherited_property_count"), Properties.Num() - OwnCount);
    Data->SetNumberField(TEXT("property_count"), Properties.Num());
    Data->SetArrayField(TEXT("properties"), Properties);
    return mcp_common::MakeOkReply(Data);
}

// MCP:get_class_functions - List every UFUNCTION a class exposes — the ones it DECLARES plus every inherited one, each tagged with owner_class and is_inherited, and counted separately in declared_function_count / inherited_function_count so "does this class add a function of its own?" is answerable WITHOUT reading the array (declared_only=true returns just those) — with its signature and the flags that decide whether MCP can call it: is_blueprint_callable, is_public, is_static, is_blueprint_pure, is_blueprint_event, is_call_in_editor, category, tooltip and the parameter names/types. This is the lookup to run BEFORE call_function, which refuses anything that is not BlueprintCallable+Public or a BlueprintEvent. WARNING: ZERO own functions is NOT "no Blueprint API": components and data classes usually expose theirs as PROPERTIES (get_class_properties), and a plugin's runtime API often lives in a separate static "*BlueprintLibrary" class rather than on the type itself — both traps show up on MetaHumanComponentBase, whose creator API is in MetaHumanCharacterInstanceBlueprintLibrary. get_function_info drills into ONE function (and separates return values from inputs); get_blueprint_functions lists the graphs a Blueprint AUTHORED rather than the compiled class surface.
//   @param class_name - UClass identifier, resolved with ResolveClass: bare FName ("Character"), /Script/Module.Class, a Blueprint asset path, or a generated class ("BP_MyActor_C"). WARNING: the REGISTERED FName can differ from the C++ type name and from the name the Python bindings expose — UMetaHumanCharacterInstance is registered as "MetaHumanInstance" — so a failure here is NOT evidence the class is absent; the error names the near misses instead. The response echoes what it resolved to in resolved_class_name / resolved_class_path.
//   @param declared_only - Optional bool (default false). true = ONLY the UFUNCTIONs this class declares, skipping every inherited one.
//   @returns { class_name: string, resolved_class_name: string, resolved_class_path: string, declared_only: bool, functions: [{name, owner_class, is_inherited, is_blueprint_callable, is_blueprint_event, is_public, is_static, is_blueprint_pure, is_call_in_editor, category?, tooltip?, parameters: [{name, type}]}], function_count: int, declared_function_count: int, inherited_function_count: int }

TSharedPtr<FJsonObject> HandleGetClassFunctions(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString RequestedClass;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), RequestedClass, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UClass* Class = FUEAgentGraphUtils::ResolveClass(RequestedClass);
    if (!Class)
    {
        return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(RequestedClass));
    }

    // Same reason as in get_class_properties. A list of every function with nothing
    // saying which ones the class itself declares invites a by-eye guess ("all of these are
    // UActorComponent's, so the class has no API"). owner_class / is_inherited make it a field,
    // declared_function_count answers it without reading the array, declared_only skips the rest.
    bool bDeclaredOnly = false;
    Params->TryGetBoolField(TEXT("declared_only"), bDeclaredOnly);

    TArray<TSharedPtr<FJsonValue>> Rows;
    int32 DeclaredHere = 0;
    for (TFieldIterator<UFunction> FuncIt(Class, bDeclaredOnly ? EFieldIteratorFlags::ExcludeSuper : EFieldIteratorFlags::IncludeSuper); FuncIt; ++FuncIt)
    {
        UFunction* Function = *FuncIt;
        if (Function == nullptr)
        {
            continue;
        }
        // The class that DECLARES the function: an override reports the class it is declared
        // on, which is what "own vs inherited" means here. No owner reads as "".
        const UClass* Declarer = Function->GetOwnerClass();
        const bool bInherited = Declarer != Class;
        if (!bInherited)
        {
            ++DeclaredHere;
        }

        TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
        Row->SetStringField(TEXT("name"), Function->GetName());
        Row->SetStringField(TEXT("owner_class"), Declarer ? Declarer->GetName() : FString());
        Row->SetBoolField(TEXT("is_inherited"), bInherited);
        Impl::WriteFunctionTraits(Function, Row);
        Row->SetArrayField(TEXT("parameters"), Impl::LeadingParameterEntries(Function));
        Rows.Add(MakeShared<FJsonValueObject>(Row));
    }

    TSharedPtr<FJsonObject> Listing = MakeShared<FJsonObject>();
    Listing->SetStringField(TEXT("class_name"), RequestedClass);
    Listing->SetStringField(TEXT("resolved_class_name"), Class->GetName());
    Listing->SetStringField(TEXT("resolved_class_path"), Class->GetPathName());
    Listing->SetBoolField(TEXT("declared_only"), bDeclaredOnly);
    Listing->SetNumberField(TEXT("function_count"), Rows.Num());
    Listing->SetNumberField(TEXT("declared_function_count"), DeclaredHere);
    Listing->SetNumberField(TEXT("inherited_function_count"), Rows.Num() - DeclaredHere);
    Listing->SetArrayField(TEXT("functions"), Rows);
    return mcp_common::MakeOkReply(Listing);
}

// MCP:get_object_info - Portrait of ONE object when you do not know its property names yet: identity (name, class, class path, super class, is_valid), class flags, metadata and implemented interfaces, EVERY reflected property as { name: typed_value }, and for an actor its live components with their classes.
//   @para Use it first on anything unfamiliar, then write with set_properties using the names it lists. Once you know the names, get_properties reads just those and is far cheaper -- an actor's full portrait can run to hundreds of properties; get_class_properties describes a class with no instance, get_class_hierarchy gives its ancestor chain, and get_actor_components the components alone.
//   @para include_properties=false makes it the cheap "what IS this thing?" probe, identity and class only. include_property_metadata=true puts each property's editor metadata (type, editable, edit_const, blueprint_visible, replicated, save_game, clamp, tooltip, enum entries) next to its value, which tells you before writing whether set_properties can change it. With `world` the portrait is of the live instance in Play-in-Editor, on the server or on a client -- useful when a class is swapped at runtime -- and the reply stamps which world answered. An object it cannot find is an error naming the path and the world it searched.
//   @core
//   @renamed-from inspect_object
//   @param object_path - Path or name of the object to inspect. In the editor world: full object path, asset path, CDO/archetype, an asset's sub-object as the readers give its object_path -- a graph node from get_graph_nodes, a sound cue node, a Niagara renderer or simulation stage, a PCG node's settings_path, a behavior tree node, a camera rig node, a Mass trait, a physics body -- also written "/Game/Asset:SubObject", or a dotted "Actor[.Component[.Subobject]]" whose first segment is the actor's INTERNAL name. In a live world: a fully-qualified PIE path or the same dotted form (first segment matches internal name OR editor label).
//   @param world - Optional world selector: absent/'editor' (default: the editor/asset instance) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). Anything other than absent/'editor' portrays the LIVE instance in that world; an UNRECOGNIZED value is an error naming the worlds that exist, never a silent fallback to another world.
//   @param include_properties - Optional bool (default true). false skips the properties and the components list, leaving identity and class only.
//   @param include_property_metadata - Optional bool (default false). true returns properties as an ARRAY of typed descriptors (see get_class_properties) each with its current `value`, instead of the { name: typed_value } map.
//   @returns { object_path: string, object_name: string, class_name: string, class_path: string, super_class: string, is_valid: bool, is_abstract, is_deprecated, is_placeable, is_native, config_name, implemented_interfaces: [{interface, implemented_by_k2}], class_group?, is_blueprintable?, is_blueprint_type?, display_name?, tooltip?, properties?: {prop_name: typed_value} | [<typed descriptor + value>], property_count?: int, components?: [{name, class}], world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number } -- properties/property_count/components only with include_properties; properties is the array form with include_property_metadata; super_class is "None" for UObject.

TSharedPtr<FJsonObject> HandleInspectObject(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString TargetPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), TargetPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    bool bIncludeProperties = true;
    Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);
    bool bWithMetadata = false;
    Params->TryGetBoolField(TEXT("include_property_metadata"), bWithMetadata);

    // With a live `world` the portrait is of the LIVE instance there, so a co-op reading can be
    // taken on the server AND on a client; absent/'editor' is the editor/asset instance.
    Impl::FWorldScopedObject Target;
    TSharedPtr<FJsonObject> Refusal;
    if (!Impl::ResolveWorldScopedObject(Params, TargetPath, TEXT("get_object_info"),
            [](const FString& Path) { return ResolveObjectByName(Path); }, Target, Refusal))
    {
        return Refusal;
    }

    UObject* Subject = Target.Object;
    UClass* SubjectClass = Subject->GetClass();
    const UClass* SuperClass = SubjectClass->GetSuperClass();

    TSharedPtr<FJsonObject> Portrait = MakeShared<FJsonObject>();
    Portrait->SetStringField(TEXT("object_path"), TargetPath);
    Portrait->SetStringField(TEXT("object_name"), Subject->GetName());
    Portrait->SetStringField(TEXT("class_name"), SubjectClass->GetName());
    Portrait->SetStringField(TEXT("class_path"), SubjectClass->GetPathName());
    Portrait->SetStringField(TEXT("super_class"), SuperClass ? SuperClass->GetName() : FString(TEXT("None")));
    Portrait->SetBoolField(TEXT("is_valid"), IsValid(Subject));
    Portrait->SetStringField(TEXT("world"), Target.WorldLabel);
    // WHICH world this portrait came from. Two portraits that disagree are a replication
    // finding only when they are known to come from different worlds.
    mcp_common::AddWorldEvidence(Portrait, Target.World);

    // Class-level flags / metadata / implemented interfaces of the instance's class.
    TSharedPtr<FJsonObject> ClassDesc = BuildClassDescriptor(SubjectClass);
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ClassDesc->Values) { Portrait->SetField(Pair.Key, Pair.Value); }

    if (!bIncludeProperties)
    {
        return mcp_common::MakeOkReply(Portrait);
    }
    if (bWithMetadata)
    {
        // Every reflected property: the full typed descriptor plus its current typed value.
        TArray<TSharedPtr<FJsonValue>> Described;
        for (TFieldIterator<FProperty> PropIt(SubjectClass); PropIt; ++PropIt)
        {
            FProperty* Property = *PropIt;
            if (Property == nullptr)
            {
                continue;
            }
            TSharedPtr<FJsonObject> Descriptor = BuildPropertyDescriptor(Property);
            Descriptor->SetField(TEXT("value"), mcp_common::PropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(Subject)));
            Described.Add(MakeShared<FJsonValueObject>(Descriptor));
        }
        Portrait->SetArrayField(TEXT("properties"), Described);
        Portrait->SetNumberField(TEXT("property_count"), Described.Num());
    }
    else
    {
        TSharedPtr<FJsonObject> Values = Impl::PropertyValueMap(SubjectClass, Subject);
        Portrait->SetNumberField(TEXT("property_count"), Values->Values.Num());
        Portrait->SetObjectField(TEXT("properties"), Values);
    }
    if (AActor* SubjectActor = Cast<AActor>(Subject))
    {
        Portrait->SetArrayField(TEXT("components"), Impl::ComponentSummaries(SubjectActor));
    }
    return mcp_common::MakeOkReply(Portrait);
}

// MCP:get_class_hierarchy - Walk the full inheritance chain of a class from itself up to UObject, one record per level with its path, is_native flag, class flags/metadata and implemented interfaces — the answer to "is X a Y?" and "where does the native code stop and the Blueprint chain start?". For a USubsystem class it also names the subsystem lifetime (Engine, Editor, GameInstance, World or LocalPlayer), at the top and per level. get_blueprint_parent returns only the IMMEDIATE parent of a Blueprint ASSET; this returns every ancestor of a compiled CLASS. list_derived_classes goes the other direction (descendants).
//   @param class_name - UClass identifier, resolved with ResolveClass: bare FName ("Character"), /Script/Module.Class, a Blueprint asset path, or a generated class ("BP_MyActor_C"). WARNING: the REGISTERED FName can differ from the C++ type name and from the name the Python bindings expose — UMetaHumanCharacterInstance is registered as "MetaHumanInstance" — so a failure here is NOT evidence the class is absent; the error names the near misses instead.
//   @returns { class_name: string, lifetime?: string, hierarchy: [{name, path, is_native, lifetime?, is_abstract, is_deprecated, is_placeable, config_name, implemented_interfaces: [{interface, implemented_by_k2}], class_group?, is_blueprintable?, is_blueprint_type?, display_name?, tooltip?}] } -- lifetime only on a USubsystem class and on its levels that are USubsystem classes ("Unknown" for the abstract USubsystem bases).

TSharedPtr<FJsonObject> HandleGetClassHierarchy(const TSharedPtr<FJsonObject>& Params)
{
    FString RequestedClass;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), RequestedClass, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    UClass* Leaf = FUEAgentGraphUtils::ResolveClass(RequestedClass);
    if (!Leaf)
    {
        return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(RequestedClass));
    }

    // Leaf first, then every super class up to UObject. Each level carries its own class
    // descriptor (flags, metadata, implemented interfaces), not just the leaf.
    TArray<TSharedPtr<FJsonValue>> Chain;
    for (UClass* Level = Leaf; Level != nullptr; Level = Level->GetSuperClass())
    {
        TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("name"), Level->GetName());
        Entry->SetStringField(TEXT("path"), Level->GetPathName());
        Entry->SetBoolField(TEXT("is_native"), Level->IsNative());
        if (Level->IsChildOf(USubsystem::StaticClass()))
        {
            Entry->SetStringField(TEXT("lifetime"), UEAgentSubsystemLifetime::DetectLifetimeCategory(Level));
        }
        TSharedPtr<FJsonObject> ClassDesc = BuildClassDescriptor(Level);
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : ClassDesc->Values) { Entry->SetField(Pair.Key, Pair.Value); }
        Chain.Add(MakeShared<FJsonValueObject>(Entry));
    }

    TSharedPtr<FJsonObject> Lineage = MakeShared<FJsonObject>();
    Lineage->SetStringField(TEXT("class_name"), RequestedClass);
    if (Leaf->IsChildOf(USubsystem::StaticClass()))
    {
        Lineage->SetStringField(TEXT("lifetime"), UEAgentSubsystemLifetime::DetectLifetimeCategory(Leaf));
    }
    Lineage->SetArrayField(TEXT("hierarchy"), Chain);
    return mcp_common::MakeOkReply(Lineage);
}

// Single-property reads and writes on one object.

// ---------------------------------------------------------------------------
// HARD OBJECT ARGUMENTS BY PATH.
//
// A UObject* / TSubclassOf parameter passed as a path string is resolved here, never handed to
// FProperty::ImportText_Direct. That path CANNOT report its own failure:
// FObjectPropertyBase::ImportText_Internal computes `bool bOk = ParseObjectPropertyValue(...)` and
// then NEVER READS IT (PropertyBaseObject.cpp), writing the (possibly null) result and returning
// a non-null Buffer regardless -- so a `== nullptr` check on it can never fire. The argument would
// arrive as null, ProcessEvent would run anyway, and the callee would return its default: a boolean
// query taking an object argument would answer false to the negative AND the positive control
// alike, which reads as a RESULT.
//
// Two independent failure shapes, both silent:
//   - full PIE path  -> StaticFindObject misses the live object -> null
//   - bare name      -> resolves to the EDITOR-world actor, so the call runs on the wrong world
// So the argument is resolved like the target: in the world of the object being called, which
// keeps argument and target in the SAME world by construction, falling back to the global
// asset/CDO resolver for assets and class references.
enum class EReflObjectArg : uint8 { NotApplicable, Imported, Failed };

static UObject* ReflectionPie_ResolveObjectInWorld(const FString& Path, UWorld* World); // fwd (defined below)

static EReflObjectArg ReflectionPie_ImportObjectArgument(
    FProperty* ParamProp,
    void* ParamPtr,
    const TSharedPtr<FJsonValue>& Value,
    const FString& ParamName,
    UWorld* PreferredWorld,
    FString& OutError)
{
    FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(ParamProp);
    if (!ObjProp)
    {
        return EReflObjectArg::NotApplicable;
    }
    // SOFT refs (FSoftObjectProperty / FSoftClassProperty) are paths BY DEFINITION: ImportText stores
    // the string without resolving anything, which is the correct behaviour. Leave them alone.
    if (CastField<FSoftObjectProperty>(ObjProp))
    {
        return EReflObjectArg::NotApplicable;
    }

    const EJson JsonType = Value.IsValid() ? Value->Type : EJson::None;

    if (JsonType == EJson::Null || JsonType == EJson::None)
    {
        ObjProp->SetObjectPropertyValue(ParamPtr, nullptr);
        return EReflObjectArg::Imported;
    }
    if (JsonType != EJson::String)
    {
        OutError = FString::Printf(
            TEXT("Parameter '%s' is a hard object reference (%s): pass an object PATH string, or null to clear it"),
            *ParamName, *ParamProp->GetClass()->GetName());
        return EReflObjectArg::Failed;
    }

    FString RawPath = Value->AsString();
    RawPath.TrimStartAndEndInline();
    if (RawPath.IsEmpty() || RawPath.Equals(TEXT("None"), ESearchCase::IgnoreCase))
    {
        ObjProp->SetObjectPropertyValue(ParamPtr, nullptr);
        return EReflObjectArg::Imported;
    }

    // 1) the live object in the target's own world (handles full PIE paths and dotted
    //    "Actor.Component" forms), then 2) the global asset / CDO resolver.
    UObject* Resolved = nullptr;
    if (PreferredWorld)
    {
        Resolved = ReflectionPie_ResolveObjectInWorld(RawPath, PreferredWorld);
    }
    if (!Resolved)
    {
        Resolved = mcp_common::ResolveObjectByPath(RawPath, nullptr);
    }
    if (!Resolved)
    {
        OutError = FString::Printf(
            TEXT("Parameter '%s': could not resolve '%s' to any UObject%s. Passing it on would call the ")
            TEXT("function with NULL and make it return a wrong answer for the wrong reason."),
            *ParamName, *RawPath,
            PreferredWorld ? TEXT(" (searched the target's world, then the asset registry)") : TEXT(""));
        return EReflObjectArg::Failed;
    }

    // TSubclassOf / UClass* parameters: a Blueprint ASSET path must become its GeneratedClass, which
    // is the translation ImportText_Direct never does (hence "pass /Game/X.X_C" folklore).
    if (FClassProperty* ClassProp = CastField<FClassProperty>(ObjProp))
    {
        UClass* AsClass = Cast<UClass>(Resolved);
        if (!AsClass)
        {
            if (const UBlueprint* AsBlueprint = Cast<UBlueprint>(Resolved))
            {
                AsClass = AsBlueprint->GeneratedClass;
            }
        }
        if (!AsClass)
        {
            OutError = FString::Printf(
                TEXT("Parameter '%s' expects a class, but '%s' resolved to a %s"),
                *ParamName, *RawPath, *Resolved->GetClass()->GetName());
            return EReflObjectArg::Failed;
        }
        if (ClassProp->MetaClass && !AsClass->IsChildOf(ClassProp->MetaClass))
        {
            OutError = FString::Printf(
                TEXT("Parameter '%s' expects a subclass of %s, but '%s' is a %s"),
                *ParamName, *ClassProp->MetaClass->GetName(), *RawPath, *AsClass->GetName());
            return EReflObjectArg::Failed;
        }
        ClassProp->SetObjectPropertyValue(ParamPtr, AsClass);
        return EReflObjectArg::Imported;
    }

    if (ObjProp->PropertyClass && !Resolved->IsA(ObjProp->PropertyClass))
    {
        OutError = FString::Printf(
            TEXT("Parameter '%s' expects a %s, but '%s' resolved to a %s"),
            *ParamName, *ObjProp->PropertyClass->GetName(), *RawPath, *Resolved->GetClass()->GetName());
        return EReflObjectArg::Failed;
    }

    ObjProp->SetObjectPropertyValue(ParamPtr, Resolved);
    return EReflObjectArg::Imported;
}

// Convert a JSON SCALAR value into the UE property-text form consumed by FProperty::ImportText_Direct
// (string/number/bool). Object/array JSON is handled BEFORE this by the caller via
// FJsonObjectConverter::JsonValueToUProperty (structs/containers/nested) — this helper only ever sees
// scalars, and it refuses object/array honestly (they can't reach ImportText anyway).
static bool JsonValueToPropertyText(const TSharedPtr<FJsonValue>& Value, FString& OutText, FString& OutError)
{
    if (!Value.IsValid())
    {
        OutError = TEXT("null value");
        return false;
    }

    switch (Value->Type)
    {
    case EJson::String:
        OutText = Value->AsString();
        return true;
    case EJson::Boolean:
        OutText = Value->AsBool() ? TEXT("true") : TEXT("false");
        return true;
    case EJson::Number:
    {
        const double Number = Value->AsNumber();
        if (Number == FMath::TruncToDouble(Number) && FMath::Abs(Number) < 9.007199254740992e15)
        {
            OutText = FString::Printf(TEXT("%lld"), static_cast<int64>(Number));
        }
        else
        {
            OutText = FString::SanitizeFloat(Number);
        }
        return true;
    }
    default:
        OutError = TEXT("object/array values are not supported; pass struct/container parameters as a UE-format text string (e.g. \"(X=1,Y=2,Z=3)\")");
        return false;
    }
}

// Pack a JSON {param_name: value} dict into Function's parameter frame, the marshaling every call_*
// handler shares. Every key must name a real input parameter and import cleanly; otherwise this fails
// with OutError naming it instead of silently dropping it. FunctionLabel is the name the errors quote.
static bool ReflectionPie_MarshalArguments(
    UObject* Object,
    UFunction* Function,
    const TSharedPtr<FJsonObject>& Arguments,
    const FString& FunctionLabel,
    uint8* Frame,
    FString& OutError)
{
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Arguments->Values)
    {
        FProperty* ParamProp = Function->FindPropertyByName(*Pair.Key);
        if (!ParamProp || !ParamProp->HasAnyPropertyFlags(CPF_Parm) || ParamProp->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            OutError = FString::Printf(TEXT("Unknown input parameter '%s' for function '%s'"), *Pair.Key, *FunctionLabel);
            return false;
        }

        void* ParamPtr = ParamProp->ContainerPtrToValuePtr<void>(Frame);
        const EJson JsonType = Pair.Value.IsValid() ? Pair.Value->Type : EJson::None;

        // A single FGameplayTag parameter is VALIDATED against the tag registry
        // (mirror of mcp_common::SetObjectProperty). Accept EITHER {"TagName":"A.B"} OR a bare
        // "A.B" string; JsonValueToUProperty would otherwise write the internal FName WITHOUT
        // validation (a typo'd/unregistered tag is dead-set silently) and a bare string does
        // not reliably construct the tag. FGameplayTagContainer is left to the paths below.
        if (const FStructProperty* TagStructProp = CastField<FStructProperty>(ParamProp))
        {
            if (TagStructProp->Struct == FGameplayTag::StaticStruct())
            {
                FString TagName;
                if (JsonType == EJson::String)
                {
                    TagName = Pair.Value->AsString();
                }
                else if (JsonType == EJson::Object)
                {
                    const TSharedPtr<FJsonObject> TagObj = Pair.Value->AsObject();
                    if (TagObj.IsValid()) { TagObj->TryGetStringField(TEXT("TagName"), TagName); }
                }
                TagName.TrimStartAndEndInline();

                if (TagName.IsEmpty())
                {
                    *static_cast<FGameplayTag*>(ParamPtr) = FGameplayTag();
                }
                else
                {
                    const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound=*/ false);
                    if (!Tag.IsValid())
                    {
                        OutError = FString::Printf(
                            TEXT("Parameter '%s': unregistered gameplay tag '%s' (not in the tag registry). ")
                            TEXT("Register it (create_gameplay_tag / a tag table) or fix the spelling; refusing to dead-set an invalid tag."),
                            *Pair.Key, *TagName);
                        return false;
                    }
                    *static_cast<FGameplayTag*>(ParamPtr) = Tag;
                }
                continue; // handled — skip the generic object/array + scalar import below
            }
        }

        // FGuid marshaling: an FGuid parameter passed as a STRING (its textual form —
        // 32 hex digits, or the "A-B-C-D"/braced variants FGuid::Parse accepts) does NOT
        // round-trip through the paths below: ImportText_Direct expects the struct-text form
        // "(A=..,B=..,C=..,D=..)", and FJsonObjectConverter has no string->FGuid path either.
        // Parse it explicitly with FGuid::Parse BEFORE the generic marshaling so functions whose
        // params are FGuid structs land. An empty/absent string leaves the GUID default-init
        // (all-zero). A JSON object {"A":..,"B":..,"C":..,"D":..} still falls through to
        // JsonValueToUProperty below (which walks the subfields, default-init'ing any absent one).
        if (const FStructProperty* GuidStructProp = CastField<FStructProperty>(ParamProp))
        {
            if (GuidStructProp->Struct == TBaseStructure<FGuid>::Get() && JsonType == EJson::String)
            {
                const FString GuidStr = Pair.Value->AsString();
                if (GuidStr.TrimStartAndEnd().IsEmpty())
                {
                    *static_cast<FGuid*>(ParamPtr) = FGuid();
                }
                else
                {
                    FGuid ParsedGuid;
                    if (!FGuid::Parse(GuidStr, ParsedGuid))
                    {
                        OutError = FString::Printf(
                            TEXT("Parameter '%s': '%s' is not a parseable FGuid (expected 32 hex digits or a hyphenated/braced GUID string)"),
                            *Pair.Key, *GuidStr);
                        return false;
                    }
                    *static_cast<FGuid*>(ParamPtr) = ParsedGuid;
                }
                continue; // handled — skip the generic object/array + scalar import below
            }
        }

        // Hard object / class arguments, resolved in the TARGET'S OWN WORLD (see
        // ReflectionPie_ImportObjectArgument): ImportText_Direct cannot fail on these, so without
        // this branch a bad path would arrive as a silent null.
        {
            const EReflObjectArg ObjArg = ReflectionPie_ImportObjectArgument(
                ParamProp, ParamPtr, Pair.Value, Pair.Key, Object->GetWorld(), OutError);
            if (ObjArg == EReflObjectArg::Failed)
            {
                return false;
            }
            if (ObjArg == EReflObjectArg::Imported)
            {
                continue;
            }
        }

        if (JsonType == EJson::Object || JsonType == EJson::Array)
        {
            // Struct / container params: nativize the JSON straight into the property via
            // FJsonObjectConverter (handles nested BP structs, arrays of structs, maps, and
            // enum-by-name inside structs — JSON keys match property AUTHORED names, so BP
            // struct fields with GUID-mangled internal names still resolve). ImportText_Direct
            // can't parse JSON struct syntax; this is what allows building a BP struct value.
            FText FailReason;
            if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, ParamProp, ParamPtr, 0, 0, false, &FailReason))
            {
                OutError = FString::Printf(
                    TEXT("Parameter '%s' (type %s): could not nativize JSON %s (%s)"),
                    *Pair.Key, *ParamProp->GetClass()->GetName(),
                    JsonType == EJson::Object ? TEXT("object") : TEXT("array"),
                    *FailReason.ToString());
                return false;
            }
        }
        else
        {
            // Scalars: text form + ImportText (enum by its entry NAME as a string e.g.
            // "Harvest", numbers, bools; a UE-format struct text like "(X=1,Y=2,Z=3)" also
            // works here).
            // Object refs are NOT handled here but above: ImportText_Direct writes null without
            // ever reporting a failure.
            FString ImportText;
            FString ConvertError;
            if (!JsonValueToPropertyText(Pair.Value, ImportText, ConvertError))
            {
                OutError = FString::Printf(TEXT("Parameter '%s': %s"), *Pair.Key, *ConvertError);
                return false;
            }
            FString ParseError;
            if (!mcp_common::ImportPropertyTextStrict(ParamProp, ImportText, ParamPtr, Object, ParseError))
            {
                OutError = FString::Printf(TEXT("Failed to import into parameter '%s': %s"), *Pair.Key, *ParseError);
                return false;
            }
        }
    }
    return true;
}

// UFUNCTION calls on an editor-world object, and one function's call contract.

// MCP:call_function - Invoke a UFUNCTION and get its return value and out-params back typed. The target is any UObject named by object_path -- a level actor, component archetype, class default object, asset, or a LIVE PIE subobject when `world` picks a live world (the response stamps which world ran it) -- or, with class_name instead, that class's Class Default Object (the way to reach UKismetSystemLibrary, UGameplayStatics or any Blueprint Function Library when there is no instance), or, with component_class, the FIRST component of that class on the actor object_path names (no hand-built component path; in co-op, world='server' is where authoritative component state lives). Arguments go in the `parameters` dict, keyed by parameter name (an unknown key is a hard error, so read the signature with get_function_info first). Marshals scalars, ENUM by entry name, Blueprint STRUCTs as JSON objects, ARRAY/set as JSON arrays, FGameplayTag (validated against the registry), FGuid, and OBJECT/class arguments by path (resolved in the target's own world first, then as an asset; a path that resolves to nothing is a hard error, never a silent null). Only BlueprintCallable+Public or BlueprintEvent functions are callable; pass transact=true to make the write undoable and actually dirty an OFPA package.
//   @param object_path - Path or name of the target object: full object path, asset path, CDO/archetype, or a dotted "Actor[.Component[.Subobject]]" (first segment: the actor's internal name in the editor world, internal name OR editor label in a live world). With component_class it names the ACTOR whose component is called. Required unless class_name is given.
//   @param actor - Alias of object_path.
//   @param class_name - Optional, instead of object_path: a class NAME (bare/native e.g. "KismetSystemLibrary", /Script path, or a Blueprint asset path -- a Blueprint path yields its GeneratedClass); the function runs on that class's Class Default Object, with no `world`.
//   @param component_class - Optional component CLASS (bare/native name, /Script path, or Blueprint asset path) to locate on the actor object_path names; the FIRST component of (or derived from) that class is the target. Must derive from UActorComponent.
//   @param function_name - Name of the function to call, exactly as declared (a Blueprint graph named "Add Item" keeps its spaces)
//   @param function - Alias of function_name.
//   @param parameters [object] - Optional {param_name: value} dict. Scalars map directly; pass an ENUM by its entry name as a string (e.g. "Harvest"); pass a STRUCT param as a JSON object {Field: val, ...} and an ARRAY/set as a JSON array (nativized via FJsonObjectConverter — nested structs + enum-by-name inside structs supported; JSON keys match the field's authored name); FGameplayTag as {"TagName":"A.B"}; FGuid as a hex/braced string. A UE-format text string (e.g. "(X=1,Y=2,Z=3)") is also accepted for structs.
//   @param args_json [object] - Alias of parameters; like parameters it also takes the dict as JSON text.
//   @param params [object] - Alias of parameters.
//   @param world - Optional world selector (takes precedence over use_pie_world): absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). Anything other than absent/'editor' resolves object_path in that live world; an unrecognised value is an error naming the worlds that exist. Ignored with class_name.
//   @param use_pie_world [bool] - Optional (default false). Resolve object_path in the running PIE world instead of the editor world. Equivalent to world='pie'; `world` wins when both are given.
//   @param transact [bool] - Optional (default false). When true, wrap the invocation in an FScopedTransaction and Modify() the target (and, for a component, its owner) BEFORE calling, so whatever the function writes is undoable AND marks the owning package dirty. Without this, a CallInEditor-style function that fills state on a World Partition (OFPA) actor leaves the package clean and save_level reports success while writing nothing. REFUSED for pure/const functions and for PIE-world targets (both would only produce a false-positive dirty flag).
//   @returns { object_path: string, function_name: string, success: bool, message: string, return_value: any (typed: bool/number/string natively, struct/map as object, array/set as array, object-ref as path string), out_params: object (same typed values), world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number, class_name?: string, class_path?: string, actor?: string, component_class?: string, component_path?: string, transaction?: { requested, modified: [path], saved_to_transaction_buffer, is_package_external, package_name, package_file, package_file_exists, package_dirty_before, package_dirty_after }, verification?: object (only when transact=true) } -- object_path echoes the request (with class_name, the class default object's path); class_name/class_path with class_name; actor/component_class/component_path with component_class.

TSharedPtr<FJsonObject> HandleCallFunction(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString RequestedFunction;
    if (!Params->TryGetStringField(TEXT("function_name"), RequestedFunction) && !Params->TryGetStringField(TEXT("function"), RequestedFunction))
    {
        return mcp_common::MissingParameterReply(TEXT("function_name"));
    }
    // The target: object_path (alias actor), or class_name for a class default object.
    FString TargetPath;
    const bool bHasPath = (Params->TryGetStringField(TEXT("object_path"), TargetPath) || Params->TryGetStringField(TEXT("actor"), TargetPath))
        && !TargetPath.IsEmpty();
    FString RequestedClass;
    const bool bHasClass = Params->TryGetStringField(TEXT("class_name"), RequestedClass) && !RequestedClass.IsEmpty();
    FString ComponentClassName;
    const bool bHasComponent = Params->TryGetStringField(TEXT("component_class"), ComponentClassName) && !ComponentClassName.IsEmpty();
    // `world` wins over `use_pie_world`, a bool that can only say "some PIE world", never WHICH one:
    // use_pie_world=true without `world` resolves like world='pie'.
    bool bUsePieWorld = false;
    Params->TryGetBoolField(TEXT("use_pie_world"), bUsePieWorld);
    if (bHasPath == bHasClass)
    {
        return mcp_common::MakeErrorReply(bHasPath
            ? TEXT("Pass object_path or class_name, not both")
            : TEXT("Missing required parameter: object_path (or class_name for a class default object)"));
    }
    if (bHasClass && bHasComponent)
    {
        return mcp_common::MakeErrorReply(TEXT("component_class needs object_path naming the actor, not class_name"));
    }

    // The object the function runs on, and the world that answered (null for an asset, CDO or archetype).
    UObject* Object = nullptr;
    UWorld* AnsweringWorld = nullptr;
    FString WorldLabel = TEXT("editor");
    TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
    if (bHasClass)
    {
        // ResolveClass takes a bare name, a /Script path or a Blueprint asset path (returning its GeneratedClass);
        // the call runs on that class's default object, which a hand-built "Default__<Class>_C" path addresses.
        UClass* LibraryClass = FUEAgentGraphUtils::ResolveClass(RequestedClass);
        if (!LibraryClass)
        {
            return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(RequestedClass));
        }
        Object = LibraryClass->GetDefaultObject();
        if (!Object)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Failed to get class default object for: %s"), *LibraryClass->GetName()));
        }
        Identity->SetStringField(TEXT("class_name"), RequestedClass);
        Identity->SetStringField(TEXT("class_path"), LibraryClass->GetPathName());
    }
    else
    {
        Impl::FWorldScopedObject Resolved;
        TSharedPtr<FJsonObject> Refusal;
        if (!Impl::ResolveWorldScopedObject(Params, TargetPath, TEXT("call_function"),
                [](const FString& Path) { return ResolveObjectByName(Path); }, Resolved, Refusal, bUsePieWorld))
        {
            return Refusal;
        }
        Object = Resolved.Object;
        AnsweringWorld = Resolved.World;
        WorldLabel = Resolved.WorldLabel;
    }

    if (bHasComponent)
    {
        AActor* Actor = Cast<AActor>(Object);
        if (!Actor)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("'%s' is not an actor (class %s); component_class needs object_path to name the actor"), *TargetPath, *Object->GetClass()->GetName()));
        }
        UClass* ComponentClass = FUEAgentGraphUtils::ResolveClass(ComponentClassName);
        if (!ComponentClass)
        {
            return mcp_common::MakeErrorReply(FString::Printf(TEXT("Component class not found: %s"), *ComponentClassName));
        }
        if (!ComponentClass->IsChildOf(UActorComponent::StaticClass()))
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("'%s' is not a UActorComponent subclass (component_class must name an actor component type)"), *ComponentClass->GetName()));
        }
        UActorComponent* Component = Actor->FindComponentByClass(ComponentClass);
        if (!Component)
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("Actor '%s' (class %s) has no component of class '%s'"), *Actor->GetName(), *Actor->GetClass()->GetName(), *ComponentClass->GetName()));
        }
        Identity->SetStringField(TEXT("actor"), Actor->GetPathName());
        Identity->SetStringField(TEXT("component_class"), ComponentClass->GetName());
        Identity->SetStringField(TEXT("component_path"), Component->GetPathName());
        Object = Component;
    }

    UFunction* Function = Object->FindFunction(*RequestedFunction);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function not found: %s on %s (class %s)"),
            *RequestedFunction, *Object->GetName(), *Object->GetClass()->GetName()));
    }

    // Honest gate: only functions exposed to scripting are invoked.
    const FString NotCallable = Impl::NotScriptCallableReason(Function, RequestedFunction);
    if (!NotCallable.IsEmpty())
    {
        return mcp_common::MakeErrorReply(NotCallable);
    }

    // `transact` is opt-in (false = the plain call). The meaningless cases are refused
    // BEFORE any argument is marshaled, so a bad request costs nothing.
    bool bTransact = false;
    Params->TryGetBoolField(TEXT("transact"), bTransact);
    FString RefuseReason;
    if (bTransact && ReflectionTransact_Refuse(Object, Function, RefuseReason))
    {
        return mcp_common::MakeErrorReply(RefuseReason);
    }

    // The function's parameter frame (a UFunction is a UStruct describing that layout),
    // default-constructed: an argument that is not provided keeps its default value.
    FStructOnScope ArgumentFrame(Function);
    uint8* Frame = ArgumentFrame.GetStructMemory();

    // Optional {param_name: value} arguments, marshaled into the frame. args_json and params are older names, and
    // the object may also come as its JSON text (the form args_json took): text that is not a JSON object is refused,
    // never ignored, or the call would run with every argument left at its default.
    TSharedPtr<FJsonObject> Arguments;
    for (const TCHAR* Key : { TEXT("parameters"), TEXT("args_json"), TEXT("params") })
    {
        const TSharedPtr<FJsonValue> Value = Params->TryGetField(FStringView(Key));
        if (!Value.IsValid() || Value->Type == EJson::Null)
        {
            continue;
        }
        if (Value->Type == EJson::Object)
        {
            Arguments = Value->AsObject();
        }
        else if (Value->Type == EJson::String)
        {
            const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Value->AsString());
            if (!FJsonSerializer::Deserialize(Reader, Arguments) || !Arguments.IsValid())
            {
                return mcp_common::MakeErrorReply(FString::Printf(
                    TEXT("%s is a string that is not a JSON object; pass {param_name: value} (as an object or its JSON text)"), Key));
            }
        }
        else
        {
            return mcp_common::MakeErrorReply(FString::Printf(
                TEXT("%s must be a {param_name: value} object (or its JSON text)"), Key));
        }
        break;
    }
    if (Arguments.IsValid())
    {
        FString MarshalError;
        if (!ReflectionPie_MarshalArguments(Object, Function, Arguments, RequestedFunction, Frame, MarshalError))
        {
            return mcp_common::MakeErrorReply(MarshalError);
        }
    }

    // AActor::ProcessEvent (Actor.cpp) silently no-ops -- does NOT
    // call Super::ProcessEvent at all, no error, Frame left untouched -- unless
    // World->AreActorsInitialized() (true only once a world has begun play, e.g. PIE) OR
    // bAllowScriptExecution (GAllowActorScriptExecutionInEditor || CallInEditor metadata ||
    // FUNC_UbergraphFunction) OR the object is a CDO. In the plain (non-PIE) editor world
    // AreActorsInitialized() is false, so a call_function invocation of a non-CallInEditor
    // AActor instance method would silently do nothing -- Frame stays zero-initialized, and
    // call_function cannot detect the no-op (ProcessEvent returns void), so it would report
    // success:true with a garbage return_value regardless. GAllowActorScriptExecutionInEditor
    // (CoreGlobals.h, doc comment: "allow AActor object to execute script in the editor from
    // specific entry points") is the engine's own sanctioned escape hatch for exactly this --
    // same TGuardValue idiom FunctionalTestingManager.cpp uses to tick an editor-placed actor.
    // Harmless in PIE, where AreActorsInitialized() is already true.
    TGuardValue<bool> AllowActorScriptExecutionGuard(GAllowActorScriptExecutionInEditor, true);

    // Transact — open the undo bracket and snapshot/dirty the target BEFORE the call.
    Impl::FCallTransaction Transaction;
    if (bTransact)
    {
        Transaction.Open(Object, TEXT("call_function"), RequestedFunction);
    }

    Object->ProcessEvent(Function, Frame);

    TSharedPtr<FJsonObject> Call = MakeShared<FJsonObject>();
    // object_path echoes the request, as the caller wrote it; with class_name it is the class default object's path.
    Call->SetStringField(TEXT("object_path"), bHasPath ? TargetPath : Object->GetPathName());
    Call->SetStringField(TEXT("function_name"), RequestedFunction);
    Call->SetBoolField(TEXT("success"), true);
    Call->SetStringField(TEXT("message"), TEXT("Function called successfully"));
    for (const auto& Field : Identity->Values)
    {
        Call->SetField(Field.Key, Field.Value);
    }
    // The return value and the out-params, typed, exactly as ProcessEvent left them.
    Impl::ReadBackCallFrame(Function, Frame, Call);
    // WHICH world ran the function -- "pie" alone is ambiguous with two or more instances.
    Call->SetStringField(TEXT("world"), WorldLabel);
    mcp_common::AddWorldEvidence(Call, AnsweringWorld);

    if (!bTransact)
    {
        return mcp_common::MakeOkReply(Call);
    }

    FString VerificationDetail;
    const mcp_common::EMcpVerification Verification = Transaction.Close(Object, Call, VerificationDetail);
    return mcp_common::MakeVerifiedReply(
        Call, /*bSuccess=*/true, Verification,
        TEXT("UPackage::IsDirty() re-read after FScopedTransaction + Modify()"), VerificationDetail);
}

// MCP:get_function_info - Inspect ONE UFUNCTION on a class and get its exact call contract: input parameters and RETURN/out values split into separate lists (get_class_functions lumps them together), plus is_blueprint_callable / is_public / is_static / is_blueprint_pure / is_call_in_editor, category and tooltip. Run it to learn the PARAMETER NAMES to put in call_function's `parameters` — that dict is keyed by name and an unknown key is a hard error.
//   @param class_name - Bare UClass name resolved with FindFirstObject (e.g. "KismetSystemLibrary", "BP_MyActor_C"); an asset path is NOT accepted here
//   @param function_name - Name of the function to inspect, exactly as the class declares it (a Blueprint graph named "Add Item" keeps its spaces)
//   @returns { class_name: string, function_name: string, is_blueprint_callable: bool, is_blueprint_event: bool, is_public: bool, is_static: bool, is_blueprint_pure: bool, is_call_in_editor: bool, category?, tooltip?, parameters: [{name, type}], returns: [{name, type}] }

TSharedPtr<FJsonObject> HandleGetFunctionInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString RequestedClass;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("class_name"), RequestedClass, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString RequestedFunction;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("function_name"), RequestedFunction, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    UClass* Owner = FUEAgentGraphUtils::ResolveClass(RequestedClass);
    if (!Owner)
    {
        return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(RequestedClass));
    }
    UFunction* Function = Owner->FindFunctionByName(*RequestedFunction);
    if (!Function)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Function not found: %s"), *RequestedFunction));
    }

    // Unlike get_class_functions, the return value is split out of the inputs: every
    // CPF_ReturnParm property goes to "returns", every other CPF_Parm one to "parameters".
    TArray<TSharedPtr<FJsonValue>> Inputs;
    TArray<TSharedPtr<FJsonValue>> Outputs;
    for (TFieldIterator<FProperty> PropIt(Function); PropIt; ++PropIt)
    {
        const FProperty* Member = *PropIt;
        if (Member == nullptr)
        {
            continue;
        }
        if (Member->HasAnyPropertyFlags(CPF_ReturnParm))
        {
            Outputs.Add(Impl::ParameterEntry(Member));
        }
        else if (Member->HasAnyPropertyFlags(CPF_Parm))
        {
            Inputs.Add(Impl::ParameterEntry(Member));
        }
    }

    TSharedPtr<FJsonObject> Contract = MakeShared<FJsonObject>();
    Contract->SetStringField(TEXT("class_name"), RequestedClass);
    Contract->SetStringField(TEXT("function_name"), RequestedFunction);
    Impl::WriteFunctionTraits(Function, Contract);
    Contract->SetArrayField(TEXT("parameters"), Inputs);
    Contract->SetArrayField(TEXT("returns"), Outputs);
    return mcp_common::MakeOkReply(Contract);
}

// Definitions of a UScriptStruct and of a UEnum, the contents of a loaded package, and the property diff of
// two live objects.

// MCP:get_struct_info - Read the definition of a UScriptStruct (native or Blueprint-authored): its path and every member property as a typed descriptor. Use it to learn the FIELD NAMES before passing a struct as a JSON object to call_function / set_properties, or to read the row layout behind a DataTable. get_struct_members is the Blueprint-struct-authoring view of the same asset; get_enum_info is the enum equivalent.
//   @param struct_name - Bare struct name resolved with FindFirstObject (e.g. "Vector", "S_ItemStructure"); an asset path is NOT accepted here
//   @returns { struct_name: string, struct_path: string, properties: [<typed property descriptor>] }

TSharedPtr<FJsonObject> HandleGetStructInfo(const TSharedPtr<FJsonObject>& Params)
{
    FString RequestedStruct;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("struct_name"), RequestedStruct, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Bare name only (FindFirstObject over loaded structs); an asset path is not resolved.
    const UScriptStruct* Found = FindFirstObject<UScriptStruct>(*RequestedStruct);
    if (Found == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Struct not found: %s"), *RequestedStruct));
    }

    TArray<TSharedPtr<FJsonValue>> Members;
    for (TFieldIterator<FProperty> PropIt(Found); PropIt; ++PropIt)
    {
        if (FProperty* Member = *PropIt)
        {
            Members.Add(MakeShared<FJsonValueObject>(BuildPropertyDescriptor(Member)));
        }
    }

    TSharedPtr<FJsonObject> Layout = MakeShared<FJsonObject>();
    Layout->SetStringField(TEXT("struct_name"), RequestedStruct);
    Layout->SetStringField(TEXT("struct_path"), Found->GetPathName());
    Layout->SetArrayField(TEXT("properties"), Members);
    return mcp_common::MakeOkReply(Layout);
}

// MCP:get_enum_info - Read an enum's definition — native or Blueprint (UUserDefinedEnum) — listing every entry (the _MAX sentinel excluded) with index, internal_name, display_name and numeric value. WARNING: the two names DIVERGE on a Blueprint enum: the editor shows the display_name ("Snowy") while the authored entry is the opaque internal_name ("NewEnumerator5"), and the INTERNAL name is what set_properties / set_component_property / call_function's enum-by-name marshaling accept. Read it here before writing an enum, or the write imports nothing.
//   @param enum_name - Bare enum name resolved with FindFirstObject (e.g. "ECollisionEnabled", "E_MyEnum"); an asset path is NOT accepted here
//   @returns { enum_name: string, enum_path: string, is_user_defined: bool, values: [{index, name, internal_name, display_name, value}] }

TSharedPtr<FJsonObject> HandleGetEnumInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString RequestedEnum;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("enum_name"), RequestedEnum, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    const UEnum* Found = FindFirstObject<UEnum>(*RequestedEnum);
    if (Found == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Enum not found: %s"), *RequestedEnum));
    }

    // Every entry except the trailing _MAX sentinel.
    TArray<TSharedPtr<FJsonValue>> Entries;
    const int32 LastRealEntry = Found->NumEnums() - 1;
    for (int32 Index = 0; Index < LastRealEntry; ++Index)
    {
        Entries.Add(Impl::EnumEntry(Found, Index));
    }

    TSharedPtr<FJsonObject> Definition = MakeShared<FJsonObject>();
    Definition->SetStringField(TEXT("enum_name"), RequestedEnum);
    Definition->SetStringField(TEXT("enum_path"), Found->GetPathName());
    // Blueprint enums are UUserDefinedEnum: their user-facing labels exist only as per-entry
    // display names, the internal names being the opaque "NewEnumeratorN" form.
    Definition->SetBoolField(TEXT("is_user_defined"), Found->IsA<UUserDefinedEnum>());
    Definition->SetArrayField(TEXT("values"), Entries);
    return mcp_common::MakeOkReply(Definition);
}

// MCP:get_package_info - Inspect a package that is ALREADY LOADED IN MEMORY: its long name, its is_dirty flag, and every UObject it contains with that object's class. Answers "what actually lives inside this .uasset, besides the asset named after it?" and "does this package have unsaved changes?". WARNING: it resolves with FindFirstObject over loaded UPackages — a package that is on disk but not loaded is reported as not found; use get_asset_by_name / resolve_asset_by_name for on-disk existence, and list_dirty_packages to sweep every dirty package at once.
//   @param package_path - Long package name of a LOADED package (e.g. "/Game/MyGame/Systems/Foo/BP_Foo"), matched against loaded UPackage objects
//   @returns { package_path: string, package_name: string, is_dirty: bool, objects: [{name, class}], object_count: int }

TSharedPtr<FJsonObject> HandleGetPackageInfo(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString RequestedPackage;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("package_path"), RequestedPackage, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Only packages already loaded in memory are found; nothing is loaded from disk here.
    const UPackage* Loaded = FindFirstObject<UPackage>(*RequestedPackage);
    if (Loaded == nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Package not found: %s"), *RequestedPackage));
    }

    const TArray<TSharedPtr<FJsonValue>> Members = Impl::PackageMembers(Loaded);
    TSharedPtr<FJsonObject> Contents = MakeShared<FJsonObject>();
    Contents->SetStringField(TEXT("package_path"), RequestedPackage);
    Contents->SetStringField(TEXT("package_name"), Loaded->GetName());
    Contents->SetBoolField(TEXT("is_dirty"), Loaded->IsDirty());
    Contents->SetNumberField(TEXT("object_count"), Members.Num());
    Contents->SetArrayField(TEXT("objects"), Members);
    return mcp_common::MakeOkReply(Contents);
}

// MCP:diff_objects - Diff TWO LIVE objects property by property at the same instant, using the ENGINE's own equality (FProperty::Identical + PPF_DeepComparison) rather than exported text, so FText localization identity and object-outer differences are classified as `noise` instead of being reported as real differences. Use it for "instance vs its CDO", "fork vs original", "this actor vs that one". Accepts `world`, which resolves BOTH paths in the SAME world (e.g. two live actors inside the server world); to compare the same actor ACROSS worlds — server vs client — use capture_object_snapshot + diff_object_snapshots, which is what a replication check needs. list_instance_overrides diffs a placed actor against its archetype and also tells you which package carries the override; diff_blueprints diffs the AUTHORED graphs instead of live values.
//   @renamed-from compare_objects
//   @param object_path_1 - Path or name of the first object (full object path, asset path, CDO/archetype, or dotted "Actor[.Component[.Subobject]]" by internal name; in a live world the first segment also matches the editor LABEL)
//   @param object_path_2 - Path or name of the second object, same resolution as object_path_1 and in the SAME world
//   @param equality_predicate - Optional 'identical' (DEFAULT, FProperty::Identical + PPF_DeepComparison — what the engine itself calls equal) or 'export_text' (raw ExportTextItem_Direct(Defaults=nullptr, PPF_None) string compare, kept only for callers that depend on its over-reporting behavior).
//   @param include_noise - Optional (default false). With equality_predicate='identical', also emit under `noise[]` the properties the engine considers EQUAL but whose exported text differs (FText localization identity, object outer). They are ALWAYS counted in `noise_counts` whether or not this is set.
//   @param world - Optional world selector applied to BOTH paths: absent/'editor' (default) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). It cannot straddle two worlds — one selector, both objects; an UNRECOGNIZED value is an error naming the worlds that exist, never a silent fallback.
//   @returns { object_path_1: string, object_path_2: string, same_class: bool, same_type: bool, equality_predicate: string, differences: [{property, difference, kind, value_1?: typed_value, value_2?: typed_value, export_text_1?, export_text_2?, note?}], difference_count: int, noise_counts: object, noise_count: int, noise?: [...], world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleCompareObjects(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString FirstPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path_1"), FirstPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }
    FString SecondPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path_2"), SecondPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // Equality predicate, 'identical' by DEFAULT on purpose. Comparing export text makes
    // this command contradict itself ("Values differ" with value_1 == value_2 == "Player"):
    // two FText values the engine calls EQUAL (FTextProperty::Identical compares the display
    // string in the editor) still export different localization identities. 'export_text'
    // is that predicate, byte for byte, for callers that depend on it.
    FString Predicate = TEXT("identical");
    Params->TryGetStringField(TEXT("equality_predicate"), Predicate);
    Predicate = Predicate.TrimStartAndEnd().ToLower();
    if (Predicate.IsEmpty())
    {
        Predicate = TEXT("identical");
    }
    const bool bEngineEquality = Predicate == TEXT("identical");
    if (!bEngineEquality && Predicate != TEXT("export_text"))
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Unknown equality_predicate '%s' (expected 'identical' or 'export_text')"), *Predicate));
    }

    bool bIncludeNoise = false;
    Params->TryGetBoolField(TEXT("include_noise"), bIncludeNoise);

    // ONE world for both paths: a diff whose halves come from different worlds is a replication
    // question, which capture_object_snapshot + diff_object_snapshots answers.
    Impl::FWorldChoice Where;
    TSharedPtr<FJsonObject> Refusal;
    if (!Impl::ChooseWorld(Params, TEXT("diff_objects"), Where, Refusal))
    {
        return Refusal;
    }
    const auto EditorLookup = [](const FString& Path) { return ResolveObjectByName(Path); };
    UObject* First = Impl::ResolveInChoice(Where, FirstPath, EditorLookup);
    UObject* Second = Impl::ResolveInChoice(Where, SecondPath, EditorLookup);
    if (!First)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Object 1 not found: %s (world='%s')"), *FirstPath, *Where.Label));
    }
    if (!Second)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("Object 2 not found: %s (world='%s')"), *SecondPath, *Where.Label));
    }

    UClass* ClassA = First->GetClass();
    UClass* ClassB = Second->GetClass();

    TSharedPtr<FJsonObject> Diff = MakeShared<FJsonObject>();
    Diff->SetStringField(TEXT("object_path_1"), FirstPath);
    Diff->SetStringField(TEXT("object_path_2"), SecondPath);
    Diff->SetBoolField(TEXT("same_class"), ClassA == ClassB);
    Diff->SetBoolField(TEXT("same_type"), ClassA->IsChildOf(ClassB) || ClassB->IsChildOf(ClassA));
    Diff->SetStringField(TEXT("equality_predicate"), Predicate);
    Diff->SetStringField(TEXT("world"), Where.Label);
    // The world both halves came from -- the live one, or else the FIRST object's own
    // (null for an asset/CDO; the second object may legitimately be a CDO with no world).
    mcp_common::AddWorldEvidence(Diff, Where.World ? Where.World : First->GetWorld());

    TArray<TSharedPtr<FJsonValue>> Differences;
    TArray<TSharedPtr<FJsonValue>> Noise;
    TMap<FString, int32> NoiseByKind;
    TSet<FString> NamesOnFirst;

    // Pass 1: every property of the first class, matched BY NAME on the second class.
    for (TFieldIterator<FProperty> PropIt(ClassA); PropIt; ++PropIt)
    {
        FProperty* PropA = *PropIt;
        if (PropA == nullptr)
        {
            continue;
        }
        const FString PropName = PropA->GetName();
        NamesOnFirst.Add(PropName);

        FProperty* PropB = ClassB->FindPropertyByName(*PropName);
        if (PropB == nullptr)
        {
            Differences.Add(Impl::OneSidedDifference(PropName, TEXT("Property only in Object 1")));
            continue;
        }
        if (bEngineEquality)
        {
            Impl::CompareWithEngineEquality(PropName, PropA, First, PropB, Second, bIncludeNoise, Differences, Noise, NoiseByKind);
            continue;
        }

        // LEGACY 'export_text' predicate: equality of the flat export text (FString inequality,
        // so it ignores case), documented FText false positives included.
        void* ValueA = PropA->ContainerPtrToValuePtr<void>(First);
        void* ValueB = PropB->ContainerPtrToValuePtr<void>(Second);
        if (GetPropertyValueAsString(PropA, ValueA) != GetPropertyValueAsString(PropB, ValueB))
        {
            TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
            Entry->SetStringField(TEXT("property"), PropName);
            Entry->SetStringField(TEXT("difference"), TEXT("Values differ"));
            Entry->SetStringField(TEXT("kind"), TEXT("export_text_predicate"));
            Entry->SetField(TEXT("value_1"), mcp_common::PropertyValueToJson(PropA, ValueA));
            Entry->SetField(TEXT("value_2"), mcp_common::PropertyValueToJson(PropB, ValueB));
            Differences.Add(MakeShared<FJsonValueObject>(Entry));
        }
    }

    // Pass 2: names that only the second class has.
    for (TFieldIterator<FProperty> PropIt(ClassB); PropIt; ++PropIt)
    {
        const FProperty* PropB = *PropIt;
        if (PropB != nullptr && !NamesOnFirst.Contains(PropB->GetName()))
        {
            Differences.Add(Impl::OneSidedDifference(PropB->GetName(), TEXT("Property only in Object 2")));
        }
    }

    Diff->SetArrayField(TEXT("differences"), Differences);
    Diff->SetNumberField(TEXT("difference_count"), Differences.Num());

    // Anti-lie: what was suppressed is never swallowed silently.
    Impl::WriteNoiseSummary(Diff, NoiseByKind, bIncludeNoise, Noise);
    return mcp_common::MakeOkReply(Diff);
}

// ===========================================================================
// list_instance_overrides — per-instance override audit
// ===========================================================================

// The UPackage that actually PERSISTS an object.
// With One File Per Actor (World Partition) a placed actor does NOT live in the .umap:
// it owns an external package under __ExternalActors__/<Map>/<hash-path>.uasset. That is
// the file a per-instance override physically lands in, and the reason such an override is
// invisible in a normal diff — the package name is a hash nobody can cite in a doc, which
// is exactly why this tool resolves the actor LABEL alongside it. (The hashed path can also
// change over time, so a quoted path goes stale while the label stays meaningful.)
// For a component the persisting package is its OWNER's, not its own.
static UPackage* ReflectionOfpa_ResolveOwningPackage(const UObject* Object, bool& bOutIsExternal)
{
    bOutIsExternal = false;
    if (!Object)
    {
        return nullptr;
    }

    const UObject* PackageOwner = Object;
    if (const UActorComponent* Comp = Cast<UActorComponent>(Object))
    {
        if (const AActor* Owner = Comp->GetOwner())
        {
            PackageOwner = Owner;
        }
    }

    bOutIsExternal = PackageOwner->IsPackageExternal();
    if (bOutIsExternal)
    {
        if (UPackage* ExternalPackage = PackageOwner->GetExternalPackage())
        {
            return ExternalPackage;
        }
    }
    return PackageOwner->GetPackage();
}

// JSON view of that package: long name, on-disk filename, whether the file is really
// there, and the dirty flag. `package_dirty:true` is the honest caveat of every reading in
// this tool — it means the in-memory value being diffed is NOT what is on disk yet.
static void ReflectionOfpa_DescribePackage(const UObject* Object, const TSharedPtr<FJsonObject>& Out)
{
    if (!Out.IsValid())
    {
        return;
    }

    bool bIsExternal = false;
    UPackage* Package = ReflectionOfpa_ResolveOwningPackage(Object, bIsExternal);

    Out->SetBoolField(TEXT("is_package_external"), bIsExternal);
    if (!Package)
    {
        Out->SetStringField(TEXT("package_name"), TEXT(""));
        Out->SetStringField(TEXT("package_file"), TEXT(""));
        Out->SetBoolField(TEXT("package_file_exists"), false);
        Out->SetBoolField(TEXT("package_dirty"), false);
        Out->SetStringField(TEXT("package_note"), TEXT("no owning package could be resolved"));
        return;
    }

    const FString PackageName = Package->GetName();
    Out->SetStringField(TEXT("package_name"), PackageName);
    Out->SetBoolField(TEXT("package_dirty"), Package->IsDirty());

    // A world package is .umap; an OFPA external-actor package (and every other asset) is
    // .uasset. Picking the wrong extension would report package_file_exists:false on a file
    // that is right there.
    const FString& Extension = Package->ContainsMap()
        ? FPackageName::GetMapPackageExtension()
        : FPackageName::GetAssetPackageExtension();

    FString Filename;
    if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, Filename, Extension))
    {
        Out->SetStringField(TEXT("package_file"), Filename);
        Out->SetBoolField(TEXT("package_file_exists"),
            FPlatformFileManager::Get().GetPlatformFile().FileExists(*Filename));
    }
    else
    {
        Out->SetStringField(TEXT("package_file"), TEXT(""));
        Out->SetBoolField(TEXT("package_file_exists"), false);
        Out->SetStringField(TEXT("package_note"),
            TEXT("package name does not map to a content-root filename (transient/in-memory package)"));
    }
}

// Preflight for `transact:true` on the call_* handlers. Returns TRUE when the request
// must be REFUSED, with the reason in OutError. Two cases, both of which would otherwise
// manufacture a dirty package that lies:
//   (a) a PURE/CONST function — it cannot change state, so a transaction + Modify() would
//       leave the level dirty for a read. That is precisely the false positive this
//       preflight exists to kill, so it is an error, not a silent downgrade.
//   (b) a target living in the PIE/game world — its package is transient and is never
//       saved, so a dirty flag there means nothing at all.
static bool ReflectionTransact_Refuse(const UObject* Target, const UFunction* Function, FString& OutError)
{
    OutError.Reset();
    if (!Target || !Function)
    {
        OutError = TEXT("transact=true: null target or function");
        return true;
    }

    if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure | FUNC_Const))
    {
        OutError = FString::Printf(
            TEXT("transact=true refused: '%s' is a pure/const function (FUNC_BlueprintPure|FUNC_Const) -- it cannot ")
            TEXT("change persistent state, so opening a transaction and marking the package dirty would be a false ")
            TEXT("positive. Re-issue the call without transact."),
            *Function->GetName());
        return true;
    }

    if (const UWorld* World = Target->GetWorld())
    {
        if (World->WorldType == EWorldType::PIE
            || World->WorldType == EWorldType::Game
            || World->WorldType == EWorldType::GamePreview)
        {
            OutError = FString::Printf(
                TEXT("transact=true refused: '%s' lives in the %s world, whose packages are transient and are never ")
                TEXT("saved -- a dirty flag there would be meaningless. Call the editor-world instance instead."),
                *Target->GetName(),
                World->WorldType == EWorldType::PIE ? TEXT("PIE") : TEXT("game"));
            return true;
        }
    }

    return false;
}

// Build the `transaction` sub-object of a transacted call's response, and decide the
// verification verdict from a REAL re-read of the owning package's dirty flag — not from
// the fact that ProcessEvent returned (it returns void and can no-op silently).
// What this verifies: that the effect is now PERSISTABLE (save_level/save_asset will write
// the package). It does NOT and cannot verify that the function did what the caller wanted.
static mcp_common::EMcpVerification ReflectionTransact_Report(
    UObject* Target,
    bool bDirtyBefore,
    bool bAnySavedToBuffer,
    const TArray<TSharedPtr<FJsonValue>>& Modified,
    const TSharedPtr<FJsonObject>& OutTransaction,
    FString& OutDetail)
{
    if (!OutTransaction.IsValid())
    {
        OutDetail = TEXT("internal: no transaction object to fill");
        return mcp_common::EMcpVerification::NotVerifiable;
    }

    OutTransaction->SetBoolField(TEXT("requested"), true);
    OutTransaction->SetBoolField(TEXT("package_dirty_before"), bDirtyBefore);
    OutTransaction->SetBoolField(TEXT("saved_to_transaction_buffer"), bAnySavedToBuffer);
    OutTransaction->SetArrayField(TEXT("modified"), Modified);

    // Re-reads the package AFTER the call: this sets package_dirty / package_name /
    // package_file / package_file_exists / is_package_external.
    ReflectionOfpa_DescribePackage(Target, OutTransaction);

    bool bDirtyAfter = false;
    OutTransaction->TryGetBoolField(TEXT("package_dirty"), bDirtyAfter);
    OutTransaction->SetBoolField(TEXT("package_dirty_after"), bDirtyAfter);

    FString PackageName;
    OutTransaction->TryGetStringField(TEXT("package_name"), PackageName);

    if (PackageName.IsEmpty())
    {
        OutDetail = TEXT("no owning package resolved, so nothing could be dirtied -- whatever this call changed is NOT persistable from here");
        return mcp_common::EMcpVerification::NotVerifiable;
    }
    if (!bDirtyAfter)
    {
        OutDetail = FString::Printf(
            TEXT("package '%s' is still CLEAN after Modify() -- save_level/save_asset will NOT write this change"), *PackageName);
        return mcp_common::EMcpVerification::Failed;
    }
    if (bDirtyBefore && !bAnySavedToBuffer)
    {
        OutDetail = FString::Printf(
            TEXT("package '%s' was ALREADY dirty before the call and Modify() reported no transaction snapshot -- the dirty flag ")
            TEXT("cannot be attributed to this call. It will be saved, but this is not proof that this call dirtied it."), *PackageName);
        return mcp_common::EMcpVerification::NotVerifiable;
    }

    OutDetail = FString::Printf(
        TEXT("package '%s' is dirty after Modify() (it was %s before); save_level/save_asset will now write it. ")
        TEXT("This proves persistability only, NOT that the function produced the intended value -- re-read it to confirm that."),
        *PackageName, bDirtyBefore ? TEXT("already dirty") : TEXT("clean"));
    return mcp_common::EMcpVerification::Verified;
}

// Snapshot + dirty everything a transacted call must cover.
// UObject::Modify() is what actually does the work: it saves the pre-call state into the
// transaction buffer (undo) AND calls MarkPackageDirty(). Neither ProcessEvent nor a bare
// FScopedTransaction does this — note that even the engine's own CallInEditor button
// (PropertyCustomizationHelpers.cpp) opens a transaction WITHOUT any Modify(), which is
// exactly why a CallInEditor function that fills an array on an OFPA actor leaves the package
// clean and save_level reports success while writing nothing.
// A component's authored state persists in its OWNER's package, so the owner is snapshotted
// too — and it is reported, never done behind the caller's back.
static void ReflectionTransact_ModifyTargets(UObject* Target, TArray<TSharedPtr<FJsonValue>>& OutModified, bool& bOutAnySavedToBuffer)
{
    bOutAnySavedToBuffer = false;
    if (!Target)
    {
        return;
    }

    if (Target->Modify())
    {
        bOutAnySavedToBuffer = true;
    }
    OutModified.Add(MakeShared<FJsonValueString>(Target->GetPathName()));

    if (UActorComponent* Component = Cast<UActorComponent>(Target))
    {
        if (AActor* Owner = Component->GetOwner())
        {
            if (Owner->Modify())
            {
                bOutAnySavedToBuffer = true;
            }
            OutModified.Add(MakeShared<FJsonValueString>(Owner->GetPathName()));
        }
    }
}

// EComponentCreationMethod as a wire string. Written as an explicit switch rather than
// UEnum reflection so it cannot fail at runtime, matching what Config/UEAgentConfigCommands.cpp
// already does for the same enum.
static const TCHAR* ReflectionOverrides_CreationMethodToString(EComponentCreationMethod Method)
{
    switch (Method)
    {
    case EComponentCreationMethod::Native:                  return TEXT("Native");
    case EComponentCreationMethod::SimpleConstructionScript: return TEXT("SimpleConstructionScript");
    case EComponentCreationMethod::UserConstructionScript:   return TEXT("UserConstructionScript");
    case EComponentCreationMethod::Instance:                 return TEXT("Instance");
    default:                                                 return TEXT("Unknown");
    }
}

// Diff every reflected property of `Object` against `Archetype`, using the shared
// comparator (FProperty::Identical + PPF_DeepComparison). NOT GetPropertyValueAsString:
// that one exports with Defaults=nullptr and PPF_None BY DESIGN (see its own comment), so
// it over-reports — e.g. diff_objects' 'export_text' on FText.
// `OutOverrides` gets only genuine deltas; everything the engine calls equal but that
// exports differently is COUNTED in `InOutNoiseCounts` (and listed in `OutNoise` on request),
// never silently dropped.
static void ReflectionOverrides_DiffAgainstArchetype(
    UObject* Object,
    UObject* Archetype,
    bool bRequireEdit,
    bool bIncludeTransient,
    bool bIncludeNoise,
    TArray<TSharedPtr<FJsonValue>>& OutOverrides,
    TArray<TSharedPtr<FJsonValue>>& OutNoise,
    int32& InOutExamined,
    int32& InOutSkippedTransient,
    int32& InOutSkippedNotEdit,
    int32& InOutSkippedNoBaseline,
    TMap<FString, int32>& InOutNoiseCounts)
{
    if (!Object || !Archetype)
    {
        return;
    }

    UClass* ObjectClass = Object->GetClass();
    UClass* ArchetypeClass = Archetype->GetClass();
    const bool bSameClass = (ObjectClass == ArchetypeClass);

    // Never-saved property classes: reporting them as "overrides" is pure noise, they cannot
    // be authored and cannot end up in the .uasset.
    const uint64 NeverSavedMask =
        static_cast<uint64>(CPF_Transient)
        | static_cast<uint64>(CPF_DuplicateTransient)
        | static_cast<uint64>(CPF_NonPIEDuplicateTransient)
        | static_cast<uint64>(CPF_Deprecated);

    for (TFieldIterator<FProperty> It(ObjectClass); It; ++It)
    {
        FProperty* Property = *It;
        if (!Property)
        {
            continue;
        }

        if (!bIncludeTransient && Property->HasAnyPropertyFlags(NeverSavedMask))
        {
            ++InOutSkippedTransient;
            continue;
        }
        if (bRequireEdit && !Property->HasAnyPropertyFlags(CPF_Edit))
        {
            ++InOutSkippedNotEdit;
            continue;
        }

        // The archetype is normally the SAME class (CDO / SCS template), but resolve by name
        // when it is not, instead of reading the wrong offset off the baseline container.
        FProperty* ArchetypeProperty = bSameClass ? Property : ArchetypeClass->FindPropertyByName(Property->GetFName());
        if (!ArchetypeProperty)
        {
            ++InOutSkippedNoBaseline;
            continue;
        }

        ++InOutExamined;

        // Cheap struct comparison first; the JSON form (which serializes BOTH typed values)
        // is only paid for on the properties that are not identical. On a fat actor that is
        // the difference between two JSON conversions per property and two per FINDING.
        const mcp_common::FPropertyDiffResult Comparison =
            mcp_common::ComparePropertyValues(Property, Object, ArchetypeProperty, Archetype);
        if (Comparison.Kind == mcp_common::EPropertyDiffKind::Identical)
        {
            continue;
        }

        TSharedPtr<FJsonObject> DiffObj = mcp_common::ComparePropertyValuesToJson(
            Property, Object, ArchetypeProperty, Archetype);
        if (!DiffObj.IsValid())
        {
            continue;
        }

        bool bDiffers = false;
        DiffObj->TryGetBoolField(TEXT("differs"), bDiffers);
        FString Kind;
        DiffObj->TryGetStringField(TEXT("kind"), Kind);

        DiffObj->SetStringField(TEXT("property"), Property->GetName());
        DiffObj->SetStringField(TEXT("property_type"), Property->GetCPPType());
        if (UClass* DeclaringClass = Property->GetOwnerClass())
        {
            DiffObj->SetStringField(TEXT("declaring_class"), DeclaringClass->GetName());
        }
        // value_a/value_b renamed to the vocabulary of this tool: the instance vs its baseline.
        // Copied out first: SetField can rehash Values and invalidate a Find() pointer.
        TSharedPtr<FJsonValue> InstanceValue;
        TSharedPtr<FJsonValue> BaselineValue;
        if (const TSharedPtr<FJsonValue>* FoundA = DiffObj->Values.Find(TEXT("value_a"))) { InstanceValue = *FoundA; }
        if (const TSharedPtr<FJsonValue>* FoundB = DiffObj->Values.Find(TEXT("value_b"))) { BaselineValue = *FoundB; }
        if (InstanceValue.IsValid()) { DiffObj->SetField(TEXT("value"), InstanceValue); }
        if (BaselineValue.IsValid()) { DiffObj->SetField(TEXT("archetype_value"), BaselineValue); }

        if (bDiffers)
        {
            OutOverrides.Add(MakeShared<FJsonValueObject>(DiffObj));
        }
        else if (Kind != TEXT("identical"))
        {
            InOutNoiseCounts.FindOrAdd(Kind)++;
            if (bIncludeNoise)
            {
                OutNoise.Add(MakeShared<FJsonValueObject>(DiffObj));
            }
        }
    }
}

// MCP:list_instance_overrides - List the property values a placed actor, and each of its components, holds as per-instance overrides in the level -- every property that differs from its archetype (the class defaults for the actor, the Blueprint's component template for a component) -- each with the archetype value beside it, plus the package that stores the actor (for a World Partition actor its own external .uasset) and whether that package is dirty. Use it to confirm that a write such as set_properties really landed on the instance (it catches an empty "()" stored where a gameplay tag was meant); to read named properties of any object use get_properties, and for every property of one component get_object_info. Read-only: it compares the loaded actor in the editor world, so with a dirty package the result is unsaved state (caveat says which). Properties the engine counts as equal but exports differently are counted in noise_counts and listed only with include_noise. An actor not found in the editor world, or one without an archetype, is an error.
//   @param actor - Actor to audit: internal object name, editor label, or full object path, e.g. "BP_Door_C_3" or "Door_Main". Resolved in the EDITOR world, since a per-instance override is an editor/on-disk value, not a runtime one.
//   @param include_components [bool] - Optional (default true). Also compare every component of the actor with its own archetype, the Blueprint's component template rather than the Blueprint class defaults (which hold no components).
//   @param require_edit [bool] - Optional (default true). Only consider properties flagged CPF_Edit, i.e. what a human could actually have authored in the details panel. false widens to every reflected property.
//   @param include_transient [bool] - Optional (default false). Include CPF_Transient / DuplicateTransient / NonPIEDuplicateTransient / Deprecated properties. They are never saved, so they are noise in an override audit.
//   @param include_noise [bool] - Optional (default false). Also list, under `noise[]`, the properties the engine considers EQUAL but whose exported text differs (FText localization identity, object outer). They are ALWAYS counted in `noise_counts` regardless.
//   @returns { actor_path, actor_name, actor_label, actor_class, archetype_path, archetype_class, archetype_is_cdo, is_package_external, package_name, package_file, package_file_exists, package_dirty, overrides: [{property, kind, value, archetype_value, declaring_class, property_type, note, ...}], override_count, components: [{component, component_class, creation_method, archetype_path, archetype_is_cdo, overrides, override_count, noise?}], components_examined, component_override_count, total_override_count, properties_examined, skipped: {transient, not_edit, no_baseline}, noise_counts: object, noise_count: int, noise?: [...], caveat: string }

TSharedPtr<FJsonObject> HandleListInstanceOverrides(const TSharedPtr<FJsonObject>& Params)
{
    FString ActorPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("actor"), ActorPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    bool bIncludeComponents = true;
    Params->TryGetBoolField(TEXT("include_components"), bIncludeComponents);
    bool bRequireEdit = true;
    Params->TryGetBoolField(TEXT("require_edit"), bRequireEdit);
    bool bIncludeTransient = false;
    Params->TryGetBoolField(TEXT("include_transient"), bIncludeTransient);
    bool bIncludeNoise = false;
    Params->TryGetBoolField(TEXT("include_noise"), bIncludeNoise);

    // Resolve in the EDITOR world: by internal name or editor LABEL first (the label is the
    // only human-citable identity of an OFPA actor — its package name is a rotating hash),
    // then by the broad path resolver for a fully-qualified object path.
    AActor* Actor = mcp_common::FindActorByName(mcp_common::GetEditorWorld(), ActorPath);
    if (!Actor)
    {
        Actor = Cast<AActor>(ResolveObjectByName(ActorPath));
    }
    if (!Actor)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor not found in the editor world: %s (accepts internal name, editor label, or full object path)"), *ActorPath));
    }

    UObject* Archetype = Actor->GetArchetype();
    if (!Archetype)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("Actor '%s' has no resolvable archetype -- nothing to diff against"), *Actor->GetName()));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("actor_path"), Actor->GetPathName());
    ResultObj->SetStringField(TEXT("actor_name"), Actor->GetName());
    ResultObj->SetStringField(TEXT("actor_label"), Actor->GetActorLabel());
    ResultObj->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetPathName());
    ResultObj->SetStringField(TEXT("archetype_path"), Archetype->GetPathName());
    ResultObj->SetStringField(TEXT("archetype_class"), Archetype->GetClass()->GetPathName());
    ResultObj->SetBoolField(TEXT("archetype_is_cdo"), Archetype == Archetype->GetClass()->GetDefaultObject());
    ResultObj->SetBoolField(TEXT("require_edit"), bRequireEdit);
    ResultObj->SetBoolField(TEXT("include_transient"), bIncludeTransient);

    // Where the override physically lives (the OFPA half).
    ReflectionOfpa_DescribePackage(Actor, ResultObj);

    int32 PropertiesExamined = 0;
    int32 SkippedTransient = 0;
    int32 SkippedNotEdit = 0;
    int32 SkippedNoBaseline = 0;
    TMap<FString, int32> NoiseCounts;
    TArray<TSharedPtr<FJsonValue>> NoiseArray;

    TArray<TSharedPtr<FJsonValue>> ActorOverrides;
    ReflectionOverrides_DiffAgainstArchetype(
        Actor, Archetype, bRequireEdit, bIncludeTransient, bIncludeNoise,
        ActorOverrides, NoiseArray,
        PropertiesExamined, SkippedTransient, SkippedNotEdit, SkippedNoBaseline, NoiseCounts);

    ResultObj->SetArrayField(TEXT("overrides"), ActorOverrides);
    ResultObj->SetNumberField(TEXT("override_count"), ActorOverrides.Num());

    TArray<TSharedPtr<FJsonValue>> ComponentsArray;
    int32 ComponentsExamined = 0;
    int32 ComponentOverrideCount = 0;

    if (bIncludeComponents)
    {
        TArray<UActorComponent*> Components;
        Actor->GetComponents(Components);
        for (UActorComponent* Component : Components)
        {
            if (!Component)
            {
                continue;
            }

            TSharedPtr<FJsonObject> ComponentObj = MakeShared<FJsonObject>();
            ComponentObj->SetStringField(TEXT("component"), Component->GetName());
            ComponentObj->SetStringField(TEXT("component_class"), Component->GetClass()->GetPathName());
            ComponentObj->SetStringField(TEXT("creation_method"), ReflectionOverrides_CreationMethodToString(Component->CreationMethod));

            // THE reference for an instanced component is its own archetype (the
            // <X>_GEN_VARIABLE SCS template), never the owning Blueprint's CDO.
            UObject* ComponentArchetype = Component->GetArchetype();
            if (!ComponentArchetype)
            {
                ComponentObj->SetStringField(TEXT("archetype_path"), TEXT(""));
                ComponentObj->SetArrayField(TEXT("overrides"), TArray<TSharedPtr<FJsonValue>>());
                ComponentObj->SetNumberField(TEXT("override_count"), 0);
                ComponentObj->SetStringField(TEXT("note"), TEXT("no archetype resolved -- this component was NOT compared"));
                ComponentsArray.Add(MakeShared<FJsonValueObject>(ComponentObj));
                continue;
            }

            ComponentObj->SetStringField(TEXT("archetype_path"), ComponentArchetype->GetPathName());
            ComponentObj->SetBoolField(TEXT("archetype_is_cdo"),
                ComponentArchetype == ComponentArchetype->GetClass()->GetDefaultObject());

            TArray<TSharedPtr<FJsonValue>> ComponentOverrides;
            TArray<TSharedPtr<FJsonValue>> ComponentNoise;
            ReflectionOverrides_DiffAgainstArchetype(
                Component, ComponentArchetype, bRequireEdit, bIncludeTransient, bIncludeNoise,
                ComponentOverrides, ComponentNoise,
                PropertiesExamined, SkippedTransient, SkippedNotEdit, SkippedNoBaseline, NoiseCounts);

            ComponentObj->SetArrayField(TEXT("overrides"), ComponentOverrides);
            ComponentObj->SetNumberField(TEXT("override_count"), ComponentOverrides.Num());
            if (bIncludeNoise)
            {
                ComponentObj->SetArrayField(TEXT("noise"), ComponentNoise);
            }

            ++ComponentsExamined;
            ComponentOverrideCount += ComponentOverrides.Num();
            ComponentsArray.Add(MakeShared<FJsonValueObject>(ComponentObj));
        }
    }

    ResultObj->SetArrayField(TEXT("components"), ComponentsArray);
    ResultObj->SetNumberField(TEXT("components_examined"), ComponentsExamined);
    ResultObj->SetNumberField(TEXT("component_override_count"), ComponentOverrideCount);
    ResultObj->SetNumberField(TEXT("total_override_count"), ActorOverrides.Num() + ComponentOverrideCount);

    ResultObj->SetNumberField(TEXT("properties_examined"), PropertiesExamined);
    TSharedPtr<FJsonObject> SkippedObj = MakeShared<FJsonObject>();
    SkippedObj->SetNumberField(TEXT("transient"), SkippedTransient);
    SkippedObj->SetNumberField(TEXT("not_edit"), SkippedNotEdit);
    SkippedObj->SetNumberField(TEXT("no_baseline"), SkippedNoBaseline);
    ResultObj->SetObjectField(TEXT("skipped"), SkippedObj);
    UEAgentReflectionCommandsImpl::WriteNoiseSummary(ResultObj, NoiseCounts, bIncludeNoise, NoiseArray);

    // Say what this reading is and is not. It is the IN-MEMORY state of the loaded actor;
    // if its package is dirty, disk still holds something else, and a World Partition cell
    // that is not loaded contributes no actor at all to this answer.
    bool bPackageDirty = false;
    ResultObj->TryGetBoolField(TEXT("package_dirty"), bPackageDirty);
    ResultObj->SetStringField(TEXT("caveat"), bPackageDirty
        ? TEXT("package is DIRTY: this diff reflects unsaved in-memory state, not what is on disk. Save, or reload the package, before quoting it.")
        : TEXT("diff of the loaded in-memory actor against its archetype; the package is clean, so it matches disk."));

    return mcp_common::MakeOutcomeReply(ResultObj, /*bSuccess=*/true);
}

// Batch property reads and writes on one object.

// MCP:get_properties - Read one or several named properties off ONE object you can already name -- an asset path, a live object's full path, a CDO/archetype, or a dotted "Actor[.Component[.Subobject]]" whose first segment is the actor's internal name -- typed rather than stringified, as { name: typed_value } plus each property's FProperty class and not_found[] for names the class does not declare. Pick it when you have the object and the names: get_object_info when you do not know the names yet, get_actor_properties_by_class when you have no path and want instances by class or label (it drops misspelled names; this reports them). property_name reads one; property_names batches several. Accepts `world` (editor, server, client, pie:N), so the same read on server and client is the cheapest proof that a value replicated; the reply stamps which world answered. Read-only.
//   @param object_path - Path or name of the object instance. In the editor world: full object path, asset path, CDO/archetype, an asset's sub-object as the readers give its object_path -- a graph node from get_graph_nodes, a sound cue node, a Niagara renderer or simulation stage, a PCG node's settings_path, a behavior tree node, a camera rig node, a Mass trait, a physics body -- also written "/Game/Asset:SubObject", or a dotted "Actor[.Component[.Subobject]]" whose first segment is the actor's INTERNAL name (editor labels are not matched here). In a live world: a fully-qualified PIE path or the same dotted form (first segment matches internal name OR editor label).
//   @param property_names [array] - Optional names of the properties to read; every name that the class does not declare comes back in not_found[]. Required unless property_name is given.
//   @param property_name - Optional single property to read, added to property_names.
//   @param world - Optional world selector: absent/'editor' (default: the editor/asset instance) | 'pie' (ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'game' (alias of 'pie'). Anything other than absent/'editor' reads the LIVE instance in that world; an UNRECOGNIZED value is an error naming the worlds that exist, never a silent fallback to another world.
//   @returns { object_path: string, class_name: string, values: object (typed: bool/number/string natively, struct/map as object, array/set as array, object-ref as path string), property_types: object ({name: FProperty class name}), not_found: [string], world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetProperties(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString ObjectPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // property_names and the single property_name are both optional; together they are the names read.
    TArray<FString> Names;
    const TArray<TSharedPtr<FJsonValue>>* NamesArray = nullptr;
    const bool bHasNames = Params->TryGetArrayField(TEXT("property_names"), NamesArray) && NamesArray;
    if (bHasNames)
    {
        for (const TSharedPtr<FJsonValue>& NameVal : *NamesArray)
        {
            if (NameVal.IsValid())
            {
                Names.Add(NameVal->AsString());
            }
        }
    }
    FString SingleName;
    const bool bHasSingle = Params->TryGetStringField(TEXT("property_name"), SingleName) && !SingleName.IsEmpty();
    if (bHasSingle)
    {
        Names.AddUnique(SingleName);
    }
    if (!bHasNames && !bHasSingle)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: property_names (array) or property_name"));
    }

    // Optional world selector (absent/'editor' = the editor resolution). Anything
    // else reads the LIVE instance in that world -- this is what turns a batch read into a
    // co-op measurement instead of an editor-side guess. An unknown selector is REFUSED.
    Impl::FWorldScopedObject Target;
    TSharedPtr<FJsonObject> Refusal;
    if (!Impl::ResolveWorldScopedObject(Params, ObjectPath, TEXT("get_properties"),
            [](const FString& Path) { return ResolveObjectByName(Path); }, Target, Refusal))
    {
        return Refusal;
    }
    UObject* Object = Target.Object;

    UClass* Class = Object->GetClass();
    TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
    TSharedPtr<FJsonObject> TypesObj = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> NotFound;

    for (const FString& PropName : Names)
    {
        FProperty* Property = Class->FindPropertyByName(*PropName);
        if (!Property)
        {
            NotFound.Add(MakeShared<FJsonValueString>(PropName));
            continue;
        }
        void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
        ValuesObj->SetField(PropName, mcp_common::PropertyValueToJson(Property, ValuePtr));
        TypesObj->SetStringField(PropName, Property->GetClass()->GetName());
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("object_path"), ObjectPath);
    ResultObj->SetStringField(TEXT("class_name"), Class->GetName());
    ResultObj->SetObjectField(TEXT("values"), ValuesObj);
    ResultObj->SetObjectField(TEXT("property_types"), TypesObj);
    ResultObj->SetArrayField(TEXT("not_found"), NotFound);
    ResultObj->SetStringField(TEXT("world"), Target.WorldLabel);
    // WHICH world produced these values. Server and client can legitimately differ;
    // without this stamp the difference is indistinguishable from a game bug.
    mcp_common::AddWorldEvidence(ResultObj, Target.World);
    return mcp_common::MakeOkReply(ResultObj);
}

// MCP:set_properties - Write one or several properties on ONE object -- an asset, a CDO/archetype, a placed actor, or one of its components as "Actor.Component" -- in the EDITOR world, as an edit in the Details panel: each write runs Modify/PreEditChange/PostEditChange (undoable, package dirtied, the component re-registered), and the reply lists set[] and failed[{property,error}] per name.
//   @para This is the tool for any editable setting of a light, fog, sky, cloud, decal, camera, post-process volume, capture, replication flag, sound, Niagara system, physical material, montage, foliage type or other asset. Pass the engine's property names (get_object_info lists them), a struct as a JSON object of only the fields to change, a color as {"R","G","B","A"}. The actor is found by its internal name or by a label only it has; the component by its name or by a class only one of its components has ("Sun.DirectionalLightComponent", "Fog.ExponentialHeightFogComponent").
//   @para Example: object_path "Sun.DirectionalLightComponent" with properties {"Intensity": 10, "LightColor": {"R": 255, "G": 200, "B": 150}} writes two properties in one call; property_name "Intensity" with value 10 writes one. Both forms together write properties first. A struct takes only the fields to change, as {"RelativeLocation": {"Z": 200}} moves a component up and keeps its X and Y.
//   @para An asset's sub-object -- a graph node, a sound cue node, a Niagara renderer or stage, a PCG node's settings, a material expression, a behavior tree node, a camera rig node, a Mass trait, a physics body, by the object_path its reader gives -- is written the same way, and its asset is finished as its editor does it: a graph node is reconstructed and its Blueprint marked modified (compile it), a sound cue recaches its values, a Niagara system recompiles. A blend space is resampled after the write. No Blueprint is compiled.
//   @para Refused: a key inside a struct that names no field (nothing is written to that property), an unregistered gameplay tag, and any call while Play-in-Editor runs (see force). clamp=true keeps numbers inside ClampMin/ClampMax as the Details panel does, and lists what it clamped; save=true saves the asset's package (a level object is saved with save_level). Not for: a component in a live PIE world (set_component_property), a Blueprint's component template (set_blueprint_component_properties), a widget inside a Widget Blueprint (set_widget_property), or the object's own setter (call_function).
//   @core
//   @param object_path - The object, resolved in the EDITOR world: an asset path, a full object path, a CDO/archetype, an asset's sub-object as the readers give its object_path -- a graph node from get_graph_nodes, a sound cue node, a Niagara renderer or simulation stage, a PCG node's settings_path, a behavior tree node, a camera rig node, a Mass trait, a physics body -- also written "/Game/Asset:SubObject", or "Actor[.Component[.Subobject]]" where Actor is an internal name or a unique label and Component a component name or a class only one component has.
//   @param properties [object] - Optional dict of {property_name: value}; values can be scalar, nested object, or array (UE-format text like "(X=1,Y=2,Z=3)" is also accepted for structs). Required unless property_name + value is given; both together write properties{} first.
//   @param property_name - Optional single property to write (must be CPF_Edit and not EditConst, or it lands in failed[] with the reason). Needs value.
//   @param value - Optional new value for property_name: string, number, boolean, JSON object (struct), JSON array (array/set/tag container), or a UE-format text string like "(X=1,Y=2,Z=3)"
//   @param save [bool] - Save the asset's package after the writes (default false); refused, before anything is written, for an object in a level
//   @param clamp [bool] - Clamp each number written to the ClampMin/ClampMax of its property, struct fields and array elements included (default false)
//   @returns { object_path: string, resolved_path: string, set: [string], failed: [{property, error}], owning_asset?: string (a sub-object's asset), finished?: [string] (what ran on that asset), clamped?: [string] ("name=value", with clamp), saved?: bool, save_error?: string (with save), success: bool }

TSharedPtr<FJsonObject> HandleSetProperties(const TSharedPtr<FJsonObject>& Params)
{
    namespace Impl = UEAgentReflectionCommandsImpl;

    FString ObjectPath;
    if (TSharedPtr<FJsonObject> Missing = mcp_common::ReadRequiredString(Params, TEXT("object_path"), ObjectPath, mcp_common::EAllowEmpty::Yes)) { return Missing; }

    // properties{} and the single property_name + value are both optional; at least one has to be there.
    const TSharedPtr<FJsonObject>* PropsObj = nullptr;
    const bool bHasProps = Params->TryGetObjectField(FStringView(TEXT("properties")), PropsObj) && PropsObj && (*PropsObj);
    FString SingleName;
    const bool bHasSingleName = Params->TryGetStringField(TEXT("property_name"), SingleName) && !SingleName.IsEmpty();
    // `value` may be any JSON type (struct-arrays and tag containers arrive as arrays, structs as objects);
    // ReadAnyTypedValue keeps it typed for the importer.
    const TSharedPtr<FJsonValue> SingleValue = Params->HasField(TEXT("value")) ? Impl::ReadAnyTypedValue(Params) : nullptr;
    if (bHasSingleName && !SingleValue.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("property_name needs a value (array, object, string, number, or bool)"));
    }
    if (!bHasSingleName && SingleValue.IsValid())
    {
        return mcp_common::MakeErrorReply(TEXT("value needs a property_name to write it to"));
    }
    if (!bHasProps && !bHasSingleName)
    {
        return mcp_common::MakeErrorReply(TEXT("Missing required parameter: properties (object), or property_name + value"));
    }

    bool bSave = false;
    Params->TryGetBoolField(TEXT("save"), bSave);
    bool bClamp = false;
    Params->TryGetBoolField(TEXT("clamp"), bClamp);

    UObject* Object = ResolveObjectByName(ObjectPath);
    if (!Object)
    {
        return mcp_common::MakeErrorReply(DescribeUnresolvedPath(ObjectPath));
    }
    // save=true writes an asset's package; what lives in a level is saved with it.
    if (bSave && Object->GetTypedOuter<UWorld>() != nullptr)
    {
        return mcp_common::MakeErrorReply(FString::Printf(
            TEXT("save=true saves an asset, and %s lives in a level: nothing was written. Write without save, then save_level."), *ObjectPath));
    }

    TArray<TSharedPtr<FJsonValue>> SetOk;
    TArray<TSharedPtr<FJsonValue>> Failed;
    TArray<FString> Clamped;

    // A sub-object of an asset (a graph node, a sound cue node, a Niagara renderer, a PCG node's settings...): its asset
    // is recorded too, so one undo restores both, and is finished after the writes (below).
    UObject* OwningAsset = nullptr;
    if (Object->GetTypedOuter<UWorld>() == nullptr)
    {
        UPackage* OwnerPackage = Object->GetPackage();
        UObject* Asset = OwnerPackage ? OwnerPackage->FindAssetInPackage() : nullptr;
        if (Asset && Asset != Object && Object->IsIn(Asset))
        {
            OwningAsset = Asset;
            OwningAsset->Modify();
        }
    }

    // The writes, in order: properties{} first, then the single one.
    TArray<TPair<FString, TSharedPtr<FJsonValue>>> Writes;
    if (bHasProps)
    {
        for (const auto& Pair : (*PropsObj)->Values)
        {
            Writes.Emplace(FString(*Pair.Key), Pair.Value);
        }
    }
    if (bHasSingleName)
    {
        Writes.Emplace(SingleName, SingleValue);
    }

    for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Writes)
    {
        FString Error;
        if (ApplyCheckedPropertySet(Object, Pair.Key, Pair.Value, Error, bClamp ? &Clamped : nullptr))
        {
            SetOk.Add(MakeShared<FJsonValueString>(Pair.Key));
        }
        else
        {
            TSharedPtr<FJsonObject> FailObj = MakeShared<FJsonObject>();
            FailObj->SetStringField(TEXT("property"), Pair.Key);
            FailObj->SetStringField(TEXT("error"), Error);
            Failed.Add(MakeShared<FJsonValueObject>(FailObj));
        }
    }

    // What the Details panel runs after an edit and PostEditChange does not: a blend space rebuilds its runtime
    // samples only in its editor (SAnimationBlendSpace), so a changed axis would leave the old grid in use.
    if (SetOk.Num() > 0)
    {
        if (UBlendSpace* BlendSpace = Cast<UBlendSpace>(Object))
        {
            BlendSpace->ValidateSampleData();
            BlendSpace->ResampleData();
        }
    }

    // What the asset's own editor runs after a sub-object changed: a graph node rebuilds its pins and its Blueprint
    // learns of it (an AnimGraph node's Node struct, a property access node), a sound cue recaches its aggregate values,
    // a Niagara system recompiles. A Blueprint's CDO or component template is left alone: marking the Blueprint
    // modified would make its next compile regenerate them.
    TArray<TSharedPtr<FJsonValue>> Finished;
    if (SetOk.Num() > 0)
    {
        if (UEdGraphNode* GraphNode = Cast<UEdGraphNode>(Object))
        {
            GraphNode->ReconstructNode();
            if (UBlueprint* NodeBlueprint = FBlueprintEditorUtils::FindBlueprintForNode(GraphNode))
            {
                FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(NodeBlueprint);
            }
            if (UEdGraph* NodeGraph = GraphNode->GetGraph())
            {
                NodeGraph->NotifyGraphChanged();
            }
            Finished.Add(MakeShared<FJsonValueString>(TEXT("graph node reconstructed, its Blueprint marked modified (compile it)")));
        }
        else if (USoundCue* Cue = Cast<USoundCue>(OwningAsset))
        {
            Cue->CacheAggregateValues();
            Finished.Add(MakeShared<FJsonValueString>(TEXT("sound cue aggregate values recached")));
        }
        else if (UNiagaraSystem* System = Cast<UNiagaraSystem>(OwningAsset))
        {
            System->RequestCompile(false);
            Finished.Add(MakeShared<FJsonValueString>(TEXT("Niagara system compile requested")));
        }
    }

    bool bSaved = false;
    FString SaveError;
    if (bSave && SetOk.Num() > 0)
    {
        UPackage* Package = Object->GetPackage();
        UObject* Asset = Package ? Package->FindAssetInPackage() : nullptr;
        bSaved = mcp_common::SaveAssetPackage(Package, Asset ? Asset : Object, SaveError);
    }
    else if (bSave)
    {
        SaveError = TEXT("nothing was written, so nothing was saved");
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("object_path"), ObjectPath);
    ResultObj->SetStringField(TEXT("resolved_path"), Object->GetPathName());
    ResultObj->SetArrayField(TEXT("set"), SetOk);
    ResultObj->SetArrayField(TEXT("failed"), Failed);
    if (OwningAsset)
    {
        ResultObj->SetStringField(TEXT("owning_asset"), OwningAsset->GetPathName());
    }
    if (Finished.Num() > 0)
    {
        ResultObj->SetArrayField(TEXT("finished"), Finished);
    }
    if (bClamp)
    {
        TArray<TSharedPtr<FJsonValue>> ClampedJson;
        for (const FString& Entry : Clamped)
        {
            ClampedJson.Add(MakeShared<FJsonValueString>(Entry));
        }
        ResultObj->SetArrayField(TEXT("clamped"), ClampedJson);
    }
    if (bSave)
    {
        ResultObj->SetBoolField(TEXT("saved"), bSaved);
        if (!bSaved)
        {
            ResultObj->SetStringField(TEXT("save_error"), SaveError);
        }
    }
    const bool bSucceeded = Failed.Num() == 0 && (!bSave || bSaved);
    ResultObj->SetBoolField(TEXT("success"), bSucceeded);
    // The verdict measured above is the verdict EMITTED. A hardcoded success:true
    // from MakeOkReply would hide it, and callers only check that flag.
    return mcp_common::MakeOutcomeReply(ResultObj, bSucceeded,
        bSucceeded ? FString() : mcp_common::MeasuredOutcomeError(TEXT("set_properties"), TEXT("Failed.Num() == 0 && (!bSave || bSaved)")));
}

// ===========================================================================
// PIE-aware helpers (definitions of the forward decls near the top of the file)
// ===========================================================================

// Resolve a path/name/label to a live object within World. First tries the broad,
// side-effect-free path resolver (full object path / asset / CDO / archetype /
// already-loaded object — including a fully-qualified PIE path). Falls back to a
// dotted "Actor[.Component[.Subobject]]" lookup scoped to World: the first segment
// matches a live actor by object name or (editor) label; each further segment walks
// into a named component or subobject.
static UObject* ReflectionPie_ResolveObjectInWorld(const FString& Path, UWorld* World)
{
    // The path shortcut is WORLD-BLIND: for a dotted form ("Actor.Component")
    // ResolveObjectByPath finds the EDITOR instance even when the caller asked for
    // world='server' -- and since AddWorldEvidence stamps the RESOLVED world, the
    // response would certify UEDPIE_0 with a value read from the editor (same evidence,
    // different value, which makes a working write look broken).
    //
    // So the shortcut only counts if it does NOT contradict the requested world. An
    // object with no world (asset, CDO, archetype) contradicts nothing and is accepted,
    // so the asset path is untouched.
    if (UObject* Direct = mcp_common::ResolveObjectByPath(Path))
    {
        const UWorld* DirectWorld = Direct->GetWorld();
        if (!World || !DirectWorld || DirectWorld == World)
        {
            return Direct;
        }
        // Contradicts: fall through to the sweep scoped to the requested world, below.
    }
    return AssetCommandHelpers::FindDottedPathInWorld(Path, World);
}

// MCP:get_actor_properties_by_class - Find placed actors by CLASS (IsA) and/or editor label and read chosen properties of each in one call, as { path, props } per instance, in a live PIE world by default or in the editor world with world='editor' (no PIE needed). Pick it when you have no object path, or want the same properties across many actors; for one object you can name, get_properties reads it typed and reports a misspelled name in not_found[], where this tool silently drops it. Filter with class_name and/or actor_label, drill into one `component`, list the `properties` to read, and compare world='server' with 'client' to separate the authoritative value from the replicated one; the reply stamps the world that answered. It replaces an execute_python loop over GetAllActorsOfClass + get_editor_property. Read-only.
//   @param class_name - Class filter: bare UClass name resolved with FindFirstObject (e.g. "BP_MyActor_C", "StaticMeshActor"), NOT an asset path. Matches every actor that IsA(class_name). REQUIRED unless actor_label is given.
//   @param actor_label - Editor-label filter; matches actors whose GetActorLabel() equals this EXACTLY (no substring, no case-folding). REQUIRED unless class_name is given — the call errors when both are missing.
//   @param component - Optional component name to drill into on each matched actor (reads that component's props instead of the actor's). WARNING: actors that have no component with this name are DROPPED from `instances` entirely, so a short list can mean "wrong component name", not "no such actor".
//   @param properties [array] - Optional property names to read. Names the target does not have are skipped SILENTLY (no not_found list — cross-check the spelling with get_object_info or get_class_properties). Omit it to read every BlueprintVisible property, which on a Character is a very large payload.
//   @param world - Optional world selector: absent/'pie' (default, ambiguous: PlayWorld, else the first PIE context) | 'server' (the PIE world with authority) | 'client' | 'pie:N' | 'editor' | 'game' (alias of 'pie'). THIS is the parameter that separates a server reading from a client one.
//   @returns { instances: [{ path: string, props: object (typed values) }], instance_count: int, world: string, world_name: string|null, world_package: string|null, world_type: string|null, net_mode: string|null, pie_instance: number }

TSharedPtr<FJsonObject> HandleGetActorPropertiesByClass(const TSharedPtr<FJsonObject>& Params)
{
    // Default is 'pie' (this command means "the live session"), but the world is
    // addressable AND stamped -- this tool reading UEDPIE_1 while raycast_scene reads
    // UEDPIE_0 is an easy trap to fall into.
    FString WorldLabel, WorldError;
    UWorld* PieWorld = mcp_common::ResolveWorldSelector(Params, TEXT("pie"), WorldLabel, WorldError);
    if (!PieWorld)
    {
        return mcp_common::MakeErrorReply(FString::Printf(TEXT("get_actor_properties_by_class: %s"), *WorldError));
    }

    FString ClassName;
    const bool bHasClass = Params->TryGetStringField(TEXT("class_name"), ClassName) && !ClassName.IsEmpty();
    FString ActorLabel;
    const bool bHasLabel = Params->TryGetStringField(TEXT("actor_label"), ActorLabel) && !ActorLabel.IsEmpty();
    FString ComponentName;
    const bool bHasComponent = Params->TryGetStringField(TEXT("component"), ComponentName) && !ComponentName.IsEmpty();

    if (!bHasClass && !bHasLabel)
    {
        return mcp_common::MakeErrorReply(TEXT("get_actor_properties_by_class: provide at least one of class_name or actor_label"));
    }

    UClass* FilterClass = nullptr;
    if (bHasClass)
    {
        FilterClass = FUEAgentGraphUtils::ResolveClass(ClassName);
        if (!FilterClass)
        {
            return mcp_common::MakeErrorReply(FUEAgentGraphUtils::DescribeClassNotFound(ClassName));
        }
    }

    // Requested property names (optional).
    TArray<FString> RequestedProps;
    const TArray<TSharedPtr<FJsonValue>>* PropNamesArray = nullptr;
    if (Params->TryGetArrayField(TEXT("properties"), PropNamesArray) && PropNamesArray)
    {
        for (const TSharedPtr<FJsonValue>& NameVal : *PropNamesArray)
        {
            if (NameVal.IsValid())
            {
                RequestedProps.Add(NameVal->AsString());
            }
        }
    }

    TArray<TSharedPtr<FJsonValue>> Instances;
    for (TActorIterator<AActor> It(PieWorld); It; ++It)
    {
        AActor* Actor = *It;
        if (!Actor)
        {
            continue;
        }
        if (FilterClass && !Actor->IsA(FilterClass))
        {
            continue;
        }
        if (bHasLabel && Actor->GetActorLabel() != ActorLabel)
        {
            continue;
        }

        // Target object: the actor, or a named component drilled into it.
        UObject* Target = Actor;
        if (bHasComponent)
        {
            UActorComponent* FoundComp = UEAgentReflectionCommandsImpl::FindComponentNamed(Actor, ComponentName);
            if (!FoundComp)
            {
                continue; // this actor lacks the requested component
            }
            Target = FoundComp;
        }

        UClass* TargetClass = Target->GetClass();
        TSharedPtr<FJsonObject> PropsObj = MakeShared<FJsonObject>();

        if (RequestedProps.Num() > 0)
        {
            for (const FString& PropName : RequestedProps)
            {
                FProperty* Property = TargetClass->FindPropertyByName(*PropName);
                if (!Property)
                {
                    continue; // silently skip properties this target doesn't have
                }
                void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
                PropsObj->SetField(PropName, mcp_common::PropertyValueToJson(Property, ValuePtr));
            }
        }
        else
        {
            // Sensible default: every BlueprintVisible property of the target.
            for (TFieldIterator<FProperty> PropIt(TargetClass); PropIt; ++PropIt)
            {
                FProperty* Property = *PropIt;
                if (!Property || !Property->HasAnyPropertyFlags(CPF_BlueprintVisible))
                {
                    continue;
                }
                void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Target);
                PropsObj->SetField(Property->GetName(), mcp_common::PropertyValueToJson(Property, ValuePtr));
            }
        }

        TSharedPtr<FJsonObject> InstanceObj = MakeShared<FJsonObject>();
        InstanceObj->SetStringField(TEXT("path"), Target->GetPathName());
        InstanceObj->SetObjectField(TEXT("props"), PropsObj);
        Instances.Add(MakeShared<FJsonValueObject>(InstanceObj));
    }

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetArrayField(TEXT("instances"), Instances);
    ResultObj->SetNumberField(TEXT("instance_count"), Instances.Num());
    ResultObj->SetStringField(TEXT("world"), WorldLabel);
    // An instance_count of 0 means "not in THIS world" -- which world, is right here.
    mcp_common::AddWorldEvidence(ResultObj, PieWorld);
    return mcp_common::MakeOkReply(ResultObj);
}

const McpCommandFamilies::FRegistrar Registration =
{
    { TEXT("call_function"), &HandleCallFunction },
    { TEXT("diff_objects"), &HandleCompareObjects },
    { TEXT("get_actor_properties_by_class"), &HandleGetActorPropertiesByClass },
    { TEXT("get_class_functions"), &HandleGetClassFunctions },
    { TEXT("get_class_hierarchy"), &HandleGetClassHierarchy },
    { TEXT("get_class_properties"), &HandleGetClassProperties },
    { TEXT("get_enum_info"), &HandleGetEnumInfo },
    { TEXT("get_function_info"), &HandleGetFunctionInfo },
    { TEXT("get_package_info"), &HandleGetPackageInfo },
    { TEXT("get_properties"), &HandleGetProperties },
    { TEXT("get_struct_info"), &HandleGetStructInfo },
    { TEXT("get_object_info"), &HandleInspectObject },
    { TEXT("list_instance_overrides"), &HandleListInstanceOverrides },
    { TEXT("set_properties"), &HandleSetProperties },
};
} // namespace UEAgentReflectionCommands
