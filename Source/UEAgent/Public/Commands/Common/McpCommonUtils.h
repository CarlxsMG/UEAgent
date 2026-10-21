// Copyright (c) 2026 CarlxsMG.

#pragma once

#include "CoreMinimal.h"
// PPF_DeepComparison is the default argument of the property comparator
// below, so the flag enum must be visible from the header (it lives in Core,
// NOT in CoreUObject: Core/Public/UObject/PropertyPortFlags.h).
#include "UObject/PropertyPortFlags.h"
#include "UObject/ObjectMacros.h"   // ERenameFlags / REN_* (RenameToFreeName's default argument)

// Types the declarations below only name through pointers and references.
class AActor; class FJsonObject; class FProperty; class UBlueprint; class UClass; class UEdGraph;
class UEdGraphNode; class UEdGraphPin; class UFactory; class UFunction; class UObject; class UPackage;
class UStruct; class UWorld; struct FAssetData; struct FGameplayTagContainer; class IAssetRegistry;
class UK2Node_CallFunction; class UK2Node_Event; class UK2Node_Self;
class UK2Node_VariableGet; class UK2Node_VariableSet; class FTransaction; class UActorComponent;

// The Json module's headers one by one: the monolithic Json.h warns in every file that includes it,
// and Fab requires a plugin to build without it (IWYU).
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * Shared free-function helpers for the MCP command handlers.
 */
namespace mcp_common
{
    /**
     * JSON param extraction helper. Strict-equivalent to:
     *     Params->TryGetStringField(FieldName, OutPath) && !OutPath.IsEmpty()
     */
    UEAGENT_API bool ExtractStringPath(
        const TSharedPtr<FJsonObject>& Params,
        const FString& FieldName,
        FString& OutPath);

    /**
     * True if a Play-in-Editor session is running. SAME detector that
     * `is_play_in_editor_active` reports (ULevelEditorSubsystem::IsInPlayInEditor, with
     * GEditor->PlayWorld as a fallback) -- on purpose: if the guard and the tool callers
     * are told to check could disagree, no refusal would be credible.
     *
     * When an asset save meets a running PIE session, the plugin ANSWERS that the save
     * cannot happen and leaves the PIE session alone -- the same policy as create_blueprint,
     * which refuses and reports it. The real refusal happens in the dispatcher (CheckPIESafetyGuard);
     * this is defense in depth for internal paths that do not go through it.
     */
    UEAGENT_API bool IsPieActive();

    /**
     * Tells the dispatcher that this call changed nothing although the engine recorded a change in the undo
     * buffer (an animation data controller bracket records one whatever it writes), so the call's undo step
     * would undo nothing. The dispatcher then drops that step, which also keeps the redo history, but only when
     * it is the outermost transaction: inside a batch or a begin_transaction the step belongs to the caller.
     * Call it only once the call's own probe has proved that no stored data changed. Game thread.
     */
    UEAGENT_API void DiscardUndoStepOfThisCall();

    /** Whether DiscardUndoStepOfThisCall ran since the last call of this, clearing the request (the dispatcher's). */
    UEAGENT_API bool ConsumeDiscardUndoStepRequest();

    /**
     * Marks the package dirty and saves it to disk (.uasset) with
     * the canonical authoring flags (RF_Public|RF_Standalone,
     * SAVE_NoError, Error=GError). Returns false + fills OutError on
     * failure. A package that contains a map is saved as a .umap through
     * the editor's own map save (UEditorLoadingAndSavingUtils::SavePackages,
     * no dialog), which runs the world's pre-save and also writes that
     * world's dirty external packages and its _BuiltData package.
     *
     * PIE: if a session is running this REFUSES and returns false with OutError
     * explaining why, instead of saving. Not caution: the engine guard
     * (Editor.AllowSavingAssetsDuringPIE=0) rejects the write BEFORE attempting anything,
     * so the alternative is not "a worse save" but reporting success without writing a
     * single byte. The PIE session is left alone. Pass OutPieWasActive (optional) to
     * observe it.
     *
     * READ-ONLY ROOT GUARD (enforcing by default). Before dirtying the package this
     * consults IsUnderReadOnlyRoot() against GetDefaultReadonlyRoots() (the project's
     * Read Only Content Roots) and REFUSES the save (returns false, OutError explains how
     * to opt out). `mcp.ReadOnlyRootEnforcement 0` on the console switches to report-only
     * mode, where a Warning is logged and the save proceeds. Pass OutWasUnderReadOnlyRoot
     * to observe the classification regardless of mode.
     */
    UEAGENT_API bool SaveAssetPackage(
        UPackage* Package,
        UObject* Asset,
        FString& OutError,
        bool* OutPieWasActive = nullptr,
        bool* OutWasUnderReadOnlyRoot = nullptr);
    /**
     * Convenience overload for callers that don't surface the
     * error string.
     */
    UEAGENT_API bool SaveAssetPackage(UPackage* Package, UObject* Asset);

    /**
     * Why an object of ObjectClass cannot be created as ObjectName in the package PackageName, or empty
     * when it can. The engine's own test -- an existing object whose class is not ObjectClass or a child
     * of it -- applied in two places:
     *  - in memory, where NewObject would abort the editor ("Cannot replace existing object of a
     *    different class");
     *  - on disk only, where saving the new object would overwrite the existing asset.
     * Call it BEFORE CreatePackage: creating the package first would shadow an on-disk package with an
     * empty one. PackageName may also be an object path ("/Game/A/B.B").
     */
    UEAGENT_API FString AssetNameConflict(const FString& PackageName, const FString& ObjectName, const UClass* ObjectClass);

    inline FString AssetNameConflict(const FString& PackageName, FName ObjectName, const UClass* ObjectClass)
    {
        return AssetNameConflict(PackageName, ObjectName.ToString(), ObjectClass);
    }
    inline FString AssetNameConflict(const FString& PackageName, const TCHAR* ObjectName, const UClass* ObjectClass)
    {
        return AssetNameConflict(PackageName, FString(ObjectName), ObjectClass);
    }

    /**
     * How many characters past an asset's object path ("<package>.<asset>") the paths of the objects the engine
     * creates inside it reach. The transaction buffer makes an FName of the full path of every transactional object
     * it records (FTransactionObjectId::SetObject), and every call runs inside a transaction. The longest such
     * paths measured in the engine source, with the tools' default names:
     * - an anim blueprint scaffolded with a state machine, 136:
     *   ":AnimGraph.AnimGraphNode_StateMachine_0.Locomotion.AnimStateTransitionNode_0.AnimationTransitionGraph_0.AnimGraphNode_TransitionResult_0";
     * - an actor placed in a World Partition map, 92:
     *   ":PersistentLevel.SkeletalMeshActor_UAID_<18 hex>_<10 digits>.SkeletalMeshComponent0";
     * - a level sequence's track and section, 85: ":MovieScene_0.MovieSceneSkeletalAnimationTrack_0.MovieSceneSkeletalAnimationSection_0";
     * - a new World Partition map's minimap actor, 80: ":PersistentLevel.WorldPartitionMiniMap_UAID_<18 hex>_<10 digits>.Sprite",
     *   and its runtime hash, 78: ":PersistentLevel.WorldSettings.WorldPartition_0.WorldPartitionRuntimeHashSet_0";
     * - a Blueprint's SCS node, 40 ("_C:SimpleConstructionScript_0.SCS_Node_0"), and its CDO, 16 ("Default__" and "_C").
     * The margin keeps 24 characters past the longest for names longer than the defaults.
     */
    constexpr int32 ObjectPathSubobjectMargin = 160;

    /**
     * The same, for the objects the engine names after the asset itself, past the object path plus the asset name:
     * a material's "<object path>:<asset>EditorOnlyData" (15) and a map's level script blueprint graph,
     * "<object path>:PersistentLevel.<asset>.EventGraph.K2Node_Event_0" (43), rounded up.
     */
    constexpr int32 NamedSubobjectPathMargin = 64;

    /**
     * Why PackageName (a package or object path) cannot name an asset, or empty when it can. CreatePackage,
     * and every LoadObject that has to create the package it resolves, aborts the editor with a Fatal error
     * on a name containing "//" -- what joining a folder that ends in '/', or a name that is itself a path,
     * produces -- and FName's checkf aborts it on a name of NAME_SIZE characters or more: the package name, which
     * a folder and a name can join to although each is shorter, and the path of every object the engine creates
     * inside the asset, which this refuses through the asset's object path (the package, a '.', and the object
     * name given or else the last path segment) plus ObjectPathSubobjectMargin, or plus the asset name and
     * NamedSubobjectPathMargin. Call it on the joined path before the first load or create.
     */
    UEAGENT_API FString InvalidPackageName(const FString& PackageName);

    /** InvalidPackageName as an error reply; null when PackageName can name an asset. */
    UEAGENT_API TSharedPtr<FJsonObject> InvalidPackageNameReply(const FString& PackageName);

    /**
     * The package for a new asset named AssetName of Class at PackageName, created if it does not exist
     * yet. Null, with OutError saying why, when another object holds that name (AssetNameConflict) or the
     * package cannot be created.
     */
    UEAGENT_API UPackage* CreateAssetPackage(const FString& PackageName, const FString& AssetName, const UClass* Class, FString& OutError);
    inline UPackage* CreateAssetPackage(const FString& PackageName, FName AssetName, const UClass* Class, FString& OutError)
    {
        return CreateAssetPackage(PackageName, AssetName.ToString(), Class, OutError);
    }
    inline UPackage* CreateAssetPackage(const FString& PackageName, const TCHAR* AssetName, const UClass* Class, FString& OutError)
    {
        return CreateAssetPackage(PackageName, FString(AssetName), Class, OutError);
    }

    /**
     * Load-or-create the asset named after PackageName's last segment: the existing object when it is a
     * Class (loaded from disk if needed); otherwise a new one, registered and marked dirty but not saved
     * (*bOutCreated tells which). Null, with OutError, when the name is taken by another class or the path
     * is not an asset path.
     */
    UEAGENT_API UObject* CreateOrLoadAsset(const FString& PackageName, UClass* Class, FString& OutError, bool* bOutCreated = nullptr);
    template <typename T>
    T* CreateOrLoadAsset(const FString& PackageName, FString& OutError, bool* bOutCreated = nullptr)
    {
        return static_cast<T*>(CreateOrLoadAsset(PackageName, T::StaticClass(), OutError, bOutCreated));
    }

    /**
     * The Blueprint of ParentClass at PackagePath/Name: the existing one (bOutAlreadyExisted), or a new one,
     * registered, compiled and saved. Null, with OutError, when the path cannot name an asset, an asset of
     * another kind holds it, or the create or the save fails.
     */
    UEAGENT_API UBlueprint* CreateOrLoadBlueprint(UClass* ParentClass, const FString& PackagePath, const FString& Name,
        bool& bOutAlreadyExisted, FString& OutError);

    /**
     * Asset IMPORT helper -- the only safe path for importing external
     * files from inside the bridge's FTSTicker callback. We deliberately
     * avoid IAssetTools::ImportAssets / ImportAssetTasks: those pump the
     * game-thread task graph and trip a RecursionGuard assertion when
     * re-entering from inside the bridge task. This calls
     * UFactory::StaticImportObject directly: inline on the calling thread,
     * no task-graph re-entry, no dialogs (overwrite is suppressed).
     *
     * Returns the imported object path (e.g.
     * "/Game/Imported/MyTex.MyTex") or empty FString on failure.
     */
    UEAGENT_API FString FactoryCreateBinaryImport(
        const FString& SourceFilePath,
        const FString& DestinationPackagePath,
        const FString& DestinationName,
        UFactory* Factory,
        UClass* TargetClass,
        FString& OutError);

    /**
     * Find an actor in World by object name OR editor label
     * (first match), or nullptr if not found / World is null.
     */
    UEAGENT_API AActor* FindActorByName(UWorld* World, const FString& Name);

    /** Find an actor in World by editor label only (first match), or nullptr if not found / World is null. */
    UEAGENT_API AActor* FindActorByLabel(UWorld* World, const FString& Label);

    /**
     * Object->Rename(WantedName, NewOuter, Flags), or to a unique variant of WantedName when another
     * object of the target outer already has that name: renaming on top of an existing object is fatal.
     * An empty WantedName lets the engine pick the name. Returns the name the object ends up with.
     */
    UEAGENT_API FName RenameToFreeName(UObject* Object, const FString& WantedName, UObject* NewOuter = nullptr,
        ERenameFlags Flags = REN_DontCreateRedirectors);

    /**
     * JSON -> UStruct conversion (wraps FJsonObjectConverter).
     * StructType may be a UScriptStruct or a UClass; Out points at the
     * struct value or the object. Returns false + fills OutError (with
     * the engine's fail reason) on failure.
     */
    UEAGENT_API bool JsonToStruct(
        const TSharedPtr<FJsonObject>& Json,
        const UStruct* StructType,
        void* Out,
        FString& OutError,
        int64 CheckFlags = 0,
        int64 SkipFlags = 0);

    /**
     * UStruct -> JSON conversion (wraps FJsonObjectConverter).
     * StructType may be a UScriptStruct or a UClass; In points at the
     * struct value or the object. Returns the populated object, or
     * nullptr on failure.
     */
    UEAGENT_API TSharedPtr<FJsonObject> StructToJson(
        const UStruct* StructType,
        const void* In,
        int64 CheckFlags = 0,
        int64 SkipFlags = 0);

    /**
     * Enforce ClampMin/ClampMax UPROPERTY metadata on every
     * numeric field of a struct/object value (FJsonObjectConverter
     * ignores Clamp meta). Editor-only (WITH_EDITORONLY_DATA); a no-op
     * in non-editor builds.
     */
    UEAGENT_API void ClampNumericByMeta(const UStruct* StructType, void* Container);

    /**
     * Partial JSON merge onto a struct/object value (only present
     * fields), then the ClampNumericByMeta post-pass. Returns false +
     * fills OutError on merge failure.
     */
    UEAGENT_API bool MergeJsonWithClamp(
        const TSharedPtr<FJsonObject>& Json,
        const UStruct* StructType,
        void* Out,
        FString& OutError,
        int64 CheckFlags = 0,
        int64 SkipFlags = 0);

    /**
     * Build a standard JSON error response of the form
     * `{ "success": false, "error": "<Message>" }`. The wire shape is
     * the canonical response envelope across every MCP handler; do not
     * add fields or change the casing without auditing every caller.
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeErrorReply(const FString& Message);

    /**
     * Build a standard JSON success response of the form
     * `{ "success": true, "data"?: <Data> }`. The optional `Data`
     * object is included verbatim only when `Data.IsValid()`.
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeOkReply(const TSharedPtr<FJsonObject>& Data = nullptr);

    /** One call of a tool made of other tools (RunSubCommands). */
    struct FSubCommand
    {
        /** The parameter (or parameters) of the calling tool this call applies: its key in `applied`. */
        FString Label;
        FString Command;
        TSharedPtr<FJsonObject> Params;
    };

    /**
     * Runs Steps in order through the bridge's own dispatch -- PIE guard, preconditions and parameter aliases
     * included, in this game-thread pass -- for a tool that applies several settings by calling the tool that owns
     * each, and stops at the first step that fails. Reply gets `applied` (label -> that step's reply) and, on a
     * failure, `failed` {label, command, error}; the steps before it stay applied, and the call's undo step reverts
     * them. No step is an error: nothing was asked. Returns the reply to send.
     */
    UEAGENT_API TSharedPtr<FJsonObject> RunSubCommands(const TCHAR* Tool, const TArray<FSubCommand>& Steps, const TSharedPtr<FJsonObject>& Reply);

    /** Runs Command with Params through the bridge's own dispatch, as RunSubCommands does one step, and returns that
     *  command's reply as it is: for a tool that only picks which tool runs. */
    UEAGENT_API TSharedPtr<FJsonObject> RunSubCommand(const FString& Command, const TSharedPtr<FJsonObject>& Params);

    /**
     * Wraps Data like MakeOkReply (under "data") with a top-level "success" that is
     * the operation's real outcome. For a payload that carries a bSuccess/bSaved that
     * can be false: the MCP caller then never reads "status":"success" for an operation
     * that failed.
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeOutcomeReply(
        const TSharedPtr<FJsonObject>& Data,
        bool bSuccess,
        const FString& ErrorMessage = FString());

    /**
     * The reply of a mutation that saves its package: success only when the package
     * reached disk. A failed save leaves the change in memory only, and the error names
     * Command and points at the save error the payload carries. Data gets "saved" (and,
     * from the second overload, "save_error" when the save failed) unless it already has it.
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeSavedReply(const TSharedPtr<FJsonObject>& Data, bool bSaved, const TCHAR* Command);
    UEAGENT_API TSharedPtr<FJsonObject> MakeSavedReply(const TSharedPtr<FJsonObject>& Data, bool bSaved, const TCHAR* Command, const FString& SaveError);

    /** The error reply every missing param gets: "Missing required parameter: <Key>". */
    UEAGENT_API TSharedPtr<FJsonObject> MissingParameterReply(const TCHAR* Key);

    /** Whether ReadRequiredString takes an empty string as present. */
    enum class EAllowEmpty : uint8 { No, Yes };

    /**
     * Read the required string param Key into Out. Null when it is there; otherwise MissingParameterReply(Key)
     * (an empty string counts as missing unless AllowEmpty). The catalog generator reads
     * `ReadRequired<Type>(Params, TEXT("key"), ...)` as a required param of that JSON type.
     */
    UEAGENT_API TSharedPtr<FJsonObject> ReadRequiredString(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key,
        FString& Out, EAllowEmpty AllowEmpty = EAllowEmpty::No);

    /** ReadRequiredString for a number param: Out is any type FJsonObject::TryGetNumberField reads. */
    template <typename NumberType>
    TSharedPtr<FJsonObject> ReadRequiredNumber(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, NumberType& Out)
    {
        if (!Params->TryGetNumberField(Key, Out))
        {
            return MissingParameterReply(Key);
        }
        return nullptr;
    }

    /** ReadRequiredString for a bool param. */
    UEAGENT_API TSharedPtr<FJsonObject> ReadRequiredBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key, bool& Out);

    /** Data["saved"] = bSaved and, when the save failed, Data["save_error"] = SaveError: the save outcome
     *  of a reply whose own success does not depend on the save. */
    UEAGENT_API void AddSaveOutcome(const TSharedPtr<FJsonObject>& Data, bool bSaved, const FString& SaveError);

    /**
     * The error of a Command whose verdict needs BOTH the operation and its save (MakeOutcomeReply with
     * bOk && bSaved): it tells the caller to read `saved` to learn which of the two failed.
     */
    UEAGENT_API FString SavedOutcomeError(const TCHAR* Command);

    /**
     * The error of a Command whose verdict is one measured value (MakeOutcomeReply with that value): it names the
     * value that came back false and points at the payload, which carries what was read.
     */
    UEAGENT_API FString MeasuredOutcomeError(const TCHAR* Command, const TCHAR* MeasuredOutcome);

    /**
     * Copy the named params under Data["requested"], so a value that came
     * FROM the caller can never be mistaken for a re-read of the object.
     *
     * This guards against the most common misleading pattern in the catalogue
     * (the "ECHO"): the handler re-emits its own input as if it were an
     * observation. An echo always agrees with what you asked for, so it can
     * never expose a mismatch -- you ask for rotation [0,0,66.8], you get back
     * rotation [0,0,66.8], and the actor is sitting there with Roll=66.8.
     *
     * ADDITIVE: does not touch or remove any top-level field the handler already
     * emitted. Names absent from `Params` are skipped silently. If
     * Data["requested"] already exists as an object, this MERGES onto it instead
     * of replacing it, so several call sites can contribute. Values are copied as
     * the raw TSharedPtr<FJsonValue> -- no type conversion, no reformatting, so
     * what shows up under "requested" is byte-for-byte what arrived on the wire.
     * Nothing is written when no name matched (no empty `"requested": {}`).
     * No-op when `Data` or `Params` is invalid.
     */
    UEAGENT_API void AddRequestedEcho(
        const TSharedPtr<FJsonObject>& Data,
        const TSharedPtr<FJsonObject>& Params,
        const TArray<FString>& FieldNames);

    /**
     * Read a JSON array field as `TArray<float>`. Resets
     * `OutArray` on entry. Missing field = empty array (NOT an error).
     */
    UEAGENT_API void ReadJsonFloatArray(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName, TArray<float>& OutArray);

    /**
     * Read a JSON array field as `FVector2D`. Expects >=2
     * numeric elements; shorter array or missing field returns
     * FVector2D(0,0). Order: [X, Y].
     */
    UEAGENT_API FVector2D ReadJsonVector2D(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

    /**
     * Read a JSON array field as `FVector`. Expects >=3
     * numeric elements; shorter array or missing field returns
     * FVector(0,0,0). Order: [X, Y, Z].
     */
    UEAGENT_API FVector ReadJsonVector(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

    /**
     * Read a JSON field as `FRotator` (degrees). TWO accepted forms:
     *
     *   OBJECT FORM -- RECOMMENDED, unambiguous:
     *       "rotation": { "roll": R, "pitch": P, "yaw": Y }
     *     Any subset is legal; every absent axis is 0. Key lookup is
     *     case-insensitive (FJsonObject's own map is), so "Yaw"/"YAW" work.
     *
     *   ARRAY FORM -- ambiguous, order FROZEN:
     *       "rotation": [ Pitch, Yaw, Roll ]
     *                     [0]     [1]  [2]
     *
     * !!! THE ARRAY ORDER IS A TRAP !!!
     * It matches C++'s FRotator(Pitch, Yaw, Roll) constructor, and NOTHING
     * ELSE the caller is likely to have in mind:
     *   - Python's `unreal.Rotator(roll, pitch, yaw)` is a DIFFERENT order.
     *   - In a LOCATION array index 2 is Z, and yaw is the rotation ABOUT Z,
     *     so `[0, 0, 90]` reads as "yaw 90" to almost everybody. It is ROLL 90.
     * Typical result: a PlayerStart with Roll=66.8 instead of Yaw=66.8 (the
     * player spawns tilted), or an actor upside down.
     *
     * The array order deliberately stays: call sites written against it would
     * all break, without a single error, if it were inverted. What removes the
     * ambiguity is the OBJECT form, and reporting rotations back with
     * AddRotatorField().
     *
     * Missing field / non-object-non-array value / array shorter than 3
     * elements => FRotator(0,0,0).
     */
    UEAGENT_API FRotator ReadJsonRotator(const TSharedPtr<FJsonObject>& JsonObject, const FString& FieldName);

    /** Emit an FRotator with the axes NAMED -- {"roll":..,"pitch":..,"yaw":..}.
     *  A 3-number array is ambiguous; a response that
     *  reports a rotation must name the axes. Symmetric with the object form
     *  ReadJsonRotator accepts, so a response can be fed straight back in.
     *  No-op when `Obj` is invalid. */
    UEAGENT_API void AddRotatorField(
        const TSharedPtr<FJsonObject>& Obj,
        const FString& FieldName,
        const FRotator& Rotator);

    /** A linear colour as {"r", "g", "b", "a"} (keys in that order). */
    UEAGENT_API TSharedPtr<FJsonObject> LinearColorToJsonObject(const FLinearColor& Color);

    /** [X, Y, Z] of any vector type as a JSON number array. */
    template <typename VectorType>
    TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const VectorType& Vector)
    {
        return { MakeShared<FJsonValueNumber>(Vector.X), MakeShared<FJsonValueNumber>(Vector.Y),
                 MakeShared<FJsonValueNumber>(Vector.Z) };
    }

    /** [Pitch, Yaw, Roll] as a JSON number array: the order of FRotator's constructor, with the
     *  ambiguity ReadJsonRotator describes. New replies name the axes with AddRotatorField. */
    template <typename RotatorType>
    TArray<TSharedPtr<FJsonValue>> RotatorToJsonArray(const RotatorType& Rotator)
    {
        return { MakeShared<FJsonValueNumber>(Rotator.Pitch), MakeShared<FJsonValueNumber>(Rotator.Yaw),
                 MakeShared<FJsonValueNumber>(Rotator.Roll) };
    }

    /** [R, G, B, A] of an FLinearColor (0..1) or an FColor (0..255) as a JSON number array. */
    template <typename ColorType>
    TArray<TSharedPtr<FJsonValue>> ColorToJsonArray(const ColorType& Color)
    {
        return { MakeShared<FJsonValueNumber>(Color.R), MakeShared<FJsonValueNumber>(Color.G),
                 MakeShared<FJsonValueNumber>(Color.B), MakeShared<FJsonValueNumber>(Color.A) };
    }

    /** [X, Y] of any 2D vector type as a JSON number array. */
    template <typename Vector2Type>
    TArray<TSharedPtr<FJsonValue>> Vector2DToJsonArray(const Vector2Type& Vector)
    {
        return { MakeShared<FJsonValueNumber>(Vector.X), MakeShared<FJsonValueNumber>(Vector.Y) };
    }

    /** A vector as {"x", "y", "z"} (keys in that order). */
    UEAGENT_API TSharedPtr<FJsonObject> VectorToJsonObject(const FVector& Vector);

    /** A rotator as {"pitch", "yaw", "roll"} (keys in that order). */
    UEAGENT_API TSharedPtr<FJsonObject> RotatorToJsonObject(const FRotator& Rotator);

    /** Each string as a JSON string, in order. */
    UEAGENT_API TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Strings);

    /** Each tag of the container by its full name ("A.B.C"), in container order. */
    UEAGENT_API TArray<TSharedPtr<FJsonValue>> GameplayTagsToJson(const FGameplayTagContainer& Tags);

    /**
     * Build a JSON value representing an actor: name, class,
     * location (XYZ), rotation (Pitch/Yaw/Roll), scale (XYZ). Returns
     * FJsonValueNull when Actor is nullptr.
     */
    UEAGENT_API TSharedPtr<FJsonValue> DescribeActorValue(AActor* Actor);

    /**
     * Build a JSON object (not value) representing an actor
     * with the same fields as DescribeActorValue. When `bDetailed` is true
     * the implementation may add additional fields (currently a no-op
     * flag for forward-compat). Returns nullptr when Actor is null.
     */
    UEAGENT_API TSharedPtr<FJsonObject> DescribeActorObject(AActor* Actor, bool bDetailed = false);

    /**
     * Find a UBlueprint by short name (assumed to live under
     * `/Game/Blueprints/<Name>`) or by full asset path. Convenience
     * forward to `mcp_common::ResolveBlueprintByName`.
     */
    UEAGENT_API UBlueprint* ResolveBlueprint(const FString& BlueprintName);

    /**
     * Find a UBlueprint by name OR full asset path. Honors
     * `/Game/...` and `/Engine/...` paths as-is (no pre-pending). Bare
     * short names try `/Game/Blueprints/<Name>` first (the fast
     * path) and, only on a miss, fall back to a project-wide recursive
     * AssetRegistry scan over `/Game` by AssetName (matches UBlueprint
     * subclasses too); ambiguous short names resolve deterministically to
     * the lexically-first package with a Warning log. The
     * rationale lives in the canonical impl.
     */
    UEAGENT_API UBlueprint* ResolveBlueprintByName(const FString& BlueprintName);

    /**
     * Find the existing EventGraph on a Blueprint (one whose
     * name contains "EventGraph"), or create a new one if none exists.
     * Returns nullptr when Blueprint is null.
     */
    UEAGENT_API UEdGraph* FindOrCreateEventGraph(UBlueprint* Blueprint);

    /**
     * The override event node of EventName -- an event the Blueprint inherits from a parent class
     * or an implemented interface -- on Graph: the one the Blueprint already has (on any ubergraph
     * page), or a new one at Position. Null when Graph is null or EventName is not an event the
     * Blueprint can override (a function with a return value belongs in a function graph).
     */
    UEAGENT_API UK2Node_Event* SpawnEventNode(UEdGraph* Graph, const FString& EventName, const FVector2D& Position);

    /**
     * Create a UK2Node_Self on the graph. The Self node lets
     * the graph reference the owning object instance. Returns nullptr
     * if Graph is null.
     */
    UEAGENT_API UK2Node_Self* SpawnSelfNode(UEdGraph* Graph, const FVector2D& Position);

    /**
     * Create a UK2Node_CallFunction on the graph bound to the
     * given UFunction. Returns nullptr if Graph or Function is null.
     */
    UEAGENT_API UK2Node_CallFunction* CreateFunctionCallNode(UEdGraph* Graph, UFunction* Function, const FVector2D& Position);

    /**
     * Create a UK2Node_VariableGet on the graph
     * bound to the named variable. Resolution order honors the optional `Scope`
     * hint (case-insensitive):
     *   - "auto" (default): a function-LOCAL variable declared in this graph's
     *     function wins first (UE shadowing semantics), else a SELF member
     *     (GeneratedClass -> SkeletonGeneratedClass -> NewVariableIndex for
     *     un-compiled BP vars), else -- only when TargetClass is supplied -- a
     *     non-self member on TargetClass.
     *   - "local": function-local ONLY (FBlueprintEditorUtils::FindLocalVariable
     *     on this graph; resolves nothing on an event/ubergraph).
     *   - "member": TargetClass member (when supplied) else SELF member; never
     *     local.
     * When TargetClass is supplied and the variable is found on it, the node is
     * emitted as a non-self member reference (SetFromProperty(..., bSelfContext=
     * false, OwnerClass)) leaving a typed member-access "target" pin. The node's
     * GUID is always freshly created; on not-found the function returns nullptr
     * WITHOUT ever adding a node to the graph (so no zero-GUID node is left
     * behind) and fills *OutError (when non-null) with an actionable message.
     * The 4-arg call form means auto scope, no TargetClass -> self member.
     */
    UEAGENT_API UK2Node_VariableGet* SpawnVariableGetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position,
        UClass* TargetClass = nullptr, const FString& Scope = TEXT("auto"), FString* OutError = nullptr);

    /**
     * Setter variant of SpawnVariableGetNode;
     * identical Scope / TargetClass / OutError semantics. Returns nullptr (and
     * never adds a node to the graph) when the variable cannot be resolved.
     */
    UEAGENT_API UK2Node_VariableSet* SpawnVariableSetNode(UEdGraph* Graph, UBlueprint* Blueprint, const FString& VariableName, const FVector2D& Position,
        UClass* TargetClass = nullptr, const FString& Scope = TEXT("auto"), FString* OutError = nullptr);

    /**
     * The refusal of a tool whose engine API does not exist in the running engine, e.g. on UE 5.7 a tool built on an
     * API added in 5.8. Tool names the tool, MinVersion the first engine version that has the API ("5.8"), and Why
     * what is missing. The reply says which engine is running and that nothing was changed. Use it in the
     * `#if UE_VERSION_OLDER_THAN(...)` branch of the handler, so the tool stays listed on every engine and answers
     * instead of failing to compile.
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeRequiresEngineReply(const TCHAR* Tool, const TCHAR* MinVersion, const TCHAR* Why);

    /**
     * Whether Transaction records an object of a Play-in-Editor session: one in a PIE package, or one outered to a
     * PIE GameInstance (a widget made with CreateWidget). The engine only knows the first kind
     * (FTransaction::ContainsPieObjects): it drops such a transaction, and resets the undo history at PIE end if one
     * is still there. A GameInstance lives in the transient package, so a transaction holding the second kind is
     * kept, the history keeps the PIE GameInstance alive after PIE ends, and the engine's check for leaked worlds
     * closes the editor on Stop. A null Transaction holds nothing.
     */
    UEAGENT_API bool TransactionHoldsPieSessionObjects(const FTransaction* Transaction);

    /**
     * Reset the editor's undo history when a transaction in it holds an object of a Play-in-Editor session
     * (TransactionHoldsPieSessionObjects), as the engine resets it for its own PIE objects. Call it while the PIE
     * worlds still exist (FEditorDelegates::EndPIE). Returns whether it reset the history.
     */
    UEAGENT_API bool ResetUndoHistoryHoldingPieSession();

    /**
     * Import Text into the value at ValuePtr with Property's own text parser, and only when the parser reads ALL of it
     * (surrounding whitespace aside). FProperty::ImportText_Direct alone answers success for text it did not read:
     * a float or integer takes "abc" as 0 and "1e5" as 1. The text is parsed into a scratch copy, so ValuePtr is left
     * untouched when it is refused; an empty text is refused for a number or a bool. Owner is passed to the parser
     * (instanced properties use it as the outer). Returns false and fills OutError on a refusal.
     */
    UEAGENT_API bool ImportPropertyTextStrict(const FProperty* Property, const FString& Text, void* ValuePtr, UObject* Owner,
        FString& OutError);

    /**
     * Set a UObject property from a JSON value. Single canonical
     * route for every FProperty kind (scalars, enums, structs, arrays,
     * sets, maps) via FJsonObjectConverter::JsonValueToUProperty.
     * Returns false + fills OutErrorMessage on invalid object,
     * missing property, or value/type mismatch.
     */
    UEAGENT_API bool SetObjectProperty(UObject* Object, const FString& PropertyName,
                                         const TSharedPtr<FJsonValue>& Value, FString& OutErrorMessage);

    /**
     * The editor world (GEditor's edit context), or nullptr if
     * GEditor is null. Used by every handler that needs to spawn /
     * look up / mutate actors in the editor's world.
     */
    UEAGENT_API UWorld* GetEditorWorld();

    /** OutWorld = GetEditorWorld(); null when there is one, otherwise the error reply with Message. */
    UEAGENT_API TSharedPtr<FJsonObject> RequireEditorWorld(UWorld*& OutWorld,
        const TCHAR* Message = TEXT("No editor world available"));

    /**
     * The world of GEngine's FIRST world context (the editor world in the editor, whatever PIE
     * is doing), or nullptr when GEngine is null or holds no context. Several readers use
     * this world rather than GetEditorWorld(); this is that choice, guarded.
     */
    UEAGENT_API UWorld* GetFirstContextWorld();

    /**
     * Full local path of a package's file on disk -- the ".uasset" or the ".umap", whichever
     * exists. Accepts a package name or an object path ("/Game/A.A"). False when the package
     * has no file (in memory only, or not under a mounted root). A package name turned into a
     * filename WITHOUT an extension names no file, so a size reader built on one reports 0.
     */
    UEAGENT_API bool FindPackageFileOnDisk(const FString& PackageOrObjectPath, FString& OutFile);

    /**
     * Registry entry of the asset a path names: an object path ("/Game/A/B.B") as is, a package
     * path ("/Game/A/B") as its primary asset ("/Game/A/B.B"), else the first asset the package
     * holds. Never the UPackage itself -- which is what GetAssetByObjectPath hands back for a bare
     * package path whose package is in memory, and a caller then fails to load it as its
     * asset. Invalid when nothing matches.
     */
    UEAGENT_API FAssetData FindAssetDataByPath(const FString& PackageOrObjectPath);

    /**
     * Convert a single FProperty's
     * value to a TYPED FJsonValue -- bool/number/string natively, structs as nested
     * JSON objects, arrays/sets as JSON arrays, maps as JSON objects (stringified
     * key). Wraps the engine's own FJsonObjectConverter::UPropertyToJsonValue (same
     * machinery UStructToJsonObject uses per-field) with a small custom export
     * callback for the two MCP-specific conventions the engine default doesn't know
     * about: object references always resolve to a GetPathName() string (never
     * inlined by value, regardless of Instanced/EditInlineNew -- object_path must
     * stay a resolvable string everywhere in this toolset) and FGameplayTag/
     * FGameplayTagContainer flatten to a bare tag string / array of tag strings
     * (neither has a native ExportTextItem, so the engine default would otherwise
     * walk them field-by-field into a clunky {"TagName":...}/{"GameplayTags":[...]}).
     * The canonical value-reader wherever the caller wants real typed JSON, not a single
     * opaque UE export-text string; GetPropertyValueAsString (UEAgentReflectionCommands)
     * serves internal string-equality comparisons (compare_objects) where format doesn't matter.
     */
    UEAGENT_API TSharedPtr<FJsonValue> PropertyValueToJson(FProperty* Property, const void* ValuePtr);

    /**
     * Canonical "which world should this handler act on" resolver,
     * PIE/runtime-aware and cooked-build-safe. Scans GEngine's world
     * contexts (works with or without GEditor -- GEngine exists in
     * cooked/packaged builds too) and prefers, in order:
     *   1. An active PIE world (EWorldType::PIE)   -- editor is in Play mode.
     *   2. A Game world (EWorldType::Game)          -- packaged/-game build.
     *   3. The editor world (EWorldType::Editor), via GetEditorWorld()
     *      -- normal authoring, no PIE running.
     * This mirrors the resolution order already used by
     * HandlePlayLevelSequenceRuntime (UEAgentTimelineCommands.cpp)
     * and UEAgentEngineCommands::GetGameInstance, so every
     * runtime-aware handler in the plugin picks the same world for the
     * same reason -- one canonical route, no per-handler variants.
     * Returns nullptr only if neither GEngine nor GEditor have a usable
     * world (e.g. very early/late in the engine lifecycle).
     */
    UEAGENT_API UWorld* GetActiveWorld();

    // =====================================================================
    // CANONICAL PIE WORLD RESOLUTION
    //
    // A per-domain `ResolvePieWorld` helper that takes "the first PIE context"
    // cannot tell a server world from a client world. That is where the most
    // productive trap in the catalogue comes from: `raycast_scene` operating on
    // UEDPIE_0 while `get_actor_properties_by_class` reads UEDPIE_1 -- two correct
    // readings of two different worlds, indistinguishable from a game bug --
    // and a `diagnose_navmesh` declaring "there is no navmesh" because it
    // resolved the CLIENT (the navmesh is server-authoritative).
    // =====================================================================

    /**
     * Resolve the target world from the `world` param. Accepted values:
     *   absent | "editor"  -> the editor world (GetEditorWorld()).
     *   "pie"              -> GEditor->PlayWorld, else the first PIE context.
     *                         Ambiguous with 2+ players; the evidence stamped by
     *                         AddWorldEvidence tells which world it was.
     *   "server"           -> the PIE world with authority (GetNetMode() != NM_Client).
     *   "client"           -> the first PIE world with GetNetMode() == NM_Client.
     *   "pie:N"            -> the PIE context whose PIEInstance == N.
     * Matching is case-insensitive. Returns nullptr + fills OutError explaining
     * WHAT was asked for and WHICH worlds actually exist (name, package, type,
     * net mode, pie instance) -- an empty "not found" is what makes a caller
     * conclude the game is broken.
     */
    UEAGENT_API UWorld* ResolveWorldFromParams(
        const TSharedPtr<FJsonObject>& Params,
        FString& OutError,
        const FString& FieldName = TEXT("world"));

    /**
     * The world the `world` param selects, read the way every command reads it: trimmed, empty meaning
     * DefaultSelector ("editor" or "pie"), and "game" meaning "pie"; then ResolveWorldFromParams.
     * OutSelector receives the selector used, lower-cased, for the reply's `world` field.
     */
    UEAGENT_API UWorld* ResolveWorldSelector(const TSharedPtr<FJsonObject>& Params, const TCHAR* DefaultSelector,
        FString& OutSelector, FString& OutError);

    /**
     * Stamp on Data the EXACT identity of the world that was read. Without
     * this, two tools that resolve "pie" to different worlds produce
     * contradictory readings that look like a game bug. Emits:
     *   world_name    -- UWorld::GetName()
     *   world_package -- World->GetOutermost()->GetName(). THIS is where the
     *                    UEDPIE_N prefix lives. In a World Partition map the
     *                    UWorld is named after the MAP and only the PACKAGE
     *                    carries the PIE instance, so reading the world name
     *                    tells you nothing about which instance you are on.
     *   world_type    -- "Editor" | "PIE" | "Game" | ... (LexToString(EWorldType))
     *   net_mode      -- "Standalone" | "ListenServer" | "DedicatedServer" | "Client"
     *   pie_instance  -- FWorldContext::PIEInstance (-1 when not a PIE context)
     * All five fields are emitted as JSON null (pie_instance as -1) when World
     * is null, so the absence of a world is still visible in the payload rather
     * than being silently indistinguishable from "field not implemented".
     */
    UEAGENT_API void AddWorldEvidence(
        const TSharedPtr<FJsonObject>& Data,
        UWorld* World);

    /**
     * Side-effect-free object
     * resolve by path -- the canonical replacement for a bare
     * `StaticLoadObject(UObject::StaticClass(), nullptr, *Path)` used as a
     * resolve/probe. Gates the load behind StaticFindObject (in-memory) +
     * FPackageName::DoesPackageExist (on-disk); a genuine miss returns
     * nullptr WITHOUT leaving a phantom UPackage that would break the next
     * create_X at the same path. `ExpectedClass` narrows
     * the probe + load (nullptr => UObject, the broad-resolver default).
     * Callers handle the nullptr case with their own fallback (e.g. the
     * dotted "Actor.Component" editor-world walk).
     */
    UEAGENT_API UObject* ResolveObjectByPath(const FString& ObjectPath, UClass* ExpectedClass = nullptr);

    /** ResolveObjectByPath for one asset class: the T at "/Game/X" or "/Game/X.X", or null. */
    template <typename T>
    T* ResolveAsset(const FString& ObjectPath)
    {
        return Cast<T>(ResolveObjectByPath(ObjectPath, T::StaticClass()));
    }

    /**
     * Generic (class-parameterized) sibling of ResolveBlueprintByName that does NOT silently
     * resolve ambiguity -- it surfaces it. Resolves asset(s) by bare display name or full
     * object/package path. If ExpectedClass is non-null, only assets whose class
     * IsChildOf(ExpectedClass) are considered. SearchRoot scopes the fallback recursive scan
     * (e.g. "/Game"); pass an empty string to default to "/Game".
     *
     * Returns true and fills OutAssets with ALL matches if at least one was found
     * (OutAssets.Num()==1 means unambiguous; >1 means the caller must decide how to handle
     * ambiguity -- this function does NOT pick one, unlike ResolveBlueprintByName's
     * lexically-first fallback). Returns false with OutError set if nothing matched.
     *
     * Resolution order: (1) if NameOrPath looks like a full path, a direct AssetRegistry
     * object-path lookup; on a hit whose class matches, returns immediately with a single
     * result. (2) Otherwise (or on a step-1 miss), a project-wide FARFilter scan rooted at
     * SearchRoot with bRecursivePaths=true and, when ExpectedClass is set,
     * ClassPaths={ExpectedClass} + bRecursiveClasses=true (so subclasses match too -- e.g. an
     * ExpectedClass of UBlueprint also matches UAnimBlueprint/UWidgetBlueprint), then an exact
     * AssetName string match over the candidates.
     */
    UEAGENT_API bool ResolveAssetFlexible(
        const FString& NameOrPath,
        UClass* ExpectedClass,
        const FString& SearchRoot,
        TArray<FAssetData>& OutAssets,
        FString& OutError);

    // =====================================================================
    // READ-ONLY ROOT GUARD.
    //
    // Vendored library roots (Project Settings > Plugins > UEAgent > Read Only
    // Content Roots) must never be written by a tool. These helpers generalize
    // the `readonly_roots` parse that fixup_redirectors does inline, so
    // any handler can enforce the same rule the same way. `fixup_redirectors`
    // has identical semantics (same default, same trailing-slash trim, same
    // case-insensitive prefix match).
    // =====================================================================

    /**
     * Parse a `readonly_roots` JSON string-array param. Empty/absent field
     * (or a non-array value) yields GetDefaultReadonlyRoots().
     * Trailing slashes are trimmed from every entry and empty entries dropped;
     * an EXPLICIT empty array `[]` yields an EMPTY result (an intentional
     * "disable the guard" opt-out) -- callers that must never run unguarded
     * should check `Roots.Num() == 0` themselves.
     * Semantics are byte-identical to fixup_redirectors' inline parse, which
     * also yields "no roots" for an explicit empty array (the loop adds nothing).
     */
    UEAGENT_API TArray<FString> ParseReadonlyRoots(
        const TSharedPtr<FJsonObject>& Params,
        const FString& FieldName = FString(TEXT("readonly_roots")));

    /**
     * The default read-only roots: the project's vendored-library roots
     * (Project Settings > Plugins > UEAgent > Read Only Content Roots), unless
     * SetDefaultReadonlyRoots has overridden them. Used by the no-roots overload of
     * IsUnderReadOnlyRoot, by SaveAssetPackage's guard, and as the default
     * `base_libraries` of the lineage audits. Empty when the project protects nothing.
     */
    UEAGENT_API TArray<FString> GetDefaultReadonlyRoots();

    /** Override the default read-only roots (trailing slashes trimmed); for tests. */
    UEAGENT_API void SetDefaultReadonlyRoots(const TArray<FString>& Roots);

    /** Drop the SetDefaultReadonlyRoots override: back to the project settings. */
    UEAGENT_API void ClearDefaultReadonlyRootsOverride();

    /** Content folder where tools create an asset when the caller passes no package path. */
    inline const TCHAR* DefaultAssetFolder() { return TEXT("/Game/MCP"); }

    /**
     * True when `PackageOrObjectPath` names a package that lives at, or
     * under, one of `ReadOnlyRoots`. Accepts a package name ("/Game/VendorPack/X"),
     * an object path ("/Game/VendorPack/X.X"), or a subobject path
     * ("/Game/VendorPack/X.X:Comp") -- everything from the first '.' is stripped
     * first. Matching is case-insensitive (FString's default), matching the
     * behavior of the fixup_redirectors code this generalizes. An empty roots
     * array always returns false (guard disabled).
     */
    UEAGENT_API bool IsUnderReadOnlyRoot(
        const FString& PackageOrObjectPath,
        const TArray<FString>& ReadOnlyRoots);

    /** IsUnderReadOnlyRoot against GetDefaultReadonlyRoots(). */
    UEAGENT_API bool IsUnderReadOnlyRoot(const FString& PackageOrObjectPath);

    /**
     * Whether the SaveAssetPackage guard refuses saves under a read-only root (the
     * default) or only logs them. It reads the console variable
     *
     *     mcp.ReadOnlyRootEnforcement   (int, default 1)
     *
     * so the guard can be lowered live from the editor console. A refused save is a
     * visible, recoverable error; a silently written vendored package is drift nobody
     * sees until source control shows it.
     */
    UEAGENT_API bool IsReadOnlyRootSaveEnforcementEnabled();

    // =====================================================================
    // PROPERTY VALUE COMPARATOR (FProperty::Identical, PPF_DeepComparison)
    //
    // Answers "does this value differ from its default?", which export-text
    // equality gets wrong.
    // ExportTextItem_Direct(Defaults=
    // nullptr, PPF_None) reports FText values as different whenever
    // their localization key/namespace differs even though the engine's own
    // equality (FTextProperty::Identical -> display-string compare in editor,
    // TextProperty.cpp) calls them equal -- a
    // "Values differ" with value_1 == value_2 == "Player" false positive.
    // Same story for object refs: FObjectPropertyBase::Identical with
    // PPF_DeepComparison (PropertyBaseObject.cpp) calls two distinct
    // instanced subobjects equal when class+name+contents match, while their
    // exported path names differ only by outer.
    // =====================================================================

    /** Classification of a single property-value comparison. */
    enum class EPropertyDiffKind : uint8
    {
        /** Engine equality says equal AND the exported text is identical. */
        Identical,
        /** Engine equality says the values genuinely differ -- a real override. */
        RealOverride,
        /** Equal per the engine; the export text differs only because of FText localization identity (namespace/key/source). */
        TextLocalizationMetadataOnly,
        /** Equal per the engine (deep object compare); the export text differs only because the referenced objects have different outers. */
        ObjectOuterOnly,
        /** Equal per the engine but the export text differs for a reason this helper could not attribute. Reported, never hidden. */
        ExportTextOnly,
        /** The two properties are not the same type -- values were NOT compared. */
        TypeMismatch,
        /** Null property / null container -- nothing was compared. */
        Invalid
    };

    /** Result of ComparePropertyValues. `Kind` is the field callers should branch on. */
    struct FPropertyDiffResult
    {
        EPropertyDiffKind Kind = EPropertyDiffKind::Invalid;
        /** True only for RealOverride and TypeMismatch -- i.e. "this is a genuine difference". */
        bool bDiffers = false;
        /** Raw FProperty::Identical(..., PortFlags) result. False when not compared. */
        bool bIdenticalDeep = false;
        /** Raw ExportTextItem_Direct(Defaults=nullptr, PPF_None) string equality. */
        bool bExportTextEqual = false;
        /** True when the property type tree contains an FText leaf. */
        bool bTypeContainsText = false;
        /** True when the property type tree contains an object/class reference leaf. */
        bool bTypeContainsObjectRef = false;
        /** Export text of side A / side B (side B is empty when ContainerB is null). */
        FString ValueA;
        FString ValueB;
        /** Human-readable reason, always populated for non-Identical kinds. */
        FString Note;
    };

    /**
     * Compare one property's value between two containers using the
     * ENGINE's own equality (FProperty::Identical) instead of exported text,
     * and classify any residual export-text difference.
     *
     * `PropertyA`/`PropertyB` may be two DIFFERENT FProperty objects (the
     * compare_objects case: same property name on two unrelated classes); they
     * are checked with FProperty::SameType first and a mismatch is reported as
     * EPropertyDiffKind::TypeMismatch rather than compared. `ContainerA` /
     * `ContainerB` are the OBJECT/STRUCT base pointers (NOT value pointers) --
     * this function does the ContainerPtrToValuePtr itself, including every
     * element of a C-style static array (ArrayDim > 1).
     *
     * PortFlags defaults to PPF_DeepComparison, which is what makes two
     * distinct-but-equivalent instanced subobjects compare equal.
     */
    UEAGENT_API FPropertyDiffResult ComparePropertyValues(
        const FProperty* PropertyA,
        const void* ContainerA,
        const FProperty* PropertyB,
        const void* ContainerB,
        uint32 PortFlags = PPF_DeepComparison);

    /**
     * Same-property overload -- the common "value vs its archetype/CDO
     * baseline" case (get_blueprint_component_overrides, list_instance_overrides).
     * `ContainerA` is the authored object, `ContainerB` the baseline.
     */
    UEAGENT_API FPropertyDiffResult ComparePropertyValues(
        const FProperty* Property,
        const void* ContainerA,
        const void* ContainerB,
        uint32 PortFlags = PPF_DeepComparison);

    /** Wire string for a diff kind: "identical" | "real_override" | "text_localization_metadata_only" | "object_outer_only" | "export_text_only" | "type_mismatch" | "invalid". */
    UEAGENT_API const TCHAR* PropertyDiffKindToString(EPropertyDiffKind Kind);

    /**
     * Full JSON form of a comparison, with TYPED values (PropertyValueToJson)
     * rather than export-text blobs. Shape:
     *   { kind, differs, identical_deep, export_text_equal,
     *     type_contains_text, type_contains_object_ref,
     *     value_a, value_b, export_text_a, export_text_b, note }
     * Takes non-const FProperty* only because PropertyValueToJson does.
     */
    UEAGENT_API TSharedPtr<FJsonObject> ComparePropertyValuesToJson(
        FProperty* PropertyA,
        const void* ContainerA,
        FProperty* PropertyB,
        const void* ContainerB,
        uint32 PortFlags = PPF_DeepComparison);

    // =====================================================================
    // VERIFIED RESPONSES (honesty envelope)
    // =====================================================================

    /** Tri-state (plus "failed") outcome of a handler's own post-write verification. */
    enum class EMcpVerification : uint8
    {
        /** The effect was re-read and matches what was requested. */
        Verified,
        /** The effect was re-read and does NOT match. */
        Failed,
        /** No way to observe the effect from here -- say so, never fake a check. */
        NotVerifiable,
        /** A check exists but was deliberately skipped (e.g. opt-in / too costly). */
        NotAttempted
    };

    /** Wire string: "verified" | "failed" | "not_verifiable" | "not_attempted". */
    UEAGENT_API const TCHAR* McpVerificationToString(EMcpVerification Verification);

    /**
     * Append one piece of verification evidence to a handler's data payload.
     * Writes `data.verification_checks[]` (append-only, one object per call:
     * { status, verified, method, detail }) and recomputes the aggregate
     * `data.verification` object, where the WORST status wins
     * (Failed > NotVerifiable > NotAttempted > Verified). Safe to call N times.
     * No-op when `Data` is invalid.
     */
    UEAGENT_API void AddVerificationEvidence(
        const TSharedPtr<FJsonObject>& Data,
        EMcpVerification Verification,
        const FString& Method,
        const FString& Detail = FString());

    /**
     * Sibling of MakeOutcomeReply that also carries the evidence of
     * what was verified and with what result. Envelope is unchanged
     * (`{ success, error?, data? }`); the evidence lives INSIDE `data` so no
     * caller's top-level shape moves:
     *   { "success": <bSuccess>, "error"?: "...",
     *     "data": { ...Data..., "verification": {...}, "verification_checks": [...] } }
     * `Data` may be null -- a fresh object is created to hold the evidence.
     * Use EMcpVerification::NotVerifiable (never a fake `true`) when the effect
     * genuinely cannot be observed from the handler.
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeVerifiedReply(
        const TSharedPtr<FJsonObject>& Data,
        bool bSuccess,
        EMcpVerification Verification,
        const FString& VerificationMethod,
        const FString& VerificationDetail = FString(),
        const FString& ErrorMessage = FString());

    /**
     * Bool convenience overload -- `bVerified` maps to Verified/Failed.
     * Unambiguous against the enum overload (no implicit bool<->enum class
     * conversion exists).
     */
    UEAGENT_API TSharedPtr<FJsonObject> MakeVerifiedReply(
        const TSharedPtr<FJsonObject>& Data,
        bool bSuccess,
        bool bVerified,
        const FString& VerificationMethod,
        const FString& VerificationDetail = FString(),
        const FString& ErrorMessage = FString());

    // =====================================================================
    // RECURSIVE FGameplayTag GUARDRAIL
    // =====================================================================

    /**
     * Validate every FGameplayTag / FGameplayTagContainer reachable from a
     * JSON value against the property tree it is about to be written onto,
     * refusing unregistered tags. Recurses through structs, arrays, sets and
     * map values (bounded depth), so a tag nested inside a TArray<FMyStruct>
     * is validated exactly like a top-level one -- a level a top-level-only check
     * would NOT reach, leaving a typo'd nested tag to be dead-set silently by
     * FJsonObjectConverter::JsonValueToUProperty.
     *
     * Accepts, for a tag leaf, either the bare string form ("A.B.C") or the
     * struct form ({"TagName":"A.B.C"}); the empty string / empty object clears
     * the tag and is always valid. Returns true (and leaves OutError untouched)
     * when everything reachable is valid or when there is nothing tag-shaped to
     * check. Fills OutError with the offending tag AND its JSON path on failure.
     * `OutTagsChecked` (optional) reports how many tag leaves were inspected --
     * 0 means "this call proved nothing", which is worth surfacing.
     */
    UEAGENT_API bool ValidateGameplayTagsInJson(
        const FProperty* Property,
        const TSharedPtr<FJsonValue>& Value,
        FString& OutError,
        int32* OutTagsChecked = nullptr);

    // =====================================================================
    // SCRIPT-REFERENCE PURGE BEFORE WORLD TEARDOWN
    // =====================================================================
    //
    // WHY THIS EXISTS. Editor crashes where FReferenceChainSearch (in the engine's own
    // log) names FPyReferenceCollector::AddReferencedObjects as the SOLE retainer of a
    // UWorld/ULevel/UPackage the engine is destroying (fatal checks at
    // EditorServer.cpp, EditorServer.cpp and PlayLevel.cpp).
    //
    // ROOT CAUSE: UEAgentAutomationCommands.cpp forces
    // EPythonFileExecutionScope::Public (the engine default is Private) so that
    // 'import unreal' and names defined by an earlier execute_python survive into the
    // next one. That is a deliberate, load-bearing choice -- but it also means every
    // global bound by an execute_python lives in __main__ FOREVER, including a handle
    // to a world. When the engine then destroys that world it cannot, and it kills the
    // process. Flipping the scope to Private would break callers that rely on
    // that persistence, so the refs are released instead.
    //
    // THE MECHANISM: FEditorSupportDelegates::PrepareToCleanseEditorObject is the only
    // ENGINE_API door to FPyReferenceCollector::PurgeUnrealObjectReferences, which is
    // private (PyReferenceCollector.h -> calling it directly is LNK2019).
    // FPythonScriptPlugin subscribes at PythonScriptPlugin.cpp and its handler
    // purges with bIncludeInners=true. That route walks ALL
    // PythonWrappedInstances, so it reaches wrappers nested inside lists, dicts,
    // closures and tracebacks -- which a scan of top-level globals would not.
    //
    // THE THREE HOLES IT PLUGS (the engine emits the delegate, but not here):
    //   * EditorServer.cpp emits it only for the CURRENT editor world (+ :2058 for
    //     its sublevels). The world that actually leaks in most World-Memory-Leak
    //     fatals is a RESIDENT, non-current one -- the DESTINATION of the load.
    //   * PurgeUnrealObjectReferences walks inners via ForEachObjectWithOuter and so
    //     never touches the OUTER. A handle to the UPackage is never purged, and
    //     CheckForWorldGCLeaks tests exactly the WorldPackage (EditorServer.cpp).
    //   * PlayLevel.cpp does not emit it at all (grep -c = 0), yet its lethal checkf is
    //     at :552-553, 247 lines after the EndPIE broadcast -- same tick, ample margin.
    //
    // These refuse nothing and add no parameter. The only observable effect is that a
    // Python handle to a world/package the engine is destroying IN THAT SAME CALL goes
    // null -- and that handle would be invalid 200 lines further down anyway.

    /** Broadcast PrepareToCleanseEditorObject for one object. No-op on nullptr. */
    UEAGENT_API void PurgeScriptRefs(UObject* Object);

    /**
     * Purge script refs from everything a LoadMap is about to destroy: the current
     * editor world, every resident world, and each of their UPackage outers. Pass the
     * destination package name so the world the engine intends to REUSE as NewWorld
     * (never initialized -- EditorServer.cpp) is left alone.
     * Returns how many roots were purged. Safe to call with nothing resident.
     */
    UEAGENT_API int32 PurgeWorldsBeforeLoadMap(const FString& TargetPackageName);

    /**
     * Purge script refs from exactly the set PlayLevel.cpp's checkf tests
     * (PlayLevel.cpp): packages flagged PKG_PlayInEditor and their worlds, plus
     * any UGameInstance already marked Garbage (its outer is not a PIE package, so the
     * package loop cannot reach it). Call from an EndPIE handler. Returns roots purged.
     */
    UEAGENT_API int32 PurgePIEScriptRefs();

    // =====================================================================
    // Lookups, conversions and replies several command families share.
    // =====================================================================

    /** StaticLoadObject of Class at Path, else FSoftObjectPath(Path).TryLoad() when that path is valid and loads
     *  an object of Class. Null for an empty Path. */
    UEAGENT_API UObject* LoadAssetOrSoftPath(const FString& Path, UClass* Class);
    template <typename T>
    T* LoadAssetOrSoftPath(const FString& Path)
    {
        return static_cast<T*>(LoadAssetOrSoftPath(Path, T::StaticClass()));
    }

    /** A new Class object named AssetName in the new package PackageName (CreateAssetPackage, then FullyLoad), with
     *  Flags. Null, with OutError, when the package cannot be made (CreateAssetPackage's error) or the object
     *  cannot (CreateFailedError). */
    UEAGENT_API UObject* NewAssetInPackage(UClass* Class, const FString& PackageName, const FString& AssetName,
        const TCHAR* CreateFailedError, FString& OutError, EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional);
    template <typename T>
    T* NewAssetInPackage(const FString& PackageName, const FString& AssetName, const TCHAR* CreateFailedError,
        FString& OutError, EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional)
    {
        return static_cast<T*>(NewAssetInPackage(T::StaticClass(), PackageName, AssetName, CreateFailedError, OutError, Flags));
    }

    /** Folder without surrounding whitespace or trailing slashes; DefaultFolder when that leaves nothing. */
    UEAGENT_API FString NormalizeFolder(FString Folder, const TCHAR* DefaultFolder);

    /** The first actor of World whose object name is ObjectName (FString equality, so case is ignored); labels are
     *  not consulted. Null when World is null or no actor has that name. */
    UEAGENT_API AActor* FindActorByObjectName(UWorld* World, const FString& ObjectName);

    /** The only actor of World whose editor label is Label, or null when none or several have it. OutMatches, when
     *  given, receives how many actors have that label. */
    UEAGENT_API AActor* FindActorByUniqueLabel(UWorld* World, const FString& Label, int32* OutMatches = nullptr);

    /** The component of Actor named Name or, when no component has that name, the only one whose class is the class
     *  named Name or derives from it ("SkyLightComponent"; a leading "U" is allowed). Null when there is none, or when
     *  several components are of that class: OutAmbiguous, when given, then receives their names. */
    UEAGENT_API UActorComponent* FindComponentByNameOrClass(AActor* Actor, const FString& Name, TArray<FString>* OutAmbiguous = nullptr);

    /** GetActiveWorld() when it is a game world (a -game process; PIE worlds are not), else null. */
    UEAGENT_API UWorld* ActiveGameWorld();

    /** Every concrete subclass of BaseClass (not abstract, deprecated or superseded), sorted by name, as
     *  [{name, path}]. */
    UEAGENT_API TArray<TSharedPtr<FJsonValue>> ConcreteSubclassesToJson(UClass* BaseClass);

    /** Default with each channel Values holds replaced by that number, in [R, G, B, A] order; Default when Values
     *  is null. Nothing is checked: an element that is not a number is lexed or read as 0 (ReadJsonLinearColor
     *  checks). */
    UEAGENT_API FLinearColor LinearColorFromJsonArray(const TArray<TSharedPtr<FJsonValue>>* Values, const FLinearColor& Default);

    /**
     * Read the optional color param Key of Params, [R, G, B] or [R, G, B, A] in finite numbers within the float range,
     * into OutColor: Default with each given channel replaced (an omitted A keeps Default's alpha), or Default itself
     * when the param is absent or null. Null when it reads; otherwise an error reply naming Key and what it holds (a
     * value that is not an array, an array of another length, an element that is not such a number) and saying nothing
     * was changed, with OutColor left alone. The catalog generator reads the call as an array param.
     */
    UEAGENT_API TSharedPtr<FJsonObject> ReadJsonLinearColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* Key,
        const FLinearColor& Default, FLinearColor& OutColor);

    /** "Blueprint not found: <Requested>". */
    UEAGENT_API TSharedPtr<FJsonObject> BlueprintNotFoundReply(const FString& Requested);

    /** Gives a brush volume a box brush of HalfExtent (a spawned volume has none). False when Actor is not an
     *  AVolume. */
    UEAGENT_API bool BuildCubeBrushForVolume(AActor* Actor, const FVector& HalfExtent);

    /** The editor's asset registry, its module loaded on first use. */
    UEAGENT_API IAssetRegistry& AssetRegistry();
}
